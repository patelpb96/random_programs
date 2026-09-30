/*
 * X11 backend (Linux, BSD, and Wayland sessions through XWayland).
 *
 *  - A 32-bit ARGB visual gives per-pixel transparency under a compositor.
 *    Without one, the rounded outline is applied with the XShape extension.
 *  - Window decorations are removed with _MOTIF_WM_HINTS.
 *  - Moving/resizing is delegated to the window manager through
 *    _NET_WM_MOVERESIZE (so snapping etc. work). With no EWMH window manager
 *    a manual pointer-grab loop is used instead.
 */
#define _DEFAULT_SOURCE
#include "cgui.h"
#include "platform.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#ifdef CGUI_HAVE_XSHAPE
#include <X11/extensions/shape.h>
#endif

#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QCAP 512

enum {
    A_WM_PROTOCOLS, A_WM_DELETE_WINDOW, A_NET_WM_NAME, A_UTF8_STRING, A_MOTIF_WM_HINTS,
    A_NET_WM_MOVERESIZE, A_NET_WM_STATE, A_NET_WM_STATE_MAX_V, A_NET_WM_STATE_MAX_H,
    A_NET_WM_STATE_HIDDEN, A_NET_SUPPORTED, A_CLIPBOARD, A_TARGETS, A_CGUI_CLIP,
    A_NET_WM_WINDOW_TYPE, A_NET_WM_WINDOW_TYPE_NORMAL, A_COUNT
};

static const char *atom_names[A_COUNT] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME", "UTF8_STRING", "_MOTIF_WM_HINTS",
    "_NET_WM_MOVERESIZE", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT",
    "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_HIDDEN", "_NET_SUPPORTED", "CLIPBOARD",
    "TARGETS", "CGUI_CLIP", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL",
};

typedef struct manual_drag {
    bool active;
    int edges;
    int start_rx, start_ry;
    int x, y, w, h;
} manual_drag;

struct plat_window {
    Window win;
    GC gc;
    XIC ic;
    XImage *img;
    int w, h;
    int min_w, min_h;
    bool maximized, fake_max;
    int saved_x, saved_y, saved_w, saved_h;
    unsigned shape_serial;
    bool shape_applied;
    int cursor;
    plat_event q[QCAP];
    int qhead, qcount;
    void (*refresh)(void *);
    void *refresh_ctx;
    int root_x, root_y;
    char *clip;
    manual_drag drag;
    bool resized;
};

static Display *dpy;
static int screen;
static Window root;
static Visual *visual;
static int depth;
static Colormap cmap;
static XIM im;
static Atom atoms[A_COUNT];
static bool argb, composited, wm_moveresize, wm_state;
static float g_scale = 1.f;
static Cursor cursors[CURSOR_COUNT];

/* ------------------------------------------------------------------ */

static bool wm_supports(Atom what)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    bool found = false;
    if (XGetWindowProperty(dpy, root, atoms[A_NET_SUPPORTED], 0, 4096, False, XA_ATOM, &type,
                           &format, &n, &after, &data) == Success && data) {
        Atom *list = (Atom *)data;
        for (unsigned long i = 0; i < n; i++)
            if (list[i] == what) found = true;
        XFree(data);
    }
    return found;
}

static float read_scale(void)
{
    const char *env = getenv("CGUI_SCALE");
    if (env && atof(env) > 0) return (float)atof(env);
    char *rms = XResourceManagerString(dpy);
    if (rms) {
        const char *p = strstr(rms, "Xft.dpi:");
        if (p) {
            float dpi = (float)atof(p + 8);
            if (dpi >= 48 && dpi <= 480) return dpi / 96.f;
        }
    }
    return 1.f;
}

bool plat_init(void)
{
    if (dpy) return true;
    const char *loc = setlocale(LC_CTYPE, NULL);
    if (!loc || strcmp(loc, "C") == 0) setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "cgui: cannot open X display\n");
        return false;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    XInternAtoms(dpy, (char **)atom_names, A_COUNT, False, atoms);

    XVisualInfo vi;
    if (XMatchVisualInfo(dpy, screen, 32, TrueColor, &vi)) {
        visual = vi.visual;
        depth = 32;
        argb = true;
    } else {
        visual = DefaultVisual(dpy, screen);
        depth = DefaultDepth(dpy, screen);
    }
    cmap = XCreateColormap(dpy, root, visual, AllocNone);

    char cm_name[32];
    snprintf(cm_name, sizeof cm_name, "_NET_WM_CM_S%d", screen);
    composited = XGetSelectionOwner(dpy, XInternAtom(dpy, cm_name, False)) != None;
    wm_moveresize = wm_supports(atoms[A_NET_WM_MOVERESIZE]);
    wm_state = wm_supports(atoms[A_NET_WM_STATE]);
    g_scale = read_scale();

    im = XOpenIM(dpy, NULL, NULL, NULL);

    static const unsigned shapes[CURSOR_COUNT] = {
        XC_left_ptr, XC_xterm, XC_hand2, XC_sb_h_double_arrow, XC_sb_v_double_arrow,
        XC_bottom_right_corner, XC_bottom_left_corner,
    };
    for (int i = 0; i < CURSOR_COUNT; i++) cursors[i] = XCreateFontCursor(dpy, shapes[i]);
    return true;
}

void plat_shutdown(void)
{
    if (!dpy) return;
    if (im) XCloseIM(im);
    XCloseDisplay(dpy);
    dpy = NULL;
}

double plat_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static void set_min_hints(plat_window *pw)
{
    XSizeHints *sh = XAllocSizeHints();
    sh->flags = PMinSize;
    sh->min_width = pw->min_w;
    sh->min_height = pw->min_h;
    XSetWMNormalHints(dpy, pw->win, sh);
    XFree(sh);
}

plat_window *plat_window_create(const char *title, int w, int h)
{
    plat_window *pw = (plat_window *)calloc(1, sizeof *pw);
    pw->w = (int)(w * g_scale);
    pw->h = (int)(h * g_scale);
    pw->cursor = -1;

    XSetWindowAttributes wa;
    memset(&wa, 0, sizeof wa);
    wa.colormap = cmap;
    wa.border_pixel = 0;
    wa.background_pixel = 0;
    wa.bit_gravity = NorthWestGravity;
    wa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                    ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask |
                    LeaveWindowMask | EnterWindowMask | PropertyChangeMask;
    pw->win = XCreateWindow(dpy, root, 0, 0, (unsigned)pw->w, (unsigned)pw->h, 0, depth, InputOutput,
                            visual, CWColormap | CWBorderPixel | CWBackPixel | CWEventMask | CWBitGravity,
                            &wa);
    if (!pw->win) {
        free(pw);
        return NULL;
    }
    pw->gc = XCreateGC(dpy, pw->win, 0, NULL);

    /* No WM decorations: we draw our own. */
    struct { unsigned long flags, functions, decorations; long input_mode; unsigned long status; } mwm = { 2, 0, 0, 0, 0 };
    XChangeProperty(dpy, pw->win, atoms[A_MOTIF_WM_HINTS], atoms[A_MOTIF_WM_HINTS], 32, PropModeReplace,
                    (unsigned char *)&mwm, 5);
    XSetWMProtocols(dpy, pw->win, &atoms[A_WM_DELETE_WINDOW], 1);
    Atom type = atoms[A_NET_WM_WINDOW_TYPE_NORMAL];
    XChangeProperty(dpy, pw->win, atoms[A_NET_WM_WINDOW_TYPE], XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&type, 1);
    XClassHint *ch = XAllocClassHint();
    ch->res_name = (char *)"cgui";
    ch->res_class = (char *)"cgui";
    XSetClassHint(dpy, pw->win, ch);
    XFree(ch);
    plat_window_set_title(pw, title);

    if (im) {
        pw->ic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
                           pw->win, XNFocusWindow, pw->win, (void *)NULL);
    }
    XMapWindow(dpy, pw->win);
    XFlush(dpy);
    return pw;
}

void plat_window_destroy(plat_window *pw)
{
    if (!pw) return;
    if (pw->img) {
        pw->img->data = NULL;
        XDestroyImage(pw->img);
    }
    if (pw->ic) XDestroyIC(pw->ic);
    XFreeGC(dpy, pw->gc);
    XDestroyWindow(dpy, pw->win);
    XFlush(dpy);
    free(pw->clip);
    free(pw);
}

void plat_window_set_title(plat_window *pw, const char *title)
{
    XStoreName(dpy, pw->win, title);
    XChangeProperty(dpy, pw->win, atoms[A_NET_WM_NAME], atoms[A_UTF8_STRING], 8, PropModeReplace,
                    (const unsigned char *)title, (int)strlen(title));
}

void plat_window_set_min_size(plat_window *pw, int w, int h)
{
    pw->min_w = w;
    pw->min_h = h;
    set_min_hints(pw);
}

void plat_window_size(plat_window *pw, int *w, int *h)
{
    *w = pw->w;
    *h = pw->h;
}

float plat_window_scale(plat_window *pw)
{
    (void)pw;
    return g_scale;
}

bool plat_window_has_alpha(plat_window *pw)
{
    (void)pw;
    /* A compositor can start or stop at any time; re-check once a second. */
    static double last_check;
    double now = plat_time();
    if (argb && now - last_check > 1.0) {
        char name[32];
        snprintf(name, sizeof name, "_NET_WM_CM_S%d", screen);
        composited = XGetSelectionOwner(dpy, XInternAtom(dpy, name, False)) != None;
        last_check = now;
    }
    return argb && composited;
}

void plat_window_set_refresh(plat_window *pw, void (*fn)(void *), void *ctx)
{
    pw->refresh = fn;
    pw->refresh_ctx = ctx;
}

#ifdef CGUI_HAVE_XSHAPE
/* Without a compositor alpha is ignored, so cut the outline with XShape. */
static void apply_shape(plat_window *pw, const uint32_t *px, int w, int h)
{
    int bpl = (w + 7) / 8;
    char *bits = (char *)calloc((size_t)bpl * h, 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if ((px[(size_t)y * w + x] >> 24) >= 128) bits[y * bpl + x / 8] |= (char)(1 << (x & 7));
    Pixmap mask = XCreateBitmapFromData(dpy, pw->win, bits, (unsigned)w, (unsigned)h);
    XShapeCombineMask(dpy, pw->win, ShapeBounding, 0, 0, mask, ShapeSet);
    XFreePixmap(dpy, mask);
    free(bits);
}
#endif

void plat_window_present(plat_window *pw, const uint32_t *pixels, int w, int h, unsigned shape_serial)
{
    if (!pw->img || pw->img->width != w || pw->img->height != h) {
        if (pw->img) {
            pw->img->data = NULL;
            XDestroyImage(pw->img);
        }
        pw->img = XCreateImage(dpy, visual, (unsigned)depth, ZPixmap, 0, NULL, (unsigned)w, (unsigned)h, 32, w * 4);
        if (!pw->img) return;
        const uint32_t one = 1;
        pw->img->byte_order = *(const uint8_t *)&one ? LSBFirst : MSBFirst;
    }
    pw->img->data = (char *)pixels;
    XPutImage(dpy, pw->win, pw->gc, pw->img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    pw->img->data = NULL;
#ifdef CGUI_HAVE_XSHAPE
    if (!argb || !composited) {
        if (!pw->shape_applied || shape_serial != pw->shape_serial) {
            apply_shape(pw, pixels, w, h);
            pw->shape_applied = true;
            pw->shape_serial = shape_serial;
        }
    } else if (pw->shape_applied) {
        XShapeCombineMask(dpy, pw->win, ShapeBounding, 0, 0, None, ShapeSet);
        pw->shape_applied = false;
    }
#else
    (void)shape_serial;
#endif
    XFlush(dpy);
}

void plat_window_set_cursor(plat_window *pw, int cursor)
{
    if (cursor == pw->cursor || cursor < 0 || cursor >= CURSOR_COUNT) return;
    pw->cursor = cursor;
    XDefineCursor(dpy, pw->win, cursors[cursor]);
    XFlush(dpy);
}

void plat_window_minimize(plat_window *pw)
{
    XIconifyWindow(dpy, pw->win, screen);
    XFlush(dpy);
}

static void send_state(plat_window *pw, long action, Atom a1, Atom a2)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = pw->win;
    ev.xclient.message_type = atoms[A_NET_WM_STATE];
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = action;
    ev.xclient.data.l[1] = (long)a1;
    ev.xclient.data.l[2] = (long)a2;
    ev.xclient.data.l[3] = 1;
    XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

static void window_origin(plat_window *pw, int *x, int *y)
{
    Window child;
    XTranslateCoordinates(dpy, pw->win, root, 0, 0, x, y, &child);
}

void plat_window_toggle_maximize(plat_window *pw)
{
    if (wm_state) {
        send_state(pw, 2 /* toggle */, atoms[A_NET_WM_STATE_MAX_V], atoms[A_NET_WM_STATE_MAX_H]);
        XFlush(dpy);
        return;
    }
    /* No window manager: do it ourselves. */
    if (!pw->fake_max) {
        window_origin(pw, &pw->saved_x, &pw->saved_y);
        pw->saved_w = pw->w;
        pw->saved_h = pw->h;
        XMoveResizeWindow(dpy, pw->win, 0, 0, (unsigned)DisplayWidth(dpy, screen),
                          (unsigned)DisplayHeight(dpy, screen));
        pw->fake_max = true;
    } else {
        XMoveResizeWindow(dpy, pw->win, pw->saved_x, pw->saved_y, (unsigned)pw->saved_w, (unsigned)pw->saved_h);
        pw->fake_max = false;
    }
    pw->maximized = pw->fake_max;
    XFlush(dpy);
}

bool plat_window_is_maximized(plat_window *pw) { return pw->maximized; }

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void push(plat_window *pw, const plat_event *e)
{
    /* Coalesce consecutive motion events. */
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

static int x_mods(unsigned state)
{
    int m = 0;
    if (state & ShiftMask) m |= CG_MOD_SHIFT;
    if (state & ControlMask) m |= CG_MOD_CTRL;
    if (state & Mod1Mask) m |= CG_MOD_ALT;
    if (state & Mod4Mask) m |= CG_MOD_SUPER;
    return m;
}

static int map_key(KeySym ks)
{
    if (ks >= XK_a && ks <= XK_z) return 'A' + (int)(ks - XK_a);
    if (ks >= XK_A && ks <= XK_Z) return 'A' + (int)(ks - XK_A);
    if (ks >= XK_0 && ks <= XK_9) return '0' + (int)(ks - XK_0);
    switch (ks) {
    case XK_BackSpace: return CG_KEY_BACKSPACE;
    case XK_Delete: case XK_KP_Delete: return CG_KEY_DELETE;
    case XK_Return: case XK_KP_Enter: return CG_KEY_ENTER;
    case XK_Tab: case XK_ISO_Left_Tab: return CG_KEY_TAB;
    case XK_Escape: return CG_KEY_ESCAPE;
    case XK_Left: case XK_KP_Left: return CG_KEY_LEFT;
    case XK_Right: case XK_KP_Right: return CG_KEY_RIGHT;
    case XK_Up: case XK_KP_Up: return CG_KEY_UP;
    case XK_Down: case XK_KP_Down: return CG_KEY_DOWN;
    case XK_Home: case XK_KP_Home: return CG_KEY_HOME;
    case XK_End: case XK_KP_End: return CG_KEY_END;
    case XK_Page_Up: case XK_KP_Page_Up: return CG_KEY_PAGE_UP;
    case XK_Page_Down: case XK_KP_Page_Down: return CG_KEY_PAGE_DOWN;
    default: return 0;
    }
}

static void read_wm_state(plat_window *pw)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    int maxed = 0;
    if (XGetWindowProperty(dpy, pw->win, atoms[A_NET_WM_STATE], 0, 64, False, XA_ATOM, &type, &format, &n,
                           &after, &data) == Success && data) {
        Atom *a = (Atom *)data;
        for (unsigned long i = 0; i < n; i++)
            if (a[i] == atoms[A_NET_WM_STATE_MAX_V] || a[i] == atoms[A_NET_WM_STATE_MAX_H]) maxed++;
        XFree(data);
    }
    bool m = maxed >= 2;
    if (m != pw->maximized) {
        pw->maximized = m;
        plat_event e;
        memset(&e, 0, sizeof e);
        e.type = PE_STATE;
        push(pw, &e);
    }
}

static void answer_selection(plat_window *pw, XSelectionRequestEvent *r)
{
    XSelectionEvent sev;
    memset(&sev, 0, sizeof sev);
    sev.type = SelectionNotify;
    sev.requestor = r->requestor;
    sev.selection = r->selection;
    sev.target = r->target;
    sev.time = r->time;
    sev.property = None;
    Atom prop = r->property != None ? r->property : r->target;
    if (r->target == atoms[A_TARGETS]) {
        Atom t[3] = { atoms[A_TARGETS], atoms[A_UTF8_STRING], XA_STRING };
        XChangeProperty(dpy, r->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 3);
        sev.property = prop;
    } else if ((r->target == atoms[A_UTF8_STRING] || r->target == XA_STRING) && pw->clip) {
        XChangeProperty(dpy, r->requestor, prop, r->target, 8, PropModeReplace, (unsigned char *)pw->clip,
                        (int)strlen(pw->clip));
        sev.property = prop;
    }
    XSendEvent(dpy, r->requestor, False, 0, (XEvent *)&sev);
}

static void drag_motion(plat_window *pw, int rx, int ry)
{
    manual_drag *d = &pw->drag;
    int dx = rx - d->start_rx, dy = ry - d->start_ry;
    int x = d->x, y = d->y, w = d->w, h = d->h;
    if (d->edges == 0) {
        x += dx;
        y += dy;
    }
    if (d->edges & EDGE_LEFT) {
        int nw = w - dx < pw->min_w ? pw->min_w : w - dx;
        x += w - nw;
        w = nw;
    }
    if (d->edges & EDGE_RIGHT) w = w + dx < pw->min_w ? pw->min_w : w + dx;
    if (d->edges & EDGE_TOP) {
        int nh = h - dy < pw->min_h ? pw->min_h : h - dy;
        y += h - nh;
        h = nh;
    }
    if (d->edges & EDGE_BOTTOM) h = h + dy < pw->min_h ? pw->min_h : h + dy;
    XMoveResizeWindow(dpy, pw->win, x, y, (unsigned)w, (unsigned)h);
}

static void handle(plat_window *pw, XEvent *ev)
{
    if (XFilterEvent(ev, None)) return;
    plat_event e;
    memset(&e, 0, sizeof e);
    switch (ev->type) {
    case Expose:
        if (ev->xexpose.count == 0) {
            e.type = PE_EXPOSE;
            push(pw, &e);
        }
        break;
    case ConfigureNotify:
        if (ev->xconfigure.width != pw->w || ev->xconfigure.height != pw->h) {
            pw->w = ev->xconfigure.width;
            pw->h = ev->xconfigure.height;
            pw->resized = true;
            e.type = PE_RESIZE;
            e.w = pw->w;
            e.h = pw->h;
            push(pw, &e);
        }
        break;
    case MotionNotify:
        pw->root_x = ev->xmotion.x_root;
        pw->root_y = ev->xmotion.y_root;
        if (pw->drag.active) {
            drag_motion(pw, pw->root_x, pw->root_y);
            break;
        }
        e.type = PE_MOUSE_MOVE;
        e.x = ev->xmotion.x;
        e.y = ev->xmotion.y;
        e.mods = x_mods(ev->xmotion.state);
        push(pw, &e);
        break;
    case ButtonPress:
    case ButtonRelease: {
        pw->root_x = ev->xbutton.x_root;
        pw->root_y = ev->xbutton.y_root;
        if (pw->drag.active) {
            if (ev->type == ButtonRelease && ev->xbutton.button == Button1) pw->drag.active = false;
            break;
        }
        unsigned b = ev->xbutton.button;
        e.x = ev->xbutton.x;
        e.y = ev->xbutton.y;
        e.mods = x_mods(ev->xbutton.state);
        if (b >= 4 && b <= 7) {
            if (ev->type == ButtonPress) {
                e.type = PE_WHEEL;
                if (b == 4) e.wheel_y = 1;
                if (b == 5) e.wheel_y = -1;
                if (b == 6) e.wheel_x = 1;
                if (b == 7) e.wheel_x = -1;
                push(pw, &e);
            }
            break;
        }
        e.type = ev->type == ButtonPress ? PE_MOUSE_DOWN : PE_MOUSE_UP;
        e.button = b == Button1 ? 0 : b == Button3 ? 1 : b == Button2 ? 2 : -1;
        if (e.button >= 0) push(pw, &e);
        break;
    }
    case KeyPress: {
        char buf[64];
        KeySym ks = 0;
        Status st = 0;
        int n;
        if (pw->ic)
            n = Xutf8LookupString(pw->ic, &ev->xkey, buf, sizeof buf - 1, &ks, &st);
        else
            n = XLookupString(&ev->xkey, buf, sizeof buf - 1, &ks, NULL);
        if (n < 0 || st == XBufferOverflow) n = 0;
        buf[n] = 0;
        int mods = x_mods(ev->xkey.state);
        e.mods = mods;
        if (st != XLookupChars) {
            int key = map_key(ks);
            if (key) {
                e.type = PE_KEY_DOWN;
                e.key = key;
                push(pw, &e);
            }
        }
        bool ctrl_only = (mods & CG_MOD_CTRL) && !(mods & CG_MOD_ALT);
        if (n > 0 && !ctrl_only && (unsigned char)buf[0] >= 32 && buf[0] != 127) {
            plat_event t;
            memset(&t, 0, sizeof t);
            t.type = PE_TEXT;
            /* Split into chunks that fit the event. */
            for (int i = 0; i < n;) {
                int take = n - i < (int)sizeof t.text - 1 ? n - i : (int)sizeof t.text - 1;
                while (take > 0 && i + take < n && ((unsigned char)buf[i + take] & 0xC0) == 0x80) take--;
                memcpy(t.text, buf + i, (size_t)take);
                t.text[take] = 0;
                push(pw, &t);
                i += take;
            }
        }
        break;
    }
    case LeaveNotify:
        if (ev->xcrossing.mode == NotifyNormal) {
            e.type = PE_MOUSE_LEAVE;
            push(pw, &e);
        }
        break;
    case FocusIn:
        if (pw->ic) XSetICFocus(pw->ic);
        e.type = PE_FOCUS;
        push(pw, &e);
        break;
    case FocusOut:
        if (pw->ic) XUnsetICFocus(pw->ic);
        e.type = PE_UNFOCUS;
        push(pw, &e);
        break;
    case ClientMessage:
        if (ev->xclient.message_type == atoms[A_WM_PROTOCOLS] &&
            (Atom)ev->xclient.data.l[0] == atoms[A_WM_DELETE_WINDOW]) {
            e.type = PE_CLOSE;
            push(pw, &e);
        }
        break;
    case PropertyNotify:
        if (ev->xproperty.atom == atoms[A_NET_WM_STATE]) read_wm_state(pw);
        break;
    case SelectionRequest:
        answer_selection(pw, &ev->xselectionrequest);
        break;
    case SelectionClear:
        if (ev->xselectionclear.selection == atoms[A_CLIPBOARD]) {
            free(pw->clip);
            pw->clip = NULL;
        }
        break;
    default:
        break;
    }
}

static void pump(plat_window *pw)
{
    while (XPending(dpy)) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        handle(pw, &ev);
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
    XFlush(dpy);
    if (XPending(dpy)) return;
    struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
    poll(&pfd, 1, timeout_ms);
}

void plat_window_begin_drag(plat_window *pw, int edges)
{
    if (wm_moveresize) {
        static const int dirs[16] = {
            /* index = edge flags; value = _NET_WM_MOVERESIZE direction */
            8, 7, 3, -1, 1, 0, 2, -1, 5, 6, 4, -1, -1, -1, -1, -1,
        };
        int dir = dirs[edges & 15];
        if (dir < 0) return;
        XUngrabPointer(dpy, CurrentTime);
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.xclient.type = ClientMessage;
        ev.xclient.window = pw->win;
        ev.xclient.message_type = atoms[A_NET_WM_MOVERESIZE];
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = pw->root_x;
        ev.xclient.data.l[1] = pw->root_y;
        ev.xclient.data.l[2] = dir;
        ev.xclient.data.l[3] = Button1;
        ev.xclient.data.l[4] = 1;
        XSendEvent(dpy, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(dpy);
        return;
    }

    /* Manual fallback: grab the pointer and move/resize ourselves, but only
     * while the button is still held (it may already have been released). */
    Window r_root, r_child;
    int rx, ry, wx, wy;
    unsigned mask = 0;
    XQueryPointer(dpy, pw->win, &r_root, &r_child, &rx, &ry, &wx, &wy, &mask);
    if (!(mask & Button1Mask)) return;
    pw->root_x = rx;
    pw->root_y = ry;
    manual_drag *d = &pw->drag;
    d->active = true;
    d->edges = edges;
    d->start_rx = pw->root_x;
    d->start_ry = pw->root_y;
    window_origin(pw, &d->x, &d->y);
    d->w = pw->w;
    d->h = pw->h;
    XGrabPointer(dpy, pw->win, False, PointerMotionMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);
    while (d->active) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        handle(pw, &ev);
        if (pw->resized && pw->refresh && !XPending(dpy)) {
            pw->resized = false;
            pw->refresh(pw->refresh_ctx);
        }
    }
    XUngrabPointer(dpy, CurrentTime);
    XFlush(dpy);
}

/* ------------------------------------------------------------------ */
/* Clipboard                                                           */
/* ------------------------------------------------------------------ */

void plat_clipboard_set(plat_window *pw, const char *text)
{
    free(pw->clip);
    pw->clip = strdup(text ? text : "");
    XSetSelectionOwner(dpy, atoms[A_CLIPBOARD], pw->win, CurrentTime);
    XFlush(dpy);
}

char *plat_clipboard_get(plat_window *pw)
{
    Window owner = XGetSelectionOwner(dpy, atoms[A_CLIPBOARD]);
    if (owner == pw->win) return pw->clip ? strdup(pw->clip) : NULL;
    if (owner == None) return NULL;
    XConvertSelection(dpy, atoms[A_CLIPBOARD], atoms[A_UTF8_STRING], atoms[A_CGUI_CLIP], pw->win, CurrentTime);
    XFlush(dpy);
    XEvent ev;
    double deadline = plat_time() + 1.0;
    for (;;) {
        if (XCheckTypedWindowEvent(dpy, pw->win, SelectionNotify, &ev)) break;
        if (plat_time() > deadline) return NULL;
        struct pollfd pfd = { ConnectionNumber(dpy), POLLIN, 0 };
        poll(&pfd, 1, 10);
    }
    if (ev.xselection.property == None) return NULL;
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    char *out = NULL;
    if (XGetWindowProperty(dpy, pw->win, atoms[A_CGUI_CLIP], 0, LONG_MAX / 4, True, AnyPropertyType, &type,
                           &format, &n, &after, &data) == Success && data) {
        if (format == 8) {
            out = (char *)malloc(n + 1);
            memcpy(out, data, n);
            out[n] = 0;
        }
        XFree(data);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Font directories                                                    */
/* ------------------------------------------------------------------ */

static void add_dir(char **out, int *n, int max, const char *a, const char *b)
{
    if (*n >= max || !a || !*a) return;
    size_t len = strlen(a) + (b ? strlen(b) : 0) + 1;
    char *s = (char *)malloc(len);
    snprintf(s, len, "%s%s", a, b ? b : "");
    char *real = realpath(s, NULL);
    free(s);
    if (real) out[(*n)++] = real;
}

int plat_font_dirs(char **out, int max)
{
    int n = 0;
    const char *home = getenv("HOME");
    const char *xdg_home = getenv("XDG_DATA_HOME");
    if (xdg_home && *xdg_home) add_dir(out, &n, max, xdg_home, "/fonts");
    else add_dir(out, &n, max, home, "/.local/share/fonts");
    add_dir(out, &n, max, home, "/.fonts");
    const char *xdg = getenv("XDG_DATA_DIRS");
    if (!xdg || !*xdg) xdg = "/usr/local/share:/usr/share";
    char *dirs = strdup(xdg);
    for (char *tok = strtok(dirs, ":"); tok; tok = strtok(NULL, ":")) add_dir(out, &n, max, tok, "/fonts");
    free(dirs);
    add_dir(out, &n, max, "/usr/share/fonts", NULL);
    add_dir(out, &n, max, "/usr/local/share/fonts", NULL);
    add_dir(out, &n, max, "/usr/X11R6/lib/X11/fonts", NULL);
    return n;
}
