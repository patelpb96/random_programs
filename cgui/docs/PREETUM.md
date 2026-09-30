# preetum: app structure and how to extend it

preetum is the app built on cgui: a main menu of small tools. Fonts were only the first
subject, and the structure is meant for tools about anything: colours, time, files, images,
text utilities. This document is the guide for adding more.

## Files

| File | Role |
|---|---|
| `apps/preetum/preetum.h` | The `preetum` app struct (all state), the `tool` contract, service flags, declarations. |
| `apps/preetum/main.c` | Window setup, the tool registry (`tools[]`), `set_screen` (navigation and service lifecycle), main menu, appearance bar, `main()`. |
| `apps/preetum/logo.c` | The 16×16 pixel-art diamond, built procedurally (`logo_build`). Used as the window icon and on the menu. |
| `apps/preetum/fonts.c` | The **fonts service**: font database, selected font, dropdown previews, `fonts_picker`. |
| `apps/preetum/tool_compare.c` | Font Compare tool. |
| `apps/preetum/tool_glyphs.c` | Glyph Map tool. |

## Screens and navigation

`app->screen` is `-1` for the main menu, otherwise an index into `tools[]`.

- **Opening a tool:** click its card, or press `1`–`9` on the menu.
- **Going back to the menu:** press **‹ Menu**, or Esc (only when no widget has keyboard
  focus).
- **All switches go through `set_screen(app, n)`.** It runs the lifecycle:

```
old tool's leave()  →  close services the new screen doesn't need
→  app->screen = n  →  open services the new screen needs  →  new tool's enter()
```

Every screen gets the same frame layout from `frame()` in `main.c`:
- the appearance bar at the bottom (theme, frame and body opacity, rounded corners with
  radius, accent colour);
- a nav row (back button, the tool's icon and name) on tool screens;
- the remaining rect goes to the tool's `frame`.

## The tool contract

```c
typedef struct tool {
    const char *name;                 /* menu card title, nav row, window title   */
    const char *blurb[2];             /* two card lines, ~30 characters each      */
    unsigned needs;                   /* NEEDS_* services held while open         */
    void (*enter)(preetum *app);      /* optional, after services are opened      */
    void (*leave)(preetum *app);      /* optional, before services are closed     */
    void (*icon)(cg_window *, cg_rect, preetum *);   /* 48×48 card icon           */
    void (*frame)(cg_window *, cg_rect, preetum *);  /* the tool's whole UI       */
} tool;
```

**Card sizing.** Cards are 300×112. The blurb is drawn at 13 px and clipped at about 190 px
wide, so keep each line to about 30 characters. The name uses 17 px.

**`icon`** runs **on the main menu, where no services are open**:
- `app->font` may be NULL. Fall back to `cg_ui_font(win)`.
- Don't call service functions from it.

**`frame`** runs every frame the tool is open (only on input, or on the wake-ups you
request):
- Use rect cutting for layout, and the widgets and drawing calls in `cgui.h`.
- Keep the tool's state in `struct preetum`. Give it a comment header and a prefix
  (`glyph_*`, `in_*`/`out_*`).

**`leave`** should free anything big the tool built, such as Glyph Map's codepoint table.
**Persistent state that is cheap should survive.** Font Compare keeps the user's text, and
the fonts service keeps the selected family by *name*.

**Keyboard shortcuts** at tool level only when `cg_focused(win) == 0`. Otherwise you'll
steal keys from a text box or an open dropdown.

**Animation or timing:** call `cg_request_wakeup(win, seconds)` from `frame` for as long as
something moves. When nothing requests wake-ups, the app sleeps.

## Services

A service is a shared, expensive resource that exists only while some open tool needs it.

### The fonts service

`NEEDS_FONTS`, in `fonts.c`, is the only service so far.

| Call | Meaning |
|---|---|
| `fonts_open(app)` | Called by `set_screen`. Cheap: sets the state to LOADING. |
| `fonts_ready(win, r, app)` | Call first in the tool's frame (or before the part that needs fonts). First frame: draws "Loading fonts…" into `r`, requests a wake-up, returns false. Next frame: scans and loads, returns true. |
| `fonts_picker(win, row, app)` | Family (filterable, with previews) and style dropdowns. |
| `app->font`, `app->font_desc` | The selected `cg_font` and a description ("Family Style · path"). |
| `fonts_close(app)` | Called by `set_screen`. Frees previews, the selected font and the database, and remembers the selection by name. |

Font Compare calls `fonts_ready` only before its *right* half, so the editor works while
fonts load. Glyph Map gates its whole frame.

### Adding a new service

Example: images for an image-viewer tool.
1. Add a flag: `enum { NEEDS_FONTS = 1, NEEDS_IMAGES = 2 };`.
2. Create `images.c` with `images_open`, `images_ready`/`images_get`, and `images_close`,
   plus its state in `struct preetum`. Mirror `fonts.c`:
   - **`open`** is cheap;
   - **heavy work** happens on first use from a frame (show a placeholder first if it can
     take long);
   - **`close`** frees everything and keeps only small, name-based memory of choices.
3. In `set_screen`, add the matching lines next to the `NEEDS_FONTS` ones (close when
   `had & ~want`, open when `want & ~had`).
4. Declare it in `preetum.h`, and list the file in `CMakeLists.txt`. The Makefile globs
   `apps/preetum/*.c`.

If a service needs a new third-party library, prefer a vendored single-file library, such
as `stb_image.h`, placed in `apps/preetum/third_party/`. That way the build stays
"FreeType + OS libraries".

## Adding a tool: complete example (no fonts)

A stopwatch with a start/stop button, a reset button and animation. It needs no services.

**1. State.** Add this to `struct preetum` in `preetum.h`:

```c
    /* Stopwatch. */
    double sw_elapsed, sw_started;
    bool sw_running;
```

**2. Declarations**, also in `preetum.h`:

```c
void stopwatch_icon(cg_window *win, cg_rect r, preetum *app);
void stopwatch_frame(cg_window *win, cg_rect r, preetum *app);
```

**3. `apps/preetum/tool_stopwatch.c`:**

```c
/* Stopwatch: start/stop and reset. Shows how to animate: request wake-ups
 * only while running, so the app sleeps otherwise. Needs no services. */
#include "preetum.h"

#include <math.h>
#include <stdio.h>

void stopwatch_icon(cg_window *win, cg_rect r, preetum *a)
{
    (void)a; /* services are closed on the menu; don't use them here */
    const cg_theme *th = &cg_window_style(win)->theme;
    float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.55f, rad = r.h * 0.36f;
    cg_stroke_circle(win, cx, cy, rad, 2, th->text);
    cg_line(win, cx, cy, cx + rad * 0.45f, cy - rad * 0.6f, 2, th->accent);
    cg_fill_rrect(win, cg_rect_make(cx - 4, cy - rad - 6, 8, 3), 1, th->text);
}

void stopwatch_frame(cg_window *win, cg_rect r, preetum *a)
{
    const cg_theme *th = &cg_window_style(win)->theme;
    double t = a->sw_elapsed + (a->sw_running ? cg_time() - a->sw_started : 0);
    if (a->sw_running) cg_request_wakeup(win, 1.0 / 30); /* ~30 fps only while running */

    char buf[32];
    snprintf(buf, sizeof buf, "%02d:%05.2f", (int)(t / 60), fmod(t, 60));
    cg_font *f = cg_ui_font(win);
    float size = 72, lh, w = cg_text_width(win, f, size, buf, -1);
    cg_font_metrics(win, f, size, NULL, NULL, &lh);
    cg_draw_text(win, f, size, r.x + (r.w - w) * 0.5f, r.y + r.h * 0.35f - lh * 0.5f, buf, -1, th->text);

    cg_rect row = cg_rect_make(r.x + r.w * 0.5f - 130, r.y + r.h * 0.35f + lh * 0.5f + 20, 260, 34);
    if (cg_button(win, cg_id_str("sw.toggle"), cg_cut_left(&row, 125), a->sw_running ? "Stop" : "Start")) {
        if (a->sw_running) a->sw_elapsed += cg_time() - a->sw_started;
        else a->sw_started = cg_time();
        a->sw_running = !a->sw_running;
    }
    cg_cut_left(&row, 10);
    if (cg_button(win, cg_id_str("sw.reset"), row, "Reset")) {
        a->sw_elapsed = 0;
        a->sw_started = cg_time();
    }
}
```

**4. Register it** in `main.c`'s `tools[]`:

```c
    { "Stopwatch", { "Time things. Start, stop,", "reset." },
      0 /* no services */, NULL, NULL, stopwatch_icon, stopwatch_frame },
```

**5. Build.** Add the file to `add_executable(preetum ...)` in `CMakeLists.txt` and run
`make`. The card, its number key, the nav row and the window title all come from the table
entry.

## Ideas for future tools

Grouped by what they need:
- **No services:**
  - colour picker and WCAG contrast checker (RGB/HSL sliders, `cg_swatch`);
  - unit converter;
  - stopwatch or pomodoro timer (above);
  - hash or base64 calculator using two `cg_textedit`s;
  - JSON formatter;
  - calculator;
  - a "ruler" overlay using the window's transparency.
- **Fonts service:** a font pairing previewer (two fonts side by side), and a waterfall of
  one font at many sizes.
- **New services:**
  - an image viewer or palette extractor (`NEEDS_IMAGES` with a vendored `stb_image.h`);
  - a clipboard history (needs periodic polling through `cg_request_wakeup`);
  - a file browser (a `plat_`-like directory listing already exists in `fontdb.c`, so
    generalize it).

## Style conventions

- **Language:** C99, 4-space indent, `snake_case`, and a comment header at the top of each
  file saying what it's for.
- **Warnings:** the build must be warning-free with `-Wall -Wextra` (`make` shows them),
  and also with the MinGW cross-compile (see [PITFALLS.md](PITFALLS.md#testing-recipes)).
- **Library boundary:** tools use only the public `cgui.h`. If a tool needs something from
  `src/`, add a small public function to `cgui.h` rather than including `cg_internal.h`.
  `cg_interact`, `cg_wheel`, `cg_focused` and `cg_draw_image` were added this way.
