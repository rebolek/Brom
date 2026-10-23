#ifndef RECOIL_GUI_H
#define RECOIL_GUI_H

/*
 * recoil-gui — unified GUI backend contract.
 *
 * One small C ABI implemented by each backend (raylib, pugl, ...). Colors are
 * packed 0xRRGGBBAA u32 and every entry point is scalar-only, so the Recoil
 * binding needs no struct-by-value or out-parameters. The app depends solely on
 * the `audio/gui` Recoil module; the backend is selected at install time.
 */

#include <stdbool.h>
#include <stdint.h>

/* Backend-neutral key values. They intentionally match raylib's established
 * values so the raylib backend remains a direct pass-through. */
#define RGUI_KEY_ESCAPE    256
#define RGUI_KEY_ENTER     257
#define RGUI_KEY_TAB       258
#define RGUI_KEY_BACKSPACE 259

/* Context API ------------------------------------------------------------
 *
 * The original rgui_* API below is intentionally process-global for small
 * standalone examples. Embedded plug-ins need more than one editor at once,
 * so new code should use this handle-based surface instead. Context callbacks
 * always receive the user pointer supplied at creation time. Attach, tick, and
 * detach must run on the native parent's UI thread. The host owns that thread,
 * the parent, callback, and user pointer until detach. Detaching from inside a
 * frame callback is unsupported.
 */
typedef struct rgui_context rgui_context;
typedef void (*rgui_context_frame_fn)(void *user);

rgui_context *rgui_context_attach(void *parent, int width, int height,
                                  rgui_context_frame_fn frame, void *user);
void rgui_context_detach(rgui_context *context);
void rgui_context_tick(rgui_context *context);

void rgui_context_begin_frame(rgui_context *context);
void rgui_context_end_frame(rgui_context *context);
void rgui_context_clear(rgui_context *context, uint32_t rgba);
void rgui_context_draw_rect(rgui_context *context, int x, int y, int w, int h, uint32_t rgba);
void rgui_context_draw_rect_gradient_v(rgui_context *context, int x, int y, int w, int h,
                                       uint32_t top_rgba, uint32_t bottom_rgba);
void rgui_context_draw_line(rgui_context *context, int x0, int y0, int x1, int y1,
                            float thick, uint32_t rgba);
void rgui_context_draw_circle(rgui_context *context, int cx, int cy, int radius, uint32_t rgba);
void rgui_context_draw_arc(rgui_context *context, int cx, int cy, int radius,
                           float start_radians, float end_radians, float thick, uint32_t rgba);
void rgui_context_draw_text(rgui_context *context, const char *text, int x, int y, int size,
                            uint32_t rgba);
int rgui_context_measure_text(rgui_context *context, const char *text, int size);

int rgui_context_mouse_x(const rgui_context *context);
int rgui_context_mouse_y(const rgui_context *context);
bool rgui_context_mouse_down(const rgui_context *context, int button);
bool rgui_context_mouse_pressed(const rgui_context *context, int button);
bool rgui_context_mouse_released(const rgui_context *context, int button);
bool rgui_context_mouse_double_clicked(const rgui_context *context, int button);
bool rgui_context_shift_down(const rgui_context *context);

/* Path contract
 *
 * The calls above bake a colour into every shape, so each new shape costs one
 * implementation per backend. These are the other shape: build a path, then
 * fill or stroke it with the current paint, so a Draw shape -- box, circle,
 * triangle, polygon, a knob's arc track -- is sugar written once above this
 * layer instead of five times below it.
 *
 * Coordinates are floats: once anything rotates or scales, integer pixels are
 * the wrong unit. Enum-ish arguments are plain ints so the contract does not
 * leak one backend's vocabulary into the others.
 *
 * rgui_path_supported reports whether a backend implements this at all. A
 * backend that does not still links -- it defines these as no-ops and answers
 * 0 -- so a Draw consumer gets a blank region and a clear answer rather than
 * an undefined symbol at link time.
 */
int  rgui_path_supported(const rgui_context *context);

void rgui_path_begin(rgui_context *context);
void rgui_path_move_to(rgui_context *context, float x, float y);
void rgui_path_line_to(rgui_context *context, float x, float y);
void rgui_path_bezier_to(rgui_context *context, float c1x, float c1y, float c2x, float c2y,
                         float x, float y);
void rgui_path_quad_to(rgui_context *context, float cx, float cy, float x, float y);
void rgui_path_arc(rgui_context *context, float cx, float cy, float radius,
                   float start_radians, float end_radians, int winding); /* 0 = clockwise */
void rgui_path_rect(rgui_context *context, float x, float y, float w, float h);
void rgui_path_rounded_rect(rgui_context *context, float x, float y, float w, float h, float r);
void rgui_path_ellipse(rgui_context *context, float cx, float cy, float rx, float ry);
void rgui_path_close(rgui_context *context);

void rgui_path_fill(rgui_context *context);
void rgui_path_stroke(rgui_context *context);
void rgui_path_fill_color(rgui_context *context, uint32_t rgba);
void rgui_path_stroke_color(rgui_context *context, uint32_t rgba);
void rgui_path_stroke_width(rgui_context *context, float width);
void rgui_path_line_cap(rgui_context *context, int cap);   /* 0 butt, 1 round, 2 square */
void rgui_path_line_join(rgui_context *context, int join);  /* 0 miter, 1 round, 2 bevel */

void rgui_path_save(rgui_context *context);
void rgui_path_restore(rgui_context *context);
void rgui_path_translate(rgui_context *context, float x, float y);
void rgui_path_rotate(rgui_context *context, float radians);
void rgui_path_scale(rgui_context *context, float x, float y);
void rgui_path_scissor(rgui_context *context, float x, float y, float w, float h); /* intersects */
void rgui_path_reset_scissor(rgui_context *context);
/* Paints with the current fill colour, so fill-pen governs text as it governs
 * a filled shape. y is the top of the line, matching rgui_context_draw_text. */
void rgui_path_text(rgui_context *context, float x, float y, float size, const char *text);

/* Window lifecycle + frame loop */
bool rgui_open_window(int width, int height, const char *title);
bool rgui_window_should_close(void);
void rgui_close_window(void);
void rgui_set_target_fps(int fps);
void rgui_run_loop(void (*frame)(void));   /* drives `frame` until the window closes */
int  rgui_window_width(void);
int  rgui_window_height(void);
const char *rgui_request_file(const char *title, const char *filters);
void rgui_request_file_async(const char *title, const char *filters, void (*callback)(char *path));
void rgui_set_window_icon(const char *path);

/* Embedded (host-parented) operation. A backend that cannot embed returns
 * false from rgui_attach; standalone-only backends stub all four.
 * Ordering: rgui_set_frame must be called before the first rgui_tick;
 * rgui_tick with no frame set (or no attached view) is a no-op, never a
 * crash. Standalone mode (rgui_open_window + rgui_run_loop) and embedded
 * mode (rgui_attach + host-timer-driven rgui_tick) are mutually exclusive
 * per process. */
bool rgui_attach(void *parent, int width, int height); /* child view in host parent */
void rgui_detach(void);
void rgui_set_frame(void (*frame)(void));              /* frame cb for rgui_tick */
void rgui_tick(void);                                  /* pump events + render one frame */

/* Per-frame drawing (call between begin/end) */
void rgui_begin_frame(void);
void rgui_end_frame(void);
void rgui_clear(uint32_t rgba);
void rgui_draw_rect(int x, int y, int w, int h, uint32_t rgba);
void rgui_draw_rect_gradient_v(int x, int y, int w, int h, uint32_t top_rgba, uint32_t bottom_rgba);
void rgui_draw_line(int x0, int y0, int x1, int y1, float thick, uint32_t rgba);
void rgui_draw_circle(int cx, int cy, int radius, uint32_t rgba);
void rgui_draw_arc(int cx, int cy, int radius, float start_radians, float end_radians,
                   float thick, uint32_t rgba);
void rgui_draw_text(const char *text, int x, int y, int size, uint32_t rgba);
void rgui_draw_text_mono(const char *text, int x, int y, int size, int cw, uint32_t rgba);
int  rgui_measure_text(const char *text, int size);

/* Blit a straight-alpha RGBA8 pixel buffer into a rectangle at (x,y). Row r
 * starts at pixels + r*stride; the buffer must hold at least h rows of w*4
 * bytes. Backends copy the pixels and may return before the buffer is freed. */
void rgui_draw_image(const unsigned char *pixels, int x, int y, int w, int h, int stride);

/* Input */
int  rgui_mouse_x(void);
int  rgui_mouse_y(void);
bool rgui_mouse_down(int button);     /* 0 = left */
bool rgui_mouse_pressed(int button);
bool rgui_key_pressed(int key);
int  rgui_char_pressed(void);         /* next queued Unicode codepoint, 0 = none */

/* Native widgets (optional; portable backends advertise no support) ----------
 *
 * A backend may render the "control" VID faces (button/slider/field) as real
 * OS widgets instead of custom-drawn chrome. The frontend drives a per-frame
 * reconciliation pass and polls a pending-event queue from inside its frame
 * callback -- native signal/target handlers never dispatch directly, they only
 * enqueue. This keeps every Recoil callback on the frame thread so a handler
 * can never rebuild/close the view mid-callback (which would free the widget
 * whose signal is still executing).
 *
 * Kind constants are a backend-neutral namespace (maps from the fk-* face kind
 * in the Recoil binding). `rgui_native_supported` returns a bitmask over them;
 * 0 means native widgets are unavailable and the caller uses custom drawing.
 */
#define RGUI_NATIVE_BUTTON 0
#define RGUI_NATIVE_SLIDER 1
#define RGUI_NATIVE_FIELD  2
#define RGUI_NATIVE_TEXT   3

/* Echo-suppression tolerance for normalized values. A backend compares a value
 * against the last one it saw with this epsilon rather than for equality: the
 * native control quantises whatever gets pushed into it (Win32 trackbars to
 * 1/1000, GTK scales to their step), so an exact test would let a pushed value
 * come straight back as a user change and feed the next push -- an endless
 * round trip. This is an order of magnitude finer than the coarsest backend
 * step, so a genuine movement is never swallowed. */
#define RGUI_NATIVE_EPS 1e-4

int  rgui_native_supported(void);   /* bitmask of RGUI_NATIVE_* kinds; 0 = none */
/* Natural size for a control of `kind` with `label`. `req_w`/`req_h` are the
 * face's requested size (0 = unspecified); `out_w`/`out_h` receive the size the
 * widget will actually use (never smaller than the OS minimum; larger requests
 * honored). Must be callable before any window exists. */
void rgui_native_natural_size(int kind, const char *label,
                              int req_w, int req_h, int *out_w, int *out_h);

/* Reconciliation pass, driven from the frontend frame callback:
 *   rgui_native_begin();
 *   rgui_native_face(idx, kind, label, x, y, w, h, value, text); 0..n
 *   rgui_native_end();
 * `rgui_native_face` creates or updates the widget for face index `idx`;
 * `rgui_native_end` destroys any control whose index was not synced this pass
 * (handles rebuilds and teardown). Symbols for labels are strdup'ed by the
 * backend.
 *
 * `value` is the face's normalized position in 0.0..1.0, matching Rebol/Red's
 * `data` facet and the CLAP/VST3 host parameter range. It is deliberately
 * linear: any perceptual curve (a logarithmic filter cutoff, say) belongs in
 * the application's parameter layer, not here, because host automation writes
 * parameters without this path being involved at all -- a taper applied in the
 * widget would make an automated value and a dragged one disagree. Backends
 * whose native control is integer-only (Win32 trackbars) keep an internal
 * integer range and convert at this boundary. */
void rgui_native_begin(void);
void rgui_native_face(int idx, int kind, const char *label,
                      int x, int y, int w, int h, double value, const char *text);
void rgui_native_end(void);

/* Pending-event drain, polled from inside the frontend frame callback. Each
 * `take` returns 1 and fills `*idx` (and `*value`) for the next queued event, or
 * 0 when the queue is drained. Value changes carry absolute normalized 0.0..1.0
 * positions.
 * `rgui_native_take_text` returns a backend-owned NUL-terminated text buffer
 * (valid until the next `take_text`/`end`/`clear`; the caller copies it) and
 * sets `*idx`; when drained it sets `*idx` to -1 and returns "". */
int rgui_native_take_click(int *idx);
int rgui_native_take_change(int *idx, double *value);
const char *rgui_native_take_text(int *idx);

#endif /* RECOIL_GUI_H */
