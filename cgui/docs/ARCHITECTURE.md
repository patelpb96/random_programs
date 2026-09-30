# cgui architecture

This document explains how cgui is put together: the layers, how a frame is produced, and
who owns what. Read it before changing anything in `src/`.

- [DESIGN.md](DESIGN.md) explains *why* things are this way.
- [PITFALLS.md](PITFALLS.md) lists the traps.
- [PREETUM.md](PREETUM.md) covers the app built on top.

## Layers

```
┌──────────────────────────────────────────────────────────────┐
│ app (apps/preetum/*)                                          │
│   frame callback: layout + widgets + drawing, every frame     │
├──────────────────────────────────────────────────────────────┤
│ public API  include/cgui.h                                    │
├───────────────┬───────────────┬──────────────┬───────────────┤
│ window.c      │ ui.c          │ textedit.c   │ theme.c       │
│ loop, chrome, │ widget core,  │ editor       │ built-in      │
│ input state   │ widgets, popup│ widget       │ themes        │
├───────────────┴──────┬────────┴──────────────┴───────────────┤
│ render.c             │ font.c (+ fontdb.c)                    │
│ software rasterizer  │ FreeType glyph cache, system font list │
├──────────────────────┴───────────────────────────────────────┤
│ platform.h  ── one backend per OS, nothing else is OS-specific│
│ platform_x11.c │ platform_win32.c │ platform_cocoa.m          │
└──────────────────────────────────────────────────────────────┘
```

- **Private state:** everything below the public API shares the `cg_window` struct and
  helpers declared in `src/cg_internal.h`. Only `cgui.h` is public.
- **Platform layer:** the only OS code lives in the three backends. They know nothing about
  widgets. They create a window, deliver input events, show a pixel buffer, and do
  OS-level window operations.

## Directory map

| Path | What it is |
|---|---|
| `include/cgui.h` | The whole public API, commented. |
| `src/cg_internal.h` | `cg_window` definition, canvas and font internals, cross-file helpers. |
| `src/window.c` | `cg_run` event loop, frame execution, window chrome (title bar, curved buttons, resize zones), debug screenshot. |
| `src/ui.c` | Ids, hot/active/focus, drawing wrappers in logical units, clip stack, rect-cut layout, widgets, dropdown popup. |
| `src/textedit.c` | `cg_textbuf` and the multi-line editor widget. |
| `src/render.c` | Canvas and anti-aliased shapes (SDF), glyph and image blits. |
| `src/font.c` | `cg_font`: FreeType face, glyph cache, measuring and drawing, UTF-8 helpers. |
| `src/fontdb.c` | System font discovery (`cg_fontdb_*`), UI font lookup. |
| `src/theme.c` | The six built-in themes. |
| `src/platform.h` | The backend contract (see below). |
| `src/platform_*.c/.m` | X11, Win32 and Cocoa backends. |
| `apps/preetum/` | The preetum app (menu plus tools). |
| `tests/` | CTest programs (`font_cache.c`). |
| `docs/` | These documents and the README screenshots. |

## Coordinate systems

There are three coordinate spaces. Mixing them up is the most common source of bugs.

| Space | Used by | Conversion |
|---|---|---|
| **Physical pixels** | The platform layer, the canvas (`cg_canvas`), `font.c` internals (`px_size`) | — |
| **Logical pixels** | The whole public API: rects, font sizes, mouse position | logical = physical / `w->scale` |
| **26.6 fixed point** | FreeType advances and pens inside `font.c` / `textedit.c` layout | px = v / 64 |

Every public drawing call multiplies by `w->scale` at the boundary (`cg_fill_rect` and so on
in `ui.c`, `cg_draw_text` in `font.c`). Text layout is done in physical pixels so that line
breaks match the rasterized advances exactly, then converted back to logical units.

## The event loop and one frame

`cg_run(win, frame_fn, user)` owns the loop (`window.c`). The app never pumps events itself;
[DESIGN.md](DESIGN.md#a-callback-driven-event-loop-cg_run) explains why.

```
loop while running:
  if nothing is dirty: plat_wait_events(timeout until wake_at, or forever)
  pump_events()            platform events -> apply_event() -> cg_input
  wake_at reached?         mark dirty (caret blink, key repeat, animations)
  if dirty: run_frames(); present()
  do_pending()             window move/resize/min/max/close, outside the frame
```

**`apply_event`** turns platform events into `cg_input` state:
- mouse position, held buttons, and pressed/released edges;
- click count (1–3) for double and triple clicks;
- accumulated wheel movement;
- the list of key presses (with modifiers) and the typed UTF-8 text.

If a press and a release of the same button arrive in one batch, it runs a frame in
between. Otherwise the click would be lost.

**`run_frames`** calls `do_frame` up to 3 times while a widget sets `w->redraw`. That lets
the layout settle, for example after a dropdown closes. Transient input (edges, keys, text,
wheel) is cleared after the first pass, so it is seen exactly once.

**`do_frame`**, in order:
1. Sync size and DPI scale from the platform. Resize and clear the canvas to transparent.
2. **Resize zones first.** `chrome_hit_edges` decides whether the mouse is on an edge. If it
   is, widget hover is blocked (`block_mouse`). A press there *arms* a resize; it doesn't
   start one yet.
3. `ui_begin_frame`: reset `hot`. A click outside an open popup closes it and is swallowed.
4. `chrome_draw`: the frame fill (title and body opacity), outline, icon, title, the curved
   button track, and the three buttons (these are ordinary widgets via `ui_behave`).
5. The app's frame callback, clipped to the content rect (below the title bar).
6. `ui_popup_end`: the open dropdown popup is processed and drawn *last*, on top.
7. Title bar: a press no widget took (`!press_taken`) either arms a move, or maximizes on a
   double click. An armed move or resize becomes `pending_drag` only after the pointer moves
   3 px while held (the drag threshold).
8. `ui_end_frame`: clear a stuck `active`. A press that no focusable widget took clears
   focus.
9. Apply the cursor if it changed. Bump `shape_serial` if the outline changed.

**`present`** hands the canvas to `plat_window_present`. **`do_pending`** then runs window
operations. `plat_window_begin_drag` may block in an OS modal loop (Win32, macOS resize, the
X11 fallback). During that loop the backend calls `refresh_cb`, which pumps, runs frames and
presents. That is why no window operation may be started *inside* a frame, where it would
nest a frame within a frame.

**Sleeping.** Nothing redraws unless input arrives or a wake-up is due. Widgets that animate
call `cg_request_wakeup(win, seconds)`; the caret blink is the main user. An idle window
uses 0% CPU.

## Rendering (`render.c`)

- **Pixel format.** The canvas is `uint32_t` premultiplied `0xAARRGGBB`, with stride equal
  to the width. That is the native format of X11 ARGB visuals, Win32 `UpdateLayeredWindow`
  and CoreGraphics (BGRA little-endian), so presenting is a plain copy.
- **Colours.** `pm_from(color, opacity)` premultiplies a `cg_color`. `blend()` does
  source-over with an 8-bit coverage value.
- **Anti-aliased shapes.** Each shape has a signed-distance function; coverage is
  `clamp(0.5 − d, 0, 1)` at the pixel centre.
  - `canvas_fill_rrect` has per-corner radii (order: tl, tr, br, bl). It fills solid spans
    where coverage is certainly 1 and evaluates the SDF only near edges and corners.
  - `canvas_stroke_rrect` computes outer coverage minus inner coverage.
  - Also available: circles, rings, capsule lines, and `canvas_arc`, a thick arc with round
    caps (the button track).
- **Blits.**
  - `canvas_blit_a8` draws glyph alpha masks.
  - `canvas_blit_image` does nearest-neighbour sampling, used for pixel-art icons.
- **Clipping.** One integer rectangle on the canvas (`cx0..cx1`, `cy0..cy1`). `ui.c`
  maintains a stack of logical rects (`cg_push_clip` / `cg_pop_clip`), each intersected with
  its parent.

## Widgets (`ui.c`)

**Ids.** Every interactive widget has a `cg_id`, a 64-bit value.
- `cg_id_str("name")` is an FNV-1a hash of a string.
- `cg_id_ptr(&thing)` hashes an address.
- Some widgets use `id + k` for sub-parts (the spin box uses `id+1` and `id+2`), so never
  give two widgets ids that differ by a small integer.

**Three pieces of state per window:**
- `hot`: the mouse is over the widget and nothing else is active. Recomputed every frame.
- `active`: the widget holds the mouse (pressed and not yet released). It persists across
  frames and is cleared if its widget isn't drawn in a frame (`active_seen`).
- `focus`: the widget receives keyboard input (text editors, an open dropdown). A press that
  no focusable widget takes clears it.

**`ui_behave(w, id, rect, focusable)`** is the single interaction primitive. It returns
`hover`, `pressed`, `down`, `released` and `clicked`, and records `press_taken`.
`cg_interact` is its public face for custom widgets. `ui_hover` returns false when the
mouse is:
- over the resize border,
- over the popup overlay, or
- outside the current clip.

**Popups.** There is one dropdown popup per window (`cg_popup`).
- **Registering.** Each frame, the owning `cg_dropdown` re-registers the popup and copies
  its `cg_list_source`.
- **Drawing.** `ui_popup_end` draws the popup at the end of the frame. Because widgets drawn
  earlier can't know about it, the popup's rect from the *previous* frame
  (`w->overlay`) blocks their hover.
- **Returning a selection.** The choice comes back on the *next* frame through
  `result_id`/`result`, and `w->redraw` makes that immediate.
- **Keyboard.** While open, the popup holds keyboard focus. Typing filters, arrows move,
  Enter picks and Esc closes.

**Layout.** Layout is "rect cut". `cg_cut_left(&r, 100)` returns the left 100 px and
shrinks `r`. Combined with `cg_inset`, that is the entire layout system.

## Text editor (`textedit.c`)

- **The buffer.** `cg_textbuf` is owned by the caller. Its public fields are `data`/`len`
  (always NUL-terminated UTF-8), `caret`/`anchor` (byte offsets, always on UTF-8
  boundaries), `scroll`, and `version` (bumped on every change; compare it to detect edits).
  Private state lives in `priv`: the layout cache and the undo/redo stacks.
- **Layout.** Greedy word wrap into `tline {start, end}` byte ranges, in physical 26.6
  units. It is cached on (`version`, width, pixel size, font).
  - Spaces "hang" past the margin and never cause a wrap.
  - A caret position that is both the end of one soft-wrapped line and the start of the
    next belongs to the next line. `pos_at_x` keeps clicks at a line end on that line.
- **Undo.** A snapshot of the whole buffer is taken before an edit. Consecutive typing, or
  consecutive deleting, within 1 s coalesces into one snapshot. There are 64 levels.
- **Blink.** The caret blinks through `cg_request_wakeup` only while the editor has focus.

## Fonts (`font.c`, `fontdb.c`)

**`cg_font`** is one FreeType `FT_Face` plus an open-addressing hash of glyphs keyed by
(glyph index, size in 26.6).
- **Measuring doesn't rasterize.** It caches only the advance (`rendered = false`). A glyph
  gets a bitmap the first time it is drawn.
- **Bounded cache.** It is flushed wholesale above 16 MB of bitmaps or 65,536 entries.
- **Pens and kerning.** Pens are 26.6, snapped to whole pixels per glyph. Kerning comes from
  the `kern` table only (`FT_Get_Kerning`).
- **Symbol fonts** (MS Symbol charmap) are handled by folding `U+00xx` onto `U+F0xx`.
- **`font_run`** is the single iterator for "advance the pen by one codepoint". Every
  measuring and layout path uses it, so widths always agree with drawing.

**`fontdb`** walks the platform's font directories (`plat_font_dirs`) and reads family and
style names with FreeType.
- **Result:** it sorts and de-duplicates the faces and groups them into families. Named
  instances of variable fonts become separate styles.
- **Cost:** scanning can take seconds, so the database is loaded only on demand
  (`cg_fontdb_scan`) and can be dropped again (`cg_fontdb_release`). All family and face
  pointers and indices die on release.
- **UI font:** `fontdb_default_ui_font_path` checks `$CGUI_FONT`, then a short list of
  well-known file paths per OS, and only then falls back to a full scan. **Opening a window
  normally reads exactly one font file.**

## The platform contract (`platform.h`)

Every backend implements these functions. All sizes and positions are physical pixels.

| Function | Contract |
|---|---|
| `plat_init` / `plat_shutdown` | One-time setup: display connection, window class, NSApp. |
| `plat_window_create(title, w, h)` | `w` and `h` are **logical**. The backend applies its own DPI scale. Create hidden decorations, map or show the window. |
| `plat_window_present(px, w, h, shape_serial)` | Show a premultiplied buffer. When `shape_serial` changes, the outline changed (X11 without a compositor rebuilds its XShape mask; macOS invalidates its shadow). |
| `plat_window_size` / `plat_window_scale` | The current physical size and DPI scale. Read once per frame. |
| `plat_window_has_alpha` | True only if translucent pixels will really show what's behind the window. When false, the core draws the frame opaque. |
| `plat_window_begin_drag(edges)` | `edges` is a combination of `EDGE_*` flags; 0 means move. It is called with the button held and may block; call the refresh callback when the size changes. Must return immediately if the button is already released. |
| `plat_window_minimize` / `toggle_maximize` / `is_maximized` | Window state. |
| `plat_window_set_cursor` | `CURSOR_*`. The public `CG_CURSOR_*` values match the first five. |
| `plat_window_set_icon(argb, w, h)` | Straight-alpha pixels for the taskbar or dock. Upscale with nearest-neighbour. |
| `plat_window_set_refresh(fn, ctx)` | The callback for modal loops. |
| `plat_poll_event` / `plat_wait_events` | A per-window event queue. Motion events are coalesced. Must not consume OS events that an active modal loop is waiting for. |
| `plat_clipboard_set` / `plat_clipboard_get` | UTF-8. `get` returns a `malloc`'d string. |
| `plat_time` | Monotonic seconds. |
| `plat_font_dirs` | The directories to scan (`malloc`'d strings). |

**Event types** (`PE_*`): mouse move, down, up and leave; wheel (in notches, `+y` = up);
key down (a `CG_KEY_*` or an upper-case letter or digit, plus `CG_MOD_*`); text (UTF-8,
control characters filtered); resize; scale; close; focus; unfocus; expose; state.

Adding a backend (for example native Wayland or SDL) means one new file implementing this
table. Nothing above it changes.

## Ownership and lifetimes

| Object | Owner / rule |
|---|---|
| `cg_window` | The app: `cg_window_create` / `cg_window_destroy`. Owns the UI font, the canvas and the icon copy. |
| `cg_font` from `cg_font_load` | The caller frees it with `cg_font_free`. Independent of the fontdb. |
| `cg_font_family` / `cg_font_face` / fontdb indices | Owned by the library; valid until `cg_fontdb_release`. Keep *names* if a choice must survive a release. |
| `cg_textbuf` | The caller: `cg_textbuf_init` / `cg_textbuf_free`. |
| `cg_list_source` | Copied, but its callbacks and `user` data must stay valid until the end of the frame (the popup runs after your callback returns). |
| Icon pixels | Copied by `cg_window_set_icon`. |
| `cg_clipboard_get` result | The caller must `free()` it. |
| Glyph pointers inside `font.c` | Valid only until the next glyph lookup: the cache may grow or be flushed. Use them immediately. |
