//! GlassDock: makes the Windows 10 taskbar look like the Windows 11 one, with a liquid-glass dock:
//! - it centres Start and the icons on the taskbar (moving the task list over, as TaskbarX did),
//!   Start one more cell of the icon grid right before the first icon;
//! - it hooks taskbar_hooks.dll (next to this exe) onto Explorer's taskbar thread, which makes
//!   the icon buttons square and draws the Windows 11 Start logo (the DLL goes away again when
//!   GlassDock exits);
//! - it keeps what Start opens centred on the screen: the Start menu and the Search pane;
//! - it runs start-glass.exe (next to this exe) for every new Start menu host, which makes the
//!   Start menu's acrylic see-through (frosted glass), rounds it and animates it;
//! - it draws a liquid-glass dock behind Start and the icons with taskbar-dock.dll (next to this
//!   exe), which also glides Start and the icons to their new place when the row grows or shrinks.
//!
//! It reacts to events (location changes on the taskbar thread and of those panels, panels
//! being shown or hidden) instead of polling, re-checks every 2 s in case Explorer restarted,
//! and keeps a single instance.
//!
//! As an app: started anywhere but its own folder (%LOCALAPPDATA%\Programs\GlassDock), it installs
//! itself and its companions there, starts with Windows, shows in Apps & features and runs from
//! there. It sits in the notification area; its menu turns starting with Windows on and off, opens
//! the settings, restarts it (which applies them), quits (putting the taskbar back) and
//! uninstalls it. `GlassDock.exe --restore` puts Start, the icons and the panels back where
//! Windows has them (for when it was ended some other way); `--install` / `--uninstall` (with
//! `--quiet`: no questions) do what they say.

#![windows_subsystem = "windows"]

use std::cell::Cell;
use std::ffi::c_void;
use std::mem::transmute;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::ptr::{null, null_mut};

type Hwnd = isize;

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

#[repr(C)]
#[derive(Default, Clone, Copy)]
struct Point {
    x: i32,
    y: i32,
}

#[repr(C)]
#[derive(Default)]
struct Msg {
    hwnd: Hwnd,
    message: u32,
    wparam: usize,
    lparam: isize,
    time: u32,
    pt: Point,
}

#[repr(C)]
struct Guid(u32, u16, u16, [u8; 8]);

#[repr(C)]
struct StartupInfo {
    cb: u32,
    reserved: *mut u16,
    desktop: *mut u16,
    title: *mut u16,
    x: u32,
    y: u32,
    x_size: u32,
    y_size: u32,
    x_count_chars: u32,
    y_count_chars: u32,
    fill_attribute: u32,
    flags: u32,
    show_window: u16,
    reserved2_size: u16,
    reserved2: *mut u8,
    std_input: isize,
    std_output: isize,
    std_error: isize,
}

#[repr(C)]
struct ProcessInformation {
    process: isize,
    thread: isize,
    process_id: u32,
    thread_id: u32,
}

/// MODULEENTRY32W.
#[repr(C)]
struct ModuleEntry {
    size: u32,
    module_id: u32,
    process_id: u32,
    global_usage: u32,
    process_usage: u32,
    base: usize,
    base_size: u32,
    module: isize,
    name: [u16; 256],
    path: [u16; 260],
}

/// VARIANT (24 bytes on x64): type tag, then the value in the first bytes of `data`.
#[repr(C)]
#[derive(Default, Clone, Copy)]
struct Variant {
    vt: u16,
    reserved: [u16; 3],
    data: [u64; 2],
}

type WndProc = extern "system" fn(Hwnd, u32, usize, isize) -> isize;

/// WNDCLASSEXW.
#[repr(C)]
struct WndClassEx {
    size: u32,
    style: u32,
    proc: WndProc,
    class_extra: i32,
    window_extra: i32,
    instance: isize,
    icon: isize,
    cursor: isize,
    background: isize,
    menu_name: *const u16,
    class_name: *const u16,
    small_icon: isize,
}

/// NOTIFYICONDATAW (976 bytes on x64).
#[repr(C)]
struct NotifyIconData {
    size: u32,
    hwnd: Hwnd,
    id: u32,
    flags: u32,
    callback_message: u32,
    icon: isize,
    tip: [u16; 128],
    state: u32,
    state_mask: u32,
    info: [u16; 256],
    version: u32,
    info_title: [u16; 64],
    info_flags: u32,
    guid: Guid,
    balloon_icon: isize,
}

/// RTL_OSVERSIONINFOW.
#[repr(C)]
struct OsVersionInfo {
    size: u32,
    major: u32,
    minor: u32,
    build: u32,
    platform: u32,
    service_pack: [u16; 128],
}

type WinEventProc = extern "system" fn(isize, u32, Hwnd, i32, i32, u32, u32);
/// taskbar-dock.dll's dock_place(below, bounds, shape, radius, visible, notify_thread, list,
/// list_x, start, start_x).
type DockPlace = extern "system" fn(Hwnd, *const Rect, *const Rect, i32, i32, u32, Hwnd, i32, Hwnd, i32) -> i32;

#[link(name = "user32")]
extern "system" {
    fn SetProcessDpiAwarenessContext(context: isize) -> i32;
    fn GetDpiForWindow(hwnd: Hwnd) -> u32;
    fn FindWindowW(class: *const u16, title: *const u16) -> Hwnd;
    fn FindWindowExW(parent: Hwnd, after: Hwnd, class: *const u16, title: *const u16) -> Hwnd;
    fn GetClassNameW(hwnd: Hwnd, buffer: *mut u16, size: i32) -> i32;
    fn IsWindow(hwnd: Hwnd) -> i32;
    fn GetWindowRect(hwnd: Hwnd, rect: *mut Rect) -> i32;
    fn ScreenToClient(hwnd: Hwnd, point: *mut Point) -> i32;
    fn SetWindowPos(hwnd: Hwnd, after: Hwnd, x: i32, y: i32, cx: i32, cy: i32, flags: u32) -> i32;
    fn GetParent(hwnd: Hwnd) -> Hwnd;
    fn GetWindowThreadProcessId(hwnd: Hwnd, pid: *mut u32) -> u32;
    fn SetWinEventHook(min: u32, max: u32, module: isize, callback: WinEventProc, pid: u32, tid: u32, flags: u32) -> isize;
    fn UnhookWinEvent(hook: isize) -> i32;
    fn SetWindowsHookExW(id: i32, callback: isize, module: isize, tid: u32) -> isize;
    fn UnhookWindowsHookEx(hook: isize) -> i32;
    fn PostMessageW(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize) -> i32;
    fn SendMessageTimeoutW(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize, flags: u32, timeout: u32, result: *mut usize) -> isize;
    fn SetTimer(hwnd: Hwnd, id: usize, ms: u32, callback: isize) -> usize;
    fn KillTimer(hwnd: Hwnd, id: usize) -> i32;
    fn GetMessageW(msg: *mut Msg, hwnd: Hwnd, min: u32, max: u32) -> i32;
    fn DispatchMessageW(msg: *const Msg) -> isize;
    fn RegisterClassExW(class: *const WndClassEx) -> u16;
    fn CreateWindowExW(
        ex_style: u32,
        class: *const u16,
        title: *const u16,
        style: u32,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        parent: Hwnd,
        menu: isize,
        instance: isize,
        param: *const c_void,
    ) -> Hwnd;
    fn DefWindowProcW(hwnd: Hwnd, msg: u32, wparam: usize, lparam: isize) -> isize;
    fn PostQuitMessage(code: i32);
    fn RegisterWindowMessageW(name: *const u16) -> u32;
    fn CreatePopupMenu() -> isize;
    fn AppendMenuW(menu: isize, flags: u32, id: usize, text: *const u16) -> i32;
    fn TrackPopupMenu(menu: isize, flags: u32, x: i32, y: i32, reserved: i32, hwnd: Hwnd, rect: *const Rect) -> i32;
    fn DestroyMenu(menu: isize) -> i32;
    fn SetForegroundWindow(hwnd: Hwnd) -> i32;
    fn GetCursorPos(point: *mut Point) -> i32;
    fn MessageBoxW(hwnd: Hwnd, text: *const u16, caption: *const u16, flags: u32) -> i32;
    fn LoadImageW(instance: isize, name: *const u16, kind: u32, width: i32, height: i32, flags: u32) -> isize;
    fn GetSystemMetricsForDpi(index: i32, dpi: u32) -> i32;
    fn GetDpiForSystem() -> u32;
}

#[link(name = "shell32")]
extern "system" {
    fn Shell_NotifyIconW(message: u32, data: *const NotifyIconData) -> i32;
    fn ShellExecuteW(hwnd: Hwnd, operation: *const u16, file: *const u16, parameters: *const u16, directory: *const u16, show: i32) -> isize;
}

#[link(name = "advapi32")]
extern "system" {
    fn RegSetKeyValueW(key: isize, subkey: *const u16, name: *const u16, kind: u32, data: *const c_void, size: u32) -> i32;
    fn RegGetValueW(key: isize, subkey: *const u16, name: *const u16, flags: u32, kind: *mut u32, data: *mut c_void, size: *mut u32) -> i32;
    fn RegDeleteKeyValueW(key: isize, subkey: *const u16, name: *const u16) -> i32;
    fn RegDeleteKeyW(key: isize, subkey: *const u16) -> i32;
}

#[link(name = "ntdll")]
extern "system" {
    fn RtlGetVersion(info: *mut OsVersionInfo) -> i32;
}

#[link(name = "kernel32")]
extern "system" {
    fn CreateMutexW(attributes: *const u8, initial_owner: i32, name: *const u16) -> isize;
    fn GetLastError() -> u32;
    fn GetCurrentThreadId() -> u32;
    fn GetTickCount64() -> u64;
    fn LoadLibraryW(path: *const u16) -> isize;
    fn GetProcAddress(module: isize, name: *const u8) -> isize;
    fn GetModuleFileNameW(module: isize, buffer: *mut u16, size: u32) -> u32;
    fn OpenProcess(access: u32, inherit: i32, pid: u32) -> isize;
    fn QueryFullProcessImageNameW(process: isize, flags: u32, buffer: *mut u16, size: *mut u32) -> i32;
    fn CloseHandle(handle: isize) -> i32;
    fn Sleep(ms: u32);
    fn CreateProcessW(
        application: *const u16,
        command_line: *mut u16,
        process_attributes: *const c_void,
        thread_attributes: *const c_void,
        inherit_handles: i32,
        flags: u32,
        environment: *const c_void,
        directory: *const u16,
        startup: *const StartupInfo,
        info: *mut ProcessInformation,
    ) -> i32;
    fn TerminateProcess(process: isize, code: u32) -> i32;
    fn CreateToolhelp32Snapshot(flags: u32, pid: u32) -> isize;
    fn Module32FirstW(snapshot: isize, entry: *mut ModuleEntry) -> i32;
    fn Module32NextW(snapshot: isize, entry: *mut ModuleEntry) -> i32;
    fn GetModuleHandleW(name: *const u16) -> isize;
    fn WaitForSingleObject(handle: isize, ms: u32) -> u32;
    fn GetUserDefaultUILanguage() -> u16;
}

#[link(name = "ole32")]
extern "system" {
    fn CoInitializeEx(reserved: *const c_void, flags: u32) -> i32;
    fn CoCreateInstance(clsid: *const Guid, outer: *mut c_void, context: u32, iid: *const Guid, object: *mut *mut c_void) -> i32;
}

#[link(name = "oleacc")]
extern "system" {
    fn AccessibleObjectFromWindow(hwnd: Hwnd, id: u32, iid: *const Guid, object: *mut *mut c_void) -> i32;
    fn AccessibleChildren(container: *mut c_void, start: i32, count: i32, children: *mut Variant, obtained: *mut i32) -> i32;
}

#[link(name = "oleaut32")]
extern "system" {
    fn SysStringLen(text: *const u16) -> u32;
    fn SysFreeString(text: *mut u16);
    fn VariantClear(variant: *mut Variant) -> i32;
}

const DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2: isize = -4;
const EVENT_OBJECT_LOCATIONCHANGE: u32 = 0x800B;
const EVENT_OBJECT_CLOAKED: u32 = 0x8017;
const EVENT_OBJECT_UNCLOAKED: u32 = 0x8018;
const WINEVENT_OUTOFCONTEXT: u32 = 0x0000;
const WINEVENT_SKIPOWNPROCESS: u32 = 0x0002;
const OBJID_WINDOW: i32 = 0;
const HWND_TOP: Hwnd = 0;
const SWP_NOSIZE: u32 = 0x0001;
const SWP_NOMOVE: u32 = 0x0002;
const SWP_NOZORDER: u32 = 0x0004;
const SWP_NOACTIVATE: u32 = 0x0010;
const SWP_ASYNCWINDOWPOS: u32 = 0x4000;
const WH_GETMESSAGE: i32 = 3;
const WM_NULL: u32 = 0x0000;
const WM_TIMER: u32 = 0x0113;
/// WM_APP + 0x10: task buttons may have changed. Posted by taskbar-dock.dll when windows come and
/// go (wParam 0) and by taskbar_hooks.dll when Explorer removes (1) or adds (2) a button.
const WM_TASKS_CHANGED: u32 = 0x8010;
const SMTO_ABORTIFHUNG: u32 = 0x0002;
const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
const ERROR_ALREADY_EXISTS: u32 = 183;
const COINIT_APARTMENTTHREADED: u32 = 0x2;
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const PROCESS_TERMINATE: u32 = 0x0001;
const TH32CS_SNAPMODULE: u32 = 0x0008;
const CLSCTX_INPROC_SERVER: u32 = 0x1;
const TREE_SCOPE_CHILDREN: i32 = 2;
const OBJID_CLIENT: u32 = 0xFFFF_FFFC;
const VT_I4: u16 = 3;
const VT_DISPATCH: u16 = 9;
/// IAccessible.
const IID_IACCESSIBLE: Guid = Guid(0x618736e0, 0x3c3d, 0x11cf, [0x81, 0x0c, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71]);
/// MSAA roles of task buttons: push button, button drop-down, button menu.
const TASK_BUTTON_ROLES: [i32; 3] = [0x2B, 0x38, 0x39];
/// CUIAutomation and IUIAutomation.
const CLSID_CUIAUTOMATION: Guid = Guid(0xff48dba4, 0x60ef, 0x4201, [0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e]);
const IID_IUIAUTOMATION: Guid = Guid(0x30cbe57d, 0xd9d0, 0x452a, [0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee]);
/// Makes the task band reload its theme and lay its buttons out again (WM_USER + 82, wParam 3).
const TASKBAND_THEME_RELOAD: u32 = 0x452;
/// Width of the Windows 10 Start button with large taskbar icons, at 96 dpi.
const START_WIDTH_96DPI: i32 = 48;
/// UI Automation id of the visible Start menu inside the Start window.
const START_MENU_FRAME: &str = "SplitViewFrameXAMLWindow";

const APP: &str = "GlassDock";
const VERSION: &str = env!("CARGO_PKG_VERSION");
const REPO_URL: &str = "https://github.com/tien11668899/GlassDock";
/// What runs next to GlassDock.exe.
const COMPANIONS: [&str; 4] = ["taskbar_hooks.dll", "taskbar-dock.dll", "start-glass.dll", "start-glass.exe"];
/// Settings written where there are none yet (the DLLs read them next to themselves).
const DOCK_INI: &str = "; GlassDock's dock (see the README). Restart GlassDock from its menu to apply.\r\n\
frost=2\r\nshadow=100\r\nsmoothing=60\r\nperiod=380\r\nfps=30\r\nlog=0\r\n";
const GLASS_INI: &str = "; GlassDock's Start menu (see the README). Restart GlassDock from its menu to apply.\r\n\
tint=12\r\nluminosity=40\r\nradius=16\r\nsmoothing=60\r\ninner_radius=8\r\nnav_inset=8\r\nrow_gap=4\r\ntile_gap=8\r\n\
row_trim=2\r\ntile_shift=2\r\ntargets=AcrylicBorder\r\nopen=1\r\ncascade=1\r\nlog=0\r\n";
const RUN_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const UNINSTALL_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GlassDock";
/// The hidden window that owns the notification-area icon (and lets another copy find this one).
const TRAY_CLASS: &str = "GlassDockTray";
/// The icon's mouse messages, and another copy saying it was started again.
const WM_TRAY: u32 = 0x8001;
const WM_STARTED_AGAIN: u32 = 0x8002;
const WM_CLOSE: u32 = 0x0010;
const WM_LBUTTONUP: u32 = 0x0202;
const WM_RBUTTONUP: u32 = 0x0205;
const WS_POPUP: u32 = 0x8000_0000;
const WS_EX_TOOLWINDOW: u32 = 0x0000_0080;
const NIM_ADD: u32 = 0;
const NIM_MODIFY: u32 = 1;
const NIM_DELETE: u32 = 2;
const NIF_MESSAGE: u32 = 0x1;
const NIF_ICON: u32 = 0x2;
const NIF_TIP: u32 = 0x4;
const NIF_INFO: u32 = 0x10;
const NIIF_INFO: u32 = 0x1;
const MF_STRING: u32 = 0x0;
const MF_GRAYED: u32 = 0x1;
const MF_CHECKED: u32 = 0x8;
const MF_POPUP: u32 = 0x10;
const MF_SEPARATOR: u32 = 0x800;
const TPM_RIGHTBUTTON: u32 = 0x2;
const TPM_NONOTIFY: u32 = 0x80;
const TPM_RETURNCMD: u32 = 0x100;
const MB_OKCANCEL: u32 = 0x1;
const MB_YESNO: u32 = 0x4;
const MB_ICONERROR: u32 = 0x10;
const MB_ICONWARNING: u32 = 0x30;
const MB_ICONINFORMATION: u32 = 0x40;
const MB_SETFOREGROUND: u32 = 0x1_0000;
const IDOK: i32 = 1;
const IDYES: i32 = 6;
const IMAGE_ICON: u32 = 1;
const SM_CXSMICON: i32 = 49;
const SW_SHOWNORMAL: i32 = 1;
const REG_SZ: u32 = 1;
const REG_DWORD: u32 = 4;
const RRF_RT_REG_SZ: u32 = 0x2;
const SYNCHRONIZE: u32 = 0x0010_0000;
/// The notification-area menu.
const ID_AUTOSTART: usize = 10;
const ID_DOCK_SETTINGS: usize = 11;
const ID_START_SETTINGS: usize = 12;
const ID_RESTART: usize = 13;
const ID_WEBSITE: usize = 14;
const ID_QUIT: usize = 20;
const ID_UNINSTALL: usize = 21;
/// explorer.exe's time stamp in the one build taskbar_hooks.dll patches (Windows 10 22H2,
/// 10.0.19045.2965); elsewhere the DLL leaves Explorer alone and the rest still works.
const KNOWN_EXPLORER: u32 = 0x71fd_c1b4;

/// How GlassDock ends: quitting, restarting, uninstalling.
#[derive(Clone, Copy, PartialEq)]
enum Ending {
    Quit,
    Restart,
    Uninstall,
}

#[derive(Default, Clone, Copy)]
struct Taskbar {
    tray: Hwnd,
    start: Hwnd,
    band: Hwnd,
    list: Hwnd,
    events: isize,
    buttons_hook: isize,
}

/// What Start opens. The Search pane is a window the size of the pane; the Start window is as
/// wide as the screen, with the menu drawn at its left edge.
#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Search = 0,
    Start = 1,
}

const PANEL_PROCESSES: [(&str, Kind); 2] = [("\\searchapp.exe", Kind::Search), ("\\startmenuexperiencehost.exe", Kind::Start)];

/// A panel window and the location-change hook on its process.
#[derive(Default, Clone, Copy)]
struct Panel {
    hwnd: Hwnd,
    pid: u32,
    events: isize,
}

thread_local! {
    static TASKBAR: Cell<Taskbar> = Cell::new(Taskbar::default());
    static PANELS: Cell<[Panel; 2]> = Cell::new([Panel::default(); 2]);
    /// Width of the visible Start menu, 0 until measured.
    static START_MENU_WIDTH: Cell<i32> = Cell::new(0);
    /// Timer that measures the Start menu outside of event callbacks, 0 when none is pending.
    static MEASURE_TIMER: Cell<usize> = Cell::new(0);
    /// Icon grid of the task list: first button's offset from the list's left edge, button
    /// width, last button's right edge from the list's left edge, and the list's left edge
    /// when that was measured (all 0 until measured).
    static CELL: Cell<(i32, i32, i32, i32)> = Cell::new((0, 0, 0, 0));
    /// Timer that measures the icon grid once the icons have stopped moving, 0 when none is pending.
    static CELL_TIMER: Cell<usize> = Cell::new(0);
    /// Timer that measures the icon grid soon after windows came or went, even while the icons
    /// move, 0 when none is pending.
    static TASKS_TIMER: Cell<usize> = Cell::new(0);
    /// Measurements still to try until the row's end changes, and that end when windows came or
    /// went (Explorer takes a moment to drop the button of a closed window).
    static TASKS_TRIES: Cell<(u32, i32)> = Cell::new((0, 0));
    /// IUIAutomation, null if it could not be created.
    static UIA: Cell<usize> = Cell::new(0);
    /// taskbar_hooks.dll loaded here, and its hook procedure: (module, hook_proc).
    static TASKBAR_HOOKS: Cell<(isize, isize)> = Cell::new((0, 0));
    /// taskbar-dock.dll's dock_place; 0 until loaded, -1 if it is not there.
    static DOCK: Cell<isize> = Cell::new(0);
    /// Taskbars found within the last minute and when the first of them was (ms): a crashing
    /// Explorer comes back every few seconds, and taskbar_hooks.dll stays out of it then.
    static TASKBAR_STARTS: Cell<(u32, u64)> = Cell::new((0, 0));
    /// The hook on panels being shown and hidden.
    static CLOAK_HOOK: Cell<isize> = Cell::new(0);
    /// The notification-area icon: its window, its icon, and the message Explorer broadcasts
    /// when it starts again (the icon has to be added again then).
    static TRAY: Cell<(Hwnd, isize, u32)> = Cell::new((0, 0, 0));
    /// How the message loop was ended.
    static ENDING: Cell<Ending> = Cell::new(Ending::Quit);
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn child(parent: Hwnd, class: &str) -> Hwnd {
    unsafe { FindWindowExW(parent, 0, wide(class).as_ptr(), null()) }
}

fn rect(hwnd: Hwnd) -> Rect {
    let mut r = Rect::default();
    unsafe { GetWindowRect(hwnd, &mut r) };
    r
}

fn find_taskbar() -> Option<Taskbar> {
    let tray = unsafe { FindWindowW(wide("Shell_TrayWnd").as_ptr(), null()) };
    if tray == 0 {
        return None;
    }
    let start = child(tray, "Start");
    let band = child(child(tray, "ReBarWindow32"), "MSTaskSwWClass");
    let list = child(band, "MSTaskListWClass");
    if start == 0 || list == 0 {
        return None;
    }
    Some(Taskbar { tray, start, band, list, events: 0, buttons_hook: 0 })
}

/// Start button width Explorer itself uses (large icons), for the taskbar's DPI.
fn start_home_width(tray: Hwnd) -> i32 {
    let dpi = unsafe { GetDpiForWindow(tray) }.max(96) as i32;
    (START_WIDTH_96DPI * dpi + 48) / 96
}

/// Moves `hwnd` (a child window) so that its left edge is at `x` on the screen, keeping its top;
/// nothing when it is there already.
fn move_left_to(hwnd: Hwnd, x: i32) {
    let r = rect(hwnd);
    if hwnd == 0 || r.left == x {
        return;
    }
    let mut p = Point { x, y: r.top };
    unsafe {
        ScreenToClient(GetParent(hwnd), &mut p);
        SetWindowPos(hwnd, 0, p.x, p.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
}

/// Sizes the Start button, keeping where it is.
fn size_start(tb: &Taskbar, width: i32, height: i32) {
    let start = rect(tb.start);
    if width != start.right - start.left || height != start.bottom - start.top {
        unsafe { SetWindowPos(tb.start, HWND_TOP, 0, 0, width, height, SWP_NOMOVE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) };
    }
}

/// Start, the icons and the dock behind them, centred on the taskbar. Start is one more cell of
/// the icon grid right before the first button, as wide as a cell, so its logo sits where an app
/// icon would; the dock leaves the same room around the icons on all four sides; the
/// whole row is centred so the dock is. The task list window moves over to put its buttons there
/// (what TaskbarX used to do), and taskbar-dock.dll moves it and Start, gliding them along with
/// the dock when the row grows or shrinks. Until the grid is measured (MSAA), Start sits in a
/// square cell right before the task list and there is no dock.
/// With `home` (and on a vertical taskbar): Start at its normal place and size on the left edge,
/// the task list right after it as Explorer has it, no dock.
fn park(tb: &Taskbar, home: bool) {
    let (tray, start, band, list) = (rect(tb.tray), rect(tb.start), rect(tb.band), rect(tb.list));
    let height = start.bottom - start.top;
    if home || tray.right - tray.left < tray.bottom - tray.top {
        place_dock(tb, None);
        if home {
            size_start(tb, start_home_width(tb.tray), height);
            move_left_to(tb.start, tray.left);
            move_left_to(tb.list, band.left);
        }
        return;
    }
    let (margin, pitch, end, measured_at) = CELL.with(Cell::get);
    if measured_at != list.left {
        schedule_cell_measure(300); // the icons moved: check the grid again once they settle
    }
    if !(pitch > height / 2 && pitch < height * 2) {
        place_dock(tb, None);
        size_start(tb, height, height);
        move_left_to(tb.start, (list.left - height).max(tray.left));
        return;
    }
    let buttons = ((end - margin) as f32 / pitch as f32).round().max(0.0) as i32;
    let dpi = unsafe { GetDpiForWindow(tb.tray) }.max(96) as i32;
    let icon = (24 * dpi + 48) / 96; // icons are 24 x 24 at 96 dpi
    // An icon's left edge in its cell: taskbar_hooks.dll overlaps the buttons so that the cells
    // (a button widened by Explorer's spacing) are one icon step wide with the icon in the middle.
    let inset = (pitch - icon) / 2;
    let room = (tray.bottom - tray.top - icon) / 2; // above and below the icons, so left and right of them too
    let width = room + buttons * pitch + icon + room; // Start's icon to the last button's
    let left = (tray.left + tray.right - width) / 2;
    let start_x = left + room - inset;
    let list_x = start_x + pitch - margin;
    size_start(tb, pitch, height); // one more cell, its logo in the middle like the icons in theirs
    let shape = Rect { left, top: tray.top, right: left + width, bottom: tray.bottom };
    place_dock(tb, Some((shape, list_x, start_x)));
}

/// The dock behind Start and the icons (taskbar-dock.dll) at `shape`, with the task list and
/// Start to have their left edges at the two x (screen px); the dock puts them there, along with
/// itself when it glides. `None` hides it. Without the DLL they are put there at once.
fn place_dock(tb: &Taskbar, layout: Option<(Rect, i32, i32)>) {
    let place = dock();
    if place <= 0 {
        if let Some((_, list_x, start_x)) = layout {
            move_left_to(tb.list, list_x);
            move_left_to(tb.start, start_x);
        }
        return;
    }
    let tray = rect(tb.tray);
    let dpi = unsafe { GetDpiForWindow(tb.tray) }.max(96) as i32;
    let radius = (8 * dpi + 48) / 96; // the inner radius of the Start menu, 8 at 96 dpi
    let (shape, list_x, start_x) = layout.unwrap_or((Rect::default(), 0, 0));
    let place: DockPlace = unsafe { transmute(place) };
    let thread = unsafe { GetCurrentThreadId() };
    place(tb.tray, &tray, &shape, radius, layout.is_some() as i32, thread, tb.list, list_x, tb.start, start_x);
}

/// Loads taskbar-dock.dll from this exe's folder once: its dock_place, or -1 if it is not there.
fn dock() -> isize {
    let loaded = DOCK.with(Cell::get);
    if loaded != 0 {
        return loaded;
    }
    let place = match exe_dir() {
        Some(dir) => match unsafe { LoadLibraryW(wide(&format!("{dir}\\taskbar-dock.dll")).as_ptr()) } {
            0 => -1,
            module => match unsafe { GetProcAddress(module, b"dock_place\0".as_ptr()) } {
                0 => -1,
                place => place,
            },
        },
        None => -1,
    };
    DOCK.with(|c| c.set(place));
    place
}

fn is_core_window(hwnd: Hwnd) -> bool {
    let mut class = [0u16; 64];
    let len = unsafe { GetClassNameW(hwnd, class.as_mut_ptr(), class.len() as i32) } as usize;
    String::from_utf16_lossy(&class[..len.min(class.len())]) == "Windows.UI.Core.CoreWindow"
}

/// Search pane or Start menu process?
fn panel_kind(pid: u32) -> Option<Kind> {
    let panels = PANELS.with(Cell::get);
    if let Some(kind) = [Kind::Search, Kind::Start].into_iter().find(|k| pid != 0 && panels[*k as usize].pid == pid) {
        return Some(kind);
    }
    let process = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
    if process == 0 {
        return None;
    }
    let mut path = [0u16; 260];
    let mut len = path.len() as u32;
    let ok = unsafe { QueryFullProcessImageNameW(process, 0, path.as_mut_ptr(), &mut len) } != 0;
    unsafe { CloseHandle(process) };
    let path = String::from_utf16_lossy(&path[..(len as usize).min(path.len())]).to_lowercase();
    PANEL_PROCESSES.iter().find(|(name, _)| ok && path.ends_with(name)).map(|(_, kind)| *kind)
}

/// The `index`-th method of a COM object.
unsafe fn com_method(object: *mut c_void, index: usize) -> usize {
    *(*(object as *const *const usize)).add(index)
}

unsafe fn com_release(object: *mut c_void) {
    let release: extern "system" fn(*mut c_void) -> u32 = transmute(com_method(object, 2));
    release(object);
}

/// Right edge of the visible Start menu: the bounding box of the SplitViewFrameXAMLWindow
/// element among the Start window's UI Automation children. Works while the menu is hidden.
fn start_menu_right(hwnd: Hwnd) -> Option<i32> {
    type ElementFromHandle = extern "system" fn(*mut c_void, Hwnd, *mut *mut c_void) -> i32;
    type CreateTrueCondition = extern "system" fn(*mut c_void, *mut *mut c_void) -> i32;
    type FindAll = extern "system" fn(*mut c_void, i32, *mut c_void, *mut *mut c_void) -> i32;
    type Length = extern "system" fn(*mut c_void, *mut i32) -> i32;
    type GetElement = extern "system" fn(*mut c_void, i32, *mut *mut c_void) -> i32;
    type AutomationId = extern "system" fn(*mut c_void, *mut *mut u16) -> i32;
    type BoundingRectangle = extern "system" fn(*mut c_void, *mut Rect) -> i32;

    let uia = UIA.with(Cell::get) as *mut c_void;
    if uia.is_null() {
        return None;
    }
    unsafe {
        // IUIAutomation: 6 ElementFromHandle, 21 CreateTrueCondition. IUIAutomationElement:
        // 6 FindAll, 29 CurrentAutomationId, 43 CurrentBoundingRectangle. Array: 3 Length, 4 GetElement.
        let mut root = null_mut();
        let element_from_handle: ElementFromHandle = transmute(com_method(uia, 6));
        if element_from_handle(uia, hwnd, &mut root) < 0 || root.is_null() {
            return None;
        }
        let mut condition = null_mut();
        let create_true_condition: CreateTrueCondition = transmute(com_method(uia, 21));
        create_true_condition(uia, &mut condition);
        let mut children = null_mut();
        if !condition.is_null() {
            let find_all: FindAll = transmute(com_method(root, 6));
            find_all(root, TREE_SCOPE_CHILDREN, condition, &mut children);
            com_release(condition);
        }
        let mut right = None;
        if !children.is_null() {
            let mut count = 0;
            let length: Length = transmute(com_method(children, 3));
            length(children, &mut count);
            let get_element: GetElement = transmute(com_method(children, 4));
            for i in 0..count {
                let mut element = null_mut();
                if get_element(children, i, &mut element) < 0 || element.is_null() {
                    continue;
                }
                let mut id = null_mut();
                let automation_id: AutomationId = transmute(com_method(element, 29));
                if automation_id(element, &mut id) >= 0 && !id.is_null() {
                    let name = String::from_utf16_lossy(std::slice::from_raw_parts(id, SysStringLen(id) as usize));
                    SysFreeString(id);
                    if name == START_MENU_FRAME {
                        let mut r = Rect::default();
                        let bounding_rectangle: BoundingRectangle = transmute(com_method(element, 43));
                        if bounding_rectangle(element, &mut r) >= 0 {
                            right = Some(r.right);
                        }
                    }
                }
                com_release(element);
                if right.is_some() {
                    break;
                }
            }
            com_release(children);
        }
        com_release(root);
        right
    }
}

/// Task buttons in an MSAA subtree (what TaskbarX measures too): (left, width) of each,
/// from up to three levels of children.
unsafe fn collect_task_buttons(acc: *mut c_void, depth: u32, found: &mut Vec<(i32, i32)>) {
    type ChildCount = extern "system" fn(*mut c_void, *mut i32) -> i32;
    type Role = extern "system" fn(*mut c_void, *const Variant, *mut Variant) -> i32;
    type Location = extern "system" fn(*mut c_void, *mut i32, *mut i32, *mut i32, *mut i32, *const Variant) -> i32;
    type QueryInterface = extern "system" fn(*mut c_void, *const Guid, *mut *mut c_void) -> i32;

    // IAccessible: 8 get_accChildCount, 13 get_accRole, 22 accLocation.
    let mut count = 0;
    let child_count: ChildCount = transmute(com_method(acc, 8));
    if child_count(acc, &mut count) < 0 || count <= 0 || count > 256 {
        return;
    }
    let mut children = vec![Variant::default(); count as usize];
    let mut obtained = 0;
    if AccessibleChildren(acc, 0, count, children.as_mut_ptr(), &mut obtained) < 0 {
        return;
    }
    for child in children.iter_mut().take(obtained.max(0) as usize) {
        if child.vt == VT_DISPATCH && child.data[0] != 0 {
            let dispatch = child.data[0] as *mut c_void;
            let mut inner = null_mut();
            let query_interface: QueryInterface = transmute(com_method(dispatch, 0));
            if depth < 3 && query_interface(dispatch, &IID_IACCESSIBLE, &mut inner) >= 0 && !inner.is_null() {
                collect_task_buttons(inner, depth + 1, found);
                com_release(inner);
            }
        } else if child.vt == VT_I4 {
            let mut role = Variant::default();
            let get_role: Role = transmute(com_method(acc, 13));
            let is_button = get_role(acc, child, &mut role) >= 0
                && role.vt == VT_I4
                && TASK_BUTTON_ROLES.contains(&(role.data[0] as i32));
            VariantClear(&mut role);
            let (mut x, mut y, mut w, mut h) = (0, 0, 0, 0);
            let location: Location = transmute(com_method(acc, 22));
            if is_button && location(acc, &mut x, &mut y, &mut w, &mut h, child) >= 0 && w > 0 && h > 0 {
                found.push((x, w));
            }
        }
        VariantClear(child);
    }
}

/// Measures the icon grid: where the first task button starts inside the task list and how
/// wide a button is. Parks Start again when that changed.
fn measure_cell() {
    let tb = TASKBAR.with(Cell::get);
    if tb.list == 0 {
        return;
    }
    let mut buttons = Vec::new();
    let list_left = rect(tb.list).left;
    unsafe {
        let mut acc = null_mut();
        if AccessibleObjectFromWindow(tb.list, OBJID_CLIENT, &IID_IACCESSIBLE, &mut acc) >= 0 && !acc.is_null() {
            collect_task_buttons(acc, 0, &mut buttons);
            com_release(acc);
        }
    }
    let Some(&(left, width)) = buttons.iter().min_by_key(|b| b.0) else { return };
    let end = buttons.iter().map(|b| b.0 + b.1).max().unwrap_or(left + width);
    let old = CELL.with(Cell::get);
    let mut cell = if rect(tb.list).left == list_left || old.1 == 0 {
        (left - list_left, width, end - list_left, list_left)
    } else {
        // The icons moved while they were measured: trust only the row's own width (first to
        // last button, read together), from the first button's known place in the list.
        (old.0, old.1, old.0 + (end - left), old.3)
    };
    // The row only ever grows or shrinks by whole buttons: round off what a button sliding or
    // collapsing in the middle of a measurement adds.
    if cell.1 > 0 {
        let buttons = ((cell.2 - cell.0) as f32 / cell.1 as f32).round() as i32;
        cell.2 = cell.0 + buttons.max(0) * cell.1;
    }
    CELL.with(|c| c.set(cell));
    if (old.0, old.1, old.2) != (cell.0, cell.1, cell.2) {
        park(&tb, false);
    }
}

/// Measures the grid `ms` after the last call (TaskbarX animates the icons into place).
fn schedule_cell_measure(ms: u32) {
    let id = unsafe { SetTimer(0, CELL_TIMER.with(Cell::get), ms, 0) };
    CELL_TIMER.with(|c| c.set(id));
}

/// Measures the grid `ms` from now, up to `tries` times until the row's end changes.
fn schedule_tasks_measure(ms: u32, tries: u32) {
    TASKS_TRIES.with(|c| c.set((tries, CELL.with(Cell::get).2)));
    let id = unsafe { SetTimer(0, TASKS_TIMER.with(Cell::get), ms, 0) };
    TASKS_TIMER.with(|c| c.set(id));
}

/// Measures how wide the Start menu is (from the Start window's left edge to the menu's right edge).
fn measure_start_menu(hwnd: Hwnd) {
    if let Some(right) = start_menu_right(hwnd) {
        let width = right - rect(hwnd).left;
        if width > 0 && width < 4000 {
            START_MENU_WIDTH.with(|c| c.set(width));
        }
    }
}

/// Measuring talks to another process through COM, so it never runs inside an event callback.
fn schedule_measure() {
    if MEASURE_TIMER.with(Cell::get) == 0 {
        let id = unsafe { SetTimer(0, 0, 60, 0) };
        MEASURE_TIMER.with(|c| c.set(id));
    }
}

/// Centres a panel that sits on top of the taskbar on the screen: the Search pane itself, or
/// the Start window shifted so the menu at its left edge is in the middle. With `home`, puts
/// it back where Windows has it with the Start button on the left edge.
/// Async, because the Search app is often suspended while its pane is hidden.
fn place_panel(kind: Kind, hwnd: Hwnd, home: bool) {
    let Some(tb) = find_taskbar() else { return };
    let (tray, panel) = (rect(tb.tray), rect(hwnd));
    let horizontal = tray.right - tray.left > tray.bottom - tray.top;
    if !horizontal || (panel.bottom - tray.top).abs() > 4 || panel.right <= panel.left {
        return;
    }
    let centre = (tray.left + tray.right) / 2;
    let x = match (kind, home) {
        (Kind::Search, true) => tray.left + start_home_width(tb.tray),
        (Kind::Search, false) => centre - (panel.right - panel.left) / 2,
        (Kind::Start, true) => tray.left,
        (Kind::Start, false) => match START_MENU_WIDTH.with(Cell::get) {
            0 => return,
            width => centre - width / 2,
        },
    };
    if (x - panel.left).abs() > 1 {
        unsafe {
            SetWindowPos(hwnd, 0, x, panel.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
    }
}

extern "system" fn on_panel_moved(_: isize, _: u32, hwnd: Hwnd, object: i32, _: i32, _: u32, _: u32) {
    if object != OBJID_WINDOW {
        return;
    }
    let panels = PANELS.with(Cell::get);
    if let Some(kind) = [Kind::Search, Kind::Start].into_iter().find(|k| panels[*k as usize].hwnd == hwnd) {
        place_panel(kind, hwnd, false);
    }
}

/// Follows the panel's moves too, so a re-anchored panel is centred again before it shows.
fn watch_panel(kind: Kind, hwnd: Hwnd, pid: u32) {
    let mut panels = PANELS.with(Cell::get);
    let panel = &mut panels[kind as usize];
    panel.hwnd = hwnd;
    if panel.pid != pid {
        unsafe {
            if panel.events != 0 {
                UnhookWinEvent(panel.events);
            }
            panel.events = SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE,
                EVENT_OBJECT_LOCATIONCHANGE,
                0,
                on_panel_moved,
                pid,
                0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
            );
        }
        panel.pid = pid;
        if kind == Kind::Start {
            glass_start_menu(pid); // a new Start host: give it the frosted glass
        }
    }
    PANELS.with(|c| c.set(panels));
}

/// Folder of this exe, where its companions (taskbar_hooks.dll, start-glass.exe) live.
fn exe_dir() -> Option<String> {
    let mut buffer = [0u16; 260];
    let len = unsafe { GetModuleFileNameW(0, buffer.as_mut_ptr(), buffer.len() as u32) } as usize;
    let exe = String::from_utf16_lossy(&buffer[..len.min(buffer.len())]);
    exe.rfind('\\').map(|i| exe[..i].to_string())
}

/// Frosted-glass Start menu: start-glass.exe (next to this exe) loads start-glass.dll into the
/// Start menu's host through XAML Diagnostics, then exits. Nothing happens if it is not there.
fn glass_start_menu(pid: u32) {
    let Some(dir) = exe_dir() else { return };
    let exe = format!("{dir}\\start-glass.exe");
    let mut command = wide(&format!("\"{exe}\" {pid}"));
    unsafe {
        let mut startup: StartupInfo = std::mem::zeroed();
        startup.cb = std::mem::size_of::<StartupInfo>() as u32;
        let mut info: ProcessInformation = std::mem::zeroed();
        let started = CreateProcessW(
            wide(&exe).as_ptr(),
            command.as_mut_ptr(),
            null(),
            null(),
            0,
            CREATE_NO_WINDOW,
            null(),
            null(),
            &startup,
            &mut info,
        );
        if started != 0 {
            CloseHandle(info.thread);
            CloseHandle(info.process);
        }
    }
}

/// Is start-glass.dll loaded in that process?
fn has_glass(pid: u32) -> bool {
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
        if snapshot == -1 {
            return false;
        }
        let mut entry: ModuleEntry = std::mem::zeroed();
        entry.size = std::mem::size_of::<ModuleEntry>() as u32;
        let mut found = false;
        let mut more = Module32FirstW(snapshot, &mut entry) != 0;
        while more && !found {
            let len = entry.name.iter().position(|&c| c == 0).unwrap_or(entry.name.len());
            found = String::from_utf16_lossy(&entry.name[..len]).eq_ignore_ascii_case("start-glass.dll");
            more = Module32NextW(snapshot, &mut entry) != 0;
        }
        CloseHandle(snapshot);
        found
    }
}

/// Ends a Start host that has start-glass.dll; Windows starts a fresh one when Start is next used.
fn end_glass(pid: u32) {
    if has_glass(pid) {
        unsafe {
            let process = OpenProcess(PROCESS_TERMINATE, 0, pid);
            if process != 0 {
                TerminateProcess(process, 0);
                CloseHandle(process);
            }
        }
    }
}

/// A panel was shown or hidden. Shown: centre it (the Start menu once its width is known).
/// Start menu hidden: measure it again, the menu may have been resized while it was open.
extern "system" fn on_cloak_change(_: isize, event: u32, hwnd: Hwnd, object: i32, _: i32, _: u32, _: u32) {
    if object != OBJID_WINDOW || !is_core_window(hwnd) {
        return;
    }
    let mut pid = 0;
    unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
    let Some(kind) = panel_kind(pid) else { return };
    watch_panel(kind, hwnd, pid);
    let shown = event == EVENT_OBJECT_UNCLOAKED;
    match kind {
        Kind::Search if shown => place_panel(kind, hwnd, false),
        Kind::Start if shown && START_MENU_WIDTH.with(Cell::get) > 0 => place_panel(kind, hwnd, false),
        Kind::Start => schedule_measure(),
        Kind::Search => {}
    }
}

/// Existing Search pane / Start windows. They live in a shell z-order band that EnumWindows
/// skips, so walk the top-level CoreWindows with FindWindowEx instead.
fn panels() -> Vec<(Kind, Hwnd, u32)> {
    let class = wide("Windows.UI.Core.CoreWindow");
    let mut found = Vec::new();
    let mut hwnd = 0;
    loop {
        hwnd = unsafe { FindWindowExW(0, hwnd, class.as_ptr(), null()) };
        if hwnd == 0 {
            return found;
        }
        let mut pid = 0;
        unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
        if let Some(kind) = panel_kind(pid) {
            found.push((kind, hwnd, pid));
        }
    }
}

extern "system" fn on_location_change(_: isize, _: u32, hwnd: Hwnd, object: i32, _: i32, _: u32, _: u32) {
    if object != OBJID_WINDOW {
        return;
    }
    let tb = TASKBAR.with(Cell::get);
    if hwnd == tb.list || hwnd == tb.start {
        park(&tb, false);
    }
}

/// Loads taskbar_hooks.dll from this exe's folder once; (0, 0) if it is not there.
fn taskbar_hooks() -> (isize, isize) {
    let loaded = TASKBAR_HOOKS.with(Cell::get);
    if loaded.1 != 0 {
        return loaded;
    }
    let Some(dir) = exe_dir() else { return (0, 0) };
    let module = unsafe { LoadLibraryW(wide(&format!("{dir}\\taskbar_hooks.dll")).as_ptr()) };
    if module == 0 {
        return (0, 0);
    }
    let hook_proc = unsafe { GetProcAddress(module, b"hook_proc\0".as_ptr()) };
    TASKBAR_HOOKS.with(|c| c.set((module, hook_proc)));
    (module, hook_proc)
}

/// (Re)attaches to the taskbar when Explorer (re)starts, then parks Start.
fn refresh() {
    let mut tb = TASKBAR.with(Cell::get);
    let alive = tb.tray != 0 && unsafe { IsWindow(tb.tray) != 0 && IsWindow(tb.start) != 0 && IsWindow(tb.list) != 0 };
    if !alive {
        unsafe {
            if tb.events != 0 {
                UnhookWinEvent(tb.events);
            }
            if tb.buttons_hook != 0 {
                UnhookWindowsHookEx(tb.buttons_hook);
            }
        }
        tb = match find_taskbar() {
            Some(found) => found,
            None => {
                TASKBAR.with(|c| c.set(Taskbar::default()));
                place_dock(&Taskbar::default(), None); // no taskbar, no dock
                return;
            }
        };
        // Only the taskbar thread's events: Explorer lays the icons and Start out there.
        let mut pid = 0;
        let tid = unsafe { GetWindowThreadProcessId(tb.tray, &mut pid) };
        tb.events = unsafe {
            SetWinEventHook(
                EVENT_OBJECT_LOCATIONCHANGE,
                EVENT_OBJECT_LOCATIONCHANGE,
                0,
                on_location_change,
                pid,
                tid,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
            )
        };
        // Square icon buttons and the Windows 11 Start logo: the hook loads taskbar_hooks.dll
        // into that thread only. Not if Explorer keeps restarting (it may be crashing on it).
        let now = unsafe { GetTickCount64() };
        let (starts, since) = TASKBAR_STARTS.with(Cell::get);
        let starts = if now - since < 60_000 { starts + 1 } else { 1 };
        TASKBAR_STARTS.with(|c| c.set((starts, if starts == 1 { now } else { since })));
        let (module, hook_proc) = if starts <= 3 { taskbar_hooks() } else { (0, 0) };
        if hook_proc != 0 {
            unsafe {
                tb.buttons_hook = SetWindowsHookExW(WH_GETMESSAGE, hook_proc, module, tid);
                PostMessageW(tb.tray, WM_NULL, 0, 0);
            }
        }
        TASKBAR.with(|c| c.set(tb));
    }
    park(&tb, false);
}

// ---- the app: notification-area icon, settings, install and uninstall ----

/// Chinese when Windows shows Chinese, else English.
fn text(chinese: &'static str, english: &'static str) -> &'static str {
    if unsafe { GetUserDefaultUILanguage() } & 0x3FF == 0x04 {
        chinese
    } else {
        english
    }
}

fn message(text: &str, flags: u32) -> i32 {
    unsafe { MessageBoxW(0, wide(text).as_ptr(), wide(APP).as_ptr(), flags | MB_SETFOREGROUND) }
}

/// Copies `text` into a fixed-size UTF-16 field, cut short to fit with its terminating 0.
fn fill(field: &mut [u16], text: &str) {
    let units: Vec<u16> = text.encode_utf16().take(field.len() - 1).collect();
    field[..units.len()].copy_from_slice(&units);
    field[units.len()] = 0;
}

fn windows_build() -> u32 {
    let mut info: OsVersionInfo = unsafe { std::mem::zeroed() };
    info.size = std::mem::size_of::<OsVersionInfo>() as u32;
    unsafe { RtlGetVersion(&mut info) };
    info.build
}

/// Whether Explorer is the build taskbar_hooks.dll changes: the time stamp in explorer.exe's header.
fn explorer_known() -> bool {
    use std::io::Read;
    let Ok(windows) = std::env::var("WINDIR") else { return false };
    let mut header = [0u8; 1024];
    let read = std::fs::File::open(format!("{windows}\\explorer.exe")).and_then(|mut file| file.read_exact(&mut header));
    if read.is_err() {
        return false;
    }
    let pe = u32::from_le_bytes([header[0x3C], header[0x3D], header[0x3E], header[0x3F]]) as usize;
    pe + 12 <= header.len()
        && header[pe..pe + 4] == *b"PE\0\0"
        && u32::from_le_bytes([header[pe + 8], header[pe + 9], header[pe + 10], header[pe + 11]]) == KNOWN_EXPLORER
}

/// HKEY_CURRENT_USER.
fn current_user() -> isize {
    0x8000_0001u32 as i32 as isize
}

fn set_registry_string(key: &str, name: &str, value: &str) {
    let data = wide(value);
    unsafe {
        let size = (data.len() * 2) as u32;
        RegSetKeyValueW(current_user(), wide(key).as_ptr(), wide(name).as_ptr(), REG_SZ, data.as_ptr() as *const c_void, size);
    }
}

fn set_registry_number(key: &str, name: &str, value: u32) {
    unsafe {
        let data = &value as *const u32 as *const c_void;
        RegSetKeyValueW(current_user(), wide(key).as_ptr(), wide(name).as_ptr(), REG_DWORD, data, 4);
    }
}

fn registry_string(key: &str, name: &str) -> Option<String> {
    let (key, name) = (wide(key), wide(name));
    let mut size = 0u32;
    unsafe {
        if RegGetValueW(current_user(), key.as_ptr(), name.as_ptr(), RRF_RT_REG_SZ, null_mut(), null_mut(), &mut size) != 0 {
            return None;
        }
        let mut buffer = vec![0u16; size as usize / 2 + 1];
        let data = buffer.as_mut_ptr() as *mut c_void;
        if RegGetValueW(current_user(), key.as_ptr(), name.as_ptr(), RRF_RT_REG_SZ, null_mut(), data, &mut size) != 0 {
            return None;
        }
        let len = buffer.iter().position(|&c| c == 0).unwrap_or(buffer.len());
        Some(String::from_utf16_lossy(&buffer[..len]))
    }
}

fn delete_registry_value(key: &str, name: &str) {
    unsafe { RegDeleteKeyValueW(current_user(), wide(key).as_ptr(), wide(name).as_ptr()) };
}

/// Where GlassDock installs itself: %LOCALAPPDATA%\Programs\GlassDock.
fn install_dir() -> Option<String> {
    std::env::var("LOCALAPPDATA").ok().map(|local| format!("{local}\\Programs\\{APP}"))
}

fn runs_installed() -> bool {
    match (exe_dir(), install_dir()) {
        (Some(here), Some(home)) => here.eq_ignore_ascii_case(&home),
        _ => false,
    }
}

fn starts_with_windows() -> bool {
    registry_string(RUN_KEY, APP).is_some()
}

fn set_starts_with_windows(on: bool) {
    match std::env::current_exe() {
        Ok(exe) if on => set_registry_string(RUN_KEY, APP, &format!("\"{}\"", exe.display())),
        _ => delete_registry_value(RUN_KEY, APP),
    }
}

/// Starting with Windows, and an entry in Apps & features, whose Uninstall runs `--uninstall`.
fn register(dir: &str) {
    let exe = format!("{dir}\\GlassDock.exe");
    set_registry_string(RUN_KEY, APP, &format!("\"{exe}\""));
    for (name, value) in [
        ("DisplayName", APP.to_string()),
        ("DisplayVersion", VERSION.to_string()),
        ("Publisher", "tien11668899".to_string()),
        ("DisplayIcon", exe.clone()),
        ("InstallLocation", dir.to_string()),
        ("UninstallString", format!("\"{exe}\" --uninstall")),
        ("QuietUninstallString", format!("\"{exe}\" --uninstall --quiet")),
        ("URLInfoAbout", REPO_URL.to_string()),
    ] {
        set_registry_string(UNINSTALL_KEY, name, &value);
    }
    for (name, value) in [("NoModify", 1), ("NoRepair", 1), ("EstimatedSize", 1300)] {
        set_registry_number(UNINSTALL_KEY, name, value);
    }
}

fn unregister() {
    delete_registry_value(RUN_KEY, APP);
    unsafe { RegDeleteKeyW(current_user(), wide(UNINSTALL_KEY).as_ptr()) };
}

/// Opens a settings file (next to the exe, where the DLLs read it) in Notepad, writing the
/// defaults first when it is not there.
fn edit_settings(name: &str, defaults: &str) {
    let Some(dir) = exe_dir() else { return };
    let path = format!("{dir}\\{name}");
    if !Path::new(&path).exists() {
        let _ = std::fs::write(&path, defaults);
    }
    let file = wide(&format!("\"{path}\""));
    unsafe { ShellExecuteW(0, wide("open").as_ptr(), wide("notepad.exe").as_ptr(), file.as_ptr(), null(), SW_SHOWNORMAL) };
}

fn open_website() {
    unsafe { ShellExecuteW(0, wide("open").as_ptr(), wide(REPO_URL).as_ptr(), null(), null(), SW_SHOWNORMAL) };
}

/// The notification-area icon and the hidden window it reports to.
fn create_tray() {
    unsafe {
        let instance = GetModuleHandleW(null());
        let class = wide(TRAY_CLASS);
        let window_class = WndClassEx {
            size: std::mem::size_of::<WndClassEx>() as u32,
            style: 0,
            proc: tray_proc,
            class_extra: 0,
            window_extra: 0,
            instance,
            icon: 0,
            cursor: 0,
            background: 0,
            menu_name: null(),
            class_name: class.as_ptr(),
            small_icon: 0,
        };
        RegisterClassExW(&window_class);
        let hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, class.as_ptr(), wide(APP).as_ptr(), WS_POPUP, 0, 0, 0, 0, 0, 0, instance, null());
        let size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
        let icon = LoadImageW(instance, 2 as *const u16, IMAGE_ICON, size, size, 0); // tray.ico
        let explorer_started = RegisterWindowMessageW(wide("TaskbarCreated").as_ptr());
        TRAY.with(|c| c.set((hwnd, icon, explorer_started)));
    }
}

/// Adds (`NIM_ADD`) or updates (`NIM_MODIFY`) the notification-area icon; `note`: a notification
/// (title, text) to show with it.
fn tray_icon(operation: u32, note: Option<(&str, &str)>) {
    let (hwnd, icon, _) = TRAY.with(Cell::get);
    let mut data: NotifyIconData = unsafe { std::mem::zeroed() };
    data.size = std::mem::size_of::<NotifyIconData>() as u32;
    data.hwnd = hwnd;
    data.id = 1;
    if operation != NIM_DELETE {
        data.flags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.callback_message = WM_TRAY;
        data.icon = icon;
        fill(&mut data.tip, APP);
    }
    if let Some((title, body)) = note {
        data.flags |= NIF_INFO;
        fill(&mut data.info_title, title);
        fill(&mut data.info, body);
        data.info_flags = NIIF_INFO;
    }
    unsafe { Shell_NotifyIconW(operation, &data) };
}

extern "system" fn tray_proc(hwnd: Hwnd, message: u32, wparam: usize, lparam: isize) -> isize {
    match message {
        WM_TRAY if matches!(lparam as u32, WM_LBUTTONUP | WM_RBUTTONUP) => show_menu(hwnd),
        WM_STARTED_AGAIN => tray_icon(
            NIM_MODIFY,
            Some((
                APP,
                text("GlassDock 已經在執行了，設定都在這個圖示的選單裡。", "GlassDock is already running; its settings are in this icon's menu."),
            )),
        ),
        WM_CLOSE => end(Ending::Quit),
        _ if message != 0 && message == TRAY.with(Cell::get).2 => tray_icon(NIM_ADD, None), // Explorer started again
        _ => return unsafe { DefWindowProcW(hwnd, message, wparam, lparam) },
    }
    0
}

/// Ends the message loop; `main` then puts the taskbar back and does the rest of `ending`.
fn end(ending: Ending) {
    ENDING.with(|c| c.set(ending));
    unsafe { PostQuitMessage(0) };
}

fn show_menu(hwnd: Hwnd) {
    unsafe {
        let menu = CreatePopupMenu();
        let settings = CreatePopupMenu();
        let add = |menu: isize, flags: u32, id: usize, label: &str| {
            AppendMenuW(menu, flags, id, wide(label).as_ptr());
        };
        add(menu, MF_STRING | MF_GRAYED, 0, &format!("{APP} {VERSION}"));
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        let autostart = if starts_with_windows() { MF_CHECKED } else { MF_STRING };
        add(menu, autostart, ID_AUTOSTART, text("開機時啟動", "Start with Windows"));
        add(settings, MF_STRING, ID_DOCK_SETTINGS, text("Dock（taskbar-dock.ini）", "Dock (taskbar-dock.ini)"));
        add(settings, MF_STRING, ID_START_SETTINGS, text("開始選單（start-glass.ini）", "Start menu (start-glass.ini)"));
        add(menu, MF_POPUP, settings as usize, text("設定", "Settings"));
        add(menu, MF_STRING, ID_RESTART, text("重新啟動（套用設定）", "Restart (applies the settings)"));
        add(menu, MF_STRING, ID_WEBSITE, text("GitHub 專案頁面", "GitHub page"));
        AppendMenuW(menu, MF_SEPARATOR, 0, null());
        add(menu, MF_STRING, ID_QUIT, text("結束（還原工作列）", "Quit (restores the taskbar)"));
        add(menu, MF_STRING, ID_UNINSTALL, text("解除安裝…", "Uninstall…"));
        let mut at = Point::default();
        GetCursorPos(&mut at);
        SetForegroundWindow(hwnd); // so that the menu closes when the user clicks elsewhere
        let chosen = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, at.x, at.y, 0, hwnd, null()) as usize;
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu); // and the settings submenu with it
        match chosen {
            ID_AUTOSTART => set_starts_with_windows(!starts_with_windows()),
            ID_DOCK_SETTINGS => edit_settings("taskbar-dock.ini", DOCK_INI),
            ID_START_SETTINGS => edit_settings("start-glass.ini", GLASS_INI),
            ID_RESTART => end(Ending::Restart),
            ID_WEBSITE => open_website(),
            ID_QUIT => end(Ending::Quit),
            ID_UNINSTALL if confirm_uninstall() => end(Ending::Uninstall),
            _ => {}
        }
    }
}

fn confirm_uninstall() -> bool {
    let question = text(
        "要解除安裝 GlassDock 嗎？\n工作列會還原成 Windows 原本的樣子，GlassDock 的資料夾和設定也會刪除。",
        "Uninstall GlassDock?\nThe taskbar goes back to how Windows has it, and GlassDock's folder and settings are deleted.",
    );
    message(question, MB_YESNO | MB_ICONWARNING) == IDYES
}

/// The one GlassDock: a named mutex. Started again by GlassDock itself (restarting, installing),
/// it waits up to 10 s for the one before to be gone.
fn single_instance(wait: bool) -> Option<isize> {
    let name = wide("Local\\GlassDock");
    for _ in 0..if wait { 100 } else { 1 } {
        unsafe {
            let mutex = CreateMutexW(null(), 0, name.as_ptr());
            if GetLastError() != ERROR_ALREADY_EXISTS {
                return Some(mutex);
            }
            CloseHandle(mutex);
            Sleep(100);
        }
    }
    None
}

/// Asks the GlassDock running now (if one is) to quit, which puts the taskbar back, and waits
/// until it is gone. False when none was running.
fn stop_running() -> bool {
    let hwnd = unsafe { FindWindowW(wide(TRAY_CLASS).as_ptr(), null()) };
    if hwnd == 0 {
        return false;
    }
    unsafe {
        let mut pid = 0;
        GetWindowThreadProcessId(hwnd, &mut pid);
        let process = OpenProcess(SYNCHRONIZE, 0, pid);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if process != 0 {
            WaitForSingleObject(process, 15_000);
            CloseHandle(process);
        }
    }
    true
}

/// Files an update renamed out of the way because they were still loaded.
fn remove_old_files() {
    let Some(dir) = exe_dir() else { return };
    for entry in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        if entry.file_name().to_string_lossy().ends_with(".old") {
            let _ = std::fs::remove_file(entry.path());
        }
    }
}

/// The Start menu's host is an AppContainer: it may load start-glass.dll and read its settings
/// only when the folder lets ALL APPLICATION PACKAGES (S-1-15-2-1) read it.
fn grant_app_containers(dir: &str) {
    let _ = Command::new("icacls.exe")
        .arg(dir)
        .args(["/grant", "*S-1-15-2-1:(OI)(CI)(RX)"])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .creation_flags(CREATE_NO_WINDOW)
        .status();
}

fn same_file(a: &Path, b: &Path) -> bool {
    matches!((std::fs::canonicalize(a), std::fs::canonicalize(b)), (Ok(a), Ok(b)) if a == b)
}

/// Copies this exe and its companions from `source` into `target`. A file Windows still has
/// loaded (Explorer keeps taskbar_hooks.dll until the taskbar's next message, a Start host
/// start-glass.dll until it ends) cannot be overwritten, but it can be renamed out of the way.
fn copy_into(exe: &Path, source: &str, target: &str) -> std::io::Result<()> {
    std::fs::create_dir_all(target)?;
    grant_app_containers(target);
    let mut files = vec![(exe.to_path_buf(), PathBuf::from(format!("{target}\\GlassDock.exe")))];
    for name in COMPANIONS {
        files.push((PathBuf::from(format!("{source}\\{name}")), PathBuf::from(format!("{target}\\{name}"))));
    }
    for (from, to) in files {
        if same_file(&from, &to) {
            continue; // installing from the install folder itself
        }
        if std::fs::copy(&from, &to).is_err() {
            let aside = format!("{}.{}.old", to.display(), unsafe { GetTickCount64() });
            std::fs::rename(&to, aside)?;
            std::fs::copy(&from, &to)?;
        }
    }
    Ok(())
}

/// Installs GlassDock from this exe's folder into its own (`install_dir`): the copy running there
/// quits (which puts the taskbar back), this exe and its companions go there, the default
/// settings go where there are none, and it is registered and started. `ask`: asks first and
/// says what went wrong.
fn install(ask: bool) {
    let (Some(source), Some(target), Ok(exe)) = (exe_dir(), install_dir(), std::env::current_exe()) else { return };
    if ask {
        let question = format!(
            "{}\n\n{target}",
            text(
                "要安裝 GlassDock 嗎？它會裝到下面這個資料夾，並在開機時啟動（已經裝過的會更新，設定會保留）：",
                "Install GlassDock? It goes into the folder below and starts with Windows (an installed one is updated, its settings kept):",
            )
        );
        if message(&question, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK {
            return;
        }
    }
    let missing: Vec<&str> = COMPANIONS.into_iter().filter(|name| !Path::new(&format!("{source}\\{name}")).exists()).collect();
    if !missing.is_empty() {
        if ask {
            let problem = text(
                "GlassDock.exe 旁邊少了這些檔案，請把下載的 zip 整個解壓縮，再從解壓出來的資料夾執行：",
                "These files are missing next to GlassDock.exe; extract the whole zip and run it from there:",
            );
            message(&format!("{problem}\n\n{}", missing.join("\n")), MB_ICONERROR);
        }
        return;
    }
    stop_running();
    if let Err(error) = copy_into(&exe, &source, &target) {
        if ask {
            message(&format!("{}\n\n{error}", text("安裝失敗：", "Installing failed:")), MB_ICONERROR);
        }
        return;
    }
    for (name, defaults) in [("taskbar-dock.ini", DOCK_INI), ("start-glass.ini", GLASS_INI)] {
        let path = format!("{target}\\{name}");
        if !Path::new(&path).exists() {
            let _ = std::fs::write(&path, defaults);
        }
    }
    register(&target);
    let _ = Command::new(format!("{target}\\GlassDock.exe")).arg("--installed").spawn();
}

/// Uninstalls GlassDock (Apps & features runs this): the running copy quits (which puts the
/// taskbar back; with none running, this does), then `finish_uninstall`. `ask`: asks first and
/// says when it is done.
fn uninstall(ask: bool) {
    if ask && !confirm_uninstall() {
        return;
    }
    if !stop_running() {
        restore_taskbar();
    }
    finish_uninstall(ask);
}

/// The registration goes, and GlassDock's folder too once this exe (in it) has ended.
fn finish_uninstall(say: bool) {
    unregister();
    if say {
        message(text("GlassDock 已解除安裝。", "GlassDock has been uninstalled."), MB_ICONINFORMATION);
    }
    if let Some(dir) = install_dir().filter(|dir| Path::new(dir).exists()) {
        // A command prompt deletes it, trying every second for half a minute: this exe is in it,
        // and Explorer may keep taskbar_hooks.dll a moment longer.
        let script = format!("/c for /l %i in (1,1,30) do @if exist \"{dir}\" (rmdir /s /q \"{dir}\" 2>nul & ping -n 2 127.0.0.1 >nul)");
        let _ = Command::new("cmd.exe").raw_arg(script).current_dir(std::env::temp_dir()).creation_flags(CREATE_NO_WINDOW).spawn();
    }
}

/// Puts Start, the icons and the panels back where Windows has them, ends a Start host that has
/// the frosted glass (it can only leave with its process), and has the taskbar lay its buttons
/// out again at their normal width once Explorer has dropped taskbar_hooks.dll (at the taskbar
/// thread's next message after the hook is gone).
fn restore_taskbar() {
    let Some(tb) = find_taskbar() else { return };
    park(&tb, true);
    for (kind, hwnd, pid) in panels() {
        place_panel(kind, hwnd, true);
        if kind == Kind::Start {
            end_glass(pid);
        }
    }
    unsafe {
        PostMessageW(tb.tray, WM_NULL, 0, 0);
        Sleep(500);
        let mut result = 0usize;
        SendMessageTimeoutW(tb.band, TASKBAND_THEME_RELOAD, 3, 0, SMTO_ABORTIFHUNG, 2000, &mut result);
    }
}

/// Unhooks everything (Explorer then drops taskbar_hooks.dll), hides the dock and puts the
/// taskbar back.
fn shut_down() {
    let tb = TASKBAR.with(Cell::get);
    let panels = PANELS.with(Cell::get);
    unsafe {
        for hook in [CLOAK_HOOK.with(Cell::get), tb.events, panels[0].events, panels[1].events] {
            if hook != 0 {
                UnhookWinEvent(hook);
            }
        }
        if tb.buttons_hook != 0 {
            UnhookWindowsHookEx(tb.buttons_hook);
        }
    }
    if tb.tray != 0 {
        place_dock(&tb, None);
        unsafe { Sleep(50) }; // the dock's thread stops moving Start and the icons
    }
    restore_taskbar();
}

fn main() {
    unsafe { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) };
    let args: Vec<String> = std::env::args().skip(1).collect();
    let flag = |name: &str| args.iter().any(|a| a == name);

    if flag("--restore") {
        restore_taskbar();
        return;
    }
    if windows_build() >= 22000 {
        message(
            text("GlassDock 是給 Windows 10 用的：Windows 11 的工作列本來就是這個樣子。", "GlassDock is for Windows 10: the Windows 11 taskbar already looks like this."),
            MB_ICONINFORMATION,
        );
        return;
    }
    if flag("--uninstall") {
        uninstall(!flag("--quiet"));
        return;
    }
    if flag("--install") || !runs_installed() {
        install(!flag("--quiet"));
        return;
    }

    // One copy is enough; a second one would only repeat the first one's moves. Started again,
    // it has the running one say so.
    let Some(mutex) = single_instance(flag("--restart") || flag("--installed")) else {
        let running = unsafe { FindWindowW(wide(TRAY_CLASS).as_ptr(), null()) };
        if running != 0 {
            unsafe { PostMessageW(running, WM_STARTED_AGAIN, 0, 0) };
        }
        return;
    };
    remove_old_files();

    // COM: MSAA measures the icon grid, UI Automation the Start menu.
    unsafe { CoInitializeEx(null(), COINIT_APARTMENTTHREADED) };

    create_tray();
    let note = flag("--installed").then(|| {
        let body = if explorer_known() {
            text("這個圖示的選單裡有設定、結束和解除安裝。", "This icon's menu has the settings, Quit and Uninstall.")
        } else {
            text(
                "這個版本的 Explorer 沒有方形按鈕和 Windows 11 開始圖示，其他功能照常。選單在這個圖示上。",
                "This Explorer build gets no square buttons or Windows 11 Start logo; the rest works. The menu is on this icon.",
            )
        };
        (text("GlassDock 已啟動", "GlassDock is running"), body)
    });
    tray_icon(NIM_ADD, note);

    refresh();

    // Panels Start opens: UI Automation to measure the Start menu, an event whenever a panel
    // is shown or hidden, and the panels that already exist.
    unsafe {
        let mut uia = null_mut();
        if CoCreateInstance(&CLSID_CUIAUTOMATION, null_mut(), CLSCTX_INPROC_SERVER, &IID_IUIAUTOMATION, &mut uia) >= 0 {
            UIA.with(|c| c.set(uia as usize));
        }
        let hook = SetWinEventHook(
            EVENT_OBJECT_CLOAKED,
            EVENT_OBJECT_UNCLOAKED,
            0,
            on_cloak_change,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS,
        );
        CLOAK_HOOK.with(|c| c.set(hook));
    }
    for (kind, hwnd, pid) in panels() {
        watch_panel(kind, hwnd, pid);
        if kind == Kind::Start {
            measure_start_menu(hwnd);
        }
        place_panel(kind, hwnd, false);
    }

    unsafe { SetTimer(0, 0, 2000, 0) };
    let mut msg = Msg::default();
    while unsafe { GetMessageW(&mut msg, 0, 0, 0) } > 0 {
        if msg.hwnd == 0 && msg.message == WM_TASKS_CHANGED {
            // From taskbar_hooks.dll (wParam 1 or 2): Explorer just removed or added a button.
            // From the dock (0): windows came or went, so a button may follow shortly.
            // Measure soon, whatever the icons do meanwhile.
            if msg.wparam == 0 {
                schedule_tasks_measure(120, 8);
            } else {
                schedule_tasks_measure(16, 3);
            }
            continue;
        }
        // Thread timers are ours; window timers belong to the dock's composition.
        if msg.message != WM_TIMER || msg.hwnd != 0 {
            unsafe { DispatchMessageW(&msg) };
            continue;
        }
        let cell = CELL_TIMER.with(Cell::get);
        let tasks = TASKS_TIMER.with(Cell::get);
        let measure = MEASURE_TIMER.with(Cell::get);
        if cell != 0 && msg.wparam == cell {
            unsafe { KillTimer(0, cell) };
            CELL_TIMER.with(|c| c.set(0));
            measure_cell();
        } else if tasks != 0 && msg.wparam == tasks {
            unsafe { KillTimer(0, tasks) };
            TASKS_TIMER.with(|c| c.set(0));
            measure_cell();
            let (tries, end) = TASKS_TRIES.with(Cell::get);
            if tries > 1 && CELL.with(Cell::get).2 == end {
                schedule_tasks_measure(100, tries - 1); // no change yet: look again
            }
        } else if measure != 0 && msg.wparam == measure {
            unsafe { KillTimer(0, measure) };
            MEASURE_TIMER.with(|c| c.set(0));
            let start = PANELS.with(Cell::get)[Kind::Start as usize].hwnd;
            if start != 0 {
                measure_start_menu(start);
                place_panel(Kind::Start, start, false);
            }
        } else {
            refresh();
        }
    }

    tray_icon(NIM_DELETE, None);
    shut_down();
    unsafe { CloseHandle(mutex) };
    match ENDING.with(Cell::get) {
        Ending::Restart => {
            if let Ok(exe) = std::env::current_exe() {
                let _ = Command::new(exe).arg("--restart").spawn();
            }
        }
        Ending::Uninstall => finish_uninstall(true),
        Ending::Quit => {}
    }
}
