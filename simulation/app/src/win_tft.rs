//! Minimal native Win32 pure-TFT window: displays ONLY the 480x320 TFT framebuffer
//! with zero menus, toolbars, buttons, or extra UI chrome.
use std::ffi::c_void;
use std::ptr::{null, null_mut};
use std::sync::atomic::{AtomicBool, AtomicIsize, AtomicU32, Ordering};
use std::sync::{Arc, Mutex};

pub const TFT_W: usize = 480;
pub const TFT_H: usize = 320;

/// Shared framebuffer and touch state between the Win32 UI thread and the ESP32-S3 emulator thread.
pub struct SharedTft {
    /// 480 * 320 pixels in 0x00RRGGBB (Win32 32-bit BI_RGB top-down DIB format).
    pub fb: Mutex<Vec<u32>>,
    /// Packed touch state: bit 31 = down, bits 16..31 = y, bits 0..16 = x.
    touch_packed: AtomicU32,
    pub running: AtomicBool,
    hwnd: AtomicIsize,
}

impl SharedTft {
    pub fn new() -> Arc<Self> {
        Arc::new(SharedTft {
            fb: Mutex::new(vec![0u32; TFT_W * TFT_H]),
            touch_packed: AtomicU32::new(0),
            running: AtomicBool::new(true),
            hwnd: AtomicIsize::new(0),
        })
    }

    pub fn set_touch(&self, x: u16, y: u16, down: bool) {
        let packed = ((down as u32) << 31)
            | (((y.min((TFT_H - 1) as u16) as u32) & 0x7fff) << 16)
            | ((x.min((TFT_W - 1) as u16) as u32) & 0xffff);
        self.touch_packed.store(packed, Ordering::Release);
    }

    pub fn get_touch(&self) -> (u16, u16, bool) {
        let p = self.touch_packed.load(Ordering::Acquire);
        let down = (p >> 31) != 0;
        let y = ((p >> 16) & 0x7fff) as u16;
        let x = (p & 0xffff) as u16;
        (x, y, down)
    }

    /// Update the BGRA framebuffer from RGB565 pixels and notify the window to repaint.
    pub fn publish_rgb565(&self, rgb565: &[u16]) {
        if let Ok(mut fb) = self.fb.lock() {
            for (dst, &px) in fb.iter_mut().zip(rgb565.iter()) {
                let r5 = ((px >> 11) & 0x1f) as u32;
                let g6 = ((px >> 5) & 0x3f) as u32;
                let b5 = (px & 0x1f) as u32;
                let r = (r5 << 3) | (r5 >> 2);
                let g = (g6 << 2) | (g6 >> 4);
                let b = (b5 << 3) | (b5 >> 2);
                *dst = (r << 16) | (g << 8) | b;
            }
        }
        let h = self.hwnd.load(Ordering::Acquire);
        if h != 0 {
            unsafe {
                InvalidateRect(h as HWND, null(), 0);
            }
        }
    }
}

// ------------------------------------------------------------------ Win32 FFI
type HWND = *mut c_void;
type HINSTANCE = *mut c_void;
type HICON = *mut c_void;
type HCURSOR = *mut c_void;
type HBRUSH = *mut c_void;
type HMENU = *mut c_void;
type HDC = *mut c_void;
type WPARAM = usize;
type LPARAM = isize;
type LRESULT = isize;
type BOOL = i32;
type UINT = u32;
type DWORD = u32;
type LONG = i32;
type WORD = u16;
type WNDPROC = unsafe extern "system" fn(HWND, UINT, WPARAM, LPARAM) -> LRESULT;

const WS_OVERLAPPEDWINDOW: DWORD = 0x00CF_0000;
const WS_POPUP: DWORD = 0x8000_0000;
const WS_VISIBLE: DWORD = 0x1000_0000;
const CS_HREDRAW: UINT = 0x0002;
const CS_VREDRAW: UINT = 0x0001;
const CW_USEDEFAULT: i32 = -2147483648;
const IDC_ARROW: *const u16 = 32512 as *const u16;
const COLORONCOLOR: i32 = 3;
const DIB_RGB_COLORS: UINT = 0;
const SRCCOPY: DWORD = 0x00CC_0020;
const GWLP_USERDATA: i32 = -21;

const WM_DESTROY: UINT = 0x0002;
const WM_PAINT: UINT = 0x000F;
const WM_ERASEBKGND: UINT = 0x0014;
const WM_KEYDOWN: UINT = 0x0100;
const WM_MOUSEMOVE: UINT = 0x0200;
const WM_LBUTTONDOWN: UINT = 0x0201;
const WM_LBUTTONUP: UINT = 0x0202;
const WM_RBUTTONDOWN: UINT = 0x0204;
const WM_CAPTURECHANGED: UINT = 0x0215;
const WM_SIZING: UINT = 0x0214;
const WM_NCLBUTTONDOWN: UINT = 0x00A1;
const HTCAPTION: WPARAM = 2;
const VK_ESCAPE: WPARAM = 0x1B;

const WMSZ_LEFT: WPARAM = 1;
const WMSZ_RIGHT: WPARAM = 2;
const WMSZ_TOP: WPARAM = 3;
const WMSZ_TOPLEFT: WPARAM = 4;
const WMSZ_TOPRIGHT: WPARAM = 5;
const WMSZ_BOTTOM: WPARAM = 6;
const WMSZ_BOTTOMLEFT: WPARAM = 7;
const WMSZ_BOTTOMRIGHT: WPARAM = 8;

#[repr(C)]
struct WNDCLASSW {
    style: UINT,
    lpfn_wnd_proc: WNDPROC,
    cb_cls_extra: i32,
    cb_wnd_extra: i32,
    h_instance: HINSTANCE,
    h_icon: HICON,
    h_cursor: HCURSOR,
    hbr_background: HBRUSH,
    lpsz_menu_name: *const u16,
    lpsz_class_name: *const u16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RECT {
    left: LONG,
    top: LONG,
    right: LONG,
    bottom: LONG,
}

#[repr(C)]
#[derive(Default)]
struct POINT {
    x: LONG,
    y: LONG,
}

#[repr(C)]
#[derive(Default)]
struct MSG {
    hwnd: HWND,
    message: UINT,
    w_param: WPARAM,
    l_param: LPARAM,
    time: DWORD,
    pt: POINT,
}

#[repr(C)]
struct PAINTSTRUCT {
    hdc: HDC,
    f_erase: BOOL,
    rc_paint: RECT,
    f_restore: BOOL,
    f_inc_update: BOOL,
    rgb_reserved: [u8; 32],
}

#[repr(C)]
struct BITMAPINFOHEADER {
    bi_size: DWORD,
    bi_width: LONG,
    bi_height: LONG,
    bi_planes: WORD,
    bi_bit_count: WORD,
    bi_compression: DWORD,
    bi_size_image: DWORD,
    bi_x_pels_per_meter: LONG,
    bi_y_pels_per_meter: LONG,
    bi_clr_used: DWORD,
    bi_clr_important: DWORD,
}

#[repr(C)]
struct BITMAPINFO {
    bmi_header: BITMAPINFOHEADER,
    bmi_colors: [DWORD; 1],
}

const SW_SHOW: i32 = 5;

#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleW(lp_module_name: *const u16) -> HINSTANCE;
    fn FreeConsole() -> BOOL;
}

#[link(name = "user32")]
extern "system" {
    fn RegisterClassW(lp_wnd_class: *const WNDCLASSW) -> WORD;
    fn CreateWindowExW(
        dw_ex_style: DWORD,
        lp_class_name: *const u16,
        lp_window_name: *const u16,
        dw_style: DWORD,
        x: i32,
        y: i32,
        n_width: i32,
        n_height: i32,
        h_wnd_parent: HWND,
        h_menu: HMENU,
        h_instance: HINSTANCE,
        lp_param: *mut c_void,
    ) -> HWND;
    fn DefWindowProcW(hwnd: HWND, msg: UINT, w_param: WPARAM, l_param: LPARAM) -> LRESULT;
    fn GetMessageW(lp_msg: *mut MSG, hwnd: HWND, w_msg_filter_min: UINT, w_msg_filter_max: UINT) -> BOOL;
    fn TranslateMessage(lp_msg: *const MSG) -> BOOL;
    fn DispatchMessageW(lp_msg: *const MSG) -> LRESULT;
    fn PostQuitMessage(n_exit_code: i32);
    fn DestroyWindow(hwnd: HWND) -> BOOL;
    fn LoadCursorW(h_instance: HINSTANCE, lp_cursor_name: *const u16) -> HCURSOR;
    fn AdjustWindowRectEx(lp_rect: *mut RECT, dw_style: DWORD, b_menu: BOOL, dw_ex_style: DWORD) -> BOOL;
    fn GetClientRect(hwnd: HWND, lp_rect: *mut RECT) -> BOOL;
    fn GetWindowRect(hwnd: HWND, lp_rect: *mut RECT) -> BOOL;
    fn InvalidateRect(hwnd: HWND, lp_rect: *const RECT, b_erase: BOOL) -> BOOL;
    fn BeginPaint(hwnd: HWND, lp_paint: *mut PAINTSTRUCT) -> HDC;
    fn EndPaint(hwnd: HWND, lp_paint: *const PAINTSTRUCT) -> BOOL;
    fn SetCapture(hwnd: HWND) -> HWND;
    fn ReleaseCapture() -> BOOL;
    fn SendMessageW(hwnd: HWND, msg: UINT, w_param: WPARAM, l_param: LPARAM) -> LRESULT;
    fn SetWindowLongPtrW(hwnd: HWND, n_index: i32, dw_new_long: isize) -> isize;
    fn GetWindowLongPtrW(hwnd: HWND, n_index: i32) -> isize;
    fn SetProcessDPIAware() -> BOOL;
    fn ShowWindow(hwnd: HWND, n_cmd_show: i32) -> BOOL;
    fn UpdateWindow(hwnd: HWND) -> BOOL;
}

#[link(name = "gdi32")]
extern "system" {
    fn SetStretchBltMode(hdc: HDC, mode: i32) -> i32;
    fn StretchDIBits(
        hdc: HDC,
        x_dest: i32,
        y_dest: i32,
        dest_width: i32,
        dest_height: i32,
        x_src: i32,
        y_src: i32,
        src_width: i32,
        src_height: i32,
        lp_bits: *const c_void,
        lp_bmi: *const BITMAPINFO,
        i_usage: UINT,
        rop: DWORD,
    ) -> i32;
}

fn to_wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

struct WindowCtx {
    shared: Arc<SharedTft>,
    dragging_touch: bool,
    borderless: bool,
}

fn client_to_tft(hwnd: HWND, l_param: LPARAM) -> (u16, u16) {
    let mx = (l_param & 0xffff) as i16 as i32;
    let my = ((l_param >> 16) & 0xffff) as i16 as i32;
    let mut rc = RECT::default();
    unsafe { GetClientRect(hwnd, &mut rc); }
    let cw = (rc.right - rc.left).max(1);
    let ch = (rc.bottom - rc.top).max(1);
    let tx = ((mx.clamp(0, cw - 1) as i64) * (TFT_W as i64) / (cw as i64)) as u16;
    let ty = ((my.clamp(0, ch - 1) as i64) * (TFT_H as i64) / (ch as i64)) as u16;
    (tx.min((TFT_W - 1) as u16), ty.min((TFT_H - 1) as u16))
}

unsafe extern "system" fn wnd_proc(hwnd: HWND, msg: UINT, w_param: WPARAM, l_param: LPARAM) -> LRESULT {
    let ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA) as *mut WindowCtx;
    if ptr.is_null() {
        return DefWindowProcW(hwnd, msg, w_param, l_param);
    }
    let ctx = &mut *ptr;

    match msg {
        WM_ERASEBKGND => 1,
        WM_PAINT => {
            let mut ps: PAINTSTRUCT = std::mem::zeroed();
            let hdc = BeginPaint(hwnd, &mut ps);
            let mut rc = RECT::default();
            GetClientRect(hwnd, &mut rc);
            let cw = (rc.right - rc.left).max(1);
            let ch = (rc.bottom - rc.top).max(1);

            let bmi = BITMAPINFO {
                bmi_header: BITMAPINFOHEADER {
                    bi_size: std::mem::size_of::<BITMAPINFOHEADER>() as DWORD,
                    bi_width: TFT_W as LONG,
                    bi_height: -(TFT_H as LONG), // top-down
                    bi_planes: 1,
                    bi_bit_count: 32,
                    bi_compression: 0, // BI_RGB
                    bi_size_image: 0,
                    bi_x_pels_per_meter: 0,
                    bi_y_pels_per_meter: 0,
                    bi_clr_used: 0,
                    bi_clr_important: 0,
                },
                bmi_colors: [0],
            };

            if let Ok(fb) = ctx.shared.fb.lock() {
                SetStretchBltMode(hdc, COLORONCOLOR);
                StretchDIBits(
                    hdc,
                    0,
                    0,
                    cw,
                    ch,
                    0,
                    0,
                    TFT_W as i32,
                    TFT_H as i32,
                    fb.as_ptr() as *const c_void,
                    &bmi,
                    DIB_RGB_COLORS,
                    SRCCOPY,
                );
            }
            EndPaint(hwnd, &ps);
            0
        }
        WM_LBUTTONDOWN => {
            ctx.dragging_touch = true;
            SetCapture(hwnd);
            let (tx, ty) = client_to_tft(hwnd, l_param);
            ctx.shared.set_touch(tx, ty, true);
            0
        }
        WM_MOUSEMOVE => {
            if ctx.dragging_touch {
                let (tx, ty) = client_to_tft(hwnd, l_param);
                ctx.shared.set_touch(tx, ty, true);
            }
            0
        }
        WM_LBUTTONUP => {
            if ctx.dragging_touch {
                ctx.dragging_touch = false;
                ReleaseCapture();
                let (tx, ty) = client_to_tft(hwnd, l_param);
                ctx.shared.set_touch(tx, ty, false);
            }
            0
        }
        WM_CAPTURECHANGED => {
            if ctx.dragging_touch {
                ctx.dragging_touch = false;
                let (tx, ty, _) = ctx.shared.get_touch();
                ctx.shared.set_touch(tx, ty, false);
            }
            0
        }
        WM_RBUTTONDOWN => {
            // In borderless mode, right-drag moves the window without interfering with touch.
            if ctx.borderless {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            0
        }
        WM_KEYDOWN => {
            if ctx.borderless && w_param == VK_ESCAPE {
                DestroyWindow(hwnd);
            }
            0
        }
        WM_SIZING => {
            // Preserve exact 3:2 (480x320) TFT aspect ratio when resizing.
            let rect = &mut *(l_param as *mut RECT);
            let mut wr = RECT::default();
            let mut cr = RECT::default();
            GetWindowRect(hwnd, &mut wr);
            GetClientRect(hwnd, &mut cr);
            let extra_w = (wr.right - wr.left) - (cr.right - cr.left);
            let extra_h = (wr.bottom - wr.top) - (cr.bottom - cr.top);

            let cw = (rect.right - rect.left - extra_w).max(240);
            let ch = (rect.bottom - rect.top - extra_h).max(160);
            match w_param {
                WMSZ_TOP | WMSZ_BOTTOM => {
                    let target_w = (ch * TFT_W as i32) / TFT_H as i32;
                    rect.right = rect.left + target_w + extra_w;
                }
                WMSZ_LEFT | WMSZ_RIGHT | WMSZ_BOTTOMRIGHT | WMSZ_TOPRIGHT => {
                    let target_h = (cw * TFT_H as i32) / TFT_W as i32;
                    if w_param == WMSZ_TOPRIGHT {
                        rect.top = rect.bottom - target_h - extra_h;
                    } else {
                        rect.bottom = rect.top + target_h + extra_h;
                    }
                }
                WMSZ_TOPLEFT | WMSZ_BOTTOMLEFT => {
                    let target_h = (cw * TFT_H as i32) / TFT_W as i32;
                    if w_param == WMSZ_TOPLEFT {
                        rect.top = rect.bottom - target_h - extra_h;
                    } else {
                        rect.bottom = rect.top + target_h + extra_h;
                    }
                }
                _ => {}
            }
            1
        }
        WM_DESTROY => {
            ctx.shared.running.store(false, Ordering::Release);
            ctx.shared.hwnd.store(0, Ordering::Release);
            PostQuitMessage(0);
            0
        }
        _ => DefWindowProcW(hwnd, msg, w_param, l_param),
    }
}

pub fn run_tft_window(shared: Arc<SharedTft>, scale: u32, borderless: bool, keep_console: bool) -> Result<(), String> {
    unsafe {
        if !keep_console {
            FreeConsole();
        }
        SetProcessDPIAware();
        let h_instance = GetModuleHandleW(null());
        let class_name = to_wide("MyDeskmateTftWindow");
        let title = to_wide("ESP32-S3 TFT 3.5\"");

        let wc = WNDCLASSW {
            style: CS_HREDRAW | CS_VREDRAW,
            lpfn_wnd_proc: wnd_proc,
            cb_cls_extra: 0,
            cb_wnd_extra: 0,
            h_instance,
            h_icon: null_mut(),
            h_cursor: LoadCursorW(null_mut(), IDC_ARROW),
            hbr_background: null_mut(),
            lpsz_menu_name: null(),
            lpsz_class_name: class_name.as_ptr(),
        };
        if RegisterClassW(&wc) == 0 {
            return Err("RegisterClassW failed".into());
        }

        let style = if borderless {
            WS_POPUP | WS_VISIBLE
        } else {
            WS_OVERLAPPEDWINDOW | WS_VISIBLE
        };
        let scale = scale.clamp(1, 4) as i32;
        let mut rc = RECT {
            left: 0,
            top: 0,
            right: TFT_W as i32 * scale,
            bottom: TFT_H as i32 * scale,
        };
        AdjustWindowRectEx(&mut rc, style, 0, 0);

        let hwnd = CreateWindowExW(
            0,
            class_name.as_ptr(),
            title.as_ptr(),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rc.right - rc.left,
            rc.bottom - rc.top,
            null_mut(),
            null_mut(),
            h_instance,
            null_mut(),
        );
        if hwnd.is_null() {
            return Err("CreateWindowExW failed".into());
        }

        let ctx = Box::new(WindowCtx {
            shared: shared.clone(),
            dragging_touch: false,
            borderless,
        });
        let ctx_ptr = Box::into_raw(ctx);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, ctx_ptr as isize);
        shared.hwnd.store(hwnd as isize, Ordering::Release);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        InvalidateRect(hwnd, null(), 0);

        let mut msg = MSG::default();
        while GetMessageW(&mut msg, null_mut(), 0, 0) > 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        drop(Box::from_raw(ctx_ptr));
    }
    Ok(())
}
