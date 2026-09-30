/*
 * The preetum logo: a pixel-art cut diamond with a little sparkle, built
 * procedurally so it can be tweaked easily. 16x16, straight-alpha ARGB.
 */
#include "preetum.h"

#include <string.h>

/* The gem occupies rows 2..13: a 16-wide girdle, a crown narrowing upward
 * to an 8-pixel table, and a pavilion narrowing down to a 2-pixel point. */
static bool in_gem(int x, int y)
{
    int g = y - 2;
    if (g < 0 || g > 11) return false;
    int left = g <= 4 ? 4 - g : g - 4;
    int right = g <= 4 ? 11 + g : 19 - g;
    return x >= left && x <= right;
}

void logo_build(uint32_t out[LOGO_W * LOGO_H])
{
    const uint32_t outline = 0xFF0B2E4F, deep = 0xFF1F6FB2, mid = 0xFF3FA0E0;
    const uint32_t light = 0xFF8FD8FF, pale = 0xFFD6F4FF, white = 0xFFFFFFFF;
    memset(out, 0, sizeof(uint32_t) * LOGO_W * LOGO_H);

    for (int y = 0; y < LOGO_H; y++) {
        for (int x = 0; x < LOGO_W; x++) {
            if (!in_gem(x, y)) continue;
            uint32_t c;
            bool edge = !in_gem(x - 1, y) || !in_gem(x + 1, y) || !in_gem(x, y - 1) || !in_gem(x, y + 1);
            int g = y - 2;
            if (edge) {
                c = outline;
            } else if (g < 4) {
                /* Crown: bright table in the middle, bezel facets either side. */
                c = x <= 4 ? light : x >= 11 ? mid : pale;
                if (x == 5 + (3 - g) || x == 10 - (3 - g)) c = light; /* facet edges */
            } else if (g == 4) {
                c = x < 8 ? mid : deep; /* girdle */
            } else {
                /* Pavilion: facets converging on the point. */
                float d = x - 7.5f, span = (19 - g) - 7.5f;
                c = d < -span * 0.35f ? light : d > span * 0.35f ? deep : mid;
            }
            out[y * LOGO_W + x] = c;
        }
    }
    /* Glints on the table and a four-point sparkle off the top-right. */
    out[3 * LOGO_W + 5] = white;
    out[4 * LOGO_W + 4] = white;
    out[0 * LOGO_W + 14] = white;
    out[1 * LOGO_W + 13] = white;
    out[1 * LOGO_W + 14] = white;
    out[1 * LOGO_W + 15] = white;
    out[2 * LOGO_W + 14] = white;
}
