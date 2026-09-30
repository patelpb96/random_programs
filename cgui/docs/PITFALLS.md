# Pitfalls and things to look out for

Every item here is a bug that really happened during development, or a trap the design
walks close to. Each lists the **symptom**, the **cause**, and the **rule** that prevents
it. The last sections cover how to test, and the quirks of the development environment.

## Input and event handling

**1. Fast clicks get lost.**
- *Symptom:* a quick click does nothing.
- *Cause:* the press and the release arrive in the same event batch. If only state is
  tracked, the frame sees neither edge.
- *Rule:* `apply_event` runs a frame between two edges of the same button
  (`window.c`). Keep that, and keep per-frame edge flags (`pressed`/`released`) separate
  from held state (`down`).

**2. A button click also starts a window move.**
- *Symptom:* clicking a title-bar button makes the window follow the mouse. On X11 without
  a WM the app then hangs in the drag loop.
- *Cause:* in the one frame that saw both press and release, the button set and cleared
  `active`, so the "nobody took the press" check thought the title bar was free.
- *Rule:* the title-bar check uses `press_taken`, which `ui_behave` sets on any press,
  not `active == 0`.

**3. Double-click to maximize doesn't work under a window manager.**
- *Cause:* the move was handed to the WM on the first press, and the WM swallowed the
  second click.
- *Rule:* the drag threshold (3 px) *arms* on press and starts only on movement. Never call
  `plat_window_begin_drag` on a bare press.

**4. The manual X11 drag loop hangs.**
- *Cause:* the loop waited for a ButtonRelease that had already been processed.
- *Rule:* every `begin_drag` implementation first checks that the button is still held
  (`XQueryPointer`, `GetAsyncKeyState`, `[NSEvent pressedMouseButtons]`) and returns if not.

**5. Nested event pumping steals the modal loop's events.**
- *Cause:* during a modal resize loop the refresh callback pumps events. If it dequeues the
  mouse-up the loop is waiting for, the loop never ends.
- *Rule:*
  - The X11 fallback routes events through one handler that knows about the drag.
  - Cocoa's `plat_poll_event` does not pump while `in_resize`.
  - Any new backend needs the same care.

**6. Global keyboard shortcuts fight with focused widgets.**
- *Example:* Glyph Map's arrow keys moved the selection *and* the highlight in an open
  dropdown.
- *Rule:* screen-level key handling runs only when `cg_focused(win) == 0`.
  - An open dropdown holds focus, and so does a text editor.
  - Esc-to-menu in preetum follows the same rule, so Esc first closes a popup.

**7. Widget ids collide.**
- *Cause:* `cg_spinbox` uses `id+1` and `id+2` for its −/+ buttons, and the accent
  swatches use `cg_id_str("accent") + k`.
- *Rule:* derive ids by hashing distinct strings or pointers. Never use `base + small int`
  for unrelated widgets.

**8. A widget that disappears while held.**
- If the active widget isn't drawn in a frame, `active` would stick and block all hover.
- `ui_end_frame` clears it when `active_seen` is false. Don't remove that.

## Rendering and the window

**9. XShape eats clicks at the corners** (X11 with no compositor).
- The rounded outline is cut with XShape, and cut-away pixels don't receive input. A click
  on the exact corner pixel falls through to the desktop.
- This is expected. Test resizes from a point slightly inside the curve.

**10. Translucency without a compositor looks dark, not see-through.**
- *Rule:* respect `plat_window_has_alpha`. It forces opacity 1 for the frame and body.
  `CGUI_SCREENSHOT` pretends alpha is available so the dumps show the checkerboard.

**11. The window cursor is never set.**
- *Cause:* `cursor_shown` started at `CURSOR_ARROW`, so the first "set arrow" was skipped.
  A bare X server then shows its default "X" cursor.
- *Rule:* `cursor_shown` starts at −1.

**12. Changing the button geometry.**
- The angles, radii and track live in `window.c` `geometry()`, and hit-testing uses the
  same values.
- After changing them, check:
  - the close button stays above `y = T` (the separator);
  - the track stays clipped to the title bar;
  - the outer edge of the buttons stays inside the curved resize band
    (`d < T − resize_margin`).
- Take a `CGUI_SCREENSHOT` and zoom into the corner. The README's `docs/corner.png` was made
  that way; the command is below.

**13. Pixel art goes blurry.**
- Draw icons at an *integer* number of physical pixels per art pixel. The title-bar icon
  code does `floor(target / icon_w)` in physical pixels.
- `cg_draw_image` samples nearest-neighbour, but a non-integer scale still gives uneven
  pixel sizes.

## Fonts and text

**14. Glyph pointers go stale.**
- `get_glyph` returns a pointer into a hash table that can grow (rehash) or be flushed
  (above 16 MB or 65,536 entries) on the *next* lookup.
- Use the pointer immediately. Never keep it across another lookup.

**15. Fontdb pointers and indices die on release.**
- `cg_fontdb_release()` frees every `cg_font_family`/`cg_font_face` and makes indices
  meaningless.
- preetum remembers the selection by *name* (`keep_family` / `keep_style`) and resolves it
  after a rescan. Anything else that must survive a release needs the same treatment.
- `cg_font` objects stay valid; they don't depend on the database.

**16. Menu-card icons run with services closed.**
- `tool.icon` is drawn on the main menu, where no fonts are loaded.
- Use `app->font ? app->font : cg_ui_font(win)`, and never call `fonts_picker` or the
  `cg_fontdb_*` functions from an icon.

**17. Don't scan fonts eagerly.**
- `cg_fontdb_scan()` can take seconds on font-heavy machines.
- Call it only from code that runs while a font tool is open, which in preetum means through
  `fonts_ready`.
- `cg_window_create` must not trigger a scan either. Check that `known_ui_font()` in
  `fontdb.c` still covers common installs when adding platforms.
- Verify with `strace -e trace=openat` (see below).

**18. Dropdown sources must live until the end of the frame.**
- The popup is processed in `ui_popup_end`, after your frame callback returns, and calls
  `label()` / `preview_font()` then.
- Don't point `user` at a stack variable of a helper that has already returned.

**19. Changing the family invalidates the style index.**
- A style index is only meaningful for its family.
- After the family changes, reset it (`a->style = fam->regular`) before any code calls
  `style_label` again. `fonts_picker` draws the style dropdown *before* the family dropdown
  for this reason.

**20. UTF-8 boundaries.**
- Caret and anchor must always sit on UTF-8 boundaries. `cg_textbuf_set` clamps them when
  the text changes underneath.
- Build new editing operations on `utf8_prev` / `utf8_next`, never `±1`.

**21. Soft-wrap caret affinity.**
- The end of a soft-wrapped line is the same offset as the start of the next line.
- `line_of()` resolves it to the next line. `pos_at_x()` and End step back one character
  on soft-wrapped lines, so clicks and End keep the caret on the visible line.

**22. Kerning is partial.**
- Only the `kern` table is used. Many modern OpenType fonts kern through GPOS, so their
  kerning isn't applied.
- That is a known limitation, not a bug in the measuring code. Measuring and drawing
  share `font_run`, so they always agree.

## Platform quirks

**23. Win32: helper names can collide with the Windows API.** A static helper called
`mouse_event` clashed with the Win32 function of that name. Don't name helpers after
Windows API functions; that helper is now `queue_mouse`.

**24. Win32 under Wine: window size vs client size.** Wine keeps a hidden 3 px frame inset
even though `WM_NCCALCSIZE` returns 0. Sizing from the client rect made
`UpdateLayeredWindow` shrink the window. The backend sizes from `GetWindowRect`, which on
real Windows is identical.

**25. Win32 under Wine: maximize snaps back.** Wine's X11 driver sends its own `WM_SIZE`
back to the restored size right after maximizing, and still reports "zoomed". Tracing
showed cgui presenting the right size before that. It hasn't been checked on real Windows;
if it reproduces there, investigate `WM_WINDOWPOSCHANGING`.

**26. macOS has never been compiled.** `platform_cocoa.m` was written without an SDK. It
uses manual retain/release: don't build it with `-fobjc-arc`.
- The likely problems are small API or constant-name mismatches.
- Fix them, then remove this item and update the README's status section.

**27. X11 keyboard focus in tests.** Under a bare Xvfb (no WM), nothing gives the window
keyboard focus, so `xdotool key` may go nowhere. Either run `openbox &` first, or drive the
test with mouse clicks.

**28. Memory numbers.** After closing a service, RSS doesn't shrink much: glibc keeps freed
heap pages. Check `/proc/<pid>/maps` for unmapped font files instead.

## Testing recipes

The dev container has Xvfb, xdotool, ImageMagick, MinGW, Wine, Openbox and xcompmgr (all
installed with `apt-get`). A normal desktop Linux works the same way without Xvfb.

```sh
cd cgui
make                                   # library + build/preetum (warnings are errors in spirit: keep it clean)
cmake -S . -B /tmp/b && cmake --build /tmp/b && (cd /tmp/b && ctest)   # includes tests/font_cache.c

# Headless run
Xvfb :99 -screen 0 1400x900x24 &  export DISPLAY=:99
openbox &            # optional: EWMH window manager (move/resize/maximize via WM, key focus)
xcompmgr &           # optional: compositor (real translucency)

# Screenshot of one screen, composited over a checkerboard, then exit:
CGUI_SCREENSHOT=/tmp/menu.ppm ./build/preetum && convert /tmp/menu.ppm /tmp/menu.png
PREETUM_TOOL=2 CGUI_SCREENSHOT=/tmp/glyphs.ppm ./build/preetum     # open tool 2 directly
# Zoom the button corner (as docs/corner.png):
convert /tmp/menu.ppm -crop 150x100+970+0 -filter point -resize 300% /tmp/corner.png

# Drive it: click cards, type, drag edges
./build/preetum & sleep 1
eval $(xdotool search --class cgui getwindowgeometry --shell)   # X, Y, WIDTH, HEIGHT
xdotool mousemove $((X+560)) $((Y+470)) click 1                  # a menu card
xwd -root -silent | convert xwd:- /tmp/shot.png                   # screenshot the screen

# Button centre i (0 = minimize, 1 = maximize, 2 = close); default title height T = 54:
python3 -c "import math;T=54;W=$WIDTH;a=(86,53,20)[2];print(int($X+W-T+0.68*T*math.cos(math.radians(a))), int($Y+T-0.68*T*math.sin(math.radians(a))))"

# Prove lazy font loading: only the UI font should be opened on the menu
CGUI_SCREENSHOT=/tmp/m.ppm strace -f -e trace=openat -o /tmp/st.txt ./build/preetum
grep -E '\.(ttf|otf|ttc)"' /tmp/st.txt
grep -E '\.(ttf|otf|ttc)' /proc/<pid>/maps      # while running: fonts mapped right now

# Windows build (cross-compile) and run under Wine.
# FreeType for MinGW: the freetype-py wheel ships a DLL (the proxy blocks source downloads):
pip download --no-deps --only-binary=:all: --platform win_amd64 --python-version 3.11 freetype-py -d wheels
#   unzip it, copy freetype/libfreetype.dll to both libfreetype.dll and freetype.dll next to the exe
x86_64-w64-mingw32-gcc -O2 -std=c99 -Iinclude -Isrc -I/usr/include/freetype2 \
  src/render.c src/font.c src/fontdb.c src/theme.c src/ui.c src/textedit.c src/window.c \
  src/platform_win32.c apps/preetum/*.c libfreetype.dll -lgdi32 -luser32 -lm -mwindows -o preetum.exe
WINEPREFIX=/tmp/wp wine64 wineboot -i        # the prefix has no fonts: copy some .ttf into drive_c/windows/Fonts
WINEPREFIX=/tmp/wp wine64 preetum.exe &      # startup takes ~5-9 s under Wine; wineserver -k to stop
```

What to check after any change to the window chrome, input or the platform layer:
- typing and selection;
- the dropdown (open, filter by typing, pick with the keyboard, scroll with the wheel);
- moving (title-bar drag);
- resizing from an edge, a corner and the curved top-right corner;
- maximize and restore, by button and by double click;
- minimize;
- close;
- idle CPU. It should be 0: read `/proc/<pid>/stat` fields 14 and 15 over a few seconds.
