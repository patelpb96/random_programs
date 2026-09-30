# cgui / preetum: working notes for Claude

Read this first when picking up work here. It is the handoff from earlier sessions. The
detailed documents are in `docs/`:

| Document | Read it when |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Before changing anything in `src/`: layers, the frame pipeline, coordinate spaces, widget state, the platform contract, ownership. |
| [docs/DESIGN.md](docs/DESIGN.md) | Before changing a design decision: why things are the way they are, and what was rejected. |
| [docs/PITFALLS.md](docs/PITFALLS.md) | Always skim. Real bugs that happened, the rules that prevent them, and **testing recipes**. |
| [docs/PREETUM.md](docs/PREETUM.md) | Adding a tool or a service to the app; a complete worked example. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Platform status, known issues, what to build next. Keep it updated. |

## What this is

- **cgui** (`include/`, `src/`) is a C99 GUI toolkit with an entirely custom-drawn window:
  - a software rasterizer into a per-pixel-alpha buffer;
  - the three window buttons laid along the curved top-right corner;
  - separate frame and body transparency, themes, and optional rounded corners;
  - an immediate-mode widget API;
  - backends for X11, Win32 and Cocoa. The only dependency is FreeType.
- **preetum** (`apps/preetum/`) is the app: a main menu of tools (Font Compare, Glyph Map
  so far) with a pixel-art diamond logo. It is meant to grow well beyond fonts.

The user (repo owner) wants:
- polished visuals;
- the curved button corner kept as the signature element;
- nothing heavy loaded unless the open screen uses it (font data included);
- documentation kept current.

## Build, run, test

```sh
cd cgui
make && ./build/preetum                 # Linux/BSD/macOS; must stay warning-free
cmake -S . -B /tmp/b && cmake --build /tmp/b && (cd /tmp/b && ctest)   # Windows uses CMake
PREETUM_TOOL=2 ./build/preetum          # open a tool directly (1-based)
CGUI_SCREENSHOT=/tmp/s.ppm ./build/preetum   # render, dump PPM (checkerboard = transparency), exit
```

In the cloud container there is no display. Start `Xvfb :99 &` and `export DISPLAY=:99`.
The tools available (xdotool, xwd/ImageMagick, openbox, xcompmgr, strace, MinGW, Wine) and
exact commands are in [PITFALLS.md → Testing recipes](docs/PITFALLS.md#testing-recipes).

**Verify visually.** For any visual change, take a screenshot and look at it, zooming into
details such as the button corner. For interaction changes, drive the app with xdotool.
Don't claim a platform works unless you ran it there. macOS has never been compiled.

## Code map (one line each)

- `src/window.c`: the `cg_run` loop, `do_frame` (the order matters), the chrome and the
  curved-corner `geometry()`, resize zones, the drag threshold, `do_pending`.
- `src/ui.c`: `ui_behave` (hot/active/focus), drawing wrappers (logical → physical), the
  clip stack, rect cutting, the widgets, the dropdown popup (`ui_popup_end`).
- `src/textedit.c`: `cg_textbuf` and the editor (wrap layout, selection, undo, clipboard).
- `src/render.c`: premultiplied ARGB canvas, SDF anti-aliased shapes, blits.
- `src/font.c`: `cg_font` and its glyph cache (measuring skips rasterizing; bounded).
- `src/fontdb.c`: font discovery, `cg_fontdb_scan` / `release`, and the known-path UI font
  lookup.
- `src/platform_{x11,win32}.c`, `src/platform_cocoa.m`: backends implementing
  `src/platform.h`.
- `apps/preetum/main.c`: the `tools[]` registry, `set_screen` (services lifecycle), the
  menu, the appearance bar.
- `apps/preetum/fonts.c`: the fonts service (open → "Loading fonts…" → ready → close).
- `apps/preetum/tool_*.c`: one file per tool. `logo.c` builds the icon.
- `tests/font_cache.c`: the CTest for glyph cache behaviour.

## Rules that are easy to break

(Details and reasons are in PITFALLS.md.)

- **Window operations run after the frame.** Never call `plat_window_begin_drag` or any
  window operation inside a frame. Queue it (`pending_drag` / `pending_op`).
- **Drags start after a threshold.** A title-bar or edge press only *arms* a drag, and it
  starts after 3 px of movement. Use `press_taken`, not `active`, to know whether a widget
  took a press.
- **No font scanning outside the fonts service.** In preetum, font data is touched only
  through the fonts service (`NEEDS_FONTS` plus `fonts_ready`). Menu-card `icon()` functions
  run with services closed. `cg_window_create` must not scan fonts.
- **Fontdb indices die on `cg_fontdb_release`.** Persist names, not indices.
- **Glyph pointers inside `font.c`** are valid only until the next lookup.
- **Screen-level shortcuts only when `cg_focused(win) == 0`.**
- **Ids:** hash distinct strings (`cg_id_str`). Widgets use `id+1` and `id+2` internally.
- **Tools use only `cgui.h`.** Add small public APIs rather than reaching into
  `cg_internal.h`.
- **Changing the button angles** in `geometry()`: re-check that the close button is above
  the separator and that the buttons stay inside the resize band. Zoom a screenshot.

## Workflow conventions

- **Commits:**
  - Develop on the branch the session names.
  - Commit messages have a subject line plus a body explaining *why*.
  - Push with `git push -u origin <branch>`.
- **PR:** work so far is in PR patelpb96/random_programs#2
  (branch `claude/magical-wright-0sni0p`). The user sometimes pushes commits too, so always
  `git pull` before starting.
- **Keep docs current:**
  - `docs/ROADMAP.md` for status and issues;
  - `README.md` for user-facing features and screenshots (regenerate `docs/*.png` with
    `CGUI_SCREENSHOT`; the crop command for `corner.png` is in PITFALLS);
  - this file for new rules.
