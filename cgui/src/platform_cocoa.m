/*
 * macOS backend (Cocoa, manual retain/release; compile without -fobjc-arc).
 *
 *  - A borderless, non-opaque NSWindow with a clear background gives
 *    per-pixel alpha; the rendered buffer becomes a CGImage that is set as
 *    the content view's layer contents.
 *  - Moving uses -performWindowDragWithEvent: (10.11+). Resizing runs a small
 *    tracking loop that sets the frame and redraws through the refresh
 *    callback.
 *  - Coordinates handed to the core are physical pixels (points * backing
 *    scale factor), like the other backends.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "cgui.h"
#include "platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QCAP 512

@interface CGWindow : NSWindow
@end

@interface CGView : NSView {
@public
    struct plat_window *pw;
    NSTrackingArea *tracking;
}
@end

@interface CGWindowDelegate : NSObject <NSWindowDelegate> {
@public
    struct plat_window *pw;
}
@end

@interface CGAppDelegate : NSObject <NSApplicationDelegate>
@end

struct plat_window {
    CGWindow *win;
    CGView *view;
    CGWindowDelegate *delegate;
    int w, h;
    float scale;
    int min_w, min_h;
    plat_event q[QCAP];
    int qhead, qcount;
    void (*refresh)(void *);
    void *refresh_ctx;
    NSEvent *last_down;
    bool maximized;
    NSRect saved_frame;
    int cursor;
    unsigned shape_serial;
    bool in_resize;
};

static plat_window *g_main_window;
static CGColorSpaceRef g_colorspace;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void push(plat_window *pw, const plat_event *e)
{
    if (e->type == PE_MOUSE_MOVE && pw->qcount > 0) {
        plat_event *last = &pw->q[(pw->qhead + pw->qcount - 1) % QCAP];
        if (last->type == PE_MOUSE_MOVE) {
            *last = *e;
            return;
        }
    }
    if (pw->qcount == QCAP) return;
    pw->q[(pw->qhead + pw->qcount) % QCAP] = *e;
    pw->qcount++;
}

static void push_simple(plat_window *pw, int type)
{
    plat_event e;
    memset(&e, 0, sizeof e);
    e.type = type;
    push(pw, &e);
}

static void update_size(plat_window *pw)
{
    NSRect b = [pw->view bounds];
    pw->scale = (float)[pw->win backingScaleFactor];
    if (pw->scale <= 0) pw->scale = 1;
    int w = (int)(b.size.width * pw->scale + 0.5), h = (int)(b.size.height * pw->scale + 0.5);
    if (w != pw->w || h != pw->h) {
        pw->w = w;
        pw->h = h;
        plat_event e;
        memset(&e, 0, sizeof e);
        e.type = PE_RESIZE;
        e.w = w;
        e.h = h;
        push(pw, &e);
    }
}

static int mods_from(NSEventModifierFlags f)
{
    int m = 0;
    if (f & NSEventModifierFlagShift) m |= CG_MOD_SHIFT;
    if (f & NSEventModifierFlagControl) m |= CG_MOD_CTRL;
    if (f & NSEventModifierFlagOption) m |= CG_MOD_ALT;
    if (f & NSEventModifierFlagCommand) m |= CG_MOD_SUPER;
    return m;
}

static int map_key(NSEvent *e)
{
    switch ([e keyCode]) {
    case 0x33: return CG_KEY_BACKSPACE;
    case 0x75: return CG_KEY_DELETE;
    case 0x24: case 0x4C: return CG_KEY_ENTER;
    case 0x30: return CG_KEY_TAB;
    case 0x35: return CG_KEY_ESCAPE;
    case 0x7B: return CG_KEY_LEFT;
    case 0x7C: return CG_KEY_RIGHT;
    case 0x7E: return CG_KEY_UP;
    case 0x7D: return CG_KEY_DOWN;
    case 0x73: return CG_KEY_HOME;
    case 0x77: return CG_KEY_END;
    case 0x74: return CG_KEY_PAGE_UP;
    case 0x79: return CG_KEY_PAGE_DOWN;
    default: break;
    }
    NSString *s = [e charactersIgnoringModifiers];
    if ([s length] == 1) {
        unichar c = [s characterAtIndex:0];
        if (c >= 'a' && c <= 'z') return 'A' + (c - 'a');
        if (c >= 'A' && c <= 'Z') return c;
        if (c >= '0' && c <= '9') return c;
    }
    return 0;
}

static NSCursor *cursor_for(int c)
{
    switch (c) {
    case CURSOR_IBEAM: return [NSCursor IBeamCursor];
    case CURSOR_HAND: return [NSCursor pointingHandCursor];
    case CURSOR_RESIZE_EW: return [NSCursor resizeLeftRightCursor];
    case CURSOR_RESIZE_NS: return [NSCursor resizeUpDownCursor];
    case CURSOR_RESIZE_NWSE:
    case CURSOR_RESIZE_NESW: {
        /* Diagonal resize cursors are private API; fall back gracefully. */
        SEL sel = c == CURSOR_RESIZE_NWSE ? NSSelectorFromString(@"_windowResizeNorthWestSouthEastCursor")
                                          : NSSelectorFromString(@"_windowResizeNorthEastSouthWestCursor");
        if ([NSCursor respondsToSelector:sel]) return [NSCursor performSelector:sel];
        return [NSCursor crosshairCursor];
    }
    default: return [NSCursor arrowCursor];
    }
}

/* ------------------------------------------------------------------ */
/* Cocoa classes                                                       */
/* ------------------------------------------------------------------ */

@implementation CGWindow
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }
@end

@implementation CGView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)updateLayer { push_simple(pw, PE_EXPOSE); }

- (void)updateTrackingAreas
{
    if (tracking) {
        [self removeTrackingArea:tracking];
        [tracking release];
    }
    tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways |
                     NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:tracking];
    [super updateTrackingAreas];
}

- (void)mouseEvent:(NSEvent *)e type:(int)type button:(int)button
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    plat_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.button = button;
    ev.x = (int)(p.x * pw->scale);
    ev.y = (int)(p.y * pw->scale);
    ev.mods = mods_from([e modifierFlags]);
    push(pw, &ev);
}

- (void)mouseMoved:(NSEvent *)e
{
    [cursor_for(pw->cursor) set];
    [self mouseEvent:e type:PE_MOUSE_MOVE button:0];
}
- (void)mouseDragged:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_MOVE button:0]; }
- (void)rightMouseDragged:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_MOVE button:0]; }
- (void)otherMouseDragged:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_MOVE button:0]; }
- (void)mouseExited:(NSEvent *)e { push_simple(pw, PE_MOUSE_LEAVE); }

- (void)mouseDown:(NSEvent *)e
{
    [pw->last_down release];
    pw->last_down = [e retain];
    [self mouseEvent:e type:PE_MOUSE_DOWN button:0];
}
- (void)mouseUp:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_UP button:0]; }
- (void)rightMouseDown:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_DOWN button:1]; }
- (void)rightMouseUp:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_UP button:1]; }
- (void)otherMouseDown:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_DOWN button:2]; }
- (void)otherMouseUp:(NSEvent *)e { [self mouseEvent:e type:PE_MOUSE_UP button:2]; }

- (void)scrollWheel:(NSEvent *)e
{
    NSPoint p = [self convertPoint:[e locationInWindow] fromView:nil];
    plat_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = PE_WHEEL;
    ev.x = (int)(p.x * pw->scale);
    ev.y = (int)(p.y * pw->scale);
    double dx = [e scrollingDeltaX], dy = [e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) {
        dx *= 0.1;
        dy *= 0.1;
    }
    ev.wheel_x = (float)dx;
    ev.wheel_y = (float)dy;
    ev.mods = mods_from([e modifierFlags]);
    push(pw, &ev);
}

- (void)keyDown:(NSEvent *)e
{
    int mods = mods_from([e modifierFlags]);
    int key = map_key(e);
    if (key) {
        plat_event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = PE_KEY_DOWN;
        ev.key = key;
        ev.mods = mods;
        push(pw, &ev);
    }
    if ((mods & CG_MOD_SUPER) || ((mods & CG_MOD_CTRL) && !(mods & CG_MOD_ALT))) return;
    NSString *chars = [e characters];
    NSUInteger n = [chars length];
    NSMutableString *text = [NSMutableString string];
    for (NSUInteger i = 0; i < n; i++) {
        unichar c = [chars characterAtIndex:i];
        if (c < 32 || c == 127 || (c >= 0xF700 && c <= 0xF8FF)) continue; /* function keys */
        [text appendFormat:@"%C", c];
    }
    const char *utf8 = [text UTF8String];
    size_t len = utf8 ? strlen(utf8) : 0;
    for (size_t i = 0; i < len;) {
        plat_event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = PE_TEXT;
        size_t take = len - i < sizeof ev.text - 1 ? len - i : sizeof ev.text - 1;
        while (take > 0 && i + take < len && ((unsigned char)utf8[i + take] & 0xC0) == 0x80) take--;
        memcpy(ev.text, utf8 + i, take);
        push(pw, &ev);
        i += take;
    }
}

- (void)dealloc
{
    [tracking release];
    [super dealloc];
}
@end

@implementation CGWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender
{
    push_simple(pw, PE_CLOSE);
    return NO;
}
- (void)windowDidResize:(NSNotification *)n { update_size(pw); }
- (void)windowDidChangeBackingProperties:(NSNotification *)n
{
    update_size(pw);
    pw->view.layer.contentsScale = pw->scale;
    push_simple(pw, PE_SCALE);
}
- (void)windowDidBecomeKey:(NSNotification *)n { push_simple(pw, PE_FOCUS); }
- (void)windowDidResignKey:(NSNotification *)n { push_simple(pw, PE_UNFOCUS); }
- (void)windowDidDeminiaturize:(NSNotification *)n { push_simple(pw, PE_EXPOSE); }
@end

@implementation CGAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    /* Cmd+Q: let the app's loop shut down cleanly instead of exit(). */
    if (g_main_window) {
        push_simple(g_main_window, PE_CLOSE);
        return NSTerminateCancel;
    }
    return NSTerminateNow;
}
@end

/* ------------------------------------------------------------------ */
/* Platform API                                                        */
/* ------------------------------------------------------------------ */

bool plat_init(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp setDelegate:[[CGAppDelegate alloc] init]];

        NSMenu *bar = [[[NSMenu alloc] init] autorelease];
        NSMenuItem *app_item = [[[NSMenuItem alloc] init] autorelease];
        [bar addItem:app_item];
        NSMenu *app_menu = [[[NSMenu alloc] init] autorelease];
        [app_menu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        [app_item setSubmenu:app_menu];
        [NSApp setMainMenu:bar];

        [NSApp finishLaunching];
        g_colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    }
    return true;
}

void plat_shutdown(void) { CGColorSpaceRelease(g_colorspace); }

double plat_time(void) { return [[NSProcessInfo processInfo] systemUptime]; }

plat_window *plat_window_create(const char *title, int w, int h)
{
    plat_window *pw = (plat_window *)calloc(1, sizeof *pw);
    @autoreleasepool {
        NSRect rect = NSMakeRect(0, 0, w, h);
        NSWindowStyleMask style = NSWindowStyleMaskBorderless | NSWindowStyleMaskMiniaturizable;
        pw->win = [[CGWindow alloc] initWithContentRect:rect
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
        [pw->win setOpaque:NO];
        [pw->win setBackgroundColor:[NSColor clearColor]];
        [pw->win setHasShadow:YES];
        [pw->win setReleasedWhenClosed:NO];
        [pw->win setAcceptsMouseMovedEvents:YES];
        [pw->win setTitle:[NSString stringWithUTF8String:title]];

        pw->view = [[CGView alloc] initWithFrame:rect];
        pw->view->pw = pw;
        [pw->view setWantsLayer:YES];
        pw->view.layer.contentsGravity = kCAGravityTopLeft;
        pw->view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
        [pw->win setContentView:pw->view];
        [pw->win makeFirstResponder:pw->view];

        pw->delegate = [[CGWindowDelegate alloc] init];
        pw->delegate->pw = pw;
        [pw->win setDelegate:pw->delegate];

        [pw->win center];
        [pw->win makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        update_size(pw);
        pw->view.layer.contentsScale = pw->scale;
    }
    if (!g_main_window) g_main_window = pw;
    return pw;
}

void plat_window_destroy(plat_window *pw)
{
    if (!pw) return;
    if (g_main_window == pw) g_main_window = NULL;
    @autoreleasepool {
        [pw->win setDelegate:nil];
        [pw->win orderOut:nil];
        [pw->win close];
        [pw->win release];
        [pw->view release];
        [pw->delegate release];
        [pw->last_down release];
    }
    free(pw);
}

void plat_window_set_title(plat_window *pw, const char *title)
{
    @autoreleasepool {
        [pw->win setTitle:[NSString stringWithUTF8String:title]];
    }
}

void plat_window_set_min_size(plat_window *pw, int w, int h)
{
    pw->min_w = w;
    pw->min_h = h;
    float s = pw->scale > 0 ? pw->scale : 1;
    [pw->win setContentMinSize:NSMakeSize(w / s, h / s)];
}

void plat_window_size(plat_window *pw, int *w, int *h)
{
    *w = pw->w;
    *h = pw->h;
}

float plat_window_scale(plat_window *pw) { return pw->scale; }

bool plat_window_has_alpha(plat_window *pw)
{
    (void)pw;
    return true;
}

void plat_window_set_refresh(plat_window *pw, void (*fn)(void *), void *ctx)
{
    pw->refresh = fn;
    pw->refresh_ctx = ctx;
}

void plat_window_present(plat_window *pw, const uint32_t *pixels, int w, int h, unsigned shape_serial)
{
    @autoreleasepool {
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)pixels, (CFIndex)w * h * 4);
        CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
        CGImageRef img = CGImageCreate((size_t)w, (size_t)h, 8, 32, (size_t)w * 4, g_colorspace,
                                       kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little, prov, NULL,
                                       false, kCGRenderingIntentDefault);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        pw->view.layer.contents = (id)img;
        pw->view.layer.contentsScale = pw->scale;
        [CATransaction commit];
        CGImageRelease(img);
        CGDataProviderRelease(prov);
        CFRelease(data);
        if (shape_serial != pw->shape_serial) {
            /* The shadow follows the window's alpha; recompute it. */
            pw->shape_serial = shape_serial;
            [pw->win invalidateShadow];
        }
    }
}

void plat_window_begin_drag(plat_window *pw, int edges)
{
    if (!([NSEvent pressedMouseButtons] & 1)) return;
    if (edges == 0) {
        if (pw->last_down) [pw->win performWindowDragWithEvent:pw->last_down];
        return;
    }
    /* Resize: track the mouse ourselves until the button is released. */
    pw->in_resize = true;
    pw->maximized = false;
    NSRect start = [pw->win frame];
    NSPoint m0 = [NSEvent mouseLocation];
    NSSize min = [pw->win contentMinSize];
    for (;;) {
        @autoreleasepool {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp
                                            untilDate:[NSDate distantFuture]
                                               inMode:NSEventTrackingRunLoopMode
                                              dequeue:YES];
            if ([e type] == NSEventTypeLeftMouseUp) break;
            NSPoint m = [NSEvent mouseLocation];
            CGFloat dx = m.x - m0.x, dy = m.y - m0.y; /* screen coords: y grows upwards */
            NSRect f = start;
            if (edges & EDGE_LEFT) {
                CGFloat nw = fmax(min.width, start.size.width - dx);
                f.origin.x = start.origin.x + start.size.width - nw;
                f.size.width = nw;
            }
            if (edges & EDGE_RIGHT) f.size.width = fmax(min.width, start.size.width + dx);
            if (edges & EDGE_TOP) f.size.height = fmax(min.height, start.size.height + dy);
            if (edges & EDGE_BOTTOM) {
                CGFloat nh = fmax(min.height, start.size.height - dy);
                f.origin.y = start.origin.y + start.size.height - nh;
                f.size.height = nh;
            }
            [pw->win setFrame:f display:NO];
            update_size(pw);
            if (pw->refresh) pw->refresh(pw->refresh_ctx);
        }
    }
    pw->in_resize = false;
    push_simple(pw, PE_MOUSE_UP);
}

void plat_window_minimize(plat_window *pw) { [pw->win miniaturize:nil]; }

void plat_window_toggle_maximize(plat_window *pw)
{
    if (pw->maximized) {
        [pw->win setFrame:pw->saved_frame display:YES animate:YES];
        pw->maximized = false;
    } else {
        pw->saved_frame = [pw->win frame];
        NSRect vis = [[pw->win screen] visibleFrame];
        [pw->win setFrame:vis display:YES animate:YES];
        pw->maximized = true;
    }
    update_size(pw);
    push_simple(pw, PE_STATE);
}

bool plat_window_is_maximized(plat_window *pw) { return pw->maximized; }

void plat_window_set_cursor(plat_window *pw, int cursor)
{
    if (cursor < 0 || cursor >= CURSOR_COUNT) return;
    pw->cursor = cursor;
    [cursor_for(cursor) set];
}

static void pump(NSDate *until)
{
    @autoreleasepool {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:until
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        while (e) {
            [NSApp sendEvent:e];
            e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                   untilDate:[NSDate distantPast]
                                      inMode:NSDefaultRunLoopMode
                                     dequeue:YES];
        }
    }
}

bool plat_poll_event(plat_window *pw, plat_event *ev)
{
    /* During the resize tracking loop, leave AppKit's queue alone so the
     * loop itself receives the mouse-up. */
    if (pw->qcount == 0 && !pw->in_resize) pump([NSDate distantPast]);
    if (pw->qcount == 0) return false;
    *ev = pw->q[pw->qhead];
    pw->qhead = (pw->qhead + 1) % QCAP;
    pw->qcount--;
    return true;
}

void plat_wait_events(plat_window *pw, int timeout_ms)
{
    if (pw->qcount > 0) return;
    @autoreleasepool {
        NSDate *until = timeout_ms < 0 ? [NSDate distantFuture]
                                       : [NSDate dateWithTimeIntervalSinceNow:timeout_ms / 1000.0];
        pump(until);
    }
}

void plat_clipboard_set(plat_window *pw, const char *text)
{
    (void)pw;
    @autoreleasepool {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        [pb setString:[NSString stringWithUTF8String:text ? text : ""] forType:NSPasteboardTypeString];
    }
}

char *plat_clipboard_get(plat_window *pw)
{
    (void)pw;
    char *out = NULL;
    @autoreleasepool {
        NSString *s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        const char *u = s ? [s UTF8String] : NULL;
        if (u) out = strdup(u);
    }
    return out;
}

int plat_font_dirs(char **out, int max)
{
    int n = 0;
    const char *home = getenv("HOME");
    const char *sys[] = { "/System/Library/Fonts", "/Library/Fonts" };
    for (int i = 0; i < 2 && n < max; i++) out[n++] = strdup(sys[i]);
    if (home && n < max) {
        size_t len = strlen(home) + 32;
        char *p = (char *)malloc(len);
        snprintf(p, len, "%s/Library/Fonts", home);
        out[n++] = p;
    }
    return n;
}
