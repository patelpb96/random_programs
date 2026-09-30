/*
 * System font enumeration. Rather than using a different API per OS
 * (fontconfig, DirectWrite, CoreText), this walks the platform's font
 * directories and asks FreeType for each face's family/style names. That
 * gives one code path everywhere, and every face found is guaranteed to be
 * loadable by the renderer.
 */
#include "cg_internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static cg_font_face *g_faces;
static int g_nfaces, g_capfaces;
static cg_font_family *g_fams;
static int g_nfams;
static bool g_scanned;

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb || !ca) return ca - cb;
    }
}

static bool has_font_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    static const char *exts[] = { ".ttf", ".otf", ".ttc", ".otc", ".pfb", ".pfa", ".dfont" };
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
        if (ci_cmp(dot, exts[i]) == 0) return true;
    return false;
}

static void add_face(FT_Face face, const char *path, int index)
{
    if (!FT_IS_SCALABLE(face) || !face->family_name || !face->family_name[0]) return;
    if (face->family_name[0] == '.') return; /* macOS private system fonts */
    if (g_nfaces == g_capfaces) {
        g_capfaces = g_capfaces ? g_capfaces * 2 : 256;
        g_faces = (cg_font_face *)realloc(g_faces, sizeof(cg_font_face) * (size_t)g_capfaces);
    }
    cg_font_face *f = &g_faces[g_nfaces++];
    f->family = strdup(face->family_name);
    f->style = strdup(face->style_name ? face->style_name : "Regular");
    f->path = strdup(path);
    f->index = index;
    f->italic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    f->weight = (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
    TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(face, FT_SFNT_OS2);
    if (os2 && os2->version != 0xFFFF && os2->usWeightClass >= 100 && os2->usWeightClass <= 1000)
        f->weight = os2->usWeightClass;
}

static void scan_file(FT_Library lib, const char *path)
{
    FT_Face face;
    if (FT_New_Face(lib, path, 0, &face) != 0) return;
    long nfaces = face->num_faces;
    for (long i = 0; i < nfaces && i < 256; i++) {
        if (i > 0 && FT_New_Face(lib, path, i, &face) != 0) continue;
        int instances = (int)(face->style_flags >> 16);
        if (instances > 0 && FT_HAS_MULTIPLE_MASTERS(face)) {
            /* Variable font: list its named instances (Light, Bold, ...). */
            FT_Done_Face(face);
            for (int k = 1; k <= instances && k < 64; k++) {
                int idx = (int)((k << 16) | i);
                if (FT_New_Face(lib, path, idx, &face) == 0) {
                    add_face(face, path, idx);
                    FT_Done_Face(face);
                }
            }
            continue;
        }
        add_face(face, path, (int)i);
        FT_Done_Face(face);
    }
}

#ifdef _WIN32
static void scan_dir(FT_Library lib, const char *dir, int depth)
{
    if (depth > 6) return;
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            scan_dir(lib, path, depth + 1);
        else if (has_font_ext(fd.cFileName))
            scan_file(lib, path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void scan_dir(FT_Library lib, const char *dir, int depth)
{
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            scan_dir(lib, path, depth + 1);
        else if (S_ISREG(st.st_mode) && has_font_ext(e->d_name))
            scan_file(lib, path);
    }
    closedir(d);
}
#endif

static int face_cmp(const void *pa, const void *pb)
{
    const cg_font_face *a = (const cg_font_face *)pa, *b = (const cg_font_face *)pb;
    int c = ci_cmp(a->family, b->family);
    if (c) return c;
    if (a->italic != b->italic) return a->italic - b->italic;
    if (a->weight != b->weight) return a->weight - b->weight;
    c = ci_cmp(a->style, b->style);
    if (c) return c;
    c = strcmp(a->path, b->path);
    return c ? c : a->index - b->index;
}

static void free_face(cg_font_face *f)
{
    free(f->family);
    free(f->style);
    free(f->path);
}

int cg_fontdb_scan(void)
{
    if (g_scanned) return g_nfams;
    g_scanned = true;
    FT_Library lib;
    if (FT_Init_FreeType(&lib) != 0) return 0;

    char *dirs[32];
    int ndirs = plat_font_dirs(dirs, 32);
    for (int i = 0; i < ndirs; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++)
            if (strcmp(dirs[i], dirs[j]) == 0) dup = true;
        if (!dup) scan_dir(lib, dirs[i], 0);
    }
    for (int i = 0; i < ndirs; i++) free(dirs[i]);
    FT_Done_FreeType(lib);

    qsort(g_faces, (size_t)g_nfaces, sizeof *g_faces, face_cmp);

    /* Drop duplicates (the same family+style installed in two places). */
    int n = 0;
    for (int i = 0; i < g_nfaces; i++) {
        if (n > 0 && ci_cmp(g_faces[n - 1].family, g_faces[i].family) == 0 &&
            ci_cmp(g_faces[n - 1].style, g_faces[i].style) == 0) {
            free_face(&g_faces[i]);
            continue;
        }
        g_faces[n++] = g_faces[i];
    }
    g_nfaces = n;

    /* Group into families. */
    g_fams = (cg_font_family *)calloc((size_t)(g_nfaces ? g_nfaces : 1), sizeof *g_fams);
    for (int i = 0; i < g_nfaces;) {
        int j = i;
        while (j < g_nfaces && ci_cmp(g_faces[j].family, g_faces[i].family) == 0) j++;
        cg_font_family *fam = &g_fams[g_nfams++];
        fam->name = g_faces[i].family;
        fam->faces = &g_faces[i];
        fam->face_count = j - i;
        int best = 0, best_score = 1 << 30;
        for (int k = 0; k < fam->face_count; k++) {
            const cg_font_face *f = &fam->faces[k];
            int score = abs(f->weight - 400) + (f->italic ? 1000 : 0);
            if (ci_cmp(f->style, "Regular") == 0) score -= 5;
            if (score < best_score) best_score = score, best = k;
        }
        fam->regular = best;
        i = j;
    }
    return g_nfams;
}

int cg_fontdb_family_count(void) { return g_nfams; }

const cg_font_family *cg_fontdb_family(int index)
{
    return index >= 0 && index < g_nfams ? &g_fams[index] : NULL;
}

int cg_fontdb_find(const char *name)
{
    int lo = 0, hi = g_nfams - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = ci_cmp(g_fams[mid].name, name);
        if (c == 0) return mid;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

const char *fontdb_default_ui_font_path(int *index)
{
    const char *env = getenv("CGUI_FONT");
    if (env && *env) {
        *index = 0;
        return env;
    }
    cg_fontdb_scan();
    static const char *prefs[] = {
        "Segoe UI", "SF Pro Text", "Helvetica Neue", "Inter", "Noto Sans", "DejaVu Sans",
        "Cantarell", "Ubuntu", "Liberation Sans", "Arial", "Helvetica", "Verdana", "FreeSans",
    };
    for (size_t i = 0; i < sizeof prefs / sizeof *prefs; i++) {
        int fi = cg_fontdb_find(prefs[i]);
        if (fi >= 0) {
            const cg_font_face *f = &g_fams[fi].faces[g_fams[fi].regular];
            *index = f->index;
            return f->path;
        }
    }
    if (g_nfams > 0) {
        const cg_font_face *f = &g_fams[0].faces[g_fams[0].regular];
        *index = f->index;
        return f->path;
    }
    return NULL;
}
