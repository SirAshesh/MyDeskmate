import argparse
import socket
import struct
import time

import mss
import numpy as np
import win32gui
from PIL import Image, ImageDraw

MON_W = 411
MON_H = 320
TILE = 32
PORT = 8788


def rgb_to_rgb565_le(arr: np.ndarray) -> np.ndarray:
    r = arr[:, :, 0].astype(np.uint16)
    g = arr[:, :, 1].astype(np.uint16)
    b = arr[:, :, 2].astype(np.uint16)
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return rgb565.astype("<u2")


def draw_cursor(img: Image.Image, cap_left: int, cap_top: int, cap_w: int, cap_h: int) -> None:
    try:
        flags, _, (cx, cy) = win32gui.GetCursorInfo()
        if not (flags & 0x00000001):
            return
        if not (cap_left <= cx < cap_left + cap_w and cap_top <= cy < cap_top + cap_h):
            return
        px = int((cx - cap_left) * MON_W / cap_w)
        py = int((cy - cap_top) * MON_H / cap_h)
        draw = ImageDraw.Draw(img)
        poly = [
            (px, py),
            (px, py + 14),
            (px + 4, py + 10),
            (px + 7, py + 16),
            (px + 10, py + 14),
            (px + 7, py + 9),
            (px + 11, py + 9),
        ]
        draw.polygon(poly, fill=(255, 255, 255), outline=(15, 15, 18))
    except Exception:
        pass


def stream_loop(args):
    sct = mss.mss()
    monitors = sct.monitors

    use_monitor_idx = args.monitor
    if use_monitor_idx == 0:
        for i in range(1, len(monitors)):
            if "MTT1337" in monitors[i].get("unique_id", ""):
                use_monitor_idx = i
                break
        if use_monitor_idx == 0:
            use_monitor_idx = len(monitors) - 1 if len(monitors) >= 3 else 1

    print(f"[MON] Menangkap Monitor {use_monitor_idx}: {monitors[use_monitor_idx]}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(6.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[MON] Menghubungkan ke {args.ip}:{args.port} ...")
    sock.connect((args.ip, args.port))
    print("[MON] Terhubung! Tekan Ctrl+C untuk berhenti.")

    prev_565 = None
    last_send = 0.0
    frame_interval = 1.0 / max(1, args.fps)

    try:
        while True:
            t0 = time.perf_counter()

            if args.scale > 0:
                m = monitors[use_monitor_idx]
                cap_w = min(m["width"], max(MON_W, int(MON_W * args.scale)))
                cap_h = min(m["height"], max(MON_H, int(MON_H * args.scale)))
                try:
                    _, _, (cx, cy) = win32gui.GetCursorInfo()
                except Exception:
                    cx, cy = m["left"] + m["width"] // 2, m["top"] + m["height"] // 2
                left = max(m["left"], min(m["left"] + m["width"] - cap_w, cx - cap_w // 2))
                top = max(m["top"], min(m["top"] + m["height"] - cap_h, cy - cap_h // 2))
                region = {"left": left, "top": top, "width": cap_w, "height": cap_h}
            else:
                region = monitors[use_monitor_idx]

            shot = sct.grab(region)
            img = Image.frombytes("RGB", shot.size, shot.bgra, "raw", "BGRX")
            img = img.resize((MON_W, MON_H), Image.Resampling.BILINEAR)
            draw_cursor(img, region["left"], region["top"], region["width"], region["height"])

            curr_565 = rgb_to_rgb565_le(np.asarray(img))

            tiles = []
            for ty in range(0, MON_H, TILE):
                th = min(TILE, MON_H - ty)
                for tx in range(0, MON_W, TILE):
                    tw = min(TILE, MON_W - tx)
                    patch = curr_565[ty : ty + th, tx : tx + tw]
                    if prev_565 is None or not np.array_equal(patch, prev_565[ty : ty + th, tx : tx + tw]):
                        tiles.append((tx, ty, tw, th, patch.tobytes()))

            prev_565 = curr_565

            now = time.perf_counter()
            if not tiles and (now - last_send) < 1.5:
                elapsed = now - t0
                if elapsed < frame_interval:
                    time.sleep(frame_interval - elapsed)
                continue

            buf = bytearray()
            buf += b"MFRM"
            buf += struct.pack("<H", len(tiles))
            for tx, ty, tw, th, raw in tiles:
                buf += struct.pack("<HHHH", tx, ty, tw, th)
                buf += raw

            sock.sendall(buf)
            last_send = time.perf_counter()

            # Tunggu 1-byte ACK dari ESP32 agar TCP buffer tidak pernah penuh/desync
            ack = sock.recv(1)
            if not ack:
                raise ConnectionResetError("ESP32 menutup koneksi")

            elapsed = time.perf_counter() - t0
            if elapsed < frame_interval:
                time.sleep(frame_interval - elapsed)
    finally:
        sock.close()


def main():
    parser = argparse.ArgumentParser(description="MyDeskmate Second Monitor Streamer (WiFi)")
    parser.add_argument("--ip", required=True, help="IP ESP32-S3 yang tampil di menu Second Monitor")
    parser.add_argument("--port", type=int, default=PORT, help="Port TCP ESP32 (default 8788)")
    parser.add_argument("--monitor", type=int, default=0, help="Index monitor (0 = auto Monitor 2 jika ada, else Monitor 1)")
    parser.add_argument("--fps", type=int, default=20, help="Target FPS (default 20)")
    parser.add_argument("--scale", type=float, default=0.0, help="0 = full monitor, 1.0 = 411x320 tajam 1:1 ikut kursor, 1.5 = 616x480")
    args = parser.parse_args()

    while True:
        try:
            stream_loop(args)
        except KeyboardInterrupt:
            print("\n[MON] Dihentikan.")
            break
        except Exception as e:
            print(f"[MON] Koneksi terputus ({e}), mencoba ulang dalam 2 detik...")
            time.sleep(2)


if __name__ == "__main__":
    main()
