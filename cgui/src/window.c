/*
 * Window lifecycle, the event loop, and the custom window chrome: title bar,
 * resize zones, and the three window buttons laid out along the curve of the
 * top-right corner.
 */
#include "cg_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Creation                                                            */
/* ------------------------------------------------------------------ */

static bool g_plat_ready;

cg_window *cg_window_create(const char *title, int width, int height)
{
    if (!g_plat_ready) {
        if (!plat_init()) return NULL;
        g_plat_ready = true;
    }
    cg_window *w = (cg_window *)calloc(1, sizeof *w);
    w->title = strdup(title ? title : "");
    w->min_w = 320;
    w->min_h = 200;
    w->style.theme = *cg_theme_get(0);
    w->style.frame_opacity = 0.92f;
    w->style.body_opacity = 0.97f;
    w->style.rounded = true;
    w->style.corner_radius = 12.f;
    w->style.title_height = 54.f;
    w->style.border_width = 1.f;
    w->style.resize_margin = 5.f;
    w->style.font_size = 14.f;
    w->pending_drag = -1;
    w->armed_drag = -1;
    w->cursor_shown = -1;
    w->popup.result = -1;

    w->pw = plat_window_create(w->title, width, height);
    if (!w->pw) {
        free(w->title);
        free(w);
        return NULL;
    }
    w->scale = plat_window_scale(w->pw);
    plat_window_size(w->pw, &w->pw_w, &w->pw_h);
    plat_window_set_min_size(w->pw, (int)(w->min_w * w->scale), (int)(w->min_h * w->scale));

    const char *dump = getenv("CGUI_SCREENSHOT");
    w->debug_dump = dump && *dump;

    int idx = 0;
    const char *path = fontdb_default_ui_font_path(&idx);
    w->ui_font = cg_font_load(path, idx);
    if (!w->ui_font) fprintf(stderr, "cgui: no usable UI font found; set CGUI_FONT\n");
    return w;
}

void cg_window_destroy(cg_window *w)
{
    if (!w) return;
    plat_window_destroy(w->pw);
    cg_font_free(w->ui_font);
    canvas_free(&w->cv);
    free(w->popup.matches);
    free(w->icon);
    free(w->title);
    free(w);
}

void cg_window_set_title(cg_window *w, const char *title)
{
    free(w->title);
    w->title = strdup(title ? title : "");
    plat_window_set_title(w->pw, w->title);
    w->dirty = true;
}

void cg_window_set_icon(cg_window *w, const uint32_t *argb, int iw, int ih)
{
    free(w->icon);
    w->icon = NULL;
    w->icon_w = w->icon_h = 0;
    if (argb && iw > 0 && ih > 0) {
        w->icon = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)iw * ih);
        memcpy(w->icon, argb, sizeof(uint32_t) * (size_t)iw * ih);
        w->icon_w = iw;
        w->icon_h = ih;
        plat_window_set_icon(w->pw, argb, iw, ih);
    }
    w->dirty = true;
}

void cg_window_set_min_size(cg_window *w, int width, int height)
{
    w->min_w = width;
    w->min_h = height;
    plat_window_set_min_size(w->pw, (int)(width * w->scale), (int)(height * w->scale));
}

cg_style *cg_window_style(cg_window *w) { return &w->style; }
float cg_window_scale(cg_window *w) { return w->scale; }
bool cg_window_maximized(cg_window *w) { return w->maximized; }
cg_font *cg_ui_font(cg_window *w) { return w->ui_font; }
double cg_time(void) { return plat_time(); }
void cg_quit(cg_window *w) { w->running = false; }
void cg_request_redraw(cg_window *w) { w->redraw = true; }

void cg_window_size(cg_window *w, float *width, float *height)
{
    if (width) *width = w->w;
    if (height) *height = w->h;
}

void cg_request_wakeup(cg_window *w, double seconds)
{
    double t = plat_time() + (seconds > 0 ? seconds : 0);
    if (w->wake_at <= 0 || t < w->wake_at) w->wake_at = t;
}

void cg_clipboard_set(cg_window *w, const char *text) { plat_clipboard_set(w->pw, text); }
char *cg_clipboard_get(cg_window *w) { return plat_clipboard_get(w->pw); }

/* ------------------------------------------------------------------ */
/* Chrome geometry                                                     */
/* ------------------------------------------------------------------ */

typedef struct chrome_geo {
    float T;          /* title height == top-right corner radius */
    float cx, cy;     /* centre of the top-right corner arc */
    float arc_r;      /* radius of the arc the buttons sit on */
    float btn_r;      /* button radius */
    float angle[3];   /* minimize, maximize, close (degrees, 90 = straight up) */
    float radii[4];   /* window corner radii: tl, tr, br, bl */
} chrome_geo;

static chrome_geo geometry(cg_window *w)
{
    chrome_geo g;
    const cg_style *s = &w->style;
    g.T = fmaxf(s->title_height, 28.f);
    g.cx = w->w - g.T;
    g.cy = g.T;
    g.arc_r = g.T * 0.68f;
    g.btn_r = g.T * 0.15f;
    g.angle[0] = 78.f;
    g.angle[1] = 45.f;
    g.angle[2] = 12.f;
    bool round = s->rounded && !w->maximized;
    float r = round ? s->corner_radius : 0.f;
    g.radii[0] = r;
    g.radii[1] = round ? g.T : 0.f;
    g.radii[2] = r;
    g.radii[3] = r;
    return g;
}

static void button_center(const chrome_geo *g, int i, float *x, float *y)
{
    const float deg = 3.14159265f / 180.f;
    *x = g->cx + g->arc_r * cosf(g->angle[i] * deg);
    *y = g->cy - g->arc_r * sinf(g->angle[i] * deg);
}

cg_rect chrome_content_rect(cg_window *w)
{
    chrome_geo g = geometry(w);
    float b = w->maximized ? 0.f : w->style.border_width;
    return cg_rect_make(b, g.T, w->w - 2 * b, w->h - g.T - b);
}

/* Which edges would a press at (x, y) resize? 0 = none. */
int chrome_hit_edges(cg_window *w, float x, float y)
{
    if (w->maximized) return 0;
    chrome_geo g = geometry(w);
    float m = w->style.resize_margin;
    float W = w->w, H = w->h;
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;

    /* The big rounded top-right corner: follow its curve. */
    if (g.radii[1] > m && x > g.cx && y < g.cy) {
        float dx = x - g.cx, dy = g.cy - y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < g.radii[1] - m) return 0;
        float ang = atan2f(dy, dx) * 180.f / 3.14159265f;
        if (ang > 67.5f) return EDGE_TOP;
        if (ang < 22.5f) return EDGE_RIGHT;
        return EDGE_TOP | EDGE_RIGHT;
    }
    float corner = fmaxf(m * 3.f, fmaxf(g.radii[0], g.radii[2]) * 0.8f);
    int e = 0;
    if (x < m) e |= EDGE_LEFT;
    if (x >= W - m) e |= EDGE_RIGHT;
    if (y < m) e |= EDGE_TOP;
    if (y >= H - m) e |= EDGE_BOTTOM;
    if (!e) return 0;
    /* Widen the diagonal grab near corners. */
    if ((e & (EDGE_LEFT | EDGE_RIGHT)) && y < corner) e |= EDGE_TOP;
    if ((e & (EDGE_LEFT | EDGE_RIGHT)) && y >= H - corner) e |= EDGE_BOTTOM;
    if ((e & (EDGE_TOP | EDGE_BOTTOM)) && x < corner) e |= EDGE_LEFT;
    if ((e & (EDGE_TOP | EDGE_BOTTOM)) && x >= W - corner) e |= EDGE_RIGHT;
    return e;
}

static int edge_cursor(int e)
{
    switch (e) {
    case EDGE_LEFT: case EDGE_RIGHT: return CURSOR_RESIZE_EW;
    case EDGE_TOP: case EDGE_BOTTOM: return CURSOR_RESIZE_NS;
    case EDGE_LEFT | EDGE_TOP: case EDGE_RIGHT | EDGE_BOTTOM: return CURSOR_RESIZE_NWSE;
    case EDGE_RIGHT | EDGE_TOP: case EDGE_LEFT | EDGE_BOTTOM: return CURSOR_RESIZE_NESW;
    default: return CURSOR_ARROW;
    }
}

/* ------------------------------------------------------------------ */
/* Chrome drawing                                                      */
/* ------------------------------------------------------------------ */

static void scaled_radii(const chrome_geo *g, float s, float out[4])
{
    for (int i = 0; i < 4; i++) out[i] = g->radii[i] * s;
}

static void draw_button_icon(cg_window *w, int kind, float x, float y, float r, cg_color c)
{
    float s = w->scale, t = fmaxf(1.4f, r * 0.2f) * s, k = r * 0.42f;
    pm_color p = pm_from(c, 1.f);
    x *= s;
    y *= s;
    k *= s;
    if (kind == 0) {
        canvas_line(&w->cv, x - k, y, x + k, y, t, p);
    } else if (kind == 1) {
        float rr[4] = { t * 0.6f, t * 0.6f, t * 0.6f, t * 0.6f };
        if (w->maximized) {
            float o = k * 0.35f;
            canvas_stroke_rrect(&w->cv, x - k + o, y - k, 2 * k - o, 2 * k - o, rr, t * 0.8f, p);
            canvas_stroke_rrect(&w->cv, x - k, y - k + o, 2 * k - o, 2 * k - o, rr, t * 0.8f, p);
        } else {
            canvas_stroke_rrect(&w->cv, x - k, y - k, 2 * k, 2 * k, rr, t, p);
        }
    } else {
        canvas_line(&w->cv, x - k, y - k, x + k, y + k, t, p);
        canvas_line(&w->cv, x - k, y + k, x + k, y - k, t, p);
    }
}

void chrome_draw(cg_window *w)
{
    const cg_style *st = &w->style;
    const cg_theme *th = &st->theme;
    chrome_geo g = geometry(w);
    float s = w->scale, W = w->w * s, H = w->h * s, T = g.T * s;
    /* Without real per-pixel alpha, translucent pixels would just look dark. */
    float frame_op = w->alpha_ok ? st->frame_opacity : 1.f;
    float body_op = w->alpha_ok ? st->body_opacity : 1.f;
    float radii[4];
    scaled_radii(&g, s, radii);
    cg_canvas *cv = &w->cv;

    /* Title bar: the window shape clipped to the top band. */
    canvas_set_clip(cv, 0, 0, cv->w, (int)ceilf(T));
    canvas_fill_rrect(cv, 0, 0, W, H, radii, pm_from(th->frame_bg, frame_op));
    /* Body: the same shape clipped below it. */
    canvas_set_clip(cv, 0, (int)ceilf(T), cv->w, cv->h);
    canvas_fill_rrect(cv, 0, 0, W, H, radii, pm_from(th->body_bg, body_op));
    canvas_set_clip(cv, 0, 0, cv->w, cv->h);

    /* Separator between title bar and body (stops short of the corner). */
    canvas_fill_rect(cv, 0, T - fmaxf(1.f, s), g.cx * s - g.arc_r * 0.3f * s, T,
                     pm_from(th->frame_border, frame_op * 0.6f));

    /* Outline. */
    if (!w->maximized && st->border_width > 0)
        canvas_stroke_rrect(cv, 0, 0, W, H, radii, st->border_width * s,
                            pm_from(th->frame_border, frame_op));

    /* Title text. */
    if (w->ui_font && w->title) {
        float fs = st->font_size + 1.f;
        float lh;
        cg_font_metrics(w, w->ui_font, fs, NULL, NULL, &lh);
        float x = 16.f + (w->maximized ? 0.f : st->corner_radius * 0.4f);
        if (w->icon) {
            /* Whole physical pixels per icon pixel keeps pixel art crisp. */
            float target = g.T * 0.46f * s;
            float k = fmaxf(1.f, floorf(target / (float)cg_maxi(w->icon_w, w->icon_h)));
            float iw = w->icon_w * k / s, ih = w->icon_h * k / s;
            float iy = floorf((g.T - ih) * 0.5f * s) / s;
            cg_draw_image(w, cg_rect_make(floorf(x * s) / s, iy, iw, ih), w->icon, w->icon_w, w->icon_h);
            x += iw + 10.f;
        }
        cg_push_clip(w, cg_rect_make(0, 0, g.cx - g.arc_r - g.btn_r, g.T));
        cg_draw_text(w, w->ui_font, fs, x, (g.T - lh) * 0.5f, w->title, -1, th->title_text);
        cg_pop_clip(w);
    }

    /* Curved track behind the buttons, concentric with the corner. */
    canvas_arc(cv, g.cx * s, g.cy * s, g.arc_r * s, g.angle[2] - 7.f, g.angle[0] + 7.f,
               (g.btn_r + g.T * 0.07f) * s, pm_from(th->button_track, fmaxf(frame_op, 0.5f)));

    /* The buttons themselves. */
    static const char *ids[3] = { "cg.minimize", "cg.maximize", "cg.close" };
    const cg_color hover_col[3] = { th->minimize, th->maximize, th->close };
    for (int i = 0; i < 3; i++) {
        float bx, by;
        button_center(&g, i, &bx, &by);
        float r = g.btn_r;
        cg_rect hit = cg_rect_make(bx - r - 2, by - r - 2, 2 * r + 4, 2 * r + 4);
        cg_id id = cg_id_str(ids[i]);
        ui_behavior b = ui_behave(w, id, hit, false);
        cg_color fill = b.hover || b.down ? hover_col[i] : th->button_idle;
        if (b.down && b.hover) fill = cg_color_mix(fill, cg_rgb(0, 0, 0), 0.18f);
        canvas_fill_circle(cv, bx * s, by * s, r * s, pm_from(fill, 1.f));
        cg_color icon = b.hover || b.down ? cg_rgb(255, 255, 255) : th->button_icon;
        if (b.hover && i == 0) icon = cg_rgb(60, 40, 0);
        draw_button_icon(w, i, bx, by, r, icon);
        if (b.clicked) w->pending_op = i == 0 ? OP_MINIMIZE : i == 1 ? OP_MAXIMIZE : OP_CLOSE;
    }
}

/* ------------------------------------------------------------------ */
/* Frame execution                                                     */
/* ------------------------------------------------------------------ */

static void clear_transient_input(cg_input *in)
{
    for (int i = 0; i < 3; i++) in->pressed[i] = in->released[i] = false;
    in->wheel_x = in->wheel_y = 0;
    in->nkeys = 0;
    in->text_len = 0;
    in->text[0] = 0;
}

static void update_shape_serial(cg_window *w)
{
    float key[6] = { (float)w->pw_w, (float)w->pw_h, w->style.rounded ? 1.f : 0.f,
                     w->style.corner_radius, w->style.title_height, w->maximized ? 1.f : 0.f };
    if (memcmp(key, w->shape_key, sizeof key) != 0) {
        memcpy(w->shape_key, key, sizeof key);
        w->shape_serial++;
    }
}

static void do_frame(cg_window *w)
{
    plat_window_size(w->pw, &w->pw_w, &w->pw_h);
    if (w->pw_w < 1 || w->pw_h < 1) return;
    w->scale = plat_window_scale(w->pw);
    w->w = w->pw_w / w->scale;
    w->h = w->pw_h / w->scale;
    w->maximized = plat_window_is_maximized(w->pw);
    w->alpha_ok = plat_window_has_alpha(w->pw) || w->debug_dump;
    if (w->cv.w != w->pw_w || w->cv.h != w->pw_h) canvas_resize(&w->cv, w->pw_w, w->pw_h);
    canvas_clear(&w->cv, 0);
    w->in_frame = true;
    w->nclips = 0;
    w->cursor = CURSOR_ARROW;

    /* Resize zones take priority over everything else. */
    w->hover_edge = w->in.inside && w->active == 0 ? chrome_hit_edges(w, w->in.mx, w->in.my) : 0;
    w->block_mouse = w->hover_edge != 0;
    if (w->hover_edge) {
        w->cursor = edge_cursor(w->hover_edge);
        if (w->in.pressed[0]) {
            w->armed_drag = w->hover_edge;
            w->armed_x = w->in.mx;
            w->armed_y = w->in.my;
            w->in.pressed[0] = false;
        }
    }

    ui_begin_frame(w);
    chrome_draw(w);
    if (w->frame_fn) {
        cg_rect content = chrome_content_rect(w);
        cg_push_clip(w, content);
        w->frame_fn(w, content, w->user);
        w->nclips = 0;
        canvas_set_clip(&w->cv, 0, 0, w->cv.w, w->cv.h);
    }
    ui_popup_end(w);

    /* A press on bare title bar starts a window move / double-click maximize. */
    if (w->in.pressed[0] && !w->press_taken && !w->block_mouse && w->in.my < chrome_content_rect(w).y &&
        !(w->overlay_valid && cg_rect_contains(w->overlay, w->in.mx, w->in.my))) {
        if (w->in.clicks == 2) {
            w->pending_op = OP_MAXIMIZE;
        } else {
            w->armed_drag = 0;
            w->armed_x = w->in.mx;
            w->armed_y = w->in.my;
        }
    }
    /* Only hand the pointer to the window manager once it actually moves, so
     * plain clicks and double-clicks on the title bar are never swallowed. */
    if (w->armed_drag >= 0) {
        float dx = w->in.mx - w->armed_x, dy = w->in.my - w->armed_y;
        if (!w->in.down[0]) {
            w->armed_drag = -1;
        } else if (dx * dx + dy * dy >= 9.f) {
            w->pending_drag = w->armed_drag;
            w->armed_drag = -1;
        } else if (w->armed_drag > 0) {
            w->cursor = edge_cursor(w->armed_drag);
        }
    }
    ui_end_frame(w);
    w->in_frame = false;

    if (w->cursor != w->cursor_shown) {
        plat_window_set_cursor(w->pw, w->cursor);
        w->cursor_shown = w->cursor;
    }
    update_shape_serial(w);
}

static void debug_dump(cg_window *w);

static void run_frames(cg_window *w)
{
    /* Immediate-mode UIs sometimes need a second pass to settle (e.g. a
     * dropdown that just closed); transient input is only seen once. */
    for (int pass = 0; pass < 3; pass++) {
        w->redraw = false;
        do_frame(w);
        clear_transient_input(&w->in);
        if (!w->redraw) break;
    }
    w->dirty = false;
}

static void present(cg_window *w)
{
    if (w->cv.px && w->cv.w == w->pw_w && w->cv.h == w->pw_h)
        plat_window_present(w->pw, w->cv.px, w->cv.w, w->cv.h, w->shape_serial);
    debug_dump(w);
}

static void apply_event(cg_window *w, const plat_event *e)
{
    cg_input *in = &w->in;
    float s = w->scale > 0 ? w->scale : 1.f;
    switch (e->type) {
    case PE_MOUSE_MOVE:
        in->mx = e->x / s;
        in->my = e->y / s;
        in->inside = true;
        break;
    case PE_MOUSE_LEAVE:
        if (!in->down[0]) {
            in->inside = false;
            in->mx = in->my = -1e6f;
        }
        break;
    case PE_MOUSE_DOWN:
    case PE_MOUSE_UP: {
        if (e->button < 0 || e->button > 2) break;
        in->mx = e->x / s;
        in->my = e->y / s;
        in->inside = true;
        bool down = e->type == PE_MOUSE_DOWN;
        /* Both edges within one batch: run a frame so neither gets lost. */
        if ((down && (in->pressed[e->button] || in->released[e->button])) ||
            (!down && in->released[e->button]))
            run_frames(w);
        in->down[e->button] = down;
        if (down) {
            in->pressed[e->button] = true;
            if (e->button == 0) {
                double now = plat_time();
                float dx = in->mx - w->last_click_x, dy = in->my - w->last_click_y;
                if (now - w->last_click_time < 0.4 && dx * dx + dy * dy < 25.f)
                    w->click_count = w->click_count % 3 + 1;
                else
                    w->click_count = 1;
                w->last_click_time = now;
                w->last_click_x = in->mx;
                w->last_click_y = in->my;
                in->clicks = w->click_count;
            }
        } else {
            in->released[e->button] = true;
        }
        in->mods = e->mods;
        break;
    }
    case PE_WHEEL:
        in->wheel_x += e->wheel_x;
        in->wheel_y += e->wheel_y;
        in->mods = e->mods;
        break;
    case PE_KEY_DOWN:
        in->mods = e->mods;
        if (in->nkeys < CG_MAX_KEYS) {
            in->keys[in->nkeys] = e->key;
            in->key_mods[in->nkeys] = e->mods;
            in->nkeys++;
        }
        break;
    case PE_TEXT: {
        int n = (int)strlen(e->text);
        if (in->text_len + n < (int)sizeof in->text) {
            memcpy(in->text + in->text_len, e->text, (size_t)n + 1);
            in->text_len += n;
        }
        break;
    }
    case PE_CLOSE:
        w->pending_op = OP_CLOSE;
        break;
    case PE_UNFOCUS:
        for (int i = 0; i < 3; i++) in->down[i] = false;
        in->mods = 0;
        break;
    default:
        break;
    }
    w->dirty = true;
}

static void pump_events(cg_window *w)
{
    plat_event e;
    while (plat_poll_event(w->pw, &e)) apply_event(w, &e);
}

static void do_pending(cg_window *w)
{
    int op = w->pending_op;
    int drag = w->pending_drag;
    w->pending_op = OP_NONE;
    w->pending_drag = -1;
    if (drag >= 0) {
        /* The platform (or window manager) owns the pointer from here on. */
        w->in.down[0] = false;
        w->active = 0;
        plat_window_begin_drag(w->pw, drag);
        w->dirty = true;
    }
    switch (op) {
    case OP_MINIMIZE: plat_window_minimize(w->pw); break;
    case OP_MAXIMIZE:
        plat_window_toggle_maximize(w->pw);
        w->in.inside = false;
        w->dirty = true;
        break;
    case OP_CLOSE: w->running = false; break;
    default: break;
    }
}

/* Called by backends from inside modal move/resize loops. */
static void refresh_cb(void *ctx)
{
    cg_window *w = (cg_window *)ctx;
    if (w->in_frame) return;
    pump_events(w);
    run_frames(w);
    present(w);
}

int cg_run(cg_window *w, cg_frame_fn frame, void *user)
{
    w->frame_fn = frame;
    w->user = user;
    w->running = true;
    w->dirty = true;
    plat_window_set_refresh(w->pw, refresh_cb, w);
    while (w->running) {
        if (!w->dirty) {
            int timeout = -1;
            if (w->wake_at > 0) {
                double dt = w->wake_at - plat_time();
                timeout = dt <= 0 ? 0 : (int)(dt * 1000.0) + 1;
            }
            plat_wait_events(w->pw, timeout);
        }
        pump_events(w);
        if (w->wake_at > 0 && plat_time() >= w->wake_at) {
            w->wake_at = 0;
            w->dirty = true;
        }
        if (w->dirty) {
            run_frames(w);
            present(w);
        }
        do_pending(w);
    }
    plat_window_set_refresh(w->pw, NULL, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Debug: CGUI_SCREENSHOT=file.ppm writes the composited frame over a  */
/* checkerboard (to visualise transparency) and exits.                 */
/* ------------------------------------------------------------------ */

static void debug_dump(cg_window *w)
{
    static int frames;
    if (!w->debug_dump) return;
    const char *path = getenv("CGUI_SCREENSHOT");
    const char *delay = getenv("CGUI_SCREENSHOT_FRAME");
    if (++frames < (delay ? atoi(delay) : 2)) {
        w->dirty = true;
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w->cv.w, w->cv.h);
        for (int y = 0; y < w->cv.h; y++) {
            for (int x = 0; x < w->cv.w; x++) {
                uint32_t p = w->cv.px[y * w->cv.w + x];
                unsigned a = p >> 24;
                unsigned bg = ((x / 12 + y / 12) & 1) ? 200 : 150;
                unsigned char rgb[3];
                for (int k = 0; k < 3; k++) {
                    unsigned c = (p >> (16 - 8 * k)) & 255;
                    rgb[k] = (unsigned char)(c + bg * (255 - a) / 255);
                }
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
    }
    w->running = false;
}
