#include "vx-scheme.h"
#include <emscripten/emscripten.h>
#include <string>
#include <memory>
#include <vector>
#include <cstdlib>

static std::unique_ptr<Context> g_ctx;
static std::string g_last_output;

// External JS canvas hooks
extern "C" {

EM_JS(void, js_canvas_clear, (double r, double g, double b, double a), {
  if (typeof globalThis !== 'undefined' && globalThis.vxsCanvasClear) globalThis.vxsCanvasClear(r, g, b, a);
});

EM_JS(void, js_canvas_fill_rect, (double x, double y, double w, double h, double r, double g, double b, double a), {
  if (typeof globalThis !== 'undefined' && globalThis.vxsCanvasFillRect) globalThis.vxsCanvasFillRect(x, y, w, h, r, g, b, a);
});

EM_JS(void, js_canvas_draw_circle, (double x, double y, double radius, double r, double g, double b, double a), {
  if (typeof globalThis !== 'undefined' && globalThis.vxsCanvasDrawCircle) globalThis.vxsCanvasDrawCircle(x, y, radius, r, g, b, a);
});

EM_JS(void, js_canvas_draw_line, (double x1, double y1, double x2, double y2, double r, double g, double b, double a), {
  if (typeof globalThis !== 'undefined' && globalThis.vxsCanvasDrawLine) globalThis.vxsCanvasDrawLine(x1, y1, x2, y2, r, g, b, a);
});

EM_JS(void, js_canvas_draw_text, (const char *text, double x, double y, double r, double g, double b, double a), {
  if (typeof globalThis !== 'undefined' && globalThis.vxsCanvasDrawText) globalThis.vxsCanvasDrawText(UTF8ToString(text), x, y, r, g, b, a);
});

EM_JS(double, js_canvas_get_width, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsCanvasWidth) ? globalThis.vxsCanvasWidth() : 800.0;
});

EM_JS(double, js_canvas_get_height, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsCanvasHeight) ? globalThis.vxsCanvasHeight() : 600.0;
});

EM_JS(double, js_now, (), {
  return (typeof performance !== 'undefined' && performance.now) ? performance.now() : 0.0;
});

EM_JS(void, js_log, (const char* str), {
  console.log(UTF8ToString(str));
});

EM_JS(double, js_canvas_mouse_x, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsMouseX) ? globalThis.vxsMouseX() : 0.0;
});

EM_JS(double, js_canvas_mouse_y, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsMouseY) ? globalThis.vxsMouseY() : 0.0;
});

EM_JS(int, js_canvas_mouse_down, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsMouseDown) ? globalThis.vxsMouseDown() : 0;
});

} // extern "C"

double vx_get_time() {
  return js_now() / 1000.0;
}

uint32_t debug_flags() {
  return 0;
}

static Cell *nth(Cell *list, int n) {
  Cell *cur = list;
  for (int i = 0; i < n && cur != nil && cur != nullptr; ++i) {
    cur = cdr(cur);
  }
  return (cur != nil && cur != nullptr) ? car(cur) : nil;
}

static inline double asReal(Cell *c, double def_val = 0.0) {
  if (!c || c == nil) return def_val;
  if (c->is<intptr_t>()) return (double)get_int(c);
  if (c->is<double>()) return c->as<double>();
  return def_val;
}

// Scheme Subrs
static Cell *sk_canvas_clear(Context *ctx, Cell *args) {
  double r = asReal(nth(args, 0), 0.0);
  double g = asReal(nth(args, 1), 0.0);
  double b = asReal(nth(args, 2), 0.0);
  double a = asReal(nth(args, 3), 1.0);
  js_canvas_clear(r, g, b, a);
  return &Cell::Unspecified;
}

static Cell *sk_canvas_fill_rect(Context *ctx, Cell *args) {
  double x = asReal(nth(args, 0), 0.0);
  double y = asReal(nth(args, 1), 0.0);
  double w = asReal(nth(args, 2), 10.0);
  double h = asReal(nth(args, 3), 10.0);
  double r = asReal(nth(args, 4), 1.0);
  double g = asReal(nth(args, 5), 1.0);
  double b = asReal(nth(args, 6), 1.0);
  double a = asReal(nth(args, 7), 1.0);
  js_canvas_fill_rect(x, y, w, h, r, g, b, a);
  return &Cell::Unspecified;
}

static Cell *sk_canvas_draw_circle(Context *ctx, Cell *args) {
  double x = asReal(nth(args, 0), 0.0);
  double y = asReal(nth(args, 1), 0.0);
  double rad = asReal(nth(args, 2), 5.0);
  double r = asReal(nth(args, 3), 1.0);
  double g = asReal(nth(args, 4), 1.0);
  double b = asReal(nth(args, 5), 1.0);
  double a = asReal(nth(args, 6), 1.0);
  js_canvas_draw_circle(x, y, rad, r, g, b, a);
  return &Cell::Unspecified;
}

static Cell *sk_canvas_draw_line(Context *ctx, Cell *args) {
  double x1 = asReal(nth(args, 0), 0.0);
  double y1 = asReal(nth(args, 1), 0.0);
  double x2 = asReal(nth(args, 2), 0.0);
  double y2 = asReal(nth(args, 3), 0.0);
  double r = asReal(nth(args, 4), 1.0);
  double g = asReal(nth(args, 5), 1.0);
  double b = asReal(nth(args, 6), 1.0);
  double a = asReal(nth(args, 7), 1.0);
  js_canvas_draw_line(x1, y1, x2, y2, r, g, b, a);
  return &Cell::Unspecified;
}

static Cell *sk_canvas_draw_text(Context *ctx, Cell *args) {
  Cell *c0 = nth(args, 0);
  std::string str = (c0 && c0->is<std::string>()) ? c0->StringValue() : "";
  double x = asReal(nth(args, 1), 0.0);
  double y = asReal(nth(args, 2), 0.0);
  double r = asReal(nth(args, 3), 1.0);
  double g = asReal(nth(args, 4), 1.0);
  double b = asReal(nth(args, 5), 1.0);
  double a = asReal(nth(args, 6), 1.0);
  js_canvas_draw_text(str.c_str(), x, y, r, g, b, a);
  return &Cell::Unspecified;
}

static Cell *sk_canvas_width(Context *ctx, Cell *args) {
  return ctx->make_real(js_canvas_get_width());
}

static Cell *sk_canvas_height(Context *ctx, Cell *args) {
  return ctx->make_real(js_canvas_get_height());
}

static Cell *sk_mouse_x(Context *ctx, Cell *args) {
  return ctx->make_real(js_canvas_mouse_x());
}

static Cell *sk_mouse_y(Context *ctx, Cell *args) {
  return ctx->make_real(js_canvas_mouse_y());
}

static Cell *sk_mouse_down_p(Context *ctx, Cell *args) {
  return ctx->make_boolean(js_canvas_mouse_down() != 0);
}

static Cell *sk_now(Context *ctx, Cell *args) {
  return ctx->make_real(js_now());
}

static Cell *sk_random_real(Context *ctx, Cell *args) {
  double max_val = asReal(nth(args, 0), 1.0);
  double r = ((double)rand() / (double)RAND_MAX) * max_val;
  return ctx->make_real(r);
}

static Cell *sk_js_log(Context *ctx, Cell *args) {
  sstring ss;
  if (args != nil && car(args) != nil) {
    car(args)->write(ss);
    js_log(ss.str());
  }
  return &Cell::Unspecified;
}

// Exported Wasm C API
extern "C" {

EMSCRIPTEN_KEEPALIVE
int vxs_init() {
  if (g_ctx) return 1;
  g_ctx = std::make_unique<Context>();

  // Bind Web Canvas & System Subrs
  g_ctx->bind_subr("canvas-clear", sk_canvas_clear);
  g_ctx->bind_subr("canvas-fill-rect", sk_canvas_fill_rect);
  g_ctx->bind_subr("canvas-draw-circle", sk_canvas_draw_circle);
  g_ctx->bind_subr("canvas-draw-line", sk_canvas_draw_line);
  g_ctx->bind_subr("canvas-draw-text", sk_canvas_draw_text);
  g_ctx->bind_subr("canvas-width", sk_canvas_width);
  g_ctx->bind_subr("canvas-height", sk_canvas_height);
  g_ctx->bind_subr("mouse-x", sk_mouse_x);
  g_ctx->bind_subr("mouse-y", sk_mouse_y);
  g_ctx->bind_subr("mouse-down?", sk_mouse_down_p);
  g_ctx->bind_subr("now", sk_now);
  g_ctx->bind_subr("random", sk_random_real);
  g_ctx->bind_subr("js-log", sk_js_log);

  return 1;
}

EMSCRIPTEN_KEEPALIVE
const char *vxs_eval(const char *code) {
  if (!g_ctx) vxs_init();
  g_last_output.clear();
  try {
    sstring text(code);
    Cell *expr;
    Cell *result = &Cell::Unspecified;
    while ((expr = g_ctx->read(text))) {
      result = g_ctx->eval(expr);
    }
    sstring ss;
    if (result != &Cell::Unspecified && result != nullptr) {
      Cell::write(result, ss);
      g_last_output = ss.string();
    } else {
      g_last_output = "#<unspecified>";
    }
  } catch (const std::exception &e) {
    g_last_output = std::string("Error: ") + e.what();
  } catch (...) {
    g_last_output = "Unknown error occurred";
  }
  return g_last_output.c_str();
}

EMSCRIPTEN_KEEPALIVE
int vxs_step_fibers() {
  if (!g_ctx) return 0;
  try {
    return g_ctx->step_fibers() ? 1 : 0;
  } catch (...) {
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE
int vxs_active_fibers_count() {
  if (!g_ctx) return 0;
  return static_cast<int>(g_ctx->active_fibers_count());
}

} // extern "C"
