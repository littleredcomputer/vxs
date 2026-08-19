#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include "vx_reader.h"
#include "vx_compiler.h"
#include <string>
#include <memory>
#include <iostream>
#include <chrono>
#include <cstdio>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace vxs {

// Global VM instance for WebAssembly
static std::unique_ptr<VM> g_vm;

// EM_JS Canvas & Browser Interface
extern "C" {

#ifdef __EMSCRIPTEN__
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

EM_JS(double, js_canvas_mouse_x, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsMouseX) ? globalThis.vxsMouseX() : 0.0;
});

EM_JS(double, js_canvas_mouse_y, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsMouseY) ? globalThis.vxsMouseY() : 0.0;
});

EM_JS(int, js_canvas_mouse_down, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsMouseDown) ? globalThis.vxsMouseDown() : 0;
});

EM_JS(double, js_now, (), {
  return (typeof performance !== 'undefined' && performance.now) ? performance.now() : 0.0;
});

EM_JS(void, js_print_output, (const char *text), {
  if (typeof globalThis !== 'undefined' && globalThis.vxsPrint) {
    globalThis.vxsPrint(UTF8ToString(text));
  } else if (typeof console !== 'undefined' && console.log) {
    console.log(UTF8ToString(text));
  }
});

EM_JS(void, js_console_log, (const char *text), {
  if (typeof console !== 'undefined' && console.log) {
    console.log(UTF8ToString(text));
  }
});
#else
static void js_canvas_clear(double, double, double, double) {}
static void js_canvas_fill_rect(double, double, double, double, double, double, double, double) {}
static void js_canvas_draw_circle(double, double, double, double, double, double, double) {}
static void js_canvas_draw_line(double, double, double, double, double, double, double, double) {}
static void js_canvas_draw_text(const char *, double, double, double, double, double, double) {}
static double js_canvas_get_width() { return 800.0; }
static double js_canvas_get_height() { return 600.0; }
static double js_canvas_mouse_x() { return 0.0; }
static double js_canvas_mouse_y() { return 0.0; }
static int js_canvas_mouse_down() { return 0; }
static double js_now() { return 0.0; }
static void js_print_output(const char *text) { std::cout << text << std::flush; }
static void js_console_log(const char *text) { std::cout << "[CONSOLE.LOG] " << text << std::endl; }
#endif

// Register Canvas & Web primitives
#ifdef __EMSCRIPTEN__
// The host-side object table. Lives on globalThis so the embedder (app.js,
// a test harness, eventually the WebGPU bindings) can put objects in and
// read them back out; the VM only ever sees the integer.
EM_JS(void, js_ensure_handle_table, (), {
  if (!globalThis.vxsHandles) {
    globalThis.vxsHandles = {
      next: 1,
      map: new Map(),
      put: function(obj) { var id = this.next++; this.map.set(id, obj); return id; },
      get: function(id) { return this.map.get(id); },
      release: function(id) { return this.map.delete(id); },
      size: function() { return this.map.size; }
    };
  }
});

// Drop the host's reference so the browser can collect the underlying
// object. For a GPU resource the caller should also have called its own
// destroy() — releasing the table entry only removes OUR hold on it.
EM_JS(void, js_release_handle, (int id), {
  if (globalThis.vxsHandles) globalThis.vxsHandles.release(id);
});

EM_JS(int, js_handle_count, (), {
  return globalThis.vxsHandles ? globalThis.vxsHandles.size() : 0;
});
#else
static void js_ensure_handle_table() {}
static void js_release_handle(int) {}
static int js_handle_count() { return 0; }
#endif

#ifdef __EMSCRIPTEN__
// Start a timer that will settle `token` when it fires. The callback runs
// from the JS event loop — i.e. only once the VM has returned control —
// so settling can never re-enter a running dispatch. It just flips a flag
// on the future; the blocked fiber notices on the next scheduler tick,
// because touch re-executes and re-tests rather than caching anything.
EM_JS(void, js_settle_after, (int token, double ms), {
  setTimeout(function() {
    if (Module && Module._vxs_settle_number) Module._vxs_settle_number(token, ms);
  }, ms);
});
#else
static void js_settle_after(int, double) {}
#endif

//=============================================================================
// WebGPU — first contact
//=============================================================================
// Deliberately small. The point is not to wrap WebGPU; it is to prove the
// machinery underneath end to end against REAL promises and REAL host
// objects: (touch (request-adapter)) blocks a fiber, the browser settles
// it from the event loop, the fiber resumes holding a handle, and the same
// again for the device. Pipeline construction stays in JS for now — those
// become primitives once there is something to vary.
#ifdef __EMSCRIPTEN__
EM_JS(int, js_gpu_available, (), {
  return (globalThis.navigator && navigator.gpu) ? 1 : 0;
});

// Both of these settle a token rather than returning: they are promises,
// so a value cannot come back synchronously. A rejection settles as an
// ERROR, which means an ordinary (guard ...) in Scheme catches "this
// machine has no WebGPU" exactly like any other condition.
EM_JS(void, js_request_adapter, (int token), {
  var fail = function(msg) {
    Module.ccall('vxs_settle_error', 'number', ['number', 'string'], [token, msg]);
  };
  if (!globalThis.navigator || !navigator.gpu) { fail("WebGPU unavailable: navigator.gpu is undefined"); return; }
  try {
    navigator.gpu.requestAdapter().then(function(a) {
      if (!a) { fail("requestAdapter returned null (no compatible adapter)"); return; }
      var id = globalThis.vxsHandles.put(a);
      Module.ccall('vxs_settle_handle', 'number', ['number', 'number', 'string'],
                   [token, id, 'gpu-adapter']);
    }, function(e) { fail("requestAdapter rejected: " + e); });
  } catch (e) { fail("requestAdapter threw: " + e); }
});

EM_JS(void, js_request_device, (int token, int adapterId), {
  var fail = function(msg) {
    Module.ccall('vxs_settle_error', 'number', ['number', 'string'], [token, msg]);
  };
  var adapter = globalThis.vxsHandles ? globalThis.vxsHandles.get(adapterId) : null;
  if (!adapter) { fail("request-device: adapter handle is not live"); return; }
  try {
    adapter.requestDevice().then(function(d) {
      var id = globalThis.vxsHandles.put(d);
      Module.ccall('vxs_settle_handle', 'number', ['number', 'number', 'string'],
                   [token, id, 'gpu-device']);
    }, function(e) { fail("requestDevice rejected: " + e); });
  } catch (e) { fail("requestDevice threw: " + e); }
});

// Draw one triangle with the given WGSL. Returns 0 on success, or a
// negative code whose message is fetched separately — EM_JS cannot return
// a string without malloc gymnastics, and an error path is not worth them.
EM_JS(int, js_gpu_draw, (int deviceId, const char *wgslPtr, const char *canvasIdPtr), {
  var wgsl = UTF8ToString(wgslPtr);
  var canvasId = UTF8ToString(canvasIdPtr);
  globalThis.vxsGpuError = "";
  try {
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var canvas = document.getElementById(canvasId);
    if (!canvas) { globalThis.vxsGpuError = "no canvas with id " + canvasId; return -2; }
    var ctx = canvas.getContext('webgpu');
    if (!ctx) { globalThis.vxsGpuError = "getContext('webgpu') returned null"; return -3; }

    var format = navigator.gpu.getPreferredCanvasFormat();
    ctx.configure({ device: device, format: format, alphaMode: 'opaque' });

    var module = device.createShaderModule({ code: wgsl });

    // EXPLICIT pipeline layout, never layout:"auto". With auto, bind group
    // layouts are derived per pipeline, so bind groups become
    // pipeline-specific and every recompile invalidates them — which
    // forecloses hot-swap before it is even attempted. This triangle binds
    // nothing, so the layout is empty; the habit is the point.
    var layout = device.createPipelineLayout({ bindGroupLayouts: [] });

    var pipeline = device.createRenderPipeline({
      layout: layout,
      vertex:   { module: module, entryPoint: 'vs' },
      fragment: { module: module, entryPoint: 'fs', targets: [{ format: format }] },
      primitive: { topology: 'triangle-list' }
    });

    var encoder = device.createCommandEncoder();
    var pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.02, g: 0.03, b: 0.05, a: 1.0 },
        loadOp: 'clear',
        storeOp: 'store'
      }]
    });
    pass.setPipeline(pipeline);
    pass.draw(3);
    pass.end();
    device.queue.submit([encoder.finish()]);
    return 0;
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -4;
  }
});

EM_JS(char *, js_gpu_last_error, (), {
  var s = globalThis.vxsGpuError || "";
  var n = lengthBytesUTF8(s) + 1;
  var p = _malloc(n);
  stringToUTF8(s, p, n);
  return p;
});
#else
static int js_gpu_available() { return 0; }
static void js_request_adapter(int) {}
static void js_request_device(int, int) {}
static int js_gpu_draw(int, const char *, const char *) { return -1; }
static char *js_gpu_last_error() { return nullptr; }
#endif

static void register_wasm_primitives(VM &vm) {
  vm.def_global("gpu-available?", vm.heap.make_subr("gpu-available?", [](VM &, uint32_t, Value *) -> Value {
    return Value::from_bool(js_gpu_available() != 0);
  }, 0, 0));

  vm.def_global("request-adapter", vm.heap.make_subr("request-adapter", [](VM &vm, uint32_t, Value *) -> Value {
    Value fut = vm.heap.make_external_future();
    uint32_t token = vm.register_external(fut);
    js_request_adapter(static_cast<int>(token));
    return fut;
  }, 0, 0));

  vm.def_global("request-device", vm.heap.make_subr("request-device", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) {
      vm.raise_contract("request-device: expected an adapter handle, got " +
                        vm.format_value(args[0]));
    }
    ObjHandle *a = args[0].as_ptr<ObjHandle>();
    if (a->released) vm.raise_contract("request-device: adapter handle was released");
    Value fut = vm.heap.make_external_future();
    uint32_t token = vm.register_external(fut);
    js_request_device(static_cast<int>(token), static_cast<int>(a->id));
    return fut;
  }, 1, 1));

  // (gpu-draw-triangle! device wgsl [canvas-id]) — synchronous: encoding
  // and submitting are not promise-returning. The GPU works afterwards on
  // its own schedule, which is exactly why nothing here needs to wait.
  vm.def_global("gpu-draw-triangle!", vm.heap.make_subr("gpu-draw-triangle!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) {
      vm.raise_contract("gpu-draw-triangle!: expected a device handle, got " +
                        vm.format_value(args[0]));
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-draw-triangle!: device handle was released");
    if (!Heap::is_string(args[1])) {
      vm.raise_contract("gpu-draw-triangle!: expected WGSL source as a string");
    }
    std::string wgsl(args[1].as_ptr<ObjString>()->view());
    std::string canvas_id = (argc > 2 && Heap::is_string(args[2]))
        ? std::string(args[2].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_draw(static_cast<int>(d->id), wgsl.c_str(), canvas_id.c_str());
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-draw-triangle!: " + detail);
    }
    return Value::boolean_true();
  }, 2, 3));

  // (sleep ms) -> future. The first consumer of the external-future path,
  // and useful in its own right: a fiber can wait without blocking the
  // browser, because waiting means "suspend and let the scheduler run",
  // not "spin".
  vm.def_global("sleep", vm.heap.make_subr("sleep", [](VM &vm, uint32_t, Value *args) -> Value {
    double ms = args[0].is_int() ? static_cast<double>(args[0].as_int())
                                 : args[0].as_real();
    Value fut = vm.heap.make_external_future();
    uint32_t token = vm.register_external(fut);
    js_settle_after(static_cast<int>(token), ms);
    return fut;
  }, 1, 1));

  auto subr_clear = [](VM &, uint32_t argc, Value *args) -> Value {
    double r = argc > 0 ? args[0].as_real() : 0.0;
    double g = argc > 1 ? args[1].as_real() : 0.0;
    double b = argc > 2 ? args[2].as_real() : 0.0;
    double a = argc > 3 ? args[3].as_real() : 1.0;
    js_canvas_clear(r, g, b, a);
    return Value::unspecified();
  };
  vm.def_global("canvas-clear", vm.heap.make_subr("canvas-clear", subr_clear, 0, 4));

  auto subr_fill_rect = [](VM &, uint32_t argc, Value *args) -> Value {
    double x = args[0].as_real();
    double y = args[1].as_real();
    double w = args[2].as_real();
    double h = args[3].as_real();
    double r = argc > 4 ? args[4].as_real() : 1.0;
    double g = argc > 5 ? args[5].as_real() : 1.0;
    double b = argc > 6 ? args[6].as_real() : 1.0;
    double a = argc > 7 ? args[7].as_real() : 1.0;
    js_canvas_fill_rect(x, y, w, h, r, g, b, a);
    return Value::unspecified();
  };
  vm.def_global("canvas-fill-rect", vm.heap.make_subr("canvas-fill-rect", subr_fill_rect, 4, 8));

  auto subr_draw_circle = [](VM &, uint32_t argc, Value *args) -> Value {
    double x = args[0].as_real();
    double y = args[1].as_real();
    double rad = args[2].as_real();
    double r = argc > 3 ? args[3].as_real() : 1.0;
    double g = argc > 4 ? args[4].as_real() : 1.0;
    double b = argc > 5 ? args[5].as_real() : 1.0;
    double a = argc > 6 ? args[6].as_real() : 1.0;
    js_canvas_draw_circle(x, y, rad, r, g, b, a);
    return Value::unspecified();
  };
  vm.def_global("canvas-draw-circle", vm.heap.make_subr("canvas-draw-circle", subr_draw_circle, 3, 7));

  auto subr_draw_line = [](VM &, uint32_t argc, Value *args) -> Value {
    double x1 = args[0].as_real();
    double y1 = args[1].as_real();
    double x2 = args[2].as_real();
    double y2 = args[3].as_real();
    double r = argc > 4 ? args[4].as_real() : 1.0;
    double g = argc > 5 ? args[5].as_real() : 1.0;
    double b = argc > 6 ? args[6].as_real() : 1.0;
    double a = argc > 7 ? args[7].as_real() : 1.0;
    js_canvas_draw_line(x1, y1, x2, y2, r, g, b, a);
    return Value::unspecified();
  };
  vm.def_global("canvas-draw-line", vm.heap.make_subr("canvas-draw-line", subr_draw_line, 4, 8));

  auto subr_draw_text = [](VM &, uint32_t argc, Value *args) -> Value {
    std::string text = Heap::is_string(args[0]) ? std::string(args[0].as_ptr<ObjString>()->view()) : "";
    double x = args[1].as_real();
    double y = args[2].as_real();
    double r = argc > 3 ? args[3].as_real() : 1.0;
    double g = argc > 4 ? args[4].as_real() : 1.0;
    double b = argc > 5 ? args[5].as_real() : 1.0;
    double a = argc > 6 ? args[6].as_real() : 1.0;
    js_canvas_draw_text(text.c_str(), x, y, r, g, b, a);
    return Value::unspecified();
  };
  vm.def_global("canvas-draw-text", vm.heap.make_subr("canvas-draw-text", subr_draw_text, 3, 7));

  auto subr_width = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_double(js_canvas_get_width());
  };
  vm.def_global("canvas-width", vm.heap.make_subr("canvas-width", subr_width, 0, 0));

  auto subr_height = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_double(js_canvas_get_height());
  };
  vm.def_global("canvas-height", vm.heap.make_subr("canvas-height", subr_height, 0, 0));

  auto subr_mouse_x = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_double(js_canvas_mouse_x());
  };
  vm.def_global("mouse-x", vm.heap.make_subr("mouse-x", subr_mouse_x, 0, 0));

  auto subr_mouse_y = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_double(js_canvas_mouse_y());
  };
  vm.def_global("mouse-y", vm.heap.make_subr("mouse-y", subr_mouse_y, 0, 0));

  auto subr_mouse_down = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_bool(js_canvas_mouse_down() != 0);
  };
  vm.def_global("mouse-down?", vm.heap.make_subr("mouse-down?", subr_mouse_down, 0, 0));

  auto subr_now = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_double(js_now());
  };
  vm.def_global("current-time", vm.heap.make_subr("current-time", subr_now, 0, 0));

  auto subr_display = [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::ostringstream ss;
    for (uint32_t i = 0; i < argc; ++i) {
      vm.display_value(args[i], ss);
    }
    std::string s = ss.str();
    js_print_output(s.c_str());
    return Value::unspecified();
  };
  vm.def_global("display", vm.heap.make_subr("display", subr_display, 1, UINT32_MAX));

  auto subr_newline = [](VM &, uint32_t, Value *) -> Value {
    js_print_output("\n");
    return Value::unspecified();
  };
  vm.def_global("newline", vm.heap.make_subr("newline", subr_newline, 0, 0));

  auto subr_console_log = [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::ostringstream ss;
    for (uint32_t i = 0; i < argc; ++i) {
      if (i > 0) ss << " ";
      vm.display_value(args[i], ss);
    }
    std::string s = ss.str();
    js_console_log(s.c_str());
    return Value::unspecified();
  };
  vm.def_global("console-log", vm.heap.make_subr("console-log", subr_console_log, 1, UINT32_MAX));
  vm.def_global("console.log", vm.heap.make_subr("console.log", subr_console_log, 1, UINT32_MAX));
}

} // extern "C"

} // namespace vxs

using namespace vxs;

static std::string g_eval_result_buffer;

// A synchronously-evaluated form can suspend — on (yield), or on (touch)
// of a future that has not settled. Drive the scheduler and resume it
// until it finishes or the deadline expires. Without this the form is
// abandoned mid-expression and quietly evaluates to #<unspecified>.
//
// Still bounded by the SAME deadline the caller passed: this is the
// browser's main thread, and pumping forever is exactly the tab freeze
// the deadline exists to prevent. A future that can only be settled from
// outside the VM — a promise, once those exist — therefore cannot be
// awaited from a synchronous eval at all, and correctly reports a
// timeout. The way to await one is from a fiber, driven by
// requestAnimationFrame, which is the whole point of the design.
static VM::StepResult pump_until_settled(Fiber &fiber, VM::StepResult res,
                                         std::chrono::steady_clock::time_point deadline) {
  while (res == VM::StepResult::Yielded &&
         fiber.state == Fiber::State::Suspended &&
         std::chrono::steady_clock::now() < deadline) {
    g_vm->step_all_active_fibers(VM::UNBOUNDED, std::chrono::milliseconds::max());
    if (Heap::is_future(fiber.awaited) && g_vm->active_fibers.empty()) {
      ObjFuture *awaited = fiber.awaited.as_ptr<ObjFuture>();
      if (!awaited->is_completed && !awaited->fiber) {
        // An EXTERNAL future can never settle during a synchronous eval:
        // the JS event loop cannot run while we hold the main thread, so
        // pumping just burns the deadline and reports a useless timeout.
        // Bail immediately with guidance instead — the way to await one is
        // from a fiber, driven by requestAnimationFrame.
        if (awaited->external) {
          fiber.state = Fiber::State::Error;
          fiber.error_message =
              "[VM Error] touch: cannot await an external future from a "
              "synchronous evaluation — the event loop cannot run. Await it "
              "inside (future ...) instead, and let the frame loop drive it.";
          return VM::StepResult::Error;
        }
        // Otherwise nothing in the VM can ever settle it: a real deadlock.
        return VM::StepResult::Preempted;
      }
    }
    res = g_vm->step_fiber(fiber, VM::UNBOUNDED, deadline);
  }
  // Still suspended with the clock run out: a timeout, reported as one.
  if (res == VM::StepResult::Yielded && fiber.state == Fiber::State::Suspended) {
    return VM::StepResult::Preempted;
  }
  return res;
}

// Separate from g_eval_result_buffer on purpose — see vxs_stats_json.
static std::string g_stats_buffer;
// Scheduler counters the VM itself has no reason to keep: these describe
// how the embedder has been pumping it, not the VM's own state.
static size_t g_step_calls = 0;
static size_t g_preempt_total = 0;

extern "C" {

EMSCRIPTEN_KEEPALIVE
int vxs_init() {
  g_vm = std::make_unique<VM>();
  js_ensure_handle_table();
  // Teach the VM how to drop a host object when Scheme releases a handle.
  g_vm->host_handle_releaser = [](uint32_t id) {
    js_release_handle(static_cast<int>(id));
  };
  register_wasm_primitives(*g_vm);
  return 1;
}

static std::string escape_json(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

EMSCRIPTEN_KEEPALIVE
const char *vxs_eval_json(const char *code) {
  if (!g_vm) vxs_init();
  if (!code || *code == '\0') {
    g_eval_result_buffer = "{\"ok\":true,\"result\":\"\",\"type\":\"nil\",\"active_fibers\":0}";
    return g_eval_result_buffer.c_str();
  }

  auto t_start = std::chrono::high_resolution_clock::now();

  try {
    Reader reader(*g_vm, code);
    Value form = reader.read_all_forms();
    Compiler compiler(*g_vm);
    ObjClosure *closure = compiler.compile_top_level(form);

    Fiber fiber;
    fiber.push(Value::from_ptr(closure));
    fiber.stack.resize(std::max<size_t>(1, closure->max_locals), Value::unspecified());
    fiber.frames.push_back({closure, closure->chunk->code.data(), 0});

    // Unbounded instructions, but a wall-clock deadline: this is a
    // synchronous call on the browser's main thread, so a runaway
    // evaluation would freeze the tab. 750ms is editor-scale — generous
    // for a one-shot "run what's in the buffer" action. A hit deadline
    // is reported as a timeout, never dressed up as a result — that
    // would be the Yielded/Preempted conflation rebuilt one layer up.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    VM::StepResult res = g_vm->step_fiber(fiber, VM::UNBOUNDED, deadline);
    res = pump_until_settled(fiber, res, deadline);
    auto t_end = std::chrono::high_resolution_clock::now();
    double time_us = std::chrono::duration<double, std::micro>(t_end - t_start).count();

    if (res == VM::StepResult::Preempted) {
      g_eval_result_buffer = "{\"ok\":false,\"error\":\"evaluation exceeded 750ms and was stopped\",\"error_type\":\"timeout\",\"time_us\":" + std::to_string(time_us) + "}";
    } else if (res == VM::StepResult::Error) {
      g_eval_result_buffer = "{\"ok\":false,\"error\":\"" + escape_json(fiber.error_message) + "\",\"error_type\":\"runtime\",\"time_us\":" + std::to_string(time_us) + "}";
    } else {
      std::string formatted = g_vm->format_value(fiber.result);
      std::string type_name = fiber.result.is_int() ? "integer" :
                              fiber.result.is_double() ? "real" :
                              fiber.result.is_symbol() ? "symbol" :
                              fiber.result.is_bool() ? "boolean" :
                              fiber.result.is_nil() ? "nil" :
                              Heap::is_cons(fiber.result) ? "pair" :
                              Heap::is_closure(fiber.result) ? "procedure" :
                              Heap::is_subr(fiber.result) ? "primitive" :
                              Heap::is_future(fiber.result) ? "future" : "object";

      g_eval_result_buffer = "{\"ok\":true,\"result\":\"" + escape_json(formatted) +
                             "\",\"type\":\"" + type_name +
                             "\",\"time_us\":" + std::to_string(time_us) +
                             ",\"active_fibers\":" + std::to_string(g_vm->active_fibers.size()) + "}";
    }
  } catch (const RaiseEscape &e) {
    // An uncaught (raise ...)/(error ...) — a Scheme-level condition,
    // not a VM-internal fault, hence "runtime" (matching the ordinary
    // StepResult::Error branch above) rather than the generic "exception"
    // the catch-all below reports for things like a stale continuation.
    g_eval_result_buffer = "{\"ok\":false,\"error\":\"" + escape_json(e.what()) + "\",\"error_type\":\"runtime\"}";
  } catch (const std::exception &e) {
    g_eval_result_buffer = "{\"ok\":false,\"error\":\"" + escape_json(e.what()) + "\",\"error_type\":\"exception\"}";
  }

  return g_eval_result_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char *vxs_eval(const char *code) {
  if (!g_vm) vxs_init();
  if (!code || *code == '\0') {
    g_eval_result_buffer = "";
    return g_eval_result_buffer.c_str();
  }

  try {
    Reader reader(*g_vm, code);
    Value form = reader.read_all_forms();
    Compiler compiler(*g_vm);
    ObjClosure *closure = compiler.compile_top_level(form);

    Fiber fiber;
    fiber.push(Value::from_ptr(closure));
    fiber.stack.resize(std::max<size_t>(1, closure->max_locals), Value::unspecified());
    fiber.frames.push_back({closure, closure->chunk->code.data(), 0});

    // Same deadline treatment as vxs_eval_json above.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    VM::StepResult res = g_vm->step_fiber(fiber, VM::UNBOUNDED, deadline);
    res = pump_until_settled(fiber, res, deadline);
    if (res == VM::StepResult::Preempted) {
      g_eval_result_buffer = "[Timeout] evaluation exceeded 750ms and was stopped";
    } else if (res == VM::StepResult::Error) {
      g_eval_result_buffer = fiber.error_message;
    } else {
      g_eval_result_buffer = g_vm->format_value(fiber.result);
    }
  } catch (const RaiseEscape &e) {
    // Already fully formatted — see the matching catch in vxs_eval_json.
    g_eval_result_buffer = e.what();
  } catch (const std::exception &e) {
    g_eval_result_buffer = "[Evaluation Error] " + std::string(e.what());
  }

  return g_eval_result_buffer.c_str();
}

// The per-rAF-frame scheduler pump. Default (arg <= 0): every fiber runs
// to its own (yield) under a shared ~8ms wall-clock backstop — half a
// 60fps frame, leaving the rest for rendering/compositing. The backstop
// is tab-freeze protection, not a scheduling device: a fiber it cuts off
// resumes exclusively next frame (see step_all_active_fibers), and the
// overrun is reported to the devtools console below because it means
// that fiber isn't yielding — a bug in the fiber, worth hearing about.
// A positive argument opts into the legacy/debug instruction cap instead.
EMSCRIPTEN_KEEPALIVE
int vxs_step_fibers(int instructions_per_fiber) {
  if (!g_vm) return 0;
  size_t preempted;
  if (instructions_per_fiber > 0) {
    preempted = g_vm->step_all_active_fibers(static_cast<size_t>(instructions_per_fiber));
  } else {
    preempted = g_vm->step_all_active_fibers(VM::UNBOUNDED, std::chrono::milliseconds(8));
  }
  ++g_step_calls;
  g_preempt_total += preempted;
  // Only meaningful on the default (deadline) path: there, a preemption
  // means a fiber ran past the frame budget without yielding, which is a
  // bug in that fiber and worth hearing about. Under an explicit
  // instruction cap the caller ASKED to be cut off at N instructions, so
  // preemption is the requested behaviour and there is no frame budget to
  // exceed — warning there just prints a falsehood on every call.
  if (preempted > 0 && instructions_per_fiber <= 0) {
    fprintf(stderr, "[vxs] %zu fiber(s) exceeded the frame budget without yielding\n", preempted);
  }
  return static_cast<int>(g_vm->active_fibers.size());
}

// Runtime counters as JSON. One export rather than a dozen numeric ones:
// it matches vxs_eval_json's idiom, and new counters can be added without
// touching the Makefile's export list.
//
// Deliberately its own buffer, NOT g_eval_result_buffer — polling stats
// must never invalidate the char* a caller is still holding from an eval.
//
// The live_* pair is instantaneous; the total_* pair is cumulative and
// never decreases, which is what makes allocation RATE measurable. Live
// bytes alone cannot tell "allocates nothing" apart from "allocates
// furiously and collects all of it" — the difference this whole workload
// turns on.
EMSCRIPTEN_KEEPALIVE
const char *vxs_stats_json() {
  if (!g_vm) {
    g_stats_buffer = "{\"ready\":false}";
    return g_stats_buffer.c_str();
  }
  const Heap &h = g_vm->heap;
  g_stats_buffer =
      std::string("{\"ready\":true") +
      ",\"live_bytes\":" + std::to_string(h.get_bytes_allocated()) +
      ",\"live_objects\":" + std::to_string(h.get_live_objects()) +
      ",\"total_bytes_allocated\":" + std::to_string(h.get_total_bytes_allocated()) +
      ",\"total_objects_allocated\":" + std::to_string(h.get_total_objects_allocated()) +
      ",\"total_objects_freed\":" + std::to_string(h.get_total_objects_freed()) +
      ",\"gc_count\":" + std::to_string(h.get_gc_count()) +
      ",\"gc_last_freed\":" + std::to_string(h.get_last_gc_freed()) +
      ",\"gc_threshold\":" + std::to_string(h.get_gc_threshold()) +
      ",\"active_fibers\":" + std::to_string(g_vm->active_fibers.size()) +
      ",\"step_calls\":" + std::to_string(g_step_calls) +
      ",\"fibers_preempted_total\":" + std::to_string(g_preempt_total) +
      "}";
  return g_stats_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
int vxs_active_fibers_count() {
  if (!g_vm) return 0;
  return static_cast<int>(g_vm->active_fibers.size());
}

EMSCRIPTEN_KEEPALIVE
void vxs_clear_fibers() {
  if (!g_vm) return;
  for (Fiber *f : g_vm->active_fibers) delete f;
  g_vm->active_fibers.clear();
  g_vm->preempted_fiber = nullptr;
  // Drop the roots on anything still in flight. Callbacks already
  // scheduled will fire into a world with no matching token, which
  // settle_external handles as the quiet no-op it should be.
  g_vm->pending_externals.clear();
}

// --- settling a future from outside the VM ---------------------------
// Called from JS when a promise (or timer, or GPU callback) resolves.
// Returns 1 if a waiter was settled, 0 if the token is unknown — which is
// routine, not an error: the callback outlives its future whenever the
// page reloads, fibers are cleared, or a request is cancelled.
EMSCRIPTEN_KEEPALIVE
int vxs_settle_number(int token, double value) {
  if (!g_vm) return 0;
  return g_vm->settle_external(static_cast<uint32_t>(token),
                               Value::from_double(value), false) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int vxs_settle_string(int token, const char *value) {
  if (!g_vm) return 0;
  return g_vm->settle_external(static_cast<uint32_t>(token),
                               g_vm->heap.make_string(value ? value : ""),
                               false) ? 1 : 0;
}

// A rejected promise settles the future as failed; touching it then raises,
// so an ordinary (guard ...) catches a GPU error like any other condition.
// Settle with a HANDLE to a host object the promise resolved to — a
// GPUAdapter, GPUDevice, GPUBuffer. `id` is an index the caller already
// put into globalThis.vxsHandles; `kind` names it for error messages and
// predicates ("gpu-device", "gpu-buffer", ...).
EMSCRIPTEN_KEEPALIVE
int vxs_settle_handle(int token, int id, const char *kind) {
  if (!g_vm) return 0;
  Value h = g_vm->heap.make_handle(static_cast<uint32_t>(id),
                                   g_vm->intern(kind ? kind : "host-object"));
  return g_vm->settle_external(static_cast<uint32_t>(token), h, false) ? 1 : 0;
}

// How many host objects we are holding. Nothing collects these, so this is
// the leak instrument: a workbench can watch it, and a number that only
// climbs is the bug.
EMSCRIPTEN_KEEPALIVE
int vxs_host_handle_count() {
  return js_handle_count();
}

EMSCRIPTEN_KEEPALIVE
int vxs_settle_error(int token, const char *message) {
  if (!g_vm) return 0;
  return g_vm->settle_external(static_cast<uint32_t>(token),
                               g_vm->heap.make_string(message ? message : "external failure"),
                               true) ? 1 : 0;
}

// How many futures are awaiting the outside world — surfaced so a
// workbench can distinguish "idle" from "waiting on the GPU".
EMSCRIPTEN_KEEPALIVE
int vxs_pending_externals() {
  if (!g_vm) return 0;
  return static_cast<int>(g_vm->pending_externals.size());
}

} // extern "C"
