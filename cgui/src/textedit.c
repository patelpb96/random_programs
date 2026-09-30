/*
 * Multi-line text editor widget: greedy word wrap, caret and selection,
 * mouse selection (double-click = word, triple-click = paragraph),
 * keyboard navigation, clipboard, undo/redo, scrolling, and a read-only
 * mode used for text viewers.
 *
 * Layout is done in physical pixels so wrapping matches the rasterized
 * glyph advances exactly, and converted to logical units for drawing.
 */
#include "cg_internal.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_MAX 64

typedef struct tline { int start, end; } tline;
typedef struct snapshot { char *text; int len, caret, anchor; } snapshot;

enum { EDIT_NONE, EDIT_TYPE, EDIT_DELETE, EDIT_OTHER };

struct cg_textbuf_priv {
    tline *lines;
    int nlines, cap;
    bool valid;
    unsigned lay_version;
    int lay_width;
    float lay_px;
    cg_font *lay_font;

    snapshot undo[UNDO_MAX], redo[UNDO_MAX];
    int nundo, nredo;
    int last_kind;
    double last_edit;

    double blink_start;
    float goal_x;
    bool has_goal;
    bool bar_drag;
    float bar_offset;
};

/* ------------------------------------------------------------------ */
/* Buffer                                                              */
/* ------------------------------------------------------------------ */

static void reserve(cg_textbuf *tb, int n)
{
    if (n + 1 <= tb->cap) return;
    int cap = tb->cap ? tb->cap : 64;
    while (cap < n + 1) cap *= 2;
    tb->data = (char *)realloc(tb->data, (size_t)cap);
    tb->cap = cap;
}

static struct cg_textbuf_priv *priv(cg_textbuf *tb)
{
    if (!tb->priv) tb->priv = (struct cg_textbuf_priv *)calloc(1, sizeof *tb->priv);
    return tb->priv;
}

static int clamp_boundary(const cg_textbuf *tb, int pos)
{
    if (pos < 0) return 0;
    if (pos > tb->len) return tb->len;
    while (pos > 0 && ((unsigned char)tb->data[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

void cg_textbuf_init(cg_textbuf *tb, const char *text)
{
    memset(tb, 0, sizeof *tb);
    cg_textbuf_set(tb, text);
}

void cg_textbuf_set(cg_textbuf *tb, const char *text)
{
    if (!text) text = "";
    int n = (int)strlen(text);
    reserve(tb, n);
    memcpy(tb->data, text, (size_t)n + 1);
    tb->len = n;
    tb->caret = clamp_boundary(tb, tb->caret);
    tb->anchor = clamp_boundary(tb, tb->anchor);
    tb->version++;
}

static void free_snap(snapshot *s)
{
    free(s->text);
    s->text = NULL;
}

void cg_textbuf_free(cg_textbuf *tb)
{
    if (tb->priv) {
        for (int i = 0; i < tb->priv->nundo; i++) free_snap(&tb->priv->undo[i]);
        for (int i = 0; i < tb->priv->nredo; i++) free_snap(&tb->priv->redo[i]);
        free(tb->priv->lines);
        free(tb->priv);
    }
    free(tb->data);
    memset(tb, 0, sizeof *tb);
}

static void replace_range(cg_textbuf *tb, int a, int b, const char *s, int n)
{
    reserve(tb, tb->len - (b - a) + n);
    memmove(tb->data + a + n, tb->data + b, (size_t)(tb->len - b) + 1);
    memcpy(tb->data + a, s, (size_t)n);
    tb->len += n - (b - a);
    tb->caret = tb->anchor = a + n;
    tb->version++;
}

static int sel_min(const cg_textbuf *tb) { return tb->caret < tb->anchor ? tb->caret : tb->anchor; }
static int sel_max(const cg_textbuf *tb) { return tb->caret > tb->anchor ? tb->caret : tb->anchor; }

/* ---- undo -------------------------------------------------------- */

static snapshot take(const cg_textbuf *tb)
{
    snapshot s;
    s.text = (char *)malloc((size_t)tb->len + 1);
    memcpy(s.text, tb->data, (size_t)tb->len + 1);
    s.len = tb->len;
    s.caret = tb->caret;
    s.anchor = tb->anchor;
    return s;
}

static void push_snap(snapshot *stack, int *n, snapshot s)
{
    if (*n == UNDO_MAX) {
        free_snap(&stack[0]);
        memmove(stack, stack + 1, sizeof(snapshot) * (UNDO_MAX - 1));
        (*n)--;
    }
    stack[(*n)++] = s;
}

static void restore(cg_textbuf *tb, snapshot s)
{
    reserve(tb, s.len);
    memcpy(tb->data, s.text, (size_t)s.len + 1);
    tb->len = s.len;
    tb->caret = s.caret;
    tb->anchor = s.anchor;
    tb->version++;
    free(s.text);
}

static void begin_edit(cg_textbuf *tb, int kind)
{
    struct cg_textbuf_priv *p = priv(tb);
    double now = plat_time();
    if (kind != p->last_kind || kind == EDIT_OTHER || now - p->last_edit > 1.0) {
        push_snap(p->undo, &p->nundo, take(tb));
        for (int i = 0; i < p->nredo; i++) free_snap(&p->redo[i]);
        p->nredo = 0;
    }
    p->last_kind = kind;
    p->last_edit = now;
}

static bool undo(cg_textbuf *tb, bool redo)
{
    struct cg_textbuf_priv *p = priv(tb);
    snapshot *from = redo ? p->redo : p->undo, *to = redo ? p->undo : p->redo;
    int *nf = redo ? &p->nredo : &p->nundo, *nt = redo ? &p->nundo : &p->nredo;
    if (*nf == 0) return false;
    push_snap(to, nt, take(tb));
    restore(tb, from[--(*nf)]);
    p->last_kind = EDIT_NONE;
    return true;
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

static void push_line(struct cg_textbuf_priv *p, int start, int end)
{
    if (p->nlines == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 64;
        p->lines = (tline *)realloc(p->lines, sizeof(tline) * (size_t)p->cap);
    }
    p->lines[p->nlines].start = start;
    p->lines[p->nlines].end = end;
    p->nlines++;
}

static void layout(cg_textbuf *tb, cg_font *font, float px, int width)
{
    struct cg_textbuf_priv *p = priv(tb);
    if (p->valid && p->lay_version == tb->version && p->lay_width == width && p->lay_px == px &&
        p->lay_font == font)
        return;
    p->valid = true;
    p->lay_version = tb->version;
    p->lay_width = width;
    p->lay_px = px;
    p->lay_font = font;
    p->nlines = 0;

    const char *s = tb->data;
    int len = tb->len, pos = 0;
    int32_t maxw = (int32_t)width * 64;
    for (;;) {
        int ls = pos, brk = -1, end = -1, next = -1;
        font_run run;
        font_run_begin(&run, font, px);
        while (pos < len) {
            uint32_t cp;
            int n = utf8_decode(s + pos, len - pos, &cp);
            if (cp == '\n') {
                end = pos;
                next = pos + n;
                break;
            }
            font_run_step(&run, cp);
            if (cp != ' ' && run.pen26 > maxw && pos > ls) {
                end = next = brk > ls ? brk : pos;
                break;
            }
            pos += n;
            if (cp == ' ') brk = pos;
        }
        if (end < 0) {
            push_line(p, ls, len);
            break;
        }
        push_line(p, ls, end);
        pos = next;
    }
}

static int line_of(const cg_textbuf *tb, int pos)
{
    const struct cg_textbuf_priv *p = tb->priv;
    int lo = 0, hi = p->nlines - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (p->lines[mid].start <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static bool soft_wrapped(const cg_textbuf *tb, int li)
{
    const struct cg_textbuf_priv *p = tb->priv;
    return li + 1 < p->nlines && p->lines[li + 1].start == p->lines[li].end;
}

/* Physical x of `pos` within line `li`. */
static float x_of(const cg_textbuf *tb, int li, int pos)
{
    const tline *l = &tb->priv->lines[li];
    if (pos > l->end) pos = l->end;
    return font_measure(tb->priv->lay_font, tb->priv->lay_px, tb->data + l->start, pos - l->start);
}

/* Position nearest to physical x within line li. */
static int pos_at_x(const cg_textbuf *tb, int li, float x)
{
    const struct cg_textbuf_priv *p = tb->priv;
    const tline *l = &p->lines[li];
    font_run run;
    font_run_begin(&run, p->lay_font, p->lay_px);
    int pos = l->start;
    while (pos < l->end) {
        uint32_t cp;
        int n = utf8_decode(tb->data + pos, l->end - pos, &cp);
        int32_t before = run.pen26;
        font_run_step(&run, cp);
        if (x * 64.f < (before + run.pen26) * 0.5f) return pos;
        pos += n;
    }
    /* End of a soft-wrapped line is the start of the next one; stay here. */
    if (soft_wrapped(tb, li) && pos > l->start) pos = utf8_prev(tb->data, pos);
    return pos;
}

/* ------------------------------------------------------------------ */
/* Word helpers                                                        */
/* ------------------------------------------------------------------ */

static bool is_word(uint32_t cp) { return cp >= 0x80 || isalnum((int)cp) || cp == '_'; }

static uint32_t cp_at(const cg_textbuf *tb, int pos)
{
    uint32_t cp = 0;
    if (pos < tb->len) utf8_decode(tb->data + pos, tb->len - pos, &cp);
    return cp;
}

static int word_left(const cg_textbuf *tb, int pos)
{
    while (pos > 0 && !is_word(cp_at(tb, utf8_prev(tb->data, pos)))) pos = utf8_prev(tb->data, pos);
    while (pos > 0 && is_word(cp_at(tb, utf8_prev(tb->data, pos)))) pos = utf8_prev(tb->data, pos);
    return pos;
}

static int word_right(const cg_textbuf *tb, int pos)
{
    while (pos < tb->len && !is_word(cp_at(tb, pos))) pos = utf8_next(tb->data, tb->len, pos);
    while (pos < tb->len && is_word(cp_at(tb, pos))) pos = utf8_next(tb->data, tb->len, pos);
    return pos;
}

static void select_word(cg_textbuf *tb, int pos)
{
    bool w = is_word(cp_at(tb, pos));
    int a = pos, b = pos;
    while (a > 0 && is_word(cp_at(tb, utf8_prev(tb->data, a))) == w && cp_at(tb, utf8_prev(tb->data, a)) != '\n')
        a = utf8_prev(tb->data, a);
    while (b < tb->len && is_word(cp_at(tb, b)) == w && cp_at(tb, b) != '\n')
        b = utf8_next(tb->data, tb->len, b);
    tb->anchor = a;
    tb->caret = b;
}

static void select_paragraph(cg_textbuf *tb, int pos)
{
    int a = pos, b = pos;
    while (a > 0 && tb->data[a - 1] != '\n') a--;
    while (b < tb->len && tb->data[b] != '\n') b++;
    if (b < tb->len) b++;
    tb->anchor = a;
    tb->caret = b;
}

/* ------------------------------------------------------------------ */
/* Clipboard / insertion                                               */
/* ------------------------------------------------------------------ */

static void copy_selection(cg_window *w, cg_textbuf *tb)
{
    int a = sel_min(tb), b = sel_max(tb);
    if (a == b) return;
    char *s = (char *)malloc((size_t)(b - a) + 1);
    memcpy(s, tb->data + a, (size_t)(b - a));
    s[b - a] = 0;
    cg_clipboard_set(w, s);
    free(s);
}

static void insert_text(cg_textbuf *tb, const char *s, int n, int kind)
{
    /* Drop carriage returns and other control characters except \n. */
    char *clean = (char *)malloc((size_t)n * 4 + 1);
    int m = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\t') {
            memcpy(clean + m, "    ", 4);
            m += 4;
        } else if (c >= 32 || c == '\n') {
            clean[m++] = (char)c;
        }
    }
    if (m > 0 || sel_min(tb) != sel_max(tb)) {
        begin_edit(tb, kind);
        replace_range(tb, sel_min(tb), sel_max(tb), clean, m);
    }
    free(clean);
}

/* ------------------------------------------------------------------ */
/* The widget                                                          */
/* ------------------------------------------------------------------ */

bool cg_textedit(cg_window *w, cg_id id, cg_rect r, cg_textbuf *tb, cg_font *font, float size, int flags)
{
    struct cg_textbuf_priv *p = priv(tb);
    const cg_theme *t = &w->style.theme;
    bool readonly = (flags & CG_TEXT_READONLY) != 0;
    if (!font) font = w->ui_font;
    if (!tb->data) cg_textbuf_set(tb, "");
    unsigned start_version = tb->version;
    float s = w->scale;

    float pad = 10, bar_w = 6;
    cg_rect inner = cg_inset(r, pad);
    inner.w -= bar_w + 4;
    float px = size * s;
    float asc_p, lh_p;
    font_metrics(font, px, &asc_p, NULL, &lh_p);
    float lh = lh_p / s;

    layout(tb, font, px, (int)fmaxf(1.f, inner.w * s));

    float content_h = p->nlines * lh;
    float max_scroll = fmaxf(0, content_h - inner.h);
    bool reveal = false;

    /* ---- mouse ---- */
    ui_behavior b = ui_behave(w, id, r, true);
    bool focused = w->focus == id;
    cg_rect bar = cg_rect_make(r.x + r.w - pad * 0.5f - bar_w, inner.y, bar_w, inner.h);
    float thumb_h = max_scroll > 0 ? fmaxf(24, inner.h * inner.h / content_h) : 0;
    if (b.pressed && max_scroll > 0 && w->in.mx >= bar.x - 4) {
        float ty = bar.y + (bar.h - thumb_h) * (tb->scroll / max_scroll);
        p->bar_drag = true;
        p->bar_offset = (w->in.my >= ty && w->in.my < ty + thumb_h) ? w->in.my - ty : thumb_h * 0.5f;
    }
    if (p->bar_drag) {
        if (!b.down) p->bar_drag = false;
        else if (bar.h > thumb_h)
            tb->scroll = (w->in.my - p->bar_offset - bar.y) / (bar.h - thumb_h) * max_scroll;
    } else if (b.down) {
        float my = w->in.my;
        /* Auto-scroll while dragging a selection past the edges. */
        if (!b.pressed && my < inner.y) {
            tb->scroll -= (inner.y - my) * 0.3f + 1;
            cg_request_wakeup(w, 1.0 / 60);
        } else if (!b.pressed && my > inner.y + inner.h) {
            tb->scroll += (my - inner.y - inner.h) * 0.3f + 1;
            cg_request_wakeup(w, 1.0 / 60);
        }
        tb->scroll = cg_clampf(tb->scroll, 0, max_scroll);
        int li = (int)floorf((my - inner.y + tb->scroll) / lh);
        li = li < 0 ? 0 : li >= p->nlines ? p->nlines - 1 : li;
        int pos = my < inner.y - lh && tb->scroll <= 0 ? 0 : pos_at_x(tb, li, (w->in.mx - inner.x) * s);
        if (b.pressed) {
            if (w->in.clicks == 2) {
                select_word(tb, pos);
            } else if (w->in.clicks == 3) {
                select_paragraph(tb, pos);
            } else {
                tb->caret = pos;
                if (!(w->in.mods & CG_MOD_SHIFT)) tb->anchor = pos;
            }
        } else if (w->in.clicks < 2) {
            tb->caret = pos;
        }
        p->has_goal = false;
        p->last_kind = EDIT_NONE;
        p->blink_start = plat_time();
    }
    if (b.hover && !p->bar_drag && w->in.mx < bar.x - 4) w->cursor = CURSOR_IBEAM;
    if (ui_hover(w, r) && w->in.wheel_y != 0) {
        tb->scroll -= w->in.wheel_y * lh * 3;
        w->in.wheel_y = 0;
    }

    /* ---- keyboard ---- */
    if (focused) {
#ifdef __APPLE__
        const int word_mod = CG_MOD_ALT;
#else
        const int word_mod = CG_MOD_CTRL;
#endif
        for (int k = 0; k < w->in.nkeys; k++) {
            int key = w->in.keys[k], m = w->in.key_mods[k];
            bool shift = (m & CG_MOD_SHIFT) != 0, word = (m & word_mod) != 0;
            bool sc = (m & CG_MOD_SHORTCUT) != 0;
            bool has_sel = tb->caret != tb->anchor;
            bool moved = true, vertical = false;
            layout(tb, font, px, p->lay_width);
            int li = line_of(tb, tb->caret);
#ifdef __APPLE__
            if (sc && key == CG_KEY_LEFT) key = CG_KEY_HOME, sc = false;
            else if (sc && key == CG_KEY_RIGHT) key = CG_KEY_END, sc = false;
            else if (sc && key == CG_KEY_UP) key = CG_KEY_HOME;
            else if (sc && key == CG_KEY_DOWN) key = CG_KEY_END;
#endif
            switch (key) {
            case CG_KEY_LEFT:
                if (has_sel && !shift) tb->caret = sel_min(tb);
                else tb->caret = word ? word_left(tb, tb->caret) : utf8_prev(tb->data, tb->caret);
                break;
            case CG_KEY_RIGHT:
                if (has_sel && !shift) tb->caret = sel_max(tb);
                else tb->caret = word ? word_right(tb, tb->caret) : utf8_next(tb->data, tb->len, tb->caret);
                break;
            case CG_KEY_UP:
            case CG_KEY_DOWN:
            case CG_KEY_PAGE_UP:
            case CG_KEY_PAGE_DOWN: {
                int step = (key == CG_KEY_PAGE_UP || key == CG_KEY_PAGE_DOWN) ? cg_maxi(1, (int)(inner.h / lh) - 1) : 1;
                int dir = (key == CG_KEY_UP || key == CG_KEY_PAGE_UP) ? -1 : 1;
                if (!p->has_goal) {
                    p->goal_x = x_of(tb, li, tb->caret);
                    p->has_goal = true;
                }
                int nl = li + dir * step;
                if (nl < 0) tb->caret = 0;
                else if (nl >= p->nlines) tb->caret = tb->len;
                else tb->caret = pos_at_x(tb, nl, p->goal_x);
                if (key == CG_KEY_PAGE_UP || key == CG_KEY_PAGE_DOWN) tb->scroll += dir * step * lh;
                vertical = true;
                break;
            }
            case CG_KEY_HOME:
                tb->caret = sc ? 0 : p->lines[li].start;
                break;
            case CG_KEY_END:
                tb->caret = sc ? tb->len : p->lines[li].end;
                if (!sc && soft_wrapped(tb, li) && tb->caret > p->lines[li].start)
                    tb->caret = utf8_prev(tb->data, tb->caret);
                break;
            case 'A':
                if (sc) { tb->anchor = 0; tb->caret = tb->len; shift = true; }
                else moved = false;
                break;
            default:
                moved = false;
            }
            if (moved) {
                if (!shift) tb->anchor = tb->caret;
                if (!vertical) p->has_goal = false;
                p->last_kind = EDIT_NONE;
                reveal = true;
                continue;
            }
            if (sc && (key == 'C' || key == 'X')) {
                copy_selection(w, tb);
                if (key == 'X' && !readonly && has_sel) {
                    begin_edit(tb, EDIT_OTHER);
                    replace_range(tb, sel_min(tb), sel_max(tb), "", 0);
                }
                reveal = true;
                continue;
            }
            if (readonly) continue;
            if (sc && key == 'V') {
                char *clip = cg_clipboard_get(w);
                if (clip) {
                    insert_text(tb, clip, (int)strlen(clip), EDIT_OTHER);
                    free(clip);
                }
            } else if (sc && (key == 'Z' || key == 'Y')) {
                undo(tb, key == 'Y' || shift);
            } else if (key == CG_KEY_BACKSPACE || key == CG_KEY_DELETE) {
                int a = sel_min(tb), e = sel_max(tb);
                if (a == e) {
                    if (key == CG_KEY_BACKSPACE) a = word ? word_left(tb, a) : utf8_prev(tb->data, a);
                    else e = word ? word_right(tb, e) : utf8_next(tb->data, tb->len, e);
                }
                if (a != e) {
                    begin_edit(tb, EDIT_DELETE);
                    replace_range(tb, a, e, "", 0);
                }
            } else if (key == CG_KEY_ENTER) {
                insert_text(tb, "\n", 1, EDIT_OTHER);
            } else if (key == CG_KEY_TAB) {
                insert_text(tb, "    ", 4, EDIT_TYPE);
            } else {
                continue;
            }
            p->has_goal = false;
            reveal = true;
        }
        if (!readonly && w->in.text_len > 0) {
            insert_text(tb, w->in.text, w->in.text_len, EDIT_TYPE);
            w->in.text_len = 0;
            p->has_goal = false;
            reveal = true;
        }
        if (reveal) p->blink_start = plat_time();
    }

    /* ---- relayout after edits, keep the caret in view ---- */
    layout(tb, font, px, p->lay_width);
    content_h = p->nlines * lh;
    max_scroll = fmaxf(0, content_h - inner.h);
    if (reveal) {
        float cy = line_of(tb, tb->caret) * lh;
        if (cy < tb->scroll) tb->scroll = cy;
        if (cy + lh > tb->scroll + inner.h) tb->scroll = cy + lh - inner.h;
    }
    tb->scroll = cg_clampf(tb->scroll, 0, max_scroll);

    /* ---- draw ---- */
    cg_fill_rrect(w, r, 8, t->panel_bg);
    cg_stroke_rrect(w, r, 8, 1, focused ? cg_color_alpha(t->accent, 0.8f) : t->panel_border);

    cg_push_clip(w, cg_rect_make(inner.x - 2, inner.y, inner.w + 4, inner.h));
    int first = cg_maxi(0, (int)(tb->scroll / lh));
    int a = sel_min(tb), e = sel_max(tb);
    pm_color text_pm = pm_from(t->text, 1.f);
    float space_w = font_measure(font, px, " ", 1) / s;
    for (int li = first; li < p->nlines; li++) {
        float y = inner.y + li * lh - tb->scroll;
        if (y > inner.y + inner.h) break;
        const tline *l = &p->lines[li];
        if (a != e && a <= l->end && e > l->start) {
            float x0 = x_of(tb, li, a > l->start ? a : l->start) / s;
            float x1 = x_of(tb, li, e < l->end ? e : l->end) / s;
            if (e > l->end && !soft_wrapped(tb, li)) x1 += space_w * 0.6f;
            cg_color sel = focused ? t->selection : cg_color_alpha(t->selection, 0.6f);
            cg_fill_rect(w, cg_rect_make(inner.x + x0, y, x1 - x0, lh), sel);
        }
        font_draw(&w->cv, font, px, inner.x * s, floorf(y * s + 0.5f) + asc_p, tb->data + l->start,
                  l->end - l->start, text_pm);
    }
    if (tb->len == 0 && tb->placeholder)
        cg_draw_text(w, w->ui_font, w->style.font_size, inner.x, inner.y + (lh - w->style.font_size * 1.2f) * 0.5f,
                     tb->placeholder, -1, t->text_dim);

    if (focused && !readonly) {
        double el = plat_time() - p->blink_start;
        double phase = fmod(el, 1.06);
        if (phase < 0.53) {
            int li = line_of(tb, tb->caret);
            float cx = inner.x + x_of(tb, li, tb->caret) / s;
            float cy = inner.y + li * lh - tb->scroll;
            cg_fill_rect(w, cg_rect_make(floorf(cx), cy, fmaxf(1.f, 1.5f), lh), t->accent);
        }
        cg_request_wakeup(w, 0.53 - fmod(el, 0.53) + 0.001);
    }
    cg_pop_clip(w);

    if (max_scroll > 0) {
        thumb_h = fmaxf(24, inner.h * inner.h / content_h);
        float ty = bar.y + (bar.h - thumb_h) * (tb->scroll / max_scroll);
        cg_fill_rrect(w, cg_rect_make(bar.x, ty, bar.w, thumb_h), 3, t->scrollbar);
    }
    return tb->version != start_version;
}
