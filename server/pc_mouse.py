import argparse
import ctypes
import ctypes.wintypes as wt
import socket
import struct
import time

ESP_W = 480
ESP_H = 320
UDP_PORT = 8789

WH_MOUSE_LL = 14
WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_MBUTTONDOWN = 0x0207
WM_MOUSEWHEEL = 0x020A
VK_ESCAPE = 0x1B

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32


class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("pt", wt.POINT),
        ("mouseData", wt.DWORD),
        ("flags", wt.DWORD),
        ("time", wt.DWORD),
        ("dwExtraInfo", ctypes.c_void_p),
    ]


HOOKPROC = ctypes.WINFUNCTYPE(ctypes.c_ssize_t, ctypes.c_int, wt.WPARAM, wt.LPARAM)

user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, wt.HINSTANCE, wt.DWORD]
user32.SetWindowsHookExW.restype = wt.HHOOK
user32.CallNextHookEx.argtypes = [wt.HHOOK, ctypes.c_int, wt.WPARAM, wt.LPARAM]
user32.CallNextHookEx.restype = ctypes.c_ssize_t


def main():
    parser = argparse.ArgumentParser(description="MyDeskmate PC Mouse Bridge (KVM via WiFi UDP)")
    parser.add_argument("--ip", required=True, help="IP ESP32-S3")
    parser.add_argument("--port", type=int, default=UDP_PORT, help="Port UDP ESP32 (default 8789)")
    parser.add_argument("--edge", choices=["left", "right", "top", "bottom"], default="left",
                        help="Sisi layar PC tempat kursor masuk ke ESP32 (default: left)")
    parser.add_argument("--speed", type=float, default=0.55, help="Sensitivitas kursor di ESP32 (default 0.55)")
    args = parser.parse_args()

    scr_w = user32.GetSystemMetrics(0)
    scr_h = user32.GetSystemMetrics(1)
    anchor_x = scr_w // 2
    anchor_y = scr_h // 2

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (args.ip, args.port)

    state = {
        "in_esp": False,
        "esp_x": float(ESP_W // 2),
        "esp_y": float(ESP_H // 2),
        "left_down": False,
        "last_exit": 0.0,
        "last_send": 0.0,
    }

    def send_pkt(active: bool, wheel: int = 0):
        btn = 1 if (active and state["left_down"]) else 0
        pkt = struct.pack(
            "<2sBBhhb",
            b"PM",
            1 if active else 0,
            btn,
            int(max(0, min(ESP_W - 1, state["esp_x"]))),
            int(max(0, min(ESP_H - 1, state["esp_y"]))),
            max(-127, min(127, wheel)),
        )
        sock.sendto(pkt, addr)

    def enter_esp(px: int, py: int):
        state["in_esp"] = True
        state["left_down"] = False
        if args.edge == "left":
            state["esp_x"] = float(ESP_W - 4)
            state["esp_y"] = float(max(0, min(ESP_H - 1, py * ESP_H / scr_h)))
        elif args.edge == "right":
            state["esp_x"] = 4.0
            state["esp_y"] = float(max(0, min(ESP_H - 1, py * ESP_H / scr_h)))
        elif args.edge == "top":
            state["esp_x"] = float(max(0, min(ESP_W - 1, px * ESP_W / scr_w)))
            state["esp_y"] = float(ESP_H - 4)
        else:
            state["esp_x"] = float(max(0, min(ESP_W - 1, px * ESP_W / scr_w)))
            state["esp_y"] = 4.0
        user32.SetCursorPos(anchor_x, anchor_y)
        send_pkt(True)
        print("[PC-MOUSE] Kursor MASUK ke ESP32 (klik tengah / ESC / geser balik ke tepi untuk keluar)")

    def exit_esp():
        state["in_esp"] = False
        state["left_down"] = False
        state["last_exit"] = time.perf_counter()
        send_pkt(False)
        if args.edge == "left":
            user32.SetCursorPos(6, int(state["esp_y"] * scr_h / ESP_H))
        elif args.edge == "right":
            user32.SetCursorPos(scr_w - 8, int(state["esp_y"] * scr_h / ESP_H))
        elif args.edge == "top":
            user32.SetCursorPos(int(state["esp_x"] * scr_w / ESP_W), 6)
        else:
            user32.SetCursorPos(int(state["esp_x"] * scr_w / ESP_W), scr_h - 8)
        print("[PC-MOUSE] Kursor KEMBALI ke PC")

    def hook_cb(nCode, wParam, lParam):
        if nCode >= 0:
            info = ctypes.cast(lParam, ctypes.POINTER(MSLLHOOKSTRUCT)).contents
            px, py = info.pt.x, info.pt.y

            if not state["in_esp"]:
                if wParam == WM_MOUSEMOVE:
                    hit = (
                        (args.edge == "left" and px <= 0)
                        or (args.edge == "right" and px >= scr_w - 1)
                        or (args.edge == "top" and py <= 0)
                        or (args.edge == "bottom" and py >= scr_h - 1)
                    )
                    if hit and (time.perf_counter() - state["last_exit"] > 0.35):
                        enter_esp(px, py)
                        return 1
            else:
                if wParam == WM_MOUSEMOVE:
                    dx = px - anchor_x
                    dy = py - anchor_y
                    if dx != 0 or dy != 0:
                        state["esp_x"] += dx * args.speed
                        state["esp_y"] += dy * args.speed
                        # Keluar balik ke PC jika melewati sisi berlawanan
                        exited = (
                            (args.edge == "left" and state["esp_x"] >= ESP_W)
                            or (args.edge == "right" and state["esp_x"] <= -1)
                            or (args.edge == "top" and state["esp_y"] >= ESP_H)
                            or (args.edge == "bottom" and state["esp_y"] <= -1)
                        )
                        if exited:
                            exit_esp()
                            return 1
                        state["esp_x"] = max(0.0, min(float(ESP_W - 1), state["esp_x"]))
                        state["esp_y"] = max(0.0, min(float(ESP_H - 1), state["esp_y"]))
                        now = time.perf_counter()
                        if now - state["last_send"] >= 0.016:
                            state["last_send"] = now
                            send_pkt(True)
                    return 1
                elif wParam == WM_LBUTTONDOWN:
                    state["left_down"] = True
                    send_pkt(True)
                    return 1
                elif wParam == WM_LBUTTONUP:
                    state["left_down"] = False
                    send_pkt(True)
                    return 1
                elif wParam == WM_MBUTTONDOWN:
                    exit_esp()
                    return 1
                elif wParam == WM_MOUSEWHEEL:
                    delta = ctypes.c_short(info.mouseData >> 16).value
                    step = 1 if delta > 0 else -1
                    send_pkt(True, wheel=step)
                    return 1
        return user32.CallNextHookEx(None, nCode, wParam, lParam)

    proc = HOOKPROC(hook_cb)
    hook = user32.SetWindowsHookExW(WH_MOUSE_LL, proc, None, 0)
    if not hook:
        err = ctypes.get_last_error()
        raise RuntimeError(f"Gagal memasang mouse hook Windows (err={err})")

    print(f"[PC-MOUSE] Aktif -> target {args.ip}:{args.port}")
    print(f"[PC-MOUSE] Geser mouse ke tepi '{args.edge.upper()}' layar utama untuk masuk ke ESP32!")
    print("[PC-MOUSE] Tekan Ctrl+C untuk berhenti.")

    QS_ALLINPUT = 0x04FF
    msg = wt.MSG()
    try:
        while True:
            # Bangun seketika (0 ms delay) begitu ada event mouse Windows
            user32.MsgWaitForMultipleObjects(0, None, False, 100, QS_ALLINPUT)
            while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, 1):
                user32.TranslateMessage(ctypes.byref(msg))
                user32.DispatchMessageW(ctypes.byref(msg))
            if state["in_esp"]:
                if user32.GetAsyncKeyState(VK_ESCAPE) & 0x8000:
                    exit_esp()
                elif time.perf_counter() - state["last_send"] > 1.0:
                    send_pkt(True)
                    state["last_send"] = time.perf_counter()
    except KeyboardInterrupt:
        if state["in_esp"]:
            exit_esp()
        print("\n[PC-MOUSE] Dihentikan.")
    finally:
        user32.UnhookWindowsHookEx(hook)
        sock.close()


if __name__ == "__main__":
    main()
