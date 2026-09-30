/*
 * The fonts service: the system font database, the selected font, and the
 * preview fonts used by the family dropdown.
 *
 * Lifecycle (driven by main.c from each tool's NEEDS_FONTS flag):
 *   fonts_open   CLOSED -> LOADING   (cheap; nothing is read yet)
 *   fonts_ready  LOADING: first frame draws "Loading fonts…" and asks for
 *                another frame, so the window never freezes silently; the
 *                next frame scans the database and loads the selection.
 *   fonts_close  -> CLOSED, freeing everything. The selection is remembered
 *                by family/style *name*, because indices into the database
 *                are meaningless after it is released and rescanned.
 */
#include "preetum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Choose the family/style: the remembered one if it still exists, else a
 * pleasant serif, else whatever sorts first. */
static void pick_selection(preetum *a)
{
    static const char *prefs[] = { "DejaVu Serif", "Georgia", "Noto Serif", "Times New Roman",
                                   "Liberation Serif", "FreeSerif" };
    a->family = a->keep_family[0] ? cg_fontdb_find(a->keep_family) : -1;
    for (size_t i = 0; a->family < 0 && i < sizeof prefs / sizeof *prefs; i++) a->family = cg_fontdb_find(prefs[i]);
    if (a->family < 0) a->family = 0;
    const cg_font_family *fam = cg_fontdb_family(a->family);
    a->style = fam ? fam->regular : 0;
    for (int k = 0; fam && a->keep_style[0] && k < fam->face_count; k++)
        if (strcmp(fam->faces[k].style, a->keep_style) == 0) a->style = k;
}

void fonts_open(preetum *a)
{
    if (a->fonts_state != FONTS_CLOSED) return;
    a->fonts_state = FONTS_LOADING;
    a->fonts_splash_shown = false;
}

bool fonts_ready(cg_window *win, cg_rect r, preetum *a)
{
    if (a->fonts_state == FONTS_READY) return true;
    if (a->fonts_state == FONTS_CLOSED) fonts_open(a); /* a tool forgot NEEDS_FONTS */
    if (!a->fonts_splash_shown) {
        cg_label(win, r, "Loading fonts…", CG_ALIGN_CENTER, cg_window_style(win)->theme.text_dim);
        a->fonts_splash_shown = true;
        cg_request_wakeup(win, 0); /* present this frame, then come back */
        return false;
    }
    cg_fontdb_scan();
    a->loaded_family = a->loaded_style = -1;
    for (int k = 0; k < PREVIEW_CACHE; k++) a->preview_family[k] = -1;
    if (cg_fontdb_family_count() > 0) {
        pick_selection(a);
        fonts_load_selected(a);
    }
    a->fonts_state = FONTS_READY;
    return true;
}

void fonts_close(preetum *a)
{
    if (a->fonts_state == FONTS_CLOSED) return;
    const cg_font_family *fam = cg_fontdb_family(a->family);
    if (a->fonts_state == FONTS_READY && fam) {
        snprintf(a->keep_family, sizeof a->keep_family, "%s", fam->name);
        if (a->style >= 0 && a->style < fam->face_count)
            snprintf(a->keep_style, sizeof a->keep_style, "%s", fam->faces[a->style].style);
    }
    for (int k = 0; k < PREVIEW_CACHE; k++) {
        cg_font_free(a->preview[k]);
        a->preview[k] = NULL;
        a->preview_family[k] = -1;
        a->preview_used[k] = 0;
    }
    cg_font_free(a->font);
    a->font = NULL;
    a->font_desc[0] = 0;
    a->loaded_family = a->loaded_style = -1;
    cg_fontdb_release();
    a->fonts_state = FONTS_CLOSED;
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
