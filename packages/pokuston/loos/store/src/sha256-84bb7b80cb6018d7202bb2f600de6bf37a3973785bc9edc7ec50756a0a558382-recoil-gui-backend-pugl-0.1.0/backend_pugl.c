/*
 * recoil-gui Pugl backend — implements the rgui_* contract (recoil-gui.h)
 * over Pugl (window/view embedding + event pumping) and NanoVG GL2
 * (immediate-mode drawing). Colors arrive packed
 * 0xRRGGBBAA. One shared Pugl world/view/NanoVG context (singleton) backs
 * both standalone (rgui_open_window/rgui_run_loop) and embedded
 * (rgui_attach/rgui_tick) modes, mutually exclusive per the recoil-gui.h
 * contract.
 *
 * Rendering model: Pugl only allows GL calls while handling a PUGL_EXPOSE
 * event (the context is active/current only then). rgui_begin_frame/
 * rgui_end_frame -- called by the app's `frame` callback -- are therefore
 * just nvgBeginFrame/nvgEndFrame; the actual trigger for `frame` running at
 * all is our own PUGL_EXPOSE handler, invoked in turn by puglUpdate() after
 * we've asked for a redraw with puglObscureView(). Both rgui_run_loop
 * (standalone) and rgui_tick (embedded) drive this same obscure-then-update
 * cycle -- the standard Pugl plugin integration pattern. Embedded ticks come
 * from a native CFRunLoopTimer on macOS and from the host's timer extension
 * elsewhere; see the RGUI_NATIVE_TICK_TIMER block below.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#if defined(__linux__)
#define GL_GLEXT_PROTOTYPES
#endif
#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <pugl/pugl.h>
#include <pugl/gl.h>

#define NANOVG_GL2_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

#include "font_data.h"

typedef void (*rgui_frame_fn)(void);
typedef struct rgui_context rgui_context;
typedef void (*rgui_context_frame_fn)(void *user);

void rgui_context_tick(rgui_context *context);

/* ---- singleton backend state ---- */
static PuglWorld     *g_world = NULL;
static PuglView      *g_view = NULL;
static NVGcontext    *g_vg = NULL;
static rgui_frame_fn  g_frame = NULL;
static bool           g_should_close = false;
static int            g_width = 0, g_height = 0;
static int            g_target_fps = 60;
static int            g_font = -1;

/* Input: MVP tracks only the left button (index 0) on every Pugl backend. */
static double g_mouse_x = 0.0, g_mouse_y = 0.0;
static bool   g_mouse_down = false;
static bool   g_mouse_pressed_edge = false;

void rgui_tick(void);

/* Embedded mode has no event loop of its own -- redraw/hit-test only run
 * inside a tick, which the CLAP host is supposed to drive via the OPTIONAL
 * clap.timer-support extension. Not every host implements it (observed:
 * Bitwig does not expose it to this plugin), which would leave the view
 * rendered once at realize and then permanently unresponsive.
 *
 * On macOS, drive the tick ourselves from a native CFRunLoopTimer on the main
 * run loop instead of depending on the host -- added to kCFRunLoopCommonModes
 * so it keeps firing during modal/tracking loops (e.g. an active mouse drag),
 * not just the default mode. CoreFoundation is a plain C API, so this needs no
 * Objective-C. On Linux the host drives the tick; X11 embedding has no
 * equivalent process-wide main run loop to hang a timer on. */
#if defined(__APPLE__)
#define RGUI_NATIVE_TICK_TIMER 1
#define RGUI_NATIVE_TICK_INTERVAL (1.0 / 60.0)

static CFRunLoopTimerRef rgui__timer_start(CFRunLoopTimerCallBack callback,
                                           void *info) {
    CFRunLoopTimerContext context = {0, info, NULL, NULL, NULL};
    CFRunLoopTimerRef timer =
        CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent(),
                             RGUI_NATIVE_TICK_INTERVAL, 0, 0, callback,
                             &context);
    if (timer) CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    return timer;
}

static void rgui__timer_stop(CFRunLoopTimerRef *timer) {
    if (!timer || !*timer) return;
    CFRunLoopTimerInvalidate(*timer);
    CFRelease(*timer);
    *timer = NULL;
}

static CFRunLoopTimerRef g_embed_timer = NULL;

static void rgui__embed_timer_cb(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    (void)info;
    rgui_tick();
}
#endif /* __APPLE__ */

static inline NVGcolor rgui__color(uint32_t c) {
    unsigned char r = (unsigned char)((c >> 24) & 0xFFu);
    unsigned char g = (unsigned char)((c >> 16) & 0xFFu);
    unsigned char b = (unsigned char)((c >> 8) & 0xFFu);
    unsigned char a = (unsigned char)(c & 0xFFu);
    return nvgRGBA(r, g, b, a);
}

static PuglStatus rgui__on_event(PuglView *view, const PuglEvent *event) {
    (void)view;
    switch (event->type) {
    case PUGL_CONFIGURE:
        g_width = (int)event->configure.width;
        g_height = (int)event->configure.height;
        break;
    case PUGL_EXPOSE:
        if (g_frame) g_frame();
        break;
    case PUGL_BUTTON_PRESS:
        if (event->button.button == 0U) {
            g_mouse_down = true;
            g_mouse_pressed_edge = true;
        }
        break;
    case PUGL_BUTTON_RELEASE:
        if (event->button.button == 0U) g_mouse_down = false;
        break;
    case PUGL_MOTION:
        g_mouse_x = event->motion.x;
        g_mouse_y = event->motion.y;
        break;
    case PUGL_CLOSE:
        g_should_close = true;
        break;
    default:
        break;
    }
    return PUGL_SUCCESS;
}

static void rgui__teardown(void) {
#if defined(RGUI_NATIVE_TICK_TIMER)
    rgui__timer_stop(&g_embed_timer);
#endif
    /* nvgDeleteGL2 issues GL deletes (program/shaders/textures) and needs a
     * current context, same as nvgCreateGL2 did at setup. g_vg is only ever
     * set alongside g_view/g_world (see rgui__create_view), so g_view is
     * still valid here -- delete the NanoVG context (with the GL context
     * entered) BEFORE freeing the view/world that own it. */
    if (g_vg) {
        puglEnterContext(g_view);
        nvgDeleteGL2(g_vg);
        puglLeaveContext(g_view);
        g_vg = NULL;
    }
    if (g_view) { puglFreeView(g_view); g_view = NULL; }
    if (g_world) { puglFreeWorld(g_world); g_world = NULL; }
    g_frame = NULL;
    g_should_close = false;
    g_font = -1;
}

static bool rgui__create_view(int width, int height, const char *title, PuglNativeView parent) {
    if (g_world || g_view) rgui__teardown();

    PuglWorld *world = puglNewWorld(parent ? PUGL_MODULE : PUGL_PROGRAM, 0);
    if (!world) return false;
    puglSetWorldString(world, PUGL_CLASS_NAME, "RecoilGuiPugl");

    PuglView *view = puglNewView(world);
    if (!view) { puglFreeWorld(world); return false; }

    puglSetHandle(view, NULL);
    puglSetBackend(view, puglGlBackend());
    puglSetEventFunc(view, rgui__on_event);
    puglSetViewHint(view, PUGL_CONTEXT_API, PUGL_OPENGL_API);
    puglSetViewHint(view, PUGL_CONTEXT_VERSION_MAJOR, 2);
    puglSetViewHint(view, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
    puglSetViewHint(view, PUGL_RESIZABLE, PUGL_FALSE);
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, (unsigned)width, (unsigned)height);
    if (title) puglSetViewString(view, PUGL_WINDOW_TITLE, title);
    if (parent) {
        if (puglSetParent(view, parent) != PUGL_SUCCESS) {
            puglFreeView(view);
            puglFreeWorld(world);
            return false;
        }
    }

    if (puglRealize(view) != PUGL_SUCCESS) {
        puglFreeView(view);
        puglFreeWorld(world);
        return false;
    }
    if (puglShow(view, PUGL_SHOW_PASSIVE) != PUGL_SUCCESS) {
        puglFreeView(view);
        puglFreeWorld(world);
        return false;
    }

    /* nvgCreateGL2/nvgCreateFontMem issue GL calls (glCreateProgram et al)
     * and need a current GL context. Pugl only makes the context current for
     * the duration of a PUGL_REALIZE/expose callback; puglRealize/puglShow
     * above have already returned, so nothing is current here. puglEnterContext
     * is Pugl's documented escape hatch for exactly this "one-time GL setup
     * outside the render loop" case (see pugl/gl.h) -- enter, do setup, leave. */
    if (puglEnterContext(view) != PUGL_SUCCESS) {
        puglFreeView(view);
        puglFreeWorld(world);
        return false;
    }

    NVGcontext *vg = nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!vg) {
        puglLeaveContext(view);
        puglFreeView(view);
        puglFreeWorld(world);
        return false;
    }
    int font = nvgCreateFontMem(vg, "sans", (unsigned char *)rgui_pugl_font_data,
                                 (int)rgui_pugl_font_data_len, 0);
    if (font == -1) {
        nvgDeleteGL2(vg);
        puglLeaveContext(view);
        puglFreeView(view);
        puglFreeWorld(world);
        return false;
    }

    if (puglLeaveContext(view) != PUGL_SUCCESS) {
        nvgDeleteGL2(vg);
        puglFreeView(view);
        puglFreeWorld(world);
        return false;
    }

    g_world = world;
    g_view = view;
    g_vg = vg;
    g_font = font;
    g_width = width;
    g_height = height;
    g_should_close = false;
    return true;
}

/* ---- standalone lifecycle ---- */
bool rgui_open_window(int width, int height, const char *title) {
    return rgui__create_view(width, height, title, 0);
}
bool rgui_window_should_close(void) { return g_should_close; }
void rgui_close_window(void) { rgui__teardown(); }
void rgui_set_target_fps(int fps) { g_target_fps = fps > 0 ? fps : 60; }
void rgui_run_loop(rgui_frame_fn frame) {
    g_frame = frame;
    while (!g_should_close) {
        double dt = 1.0 / (double)(g_target_fps > 0 ? g_target_fps : 60);
        puglObscureView(g_view);
        puglUpdate(g_world, dt);
        g_mouse_pressed_edge = false;
    }
}

/* ---- embedded lifecycle ---- */
bool rgui_attach(void *parent, int width, int height) {
    if (!parent) return false;
    if (!rgui__create_view(width, height, NULL, (PuglNativeView)(uintptr_t)parent)) {
        return false;
    }
#if defined(RGUI_NATIVE_TICK_TIMER)
    g_embed_timer = rgui__timer_start(rgui__embed_timer_cb, NULL);
#endif
    return true;
}
void rgui_detach(void) { rgui__teardown(); }
void rgui_set_frame(rgui_frame_fn frame) { g_frame = frame; }
void rgui_tick(void) {
    if (!g_view || !g_frame) return;
    puglObscureView(g_view);
    puglUpdate(g_world, 0.0);
    g_mouse_pressed_edge = false;
}

/* ---- per-frame drawing ---- */
void rgui_begin_frame(void) {
    if (!g_vg) return;
    nvgBeginFrame(g_vg, (float)g_width, (float)g_height, 1.0f);
}
void rgui_end_frame(void) {
    if (!g_vg) return;
    nvgEndFrame(g_vg);
}
void rgui_clear(uint32_t rgba) {
    if (!g_vg) return;
    glViewport(0, 0, g_width, g_height);
    NVGcolor c = rgui__color(rgba);
    glClearColor(c.r, c.g, c.b, c.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}
void rgui_draw_rect(int x, int y, int w, int h, uint32_t rgba) {
    if (!g_vg) return;
    nvgBeginPath(g_vg);
    nvgRect(g_vg, (float)x, (float)y, (float)w, (float)h);
    nvgFillColor(g_vg, rgui__color(rgba));
    nvgFill(g_vg);
}
void rgui_draw_rect_gradient_v(int x, int y, int w, int h, uint32_t top_rgba, uint32_t bottom_rgba) {
    if (!g_vg) return;
    NVGpaint paint = nvgLinearGradient(g_vg, (float)x, (float)y, (float)x, (float)(y + h),
                                        rgui__color(top_rgba), rgui__color(bottom_rgba));
    nvgBeginPath(g_vg);
    nvgRect(g_vg, (float)x, (float)y, (float)w, (float)h);
    nvgFillPaint(g_vg, paint);
    nvgFill(g_vg);
}
void rgui_draw_image(const unsigned char *pixels, int x, int y, int w, int h, int stride) {
    int img;
    NVGpaint paint;
    unsigned char *packed = NULL;
    int row;
    if (!g_vg || pixels == NULL || w <= 0 || h <= 0) return;
    if (stride != w * 4) {
        packed = (unsigned char *)malloc((size_t)w * (size_t)h * 4u);
        if (packed == NULL) return;
        for (row = 0; row < h; row++)
            memcpy(packed + (size_t)row * (size_t)w * 4u,
                   pixels + (size_t)row * (size_t)stride, (size_t)w * 4u);
        pixels = packed;
    }
    img = nvgCreateImageRGBA(g_vg, w, h, NVG_IMAGE_NEAREST, pixels);
    free(packed);
    if (img == 0) return;
    paint = nvgImagePattern(g_vg, (float)x, (float)y, (float)w, (float)h, 0.0f, img, 1.0f);
    nvgBeginPath(g_vg);
    nvgRect(g_vg, (float)x, (float)y, (float)w, (float)h);
    nvgFillPaint(g_vg, paint);
    nvgFill(g_vg);
    nvgDeleteImage(g_vg, img);
}
void rgui_draw_line(int x0, int y0, int x1, int y1, float thick, uint32_t rgba) {
    if (!g_vg) return;
    nvgBeginPath(g_vg);
    nvgMoveTo(g_vg, (float)x0, (float)y0);
    nvgLineTo(g_vg, (float)x1, (float)y1);
    nvgStrokeWidth(g_vg, thick);
    nvgStrokeColor(g_vg, rgui__color(rgba));
    nvgStroke(g_vg);
}
void rgui_draw_circle(int cx, int cy, int radius, uint32_t rgba) {
    if (!g_vg) return;
    nvgBeginPath(g_vg);
    nvgCircle(g_vg, (float)cx, (float)cy, (float)radius);
    nvgFillColor(g_vg, rgui__color(rgba));
    nvgFill(g_vg);
}
void rgui_draw_arc(int cx, int cy, int radius, float start_radians, float end_radians,
                   float thick, uint32_t rgba) {
    if (!g_vg) return;
    nvgBeginPath(g_vg);
    nvgArc(g_vg, (float)cx, (float)cy, (float)radius, start_radians, end_radians, NVG_CW);
    nvgStrokeWidth(g_vg, thick);
    nvgStrokeColor(g_vg, rgui__color(rgba));
    nvgStroke(g_vg);
}
void rgui_draw_text(const char *text, int x, int y, int size, uint32_t rgba) {
    if (!g_vg || !text) return;
    nvgFontFaceId(g_vg, g_font);
    nvgFontSize(g_vg, (float)size);
    nvgFillColor(g_vg, rgui__color(rgba));
    nvgText(g_vg, (float)x, (float)(y + size), text, NULL);
}
int rgui_measure_text(const char *text, int size) {
    if (!g_vg || !text) return 0;
    float bounds[4];
    nvgFontFaceId(g_vg, g_font);
    nvgFontSize(g_vg, (float)size);
    nvgTextBounds(g_vg, 0.0f, 0.0f, text, NULL, bounds);
    return (int)(bounds[2] - bounds[0]);
}

/* ---- window-shell surface ----
 * recoil-gui.h declares these for every backend, and gui/gui.rcl inlines the
 * whole module into any program that imports it, so an object built in --lib
 * mode references them even when nothing calls them. Pugl embedded in a
 * plug-in window has no window shell to answer with, so the honest answers are
 * the cached geometry and a refusal -- not an absent symbol that only fails at
 * link time, and only for the one caller that happens to link a library. */
int rgui_window_width(void) { return g_width; }
int rgui_window_height(void) { return g_height; }

const char *rgui_request_file(const char *title, const char *filters) {
    (void)title; (void)filters;
    return NULL;  /* no dialog here; NULL is the cancelled answer */
}

void rgui_request_file_async(const char *title, const char *filters,
                             void (*callback)(char *path)) {
    (void)title; (void)filters;
    /* Report the cancellation rather than never calling back, so a caller
     * waiting on the callback is not stranded. */
    if (callback) callback(NULL);
}

void rgui_set_window_icon(const char *path) {
    (void)path;  /* an embedded view owns no window to put an icon on */
}

void rgui_draw_text_mono(const char *text, int x, int y, int size, int cw,
                         uint32_t rgba) {
    /* One font is bundled and it is proportional, so the per-character advance
     * cannot be honoured; drawing the text proportionally is closer to the
     * request than drawing nothing. */
    (void)cw;
    rgui_draw_text(text, x, y, size, rgba);
}


/* ---- input ---- */
int rgui_mouse_x(void) { return (int)g_mouse_x; }
int rgui_mouse_y(void) { return (int)g_mouse_y; }
bool rgui_mouse_down(int button) { return button == 0 ? g_mouse_down : false; }
bool rgui_mouse_pressed(int button) { return button == 0 ? g_mouse_pressed_edge : false; }
bool rgui_key_pressed(int key) { (void)key; return false; }  /* keyboard unused this slice (VID frame calls this unconditionally for field faces -- must stay a safe inert stub, not a link error) */
int rgui_char_pressed(void) { return 0; }                    /* same */

/* Native widgets are a window-manager concept; pugl custom-draws controls. */
int rgui_native_supported(void) { return 0; }
void rgui_native_natural_size(int kind, const char *label, int req_w, int req_h,
                              int *out_w, int *out_h) {
    (void)kind; (void)label;
    if (out_w != NULL) *out_w = req_w;
    if (out_h != NULL) *out_h = req_h;
}
void rgui_native_begin(void) {}
void rgui_native_face(int idx, int kind, const char *label,
                      int x, int y, int w, int h, double value, const char *text) {
    (void)idx; (void)kind; (void)label; (void)x; (void)y; (void)w; (void)h;
    (void)value; (void)text;
}
void rgui_native_end(void) {}
int rgui_native_take_click(int *idx) { (void)idx; return 0; }
int rgui_native_take_change(int *idx, double *value) { (void)idx; (void)value; return 0; }
const char *rgui_native_take_text(int *idx) { if (idx != NULL) *idx = -1; return ""; }

/* ---- handle-based embedded contexts ------------------------------------ */

struct rgui_context {
    PuglWorld *world;
    PuglView *view;
    NVGcontext *vg;
    rgui_context_frame_fn frame;
    void *user;
#if defined(RGUI_NATIVE_TICK_TIMER)
    CFRunLoopTimerRef timer;
#endif
    int width;
    int height;
    int font;
    double mouse_x;
    double mouse_y;
    double last_click_time;
    double last_click_x;
    double last_click_y;
    PuglMods modifiers;
    bool mouse_down;
    bool mouse_pressed;
    bool mouse_released;
    bool mouse_double_clicked;
};

static inline void rgui__context_clear_edges(rgui_context *context) {
    context->mouse_pressed = false;
    context->mouse_released = false;
    context->mouse_double_clicked = false;
}

static PuglStatus rgui__context_event(PuglView *view, const PuglEvent *event) {
    rgui_context *context = (rgui_context *)puglGetHandle(view);
    if (!context) return PUGL_SUCCESS;

    switch (event->type) {
    case PUGL_CONFIGURE:
        context->width = (int)event->configure.width;
        context->height = (int)event->configure.height;
        break;
    case PUGL_EXPOSE:
        if (context->frame) context->frame(context->user);
        break;
    case PUGL_BUTTON_PRESS:
        context->mouse_x = event->button.x;
        context->mouse_y = event->button.y;
        context->modifiers = event->button.state;
        if (event->button.button == 0U) {
            const double dx = event->button.x - context->last_click_x;
            const double dy = event->button.y - context->last_click_y;
            context->mouse_down = true;
            context->mouse_pressed = true;
            context->mouse_double_clicked =
                event->button.time - context->last_click_time < 0.35 &&
                (dx * dx) + (dy * dy) < 36.0;
            context->last_click_time = event->button.time;
            context->last_click_x = event->button.x;
            context->last_click_y = event->button.y;
        }
        break;
    case PUGL_BUTTON_RELEASE:
        context->mouse_x = event->button.x;
        context->mouse_y = event->button.y;
        context->modifiers = event->button.state;
        if (event->button.button == 0U) {
            context->mouse_down = false;
            context->mouse_released = true;
        }
        break;
    case PUGL_MOTION:
        context->mouse_x = event->motion.x;
        context->mouse_y = event->motion.y;
        context->modifiers = event->motion.state;
        break;
    default:
        break;
    }
    return PUGL_SUCCESS;
}

static void rgui__context_destroy_native(rgui_context *context) {
    if (!context) return;
#if defined(RGUI_NATIVE_TICK_TIMER)
    rgui__timer_stop(&context->timer);
#endif
    if (context->vg && context->view) {
        if (puglEnterContext(context->view) == PUGL_SUCCESS) {
            nvgDeleteGL2(context->vg);
            puglLeaveContext(context->view);
        }
        context->vg = NULL;
    }
    if (context->view) puglFreeView(context->view);
    if (context->world) puglFreeWorld(context->world);
}

#if defined(RGUI_NATIVE_TICK_TIMER)
static void rgui__context_timer_cb(CFRunLoopTimerRef timer, void *info) {
    (void)timer;
    rgui_context_tick((rgui_context *)info);
}
#endif

rgui_context *rgui_context_attach(void *parent, int width, int height,
                                  rgui_context_frame_fn frame, void *user) {
    if (!parent || !frame || width <= 0 || height <= 0) return NULL;

    rgui_context *context = calloc(1, sizeof(*context));
    if (!context) return NULL;
    context->frame = frame;
    context->user = user;
    context->width = width;
    context->height = height;
    context->font = -1;

    context->world = puglNewWorld(PUGL_MODULE, 0);
    if (!context->world) goto fail;
    puglSetWorldString(context->world, PUGL_CLASS_NAME, "RecoilGuiPugl");
    context->view = puglNewView(context->world);
    if (!context->view) goto fail;

    puglSetHandle(context->view, context);
    puglSetBackend(context->view, puglGlBackend());
    puglSetEventFunc(context->view, rgui__context_event);
    puglSetViewHint(context->view, PUGL_CONTEXT_API, PUGL_OPENGL_API);
    puglSetViewHint(context->view, PUGL_CONTEXT_VERSION_MAJOR, 2);
    puglSetViewHint(context->view, PUGL_DOUBLE_BUFFER, PUGL_TRUE);
    puglSetViewHint(context->view, PUGL_RESIZABLE, PUGL_FALSE);
    puglSetSizeHint(context->view, PUGL_DEFAULT_SIZE, (unsigned)width, (unsigned)height);
    if (puglSetParent(context->view, (PuglNativeView)(uintptr_t)parent) != PUGL_SUCCESS) goto fail;
    if (puglRealize(context->view) != PUGL_SUCCESS) goto fail;
    if (puglShow(context->view, PUGL_SHOW_PASSIVE) != PUGL_SUCCESS) goto fail;
    if (puglEnterContext(context->view) != PUGL_SUCCESS) goto fail;

    context->vg = nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!context->vg) {
        puglLeaveContext(context->view);
        goto fail;
    }
    context->font = nvgCreateFontMem(context->vg, "sans", (unsigned char *)rgui_pugl_font_data,
                                     (int)rgui_pugl_font_data_len, 0);
    if (context->font == -1) {
        puglLeaveContext(context->view);
        goto fail;
    }
    if (puglLeaveContext(context->view) != PUGL_SUCCESS) goto fail;

#if defined(RGUI_NATIVE_TICK_TIMER)
    context->timer = rgui__timer_start(rgui__context_timer_cb, context);
#endif
    return context;

fail:
    rgui__context_destroy_native(context);
    free(context);
    return NULL;
}

void rgui_context_detach(rgui_context *context) {
    rgui__context_destroy_native(context);
    free(context);
}

void rgui_context_tick(rgui_context *context) {
    if (!context || !context->view || !context->frame) return;
    puglObscureView(context->view);
    puglUpdate(context->world, 0.0);
    rgui__context_clear_edges(context);
}

void rgui_context_begin_frame(rgui_context *context) {
    if (context && context->vg) nvgBeginFrame(context->vg, (float)context->width, (float)context->height, 1.0f);
}
void rgui_context_end_frame(rgui_context *context) {
    if (context && context->vg) nvgEndFrame(context->vg);
}
void rgui_context_clear(rgui_context *context, uint32_t rgba) {
    if (!context || !context->vg) return;
    NVGcolor c = rgui__color(rgba);
    glViewport(0, 0, context->width, context->height);
    glClearColor(c.r, c.g, c.b, c.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}
void rgui_context_draw_rect(rgui_context *context, int x, int y, int w, int h, uint32_t rgba) {
    if (!context || !context->vg) return;
    nvgBeginPath(context->vg); nvgRect(context->vg, (float)x, (float)y, (float)w, (float)h);
    nvgFillColor(context->vg, rgui__color(rgba)); nvgFill(context->vg);
}
void rgui_context_draw_rect_gradient_v(rgui_context *context, int x, int y, int w, int h,
                                       uint32_t top_rgba, uint32_t bottom_rgba) {
    if (!context || !context->vg) return;
    NVGpaint paint = nvgLinearGradient(context->vg, (float)x, (float)y, (float)x, (float)(y + h),
                                        rgui__color(top_rgba), rgui__color(bottom_rgba));
    nvgBeginPath(context->vg); nvgRect(context->vg, (float)x, (float)y, (float)w, (float)h);
    nvgFillPaint(context->vg, paint); nvgFill(context->vg);
}
void rgui_context_draw_line(rgui_context *context, int x0, int y0, int x1, int y1,
                            float thick, uint32_t rgba) {
    if (!context || !context->vg) return;
    nvgBeginPath(context->vg); nvgMoveTo(context->vg, (float)x0, (float)y0);
    nvgLineTo(context->vg, (float)x1, (float)y1); nvgStrokeWidth(context->vg, thick);
    nvgStrokeColor(context->vg, rgui__color(rgba)); nvgStroke(context->vg);
}
void rgui_context_draw_circle(rgui_context *context, int cx, int cy, int radius, uint32_t rgba) {
    if (!context || !context->vg) return;
    nvgBeginPath(context->vg); nvgCircle(context->vg, (float)cx, (float)cy, (float)radius);
    nvgFillColor(context->vg, rgui__color(rgba)); nvgFill(context->vg);
}
void rgui_context_draw_arc(rgui_context *context, int cx, int cy, int radius,
                           float start_radians, float end_radians, float thick, uint32_t rgba) {
    if (!context || !context->vg) return;
    nvgBeginPath(context->vg); nvgArc(context->vg, (float)cx, (float)cy, (float)radius,
                                      start_radians, end_radians, NVG_CW);
    nvgStrokeWidth(context->vg, thick); nvgStrokeColor(context->vg, rgui__color(rgba)); nvgStroke(context->vg);
}
void rgui_context_draw_text(rgui_context *context, const char *text, int x, int y, int size,
                            uint32_t rgba) {
    if (!context || !context->vg || !text) return;
    nvgFontFaceId(context->vg, context->font); nvgFontSize(context->vg, (float)size);
    nvgFillColor(context->vg, rgui__color(rgba)); nvgText(context->vg, (float)x, (float)(y + size), text, NULL);
}
int rgui_context_measure_text(rgui_context *context, const char *text, int size) {
    if (!context || !context->vg || !text) return 0;
    float bounds[4]; nvgFontFaceId(context->vg, context->font); nvgFontSize(context->vg, (float)size);
    nvgTextBounds(context->vg, 0.0f, 0.0f, text, NULL, bounds); return (int)(bounds[2] - bounds[0]);
}
int rgui_context_mouse_x(const rgui_context *context) { return context ? (int)context->mouse_x : 0; }
int rgui_context_mouse_y(const rgui_context *context) { return context ? (int)context->mouse_y : 0; }
bool rgui_context_mouse_down(const rgui_context *context, int button) { return context && button == 0 && context->mouse_down; }
bool rgui_context_mouse_pressed(const rgui_context *context, int button) { return context && button == 0 && context->mouse_pressed; }
bool rgui_context_mouse_released(const rgui_context *context, int button) { return context && button == 0 && context->mouse_released; }
bool rgui_context_mouse_double_clicked(const rgui_context *context, int button) { return context && button == 0 && context->mouse_double_clicked; }
bool rgui_context_shift_down(const rgui_context *context) { return context && (context->modifiers & PUGL_MOD_SHIFT) != 0; }

/* ---------------------------------------------------------------------------
 * Path contract
 *
 * The shape-per-call API above bakes a colour into every call, so each new
 * shape costs one implementation per backend. These primitives are the other
 * shape: build a path, then fill or stroke it with the current paint. Every
 * Draw shape -- box, circle, triangle, polygon, the arc track of a knob -- is
 * then sugar written once in Recoil rather than five times in C.
 *
 * They map essentially one-to-one onto NanoVG, which is why this backend gets
 * them nearly free; Cairo, CoreGraphics and Direct2D are path engines too, so
 * the same contract is implementable there when a consumer needs it.
 * Coordinates are floats throughout: transforms and curves make integer
 * pixel coordinates the wrong unit as soon as you rotate anything.
 * -------------------------------------------------------------------------*/

int rgui_path_supported(const rgui_context *context) { return context && context->vg ? 1 : 0; }

void rgui_path_begin(rgui_context *context) {
    if (!context || !context->vg) return;
    nvgBeginPath(context->vg);
}
void rgui_path_move_to(rgui_context *context, float x, float y) {
    if (!context || !context->vg) return;
    nvgMoveTo(context->vg, x, y);
}
void rgui_path_line_to(rgui_context *context, float x, float y) {
    if (!context || !context->vg) return;
    nvgLineTo(context->vg, x, y);
}
void rgui_path_bezier_to(rgui_context *context, float c1x, float c1y, float c2x, float c2y,
                         float x, float y) {
    if (!context || !context->vg) return;
    nvgBezierTo(context->vg, c1x, c1y, c2x, c2y, x, y);
}
void rgui_path_quad_to(rgui_context *context, float cx, float cy, float x, float y) {
    if (!context || !context->vg) return;
    nvgQuadTo(context->vg, cx, cy, x, y);
}
/* winding: 0 clockwise, anything else counter-clockwise. An integer rather
 * than the NVG enum so the contract does not leak NanoVG into every backend. */
void rgui_path_arc(rgui_context *context, float cx, float cy, float radius,
                   float start_radians, float end_radians, int winding) {
    if (!context || !context->vg) return;
    nvgArc(context->vg, cx, cy, radius, start_radians, end_radians,
           winding == 0 ? NVG_CW : NVG_CCW);
}
void rgui_path_rect(rgui_context *context, float x, float y, float w, float h) {
    if (!context || !context->vg) return;
    nvgRect(context->vg, x, y, w, h);
}
void rgui_path_rounded_rect(rgui_context *context, float x, float y, float w, float h, float r) {
    if (!context || !context->vg) return;
    nvgRoundedRect(context->vg, x, y, w, h, r);
}
void rgui_path_ellipse(rgui_context *context, float cx, float cy, float rx, float ry) {
    if (!context || !context->vg) return;
    nvgEllipse(context->vg, cx, cy, rx, ry);
}
void rgui_path_close(rgui_context *context) {
    if (!context || !context->vg) return;
    nvgClosePath(context->vg);
}

void rgui_path_fill(rgui_context *context) {
    if (!context || !context->vg) return;
    nvgFill(context->vg);
}
void rgui_path_stroke(rgui_context *context) {
    if (!context || !context->vg) return;
    nvgStroke(context->vg);
}
void rgui_path_fill_color(rgui_context *context, uint32_t rgba) {
    if (!context || !context->vg) return;
    nvgFillColor(context->vg, rgui__color(rgba));
}
void rgui_path_stroke_color(rgui_context *context, uint32_t rgba) {
    if (!context || !context->vg) return;
    nvgStrokeColor(context->vg, rgui__color(rgba));
}
void rgui_path_stroke_width(rgui_context *context, float width) {
    if (!context || !context->vg) return;
    nvgStrokeWidth(context->vg, width);
}
/* cap: 0 butt, 1 round, 2 square. join: 0 miter, 1 round, 2 bevel. */
void rgui_path_line_cap(rgui_context *context, int cap) {
    if (!context || !context->vg) return;
    nvgLineCap(context->vg, cap == 1 ? NVG_ROUND : (cap == 2 ? NVG_SQUARE : NVG_BUTT));
}
void rgui_path_line_join(rgui_context *context, int join) {
    if (!context || !context->vg) return;
    nvgLineJoin(context->vg, join == 1 ? NVG_ROUND : (join == 2 ? NVG_BEVEL : NVG_MITER));
}

void rgui_path_save(rgui_context *context) {
    if (!context || !context->vg) return;
    nvgSave(context->vg);
}
void rgui_path_restore(rgui_context *context) {
    if (!context || !context->vg) return;
    nvgRestore(context->vg);
}
void rgui_path_translate(rgui_context *context, float x, float y) {
    if (!context || !context->vg) return;
    nvgTranslate(context->vg, x, y);
}
void rgui_path_rotate(rgui_context *context, float radians) {
    if (!context || !context->vg) return;
    nvgRotate(context->vg, radians);
}
void rgui_path_scale(rgui_context *context, float x, float y) {
    if (!context || !context->vg) return;
    nvgScale(context->vg, x, y);
}
/* Intersects rather than replaces, so a clip inside a clip cannot widen the
 * region a caller already narrowed -- which is what nesting has to mean for a
 * widget drawn inside another widget's aperture. */
void rgui_path_scissor(rgui_context *context, float x, float y, float w, float h) {
    if (!context || !context->vg) return;
    nvgIntersectScissor(context->vg, x, y, w, h);
}
void rgui_path_reset_scissor(rgui_context *context) {
    if (!context || !context->vg) return;
    nvgResetScissor(context->vg);
}

/* Text as a path-contract citizen: it paints with the CURRENT fill colour
 * rather than one passed per call, so `fill-pen` governs text exactly as it
 * governs a filled shape and Draw needs no special case for it. */
void rgui_path_text(rgui_context *context, float x, float y, float size, const char *text) {
    if (!context || !context->vg || !text) return;
    nvgFontFaceId(context->vg, context->font);
    nvgFontSize(context->vg, size);
    nvgText(context->vg, x, y + size, text, NULL);
}
