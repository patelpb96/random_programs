# Status and roadmap

This is the current state of things as of 2026-09-30. Update it whenever an item changes,
because it is the first place to look when picking up work.

## Status by platform

| Platform | State |
|---|---|
| Linux / BSD (X11) | Works. Tested under Xvfb with no window manager, and with Openbox plus xcompmgr. Covered: rendering, input, clipboard, move and resize (edges, corners, the curved corner), maximize, minimize, close, lazy font loading. |
| Windows | Cross-compiled with MinGW and run under Wine. Covered: rendering, icon, typing, clipboard, native move and resize, close. **Not yet run on real Windows.** Under Wine, maximize snaps back ([PITFALLS #25](PITFALLS.md#platform-quirks)). |
| macOS | **Never compiled.** `platform_cocoa.m` was written blind (manual retain/release). The first job on a Mac is to build it and fix whatever breaks. |
| Wayland | Only through XWayland. |

## Known issues

1. macOS backend never compiled or run (above).
2. Under Wine, maximize snaps back to the restored size. Not checked on real Windows.
3. Kerning uses only the `kern` table. GPOS kerning, ligatures and complex scripts need
   HarfBuzz.
4. There is no glyph fallback. Characters missing from a font render as `.notdef`, which
   applies to the UI font too.
5. X11 clipboard: large pastes that use the INCR protocol aren't supported.
6. There is no IME composition window (CJK input methods). Dead keys work on X11 and
   Windows.
7. Re-entering a font tool rescans the font directories. That's deliberate, see
   [DESIGN.md](DESIGN.md#load-font-data-only-when-something-uses-it), but it could be slow on
   font-heavy machines.
8. The repo has no CI. A GitHub Actions workflow could build on Linux and MinGW and run
   `ctest`, and would catch warnings and breakage.

## Library features worth adding (cgui)

They are ordered roughly by how much new tools would benefit:

- **Single-line text input** (`cg_textfield`), with Enter meaning "submit". Many tools need
  one; today only the multi-line `cg_textedit` exists.
- **Tab focus traversal** between focusable widgets.
- **A scrollable panel** helper built on `cg_scrollbar` and `cg_wheel` (Glyph Map currently
  does this by hand).
- **Tooltips** (hover delay using `cg_request_wakeup`) and **context menus**, generalizing
  the dropdown popup.
- **Tabs or segmented control**, and a **colour picker** widget.
- **Image loading helper** (optional, vendored `stb_image.h`) feeding `cg_draw_image`.
  `cg_draw_image` would also need bilinear sampling for photos.
- **Dirty-rectangle rendering**, if large windows or animations get slow.
- **HarfBuzz shaping** (optional dependency) and **per-glyph fallback** through the font
  database.
- **Native Wayland backend.** It would be a new `platform_wayland.c`. It must implement
  the platform contract, including `begin_drag` via `xdg_toplevel_move`/`resize`.
- **Multiple windows** per process.

## preetum

- More tools: see the list in [PREETUM.md](PREETUM.md#ideas-for-future-tools).
- Saving settings (theme, opacity, accent, last font by name, last tool) to a small file in
  the user's config directory, loaded on startup.
- Menu search or filtering once there are more than about 9 tools. The number keys only
  cover 1–9.
