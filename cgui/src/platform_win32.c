/*
 * Win32 backend.
 *
 *  - The window is WS_POPUP | WS_THICKFRAME with the non-client area removed
 *    (WM_NCCALCSIZE returns 0), so there is no system frame but Windows still
 *    treats it as resizable (Aero Snap, maximize animations, taskbar).
 *  - WS_EX_LAYERED + UpdateLayeredWindow gives per-pixel alpha: our
 *    premultiplied BGRA buffer is exactly what ULW_ALPHA expects.
 *  - Moves/resizes are handed to the system with WM_NCLBUTTONDOWN, which runs
 *    the native modal size/move loop; WM_SIZE inside that loop calls the
 *    refresh callback so the content keeps up with the frame.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include "cgui.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

#define QCAP 512

struct plat_window {
    HWND hwnd;
    HDC memdc;
    HBITMAP dib, old_bmp;
    void *bits;
    int dib_w, dib_h;
    int w, h;
    float scale;
    int min_w, min_h;
    plat_event q[QCAP];
    int qhead, qcount;
    void (*refresh)(void *);
    void *refresh_ctx;
    bool in_sizemove, tracking_leave;
    int cursor;
    WCHAR high_surrogate;
};

static LARGE_INTEGER g_freq;
static HCURSOR g_cursors[CURSOR_COUNT];
static const wchar_t *CLASS_NAME = L"cgui_window";

typedef BOOL(WINAPI *SetDpiCtxFn)(HANDLE);
typedef UINT(WINAPI *GetDpiForWindowFn)(HWND);
typedef UINT(WINAPI *GetDpiForSystemFn)(void);
static GetDpiForWindowFn pGetDpiForWindow;
static GetDpiForSystemFn pGetDpiForSystem;

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

/* ------------------------------------------------------------------ */

static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    else w[0] = 0;
    return w;
}

static char *to_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc((size_t)(n > 0 ? n : 1));
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    else s[0] = 0;
    return s;
}

bool plat_init(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetDpiCtxFn set_ctx = (SetDpiCtxFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (!set_ctx || !set_ctx((HANDLE)(intptr_t)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
    pGetDpiForWindow = (GetDpiForWindowFn)(void *)GetProcAddress(user32, "GetDpiForWindow");
    pGetDpiForSystem = (GetDpiForSystemFn)(void *)GetProcAddress(user32, "GetDpiForSystem");
    QueryPerformanceFrequency(&g_freq);

    static const LPCWSTR ids[CURSOR_COUNT] = { IDC_ARROW, IDC_IBEAM, IDC_HAND, IDC_SIZEWE,
                                               IDC_SIZENS, IDC_SIZENWSE, IDC_SIZENESW };
    for (int i = 0; i < CURSOR_COUNT; i++) g_cursors[i] = LoadCursorW(NULL, ids[i]);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void plat_shutdown(void) { UnregisterClassW(CLASS_NAME, GetModuleHandleW(NULL)); }

double plat_time(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_freq.QuadPart;
}

/* With WM_NCCALCSIZE returning 0 the window and client rects coincide on
 * Windows; using the window rect also keeps UpdateLayeredWindow's size in
 * agreement with the window on implementations that keep a hidden frame. */
static void window_dims(HWND hwnd, int *w, int *h)
{
    RECT r;
    GetWindowRect(hwnd, &r);
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

static float dpi_scale(HWND hwnd)
{
    const char *env = getenv("CGUI_SCALE");
    if (env && atof(env) > 0) return (float)atof(env);
    UINT dpi = 0;
    if (hwnd && pGetDpiForWindow) dpi = pGetDpiForWindow(hwnd);
    else if (pGetDpiForSystem) dpi = pGetDpiForSystem();
    if (!dpi) {
        HDC dc = GetDC(NULL);
        dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    return dpi ? dpi / 96.f : 1.f;
}

/* ------------------------------------------------------------------ */
/* Event queue                                                         */
/* ------------------------------------------------------------------ */

static void push(plat_window *pw, const plat_event *e)
{
    if (e->type == PE_MOUSE_MOVE && pw->qcount > 0) {
        plat_event *last = &pw->q[(pw->qhead + pw->qcount - 1) % QCAP];
        if (last->type == PE_MOUSE_MOVE) {
            *last = *e;
            return;
        }
    }
    if (pw->qcount == QCAP) return;
    pw->q[(pw->qhead + pw->qcount) % QCAP] = *e;
    pw->qcount++;
}

static void push_simple(plat_window *pw, int type)
{
    plat_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    push(pw, &e);
}

static int mods_now(void)
{
    int m = 0;
    if (GetKeyState(VK_SHIFT) < 0) m |= CG_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) < 0) m |= CG_MOD_CTRL;
    if (GetKeyState(VK_MENU) < 0) m |= CG_MOD_ALT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= CG_MOD_SUPER;
    return m;
}

static int map_vk(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (int)vk;
    if (vk >= '0' && vk <= '9') return (int)vk;
    switch (vk) {
    case VK_BACK: return CG_KEY_BACKSPACE;
    case VK_DELETE: return CG_KEY_DELETE;
    case VK_RETURN: return CG_KEY_ENTER;
    case VK_TAB: return CG_KEY_TAB;
    case VK_ESCAPE: return CG_KEY_ESCAPE;
    case VK_LEFT: return CG_KEY_LEFT;
    case VK_RIGHT: return CG_KEY_RIGHT;
    case VK_UP: return CG_KEY_UP;
    case VK_DOWN: return CG_KEY_DOWN;
    case VK_HOME: return CG_KEY_HOME;
    case VK_END: return CG_KEY_END;
    case VK_PRIOR: return CG_KEY_PAGE_UP;
    case VK_NEXT: return CG_KEY_PAGE_DOWN;
    default: return 0;
    }
}

static void queue_mouse(plat_window *pw, int type, int button, LPARAM lp)
{
    plat_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    e.button = button;
    e.x = GET_X_LPARAM(lp);
    e.y = GET_Y_LPARAM(lp);
    e.mods = mods_now();
    push(pw, &e);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    plat_window *pw = (plat_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        pw = (plat_window *)cs->lpCreateParams;
        pw->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pw);
    }
    if (!pw) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) return 0; /* the whole window is client area */
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd, msg, wp, -1); /* activate without NC repaint */
    case WM_NCPAINT:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        push_simple(pw, PE_EXPOSE);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            window_dims(hwnd, &pw->w, &pw->h);
            plat_event e;
            memset(&e, 0, sizeof e);
            e.type = PE_RESIZE;
            e.w = pw->w;
            e.h = pw->h;
            push(pw, &e);
            push_simple(pw, PE_STATE);
            if (pw->in_sizemove && pw->refresh) pw->refresh(pw->refresh_ctx);
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        pw->in_sizemove = true;
        return 0;
    case WM_EXITSIZEMOVE:
        pw->in_sizemove = false;
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = pw->min_w;
        mmi->ptMinTrackSize.y = pw->min_h;
        /* Maximize to the work area, not over the taskbar. */
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(mon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        pw->scale = HIWORD(wp) / 96.f;
        const RECT *r = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        push_simple(pw, PE_SCALE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!pw->tracking_leave) {
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof tme;
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
            pw->tracking_leave = true;
        }
        queue_mouse(pw, PE_MOUSE_MOVE, 0, lp);
        return 0;
    case WM_MOUSELEAVE:
        pw->tracking_leave = false;
        push_simple(pw, PE_MOUSE_LEAVE);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        SetCapture(hwnd);
        queue_mouse(pw, PE_MOUSE_DOWN, msg == WM_LBUTTONDOWN ? 0 : msg == WM_RBUTTONDOWN ? 1 : 2, lp);
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        queue_mouse(pw, PE_MOUSE_UP, msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2, lp);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        plat_event e;
        memset(&e, 0, sizeof e);
        e.type = PE_WHEEL;
        e.x = p.x;
        e.y = p.y;
        e.mods = mods_now();
        float d = GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        if (msg == WM_MOUSEWHEEL) e.wheel_y = d;
        else e.wheel_x = -d;
        push(pw, &e);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int key = map_vk(wp);
        if (key) {
            plat_event e;
            memset(&e, 0, sizeof e);
            e.type = PE_KEY_DOWN;
            e.key = key;
            e.mods = mods_now();
            push(pw, &e);
        }
        if (msg == WM_SYSKEYDOWN) break; /* keep Alt+F4 etc. */
        return 0;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if (c >= 0xD800 && c <= 0xDBFF) {
            pw->high_surrogate = c;
            return 0;
        }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            if (!pw->high_surrogate) return 0;
            cp = 0x10000 + (((uint32_t)pw->high_surrogate - 0xD800) << 10) + (c - 0xDC00);
            pw->high_surrogate = 0;
        }
        int m = mods_now();
        if (cp < 32 || cp == 127 || ((m & CG_MOD_CTRL) && !(m & CG_MOD_ALT))) return 0;
        plat_event e;
        memset(&e, 0, sizeof e);
        e.type = PE_TEXT;
        wchar_t wbuf[3] = { 0 };
        if (cp >= 0x10000) {
            wbuf[0] = (wchar_t)(0xD800 + ((cp - 0x10000) >> 10));
            wbuf[1] = (wchar_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            wbuf[0] = (wchar_t)cp;
        }
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, e.text, (int)sizeof e.text, NULL, NULL);
        push(pw, &e);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(g_cursors[pw->cursor >= 0 ? pw->cursor : 0]);
            return TRUE;
        }
        break;
    case WM_SETFOCUS:
        push_simple(pw, PE_FOCUS);
        return 0;
    case WM_KILLFOCUS:
        push_simple(pw, PE_UNFOCUS);
        return 0;
    case WM_CLOSE:
        push_simple(pw, PE_CLOSE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

plat_window *plat_window_create(const char *title, int w, int h)
{
    plat_window *pw = (plat_window *)calloc(1, sizeof *pw);
    pw->scale = dpi_scale(NULL);
    int pw_w = (int)(w * pw->scale), pw_h = (int)(h * pw->scale);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - pw_w) / 2;
    int y = work.top + ((work.bottom - work.top) - pw_h) / 2;

    wchar_t *wt = to_wide(title);
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, CLASS_NAME, wt, style, x, y, pw_w, pw_h,
                                NULL, NULL, GetModuleHandleW(NULL), pw);
    free(wt);
    if (!hwnd) {
        free(pw);
        return NULL;
    }
    float real = dpi_scale(hwnd);
    if (real != pw->scale) {
        pw->scale = real;
        SetWindowPos(hwnd, NULL, 0, 0, (int)(w * real), (int)(h * real), SWP_NOMOVE | SWP_NOZORDER);
    }
    window_dims(hwnd, &pw->w, &pw->h);
    pw->memdc = CreateCompatibleDC(NULL);
    ShowWindow(hwnd, SW_SHOW);
    return pw;
}

void plat_window_destroy(plat_window *pw)
{
    if (!pw) return;
    if (pw->dib) {
        SelectObject(pw->memdc, pw->old_bmp);
        DeleteObject(pw->dib);
    }
    DeleteDC(pw->memdc);
    DestroyWindow(pw->hwnd);
    free(pw);
}

void plat_window_set_title(plat_window *pw, const char *title)
{
    wchar_t *wt = to_wide(title);
    SetWindowTextW(pw->hwnd, wt);
    free(wt);
}

void plat_window_set_min_size(plat_window *pw, int w, int h)
{
    pw->min_w = w;
    pw->min_h = h;
}

void plat_window_size(plat_window *pw, int *w, int *h)
{
    *w = pw->w;
    *h = pw->h;
}

float plat_window_scale(plat_window *pw) { return pw->scale; }
bool plat_window_has_alpha(plat_window *pw)
{
    (void)pw;
    return true;
}

void plat_window_set_refresh(plat_window *pw, void (*fn)(void *), void *ctx)
{
    pw->refresh = fn;
    pw->refresh_ctx = ctx;
}

void plat_window_present(plat_window *pw, const uint32_t *pixels, int w, int h, unsigned shape_serial)
{
    (void)shape_serial;
    if (!pw->dib || pw->dib_w != w || pw->dib_h != h) {
        if (pw->dib) {
            SelectObject(pw->memdc, pw->old_bmp);
            DeleteObject(pw->dib);
        }
        BITMAPINFO bi;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        pw->dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &pw->bits, NULL, 0);
        if (!pw->dib) return;
        pw->old_bmp = (HBITMAP)SelectObject(pw->memdc, pw->dib);
        pw->dib_w = w;
        pw->dib_h = h;
    }
    GdiFlush();
    memcpy(pw->bits, pixels, (size_t)w * h * 4);
    POINT src = { 0, 0 };
    SIZE size = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(pw->hwnd, NULL, NULL, &size, pw->memdc, &src, 0, &bf, ULW_ALPHA);
}

void plat_window_begin_drag(plat_window *pw, int edges)
{
    static const int ht[16] = {
        HTCAPTION, HTLEFT, HTRIGHT, 0, HTTOP, HTTOPLEFT, HTTOPRIGHT, 0,
        HTBOTTOM, HTBOTTOMLEFT, HTBOTTOMRIGHT, 0, 0, 0, 0, 0,
    };
    int code = ht[edges & 15];
    if (!code) return;
    int btn = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    if (!(GetAsyncKeyState(btn) & 0x8000)) return; /* already released */
    POINT p;
    GetCursorPos(&p);
    ReleaseCapture();
    /* Enters the system's modal move/size loop (with snapping). */
    SendMessageW(pw->hwnd, WM_NCLBUTTONDOWN, (WPARAM)code, MAKELPARAM(p.x, p.y));
}

void plat_window_minimize(plat_window *pw) { ShowWindow(pw->hwnd, SW_MINIMIZE); }

void plat_window_toggle_maximize(plat_window *pw)
{
    ShowWindow(pw->hwnd, IsZoomed(pw->hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

bool plat_window_is_maximized(plat_window *pw) { return IsZoomed(pw->hwnd) != 0; }

void plat_window_set_cursor(plat_window *pw, int cursor)
{
    if (cursor < 0 || cursor >= CURSOR_COUNT) return;
    pw->cursor = cursor;
    SetCursor(g_cursors[cursor]);
}

static void pump(plat_window *pw)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) push_simple(pw, PE_CLOSE);
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

bool plat_poll_event(plat_window *pw, plat_event *ev)
{
    if (pw->qcount == 0) pump(pw);
    if (pw->qcount == 0) return false;
    *ev = pw->q[pw->qhead];
    pw->qhead = (pw->qhead + 1) % QCAP;
    pw->qcount--;
    return true;
}

void plat_wait_events(plat_window *pw, int timeout_ms)
{
    if (pw->qcount > 0) return;
    MsgWaitForMultipleObjectsEx(0, NULL, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);
}

/* ------------------------------------------------------------------ */
/* Clipboard                                                           */
/* ------------------------------------------------------------------ */

void plat_clipboard_set(plat_window *pw, const char *text)
{
    if (!text || !OpenClipboard(pw->hwnd)) return;
    EmptyClipboard();
    /* Windows apps expect CRLF line endings. */
    size_t n = strlen(text), extra = 0;
    for (size_t i = 0; i < n; i++) extra += text[i] == '\n';
    char *crlf = (char *)malloc(n + extra + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\n') crlf[k++] = '\r';
        crlf[k++] = text[i];
    }
    crlf[k] = 0;
    wchar_t *w = to_wide(crlf);
    free(crlf);
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        memcpy(GlobalLock(mem), w, bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    free(w);
    CloseClipboard();
}

char *plat_clipboard_get(plat_window *pw)
{
    if (!OpenClipboard(pw->hwnd)) return NULL;
    char *out = NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            out = to_utf8(w);
            GlobalUnlock(h);
            size_t k = 0;
            for (size_t i = 0; out[i]; i++)
                if (out[i] != '\r') out[k++] = out[i];
            out[k] = 0;
        }
    }
    CloseClipboard();
    return out;
}

/* ------------------------------------------------------------------ */
/* Font directories                                                    */
/* ------------------------------------------------------------------ */

int plat_font_dirs(char **out, int max)
{
    int n = 0;
    char buf[MAX_PATH];
    UINT len = GetWindowsDirectoryA(buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH - 8 && n < max) {
        strcat(buf, "\\Fonts");
        out[n++] = _strdup(buf);
    }
    const char *local = getenv("LOCALAPPDATA");
    if (local && n < max) {
        snprintf(buf, sizeof buf, "%s\\Microsoft\\Windows\\Fonts", local);
        out[n++] = _strdup(buf);
    }
    return n;
}
