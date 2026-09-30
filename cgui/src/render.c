/*
 * Software rasterizer. Shapes are anti-aliased by evaluating a signed
 * distance function at each pixel centre and turning the distance into
 * coverage (clamp(0.5 - d, 0, 1)). Pixels that are certainly fully inside a
 * shape are filled as solid spans, so large fills stay cheap.
 *
 * Pixels are premultiplied 0xAARRGGBB, which is what X11 ARGB visuals,
 * Win32 layered windows and CoreGraphics all expect.
 */
#include "cg_internal.h"

#include <stdlib.h>
#include <string.h>

static inline uint32_t mul255(uint32_t a, uint32_t b)
{
    uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

pm_color pm_from(cg_color c, float opacity)
{
    pm_color p;
    uint32_t a = (uint32_t)(c.a * cg_clampf(opacity, 0.f, 1.f) + 0.5f);
    p.a = a;
    p.r = mul255(c.r, a);
    p.g = mul255(c.g, a);
    p.b = mul255(c.b, a);
    return p;
}

static inline void blend(uint32_t *d, pm_color c, uint32_t cov)
{
    uint32_t a = c.a, r = c.r, g = c.g, b = c.b;
    if (cov < 255) {
        if (cov == 0) return;
        a = mul255(a, cov);
        r = mul255(r, cov);
        g = mul255(g, cov);
        b = mul255(b, cov);
    }
    if (a == 255) {
        *d = 0xFF000000u | (r << 16) | (g << 8) | b;
        return;
    }
    if (a == 0) return;
    uint32_t dv = *d, ia = 255 - a;
    uint32_t da = a + mul255(dv >> 24, ia);
    uint32_t dr = r + mul255((dv >> 16) & 255, ia);
    uint32_t dg = g + mul255((dv >> 8) & 255, ia);
    uint32_t db = b + mul255(dv & 255, ia);
    *d = (da << 24) | (dr << 16) | (dg << 8) | db;
}

static void fill_span(cg_canvas *cv, int y, int x0, int x1, pm_color c, uint32_t cov)
{
    if (y < cv->cy0 || y >= cv->cy1) return;
    if (x0 < cv->cx0) x0 = cv->cx0;
    if (x1 > cv->cx1) x1 = cv->cx1;
    if (x0 >= x1 || cov == 0) return;
    uint32_t *p = cv->px + (size_t)y * cv->w;
    if (cov == 255 && c.a == 255) {
        uint32_t v = 0xFF000000u | (c.r << 16) | (c.g << 8) | c.b;
        for (int x = x0; x < x1; x++) p[x] = v;
    } else {
        for (int x = x0; x < x1; x++) blend(&p[x], c, cov);
    }
}

static inline uint32_t cov_from_dist(float d)
{
    float c = 0.5f - d;
    if (c <= 0.f) return 0;
    if (c >= 1.f) return 255;
    return (uint32_t)(c * 255.f + 0.5f);
}

void canvas_resize(cg_canvas *cv, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w * h > cv->cap) {
        free(cv->px);
        cv->cap = w * h;
        cv->px = (uint32_t *)malloc((size_t)cv->cap * 4);
    }
    cv->w = w;
    cv->h = h;
    canvas_set_clip(cv, 0, 0, w, h);
}

void canvas_free(cg_canvas *cv)
{
    free(cv->px);
    memset(cv, 0, sizeof *cv);
}

void canvas_clear(cg_canvas *cv, uint32_t value)
{
    size_t n = (size_t)cv->w * cv->h;
    if (value == 0) {
        memset(cv->px, 0, n * 4);
    } else {
        for (size_t i = 0; i < n; i++) cv->px[i] = value;
    }
}

void canvas_set_clip(cg_canvas *cv, int x0, int y0, int x1, int y1)
{
    cv->cx0 = cg_maxi(0, x0);
    cv->cy0 = cg_maxi(0, y0);
    cv->cx1 = cg_mini(cv->w, x1);
    cv->cy1 = cg_mini(cv->h, y1);
    if (cv->cx1 < cv->cx0) cv->cx1 = cv->cx0;
    if (cv->cy1 < cv->cy0) cv->cy1 = cv->cy0;
}

/* Clip a float bounding box to integer pixel rows/cols. */
static bool bbox(cg_canvas *cv, float x0, float y0, float x1, float y1,
                 int *ix0, int *iy0, int *ix1, int *iy1)
{
    *ix0 = cg_maxi(cv->cx0, (int)floorf(x0));
    *iy0 = cg_maxi(cv->cy0, (int)floorf(y0));
    *ix1 = cg_mini(cv->cx1, (int)ceilf(x1));
    *iy1 = cg_mini(cv->cy1, (int)ceilf(y1));
    return *ix0 < *ix1 && *iy0 < *iy1;
}

void canvas_fill_rect(cg_canvas *cv, float x0, float y0, float x1, float y1, pm_color c)
{
    /* Snap to the pixel grid: rectangles are used for crisp UI fills. */
    int ix0 = (int)floorf(x0 + 0.5f), iy0 = (int)floorf(y0 + 0.5f);
    int ix1 = (int)floorf(x1 + 0.5f), iy1 = (int)floorf(y1 + 0.5f);
    for (int y = cg_maxi(iy0, cv->cy0); y < cg_mini(iy1, cv->cy1); y++)
        fill_span(cv, y, ix0, ix1, c, 255);
}

/* ---- rounded rectangles ------------------------------------------ */

typedef struct rrect {
    float x0, y0, x1, y1, cx, cy, hx, hy;
    float r[4];
} rrect;

static void rrect_init(rrect *s, float x, float y, float w, float h, const float r[4])
{
    s->x0 = x;
    s->y0 = y;
    s->x1 = x + w;
    s->y1 = y + h;
    s->hx = w * 0.5f;
    s->hy = h * 0.5f;
    s->cx = x + s->hx;
    s->cy = y + s->hy;
    float lim = fminf(s->hx, s->hy);
    for (int i = 0; i < 4; i++) s->r[i] = cg_clampf(r[i], 0.f, fmaxf(lim, 0.f));
}

/* Signed distance to a box with a per-quadrant corner radius. */
static inline float rrect_sd(const rrect *s, float px, float py)
{
    float dx = px - s->cx, dy = py - s->cy;
    float rr = dx < 0 ? (dy < 0 ? s->r[0] : s->r[3]) : (dy < 0 ? s->r[1] : s->r[2]);
    float qx = fabsf(dx) - s->hx + rr, qy = fabsf(dy) - s->hy + rr;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    return fminf(fmaxf(qx, qy), 0.f) + sqrtf(ox * ox + oy * oy) - rr;
}

void canvas_fill_rrect(cg_canvas *cv, float x, float y, float w, float h, const float r[4], pm_color c)
{
    if (w <= 0 || h <= 0 || c.a == 0) return;
    rrect s;
    rrect_init(&s, x, y, w, h, r);
    int ix0, iy0, ix1, iy1;
    if (!bbox(cv, s.x0, s.y0, s.x1, s.y1, &ix0, &iy0, &ix1, &iy1)) return;
    float rtop = fmaxf(s.r[0], s.r[1]), rbot = fmaxf(s.r[2], s.r[3]);
    float rl = fmaxf(s.r[0], s.r[3]), rr = fmaxf(s.r[1], s.r[2]);
    for (int j = iy0; j < iy1; j++) {
        float py = j + 0.5f;
        uint32_t *row = cv->px + (size_t)j * cv->w;
        bool band = py - s.y0 < rtop + 1.f || s.y1 - py < rbot + 1.f;
        /* Pixels in [sx0, sx1) are known to have full horizontal coverage. */
        int sx0 = band ? (int)ceilf(s.x0 + rl) + 1 : (int)ceilf(s.x0) + 1;
        int sx1 = band ? (int)floorf(s.x1 - rr) - 1 : (int)floorf(s.x1) - 1;
        bool vfull = py - s.y0 >= 0.5f && s.y1 - py >= 0.5f;
        if (!vfull) { sx0 = ix1; sx1 = ix1; }
        if (sx0 < ix0) sx0 = ix0;
        if (sx1 > ix1) sx1 = ix1;
        if (sx1 < sx0) sx1 = sx0;
        for (int i = ix0; i < sx0; i++)
            blend(&row[i], c, cov_from_dist(rrect_sd(&s, i + 0.5f, py)));
        if (sx1 > sx0) {
            if (band) {
                for (int i = sx0; i < sx1; i++)
                    blend(&row[i], c, cov_from_dist(rrect_sd(&s, i + 0.5f, py)));
            } else {
                fill_span(cv, j, sx0, sx1, c, 255);
            }
        }
        for (int i = sx1 > sx0 ? sx1 : sx0; i < ix1; i++)
            blend(&row[i], c, cov_from_dist(rrect_sd(&s, i + 0.5f, py)));
    }
}

void canvas_stroke_rrect(cg_canvas *cv, float x, float y, float w, float h, const float r[4],
                         float t, pm_color c)
{
    if (w <= 0 || h <= 0 || t <= 0 || c.a == 0) return;
    rrect o, in;
    float ri[4];
    for (int k = 0; k < 4; k++) ri[k] = fmaxf(r[k] - t, 0.f);
    rrect_init(&o, x, y, w, h, r);
    rrect_init(&in, x + t, y + t, w - 2 * t, h - 2 * t, ri);
    int ix0, iy0, ix1, iy1;
    if (!bbox(cv, o.x0, o.y0, o.x1, o.y1, &ix0, &iy0, &ix1, &iy1)) return;
    float rtop = fmaxf(o.r[0], o.r[1]) + t + 1.f, rbot = fmaxf(o.r[2], o.r[3]) + t + 1.f;
    float edge = t + fmaxf(fmaxf(o.r[0], o.r[1]), fmaxf(o.r[2], o.r[3])) + 2.f;
    for (int j = iy0; j < iy1; j++) {
        float py = j + 0.5f;
        uint32_t *row = cv->px + (size_t)j * cv->w;
        bool full = py - o.y0 < rtop || o.y1 - py < rbot || in.hx <= 0 || in.hy <= 0;
        int a0 = ix0, a1 = ix1, b0 = ix1, b1 = ix1;
        if (!full) {
            a1 = cg_mini(ix1, (int)ceilf(o.x0 + edge));
            b0 = cg_maxi(a1, (int)floorf(o.x1 - edge));
        }
        for (int pass = 0; pass < 2; pass++) {
            int s0 = pass ? b0 : a0, s1 = pass ? b1 : a1;
            for (int i = s0; i < s1; i++) {
                float px = i + 0.5f;
                float co = cg_clampf(0.5f - rrect_sd(&o, px, py), 0.f, 1.f);
                if (co <= 0.f) continue;
                float ci = (in.hx > 0 && in.hy > 0)
                               ? cg_clampf(0.5f - rrect_sd(&in, px, py), 0.f, 1.f)
                               : 0.f;
                float cov = co - ci;
                if (cov > 0.f) blend(&row[i], c, (uint32_t)(cov * 255.f + 0.5f));
            }
        }
    }
}

/* ---- circles, lines, arcs ----------------------------------------- */

void canvas_fill_circle(cg_canvas *cv, float cx, float cy, float r, pm_color c)
{
    int ix0, iy0, ix1, iy1;
    if (r <= 0 || !bbox(cv, cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, &ix0, &iy0, &ix1, &iy1))
        return;
    for (int j = iy0; j < iy1; j++) {
        float dy = j + 0.5f - cy;
        uint32_t *row = cv->px + (size_t)j * cv->w;
        for (int i = ix0; i < ix1; i++) {
            float dx = i + 0.5f - cx;
            blend(&row[i], c, cov_from_dist(sqrtf(dx * dx + dy * dy) - r));
        }
    }
}

void canvas_stroke_circle(cg_canvas *cv, float cx, float cy, float r, float t, pm_color c)
{
    int ix0, iy0, ix1, iy1;
    float ro = r + t * 0.5f;
    if (!bbox(cv, cx - ro - 1, cy - ro - 1, cx + ro + 1, cy + ro + 1, &ix0, &iy0, &ix1, &iy1))
        return;
    for (int j = iy0; j < iy1; j++) {
        float dy = j + 0.5f - cy;
        uint32_t *row = cv->px + (size_t)j * cv->w;
        for (int i = ix0; i < ix1; i++) {
            float dx = i + 0.5f - cx;
            blend(&row[i], c, cov_from_dist(fabsf(sqrtf(dx * dx + dy * dy) - r) - t * 0.5f));
        }
    }
}

void canvas_line(cg_canvas *cv, float x0, float y0, float x1, float y1, float t, pm_color c)
{
    float hw = t * 0.5f;
    int ix0, iy0, ix1, iy1;
    if (!bbox(cv, fminf(x0, x1) - hw - 1, fminf(y0, y1) - hw - 1, fmaxf(x0, x1) + hw + 1,
              fmaxf(y0, y1) + hw + 1, &ix0, &iy0, &ix1, &iy1))
        return;
    float bx = x1 - x0, by = y1 - y0;
    float bb = bx * bx + by * by;
    for (int j = iy0; j < iy1; j++) {
        uint32_t *row = cv->px + (size_t)j * cv->w;
        for (int i = ix0; i < ix1; i++) {
            float ax = i + 0.5f - x0, ay = j + 0.5f - y0;
            float h = bb > 0 ? cg_clampf((ax * bx + ay * by) / bb, 0.f, 1.f) : 0.f;
            float dx = ax - bx * h, dy = ay - by * h;
            blend(&row[i], c, cov_from_dist(sqrtf(dx * dx + dy * dy) - hw));
        }
    }
}

void canvas_arc(cg_canvas *cv, float cx, float cy, float radius, float a0, float a1,
                float half_width, pm_color c)
{
    const float deg = 3.14159265f / 180.f;
    float ex0 = cx + radius * cosf(a0 * deg), ey0 = cy - radius * sinf(a0 * deg);
    float ex1 = cx + radius * cosf(a1 * deg), ey1 = cy - radius * sinf(a1 * deg);
    float ro = radius + half_width + 1;
    int ix0, iy0, ix1, iy1;
    if (!bbox(cv, cx - ro, cy - ro, cx + ro, cy + ro, &ix0, &iy0, &ix1, &iy1)) return;
    for (int j = iy0; j < iy1; j++) {
        uint32_t *row = cv->px + (size_t)j * cv->w;
        float py = j + 0.5f;
        for (int i = ix0; i < ix1; i++) {
            float px = i + 0.5f;
            float dx = px - cx, dy = cy - py;
            float ang = atan2f(dy, dx) / deg;
            if (ang < a0 - 180.f) ang += 360.f;
            float d;
            if (ang >= a0 && ang <= a1) {
                d = fabsf(sqrtf(dx * dx + dy * dy) - radius);
            } else {
                float d0 = hypotf(px - ex0, py - ey0), d1 = hypotf(px - ex1, py - ey1);
                d = fminf(d0, d1);
            }
            blend(&row[i], c, cov_from_dist(d - half_width));
        }
    }
}

void canvas_blit_image(cg_canvas *cv, float x, float y, float w, float h, const uint32_t *argb, int iw,
                       int ih)
{
    int x0 = (int)floorf(x + 0.5f), y0 = (int)floorf(y + 0.5f);
    int x1 = (int)floorf(x + w + 0.5f), y1 = (int)floorf(y + h + 0.5f);
    if (x1 <= x0 || y1 <= y0 || iw <= 0 || ih <= 0) return;
    int cx0 = cg_maxi(x0, cv->cx0), cy0 = cg_maxi(y0, cv->cy0);
    int cx1 = cg_mini(x1, cv->cx1), cy1 = cg_mini(y1, cv->cy1);
    for (int j = cy0; j < cy1; j++) {
        int sy = (int)((j - y0 + 0.5f) * ih / (float)(y1 - y0));
        const uint32_t *src = argb + (size_t)cg_mini(sy, ih - 1) * iw;
        uint32_t *row = cv->px + (size_t)j * cv->w;
        for (int i = cx0; i < cx1; i++) {
            uint32_t p = src[cg_mini((int)((i - x0 + 0.5f) * iw / (float)(x1 - x0)), iw - 1)];
            if (!(p >> 24)) continue;
            cg_color c = { (uint8_t)(p >> 16), (uint8_t)(p >> 8), (uint8_t)p, (uint8_t)(p >> 24) };
            blend(&row[i], pm_from(c, 1.f), 255);
        }
    }
}

void canvas_blit_a8(cg_canvas *cv, int x, int y, const uint8_t *a8, int w, int h, int pitch,
                    pm_color c)
{
    int x0 = cg_maxi(x, cv->cx0), y0 = cg_maxi(y, cv->cy0);
    int x1 = cg_mini(x + w, cv->cx1), y1 = cg_mini(y + h, cv->cy1);
    for (int j = y0; j < y1; j++) {
        const uint8_t *src = a8 + (size_t)(j - y) * pitch - x;
        uint32_t *row = cv->px + (size_t)j * cv->w;
        for (int i = x0; i < x1; i++) {
            uint32_t a = src[i];
            if (a) blend(&row[i], c, a);
        }
    }
}
