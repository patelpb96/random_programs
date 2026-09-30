# Design rationale

Why cgui and preetum are built the way they are. Each section gives the decision, the
reasons, the alternatives considered, and what would justify revisiting it. If you're about
to change one of these, read its section first. Several were learned the hard way, and
[PITFALLS.md](PITFALLS.md) has the details.

## Goals, in priority order

1. **A window that looks like nothing else.** A custom frame, the three window buttons along
   the curved top-right corner, real per-pixel transparency, themes, and rounded corners.
2. **Cross-platform from one C code base:** Linux/BSD (X11), Windows and macOS.
3. **Small and understandable.** C99, one dependency (FreeType) plus OS libraries, about
   7k lines. Someone new should be able to read the whole library in an afternoon.
4. **A home for many small tools.** preetum is a menu of tools. Fonts were only the first
   subject.

## Software rendering into a pixel buffer

**Decision.** Every frame is rasterized on the CPU into one premultiplied ARGB buffer, which
the OS shows as a per-pixel-alpha window.

**Why.**
- **Transparency is simplest with a buffer.** Per-pixel-alpha windows are easiest to get
  with a CPU buffer on all three OSes:
  - X11: an ARGB visual and `XPutImage`.
  - Win32: `UpdateLayeredWindow`.
  - macOS: a layer's contents.

  GPU paths for transparent, borderless windows differ per OS and per driver, and fail in
  odd ways.
- **Identical output everywhere.** Pixels are exactly the same on every OS, and a frame can
  be dumped to a file for tests (`CGUI_SCREENSHOT`).
- **Fast enough.** The UI redraws only on input. A full 1100×700 frame takes a few
  milliseconds, and an idle window costs nothing.

**Alternatives.**
- **OpenGL/Vulkan/Metal.** Faster for huge windows or animation, but brings context
  creation, transparency quirks and three shader paths.
- **SDL/GLFW for windowing.** Neither gives both transparent framebuffers and native
  move/resize on all three OSes. SDL2 also lacks per-pixel alpha windows.

**Revisit if.** You need 60 fps animation on 4K windows. First try dirty rectangles:
redraw only the changed rects. The canvas API (`canvas_*`) is the seam where a GPU backend
would slot in.

## Immediate-mode widget API

**Decision.** The app describes the UI every frame. Widgets are function calls that draw and
return interactions. Retained state lives in the app (`bool`, `float`, `cg_textbuf`) or in
the small `cg_window` fields (`hot`, `active`, `focus`, the popup).

**Why.**
- **Much less code.** There is no widget tree, no callbacks and no invalidation logic.
- **Layout is ordinary C.** The UI can't drift out of sync with the app's data.

**Costs, and how they're handled.**
- **Overlapping widgets** (the dropdown popup) are drawn after everything else.
  Earlier widgets are blocked using the popup's rect from the previous frame.
- **One-frame lag for results** (a dropdown's choice arrives the next frame) is hidden by
  `w->redraw`, which runs up to 3 passes before presenting.
- **Ids must be stable** across frames: hash a string or a pointer.

## A callback-driven event loop (`cg_run`)

**Decision.** cg_run owns the event loop. The app hands it a frame function instead of
running its own `while (poll) {...}` loop.

**Why.** The OS's native move and resize loops are modal:
- Win32 runs its own message loop inside `WM_NCLBUTTONDOWN`.
- macOS resizing runs a tracking loop.

While those loops run, the app's own loop is frozen. The window would show stale, stretched
content until the drag ended. Because cgui can call the frame function itself
(`refresh_cb`), it redraws live during those loops.

**Rule that follows.** Window operations (`pending_drag`, `pending_op`) are queued during
the frame and executed after `present` in `do_pending`, never from inside a frame.

## Handing move and resize to the OS

**Decision.** cgui only decides *what* should happen, a move or a resize from a given edge.
The OS or window manager does the moving:
- X11: `_NET_WM_MOVERESIZE`.
- Win32: `WM_NCLBUTTONDOWN` with `HTCAPTION` / `HTLEFT` and so on.
- macOS: `performWindowDragWithEvent:` for moves. Borderless windows get no native resize,
  so cgui runs its own tracking loop for resizing.

**Why.** Snapping, tiling, multi-monitor limits, maximize and restore animations,
"shake to minimize" and similar behaviour come for free and feel native.

**Drag threshold.** A press on the title bar or an edge only *arms* the drag. It starts
after the pointer moves 3 px with the button held. Starting on the press itself broke two
things:
- The window manager swallowed the second click of a double-click, so
  double-click-to-maximize never fired.
- A quick click made the WM wait for a release that had already happened.

**Fallback.** With no EWMH window manager (bare X servers), X11 grabs the pointer and moves
the window itself.

## Window chrome geometry: the curved corner

The top-right corner is the signature element.
- **One number drives it.** Let `T` be the title bar height, 54 by default. The top-right
  corner radius equals `T`, so the curve starts at the top edge and ends exactly where the
  body starts on the right edge. The arc centre is `(W − T, T)`.
- **Buttons.** They sit on a concentric arc of radius `0.68·T` and have radius `0.15·T`.
  The three angles are 86°, 53° and 20°, measured counter-clockwise from +x with y up.
  - That is the symmetric layout (78/45/12) rotated 8° counter-clockwise, so the close
    button clears the title/body separator. See `window.c` `geometry()`.
  - Change the angles there. Hit-testing uses the same numbers, so it follows automatically.
- **Track.** The dark curved strip behind the buttons is a thick arc with round caps
  (`canvas_arc`). It is clipped to the title bar so it ends flush with the separator.
- **Resize zone.** Near the curve the zone *follows the curve*: points within `margin` of
  the radius-`T` circle. It is classified into top, right, or top+right by angle. The
  buttons' outer edge (`0.83·T`) stays about 4 px inside that band at the default size.
- **Other corners** use `corner_radius`, and all corners go square when maximized. With
  "Rounded" off, the window is square but the button track stays curved: the curve belongs
  to the buttons, not only to the outline.

## Two opacities, and honest fallbacks

- **Two sliders.** The frame (title bar and outline) and the body have separate opacity
  multipliers, so a glassy title bar can sit over a solid body.
- **When translucency isn't possible.** Without an X11 compositor, "translucent" pixels
  just look darkened, not see-through.
  - `plat_window_has_alpha` reports this (checked once a second on X11, since compositors
    start and stop), and the core then draws the frame opaque.
  - The rounded outline still works through XShape. Under Wine or Windows and macOS it is
    always true.

## Fonts: scan directories with FreeType

**Decision.** Instead of fontconfig (Linux), DirectWrite (Windows) and CoreText (macOS),
cgui walks the font directories and asks FreeType for family and style names.

**Why.**
- **One code path everywhere.**
- **Every listed face really works.** FreeType is also the renderer, so anything listed
  can be drawn.
- **Variable fonts** expose their named instances as styles.

**Costs.**
- **Slow scans.** Scanning thousands of files takes time, which is why loading is lazy
  (next section).
- **Names come from the files** rather than the OS's localized names.
- **No per-glyph fallback font.** Missing glyphs render as the font's `.notdef`.

**Revisit if.** You need glyph fallback or localized names. Consider fontconfig as an
optional Linux path behind `fontdb.c`, keeping the same API.

## Load font data only when something uses it

**Decision.** Nothing enumerates fonts at startup.
- The window's UI font is found by probing a handful of well-known paths per OS. A full
  scan is only the last resort.
- In preetum, the font database, the selected font and the preview fonts belong to a
  *service* that is opened when a tool that declares `NEEDS_FONTS` is entered and closed
  when it is left.

**Why.**
- **Startup cost.** On a machine with thousands of fonts, a scan takes long enough to delay
  the first frame.
- **Memory.** Loaded faces are memory-mapped, and glyph caches grow.
- **Not every tool is about fonts.** preetum is meant to grow beyond them.

**Trade-offs.**
- **Rescanning on re-entry.** Entering a font tool after visiting the menu rescans.
  (35 ms for 168 fonts in testing; it can be around a second on font-heavy systems.) A
  "Loading fonts…" frame is presented first so the window never looks hung. If rescans
  become annoying, keep the database and release only fonts and previews in
  `fonts_close`. That is a one-line change, since `cg_fontdb_release` is separate.
- **Selection survives by name.** The chosen family and style are remembered by *name*,
  because indices are meaningless after a rescan.
- **RSS stays up.** The process's RSS doesn't drop right after release: the C allocator
  keeps freed heap pages. The memory-mapped font files *are* unmapped, and you can check
  that with `/proc/<pid>/maps`.

## Glyph cache: measure without rasterizing

(From the "Reduce font cache and database memory usage" commit.) Measuring text (layout,
wrap, caret positions) needs only advances. The cache stores advances first and rasterizes a
bitmap only when a glyph is drawn. It is bounded (16 MB of bitmaps or 65,536 entries; above
that, flush everything). Most text that gets measured is never drawn, for example scrolled-off
lines, so this saves a lot. `tests/font_cache.c` checks that widths match FreeType
exactly on both paths.

## Coordinates in logical pixels

The public API is all logical pixels. The platform reports physical size and a scale:
- X11: `Xft.dpi / 96`.
- Windows: `GetDpiForWindow`.
- macOS: `backingScaleFactor`.

Everything multiplies at one boundary. Apps never think about DPI. Pixel art (the icon)
is drawn at a whole number of physical pixels per art pixel to stay crisp.

## Layout by rect cutting

`cg_cut_left/right/top/bottom` plus `cg_inset` replace a layout engine. It reads top-down,
has no hidden state, and handles resizable windows well. The price is that you think in
terms of strips, and for grids you do the arithmetic yourself, as the Glyph Map and the menu
cards do.

## preetum: a tool registry with services

**Decision.** preetum is a table of `tool` structs:
- name and two-line blurb;
- `needs` flags;
- optional `enter` and `leave` hooks;
- an `icon` function and a `frame` function.

`main.c` owns the menu, navigation (card click, number keys, **‹ Menu**, Esc), the
appearance bar and the service lifecycle.

**Why.**
- **Adding a tool touches one new file and one table row.** Navigation, the menu card and
  the shortcut come for free.
- **Services** keep expensive shared resources out of memory unless a tool needs them. The
  diff-based `set_screen` means switching directly between two tools that share a service
  keeps it open.
- **Menu icons** run on the menu, where no services are open, so they must work without
  them. Font-based icons fall back to the UI font.

## Things deliberately left out

These are not oversights. They were cut to keep the scope small, and each is a candidate
for later (see [ROADMAP.md](ROADMAP.md)):
- text shaping (HarfBuzz), bidi and IME;
- per-glyph font fallback;
- Tab focus traversal;
- a single-line text input widget;
- tooltips and context menus;
- multi-window support. The loop assumes one window per `cg_run`; the data structures
  don't prevent more.
