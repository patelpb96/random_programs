/* The font selection shared by all tools: default choice, loading, and the
 * family/style dropdown pair (with per-row previews in the family list). */
#include "preetum.h"

#include <stdio.h>
#include <stdlib.h>

static const char *family_label(void *user, int i)
{
    (void)user;
    return cg_fontdb_family(i)->name;
}

static cg_font *family_preview(void *user, int i)
{
    preetum *a = (preetum *)user;
    a->tick++;
    int victim = 0;
    for (int k = 0; k < PREVIEW_CACHE; k++) {
        if (a->preview[k] && a->preview_family[k] == i) {
            a->preview_used[k] = a->tick;
            return a->preview[k];
        }
        if (a->preview_used[k] < a->preview_used[victim]) victim = k;
    }
    const cg_font_family *fam = cg_fontdb_family(i);
    const cg_font_face *face = &fam->faces[fam->regular];
    cg_font_free(a->preview[victim]);
    a->preview[victim] = cg_font_load(face->path, face->index);
    a->preview_family[victim] = i;
    a->preview_used[victim] = a->tick;
    return a->preview[victim];
}

static const char *style_label(void *user, int i)
{
    preetum *a = (preetum *)user;
    return cg_fontdb_family(a->family)->faces[i].style;
}

void fonts_pick_default(preetum *a)
{
    static const char *prefs[] = { "DejaVu Serif", "Georgia", "Noto Serif", "Times New Roman",
                                   "Liberation Serif", "FreeSerif" };
    a->loaded_family = a->loaded_style = -1;
    for (int k = 0; k < PREVIEW_CACHE; k++) a->preview_family[k] = -1;
    a->family = 0;
    for (size_t i = 0; i < sizeof prefs / sizeof *prefs; i++) {
        int f = cg_fontdb_find(prefs[i]);
        if (f >= 0) {
            a->family = f;
            break;
        }
    }
    if (cg_fontdb_family_count() > 0) {
        a->style = cg_fontdb_family(a->family)->regular;
        fonts_load_selected(a);
    }
}

void fonts_load_selected(preetum *a)
{
    if (a->family == a->loaded_family && a->style == a->loaded_style) return;
    const cg_font_family *fam = cg_fontdb_family(a->family);
    if (!fam) return;
    if (a->style < 0 || a->style >= fam->face_count) a->style = fam->regular;
    const cg_font_face *face = &fam->faces[a->style];
    cg_font *f = cg_font_load(face->path, face->index);
    if (f) {
        cg_font_free(a->font);
        a->font = f;
        snprintf(a->font_desc, sizeof a->font_desc, "%s %s  ·  %s", face->family, face->style, face->path);
    } else {
        snprintf(a->font_desc, sizeof a->font_desc, "Could not load %s", face->path);
    }
    a->loaded_family = a->family;
    a->loaded_style = a->style;
}

void fonts_picker(cg_window *win, cg_rect row, preetum *a)
{
    const cg_font_family *fam = cg_fontdb_family(a->family);
    if (!fam) {
        cg_label(win, row, "No fonts found", CG_ALIGN_LEFT, cg_window_style(win)->theme.text_dim);
        return;
    }
    cg_list_source styles = { fam->face_count, style_label, NULL, NULL, false, a };
    if (cg_dropdown(win, cg_id_str("font.style"), cg_cut_right(&row, 130), &styles, &a->style))
        fonts_load_selected(a);
    cg_cut_right(&row, 8);
    cg_list_source families = { cg_fontdb_family_count(), family_label, family_preview, "Aa Gg 123", true, a };
    int prev = a->family;
    if (cg_dropdown(win, cg_id_str("font.family"), row, &families, &a->family) && a->family != prev) {
        a->style = cg_fontdb_family(a->family)->regular;
        fonts_load_selected(a);
    }
}

void fonts_free(preetum *a)
{
    for (int k = 0; k < PREVIEW_CACHE; k++) cg_font_free(a->preview[k]);
    cg_font_free(a->font);
}
