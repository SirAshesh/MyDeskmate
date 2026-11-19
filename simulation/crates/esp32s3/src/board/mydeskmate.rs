//! MyDeskmate hardware board: ESP32-S3 + 3.5" 8-bit Parallel TFT (HX8357D, 320x480)
//! + 4-wire resistive touchscreen sharing LCD pins.
//!
//! Exact pin mapping from `pin.txt` and `src/hal/touch_4wire.cpp`:
//!   LCD_RD  -> GPIO 15
//!   LCD_WR  -> GPIO 7   (also TS_YP / ADC1_CH6)
//!   LCD_RS  -> GPIO 6   (also TS_XM / ADC1_CH5)
//!   LCD_CS  -> GPIO 5
//!   LCD_RST -> GPIO 4
//!   LCD_D0  -> GPIO 16
//!   LCD_D1  -> GPIO 17
//!   LCD_D2  -> GPIO 18
//!   LCD_D3  -> GPIO 8
//!   LCD_D4  -> GPIO 9
//!   LCD_D5  -> GPIO 10
//!   LCD_D6  -> GPIO 11  (also TS_XP)
//!   LCD_D7  -> GPIO 13  (also TS_YM)
use esp_soc::board::{BoardModel, LcdI80Pins, VirtualCycle};
use esp_soc::devices::DcsPanel;

pub const PIN_RST: u8 = 4;
pub const PIN_CS: u8 = 5;
pub const PIN_RS: u8 = 6;
pub const PIN_WR: u8 = 7;
pub const PIN_RD: u8 = 15;
pub const DATA_PINS: [u8; 8] = [16, 17, 18, 8, 9, 10, 11, 13];

pub const TS_XP: u8 = 11; // D6
pub const TS_XM: u8 = 6;  // RS (ADC1_CH5)
pub const TS_YP: u8 = 7;  // WR (ADC1_CH6)
pub const TS_YM: u8 = 13; // D7

pub const PANEL_W: usize = 320;
pub const PANEL_H: usize = 480;
pub const VIEW_W: u32 = 480;
pub const VIEW_H: u32 = 320;

// Default calibration geometry matching `include/calibration_config.h` for UI_DISPLAY_ROTATION = 3:
//   TOUCH_CAL_SWAP = true
//   TOUCH_CAL_X0 = 3430, TOUCH_CAL_X1 = 223  (maps rawY -> sx in 0..479)
//   TOUCH_CAL_Y0 = 411,  TOUCH_CAL_Y1 = 3170 (maps rawX -> sy in 0..319)
const CAL_X0: f32 = 3430.0;
const CAL_X1: f32 = 223.0;
const CAL_Y0: f32 = 411.0;
const CAL_Y1: f32 = 3170.0;

pub struct MyDeskmate {
    pub panel: DcsPanel,
    last_enable: u64,
    last_out: u64,
    last_wr: bool,
    last_rst: bool,
    touch_down: bool,
    touch_x: u16,
    touch_y: u16,
    analog_out: [(u8, u16); 2],
    gpio_count: u64,
    i80_bytes: u64,
    bitbang_bytes: u64,
}

impl Default for MyDeskmate {
    fn default() -> Self { Self::new() }
}

impl MyDeskmate {
    pub fn new() -> Self {
        MyDeskmate {
            panel: DcsPanel::new(PANEL_W, PANEL_H),
            last_enable: 0,
            last_out: 0,
            last_wr: true,
            last_rst: true,
            touch_down: false,
            touch_x: 0,
            touch_y: 0,
            analog_out: [(TS_XM, 0), (TS_YP, 4095)],
            gpio_count: 0,
            i80_bytes: 0,
            bitbang_bytes: 0,
        }
    }

    #[inline]
    fn pin_drive(enable: u64, out: u64, pin: u8) -> Option<bool> {
        let mask = 1u64 << pin;
        if enable & mask != 0 {
            Some(out & mask != 0)
        } else {
            None
        }
    }

    /// Recompute the 4-wire resistive touchscreen voltages presented to ESP32-S3 ADC1
    /// on `TS_XM` (GPIO 6) and `TS_YP` (GPIO 7) based on the live drive states of
    /// `TS_XP` (GPIO 11), `TS_XM` (GPIO 6), `TS_YP` (GPIO 7), and `TS_YM` (GPIO 13).
    fn update_touch_adc(&mut self) {
        let xp = Self::pin_drive(self.last_enable, self.last_out, TS_XP);
        let xm = Self::pin_drive(self.last_enable, self.last_out, TS_XM);
        let yp = Self::pin_drive(self.last_enable, self.last_out, TS_YP);
        let ym = Self::pin_drive(self.last_enable, self.last_out, TS_YM);

        let sx = (self.touch_x.min(VIEW_W as u16 - 1)) as f32;
        let sy = (self.touch_y.min(VIEW_H as u16 - 1)) as f32;
        let raw_x = (CAL_Y0 + (CAL_Y1 - CAL_Y0) * (sy / (VIEW_H as f32 - 1.0)))
            .round()
            .clamp(0.0, 4095.0) as u16;
        let raw_y = (CAL_X0 + (CAL_X1 - CAL_X0) * (sx / (VIEW_W as f32 - 1.0)))
            .round()
            .clamp(0.0, 4095.0) as u16;

        let (xm_raw, yp_raw) = match (xp, xm, yp, ym) {
            // Phase 1 & 4: Z pressure measurement (XP=LOW, YM=HIGH, XM=ANALOG, YP=ANALOG)
            (Some(false), None, None, Some(true)) => {
                if self.touch_down { (1100, 2300) } else { (0, 4095) }
            }
            // Phase 2: X axis measurement (XP=HIGH, XM=LOW, YM=INPUT, YP=ANALOG -> read YP)
            (Some(true), Some(false), None, None) => {
                (0, if self.touch_down { raw_x } else { 0 })
            }
            // Phase 3: Y axis measurement (YP=HIGH, YM=LOW, XP=INPUT, XM=ANALOG -> read XM)
            (None, None, Some(true), Some(false)) => {
                (if self.touch_down { raw_y } else { 0 }, 4095)
            }
            // Idle / LCD bus active
            _ => (0, 4095),
        };

        self.analog_out[0] = (TS_XM, xm_raw);
        self.analog_out[1] = (TS_YP, yp_raw);
    }

    #[inline]
    fn panel_pixel_to_rgb565(&self, mut px: u16) -> u16 {
        // The physical 3.5" HX8357D module has normally-inverted glass (`cfg.invert = true`
        // sends INVON 0x21 for normal colors) and BGR subpixels (`cfg.rgb_order = false`
        // sets MADCTL bit 3 for normal RGB colors).
        if !self.panel.inverted {
            px = !px;
        }
        if (self.panel.madctl & 0x08) == 0 {
            px = ((px & 0x001f) << 11) | (px & 0x07e0) | ((px & 0xf800) >> 11);
        }
        px
    }
}

impl BoardModel for MyDeskmate {
    fn name(&self) -> &'static str { "mydeskmate" }

    fn gpio_output_at(&mut self, _cycle: VirtualCycle, changes: &[(u8, bool)], enabled: u64, output: u64) {
        self.gpio_count += changes.len() as u64;

        let rst = Self::pin_drive(enabled, output, PIN_RST).unwrap_or(true);
        if self.last_rst && !rst {
            self.panel.reset();
        }
        self.last_rst = rst;

        let cs_low = Self::pin_drive(enabled, output, PIN_CS) == Some(false);
        let wr = Self::pin_drive(enabled, output, PIN_WR).unwrap_or(true);

        // Bit-banged 8-bit parallel write (`hardwareSafePreInit()` slowCmd/slowData):
        // latches D0..D7 on WR rising edge while CS is LOW and RST is HIGH.
        if rst && cs_low && !self.last_wr && wr {
            let all_data_out = DATA_PINS.iter().all(|&p| (enabled & (1u64 << p)) != 0);
            if all_data_out {
                let dc = Self::pin_drive(enabled, output, PIN_RS).unwrap_or(false);
                let mut byte = 0u8;
                for (bit, &pin) in DATA_PINS.iter().enumerate() {
                    if (output & (1u64 << pin)) != 0 {
                        byte |= 1u8 << bit;
                    }
                }
                self.panel.dc = dc;
                self.panel.byte(byte);
                self.bitbang_bytes += 1;
            }
        }
        self.last_wr = wr;
        self.last_enable = enabled;
        self.last_out = output;
        self.update_touch_adc();
    }

    fn lcd_i80_tx(&mut self, pins: LcdI80Pins, dc: bool, bytes: &[u8]) {
        // Verify CS is asserted LOW and RST is not held LOW.
        if Self::pin_drive(pins.gpio_enable, pins.gpio_out, PIN_CS) != Some(false) {
            return;
        }
        if Self::pin_drive(pins.gpio_enable, pins.gpio_out, PIN_RST) == Some(false) {
            return;
        }
        // Verify WR and RS are routed to the exact pins from `pin.txt`.
        if pins.wr != Some(PIN_WR) || pins.dc != Some(PIN_RS) {
            return;
        }
        // Verify which data bits D0..D7 are accurately routed to `DATA_PINS`.
        let mut wired_mask = 0u8;
        for (bit, &expected_pin) in DATA_PINS.iter().enumerate() {
            if pins.data[bit] == Some(expected_pin) {
                wired_mask |= 1u8 << bit;
            }
        }
        if wired_mask == 0 {
            return;
        }

        self.panel.dc = dc;
        if wired_mask == 0xff {
            for &b in bytes {
                self.panel.byte(b);
            }
        } else {
            for &b in bytes {
                self.panel.byte(b & wired_mask);
            }
        }
        self.i80_bytes += bytes.len() as u64;
    }

    fn analog_outputs(&self) -> &[(u8, u16)] {
        &self.analog_out
    }

    fn touch(&mut self, x: u16, y: u16, down: bool) {
        self.touch_down = down;
        self.touch_x = x.min(VIEW_W as u16 - 1);
        self.touch_y = y.min(VIEW_H as u16 - 1);
        self.update_touch_adc();
    }

    fn display(&self) -> Option<(u32, u32, Vec<u16>, u64)> {
        let mut out = vec![0u16; (VIEW_W * VIEW_H) as usize];
        if self.panel.on && !self.panel.sleeping {
            // Physical panel is 320x480 portrait mounted in landscape orientation matching
            // UI_DISPLAY_ROTATION = 3 (MAD_MV | MAD_MX | MAD_MY):
            //   gram[(479 - sx) * 320 + (319 - sy)] -> display(sx, sy).
            for sy in 0..(VIEW_H as usize) {
                let col = (PANEL_W - 1) - sy;
                let dst_row = sy * (VIEW_W as usize);
                for sx in 0..(VIEW_W as usize) {
                    let row = (PANEL_H - 1) - sx;
                    let raw = self.panel.gram[row * PANEL_W + col];
                    out[dst_row + sx] = self.panel_pixel_to_rgb565(raw);
                }
            }
        }
        Some((VIEW_W, VIEW_H, out, self.panel.pixels_written + self.panel.frames + self.panel.resets))
    }

    fn display_frames(&self) -> u64 { self.panel.frames }
    fn display_version(&self) -> u64 { self.panel.pixels_written + self.panel.frames + self.panel.resets }
    fn display_push_hz(&self) -> u64 { 60 }
    fn gpio_events(&self) -> u64 { self.gpio_count }
    fn gram(&self) -> Option<(Vec<u16>, usize, usize)> { Some((self.panel.gram.clone(), PANEL_W, PANEL_H)) }

    fn report(&self) -> String {
        format!(
            "[board] mydeskmate HX8357D: {}x{} on={} sleeping={} madctl={:#04x} inv={} frames={} pixels={} i80_bytes={} bitbang_bytes={}",
            VIEW_W, VIEW_H, self.panel.on, self.panel.sleeping, self.panel.madctl, self.panel.inverted,
            self.panel.frames, self.panel.pixels_written, self.i80_bytes, self.bitbang_bytes
        )
    }
}
