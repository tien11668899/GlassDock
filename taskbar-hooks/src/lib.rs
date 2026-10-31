//! taskbar-hooks: loaded into explorer.exe by GlassDock through a WH_GETMESSAGE hook on the
//! taskbar thread. Two changes towards the Windows 11 taskbar:
//!
//! - Evenly spaced buttons. A button is `content margins (left + right) + icon + two theme
//!   metrics` wide, followed by a button spacing, and Explorer reloads both from the theme in
//!   CTaskListWnd::_HandleThemeChanged. That function is wrapped: after it has loaded the theme,
//!   the left/right content margins are trimmed and the spacing set (overlapping the buttons where
//!   Explorer will not make them narrower) so the room between two icons equals the room above
//!   and below them. Icon-only buttons draw their icon centred, so the icons stay centred.
//! - The Windows 11 Start logo. CPearl::_GetColorizedBitmap hands the Start button the glyph to
//!   alpha-blend (or null, then the theme's glyph is drawn). It is replaced: it always returns a
//!   bitmap of the four-pane Windows 11 logo in white, and the glyph box Explorer centres it in
//!   is set to the size of the app icons (24 px at 96 dpi), so in a Start cell as wide as a task
//!   button the logo lands exactly where the app icons sit in theirs. The logo itself is as big
//!   as the icons' artwork (26 px at 125 %), centred in that box. Background, hover and pressed
//!   highlights are still drawn by Explorer.
//!
//! The patches are only applied to the exact explorer.exe build they were written against
//! (Windows 10 22H2, build 19045.2965): PE timestamp, image size and the first bytes of every
//! function involved must match, otherwise nothing is touched. When the DLL is unloaded
//! (GlassDock exits), the original bytes go back, the taskbar reloads its theme margins and
//! the Start button repaints with its own glyph.

use std::ffi::c_void;
use std::ptr::{copy_nonoverlapping, null, null_mut};
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicIsize, AtomicU32, AtomicUsize, Ordering};

#[repr(C)]
#[derive(Default)]
struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

#[repr(C)]
struct BitmapInfo {
    size: u32,
    width: i32,
    height: i32,
    planes: u16,
    bit_count: u16,
    compression: u32,
    size_image: u32,
    x_pels_per_meter: i32,
    y_pels_per_meter: i32,
    clr_used: u32,
    clr_important: u32,
    colors: [u32; 1],
}

#[link(name = "user32")]
extern "system" {
    fn CallNextHookEx(hook: isize, code: i32, wparam: usize, lparam: isize) -> isize;
    fn GetClientRect(hwnd: isize, rect: *mut Rect) -> i32;
    fn FindWindowW(class: *const u16, title: *const u16) -> isize;
    fn FindWindowExW(parent: isize, after: isize, class: *const u16, title: *const u16) -> isize;
    fn PostMessageW(hwnd: isize, msg: u32, wparam: usize, lparam: isize) -> i32;
    fn InvalidateRect(hwnd: isize, rect: *const Rect, erase: i32) -> i32;
    fn IsWindow(hwnd: isize) -> i32;
    fn GetDpiForWindow(hwnd: isize) -> u32;
    fn GetWindowThreadProcessId(hwnd: isize, pid: *mut u32) -> u32;
    fn PostThreadMessageW(thread: u32, msg: u32, wparam: usize, lparam: isize) -> i32;
    fn IsWindowVisible(hwnd: isize) -> i32;
    fn MapWindowPoints(from: isize, to: isize, points: *mut i32, count: u32) -> i32;
}

#[link(name = "gdi32")]
extern "system" {
    fn CreateDIBSection(hdc: isize, info: *const BitmapInfo, usage: u32, bits: *mut *mut c_void, section: isize, offset: u32) -> isize;
    fn DeleteObject(object: isize) -> i32;
}

#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleW(name: *const u16) -> isize;
    fn GetModuleHandleExW(flags: u32, name: *const u16, module: *mut isize) -> i32;
    fn GetModuleFileNameW(module: isize, buffer: *mut u16, size: u32) -> u32;
    fn VirtualAlloc(address: *mut c_void, size: usize, kind: u32, protect: u32) -> *mut c_void;
    fn VirtualProtect(address: *mut c_void, size: usize, protect: u32, old: *mut u32) -> i32;
    fn FlushInstructionCache(process: isize, address: *const c_void, size: usize) -> i32;
    fn GetCurrentProcess() -> isize;
    fn GetLastError() -> u32;
}

const MEM_COMMIT_RESERVE: u32 = 0x3000;
const PAGE_EXECUTE_READWRITE: u32 = 0x40;
const DLL_PROCESS_DETACH: u32 = 0;
/// Makes the task band reload its theme and lay its buttons out again (WM_USER + 82, wParam 3).
const TASKBAND_THEME_RELOAD: u32 = 0x452;

// explorer.exe of Windows 10 19045.2965, measured with tools\disasm and its public PDB.
const IMAGE_TIMESTAMP: u32 = 0x71fd_c1b4;
const IMAGE_SIZE: u32 = 0x004f_d000;

/// A function whose entry is replaced by `mov rax, replacement; jmp rax` (12 bytes, padded with
/// NOPs to `stolen`). `prologue` must match before anything is patched; its first `stolen`
/// bytes are whole instructions, none RIP-relative, so they run unchanged in the trampoline.
struct Patch {
    rva: usize,
    prologue: &'static [u8],
    stolen: usize,
    target: AtomicUsize,
    trampoline: AtomicUsize,
}

/// ?_HandleThemeChanged@CTaskListWnd@@IEAAXXZ ??loads the button margins from the theme.
static THEME_CHANGED: Patch = Patch {
    rva: 0x57e6c,
    prologue: &[
        0x48, 0x89, 0x5C, 0x24, 0x08, // mov [rsp+8],rbx
        0x48, 0x89, 0x6C, 0x24, 0x10, // mov [rsp+10h],rbp
        0x48, 0x89, 0x74, 0x24, 0x18, // mov [rsp+18h],rsi
        0x57, // push rdi
        0x48, 0x83, 0xEC, 0x40, // sub rsp,40h
        0x48, 0x8B, 0xF1, // mov rsi,rcx
    ],
    stolen: 15,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_GetColorizedBitmap@CPearl@@AEAAPEAUHBITMAP__@@_N@Z ??the Start glyph for the current state.
static START_GLYPH: Patch = Patch {
    rva: 0x4e118,
    prologue: &[
        0x48, 0x89, 0x5C, 0x24, 0x08, // mov [rsp+8],rbx
        0x57, // push rdi
        0x48, 0x83, 0xEC, 0x20, // sub rsp,20h
        0x48, 0x8B, 0xD9, // mov rbx,rcx
        0x40, 0x8A, 0xFA, // mov dil,dl
    ],
    stolen: 13,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_DrawBasePlate@CTaskBtnGroup@@AEAAXPEAUHDC__@@AEBUBUTTONRENDERINFO@@AEBUBUTTONRENDERINFOSTATES@@@Z
static DRAW_BASE_PLATE: Patch = Patch {
    rva: 0x559b4,
    prologue: &[
        0x40, 0x55, 0x53, 0x56, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, // push rbp,rbx,rsi,r12..r15
        0x48, 0x8B, 0xEC, // mov rbp,rsp
    ],
    stolen: 15,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_DrawIndicator@CTaskBtnGroup@@AEAAXPEAUHDC__@@AEBUBUTTONRENDERINFO@@AEBUBUTTONRENDERINFOSTATES@@@Z
static DRAW_INDICATOR: Patch = Patch {
    rva: 0x552c0,
    prologue: &[
        0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, // push rbp,rdi,r12,r14,r15
        0x48, 0x8D, 0xAC, 0x24, 0xF0, 0xFE, 0xFF, 0xFF, // lea rbp,[rsp-110h]
    ],
    stolen: 17,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_DrawBar@CTaskBtnGroup@@AEAAXPEAUHDC__@@AEBUBUTTONRENDERINFO@@AEBUBUTTONRENDERINFOSTATES@@@Z
static DRAW_BAR: Patch = Patch {
    rva: 0x54b10,
    prologue: &[
        0x4C, 0x8B, 0xDC, // mov r11,rsp
        0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, // push rbp,rsi,rdi,r12,r14
        0x49, 0x8D, 0xAB, 0x18, 0xFF, 0xFF, 0xFF, // lea rbp,[r11-0E8h]
    ],
    stolen: 17,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?FillRectARGB@@YAXPEAUHDC__@@PEBUtagRECT@@EK@Z ??sets a rectangle to a premultiplied colour.
static FILL_RECT: Patch = Patch {
    rva: 0x4fdac,
    prologue: &[
        0x40, 0x55, 0x53, 0x57, // push rbp,rbx,rdi
        0x48, 0x8D, 0x6C, 0x24, 0xB9, // lea rbp,[rsp-47h]
        0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, // sub rsp,0C0h
    ],
    stolen: 16,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_OnPaintInternal@CPearl@@AEAAXPEAUHWND__@@PEAUHDC__@@@Z ??paints the Start button.
static PAINT_START: Patch = Patch {
    rva: 0x4de70,
    prologue: &[
        0x48, 0x89, 0x5C, 0x24, 0x20, // mov [rsp+20h],rbx
        0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, // push rbp,rsi,rdi,r12..r15
    ],
    stolen: 16,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_TaskCreated@CTaskListWnd@@IEAAJPEAUITaskGroup@@PEAUITaskItem@@H@Z ??adds a task (button).
static TASK_CREATED: Patch = Patch {
    rva: 0x49640,
    prologue: &[
        0x48, 0x89, 0x5C, 0x24, 0x20, // mov [rsp+20h],rbx
        0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, // push rbp,rsi,rdi,r12..r15
    ],
    stolen: 16,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_TaskDestroyedFinal@CTaskListWnd@@IEAAXPEAUITaskBtnGroup@@H@Z ??removes a button for good
/// (after its icon has faded out).
static TASK_GONE: Patch = Patch {
    rva: 0x48288,
    prologue: &[
        0x48, 0x89, 0x5C, 0x24, 0x18, // mov [rsp+18h],rbx
        0x48, 0x89, 0x6C, 0x24, 0x20, // mov [rsp+20h],rbp
        0x56, 0x57, 0x41, 0x56, // push rsi,rdi,r14
    ],
    stolen: 14,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?BlendRectARGB@@YAXPEAUHDC__@@PEBUtagRECT@@EKPEAUHBITMAP__@@@Z ??fills a rectangle with a colour.
static BLEND_RECT: Patch = Patch {
    rva: 0x12184,
    prologue: &[0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57], // push rbp,rbx,rsi,rdi,r12..r15
    stolen: 13,
    target: AtomicUsize::new(0),
    trampoline: AtomicUsize::new(0),
};

/// ?_ComputeSingleButtonWidth@CTaskListWnd@@IEAAHW4eTBGROUPTYPE@@PEAH@Z ??only called, not patched.
const BUTTON_WIDTH_RVA: usize = 0x71fc;
const BUTTON_WIDTH_PROLOGUE: [u8; 12] = [0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57];
/// eTBGROUPTYPE of the combined, icon-only buttons.
const GROUP_COMBINED: i32 = 2;

// Fields, with offsets read from the disassembly of the functions above.
const TASKLIST_HWND: usize = 0x08;
const TASKLIST_CONTENT_MARGIN_LEFT: usize = 0xB4;
const TASKLIST_CONTENT_MARGIN_RIGHT: usize = 0xB8;
/// Space Explorer leaves after every button across (ScaleByDPI(1) in _HandleThemeChanged); read
/// by _RecomputeLayoutRow (the step to the next button) and GetBtnGroupRectFromIndex (the cell).
const TASKLIST_BUTTON_SPACING_X: usize = 0x1A4;
const PEARL_HWND: usize = 0x18;
const PEARL_GLYPH_WIDTH: usize = 0x68;
const PEARL_GLYPH_HEIGHT: usize = 0x6C;
/// Size of the taskbar's app icons at 96 dpi; the logo box gets the same size.
const ICON_96DPI: i32 = 24;

/// 0 = not tried yet, 1 = active, -1 = not this explorer.exe build (nothing patched).
/// Exported so its state can be read from outside with ReadProcessMemory.
#[no_mangle]
pub static TASKBAR_HOOKS_STATE: AtomicI32 = AtomicI32::new(0);

static TRIED: AtomicBool = AtomicBool::new(false);
static BUTTON_WIDTH: AtomicUsize = AtomicUsize::new(0);
/// The Windows 11 logo bitmap and its size (width << 16 | height).
static LOGO: AtomicUsize = AtomicUsize::new(0);
static LOGO_SIZE: AtomicU32 = AtomicU32::new(0);
/// The Start button (CPearl), the theme's glyph box and the enlarged box written over it.
static PEARL: AtomicUsize = AtomicUsize::new(0);
static GLYPH_THEME: AtomicU32 = AtomicU32::new(0);
static GLYPH_SET: AtomicU32 = AtomicU32::new(0);

type HandleThemeChanged = unsafe extern "system" fn(*mut c_void);
type ComputeSingleButtonWidth = unsafe extern "system" fn(*mut c_void, i32, *mut i32) -> i32;
type GetColorizedBitmap = unsafe extern "system" fn(*mut c_void, u8) -> isize;

/// Runs Explorer's own theme loading, then lays icon-only buttons out so two icons are as far
/// apart as the icons are from the top and bottom of the (horizontal, single-row) taskbar: 30 px
/// icons 10 px apart at 125 %, a step of 40 px from one to the next.
/// - The left/right content margins are trimmed so the buttons get narrower, but only down to
///   36 px at 96 dpi (45 at 125 %): narrower ones Explorer stretches across the whole task list
///   (measured on this build).
/// - Explorer puts its button spacing (1 px) after every button; it is set to what is left of the
///   step, -5 px at 125 %, so neighbouring buttons overlap by the room between their icons. The
///   cells Explorer hit-tests are a button widened by the spacing, so they still tile the row
///   without overlapping: 40 px each, an icon in the middle of each.
unsafe extern "system" fn handle_theme_changed(this: *mut c_void) {
    let original: HandleThemeChanged = std::mem::transmute(THEME_CHANGED.trampoline.load(Ordering::Acquire));
    original(this);

    let fields = this as *mut u8;
    let list = *fields.add(TASKLIST_HWND).cast::<isize>();
    let mut rc = Rect::default();
    if GetClientRect(list, &mut rc) == 0 {
        return;
    }
    let height = rc.bottom - rc.top;
    if rc.right - rc.left <= height || height <= 0 {
        return; // vertical taskbar
    }
    let dpi = GetDpiForWindow(list).max(96) as i32;
    let icon = logo_box(dpi as u32);
    let step = icon + (height - icon) / 2; // 40 px at 125 %
    let narrowest = (36 * dpi + 48) / 96;
    let width = (step - 1).max(narrowest);
    let compute: ComputeSingleButtonWidth = std::mem::transmute(BUTTON_WIDTH.load(Ordering::Acquire));
    let mut label = 0i32;
    let natural = compute(this, GROUP_COMBINED, &mut label);
    let excess = natural - width;
    if excess >= height {
        return; // more than one row
    }
    if excess > 0 {
        let left = fields.add(TASKLIST_CONTENT_MARGIN_LEFT).cast::<i32>();
        let right = fields.add(TASKLIST_CONTENT_MARGIN_RIGHT).cast::<i32>();
        *left = (*left - excess / 2).max(1);
        *right = (*right - (excess - excess / 2)).max(1);
    }
    let spacing = fields.add(TASKLIST_BUTTON_SPACING_X).cast::<i32>();
    let wanted = step - compute(this, GROUP_COMBINED, &mut label);
    if wanted < *spacing && wanted > -width / 2 {
        *spacing = wanted;
    }
}

fn pack(width: i32, height: i32) -> u32 {
    ((width as u32) << 16) | (height as u32 & 0xFFFF)
}

/// Gap between the panes: about 1/30 of the logo like the Windows 11 mark, at least a pixel,
/// and one more if that is what it takes for two equal whole-pixel panes to fill the side
/// exactly (an even side needs an even gap).
fn logo_gap(side: i32) -> i32 {
    let gap = ((side + 15) / 30).max(1);
    gap + (side - gap) % 2
}

/// Glyph box: the size of the app icons at the Start button's DPI.
fn logo_box(dpi: u32) -> i32 {
    (ICON_96DPI * dpi.max(96) as i32 + 48) / 96
}

/// The Start glyph in every state: the Windows 11 logo in the enlarged glyph box.
unsafe extern "system" fn start_glyph(this: *mut c_void, hot: u8) -> isize {
    let fields = this as *mut u8;
    let width = fields.add(PEARL_GLYPH_WIDTH).cast::<i32>();
    let height = fields.add(PEARL_GLYPH_HEIGHT).cast::<i32>();
    // Explorer centres and alpha-blends the glyph by these two fields, which it sets from the
    // theme. While they hold the theme's size (first paint, theme or DPI change), set them to
    // the app icon size and paint again: this paint has already placed its box.
    if *width > 0 && *height > 0 && pack(*width, *height) != GLYPH_SET.load(Ordering::Acquire) {
        let hwnd = *fields.add(PEARL_HWND).cast::<isize>();
        let side = logo_box(GetDpiForWindow(hwnd));
        GLYPH_THEME.store(pack(*width, *height), Ordering::Release);
        PEARL.store(this as usize, Ordering::Release);
        *width = side;
        *height = side;
        GLYPH_SET.store(pack(side, side), Ordering::Release);
        InvalidateRect(hwnd, null(), 1);
    }
    let logo = windows11_logo(*width, *height);
    if logo != 0 {
        return logo;
    }
    let original: GetColorizedBitmap = std::mem::transmute(START_GLYPH.trampoline.load(Ordering::Acquire));
    original(this, hot)
}

/// Gives the Start button its theme glyph box back, so Explorer's own glyph bitmaps (made at
/// that size) fit again. Only while the button is still alive and still has our box.
unsafe fn restore_glyph_box() {
    let pearl = PEARL.swap(0, Ordering::AcqRel);
    let theme = GLYPH_THEME.load(Ordering::Acquire);
    if pearl == 0 || theme == 0 {
        return;
    }
    let fields = pearl as *mut u8;
    let width = fields.add(PEARL_GLYPH_WIDTH).cast::<i32>();
    let height = fields.add(PEARL_GLYPH_HEIGHT).cast::<i32>();
    if IsWindow(*fields.add(PEARL_HWND).cast::<isize>()) != 0 && pack(*width, *height) == GLYPH_SET.load(Ordering::Acquire) {
        *width = (theme >> 16) as i32;
        *height = (theme & 0xFFFF) as i32;
    }
}

/// The four-pane Windows 11 logo in pure white as a premultiplied 32-bit bitmap, made once per
/// size; transparent around and between the panes. Explorer alpha-blends it from (0, 0) at
/// width x height.
unsafe fn windows11_logo(width: i32, height: i32) -> isize {
    if !(4..=256).contains(&width) || !(4..=256).contains(&height) {
        return 0;
    }
    let size = ((width as u32) << 16) | height as u32;
    let cached = LOGO.load(Ordering::Acquire);
    if cached != 0 && LOGO_SIZE.load(Ordering::Acquire) == size {
        return cached as isize;
    }

    let info = BitmapInfo {
        size: 40,
        width,
        height: -height, // top-down rows
        planes: 1,
        bit_count: 32,
        compression: 0, // BI_RGB
        size_image: 0,
        x_pels_per_meter: 0,
        y_pels_per_meter: 0,
        clr_used: 0,
        clr_important: 0,
        colors: [0],
    };
    let mut bits = null_mut();
    let bitmap = CreateDIBSection(0, &info, 0, &mut bits, 0, 0);
    if bitmap == 0 || bits.is_null() {
        return 0;
    }
    let pixels = std::slice::from_raw_parts_mut(bits.cast::<u32>(), (width * height) as usize);

    // Square logo centred in the glyph box: two panes and a gap per side. The box is an app
    // icon's; the logo is as big as an app icon's artwork, which leaves some room in its box
    // (Ae's is about 26 px in its 30), so 13/15 of the box, as many pixels shorter as keeps it
    // exactly centred.
    let box_side = width.min(height);
    let exact = box_side as f32 * 13.0 / 15.0;
    let mut side = exact.round() as i32;
    if (box_side - side) % 2 != 0 {
        side += if exact > side as f32 { 1 } else { -1 };
    }
    let gap = logo_gap(side);
    let pane = (side - gap) / 2;
    let logo = pane * 2 + gap;
    let (x0, y0) = ((width - logo) / 2, (height - logo) / 2);
    let inside = |v: i32| (0..pane).contains(&v) || (pane + gap..logo).contains(&v);
    for y in 0..height {
        for x in 0..width {
            pixels[(y * width + x) as usize] = if inside(x - x0) && inside(y - y0) { 0xFFFF_FFFF } else { 0 };
        }
    }

    let old = LOGO.swap(bitmap as usize, Ordering::AcqRel);
    LOGO_SIZE.store(size, Ordering::Release);
    if old != 0 {
        DeleteObject(old as isize); // only drawn during a paint, never kept by Explorer
    }
    bitmap
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

// --- Button drawing (diagnostic): what each of Explorer's drawing steps paints ---------------

/// Lines still allowed in the diagnostic log (it is written on every repaint).
static LOG_BUDGET: AtomicI32 = AtomicI32::new(4000);

fn log(line: &str) {
    use std::io::Write;
    if LOG_BUDGET.fetch_sub(1, Ordering::Relaxed) <= 0 {
        return;
    }
    let path = std::env::temp_dir().join("taskbar-hooks.log");
    if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(file, "{line}");
    }
}

/// Points an import address table slot at `replacement`, keeping what it held.
unsafe fn patch_slot(slot: usize, replacement: usize, original: &AtomicUsize, saved: &AtomicUsize) -> bool {
    let mut old = 0u32;
    if VirtualProtect(slot as *mut c_void, 8, 0x04 /* PAGE_READWRITE */, &mut old) == 0 {
        return false;
    }
    original.store(*(slot as *const usize), Ordering::Release);
    *(slot as *mut usize) = replacement;
    VirtualProtect(slot as *mut c_void, 8, old, &mut old);
    saved.store(slot, Ordering::Release);
    true
}

unsafe fn unpatch_slot(saved: &AtomicUsize, original: &AtomicUsize) {
    let slot = saved.swap(0, Ordering::AcqRel);
    if slot == 0 {
        return;
    }
    let mut old = 0u32;
    if VirtualProtect(slot as *mut c_void, 8, 0x04, &mut old) != 0 {
        *(slot as *mut usize) = original.load(Ordering::Acquire);
        VirtualProtect(slot as *mut c_void, 8, old, &mut old);
    }
}

// --- Rounded highlights and short running indicators --------------------------------------
//
// Explorer paints a task button in steps (CTaskBtnGroup::_DrawRegularButton): the base plate
// (the hover highlight: white at 10 %, faded in and out by an animated alpha) through
// BlendRectARGB, the indicator plate (active, pressed, flashing) through GdiAlphaBlend of a
// 1 x 1 pixel, and the bar under a running app (3 px of the accent colour that widens from 40 to
// 50 px when hovered) the same way; the Start button fills its own highlight with FillRectARGB.
// Each step is wrapped to know which one is running, and in it those fills draw this instead:
// - the hover highlights (base plate, Start): handed to the dock, which draws them as glass
//   behind the icons; drawn here like the other plates only when no dock is on screen;
// - the plates: a square as wide as the button, inset all round, with rounded corners
//   concentric with the dock behind the icons;
// - the bar: one short pill of fixed width under the icon, whatever the state.
// Colours and alphas stay Explorer's, so its fades stay; nothing else animates.

type AlphaBlend = unsafe extern "system" fn(isize, i32, i32, i32, i32, isize, i32, i32, i32, i32, u32) -> i32;
type DrawStep = unsafe extern "system" fn(*mut c_void, isize, *const c_void, *const c_void);
type BlendRectArgb = unsafe extern "system" fn(isize, *const Rect, u8, u32, isize);
type FillRectArgb = unsafe extern "system" fn(isize, *const Rect, u8, u32);
type PaintStart = unsafe extern "system" fn(*mut c_void, isize, isize);

const STEP_PLATE: u32 = 1;
const STEP_INDICATOR: u32 = 2;
const STEP_BAR: u32 = 3;
const STEP_START: u32 = 4;
/// Which painting step is running on the taskbar thread (0: none of ours).
static STEP: AtomicU32 = AtomicU32::new(0);
/// Width of a task button as last painted (50 px at 125 %).
static BUTTON_SIZE: AtomicI32 = AtomicI32::new(0);
static EXPLORER_BLEND_SLOT: AtomicUsize = AtomicUsize::new(0);
static EXPLORER_BLEND_ORIGINAL: AtomicUsize = AtomicUsize::new(0);

#[link(name = "gdi32")]
extern "system" {
    fn GetCurrentObject(hdc: isize, kind: u32) -> isize;
    fn GetObjectW(object: isize, size: i32, buffer: *mut c_void) -> i32;
    fn GdiFlush() -> i32;
    fn CreateCompatibleDC(hdc: isize) -> isize;
    fn SelectObject(hdc: isize, object: isize) -> isize;
    fn DeleteDC(hdc: isize) -> i32;
}

/// Side of the scratch bitmap the shapes are drawn in before they are blended into Explorer's.
const SCRATCH: i32 = 128;
/// The scratch bitmap (top-down, 32-bit premultiplied), the memory DC it is selected into, and
/// its pixels; made on first use on the taskbar thread.
static SCRATCH_DC: AtomicUsize = AtomicUsize::new(0);
static SCRATCH_BITMAP: AtomicUsize = AtomicUsize::new(0);
static SCRATCH_BITS: AtomicUsize = AtomicUsize::new(0);

unsafe fn scratch() -> Option<(isize, *mut u32)> {
    let dc = SCRATCH_DC.load(Ordering::Acquire);
    if dc != 0 {
        return Some((dc as isize, SCRATCH_BITS.load(Ordering::Acquire) as *mut u32));
    }
    let info = BitmapInfo {
        size: 40,
        width: SCRATCH,
        height: -SCRATCH, // top-down rows
        planes: 1,
        bit_count: 32,
        compression: 0,
        size_image: 0,
        x_pels_per_meter: 0,
        y_pels_per_meter: 0,
        clr_used: 0,
        clr_important: 0,
        colors: [0],
    };
    let mut bits = null_mut();
    let bitmap = CreateDIBSection(0, &info, 0, &mut bits, 0, 0);
    let dc = CreateCompatibleDC(0);
    if bitmap == 0 || bits.is_null() || dc == 0 {
        return None;
    }
    SelectObject(dc, bitmap);
    SCRATCH_BITMAP.store(bitmap as usize, Ordering::Release);
    SCRATCH_BITS.store(bits as usize, Ordering::Release);
    SCRATCH_DC.store(dc as usize, Ordering::Release);
    Some((dc, bits.cast()))
}

unsafe fn free_scratch() {
    let dc = SCRATCH_DC.swap(0, Ordering::AcqRel);
    if dc != 0 {
        DeleteDC(dc as isize);
    }
    let bitmap = SCRATCH_BITMAP.swap(0, Ordering::AcqRel);
    if bitmap != 0 {
        DeleteObject(bitmap as isize);
    }
}

/// Shapes in a button `size` px square (proportions taken at 125 %, where it is 50 px).
fn plate_inset(size: i32) -> f32 {
    (size as f32 * 0.08).round() // 4 px: the highlight is 42 px around a 30 px icon
}

fn plate_radius(size: i32) -> f32 {
    (size as f32 * 0.12).round() // 6 px: the dock's 10 less the 4 px inset
}

/// Paints the rounded rectangle (left, top)..(right, bottom), in the DC's logical coordinates,
/// antialiased, in the premultiplied BGRA colour `pixel` at `alpha`, over what is there: the
/// shape is drawn into the scratch bitmap and alpha-blended in by GDI (which knows the DC's
/// origin, clipping and row order). False when it could not be drawn.
unsafe fn fill_rounded(hdc: isize, left: f32, top: f32, right: f32, bottom: f32, radius: f32, pixel: u32, alpha: u32) -> bool {
    if right <= left || bottom <= top || alpha == 0 || pixel >> 24 == 0 {
        return true;
    }
    let (x0, y0) = (left.floor() as i32, top.floor() as i32);
    let (w, h) = (right.ceil() as i32 - x0, bottom.ceil() as i32 - y0);
    let blend = EXPLORER_BLEND_ORIGINAL.load(Ordering::Acquire);
    if w > SCRATCH || h > SCRATCH || blend == 0 {
        return false;
    }
    let Some((dc, bits)) = scratch() else { return false };
    GdiFlush();
    let r: f32 = radius.min((right - left) / 2.0).min((bottom - top) / 2.0).max(0.0);
    let (cx, cy) = ((left + right) / 2.0 - x0 as f32, (top + bottom) / 2.0 - y0 as f32);
    let (hx, hy) = ((right - left) / 2.0 - r, (bottom - top) / 2.0 - r);
    let channel = |shift: u32| ((pixel >> shift) & 0xFF) as f32;
    let (b, g, rd, a) = (channel(0), channel(8), channel(16), channel(24));
    for y in 0..h {
        for x in 0..w {
            // Signed distance from the pixel centre to the rounded rectangle.
            let qx = (x as f32 + 0.5 - cx).abs() - hx;
            let qy = (y as f32 + 0.5 - cy).abs() - hy;
            let outside = (qx.max(0.0).powi(2) + qy.max(0.0).powi(2)).sqrt();
            let distance = outside + qx.max(qy).min(0.0) - r;
            let coverage = (0.5 - distance).clamp(0.0, 1.0);
            let value = |c: f32| (c * coverage).round() as u32;
            *bits.add((y * SCRATCH + x) as usize) = value(a) << 24 | value(rd) << 16 | value(g) << 8 | value(b);
        }
    }
    let original: AlphaBlend = std::mem::transmute(blend);
    // AC_SRC_OVER, constant alpha, AC_SRC_ALPHA (per-pixel, premultiplied)
    original(hdc, x0, y0, w, h, dc, 0, 0, w, h, 0x0100_0000 | (alpha.min(255) << 16));
    true
}

/// The highlight for a plate whose top-left corner is (left, top) in a button `size` px wide.
unsafe fn fill_plate(hdc: isize, left: i32, top: i32, size: i32, pixel: u32, alpha: u32) -> bool {
    let inset = plate_inset(size);
    let (l, t) = (left as f32 + inset, top as f32 + inset);
    fill_rounded(hdc, l, t, left as f32 + size as f32 - inset, top as f32 + size as f32 - inset, plate_radius(size), pixel, alpha)
}

/// The colour `color` (0xAARRGGBB) at `alpha`, premultiplied, as Explorer's ARGB helpers make it.
fn premultiplied(color: u32, alpha: u32) -> u32 {
    let scale = |v: u32| v * alpha / 255;
    (alpha << 24) | (scale((color >> 16) & 0xFF) << 16) | (scale((color >> 8) & 0xFF) << 8) | scale(color & 0xFF)
}

/// The pixel of the 1 x 1 bitmap selected into `hdc` (Explorer's colour sources), premultiplied BGRA.
unsafe fn source_pixel(hdc: isize) -> Option<u32> {
    let bitmap = GetCurrentObject(hdc, 7); // OBJ_BITMAP
    let mut ds = [0u8; 104]; // DIBSECTION: bmBits at 24, bmBitsPixel at 18
    if bitmap == 0 || GetObjectW(bitmap, ds.len() as i32, ds.as_mut_ptr().cast()) != ds.len() as i32 {
        return None;
    }
    let bits = usize::from_le_bytes(ds[24..32].try_into().ok()?) as *const u32;
    if bits.is_null() || u16::from_le_bytes([ds[18], ds[19]]) != 32 {
        return None;
    }
    GdiFlush();
    Some(*bits)
}

// --- Highlights drawn by the dock --------------------------------------------------------------
//
// While the dock (taskbar-dock.dll in GlassDock) is on screen, two highlights are not painted
// here but handed to it, which draws them behind the icons and moves them from button to button
// the way S1mp1e's hotbar does: the hover highlight (a task button's base plate, the Start
// button's highlight), still a white rounded square, and the active app's plate (the white
// indicator plates: active, hovered, pressed, at whatever size Explorer animates them to), as
// the glass of the hotbar's selection. A flashing, coloured indicator stays Explorer's. Each
// paint tells the dock once which button it is, where it is on the screen and how strong its
// plate is; a button painted without that plate has none any more (_DrawRegularButton always
// calls _DrawBasePlate and _DrawIndicator).

/// WM_APP + 0x11 / 0x12 to the dock window: a hover highlight / the active app's plate.
/// wParam: the button's rectangle on the screen, four i16 from the low end (left, top, right,
/// bottom); lParam: which button (low 32 bits), how far its plate has faded in, 0..255 (bits
/// 32..40; 0: gone), and bit 40 set for the Start button.
const WM_HOVER_GLASS: u32 = 0x8011;
const WM_SELECT_GLASS: u32 = 0x8012;
const START_BUTTON: usize = 1 << 40;
/// The active app's indicator plate: white at this alpha (premultiplied 0x19191919; twice that
/// while it is hovered or pressed).
const ACTIVE_WHITE: u32 = 0x19;

static DOCK_WINDOW: AtomicIsize = AtomicIsize::new(0);
static TASK_LIST: AtomicIsize = AtomicIsize::new(0);
/// The object whose highlight step is running (a task button group or the Start button), the
/// window it paints in, and whether that step handed its highlight to the dock.
static PLATE_OWNER: AtomicUsize = AtomicUsize::new(0);
static PLATE_WINDOW: AtomicIsize = AtomicIsize::new(0);
static PLATE_SENT: AtomicBool = AtomicBool::new(false);
/// The indicator plates the running step drew (Explorer draws one per state it is fading
/// between: active, hovered, pressed): whether any, the strongest one's strength, and the union of
/// their rectangles, which the dock hears about once, at the end of the step.
static PLATE_ANY: AtomicBool = AtomicBool::new(false);
static PLATE_BEST: AtomicU32 = AtomicU32::new(0);
static PLATE_UNION: [AtomicI32; 4] = [AtomicI32::new(0), AtomicI32::new(0), AtomicI32::new(0), AtomicI32::new(0)];
/// The height of a whole indicator plate (47 px at 125 %): the tallest one drawn so far.
static FULL_PLATE_HEIGHT: AtomicI32 = AtomicI32::new(0);
/// The buttons the dock shows a hover highlight / an active plate for, so it hears once when
/// each is gone.
const NO_BUTTON: AtomicUsize = AtomicUsize::new(0);
static HOVERED: [AtomicUsize; 8] = [NO_BUTTON; 8];
static SELECTED: [AtomicUsize; 8] = [NO_BUTTON; 8];

fn lit(message: u32) -> &'static [AtomicUsize; 8] {
    if message == WM_SELECT_GLASS { &SELECTED } else { &HOVERED }
}

/// The dock window, if GlassDock runs one.
unsafe fn dock_window() -> isize {
    let mut dock = DOCK_WINDOW.load(Ordering::Relaxed);
    if dock == 0 || IsWindow(dock) == 0 {
        dock = FindWindowW(wide("TaskbarDock").as_ptr(), null());
        DOCK_WINDOW.store(dock, Ordering::Relaxed);
    }
    dock
}

/// The window the task buttons are painted in: their rectangles are in its client coordinates.
unsafe fn task_list() -> isize {
    let mut list = TASK_LIST.load(Ordering::Relaxed);
    if list == 0 || IsWindow(list) == 0 {
        let tray = FindWindowW(wide("Shell_TrayWnd").as_ptr(), null());
        let rebar = FindWindowExW(tray, 0, wide("ReBarWindow32").as_ptr(), null());
        let band = FindWindowExW(rebar, 0, wide("MSTaskSwWClass").as_ptr(), null());
        list = FindWindowExW(band, 0, wide("MSTaskListWClass").as_ptr(), null());
        TASK_LIST.store(list, Ordering::Relaxed);
    }
    list
}

fn button_id(owner: usize) -> usize {
    (owner >> 4) & 0xFFFF_FFFF
}

/// Starts a highlight step for `owner` painting in `window`.
fn plate_begin(owner: usize, window: isize) {
    PLATE_OWNER.store(owner, Ordering::Relaxed);
    PLATE_WINDOW.store(window, Ordering::Relaxed);
    PLATE_SENT.store(false, Ordering::Relaxed);
    PLATE_ANY.store(false, Ordering::Relaxed);
    PLATE_BEST.store(0, Ordering::Relaxed);
}

/// Whether the dock is on screen to draw the running step's highlight.
unsafe fn dock_takes_highlights() -> bool {
    let dock = dock_window();
    dock != 0 && PLATE_WINDOW.load(Ordering::Relaxed) != 0 && IsWindowVisible(dock) != 0
}

/// Notes an indicator plate the running step drew for the dock: (x, y, w, h) in the task list,
/// `strength` 0..255.
fn note_plate(x: i32, y: i32, w: i32, h: i32, strength: u32) {
    let [left, top, right, bottom] = &PLATE_UNION;
    if PLATE_ANY.swap(true, Ordering::Relaxed) {
        left.fetch_min(x, Ordering::Relaxed);
        top.fetch_min(y, Ordering::Relaxed);
        right.fetch_max(x + w, Ordering::Relaxed);
        bottom.fetch_max(y + h, Ordering::Relaxed);
    } else {
        left.store(x, Ordering::Relaxed);
        top.store(y, Ordering::Relaxed);
        right.store(x + w, Ordering::Relaxed);
        bottom.store(y + h, Ordering::Relaxed);
    }
    PLATE_BEST.fetch_max(strength, Ordering::Relaxed);
}

/// Hands the dock the running step's highlight (`message`): `button` in the client coordinates
/// of the window it paints in, faded in to `fade` (0..255). False when no dock is on screen to
/// draw it.
unsafe fn glass_highlight(button: &Rect, fade: u32, message: u32) -> bool {
    let dock = dock_window();
    let window = PLATE_WINDOW.load(Ordering::Relaxed);
    if dock == 0 || window == 0 || IsWindowVisible(dock) == 0 {
        if dock != 0 && window == 0 {
            log("no glass: no task list window"); // the dock is there, but where is the button?
        }
        return false;
    }
    let mut corners = [button.left, button.top, button.right, button.bottom];
    MapWindowPoints(window, 0, corners.as_mut_ptr(), 2);
    let packed = corners.iter().enumerate().fold(0usize, |p, (i, &v)| p | (v as i16 as u16 as usize) << (16 * i));
    let id = button_id(PLATE_OWNER.load(Ordering::Relaxed));
    let start = if window == task_list() { 0 } else { START_BUTTON };
    if PostMessageW(dock, message, packed, (id | (fade.min(255) as usize) << 32 | start) as isize) == 0 {
        log(&format!("no glass: post failed {}", GetLastError()));
        return false;
    }
    let set = lit(message);
    let slot = set.iter().find(|g| g.load(Ordering::Relaxed) == id);
    if fade == 0 {
        if let Some(slot) = slot {
            slot.store(0, Ordering::Relaxed); // told already
        }
    } else if slot.is_none() {
        if let Some(free) = set.iter().find(|g| g.load(Ordering::Relaxed) == 0) {
            free.store(id, Ordering::Relaxed);
        }
    }
    PLATE_SENT.store(true, Ordering::Relaxed);
    true
}

/// Ends a highlight step: hands the dock the indicator plates it drew, as one; a button that drew
/// no such highlight this time has none any more.
unsafe fn plate_end(owner: usize, message: u32) {
    if message == WM_SELECT_GLASS && PLATE_ANY.load(Ordering::Relaxed) {
        let [left, top, right, bottom] = &PLATE_UNION;
        let plates = Rect {
            left: left.load(Ordering::Relaxed),
            top: top.load(Ordering::Relaxed),
            right: right.load(Ordering::Relaxed),
            bottom: bottom.load(Ordering::Relaxed),
        };
        glass_highlight(&plates, PLATE_BEST.load(Ordering::Relaxed), message);
    }
    if PLATE_SENT.load(Ordering::Relaxed) {
        return;
    }
    let id = button_id(owner);
    if let Some(slot) = lit(message).iter().find(|g| g.load(Ordering::Relaxed) == id) {
        slot.store(0, Ordering::Relaxed);
        let dock = dock_window();
        if dock != 0 {
            PostMessageW(dock, message, 0, id as isize);
        }
    }
}

/// A grey (white) plate, not a coloured one (an app asking for attention flashes in colour).
fn grey(pixel: u32) -> bool {
    let (b, g, r) = (pixel & 0xFF, (pixel >> 8) & 0xFF, (pixel >> 16) & 0xFF);
    r.abs_diff(g) <= 8 && g.abs_diff(b) <= 8
}

unsafe fn step(patch: &Patch, which: u32, this: *mut c_void, hdc: isize, info: *const c_void, states: *const c_void) {
    let previous = STEP.swap(which, Ordering::Relaxed);
    let original: DrawStep = std::mem::transmute(patch.trampoline.load(Ordering::Acquire));
    original(this, hdc, info, states);
    STEP.store(previous, Ordering::Relaxed);
}

unsafe extern "system" fn draw_base_plate(this: *mut c_void, hdc: isize, info: *const c_void, states: *const c_void) {
    plate_begin(this as usize, task_list());
    step(&DRAW_BASE_PLATE, STEP_PLATE, this, hdc, info, states);
    plate_end(this as usize, WM_HOVER_GLASS);
}

unsafe extern "system" fn draw_indicator(this: *mut c_void, hdc: isize, info: *const c_void, states: *const c_void) {
    plate_begin(this as usize, task_list());
    step(&DRAW_INDICATOR, STEP_INDICATOR, this, hdc, info, states);
    plate_end(this as usize, WM_SELECT_GLASS);
}

unsafe extern "system" fn draw_bar(this: *mut c_void, hdc: isize, info: *const c_void, states: *const c_void) {
    step(&DRAW_BAR, STEP_BAR, this, hdc, info, states);
}

unsafe extern "system" fn paint_start(this: *mut c_void, hwnd: isize, hdc: isize) {
    let previous = STEP.swap(STEP_START, Ordering::Relaxed);
    plate_begin(this as usize, hwnd);
    let original: PaintStart = std::mem::transmute(PAINT_START.trampoline.load(Ordering::Acquire));
    original(this, hwnd, hdc);
    STEP.store(previous, Ordering::Relaxed);
    plate_end(this as usize, WM_HOVER_GLASS);
}

/// BlendRectARGB: the hover plate goes to the dock, or else becomes the rounded highlight; the
/// white wash Explorer lays over the bar is left out (the pill keeps one look).
unsafe extern "system" fn blend_rect(hdc: isize, rect: *const Rect, alpha: u8, color: u32, bitmap: isize) {
    if !rect.is_null() {
        let r = &*rect;
        match STEP.load(Ordering::Relaxed) {
            STEP_PLATE if r.right - r.left >= 16 => {
                let size = r.right - r.left;
                BUTTON_SIZE.store(size, Ordering::Relaxed);
                if glass_highlight(r, alpha as u32, WM_HOVER_GLASS) {
                    return;
                }
                let a = (alpha as u32 * (color >> 24) / 255).min(255);
                if fill_plate(hdc, r.left, r.top, size, premultiplied(color, a), 255) {
                    return;
                }
            }
            STEP_BAR => return,
            _ => {}
        }
    }
    let original: BlendRectArgb = std::mem::transmute(BLEND_RECT.trampoline.load(Ordering::Acquire));
    original(hdc, rect, alpha, color, bitmap);
}

/// FillRectARGB: the Start button's highlight (only the button-sized fill; the same function
/// also makes the Start glyphs) goes to the dock, or else becomes the rounded highlight.
unsafe extern "system" fn fill_rect(hdc: isize, rect: *const Rect, alpha: u8, color: u32) {
    if !rect.is_null() && STEP.load(Ordering::Relaxed) == STEP_START {
        let r = &*rect;
        let size = (r.right - r.left).min(r.bottom - r.top);
        if size >= 32 {
            if alpha > 0 && glass_highlight(r, alpha as u32, WM_HOVER_GLASS) {
                return;
            }
            if fill_plate(hdc, r.left, r.top, size, premultiplied(color, alpha as u32), 255) {
                return;
            }
        }
    }
    let original: FillRectArgb = std::mem::transmute(FILL_RECT.trampoline.load(Ordering::Acquire));
    original(hdc, rect, alpha, color);
}

/// GdiAlphaBlend from explorer.exe: the indicator plate becomes the rounded highlight, the bar
/// a short pill centred where the bar was.
unsafe extern "system" fn explorer_alpha_blend(dest: isize, x: i32, y: i32, w: i32, h: i32, src: isize, sx: i32, sy: i32, sw: i32, sh: i32, blend: u32) -> i32 {
    let which = STEP.load(Ordering::Relaxed);
    if (which == STEP_INDICATOR || which == STEP_BAR) && sw == 1 && sh == 1 {
        if w <= 0 || h <= 0 {
            return 1;
        }
        if let Some(pixel) = source_pixel(src) {
            let alpha = (blend >> 16) & 0xFF;
            let size = match BUTTON_SIZE.load(Ordering::Relaxed) {
                0 => w.max(h),
                s => s,
            };
            let done = if which == STEP_INDICATOR && grey(pixel) && dock_takes_highlights() {
                // The active app's plate is white at 0x19 (0x33 hovered or pressed). It fades out
                // as another app takes over, grows up from the bottom as its app becomes active and
                // shrinks back down as it stops being: its strength for the dock is how far it has
                // faded in times how much of its height it has. Drawn by the dock, at any size.
                let full = FULL_PLATE_HEIGHT.fetch_max(h, Ordering::Relaxed).max(h);
                let colour = ((pixel >> 24) * alpha / 255 * 255 / ACTIVE_WHITE).min(255);
                note_plate(x, y, w, h, colour * h as u32 / full as u32);
                true
            } else if which == STEP_INDICATOR && w >= 16 && h >= 16 {
                fill_plate(dest, x, y, w, pixel, alpha)
            } else if which == STEP_BAR && h <= 8 {
                // Fixed width, its own height, centred under the icon inside the highlight.
                let width = (size as f32 * 0.16).round(); // 8 px
                let button_top = y + h - size;
                let top = button_top as f32 + (size as f32 * 0.84).round(); // 42 px
                let centre = x as f32 + w as f32 / 2.0;
                fill_rounded(dest, centre - width / 2.0, top, centre + width / 2.0, top + h as f32, h as f32 / 2.0, pixel, alpha)
            } else {
                false
            };
            if done {
                return 1;
            }
        }
    }
    let original: AlphaBlend = std::mem::transmute(EXPLORER_BLEND_ORIGINAL.load(Ordering::Acquire));
    original(dest, x, y, w, h, src, sx, sy, sw, sh, blend)
}

// --- Buttons coming and going: GlassDock resizes the dock at once ------------------------

type TaskCreated = unsafe extern "system" fn(*mut c_void, *mut c_void, *mut c_void, i32) -> i32;
type TaskDestroyedFinal = unsafe extern "system" fn(*mut c_void, *mut c_void, i32);
/// WM_APP + 0x10 to GlassDock's thread; wParam 2: a button was added, 1: one was removed.
const WM_TASKS_CHANGED: u32 = 0x8010;

/// Tells GlassDock (the thread that owns the dock window) that the buttons changed.
unsafe fn tell_glassdock(change: usize) {
    let dock = FindWindowW(wide("TaskbarDock").as_ptr(), null());
    if dock != 0 {
        let thread = GetWindowThreadProcessId(dock, null_mut());
        PostThreadMessageW(thread, WM_TASKS_CHANGED, change, 0);
    }
}

unsafe extern "system" fn task_created(this: *mut c_void, group: *mut c_void, item: *mut c_void, flags: i32) -> i32 {
    let original: TaskCreated = std::mem::transmute(TASK_CREATED.trampoline.load(Ordering::Acquire));
    let result = original(this, group, item, flags);
    tell_glassdock(2);
    result
}

unsafe extern "system" fn task_destroyed_final(this: *mut c_void, group: *mut c_void, flags: i32) {
    let original: TaskDestroyedFinal = std::mem::transmute(TASK_GONE.trampoline.load(Ordering::Acquire));
    original(this, group, flags);
    tell_glassdock(1);
}

unsafe fn install_step_hooks(base: usize) {
    let mut ok = true;
    for (patch, replacement) in [
        (&DRAW_BASE_PLATE, draw_base_plate as *const () as usize),
        (&DRAW_INDICATOR, draw_indicator as *const () as usize),
        (&DRAW_BAR, draw_bar as *const () as usize),
        (&BLEND_RECT, blend_rect as *const () as usize),
        (&FILL_RECT, fill_rect as *const () as usize),
        (&PAINT_START, paint_start as *const () as usize),
        (&TASK_CREATED, task_created as *const () as usize),
        (&TASK_GONE, task_destroyed_final as *const () as usize),
    ] {
        ok &= matches(base + patch.rva, patch.prologue) && apply(patch, base, replacement);
    }
    // The slot is resolved on first use (an API-set import): only patch it once it points into GDI.
    let slot = base + GDI_ALPHA_BLEND_SLOT_RVA;
    let blend = in_gdi(*(slot as *const usize))
        && patch_slot(slot, explorer_alpha_blend as *const () as usize, &EXPLORER_BLEND_ORIGINAL, &EXPLORER_BLEND_SLOT);
    log(&format!("step hooks: {ok}, GdiAlphaBlend slot: {blend}"));
}

/// explorer.exe's import slot for GdiAlphaBlend (__imp_GdiAlphaBlend in its PDB).
const GDI_ALPHA_BLEND_SLOT_RVA: usize = 0x39_e468;

/// Whether `address` lies in one of the GDI DLLs.
unsafe fn in_gdi(address: usize) -> bool {
    let mut module = 0isize;
    // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
    if address == 0 || GetModuleHandleExW(0x4 | 0x2, address as *const u16, &mut module) == 0 {
        return false;
    }
    let mut path = [0u16; 260];
    let len = GetModuleFileNameW(module, path.as_mut_ptr(), path.len() as u32) as usize;
    String::from_utf16_lossy(&path[..len.min(path.len())]).to_lowercase().contains("gdi")
}

unsafe fn uninstall_step_hooks() {
    unpatch_slot(&EXPLORER_BLEND_SLOT, &EXPLORER_BLEND_ORIGINAL);
    for patch in [&DRAW_BASE_PLATE, &DRAW_INDICATOR, &DRAW_BAR, &BLEND_RECT, &FILL_RECT, &PAINT_START, &TASK_CREATED, &TASK_GONE] {
        revert(patch);
    }
    free_scratch();
}

/// Has the task list reload its theme and the Start button repaint (through whatever code is
/// in place at the time: the replacements while installed, Explorer's own after uninstall).
unsafe fn refresh_taskbar() {
    let tray = FindWindowW(wide("Shell_TrayWnd").as_ptr(), null());
    let rebar = FindWindowExW(tray, 0, wide("ReBarWindow32").as_ptr(), null());
    let band = FindWindowExW(rebar, 0, wide("MSTaskSwWClass").as_ptr(), null());
    if band != 0 {
        PostMessageW(band, TASKBAND_THEME_RELOAD, 3, 0);
    }
    let start = FindWindowExW(tray, 0, wide("Start").as_ptr(), null());
    if start != 0 {
        InvalidateRect(start, null(), 1);
    }
}

unsafe fn write_code(at: usize, bytes: &[u8]) -> bool {
    let mut old = 0u32;
    if VirtualProtect(at as *mut c_void, bytes.len(), PAGE_EXECUTE_READWRITE, &mut old) == 0 {
        return false;
    }
    copy_nonoverlapping(bytes.as_ptr(), at as *mut u8, bytes.len());
    VirtualProtect(at as *mut c_void, bytes.len(), old, &mut old);
    FlushInstructionCache(GetCurrentProcess(), at as *const c_void, bytes.len());
    true
}

unsafe fn matches(at: usize, expected: &[u8]) -> bool {
    std::slice::from_raw_parts(at as *const u8, expected.len()) == expected
}

/// Base of explorer.exe if this is the build the offsets were taken from.
unsafe fn expected_explorer() -> Option<usize> {
    let mut path = [0u16; 260];
    let len = GetModuleFileNameW(0, path.as_mut_ptr(), path.len() as u32) as usize;
    let path = String::from_utf16_lossy(&path[..len.min(path.len())]).to_lowercase();
    if !path.ends_with("\\explorer.exe") {
        return None;
    }
    let base = GetModuleHandleW(null()) as usize;
    let pe = base + *((base + 0x3c) as *const u32) as usize;
    let timestamp = *((pe + 8) as *const u32);
    let image_size = *((pe + 24 + 56) as *const u32);
    let ok = timestamp == IMAGE_TIMESTAMP
        && image_size == IMAGE_SIZE
        && matches(base + THEME_CHANGED.rva, THEME_CHANGED.prologue)
        && matches(base + START_GLYPH.rva, START_GLYPH.prologue)
        && matches(base + BUTTON_WIDTH_RVA, &BUTTON_WIDTH_PROLOGUE);
    ok.then_some(base)
}

/// Builds the trampoline (stolen prologue, then `jmp [rip+0]` back) and patches the entry.
unsafe fn apply(patch: &Patch, base: usize, replacement: usize) -> bool {
    let target = base + patch.rva;
    let trampoline = VirtualAlloc(null_mut(), 64, MEM_COMMIT_RESERVE, PAGE_EXECUTE_READWRITE) as *mut u8;
    if trampoline.is_null() {
        return false;
    }
    copy_nonoverlapping(target as *const u8, trampoline, patch.stolen);
    let jump_back = [0xFF, 0x25, 0, 0, 0, 0];
    copy_nonoverlapping(jump_back.as_ptr(), trampoline.add(patch.stolen), jump_back.len());
    let resume = ((target + patch.stolen) as u64).to_le_bytes();
    copy_nonoverlapping(resume.as_ptr(), trampoline.add(patch.stolen + jump_back.len()), resume.len());
    patch.trampoline.store(trampoline as usize, Ordering::Release);

    let mut entry = [0x90u8; 32];
    if patch.stolen < 12 || patch.stolen > entry.len() {
        return false;
    }
    entry[0] = 0x48;
    entry[1] = 0xB8;
    entry[2..10].copy_from_slice(&(replacement as u64).to_le_bytes());
    entry[10] = 0xFF;
    entry[11] = 0xE0;
    if !write_code(target, &entry[..patch.stolen]) {
        return false;
    }
    patch.target.store(target, Ordering::Release);
    true
}

/// Puts the original bytes back. The trampoline stays allocated (64 bytes; nothing runs in it now).
unsafe fn revert(patch: &Patch) -> bool {
    let target = patch.target.swap(0, Ordering::AcqRel);
    target != 0 && write_code(target, &patch.prologue[..patch.stolen])
}

unsafe fn install() {
    let Some(base) = expected_explorer() else {
        TASKBAR_HOOKS_STATE.store(-1, Ordering::Relaxed);
        return;
    };
    BUTTON_WIDTH.store(base + BUTTON_WIDTH_RVA, Ordering::Release);
    // A panic would take Explorer down with it (panic = "abort"): at least say what it was.
    std::panic::set_hook(Box::new(|info| log(&format!("panic: {info}"))));
    install_step_hooks(base);
    let squares = apply(&THEME_CHANGED, base, handle_theme_changed as *const () as usize);
    let logo = apply(&START_GLYPH, base, start_glyph as *const () as usize);
    if squares || logo {
        TASKBAR_HOOKS_STATE.store(1, Ordering::Relaxed);
        refresh_taskbar();
    }
}

unsafe fn uninstall() {
    uninstall_step_hooks();
    let squares = revert(&THEME_CHANGED);
    let logo = revert(&START_GLYPH);
    if logo {
        restore_glyph_box();
    }
    if squares || logo {
        refresh_taskbar(); // margins come back from the theme, Start repaints with its own glyph
    }
    let bitmap = LOGO.swap(0, Ordering::AcqRel);
    if bitmap != 0 {
        DeleteObject(bitmap as isize);
    }
}

/// WH_GETMESSAGE hook procedure; GlassDock installs it on the taskbar thread only.
/// Its first call (inside explorer.exe, on the taskbar thread) applies the patches.
#[no_mangle]
pub extern "system" fn hook_proc(code: i32, wparam: usize, lparam: isize) -> isize {
    if !TRIED.swap(true, Ordering::AcqRel) {
        unsafe { install() };
    }
    unsafe { CallNextHookEx(0, code, wparam, lparam) }
}

#[no_mangle]
pub extern "system" fn DllMain(_module: isize, reason: u32, reserved: *mut c_void) -> i32 {
    // Unloaded because GlassDock went away (not because Explorer is exiting): undo the patches.
    if reason == DLL_PROCESS_DETACH && reserved.is_null() {
        unsafe { uninstall() };
    }
    1
}
