mod win_tft;

use esp_soc::soc::{SocBus, Stop};
use esp32s3::board::MyDeskmate;
use esp32s3::bus::SocBus as S3Bus;
use esp32s3::periph::CPU_HZ;
use esp32s3::soc::Machine;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};
use win_tft::SharedTft;

struct Config {
    rom: PathBuf,
    bootloader: PathBuf,
    partitions: PathBuf,
    firmware: PathBuf,
    scale: u32,
    borderless: bool,
    serial: bool,
    headless_png: Option<PathBuf>,
    seconds: f64,
    taps: Vec<(f64, u16, u16, f64)>, // (start_sec, x, y, duration_sec)
}

fn find_project_root() -> PathBuf {
    // Check cwd, parent of cwd, and ancestors of executable path for `platformio.ini` or `rom/esp32s3_rev0_rom.elf`.
    let mut candidates = Vec::new();
    if let Ok(cwd) = std::env::current_dir() {
        candidates.push(cwd.clone());
        if let Some(p) = cwd.parent() {
            candidates.push(p.to_path_buf());
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        for anc in exe.ancestors().skip(1).take(5) {
            candidates.push(anc.to_path_buf());
        }
    }
    for dir in &candidates {
        if dir.join("platformio.ini").is_file() {
            return dir.clone();
        }
    }
    std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn parse_args() -> Result<Config, String> {
    let root = find_project_root();
    let pio_build = root.join(".pio").join("build").join("esp32-s3-devkitc-1");
    let mut cfg = Config {
        rom: root.join("simulation").join("rom").join("esp32s3_rev0_rom.elf"),
        bootloader: pio_build.join("bootloader.bin"),
        partitions: pio_build.join("partitions.bin"),
        firmware: pio_build.join("firmware.bin"),
        scale: 2,
        borderless: false,
        serial: false,
        headless_png: None,
        seconds: 5.0,
        taps: Vec::new(),
    };

    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--rom" => cfg.rom = PathBuf::from(args.next().ok_or("missing --rom path")?),
            "--bootloader" => cfg.bootloader = PathBuf::from(args.next().ok_or("missing --bootloader path")?),
            "--ptable" | "--partitions" => cfg.partitions = PathBuf::from(args.next().ok_or("missing --partitions path")?),
            "--app" | "--firmware" => cfg.firmware = PathBuf::from(args.next().ok_or("missing --firmware path")?),
            "--scale" => {
                let s = args.next().ok_or("missing --scale value")?;
                cfg.scale = s.parse().map_err(|_| format!("invalid --scale: {s}"))?;
            }
            "--borderless" => cfg.borderless = true,
            "--serial" => cfg.serial = true,
            "--headless-png" => cfg.headless_png = Some(PathBuf::from(args.next().ok_or("missing --headless-png path")?)),
            "--seconds" => {
                let s = args.next().ok_or("missing --seconds value")?;
                cfg.seconds = s.parse().map_err(|_| format!("invalid --seconds: {s}"))?;
            }
            "--tap" => {
                // Format: start_sec:x:y:dur_ms
                let spec = args.next().ok_or("missing --tap spec (start_sec:x:y:dur_ms)")?;
                let parts: Vec<&str> = spec.split(':').collect();
                if parts.len() != 4 {
                    return Err(format!("invalid --tap '{spec}', expected start_sec:x:y:dur_ms"));
                }
                let start: f64 = parts[0].parse().map_err(|_| format!("invalid tap start in '{spec}'"))?;
                let x: u16 = parts[1].parse().map_err(|_| format!("invalid tap x in '{spec}'"))?;
                let y: u16 = parts[2].parse().map_err(|_| format!("invalid tap y in '{spec}'"))?;
                let dur_ms: f64 = parts[3].parse().map_err(|_| format!("invalid tap dur_ms in '{spec}'"))?;
                cfg.taps.push((start, x, y, dur_ms / 1000.0));
            }
            "-h" | "--help" => {
                println!(
                    "MyDeskmate ESP32-S3 + HX8357D TFT Simulator\n\
                     Options:\n\
                       --scale <1..4>            Window scale (default: 2 -> 960x640)\n\
                       --borderless              Borderless TFT-only popup window (right-drag moves, Esc closes)\n\
                       --serial                  Print ESP32-S3 UART0 output to stdout\n\
                       --rom <elf>               Path to esp32s3_rev0_rom.elf\n\
                       --bootloader <bin>        Path to bootloader.bin\n\
                       --partitions <bin>        Path to partitions.bin\n\
                       --firmware <bin>          Path to firmware.bin\n\
                       --headless-png <png>      Run headlessly and save TFT framebuffer to PNG\n\
                       --seconds <sec>           Emulated duration for headless mode (default: 5.0)\n\
                       --tap <sec:x:y:dur_ms>    Inject a touch tap in headless mode"
                );
                std::process::exit(0);
            }
            other => return Err(format!("unknown argument: {other}")),
        }
    }
    Ok(cfg)
}

fn read_file(path: &Path, label: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("failed to read {label} ({}): {e}", path.display()))
}

fn build_machine(cfg: &Config) -> Result<Machine, String> {
    let rom = read_file(&cfg.rom, "ESP32-S3 mask ROM")?;
    let bootloader = read_file(&cfg.bootloader, "bootloader.bin")?;
    let partitions = read_file(&cfg.partitions, "partitions.bin")?;
    let firmware = read_file(&cfg.firmware, "firmware.bin")?;

    let mac = [0x02, 0x12, 0x34, 0x56, 0x78, 0x9a];
    // 8 MB QIO Flash + 8 MB OPI PSRAM matching `esp32-s3-devkitc-1` (`qio_opi`)
    let mut bus = S3Bus::new(8 << 20, 8 << 20, mac);
    bus.board = Box::new(MyDeskmate::new());
    bus.attach_board_devices();

    let mut machine = Machine::new(mac, bus);
    machine.set_debug(&esp_soc::DebugFlags::from_env());
    machine.console.mask = if cfg.serial { 0x2 } else { 0x0 };
    machine.console.capture = false;

    machine.load_rom(&rom)?;
    machine.write_flash(0x0000, &bootloader)?;
    machine.write_flash(0x8000, &partitions)?;
    machine.write_flash(0x10000, &firmware)?;
    machine.boot_rom();
    Ok(machine)
}

fn run_headless(cfg: &Config, png_path: &Path) -> Result<(), String> {
    let mut machine = build_machine(cfg)?;
    let total_cycles = (cfg.seconds * CPU_HZ as f64) as u64;
    let step_cycles = CPU_HZ / 250; // 4 ms steps

    let mut last_touch = (0u16, 0u16, false);
    while machine.bus.cycles() < total_cycles {
        let now_sec = machine.bus.cycles() as f64 / CPU_HZ as f64;
        let mut active_touch = (last_touch.0, last_touch.1, false);
        for &(start, x, y, dur) in &cfg.taps {
            if now_sec >= start && now_sec < start + dur {
                active_touch = (x, y, true);
                break;
            }
        }
        if active_touch != last_touch {
            machine.bus.touch_input(active_touch.0, active_touch.1, active_touch.2);
            last_touch = active_touch;
        }
        let target = (machine.bus.cycles() + step_cycles).min(total_cycles);
        machine.max_cycles = target;
        match machine.run(u64::MAX) {
            Stop::Halted => {}
            Stop::SwReset => {
                eprintln!("[sim] reboot at {:.3}s", machine.seconds());
                machine.reboot();
            }
            stop => {
                eprintln!("[sim] stopped early at {:.3}s: {stop:?}", machine.seconds());
                eprintln!("{}", machine.dump_regs());
                break;
            }
        }
    }

    let png_str = png_path.to_str().ok_or("invalid PNG output path")?;
    machine
        .write_tft_png(png_str, cfg.scale.max(1) as usize)
        .map_err(|e| format!("failed to write PNG: {e}"))?;
    eprintln!("{}", machine.bus.board.report());
    eprintln!(
        "[sim] simulated {:.3}s ({} insns), saved {}",
        machine.seconds(),
        machine.insns(),
        png_path.display()
    );
    Ok(())
}

fn main() {
    let cfg = match parse_args() {
        Ok(c) => c,
        Err(e) => {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
    };

    if let Some(ref png) = cfg.headless_png {
        if let Err(e) = run_headless(&cfg, png) {
            eprintln!("error: {e}");
            std::process::exit(1);
        }
        return;
    }

    // Validate required binaries exist before creating the TFT window.
    for (label, path) in [
        ("ESP32-S3 mask ROM", &cfg.rom),
        ("bootloader.bin", &cfg.bootloader),
        ("partitions.bin", &cfg.partitions),
        ("firmware.bin", &cfg.firmware),
    ] {
        if !path.is_file() {
            eprintln!("error: missing {label}: {}", path.display());
            std::process::exit(1);
        }
    }

    let shared = SharedTft::new();
    let emu_shared = shared.clone();
    let scale = cfg.scale;
    let borderless = cfg.borderless;
    let keep_console = cfg.serial;

    let emu_thread = std::thread::spawn(move || {
        let mut machine = match build_machine(&cfg) {
            Ok(m) => m,
            Err(e) => {
                eprintln!("error: {e}");
                emu_shared.running.store(false, Ordering::Release);
                return;
            }
        };

        let step_cycles = CPU_HZ / 250; // 4 ms simulated chunks
        let mut last_ver = u64::MAX;
        let mut last_touch = (0u16, 0u16, false);
        let mut wall_ref = Instant::now();
        let mut cyc_ref = machine.bus.cycles();

        while emu_shared.running.load(Ordering::Acquire) {
            let touch = emu_shared.get_touch();
            if touch != last_touch {
                machine.bus.touch_input(touch.0, touch.1, touch.2);
                last_touch = touch;
            }

            let target = machine.bus.cycles().saturating_add(step_cycles);
            machine.max_cycles = target;
            match machine.run(u64::MAX) {
                Stop::Halted => {}
                Stop::SwReset => {
                    machine.reboot();
                }
                stop => {
                    eprintln!("[sim] emulator stopped: {stop:?}");
                    break;
                }
            }

            let ver = machine.bus.board.display_version();
            if ver != last_ver {
                if let Some((w, h, pixels, v)) = machine.bus.board.display() {
                    if w as usize == win_tft::TFT_W && h as usize == win_tft::TFT_H {
                        emu_shared.publish_rgb565(&pixels);
                        last_ver = v;
                    }
                }
            }

            // Gentle real-time pacing (1x ESP32-S3 240 MHz clock)
            let emu_us = ((machine.bus.cycles().saturating_sub(cyc_ref)) * 1_000_000) / CPU_HZ;
            let wall_us = wall_ref.elapsed().as_micros() as u64;
            if emu_us > wall_us + 500 {
                let sleep_us = (emu_us - wall_us).min(4_000);
                std::thread::sleep(Duration::from_micros(sleep_us));
            } else if wall_us > emu_us + 200_000 {
                // Re-anchor if host fell behind during heavy redraws
                wall_ref = Instant::now();
                cyc_ref = machine.bus.cycles();
            }
        }
    });

    if let Err(e) = win_tft::run_tft_window(shared.clone(), scale, borderless, keep_console) {
        eprintln!("window error: {e}");
    }
    shared.running.store(false, Ordering::Release);
    let _ = emu_thread.join();
}
