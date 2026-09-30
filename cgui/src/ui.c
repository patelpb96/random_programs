/*
 * Immediate-mode widget core: hot/active/focus tracking, drawing wrappers
 * in logical units, layout helpers, and the stock widgets. The text editor
 * lives in textedit.c.
 */
#include "cg_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

cg_id cg_id_str(const char *s)
{
    uint64_t h = 1469598103934665603ull; /* FNV-1a */
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

cg_id cg_id_ptr(const void *p) { return (cg_id)(uintptr_t)p * 0x9E3779B97F4A7C15ull | 1; }

cg_color cg_color_alpha(cg_color c, float a)
{
    c.a = (uint8_t)(c.a * cg_clampf(a, 0.f, 1.f) + 0.5f);
    return c;
}

cg_color cg_color_mix(cg_color a, cg_color b, float t)
{
    cg_color c;
    c.r = (uint8_t)(a.r + (b.r - a.r) * t);
    c.g = (uint8_t)(a.g + (b.g - a.g) * t);
    c.b = (uint8_t)(a.b + (b.b - a.b) * t);
    c.a = (uint8_t)(a.a + (b.a - a.a) * t);
    return c;
}

cg_rect cg_cut_left(cg_rect *r, float a)
{
    a = cg_clampf(a, 0, r->w);
    cg_rect out = { r->x, r->y, a, r->h };
    r->x += a;
    r->w -= a;
    return out;
}

cg_rect cg_cut_right(cg_rect *r, float a)
{
    a = cg_clampf(a, 0, r->w);
    r->w -= a;
    cg_rect out = { r->x + r->w, r->y, a, r->h };
    return out;
}

cg_rect cg_cut_top(cg_rect *r, float a)
{
    a = cg_clampf(a, 0, r->h);
    cg_rect out = { r->x, r->y, r->w, a };
    r->y += a;
    r->h -= a;
    return out;
}

cg_rect cg_cut_bottom(cg_rect *r, float a)
{
    a = cg_clampf(a, 0, r->h);
    r->h -= a;
    cg_rect out = { r->x, r->y + r->h, r->w, a };
    return out;
}

cg_rect cg_inset(cg_rect r, float d)
{
    cg_rect o = { r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d };
    if (o.w < 0) o.w = 0;
    if (o.h < 0) o.h = 0;
    return o;
}

bool cg_rect_contains(cg_rect r, float x, float y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static cg_rect rect_intersect(cg_rect a, cg_rect b)
{
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y);
    float x1 = fminf(a.x + a.w, b.x + b.w), y1 = fminf(a.y + a.h, b.y + b.h);
    return cg_rect_make(x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0));
}

/* ------------------------------------------------------------------ */
/* Drawing wrappers                                                    */
/* ------------------------------------------------------------------ */

static void apply_clip(cg_window *w)
{
    if (w->nclips == 0) {
        canvas_set_clip(&w->cv, 0, 0, w->cv.w, w->cv.h);
        return;
    }
    cg_rect r = w->clips[w->nclips - 1];
    float s = w->scale;
    canvas_set_clip(&w->cv, (int)floorf(r.x * s + 0.5f), (int)floorf(r.y * s + 0.5f),
                    (int)floorf((r.x + r.w) * s + 0.5f), (int)floorf((r.y + r.h) * s + 0.5f));
}

void cg_push_clip(cg_window *w, cg_rect r)
{
    if (w->nclips > 0) r = rect_intersect(r, w->clips[w->nclips - 1]);
    if (w->nclips < CG_CLIP_STACK) w->clips[w->nclips++] = r;
    apply_clip(w);
}

void cg_pop_clip(cg_window *w)
{
    if (w->nclips > 0) w->nclips--;
    apply_clip(w);
}

void cg_fill_rect(cg_window *w, cg_rect r, cg_color c)
{
    float s = w->scale;
    canvas_fill_rect(&w->cv, r.x * s, r.y * s, (r.x + r.w) * s, (r.y + r.h) * s, pm_from(c, 1.f));
}

void cg_fill_rrect(cg_window *w, cg_rect r, float radius, cg_color c)
{
    float s = w->scale, rr[4] = { radius * s, radius * s, radius * s, radius * s };
    canvas_fill_rrect(&w->cv, r.x * s, r.y * s, r.w * s, r.h * s, rr, pm_from(c, 1.f));
}

void cg_stroke_rrect(cg_window *w, cg_rect r, float radius, float t, cg_color c)
{
    float s = w->scale, rr[4] = { radius * s, radius * s, radius * s, radius * s };
    canvas_stroke_rrect(&w->cv, r.x * s, r.y * s, r.w * s, r.h * s, rr, t * s, pm_from(c, 1.f));
}

void cg_fill_circle(cg_window *w, float cx, float cy, float r, cg_color c)
{
    float s = w->scale;
    canvas_fill_circle(&w->cv, cx * s, cy * s, r * s, pm_from(c, 1.f));
}

void cg_stroke_circle(cg_window *w, float cx, float cy, float r, float t, cg_color c)
{
    float s = w->scale;
    canvas_stroke_circle(&w->cv, cx * s, cy * s, r * s, t * s, pm_from(c, 1.f));
}

void cg_line(cg_window *w, float x0, float y0, float x1, float y1, float t, cg_color c)
{
    float s = w->scale;
    canvas_line(&w->cv, x0 * s, y0 * s, x1 * s, y1 * s, t * s, pm_from(c, 1.f));
}

void ui_text_in(cg_window *w, cg_rect r, const char *s, int align, cg_color c, float size)
{
    if (!s || !w->ui_font) return;
    float lh, tw = cg_text_width(w, w->ui_font, size, s, -1);
    cg_font_metrics(w, w->ui_font, size, NULL, NULL, &lh);
    float x = r.x;
    if (align == CG_ALIGN_CENTER) x = r.x + (r.w - tw) * 0.5f;
    else if (align == CG_ALIGN_RIGHT) x = r.x + r.w - tw;
    cg_push_clip(w, r);
    cg_draw_text(w, w->ui_font, size, floorf(x + 0.5f), floorf(r.y + (r.h - lh) * 0.5f + 0.5f), s, -1, c);
    cg_pop_clip(w);
}

/* ------------------------------------------------------------------ */
/* Input queries and interaction                                       */
/* ------------------------------------------------------------------ */

void cg_mouse_pos(cg_window *w, float *x, float *y)
{
    if (x) *x = w->in.mx;
    if (y) *y = w->in.my;
}
bool cg_mouse_down(cg_window *w, int b) { return b >= 0 && b < 3 && w->in.down[b]; }
bool cg_mouse_pressed(cg_window *w, int b) { return b >= 0 && b < 3 && w->in.pressed[b]; }
bool cg_mouse_released(cg_window *w, int b) { return b >= 0 && b < 3 && w->in.released[b]; }

bool cg_key_pressed(cg_window *w, int key, int mods)
{
    for (int i = 0; i < w->in.nkeys; i++)
        if (w->in.keys[i] == key && w->in.key_mods[i] == mods) return true;
    return false;
}

bool ui_hover(cg_window *w, cg_rect r)
{
    if (!w->in.inside || w->block_mouse) return false;
    if (w->nclips > 0) r = rect_intersect(r, w->clips[w->nclips - 1]);
    if (!cg_rect_contains(r, w->in.mx, w->in.my)) return false;
    if (w->overlay_valid && cg_rect_contains(w->overlay, w->in.mx, w->in.my)) return false;
    return true;
}

bool cg_mouse_in(cg_window *w, cg_rect r) { return ui_hover(w, r); }

ui_behavior ui_behave(cg_window *w, cg_id id, cg_rect r, bool focusable)
{
    ui_behavior b = { 0 };
    bool over = ui_hover(w, r);
    if (over && (w->active == 0 || w->active == id)) {
        w->hot = id;
        b.hover = true;
    }
    if (b.hover && w->in.pressed[0] && w->active == 0) {
        w->active = id;
        w->press_taken = true;
        b.pressed = true;
        if (focusable) {
            w->focus = id;
            w->focus_taken = true;
        }
        w->redraw = true;
    }
    if (w->active == id) {
        w->active_seen = true;
        b.down = true;
        if (!w->in.down[0] || w->in.released[0]) {
            w->active = 0;
            b.released = true;
            b.clicked = over;
            w->redraw = true;
        }
    }
    return b;
}

void ui_begin_frame(cg_window *w)
{
    w->hot = 0;
    w->active_seen = false;
    w->focus_taken = false;
    w->press_taken = false;
    w->popup.registered = false;
    w->overlay_valid = w->popup.open;
    w->overlay = w->popup.rect;

    /* A click outside an open popup closes it and is swallowed. */
    if (w->popup.open && w->in.pressed[0] && !cg_rect_contains(w->popup.rect, w->in.mx, w->in.my) &&
        !cg_rect_contains(w->popup.anchor, w->in.mx, w->in.my)) {
        w->popup.open = false;
        w->overlay_valid = false;
        w->in.pressed[0] = false;
        if (w->focus == w->popup.id) w->focus = 0;
    }
}

void ui_end_frame(cg_window *w)
{
    if (w->active && !w->active_seen) w->active = 0;
    if (w->in.pressed[0] && !w->focus_taken && !(w->popup.open && w->focus == w->popup.id))
        w->focus = 0;
}

static const cg_theme *th(cg_window *w) { return &w->style.theme; }
static float fs(cg_window *w) { return w->style.font_size; }

/* ------------------------------------------------------------------ */
/* Basic widgets                                                       */
/* ------------------------------------------------------------------ */

void cg_label(cg_window *w, cg_rect r, const char *text, int align, cg_color c)
{
    ui_text_in(w, r, text, align, c.a ? c : th(w)->text, fs(w));
}

bool cg_button(cg_window *w, cg_id id, cg_rect r, const char *label)
{
    ui_behavior b = ui_behave(w, id, r, false);
    const cg_theme *t = th(w);
    cg_color bg = b.down && b.hover ? t->control_active : b.hover ? t->control_hover : t->control_bg;
    cg_fill_rrect(w, r, 6, bg);
    ui_text_in(w, r, label, CG_ALIGN_CENTER, t->text, fs(w));
    if (b.hover) w->cursor = CURSOR_HAND;
    return b.clicked;
}

bool cg_checkbox(cg_window *w, cg_id id, cg_rect r, const char *label, bool *value)
{
    ui_behavior b = ui_behave(w, id, r, false);
    const cg_theme *t = th(w);
    float bs = 16;
    cg_rect box = cg_rect_make(r.x, r.y + (r.h - bs) * 0.5f, bs, bs);
    if (b.clicked) *value = !*value;
    if (*value) {
        cg_fill_rrect(w, box, 4, t->accent);
        float x = box.x, y = box.y;
        cg_line(w, x + 4, y + 8.5f, x + 7, y + 11.5f, 2, t->accent_text);
        cg_line(w, x + 7, y + 11.5f, x + 12.5f, y + 5, 2, t->accent_text);
    } else {
        cg_fill_rrect(w, box, 4, b.hover ? t->control_hover : t->control_bg);
        cg_stroke_rrect(w, box, 4, 1, t->panel_border);
    }
    cg_rect lr = r;
    cg_cut_left(&lr, bs + 6);
    ui_text_in(w, lr, label, CG_ALIGN_LEFT, t->text, fs(w));
    if (b.hover) w->cursor = CURSOR_HAND;
    return b.clicked;
}

bool cg_slider(cg_window *w, cg_id id, cg_rect r, float *value, float min, float max)
{
    ui_behavior b = ui_behave(w, id, r, false);
    const cg_theme *t = th(w);
    float knob = 7, x0 = r.x + knob, x1 = r.x + r.w - knob;
    float old = *value;
    if (b.down && x1 > x0) *value = min + (max - min) * cg_clampf((w->in.mx - x0) / (x1 - x0), 0, 1);
    if (b.hover && w->in.wheel_y != 0) {
        *value += (max - min) / 50.f * w->in.wheel_y;
        w->in.wheel_y = 0;
    }
    *value = cg_clampf(*value, min, max);
    float f = max > min ? (*value - min) / (max - min) : 0;
    float cy = r.y + r.h * 0.5f, kx = x0 + (x1 - x0) * f;
    cg_fill_rrect(w, cg_rect_make(x0, cy - 2, x1 - x0, 4), 2, t->control_active);
    cg_fill_rrect(w, cg_rect_make(x0, cy - 2, kx - x0, 4), 2, t->accent);
    cg_fill_circle(w, kx, cy, knob + (b.hover || b.down ? 1 : 0), t->accent);
    cg_fill_circle(w, kx, cy, knob - 3, t->accent_text);
    if (b.hover) w->cursor = CURSOR_HAND;
    return *value != old;
}

bool cg_swatch(cg_window *w, cg_id id, cg_rect r, cg_color c, bool selected)
{
    ui_behavior b = ui_behave(w, id, r, false);
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f, rad = fminf(r.w, r.h) * 0.5f - 2;
    if (selected || b.hover)
        cg_stroke_circle(w, cx, cy, rad + 1, selected ? 2 : 1, th(w)->text);
    cg_fill_circle(w, cx, cy, rad - (selected ? 2 : 1), c);
    if (b.hover) w->cursor = CURSOR_HAND;
    return b.clicked;
}

bool cg_spinbox(cg_window *w, cg_id id, cg_rect r, float *value, float min, float max, float step,
                const char *fmt)
{
    const cg_theme *t = th(w);
    float old = *value;
    cg_rect body = r;
    cg_rect minus = cg_cut_left(&body, 24), plus = cg_cut_right(&body, 24);
    cg_fill_rrect(w, r, 6, t->control_bg);

    /* -/+ buttons with auto-repeat while held. */
    for (int k = 0; k < 2; k++) {
        cg_rect br = k ? plus : minus;
        ui_behavior b = ui_behave(w, id + 1 + (cg_id)k, br, false);
        double now = plat_time();
        if (b.pressed) {
            *value += k ? step : -step;
            w->repeat_at = now + 0.4;
        } else if (b.down && b.hover && now >= w->repeat_at) {
            *value += k ? step : -step;
            w->repeat_at = now + 0.05;
        }
        if (b.down) cg_request_wakeup(w, fmax(0.0, w->repeat_at - now));
        if (b.hover) {
            cg_fill_rrect(w, cg_inset(br, 2), 5, b.down ? t->control_active : t->control_hover);
            w->cursor = CURSOR_HAND;
        }
        float cx = br.x + br.w * 0.5f, cy = br.y + br.h * 0.5f;
        cg_line(w, cx - 4, cy, cx + 4, cy, 1.6f, t->text);
        if (k) cg_line(w, cx, cy - 4, cx, cy + 4, 1.6f, t->text);
    }

    /* Drag the number horizontally to scrub; the wheel steps it. */
    ui_behavior b = ui_behave(w, id, body, false);
    if (b.pressed) {
        w->drag_anchor = w->in.mx;
        w->drag_value = *value;
    }
    if (b.down) *value = w->drag_value + roundf((w->in.mx - w->drag_anchor) / 4.f) * step;
    if (b.hover || b.down) w->cursor = CURSOR_RESIZE_EW;
    if (ui_hover(w, r) && w->in.wheel_y != 0) {
        *value += step * (w->in.wheel_y > 0 ? 1 : -1);
        w->in.wheel_y = 0;
    }
    *value = cg_clampf(*value, min, max);
    char buf[64];
    snprintf(buf, sizeof buf, fmt ? fmt : "%.0f", *value);
    ui_text_in(w, body, buf, CG_ALIGN_CENTER, t->text, fs(w));
    return *value != old;
}

/* ------------------------------------------------------------------ */
/* Dropdown and its popup                                              */
/* ------------------------------------------------------------------ */

static void draw_chevron(cg_window *w, float cx, float cy, bool up, cg_color c)
{
    float d = up ? -1.f : 1.f;
    cg_line(w, cx - 4, cy - 2 * d, cx, cy + 2 * d, 1.6f, c);
    cg_line(w, cx, cy + 2 * d, cx + 4, cy - 2 * d, 1.6f, c);
}

bool cg_dropdown(cg_window *w, cg_id id, cg_rect r, const cg_list_source *src, int *selected)
{
    cg_popup *p = &w->popup;
    const cg_theme *t = th(w);
    bool changed = false;
    if (p->result_id == id && p->result >= 0) {
        changed = *selected != p->result;
        *selected = p->result;
        p->result_id = 0;
        p->result = -1;
    }
    bool open = p->open && p->id == id;
    ui_behavior b = ui_behave(w, id, r, false);
    if (b.pressed) {
        if (open) {
            p->open = false;
            if (w->focus == id) w->focus = 0;
        } else {
            p->open = true;
            p->id = id;
            p->filter_len = 0;
            p->filter[0] = 0;
            p->highlight = *selected;
            p->scroll = 0;
            p->reveal = true;
            p->rect = cg_rect_make(0, 0, 0, 0);
            w->focus = id;
        }
        w->focus_taken = true;
        open = p->open;
    }
    if (open) {
        p->registered = true;
        p->src = *src;
        p->anchor = r;
        p->selected = *selected;
    }
    cg_color bg = open ? t->control_active : b.hover ? t->control_hover : t->control_bg;
    cg_fill_rrect(w, r, 6, bg);
    if (open) cg_stroke_rrect(w, r, 6, 1, t->accent);
    cg_rect lr = cg_inset(r, 0);
    lr.x += 10;
    lr.w -= 34;
    const char *label = *selected >= 0 && *selected < src->count ? src->label(src->user, *selected) : "—";
    ui_text_in(w, lr, label, CG_ALIGN_LEFT, t->text, fs(w));
    draw_chevron(w, r.x + r.w - 15, r.y + r.h * 0.5f, open, t->text_dim);
    if (b.hover) w->cursor = CURSOR_HAND;
    return changed;
}

static bool ci_contains(const char *hay, const char *needle)
{
    if (!*needle) return true;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) h++, n++;
        if (!*n) return true;
    }
    return false;
}

static void popup_filter(cg_popup *p)
{
    if (p->cap < p->src.count) {
        p->cap = p->src.count;
        p->matches = (int *)realloc(p->matches, sizeof(int) * (size_t)(p->cap ? p->cap : 1));
    }
    p->nmatches = 0;
    for (int i = 0; i < p->src.count; i++) {
        if (p->filter_len == 0 || ci_contains(p->src.label(p->src.user, i), p->filter))
            p->matches[p->nmatches++] = i;
    }
}

/* Processes and draws the open popup. Runs after the user's frame so it
 * paints on top; widgets underneath were blocked via w->overlay. */
void ui_popup_end(cg_window *w)
{
    cg_popup *p = &w->popup;
    if (!p->open) return;
    if (!p->registered) {
        p->open = false;
        return;
    }
    const cg_theme *t = th(w);
    cg_input *in = &w->in;
    popup_filter(p);

    /* Keyboard: typing filters, arrows move, Enter picks, Esc closes. */
    int hl_pos = -1;
    for (int i = 0; i < p->nmatches; i++)
        if (p->matches[i] == p->highlight) hl_pos = i;
    bool keyboard_moved = false;
    bool filter_changed = false;
    if (w->focus == p->id) {
        for (int k = 0; k < in->nkeys; k++) {
            int key = in->keys[k];
            int page = 10;
            if (key == CG_KEY_ESCAPE) {
                p->open = false;
                w->focus = 0;
                w->redraw = true;
                return;
            } else if (key == CG_KEY_DOWN) hl_pos++, keyboard_moved = true;
            else if (key == CG_KEY_UP) hl_pos--, keyboard_moved = true;
            else if (key == CG_KEY_PAGE_DOWN) hl_pos += page, keyboard_moved = true;
            else if (key == CG_KEY_PAGE_UP) hl_pos -= page, keyboard_moved = true;
            else if (key == CG_KEY_HOME && p->filter_len == 0) hl_pos = 0, keyboard_moved = true;
            else if (key == CG_KEY_END && p->filter_len == 0) hl_pos = p->nmatches - 1, keyboard_moved = true;
            else if (key == CG_KEY_BACKSPACE && p->filter_len > 0 && p->src.filterable) {
                p->filter_len = utf8_prev(p->filter, p->filter_len);
                p->filter[p->filter_len] = 0;
                filter_changed = true;
            } else if (key == CG_KEY_ENTER && hl_pos >= 0 && hl_pos < p->nmatches) {
                p->result_id = p->id;
                p->result = p->matches[hl_pos];
                p->open = false;
                w->focus = 0;
                w->redraw = true;
                return;
            }
        }
        if (in->text_len > 0 && p->src.filterable) {
            int n = cg_mini(in->text_len, (int)sizeof p->filter - 1 - p->filter_len);
            memcpy(p->filter + p->filter_len, in->text, (size_t)n);
            p->filter_len += n;
            p->filter[p->filter_len] = 0;
            in->text_len = 0;
            filter_changed = true;
        }
    }
    if (filter_changed) {
        popup_filter(p);
        hl_pos = p->nmatches > 0 ? 0 : -1;
        p->scroll = 0;
        keyboard_moved = true;
    }
    if (hl_pos >= p->nmatches) hl_pos = p->nmatches - 1;
    if (hl_pos < 0 && p->nmatches > 0 && keyboard_moved) hl_pos = 0;

    /* Geometry. */
    float item_h = 26, pad = 4;
    float search_h = p->src.filterable ? 32 : 0;
    int rows = cg_mini(cg_maxi(p->nmatches, 1), 12);
    float want_h = search_h + rows * item_h + 2 * pad;
    float width = fmaxf(p->anchor.w, 240);
    float x = fminf(p->anchor.x, w->w - width - 8);
    float below = w->h - (p->anchor.y + p->anchor.h) - 10;
    float above = p->anchor.y - w->style.title_height - 6;
    float y, h;
    if (below >= want_h || below >= above) {
        h = fminf(want_h, below);
        y = p->anchor.y + p->anchor.h + 4;
    } else {
        h = fminf(want_h, above);
        y = p->anchor.y - 4 - h;
    }
    p->rect = cg_rect_make(x, y, width, h);
    cg_rect list = cg_inset(p->rect, pad);
    cg_rect search = cg_cut_top(&list, search_h);

    /* Scrolling. */
    float content_h = p->nmatches * item_h;
    float max_scroll = fmaxf(0, content_h - list.h);
    if (cg_rect_contains(p->rect, in->mx, in->my) && in->wheel_y != 0) {
        p->scroll -= in->wheel_y * item_h * 3;
        in->wheel_y = 0;
    }
    if ((p->reveal || keyboard_moved) && hl_pos >= 0) {
        float iy = hl_pos * item_h;
        if (p->reveal) p->scroll = iy - (list.h - item_h) * 0.5f;
        else if (iy < p->scroll) p->scroll = iy;
        else if (iy + item_h > p->scroll + list.h) p->scroll = iy + item_h - list.h;
        p->reveal = false;
    }

    /* Scrollbar thumb dragging. */
    bool has_bar = max_scroll > 0;
    cg_rect bar = cg_rect_make(list.x + list.w - 6, list.y, 6, list.h);
    float thumb_h = has_bar ? fmaxf(24, list.h * list.h / content_h) : 0;
    if (has_bar) {
        cg_rect hit = cg_rect_make(bar.x - 4, bar.y, bar.w + 4, bar.h);
        if (in->pressed[0] && cg_rect_contains(hit, in->mx, in->my)) {
            float ty = list.y + (list.h - thumb_h) * (p->scroll / max_scroll);
            p->dragging_thumb = true;
            p->drag_offset = (in->my >= ty && in->my < ty + thumb_h) ? in->my - ty : thumb_h * 0.5f;
            in->pressed[0] = false;
        }
        if (p->dragging_thumb) {
            if (!in->down[0]) p->dragging_thumb = false;
            else if (list.h > thumb_h)
                p->scroll = (in->my - p->drag_offset - list.y) / (list.h - thumb_h) * max_scroll;
        }
    }
    p->scroll = cg_clampf(p->scroll, 0, max_scroll);

    /* Mouse over rows. */
    int hover_pos = -1;
    if (!p->dragging_thumb && cg_rect_contains(list, in->mx, in->my) &&
        !(has_bar && in->mx >= bar.x - 4)) {
        hover_pos = (int)((in->my - list.y + p->scroll) / item_h);
        if (hover_pos >= p->nmatches) hover_pos = -1;
    }
    if (hover_pos >= 0 && !keyboard_moved) hl_pos = hover_pos;
    if (hl_pos >= 0 && hl_pos < p->nmatches) p->highlight = p->matches[hl_pos];
    if (in->pressed[0] && cg_rect_contains(p->rect, in->mx, in->my)) {
        w->focus_taken = true; /* keep popup focus */
        if (hover_pos >= 0) {
            p->result_id = p->id;
            p->result = p->matches[hover_pos];
            p->open = false;
            w->focus = 0;
            w->redraw = true;
        }
        in->pressed[0] = false;
    }

    /* Draw: soft shadow, body, search field, rows, scrollbar. */
    for (int i = 3; i >= 1; i--)
        cg_fill_rrect(w, cg_rect_make(p->rect.x - i, p->rect.y - i + 3, p->rect.w + 2 * i, p->rect.h + 2 * i),
                      8 + i, cg_rgba(0, 0, 0, 22));
    cg_color body = t->panel_bg;
    body.a = 255;
    cg_fill_rrect(w, p->rect, 8, body);
    cg_stroke_rrect(w, p->rect, 8, 1, t->panel_border);

    if (search_h > 0) {
        cg_rect sf = cg_inset(search, 2);
        sf.h -= 4;
        cg_fill_rrect(w, sf, 5, t->control_bg);
        cg_rect tr = sf;
        tr.x += 26;
        tr.w -= 30;
        cg_stroke_circle(w, sf.x + 12, sf.y + sf.h * 0.5f - 1, 4.5f, 1.5f, t->text_dim);
        cg_line(w, sf.x + 15, sf.y + sf.h * 0.5f + 2, sf.x + 18.5f, sf.y + sf.h * 0.5f + 5.5f, 1.5f, t->text_dim);
        if (p->filter_len > 0) {
            ui_text_in(w, tr, p->filter, CG_ALIGN_LEFT, t->text, fs(w));
            float cx = tr.x + cg_text_width(w, w->ui_font, fs(w), p->filter, p->filter_len) + 1;
            cg_fill_rect(w, cg_rect_make(cx, sf.y + 6, 1.5f, sf.h - 12), t->accent);
        } else {
            ui_text_in(w, tr, "Type to filter…", CG_ALIGN_LEFT, t->text_dim, fs(w));
        }
        char count[32];
        snprintf(count, sizeof count, "%d", p->nmatches);
        cg_rect cr = sf;
        cr.w -= 8;
        ui_text_in(w, cr, count, CG_ALIGN_RIGHT, t->text_dim, fs(w) - 2);
    }

    cg_push_clip(w, list);
    if (p->nmatches == 0) {
        ui_text_in(w, cg_rect_make(list.x, list.y, list.w, item_h), "No matches", CG_ALIGN_CENTER,
                   t->text_dim, fs(w));
    }
    int first = (int)(p->scroll / item_h);
    for (int i = first; i < p->nmatches; i++) {
        float iy = list.y + i * item_h - p->scroll;
        if (iy > list.y + list.h) break;
        int idx = p->matches[i];
        cg_rect row = cg_rect_make(list.x, iy, list.w - (has_bar ? 10 : 0), item_h);
        if (i == hl_pos) cg_fill_rrect(w, row, 5, t->control_hover);
        if (idx == p->selected) cg_fill_rrect(w, cg_rect_make(row.x + 2, row.y + 6, 3, item_h - 12), 1.5f, t->accent);
        cg_rect tr = row;
        tr.x += 12;
        tr.w -= 16;
        cg_font *pf = p->src.preview_font ? p->src.preview_font(p->src.user, idx) : NULL;
        float preview_w = 0;
        if (pf && p->src.preview_text) {
            float size = fs(w) + 2, lh;
            cg_font_metrics(w, pf, size, NULL, NULL, &lh);
            preview_w = fminf(cg_text_width(w, pf, size, p->src.preview_text, -1), tr.w * 0.45f);
            cg_rect pr = cg_rect_make(tr.x + tr.w - preview_w, row.y, preview_w, row.h);
            cg_push_clip(w, pr);
            cg_draw_text(w, pf, size, pr.x, floorf(row.y + (row.h - lh) * 0.5f), p->src.preview_text, -1,
                         t->text_dim);
            cg_pop_clip(w);
            tr.w -= preview_w + 10;
        }
        ui_text_in(w, tr, p->src.label(p->src.user, idx), CG_ALIGN_LEFT, idx == p->selected ? t->accent : t->text,
                   fs(w));
    }
    cg_pop_clip(w);
    if (has_bar) {
        float ty = list.y + (list.h - thumb_h) * (p->scroll / max_scroll);
        cg_fill_rrect(w, cg_rect_make(bar.x, ty, bar.w, thumb_h), 3, t->scrollbar);
    }
}
