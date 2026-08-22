#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include "vx_reader.h"
#include "vx_compiler.h"
#include "vx_embedded_libs.h"
#include <string>
#include <cstring>
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

// Frames pumped. Declared here rather than beside the scheduler because
// gpu-buffer-read stamps its snapshot with it, and that primitive is
// registered long before the scheduler code appears.
static size_t g_step_calls = 0;

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

// Scroll accumulated since page load — a LEVEL, like mouse position,
// rather than an event. A "give me the delta and reset" call would be
// simpler and would break the moment two things wanted to read it; a
// running total lets each reader keep its own last value.
// Is the page asking the simulation to hold still?
//
// A pause that stopped the scheduler would freeze the RENDERER too, since
// the render loop is a fiber like any other — and then the camera could
// not be moved while paused, which is exactly when you most want to move
// it. So pause is a flag the running program consults rather than
// something done to the program.
EM_JS(int, js_paused, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsPaused) ? 1 : 0;
});

EM_JS(double, js_canvas_mouse_wheel, (), {
  return (typeof globalThis !== 'undefined' && globalThis.vxsMouseWheel) ? globalThis.vxsMouseWheel() : 0.0;
});

EM_JS(double, js_now, (), {
  return (typeof performance !== 'undefined' && performance.now) ? performance.now() : 0.0;
});

EM_JS(void, js_console_log, (const char *text), {
  if (typeof console !== 'undefined' && console.log) {
    console.log(UTF8ToString(text));
  }
});

// One hook for every output port in the browser. The port carries a sink
// NAME and JS decides where that name goes, so a page can add a sink (a
// div, localStorage, a websocket) without any new C++ — the names below
// are just the two that ship by default.
EM_JS(void, js_sink_write, (const char *name, const char *text), {
  var n = UTF8ToString(name);
  var s = UTF8ToString(text);
  if (typeof globalThis !== 'undefined' && globalThis.vxsSinks &&
      typeof globalThis.vxsSinks[n] === 'function') {
    globalThis.vxsSinks[n](s);
    return;
  }
  if (n === 'console') {
    if (typeof console !== 'undefined' && console.log) console.log(s);
    return;
  }
  if (typeof globalThis !== 'undefined' && globalThis.vxsPrint) {
    globalThis.vxsPrint(s);
  } else if (typeof console !== 'undefined' && console.log) {
    console.log(s);
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
static double js_canvas_mouse_wheel() { return 0.0; }
static int js_paused() { return 0; }
static double js_now() { return 0.0; }
static void js_console_log(const char *text) { std::cout << "[CONSOLE.LOG] " << text << std::endl; }
static void js_sink_write(const char *name, const char *text) {
  if (std::string(name) == "console") std::cout << "[CONSOLE.LOG] " << text << std::endl;
  else std::cout << text << std::flush;
}
#endif

// A streambuf that forwards to a named JS sink, one COMPLETE LINE at a
// time. Line buffering rather than per-write forwarding because the JS
// terminal appends a div per call: unbuffered, (display '(1 2)) would
// arrive as five separate lines. It also fixes the reverse bug — before
// this, `(display "x = ") (display 42)` produced two terminal lines
// instead of one, because each display call was its own call into JS.
//
// A trailing partial line stays buffered until a newline or an explicit
// flush; vxs_eval/vxs_eval_json flush at the end of evaluation so nothing
// is left stranded.
class SinkBuf : public std::streambuf {
public:
  explicit SinkBuf(std::string sink_name) : name_(std::move(sink_name)) {}
  ~SinkBuf() override { emit_pending(); }

protected:
  int overflow(int ch) override {
    if (ch == traits_type::eof()) return traits_type::not_eof(ch);
    char c = static_cast<char>(ch);
    if (c == '\n') {
      emit(pending_);
      pending_.clear();
    } else {
      pending_.push_back(c);
    }
    return ch;
  }

  std::streamsize xsputn(const char *s, std::streamsize n) override {
    for (std::streamsize i = 0; i < n; ++i) overflow(traits_type::to_int_type(s[i]));
    return n;
  }

  int sync() override {
    emit_pending();
    return 0;
  }

private:
  void emit_pending() {
    if (pending_.empty()) return;
    emit(pending_);
    pending_.clear();
  }
  void emit(const std::string &line) { js_sink_write(name_.c_str(), line.c_str()); }

  std::string name_;
  std::string pending_;
};

static Value make_sink_port(VM &vm, const std::string &name) {
  return vm.heap.make_custom_output_port(std::make_unique<SinkBuf>(name));
}

// The two sinks that ship by default. Held as plain Values because both are
// ALSO bound as VM globals, and that binding is what keeps them alive — a
// registry of every sink port here would not be a GC root, so it would
// dangle the moment one became unreachable from Scheme.
static Value g_terminal_port = Value::unspecified();
static Value g_console_port = Value::unspecified();

// Flush at the end of evaluation so a trailing partial line — a `display`
// with no newline — reaches the page instead of sitting in the buffer.
// Ports from open-output-sink are the caller's to flush with
// flush-output-port, or they flush themselves when collected.
static void flush_default_sinks(VM &vm) {
  Value ports[3] = {vm.current_out_port, g_terminal_port, g_console_port};
  for (Value p : ports) {
    if (Heap::is_port(p)) {
      ObjPort *op = p.as_ptr<ObjPort>();
      if (op->out) op->out->flush();
    }
  }
}

// Register Canvas & Web primitives
#ifdef __EMSCRIPTEN__
// The host-side object table. Lives on globalThis so the embedder (app.js,
// a test harness, eventually the WebGPU bindings) can put objects in and
// read them back out; the VM only ever sees the integer.
// Shader compilation, as a future.
//
// createShaderModule DOES NOT THROW on a bad shader — WebGPU reports
// compile problems asynchronously, through getCompilationInfo. So a kernel
// that was not a WGSL program by any reading used to produce an apparently
// successful module, an invalid pipeline, a try/catch that never fired, a
// return code of 0, and a dispatch that silently did nothing: 60fps, no
// error, black canvas. The compile log, with a line and a column, existed
// the whole time and was discarded.
//
// The fix is to make compilation a step the caller takes, ONCE, and to
// give it the shape asynchronous work already has here. gpu-compile
// returns a future; it settles with a shader handle, or it FAILS with the
// compile message, and touching a failed future raises. From then on the
// draw calls take that handle, so there is no path through this file that
// reaches a pipeline without a validated module behind it.
//
// The alternative considered — let the draw keep taking source and poison
// a cache entry when compilation is later found to have failed — works,
// but it makes the first frame silently wrong, keys pipelines by whole
// source strings, and puts the diagnosis a frame behind the mistake. A
// future costs one touch at setup and none afterwards, which is the right
// price for something that happens once per shader.
EM_JS(void, js_gpu_compile, (int token, int deviceId, const char *wgslPtr), {
  var code = UTF8ToString(wgslPtr);
  var fail = function(msg) {
    Module.ccall('vxs_settle_error', 'number', ['number', 'string'], [token, msg]);
  };
  // Diagnostics go to the PAGE, not the devtools console: a failure only
  // reaches someone working without a browser if it becomes text they can
  // paste.
  var term = function(text) {
    if (typeof globalThis.vxsPrint === 'function') globalThis.vxsPrint(text);
    else if (typeof console !== 'undefined') console.log(text);
  };
  try {
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { fail("gpu-compile: device handle is not live"); return; }
    var mod = device.createShaderModule({ code: code });

    var settle = function(errText) {
      if (errText) { fail(errText); return; }
      var id = globalThis.vxsHandles.put({ module: mod, code: code });
      Module.ccall('vxs_settle_handle', 'number', ['number', 'number', 'string'],
                   [token, id, 'gpu-shader']);
    };

    if (!mod.getCompilationInfo) { settle(null); return; }
    mod.getCompilationInfo().then(function(info) {
      var lines = code.split('\n');
      var firstError = null;
      for (var i = 0; i < info.messages.length; i++) {
        var m = info.messages[i];
        if (m.type === 'info') continue;
        var head = 'WGSL ' + m.type + ' at line ' + m.lineNum +
                   ':' + m.linePos + ' — ' + m.message;
        term(head);
        var src = lines[m.lineNum - 1];
        if (src !== undefined) term('    ' + m.lineNum + ' | ' + src);
        // Only an error fails the compile. A warning is worth printing and
        // must not reject a shader the driver was willing to accept.
        if (m.type === 'error' && firstError === null) firstError = head;
      }
      settle(firstError);
    }, function(e) {
      settle("gpu-compile: could not read compilation info: " + e);
    });
  } catch (e) {
    fail("gpu-compile: " + e);
  }
});

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
static void js_gpu_compile(int, int, const char *) {}
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
  var D = function() {
    if (globalThis.vxsDebugGpu) console.log.apply(console, ['[vxs gpu]'].concat([].slice.call(arguments)));
  };
  var fail = function(msg) {
    D('settling error for token', token, msg);
    var r = Module.ccall('vxs_settle_error', 'number', ['number', 'string'], [token, msg]);
    D('  settle_error returned', r);
  };
  D('request-adapter: token', token,
    '| Module?', typeof Module, '| ccall?', typeof (Module && Module.ccall),
    '| handles?', typeof globalThis.vxsHandles);
  if (!globalThis.navigator || !navigator.gpu) { fail("WebGPU unavailable: navigator.gpu is undefined"); return; }
  try {
    var p = navigator.gpu.requestAdapter();
    D('  requestAdapter() returned', p && typeof p.then);
    p.then(function(a) {
      D('  resolved with', a ? 'an adapter' : 'null');
      if (!a) { fail("requestAdapter returned null (no compatible adapter)"); return; }
      try {
        var id = globalThis.vxsHandles.put(a);
        D('  handle id', id, '-> settling token', token);
        var r = Module.ccall('vxs_settle_handle', 'number', ['number', 'number', 'string'],
                             [token, id, 'gpu-adapter']);
        D('  settle_handle returned', r, '(1 = a waiter was settled)');
      } catch (inner) {
        D('  SETTLE THREW', inner);
        console.error('[vxs gpu] settle threw:', inner);
      }
    }, function(e) { D('  rejected', e); fail("requestAdapter rejected: " + e); });
  } catch (e) { fail("requestAdapter threw: " + e); }
});

EM_JS(void, js_request_device, (int token, int adapterId), {
  var D = function() {
    if (globalThis.vxsDebugGpu) console.log.apply(console, ['[vxs gpu]'].concat([].slice.call(arguments)));
  };
  var fail = function(msg) {
    Module.ccall('vxs_settle_error', 'number', ['number', 'string'], [token, msg]);
  };
  var adapter = globalThis.vxsHandles ? globalThis.vxsHandles.get(adapterId) : null;
  D('request-device: token', token, 'adapter', adapterId, adapter ? 'live' : 'MISSING');
  if (!adapter) { fail("request-device: adapter handle is not live"); return; }
  try {
    adapter.requestDevice().then(function(d) {
      try {
        // Validation failures that are not compile errors surface here
        // and nowhere else. Three lines, and without them a bad bind group
        // or a buffer-size mismatch is as silent as a bad shader was.
        if (d.addEventListener) {
          d.addEventListener('uncapturederror', function(ev) {
            var msg = '[gpu] ' + (ev.error && ev.error.message ? ev.error.message : ev.error);
            if (typeof globalThis.vxsPrint === 'function') globalThis.vxsPrint(msg);
            else console.error(msg);
          });
        }
        var id = globalThis.vxsHandles.put(d);
        var r = Module.ccall('vxs_settle_handle', 'number', ['number', 'number', 'string'],
                             [token, id, 'gpu-device']);
        D('  device handle', id, 'settle returned', r);
      } catch (inner) {
        console.error('[vxs gpu] device settle threw:', inner);
      }
    }, function(e) { fail("requestDevice rejected: " + e); });
  } catch (e) { fail("requestDevice threw: " + e); }
});

// Draw one triangle with the given WGSL. Returns 0 on success, or a
// negative code whose message is fetched separately — EM_JS cannot return
// a string without malloc gymnastics, and an error path is not worth them.
EM_JS(int, js_gpu_draw, (int deviceId, int shaderId, const char *canvasIdPtr), {
  var canvasId = UTF8ToString(canvasIdPtr);
  globalThis.vxsGpuError = "";
  try {
    var shader = globalThis.vxsHandles ? globalThis.vxsHandles.get(shaderId) : null;
    if (!shader) { globalThis.vxsGpuError = "shader handle is not live"; return -6; }
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var canvas = document.getElementById(canvasId);
    if (!canvas) { globalThis.vxsGpuError = "no canvas with id " + canvasId; return -2; }
    var ctx = canvas.getContext('webgpu');
    if (!ctx) { globalThis.vxsGpuError = "getContext('webgpu') returned null"; return -3; }

    var format = navigator.gpu.getPreferredCanvasFormat();
    ctx.configure({ device: device, format: format, alphaMode: 'opaque' });

    var module = shader.module;

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

// Draw one frame of a kernel shader. Unlike js_gpu_draw above, which
// rebuilds everything per call because it draws a static triangle once,
// this caches the pipeline, uniform buffer and bind group keyed by
// (canvas, source): a per-frame shader recompile would dominate the frame
// budget completely. Change the source and you get a new entry, which is
// what makes livecoding a shader a matter of handing over a new string.
//
// Explicit bind group layout, never layout:"auto" - with auto, layouts are
// derived per pipeline, so bind groups become pipeline-specific and every
// recompile invalidates them, foreclosing exactly the hot-swap this is for.
EM_JS(int, js_gpu_run_kernel, (int deviceId, int shaderId, const char *canvasIdPtr, double time), {
  var canvasId = UTF8ToString(canvasIdPtr);
  globalThis.vxsGpuError = "";
  try {
    var shader = globalThis.vxsHandles ? globalThis.vxsHandles.get(shaderId) : null;
    if (!shader) { globalThis.vxsGpuError = "shader handle is not live"; return -6; }
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var canvas = document.getElementById(canvasId);
    if (!canvas) { globalThis.vxsGpuError = "no canvas with id " + canvasId; return -2; }
    var ctx = canvas.getContext('webgpu');
    if (!ctx) { globalThis.vxsGpuError = "getContext('webgpu') returned null"; return -3; }

    globalThis.vxsKernelCache = globalThis.vxsKernelCache || {};
    var key = canvasId + " " + shaderId;
    var entry = globalThis.vxsKernelCache[key];
    if (!entry || entry.device !== device) {
      var format = navigator.gpu.getPreferredCanvasFormat();
      ctx.configure({ device: device, format: format, alphaMode: 'opaque' });
      var module = shader.module;
      var bgl = device.createBindGroupLayout({
        entries: [{
          binding: 0,
          visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
          buffer: { type: 'uniform' }
        }]
      });
      var pipeline = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [bgl] }),
        vertex:   { module: module, entryPoint: 'vs' },
        fragment: { module: module, entryPoint: 'fs', targets: [{ format: format }] },
        primitive: { topology: 'triangle-list' }
      });
      var ubuf = device.createBuffer({
        size: 16,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
      });
      var bind = device.createBindGroup({
        layout: bgl,
        entries: [{ binding: 0, resource: { buffer: ubuf } }]
      });
      entry = { device: device, pipeline: pipeline, ubuf: ubuf, bind: bind };
      globalThis.vxsKernelCache[key] = entry;
    }

    // Matches struct U in lib/shadertoy.scm: time, width, height, pad.
    device.queue.writeBuffer(entry.ubuf, 0,
      new Float32Array([time, canvas.width, canvas.height, 0.0]));

    var encoder = device.createCommandEncoder();
    var pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.0, g: 0.0, b: 0.0, a: 1.0 },
        loadOp: 'clear',
        storeOp: 'store'
      }]
    });
    pass.setPipeline(entry.pipeline);
    pass.setBindGroup(0, entry.bind);
    pass.draw(3);
    pass.end();
    device.queue.submit([encoder.finish()]);
    return 0;
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -4;
  }
});

// Instanced draw over a storage buffer of point data.
//
// The buffer is a flat array<f32>, NOT an array of structs. WGSL gives
// vec3<f32> a 16-byte alignment inside a storage array, so a struct like
// { pos : vec2<f32>, size : f32, color : vec3<f32> } does not pack the way
// the same fields packed tightly on the host do — the mismatch is silent
// and shows up as points drawn in the wrong places. Indexing a flat float
// array by hand sidesteps the alignment rules entirely; the stride lives
// in lib/points.scm and here, and nowhere else.
//
// Pipeline, uniform buffer and storage buffer are cached by (canvas,
// source) exactly as js_gpu_run_kernel does. The storage buffer is
// reallocated only when it needs to grow.
EM_JS(int, js_gpu_draw_instances, (int deviceId, int shaderId, const char *canvasIdPtr, const unsigned char *dataPtr, int dataLen, int instances, double time, double yaw, double pitch, double dist, double fov), {
  var canvasId = UTF8ToString(canvasIdPtr);
  globalThis.vxsGpuError = "";
  try {
    var shader = globalThis.vxsHandles ? globalThis.vxsHandles.get(shaderId) : null;
    if (!shader) { globalThis.vxsGpuError = "shader handle is not live"; return -6; }
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var canvas = document.getElementById(canvasId);
    if (!canvas) { globalThis.vxsGpuError = "no canvas with id " + canvasId; return -2; }
    var ctx = canvas.getContext('webgpu');
    if (!ctx) { globalThis.vxsGpuError = "getContext('webgpu') returned null"; return -3; }

    globalThis.vxsInstanceCache = globalThis.vxsInstanceCache || {};
    var key = canvasId + " " + shaderId;
    var entry = globalThis.vxsInstanceCache[key];
    if (!entry || entry.device !== device) {
      var format = navigator.gpu.getPreferredCanvasFormat();
      ctx.configure({ device: device, format: format, alphaMode: 'opaque' });
      var module = shader.module;
      var bgl = device.createBindGroupLayout({
        entries: [
          { binding: 0,
            visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform' } },
          { binding: 1,
            visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'read-only-storage' } }
        ]
      });
      var pipeline = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [bgl] }),
        vertex:   { module: module, entryPoint: 'vs' },
        // Additive blending: overlapping points accumulate instead of
        // overwriting, which is what makes a cloud of them read as density.
        fragment: { module: module, entryPoint: 'fs', targets: [{
          format: format,
          blend: {
            color: { srcFactor: 'src-alpha', dstFactor: 'one', operation: 'add' },
            alpha: { srcFactor: 'zero', dstFactor: 'one', operation: 'add' }
          }
        }] },
        primitive: { topology: 'triangle-list' }
      });
      var ubuf = device.createBuffer({
        size: 32,   // struct U: 8 floats — see lib/points.scm
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
      });
      entry = { device: device, pipeline: pipeline, bgl: bgl, ubuf: ubuf,
                sbuf: null, sbufSize: 0, bind: null };
      globalThis.vxsInstanceCache[key] = entry;
    }

    // Grow the storage buffer only when it must. Rebuilding the bind group
    // is required whenever the buffer object itself changes.
    var needed = Math.max(16, dataLen);
    if (!entry.sbuf || entry.sbufSize < needed) {
      if (entry.sbuf) entry.sbuf.destroy();
      entry.sbuf = device.createBuffer({
        size: (needed + 15) & ~15,
        usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST
      });
      entry.sbufSize = needed;
      entry.bind = device.createBindGroup({
        layout: entry.bgl,
        entries: [
          { binding: 0, resource: { buffer: entry.ubuf } },
          { binding: 1, resource: { buffer: entry.sbuf } }
        ]
      });
    }

    // Field order is a contract with struct U in lib/points.scm. Nothing
    // but layer 17 says so, and getting it wrong is silent: the shader
    // would animate on canvas width, or orbit on the point count.
    device.queue.writeBuffer(entry.ubuf, 0,
      new Float32Array([time, canvas.width, canvas.height, instances,
                        yaw, pitch, dist, fov]));
    // Copy out of the wasm heap: writeBuffer on a view INTO the heap would
    // be reading memory the VM may move or reuse before the copy happens.
    device.queue.writeBuffer(entry.sbuf, 0,
      new Uint8Array(HEAPU8.subarray(dataPtr, dataPtr + dataLen)));

    var encoder = device.createCommandEncoder();
    var pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.0, g: 0.0, b: 0.0, a: 1.0 },
        loadOp: 'clear',
        storeOp: 'store'
      }]
    });
    pass.setPipeline(entry.pipeline);
    pass.setBindGroup(0, entry.bind);
    pass.draw(6, instances);   // two triangles per instance
    pass.end();
    device.queue.submit([encoder.finish()]);
    return 0;
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -4;
  }
});

// --- compute wrangle ---------------------------------------------------
//
// The point data now LIVES on the GPU. gpu-draw-instances! re-uploads the
// whole buffer every frame, which is right while Scheme is the producer and
// wrong the moment a compute pass is: uploading a buffer only to have the
// GPU immediately overwrite it is pure waste, and it would also lose
// whatever the previous frame computed. So the buffer is created once,
// seeded once, and thereafter read and written in place.

EM_JS(int, js_gpu_create_buffer, (int deviceId, const unsigned char *dataPtr, int dataLen), {
  globalThis.vxsGpuError = "";
  try {
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var size = Math.max(16, (dataLen + 15) & ~15);
    var buf = device.createBuffer({
      size: size,
      usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC
    });
    if (dataLen > 0) {
      device.queue.writeBuffer(buf, 0,
        new Uint8Array(HEAPU8.subarray(dataPtr, dataPtr + dataLen)));
    }
    return globalThis.vxsHandles.put(buf);
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -2;
  }
});

// One compute dispatch over `count` points. The pipeline is cached by
// source, as everything else here is; the bind group additionally depends
// on WHICH buffer, so the buffer id is part of its key.
EM_JS(int, js_gpu_wrangle, (int deviceId, int bufId, int shaderId, int count, double time, int seed, const unsigned char *paramsPtr, int paramsLen, int steps, int scratchId, int sharedId), {
  globalThis.vxsGpuError = "";
  try {
    var shader = globalThis.vxsHandles ? globalThis.vxsHandles.get(shaderId) : null;
    if (!shader) { globalThis.vxsGpuError = "shader handle is not live"; return -6; }
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var buf = globalThis.vxsHandles.get(bufId);
    if (!buf) { globalThis.vxsGpuError = "point buffer handle is not live"; return -2; }

    globalThis.vxsWrangleCache = globalThis.vxsWrangleCache || {};
    // The scratch buffer is part of the KEY, not just the bind group: a
    // kernel that declares binding 2 needs a three-entry layout, and one
    // that does not must keep the two-entry layout it had — a pipeline
    // cannot be reused across the two.
    var key = bufId + " " + shaderId + " " + scratchId + " " + sharedId;
    if (steps < 1) steps = 1;
    var scratch = scratchId
      ? (globalThis.vxsHandles ? globalThis.vxsHandles.get(scratchId) : null)
      : null;
    if (scratchId && !scratch) {
      globalThis.vxsGpuError = "scratch buffer handle is not live";
      return -7;
    }
    // Shared data every element reads, as opposed to scratch, which each
    // element owns. READ-ONLY in the shader, so the layout entry must say
    // 'read-only-storage' — a plain 'storage' entry against a
    // var<storage, read> declaration is a validation failure, and one that
    // surfaces through uncapturederror rather than as a compile error.
    var shared = sharedId
      ? (globalThis.vxsHandles ? globalThis.vxsHandles.get(sharedId) : null)
      : null;
    if (sharedId && !shared) {
      globalThis.vxsGpuError = "shared buffer handle is not live";
      return -8;
    }
    // Alignment is a device limit, not a constant. 256 is the guaranteed
    // maximum and the near-universal value, but reading it is free.
    var align = (device.limits && device.limits.minUniformBufferOffsetAlignment) || 256;
    var ustride = Math.ceil(48 / align) * align;
    var entry = globalThis.vxsWrangleCache[key];
    if (!entry || entry.device !== device) {
      var module = shader.module;
      var bgl = device.createBindGroupLayout({
        entries: [
          { binding: 0, visibility: GPUShaderStage.COMPUTE,
            buffer: { type: 'uniform', hasDynamicOffset: true } },
          { binding: 1, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } }
        ].concat(scratch ? [
          { binding: 2, visibility: GPUShaderStage.COMPUTE, buffer: { type: 'storage' } }
        ] : []).concat(shared ? [
          { binding: 3, visibility: GPUShaderStage.COMPUTE,
            buffer: { type: 'read-only-storage' } }
        ] : [])
      });
      var pipeline = device.createComputePipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [bgl] }),
        compute: { module: module, entryPoint: 'main' }
      });
      var ubuf = device.createBuffer({
        // struct WU: time/count/seed/pad, then eight parameter slots.
        // 48 rather than 16 because a kernel constant baked into the
        // source recompiles the shader every time it changes; in the
        // uniform it is free, and the uniform is rewritten before every
        // dispatch anyway.
        //
        // One such struct PER SUBSTEP, spaced by the device's dynamic
        // offset alignment. Substeps run inside a single pass, so nothing
        // can rewrite the uniform between them — the only way to give each
        // its own step index is to write them all up front and point the
        // bind group at a different slice per dispatch.
        size: ustride * steps,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
      });
      var bind = device.createBindGroup({
        layout: bgl,
        entries: [
          // An explicit size is REQUIRED with a dynamic offset: without
          // it the binding runs to the end of the buffer, and every offset
          // past the first would overrun it.
          { binding: 0, resource: { buffer: ubuf, offset: 0, size: 48 } },
          { binding: 1, resource: { buffer: buf } }
        ].concat(scratch ? [{ binding: 2, resource: { buffer: scratch } }] : [])
         .concat(shared ? [{ binding: 3, resource: { buffer: shared } }] : [])
      });
      entry = { device: device, pipeline: pipeline, ubuf: ubuf, bind: bind,
                bgl: bgl, ustride: ustride, cap: steps };
      globalThis.vxsWrangleCache[key] = entry;
    }

    // Built for fewer substeps than we now want: rebuild rather than
    // clamp. Silently running fewer steps than asked is a wrong answer
    // wearing the costume of a slow one.
    if (entry.cap < steps) {
      entry.ubuf.destroy();
      entry.ubuf = device.createBuffer({
        size: entry.ustride * steps,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
      });
      entry.bind = device.createBindGroup({
        layout: entry.bgl,
        entries: [
          { binding: 0, resource: { buffer: entry.ubuf, offset: 0, size: 48 } },
          { binding: 1, resource: { buffer: buf } }
        ].concat(scratch ? [{ binding: 2, resource: { buffer: scratch } }] : [])
         .concat(shared ? [{ binding: 3, resource: { buffer: shared } }] : [])
      });
      entry.cap = steps;
      entry.uarr = null;
    }

    // MIXED f32/u32, so one typed array will not do: count and seed are
    // integers in the struct. seed especially — as an f32 converted with
    // u32() it aliased every value past 2^24 onto its neighbours, so
    // "a different seed" quietly meant "the same noise".
    if (!entry.uarr) {
      entry.uarr = new ArrayBuffer(entry.ustride * entry.cap);
      entry.uf32 = new Float32Array(entry.uarr);
      entry.uu32 = new Uint32Array(entry.uarr);
    }
    // Every substep's header, in ONE write. They differ only in `step`,
    // which the preamble hands to rng_init as the stream index — without
    // it, N substeps would replay the identical draws N times.
    var slot = entry.ustride >> 2;   // stride in 32-bit words
    for (var k = 0; k < steps; k++) {
      var o = k * slot;
      entry.uf32[o + 0] = time;
      entry.uu32[o + 1] = count >>> 0;
      entry.uu32[o + 2] = seed >>> 0;
      entry.uu32[o + 3] = k >>> 0;
      // Parameters are optional; absent means the slots stay zero. The
      // caller owns the block and rewrites it in place, so this is a copy
      // of at most 32 bytes per substep and never an allocation.
      for (var pi = 0; pi < 8; pi++) {
        entry.uf32[o + 4 + pi] = (paramsPtr && pi * 4 < paramsLen)
          ? HEAPF32[(paramsPtr >> 2) + pi]
          : 0.0;
      }
    }
    device.queue.writeBuffer(entry.ubuf, 0, entry.uf32, 0, steps * slot);

    var encoder = device.createCommandEncoder();
    var pass = encoder.beginComputePass();
    pass.setPipeline(entry.pipeline);
    // N substeps in ONE pass, ONE encoder, ONE submit. WebGPU tracks the
    // read-write hazard on the storage buffer itself, so dispatch k+1 sees
    // what dispatch k wrote without any explicit barrier — there are no
    // manual barriers in the API at all.
    //
    // Doing this from Scheme instead is not merely slower, it is not the
    // same thing: the loop would have to yield between dispatches, so N
    // steps would cost N frames rather than one.
    //
    // Workgroup size is 64 in the shader; round up so the tail is covered,
    // and the shader early-returns for indices past the count.
    var groups = Math.ceil(count / 64);
    for (var k = 0; k < steps; k++) {
      pass.setBindGroup(0, entry.bind, [k * entry.ustride]);
      pass.dispatchWorkgroups(groups);
    }
    pass.end();
    device.queue.submit([encoder.finish()]);
    return 0;
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -3;
  }
});

// Draw straight from a GPU-resident buffer. Same shader and same pipeline
// shape as js_gpu_draw_instances, minus the per-frame upload.
EM_JS(int, js_gpu_draw_buffer, (int deviceId, int bufId, int shaderId, const char *canvasIdPtr, int instances, double time, double yaw, double pitch, double dist, double fov), {
  var canvasId = UTF8ToString(canvasIdPtr);
  globalThis.vxsGpuError = "";
  try {
    var shader = globalThis.vxsHandles ? globalThis.vxsHandles.get(shaderId) : null;
    if (!shader) { globalThis.vxsGpuError = "shader handle is not live"; return -6; }
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var buf = globalThis.vxsHandles.get(bufId);
    if (!buf) { globalThis.vxsGpuError = "point buffer handle is not live"; return -2; }
    var canvas = document.getElementById(canvasId);
    if (!canvas) { globalThis.vxsGpuError = "no canvas with id " + canvasId; return -3; }
    var ctx = canvas.getContext('webgpu');
    if (!ctx) { globalThis.vxsGpuError = "getContext('webgpu') returned null"; return -4; }

    globalThis.vxsBufDrawCache = globalThis.vxsBufDrawCache || {};
    var key = canvasId + " " + bufId + " " + shaderId;
    var entry = globalThis.vxsBufDrawCache[key];
    if (!entry || entry.device !== device) {
      var format = navigator.gpu.getPreferredCanvasFormat();
      ctx.configure({ device: device, format: format, alphaMode: 'opaque' });
      var module = shader.module;
      var bgl = device.createBindGroupLayout({
        entries: [
          { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform' } },
          { binding: 1, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'read-only-storage' } }
        ]
      });
      var pipeline = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [bgl] }),
        vertex:   { module: module, entryPoint: 'vs' },
        fragment: { module: module, entryPoint: 'fs', targets: [{
          format: format,
          blend: {
            color: { srcFactor: 'src-alpha', dstFactor: 'one', operation: 'add' },
            alpha: { srcFactor: 'zero', dstFactor: 'one', operation: 'add' }
          }
        }] },
        primitive: { topology: 'triangle-list' }
      });
      var ubuf = device.createBuffer({
        size: 32,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
      });
      var bind = device.createBindGroup({
        layout: bgl,
        entries: [
          { binding: 0, resource: { buffer: ubuf } },
          { binding: 1, resource: { buffer: buf } }
        ]
      });
      entry = { device: device, pipeline: pipeline, ubuf: ubuf, bind: bind };
      globalThis.vxsBufDrawCache[key] = entry;
    }

    device.queue.writeBuffer(entry.ubuf, 0,
      new Float32Array([time, canvas.width, canvas.height, instances,
                        yaw, pitch, dist, fov]));

    var encoder = device.createCommandEncoder();
    var pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.0, g: 0.0, b: 0.0, a: 1.0 },
        loadOp: 'clear',
        storeOp: 'store'
      }]
    });
    pass.setPipeline(entry.pipeline);
    pass.setBindGroup(0, entry.bind);
    pass.draw(6, instances);
    pass.end();
    device.queue.submit([encoder.finish()]);
    return 0;
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -5;
  }
});

// Push host bytes into a GPU-resident buffer.
//
// gpu-buffer uploads once, which is right when a compute pass is the
// producer. When the HOST is the producer — actors writing their own
// points — the buffer has to be refreshed each frame, and this is that
// refresh. The alternative, a draw call that takes bytes and uploads them
// itself, would have tied the upload to the draw and made it impossible to
// write once and draw twice.
EM_JS(int, js_gpu_buffer_write, (int deviceId, int bufId, const unsigned char *dataPtr, int dataLen), {
  globalThis.vxsGpuError = "";
  try {
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var buf = globalThis.vxsHandles.get(bufId);
    if (!buf) { globalThis.vxsGpuError = "buffer handle is not live"; return -2; }
    if (dataLen > 0) {
      // Copy out of the wasm heap: writeBuffer on a view INTO it would be
      // reading memory the VM may move or reuse before the copy happens.
      device.queue.writeBuffer(buf, 0,
        new Uint8Array(HEAPU8.subarray(dataPtr, dataPtr + dataLen)));
    }
    return 0;
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -3;
  }
});

// Instanced GEOMETRY, as opposed to instanced sprites.
//
// Separate from js_gpu_draw_buffer rather than a mode flag on it, because
// almost every piece of pipeline state differs: depth testing instead of
// additive blending, a depth attachment, no blend state, and a vertex
// count that comes from the caller. Sharing them behind a boolean would
// have made both harder to read for no saving.
//
// The depth texture is cached with the pipeline and rebuilt when the
// canvas resizes — a stale one would silently reject every fragment once
// the window grew.
EM_JS(int, js_gpu_draw_geometry, (int deviceId, int bufId, int shaderId, const char *canvasIdPtr, int vertsPerInstance, int instances, double time, double yaw, double pitch, double dist, double fov), {
  var canvasId = UTF8ToString(canvasIdPtr);
  globalThis.vxsGpuError = "";
  try {
    var shader = globalThis.vxsHandles ? globalThis.vxsHandles.get(shaderId) : null;
    if (!shader) { globalThis.vxsGpuError = "shader handle is not live"; return -6; }
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    if (!device) { globalThis.vxsGpuError = "device handle is not live"; return -1; }
    var buf = globalThis.vxsHandles.get(bufId);
    if (!buf) { globalThis.vxsGpuError = "point buffer handle is not live"; return -2; }
    var canvas = document.getElementById(canvasId);
    if (!canvas) { globalThis.vxsGpuError = "no canvas with id " + canvasId; return -3; }
    var ctx = canvas.getContext('webgpu');
    if (!ctx) { globalThis.vxsGpuError = "getContext('webgpu') returned null"; return -4; }

    globalThis.vxsGeomCache = globalThis.vxsGeomCache || {};
    var key = canvasId + " " + bufId + " " + shaderId;
    var entry = globalThis.vxsGeomCache[key];
    if (!entry || entry.device !== device) {
      var format = navigator.gpu.getPreferredCanvasFormat();
      ctx.configure({ device: device, format: format, alphaMode: 'opaque' });
      var module = shader.module;
      var bgl = device.createBindGroupLayout({
        entries: [
          { binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'uniform' } },
          { binding: 1, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT,
            buffer: { type: 'read-only-storage' } }
        ]
      });
      var pipeline = device.createRenderPipeline({
        layout: device.createPipelineLayout({ bindGroupLayouts: [bgl] }),
        vertex:   { module: module, entryPoint: 'vs' },
        // No blending: solid geometry occludes rather than accumulating.
        fragment: { module: module, entryPoint: 'fs', targets: [{ format: format }] },
        // cullMode 'none' deliberately. Back faces cost fragments we do not
        // need, but a winding mistake with culling on makes cubes vanish
        // instead of merely being slower — the wrong failure to risk first.
        primitive: { topology: 'triangle-list', cullMode: 'none' },
        depthStencil: {
          format: 'depth24plus',
          depthWriteEnabled: true,
          depthCompare: 'less'
        }
      });
      var ubuf = device.createBuffer({
        size: 32,
        usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST
      });
      var bind = device.createBindGroup({
        layout: bgl,
        entries: [
          { binding: 0, resource: { buffer: ubuf } },
          { binding: 1, resource: { buffer: buf } }
        ]
      });
      entry = { device: device, pipeline: pipeline, ubuf: ubuf, bind: bind,
                depth: null, depthW: 0, depthH: 0 };
      globalThis.vxsGeomCache[key] = entry;
    }

    if (!entry.depth || entry.depthW !== canvas.width || entry.depthH !== canvas.height) {
      if (entry.depth) entry.depth.destroy();
      entry.depth = device.createTexture({
        size: [canvas.width, canvas.height],
        format: 'depth24plus',
        usage: GPUTextureUsage.RENDER_ATTACHMENT
      });
      entry.depthW = canvas.width;
      entry.depthH = canvas.height;
    }

    device.queue.writeBuffer(entry.ubuf, 0,
      new Float32Array([time, canvas.width, canvas.height, instances,
                        yaw, pitch, dist, fov]));

    var encoder = device.createCommandEncoder();
    var pass = encoder.beginRenderPass({
      colorAttachments: [{
        view: ctx.getCurrentTexture().createView(),
        clearValue: { r: 0.02, g: 0.025, b: 0.04, a: 1.0 },
        loadOp: 'clear',
        storeOp: 'store'
      }],
      depthStencilAttachment: {
        view: entry.depth.createView(),
        depthClearValue: 1.0,
        depthLoadOp: 'clear',
        depthStoreOp: 'store'
      }
    });
    pass.setPipeline(entry.pipeline);
    pass.setBindGroup(0, entry.bind);
    pass.draw(vertsPerInstance, instances);
    pass.end();
    device.queue.submit([encoder.finish()]);
    return 0;
  } catch (e) {
    globalThis.vxsGpuError = String(e && e.message ? e.message : e);
    return -5;
  }
});

// Bring a GPU buffer home.
//
// Everything else in this file is a write or a draw: data went out and
// never came back. That was tolerable while the buffer was an OUTPUT — you
// look at the picture and the picture is the answer — and it stops being
// tolerable the moment the buffer is STATE, because then the picture is a
// claim and there is no way to check it. Sixty thousand elements doing
// something plausible, and not one number computable about them: no mean,
// no spread, no count. The eye says it looks right and nothing can
// disagree.
//
// Which is the failure this system is otherwise good at avoiding. Every
// good decision here came from being able to count something — allocation
// counters, the scene rate, known-answer vectors. On the GPU side there
// was nothing to count with.
//
// The standard staging dance: copy into a MAP_READ buffer, map it, copy
// out. No new async machinery is needed, because a promise is already a
// future here — the fiber blocks on touch, the copy lands, the fiber
// resumes. And every buffer gpu-buffer makes already carries COPY_SRC, so
// readback attaches to an existing buffer without recreating it or
// rebuilding a bind group.
//
// THE SNAPSHOT IS LAGGED, unavoidably: mapAsync settles a frame or two
// after the copy is submitted, so what returns is the most recent
// completed state and never the current one. Invisible when you are
// looking at it; a correctness trap for anything that feeds a result back
// in. Hence the frame stamp travelling with the bytes — it turns "why is
// this unstable" into "I am reacting to two-frame-old data".
EM_JS(void, js_gpu_buffer_read, (int token, int deviceId, int bufId, int byteLen, int frame), {
  var fail = function(msg) {
    Module.ccall('vxs_settle_error', 'number', ['number', 'string'], [token, msg]);
  };
  try {
    var device = globalThis.vxsHandles ? globalThis.vxsHandles.get(deviceId) : null;
    var buf = globalThis.vxsHandles ? globalThis.vxsHandles.get(bufId) : null;
    if (!device) { fail("gpu-buffer-read: device handle is not live"); return; }
    if (!buf) { fail("gpu-buffer-read: buffer handle is not live"); return; }

    // copyBufferToBuffer requires a multiple of 4, and cannot exceed the
    // source.
    var want = byteLen > 0 ? byteLen : buf.size;
    if (want > buf.size) want = buf.size;
    want = want & ~3;
    if (want <= 0) { fail("gpu-buffer-read: nothing to read"); return; }

    var staging = device.createBuffer({
      size: want,
      usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST
    });
    var enc = device.createCommandEncoder();
    enc.copyBufferToBuffer(buf, 0, staging, 0, want);
    device.queue.submit([enc.finish()]);

    staging.mapAsync(GPUMapMode.READ).then(function() {
      try {
        var src = new Uint8Array(staging.getMappedRange());
        // Bare, not Module-qualified: inside EM_JS these are globals in
        // the generated scope, the way js_gpu_buffer_write and
        // js_gpu_last_error already use them.
        var ptr = _malloc(src.length);
        HEAPU8.set(src, ptr);
        Module.ccall('vxs_settle_bytes', 'number',
                     ['number', 'number', 'number', 'number'],
                     [token, ptr, src.length, frame]);
        _free(ptr);
        staging.unmap();
        staging.destroy();
      } catch (inner) {
        fail("gpu-buffer-read: settling failed: " + inner);
      }
    }, function(e) {
      fail("gpu-buffer-read: mapAsync rejected: " + e);
    });
  } catch (e) {
    fail("gpu-buffer-read: " + e);
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
static int js_gpu_draw(int, int, const char *) { return -1; }
static int js_gpu_run_kernel(int, int, const char *, double) { return -1; }
static int js_gpu_draw_instances(int, int, const char *, const unsigned char *, int, int, double, double, double, double, double) { return -1; }
static int js_gpu_create_buffer(int, const unsigned char *, int) { return -1; }
static int js_gpu_wrangle(int, int, int, int, double, int, const unsigned char *, int, int, int, int) { return -1; }
static int js_gpu_draw_buffer(int, int, int, const char *, int, double, double, double, double, double) { return -1; }
static int js_gpu_draw_geometry(int, int, int, const char *, int, int, double, double, double, double, double) { return -1; }
static int js_gpu_buffer_write(int, int, const unsigned char *, int) { return -1; }
static void js_gpu_buffer_read(int, int, int, int, int) {}
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
  // (gpu-compile device wgsl-source) -> future of a shader handle
  //
  // Every draw path takes the HANDLE, never the source, so there is no way
  // to reach a pipeline without having waited for the compile to succeed.
  // Touching the future of a bad shader raises with the line, the column
  // and the message, at the point in the program where the shader was
  // named — which is where someone can do something about it.
  //
  // Compiling is also the natural place for this to be a future: it is the
  // one genuinely asynchronous step (getCompilationInfo resolves on a later
  // turn), it happens once per shader rather than once per frame, and the
  // handle it yields is the thing the pipeline cache should have been keyed
  // by all along.
  vm.def_global("gpu-compile", vm.heap.make_subr("gpu-compile", [](VM &vm, uint32_t argc, Value *args) -> Value {
    (void)argc;
    if (!Heap::is_handle(args[0])) {
      vm.raise_contract("gpu-compile: expected a device handle, got " +
                        vm.format_value(args[0]));
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-compile: device handle was released");
    if (!Heap::is_string(args[1])) {
      vm.raise_contract("gpu-compile: expected WGSL source as a string, got " +
                        vm.format_value(args[1]));
    }
    std::string wgsl(args[1].as_ptr<ObjString>()->view());
    Value fut = vm.heap.make_external_future();
    vm.push_temp_root(&fut);
    uint32_t token = vm.register_external(fut);
    vm.pop_temp_root();
    js_gpu_compile(static_cast<int>(token), static_cast<int>(d->id), wgsl.c_str());
    return fut;
  }, 2, 2));

  vm.def_global("gpu-draw-triangle!", vm.heap.make_subr("gpu-draw-triangle!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) {
      vm.raise_contract("gpu-draw-triangle!: expected a device handle, got " +
                        vm.format_value(args[0]));
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-draw-triangle!: device handle was released");
    if (!Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-draw-triangle!: expected a shader handle from gpu-compile, got " +
                        vm.format_value(args[1]));
    }
    ObjHandle *sh = args[1].as_ptr<ObjHandle>();
    if (sh->released) vm.raise_contract("gpu-draw-triangle!: shader handle was released");
    std::string canvas_id = (argc > 2 && Heap::is_string(args[2]))
        ? std::string(args[2].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_draw(static_cast<int>(d->id), static_cast<int>(sh->id), canvas_id.c_str());
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-draw-triangle!: " + detail);
    }
    return Value::boolean_true();
  }, 2, 3));

  // (gpu-run-kernel! device wgsl time [canvas-id]) - draw one frame of a
  // fullscreen kernel shader. `time` reaches the shader as a uniform, so
  // animating costs a 16-byte buffer write per frame rather than a shader
  // recompile. See lib/shadertoy.scm for the source this expects.
  vm.def_global("gpu-run-kernel!", vm.heap.make_subr("gpu-run-kernel!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) {
      vm.raise_contract("gpu-run-kernel!: expected a device handle, got " +
                        vm.format_value(args[0]));
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-run-kernel!: device handle was released");
    if (!Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-run-kernel!: expected a shader handle from gpu-compile, got " +
                        vm.format_value(args[1]));
    }
    ObjHandle *sh = args[1].as_ptr<ObjHandle>();
    if (sh->released) vm.raise_contract("gpu-run-kernel!: shader handle was released");
    if (!args[2].is_int() && !args[2].is_double()) {
      vm.raise_contract("gpu-run-kernel!: expected a number for time, got " +
                        vm.format_value(args[2]));
    }
    double t = args[2].is_int() ? static_cast<double>(args[2].as_int())
                                : args[2].as_double();
    std::string canvas_id = (argc > 3 && Heap::is_string(args[3]))
        ? std::string(args[3].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_run_kernel(static_cast<int>(d->id), static_cast<int>(sh->id), canvas_id.c_str(), t);
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-run-kernel!: " + detail);
    }
    return Value::boolean_true();
  }, 3, 4));

  // (gpu-draw-instances! device wgsl bytes count time camera [canvas-id])
  //
  // `camera` is a 4-vector: yaw, pitch, distance, fov. Grouped into one
  // argument rather than spread across four so that adding a fifth later
  // does not change the arity of every call site.
  //
  // Uploads `bytes` as a storage buffer and draws `count` instances from
  // it. The whole buffer is re-uploaded per frame rather than mutated in
  // place on the GPU: at a few thousand points that is tens of kilobytes,
  // far cheaper than the draw itself, and it keeps the host copy the
  // single source of truth. When a compute pass starts producing the point
  // data instead, this upload is exactly what disappears.
  vm.def_global("gpu-draw-instances!", vm.heap.make_subr("gpu-draw-instances!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) {
      vm.raise_contract("gpu-draw-instances!: expected a device handle, got " +
                        vm.format_value(args[0]));
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-draw-instances!: device handle was released");
    if (!Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-draw-instances!: expected a shader handle from gpu-compile, got " +
                        vm.format_value(args[1]));
    }
    ObjHandle *sh = args[1].as_ptr<ObjHandle>();
    if (sh->released) vm.raise_contract("gpu-draw-instances!: shader handle was released");
    ObjBytes *b = vm.require_bytes(args[2], "gpu-draw-instances!");
    if (!b) return Value::boolean_false();
    if (!args[3].is_int()) {
      vm.raise_contract("gpu-draw-instances!: expected an integer instance count, got " +
                        vm.format_value(args[3]));
    }
    int32_t instances = args[3].as_int();
    if (instances < 0) {
      vm.raise_contract("gpu-draw-instances!: instance count cannot be negative");
    }
    if (!args[4].is_int() && !args[4].is_double()) {
      vm.raise_contract("gpu-draw-instances!: expected a number for time, got " +
                        vm.format_value(args[4]));
    }
    double t = args[4].is_int() ? static_cast<double>(args[4].as_int())
                                : args[4].as_double();
    if (!Heap::is_vector(args[5]) ||
        args[5].as_ptr<ObjVector>()->size < 4) {
      vm.raise_contract("gpu-draw-instances!: expected a 4-element camera "
                        "vector (yaw pitch distance fov), got " +
                        vm.format_value(args[5]));
    }
    ObjVector *cam = args[5].as_ptr<ObjVector>();
    double camv[4];
    for (int i = 0; i < 4; ++i) {
      Value cv = cam->get(static_cast<uint32_t>(i));
      if (!cv.is_int() && !cv.is_double()) {
        vm.raise_contract("gpu-draw-instances!: camera components must be "
                          "numbers, got " + vm.format_value(cv));
      }
      camv[i] = cv.is_int() ? static_cast<double>(cv.as_int()) : cv.as_double();
    }
    std::string canvas_id = (argc > 6 && Heap::is_string(args[6]))
        ? std::string(args[6].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_draw_instances(static_cast<int>(d->id), static_cast<int>(sh->id),
                                   canvas_id.c_str(), b->data.data(),
                                   static_cast<int>(b->data.size()),
                                   instances, t,
                                   camv[0], camv[1], camv[2], camv[3]);
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-draw-instances!: " + detail);
    }
    return Value::boolean_true();
  }, 6, 7));

  // (gpu-buffer device bytes) -> handle
  //
  // Uploads once and keeps the data on the GPU. Everything else in this
  // file re-uploads per frame, which is correct while the HOST is the
  // producer; a compute wrangle is the producer instead, so re-uploading
  // would both waste the transfer and discard what the last dispatch
  // computed.
  vm.def_global("gpu-buffer", vm.heap.make_subr("gpu-buffer", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) {
      vm.raise_contract("gpu-buffer: expected a device handle, got " +
                        vm.format_value(args[0]));
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-buffer: device handle was released");
    ObjBytes *b = vm.require_bytes(args[1], "gpu-buffer");
    if (!b) return Value::boolean_false();
    int id = js_gpu_create_buffer(static_cast<int>(d->id), b->data.data(),
                                  static_cast<int>(b->data.size()));
    if (id < 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-buffer: " + detail);
    }
    return vm.heap.make_handle(static_cast<uint32_t>(id), vm.intern("gpu-buffer"));
  }, 2, 2));

  // (gpu-wrangle! device buffer wgsl count time [seed]) -> #t
  // One compute dispatch over the buffer, in place.
  vm.def_global("gpu-wrangle!", vm.heap.make_subr("gpu-wrangle!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0]) || !Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-wrangle!: expected (device buffer wgsl count time)");
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    ObjHandle *b = args[1].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-wrangle!: device handle was released");
    if (b->released) vm.raise_contract("gpu-wrangle!: buffer handle was released");
    if (!Heap::is_handle(args[2])) {
      vm.raise_contract("gpu-wrangle!: expected a shader handle from gpu-compile, got " +
                        vm.format_value(args[2]));
    }
    ObjHandle *sh = args[2].as_ptr<ObjHandle>();
    if (sh->released) vm.raise_contract("gpu-wrangle!: shader handle was released");
    if (!args[3].is_int()) {
      vm.raise_contract("gpu-wrangle!: expected an integer point count, got " +
                        vm.format_value(args[3]));
    }
    double t = args[4].is_int() ? static_cast<double>(args[4].as_int())
                                : args[4].as_double();
    // The seed is an INTEGER all the way down now. It used to be carried
    // as a double and converted in the shader with u32(), which aliases
    // every value past 2^24 onto its neighbours — a counter-based RNG
    // addressed by a float is not addressed at all. A real given as a seed
    // is truncated rather than refused, so existing callers keep working.
    int seed = 0;
    if (argc > 5) {
      seed = args[5].is_int() ? args[5].as_int()
                              : static_cast<int>(args[5].as_double());
    }
    // Optional parameter block: eight floats the caller owns and rewrites
    // in place between frames, so a live knob costs a 32-byte copy rather
    // than a shader recompile. Absent means the slots read zero.
    const unsigned char *params = nullptr;
    int params_len = 0;
    if (argc > 6 && !args[6].is_false()) {
      ObjBytes *pb = vm.require_bytes(args[6], "gpu-wrangle!");
      params = pb->data.data();
      params_len = static_cast<int>(pb->data.size());
      if (params_len > 32) params_len = 32;
    }
    // Substeps: run the kernel N times inside one encoder and one submit.
    // Looping in Scheme instead yields between dispatches, so N steps cost
    // N frames — which is why this cannot be done from the caller's side.
    int steps = 1;
    if (argc > 7) {
      if (!args[7].is_int() || args[7].as_int() < 1) {
        vm.raise_contract("gpu-wrangle!: expected a positive substep count, got " +
                          vm.format_value(args[7]));
      }
      steps = args[7].as_int();
    }
    // Optional scratch buffer, bound at 2. Present only when the kernel
    // declares attributes: a shader that never mentions binding 2 must
    // keep the two-entry layout it has always had.
    int scratch_id = 0;
    if (argc > 8 && !args[8].is_false()) {
      if (!Heap::is_handle(args[8])) {
        vm.raise_contract("gpu-wrangle!: expected a scratch buffer handle, got " +
                          vm.format_value(args[8]));
      }
      ObjHandle *sc = args[8].as_ptr<ObjHandle>();
      if (sc->released) vm.raise_contract("gpu-wrangle!: scratch handle was released");
      scratch_id = static_cast<int>(sc->id);
    }
    // Optional shared read-only buffer, bound at 3.
    int shared_id = 0;
    if (argc > 9 && !args[9].is_false()) {
      if (!Heap::is_handle(args[9])) {
        vm.raise_contract("gpu-wrangle!: expected a shared buffer handle, got " +
                          vm.format_value(args[9]));
      }
      ObjHandle *sd = args[9].as_ptr<ObjHandle>();
      if (sd->released) vm.raise_contract("gpu-wrangle!: shared handle was released");
      shared_id = static_cast<int>(sd->id);
    }
    int rc = js_gpu_wrangle(static_cast<int>(d->id), static_cast<int>(b->id),
                            static_cast<int>(sh->id), args[3].as_int(), t, seed,
                            params, params_len, steps, scratch_id, shared_id);
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-wrangle!: " + detail);
    }
    return Value::boolean_true();
  }, 5, 10));

  // (gpu-draw-buffer! device buffer wgsl count time camera [canvas-id]) -> #t
  vm.def_global("gpu-draw-buffer!", vm.heap.make_subr("gpu-draw-buffer!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0]) || !Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-draw-buffer!: expected (device buffer wgsl count time camera)");
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    ObjHandle *b = args[1].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-draw-buffer!: device handle was released");
    if (b->released) vm.raise_contract("gpu-draw-buffer!: buffer handle was released");
    if (!Heap::is_handle(args[2])) {
      vm.raise_contract("gpu-draw-buffer!: expected a shader handle from gpu-compile, got " +
                        vm.format_value(args[2]));
    }
    ObjHandle *sh = args[2].as_ptr<ObjHandle>();
    if (sh->released) vm.raise_contract("gpu-draw-buffer!: shader handle was released");
    if (!args[3].is_int()) {
      vm.raise_contract("gpu-draw-buffer!: expected an integer instance count, got " +
                        vm.format_value(args[3]));
    }
    double t = args[4].is_int() ? static_cast<double>(args[4].as_int())
                                : args[4].as_double();
    if (!Heap::is_vector(args[5]) || args[5].as_ptr<ObjVector>()->size < 4) {
      vm.raise_contract("gpu-draw-buffer!: expected a 4-element camera vector "
                        "(yaw pitch distance fov), got " + vm.format_value(args[5]));
    }
    ObjVector *cam = args[5].as_ptr<ObjVector>();
    double camv[4];
    for (int i = 0; i < 4; ++i) {
      Value cv = cam->get(static_cast<uint32_t>(i));
      if (!cv.is_int() && !cv.is_double()) {
        vm.raise_contract("gpu-draw-buffer!: camera components must be numbers, got " +
                          vm.format_value(cv));
      }
      camv[i] = cv.is_int() ? static_cast<double>(cv.as_int()) : cv.as_double();
    }
    std::string canvas_id = (argc > 6 && Heap::is_string(args[6]))
        ? std::string(args[6].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_draw_buffer(static_cast<int>(d->id), static_cast<int>(b->id),
                                static_cast<int>(sh->id), canvas_id.c_str(), args[3].as_int(),
                                t, camv[0], camv[1], camv[2], camv[3]);
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-draw-buffer!: " + detail);
    }
    return Value::boolean_true();
  }, 6, 7));

  // (gpu-buffer-write! device buffer bytes) -> #t
  // Refresh a GPU-resident buffer from the host copy.
  vm.def_global("gpu-buffer-write!", vm.heap.make_subr("gpu-buffer-write!", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_handle(args[0]) || !Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-buffer-write!: expected (device buffer bytes)");
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    ObjHandle *b = args[1].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-buffer-write!: device handle was released");
    if (b->released) vm.raise_contract("gpu-buffer-write!: buffer handle was released");
    ObjBytes *bytes = vm.require_bytes(args[2], "gpu-buffer-write!");
    if (!bytes) return Value::boolean_false();
    int rc = js_gpu_buffer_write(static_cast<int>(d->id), static_cast<int>(b->id),
                                 bytes->data.data(),
                                 static_cast<int>(bytes->data.size()));
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-buffer-write!: " + detail);
    }
    return Value::boolean_true();
  }, 3, 3));

  // (gpu-draw-geometry! device buffer wgsl verts-per-instance count time
  //                      camera [canvas-id]) -> #t
  //
  // Instanced geometry: depth-tested and opaque, where gpu-draw-buffer! is
  // additive and depth-free. Sprites accumulate and order does not matter;
  // solid geometry occludes, and occlusion is order-dependent.
  vm.def_global("gpu-draw-geometry!", vm.heap.make_subr("gpu-draw-geometry!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0]) || !Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-draw-geometry!: expected (device buffer wgsl verts count time camera)");
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    ObjHandle *b = args[1].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-draw-geometry!: device handle was released");
    if (b->released) vm.raise_contract("gpu-draw-geometry!: buffer handle was released");
    if (!Heap::is_handle(args[2])) {
      vm.raise_contract("gpu-draw-geometry!: expected a shader handle from gpu-compile, got " +
                        vm.format_value(args[2]));
    }
    ObjHandle *sh = args[2].as_ptr<ObjHandle>();
    if (sh->released) vm.raise_contract("gpu-draw-geometry!: shader handle was released");
    if (!args[3].is_int() || args[3].as_int() <= 0) {
      vm.raise_contract("gpu-draw-geometry!: expected a positive vertex count, got " +
                        vm.format_value(args[3]));
    }
    if (!args[4].is_int() || args[4].as_int() < 0) {
      vm.raise_contract("gpu-draw-geometry!: expected a non-negative instance count, got " +
                        vm.format_value(args[4]));
    }
    double t = args[5].is_int() ? static_cast<double>(args[5].as_int())
                                : args[5].as_double();
    if (!Heap::is_vector(args[6]) || args[6].as_ptr<ObjVector>()->size < 4) {
      vm.raise_contract("gpu-draw-geometry!: expected a 4-element camera vector "
                        "(yaw pitch distance fov), got " + vm.format_value(args[6]));
    }
    ObjVector *cam = args[6].as_ptr<ObjVector>();
    double camv[4];
    for (int i = 0; i < 4; ++i) {
      Value cv = cam->get(static_cast<uint32_t>(i));
      if (!cv.is_int() && !cv.is_double()) {
        vm.raise_contract("gpu-draw-geometry!: camera components must be numbers, got " +
                          vm.format_value(cv));
      }
      camv[i] = cv.is_int() ? static_cast<double>(cv.as_int()) : cv.as_double();
    }
    std::string canvas_id = (argc > 7 && Heap::is_string(args[7]))
        ? std::string(args[7].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_draw_geometry(static_cast<int>(d->id), static_cast<int>(b->id),
                                  static_cast<int>(sh->id), canvas_id.c_str(),
                                  args[3].as_int(), args[4].as_int(), t,
                                  camv[0], camv[1], camv[2], camv[3]);
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-draw-geometry!: " + detail);
    }
    return Value::boolean_true();
  }, 7, 8));

  // (gpu-buffer-read device buffer [byte-length]) -> future
  //
  // Settles with (frame . bytes): the frame the copy was submitted on, and
  // the data. Read back the whole buffer by omitting the length, or narrow
  // it — a full readback of sixty thousand seven-float elements is 1.7MB a
  // probe, and most questions want a summary a kernel could have reduced
  // to a few hundred numbers first.
  vm.def_global("gpu-buffer-read", vm.heap.make_subr("gpu-buffer-read", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_handle(args[0]) || !Heap::is_handle(args[1])) {
      vm.raise_contract("gpu-buffer-read: expected (device buffer [byte-length])");
    }
    ObjHandle *d = args[0].as_ptr<ObjHandle>();
    ObjHandle *b = args[1].as_ptr<ObjHandle>();
    if (d->released) vm.raise_contract("gpu-buffer-read: device handle was released");
    if (b->released) vm.raise_contract("gpu-buffer-read: buffer handle was released");
    int want = 0;   // 0 means the whole buffer
    if (argc > 2) {
      if (!args[2].is_int() || args[2].as_int() < 0) {
        vm.raise_contract("gpu-buffer-read: expected a non-negative byte length, got " +
                          vm.format_value(args[2]));
      }
      want = args[2].as_int();
    }
    Value fut = vm.heap.make_external_future();
    vm.push_temp_root(&fut);
    uint32_t token = vm.register_external(fut);
    vm.pop_temp_root();
    js_gpu_buffer_read(static_cast<int>(token), static_cast<int>(d->id),
                       static_cast<int>(b->id), want,
                       static_cast<int>(g_step_calls));
    return fut;
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

  auto subr_mouse_wheel = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_double(js_canvas_mouse_wheel());
  };
  vm.def_global("mouse-wheel", vm.heap.make_subr("mouse-wheel", subr_mouse_wheel, 0, 0));

  auto subr_paused = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_bool(js_paused() != 0);
  };
  vm.def_global("paused?", vm.heap.make_subr("paused?", subr_paused, 0, 0));

  auto subr_now = [](VM &, uint32_t, Value *) -> Value {
    return Value::from_double(js_now());
  };
  vm.def_global("current-time", vm.heap.make_subr("current-time", subr_now, 0, 0));

  // `display` and `newline` are deliberately NOT overridden here. They used
  // to be — each built a string and shoved it at js_print_output — which
  // meant they ignored their port argument entirely, so (display x port)
  // wrote to the terminal AND printed the port object, and string ports
  // never worked in the browser at all. The problem was being fixed one
  // layer too high: the core's display already resolves an explicit port
  // and falls back to current-output-port. All the browser actually needs
  // is for the DEFAULT port to lead somewhere, which vxs_init now arranges
  // by making stdout a sink port. Everything else follows.

  // A port writing to a named JS sink. Pages register handlers on
  // globalThis.vxsSinks, so appending to a div — or anywhere else — needs
  // no new primitive here.
  auto subr_open_sink = [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::string name = "terminal";
    if (argc > 0 && Heap::is_string(args[0])) {
      name = std::string(args[0].as_ptr<ObjString>()->view());
    }
    return make_sink_port(vm, name);
  };
  vm.def_global("open-output-sink",
                vm.heap.make_subr("open-output-sink", subr_open_sink, 0, 1));

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
static size_t g_preempt_total = 0;

extern "C" {

EMSCRIPTEN_KEEPALIVE int vxs_init();   // defined just below

// Watch mode support.
//
// lib/*.scm is compiled into the binary, which is right for shipping and
// wrong while editing: a change to a library is invisible in the browser
// until a rebuild. These two let the page serve the libraries over HTTP
// instead — vxs_lib_names says which ones exist, and vxs_register_lib
// supplies fresher source for one. `load` prefers a registered override
// over the baked-in copy, so saving in an editor is enough.
EMSCRIPTEN_KEEPALIVE
const char *vxs_lib_names() {
  static std::string names;
  names.clear();
  for (int i = 0; i < VX_EMBEDDED_LIB_COUNT; ++i) {
    if (i) names += ",";
    names += VX_EMBEDDED_LIBS[i].name;
  }
  return names.c_str();
}

EMSCRIPTEN_KEEPALIVE
void vxs_register_lib(const char *name, const char *source) {
  if (!g_vm) vxs_init();
  if (!name || !source) return;
  g_vm->lib_overrides[std::string(name)] = std::string(source);
}

EMSCRIPTEN_KEEPALIVE
int vxs_init() {
  g_vm = std::make_unique<VM>();

  // Point stdout at the page BEFORE anything can print. std::cout goes
  // nowhere in a browser, so the default output port has to be a sink
  // port; with that in place the ordinary port machinery does the rest,
  // and no output procedure needs overriding.
  g_terminal_port = make_sink_port(*g_vm, "terminal");
  g_console_port = make_sink_port(*g_vm, "console");
  g_vm->stdout_port = g_terminal_port;
  g_vm->current_out_port = g_terminal_port;
  // Bound as globals so the GC keeps them, and so Scheme can name them:
  //   (display "to the js console" console-port)
  g_vm->def_global("terminal-port", g_terminal_port);
  g_vm->def_global("console-port", g_console_port);

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

// Compile and run top-level forms ONE AT A TIME, in order — the way
// `load` already does natively, and the way Scheme top level is supposed
// to work.
//
// The browser used to read_all_forms() and compile the whole submission
// before running any of it. That breaks anything whose EFFECT must land
// before the next form is COMPILED, and macros are exactly that:
//
//   (load "lib/gpu.scm")        ; defines the define-kernel macro
//   (define-kernel plasma ...)  ; needs it registered to compile
//
// The load ran at runtime, long after define-kernel had been compiled as
// an unknown operator, so this failed in the browser while working
// natively. Two paths that should agree and didn't.
//
// The deadline is shared across all forms rather than restarted per form,
// so a submission cannot buy extra time by being split up.
struct SeqOutcome {
  VM::StepResult res = VM::StepResult::Completed;
  Value result = Value::unspecified();
  std::string error;
};

static SeqOutcome eval_forms_sequentially(
    const char *code, std::chrono::steady_clock::time_point deadline) {
  SeqOutcome out;
  Reader reader(*g_vm, code);
  while (true) {
    Value form = reader.read_form();
    if (form.is_eof()) break;
    Compiler compiler(*g_vm);
    ObjClosure *closure = compiler.compile_top_level(form);

    Fiber fiber;
    fiber.push(Value::from_ptr(closure));
    fiber.stack.resize(std::max<size_t>(1, closure->max_locals), Value::unspecified());
    fiber.frames.push_back({closure, closure->chunk->code.data(), 0});

    VM::StepResult res = g_vm->step_fiber(fiber, VM::UNBOUNDED, deadline);
    res = pump_until_settled(fiber, res, deadline);
    out.res = res;
    out.result = fiber.result;
    out.error = fiber.error_message;
    if (res == VM::StepResult::Error || res == VM::StepResult::Preempted) break;
  }
  return out;
}

EMSCRIPTEN_KEEPALIVE
const char *vxs_eval_json(const char *code) {
  if (!g_vm) vxs_init();
  if (!code || *code == '\0') {
    g_eval_result_buffer = "{\"ok\":true,\"result\":\"\",\"type\":\"nil\",\"active_fibers\":0}";
    flush_default_sinks(*g_vm);
  return g_eval_result_buffer.c_str();
  }

  auto t_start = std::chrono::high_resolution_clock::now();

  try {
    // Unbounded instructions, but a wall-clock deadline: this is a
    // synchronous call on the browser's main thread, so a runaway
    // evaluation would freeze the tab. 750ms is editor-scale — generous
    // for a one-shot "run what's in the buffer" action. A hit deadline
    // is reported as a timeout, never dressed up as a result — that
    // would be the Yielded/Preempted conflation rebuilt one layer up.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    SeqOutcome outcome = eval_forms_sequentially(code, deadline);
    VM::StepResult res = outcome.res;
    g_vm->push_temp_root(&outcome.result);
    auto t_end = std::chrono::high_resolution_clock::now();
    double time_us = std::chrono::duration<double, std::micro>(t_end - t_start).count();

    if (res == VM::StepResult::Preempted) {
      g_eval_result_buffer = "{\"ok\":false,\"error\":\"evaluation exceeded 750ms and was stopped\",\"error_type\":\"timeout\",\"time_us\":" + std::to_string(time_us) + "}";
    } else if (res == VM::StepResult::Error) {
      g_eval_result_buffer = "{\"ok\":false,\"error\":\"" + escape_json(outcome.error) + "\",\"error_type\":\"runtime\",\"time_us\":" + std::to_string(time_us) + "}";
    } else {
      Value rv = outcome.result;
      std::string formatted = g_vm->format_value(rv);
      std::string type_name = rv.is_int() ? "integer" :
                              rv.is_double() ? "real" :
                              rv.is_symbol() ? "symbol" :
                              rv.is_bool() ? "boolean" :
                              rv.is_nil() ? "nil" :
                              Heap::is_cons(rv) ? "pair" :
                              Heap::is_closure(rv) ? "procedure" :
                              Heap::is_subr(rv) ? "primitive" :
                              Heap::is_future(rv) ? "future" : "object";

      g_eval_result_buffer = "{\"ok\":true,\"result\":\"" + escape_json(formatted) +
                             "\",\"type\":\"" + type_name +
                             "\",\"time_us\":" + std::to_string(time_us) +
                             ",\"active_fibers\":" + std::to_string(g_vm->active_fibers.size()) + "}";
    }
    g_vm->pop_temp_root();
  } catch (const RaiseEscape &e) {
    // An uncaught (raise ...)/(error ...) — a Scheme-level condition,
    // not a VM-internal fault, hence "runtime" (matching the ordinary
    // StepResult::Error branch above) rather than the generic "exception"
    // the catch-all below reports for things like a stale continuation.
    g_eval_result_buffer = "{\"ok\":false,\"error\":\"" + escape_json(e.what()) + "\",\"error_type\":\"runtime\"}";
  } catch (const std::exception &e) {
    g_eval_result_buffer = "{\"ok\":false,\"error\":\"" + escape_json(e.what()) + "\",\"error_type\":\"exception\"}";
  }

  flush_default_sinks(*g_vm);
  return g_eval_result_buffer.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char *vxs_eval(const char *code) {
  if (!g_vm) vxs_init();
  if (!code || *code == '\0') {
    g_eval_result_buffer = "";
    flush_default_sinks(*g_vm);
  return g_eval_result_buffer.c_str();
  }

  try {
    // Same deadline treatment as vxs_eval_json above.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    SeqOutcome outcome = eval_forms_sequentially(code, deadline);
    if (outcome.res == VM::StepResult::Preempted) {
      g_eval_result_buffer = "[Timeout] evaluation exceeded 750ms and was stopped";
    } else if (outcome.res == VM::StepResult::Error) {
      g_eval_result_buffer = outcome.error;
    } else {
      g_vm->push_temp_root(&outcome.result);
      g_eval_result_buffer = g_vm->format_value(outcome.result);
      g_vm->pop_temp_root();
    }
  } catch (const RaiseEscape &e) {
    // Already fully formatted — see the matching catch in vxs_eval_json.
    g_eval_result_buffer = e.what();
  } catch (const std::exception &e) {
    g_eval_result_buffer = "[Evaluation Error] " + std::string(e.what());
  }

  flush_default_sinks(*g_vm);
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
  size_t preempted = 0;
  // An uncaught (raise ...) or (error ...) inside a fiber unwinds as a C++
  // exception. Nothing here used to catch it, so it escaped the wasm call
  // entirely and reached JS as "#<CppException>" — a string containing none
  // of the message, the tag or the irritants. The whole diagnosis was
  // thrown away one frame short of the reporting below.
  //
  // Caught here and routed through fiber_errors, so a raise reports exactly
  // like any other fiber death: in full, to the page.
  try {
    if (instructions_per_fiber > 0) {
      preempted = g_vm->step_all_active_fibers(static_cast<size_t>(instructions_per_fiber));
    } else {
      preempted = g_vm->step_all_active_fibers(VM::UNBOUNDED, std::chrono::milliseconds(8));
    }
  } catch (const RaiseEscape &e) {
    g_vm->fiber_errors.push_back(e.what());
  } catch (const std::exception &e) {
    g_vm->fiber_errors.push_back(std::string("[Error] ") + e.what());
  }
  ++g_step_calls;
  g_preempt_total += preempted;
  // A fiber that died this frame would otherwise vanish without a word —
  // the scheduler reaps an errored fiber exactly like a finished one. Say
  // so, loudly, on the same channel as the frame-budget warning.
  for (const std::string &msg : g_vm->fiber_errors) {
    // Both channels, deliberately. stderr reaches the browser console,
    // which is where a developer looks; the terminal sink reaches the PAGE,
    // which is where the person running the program is already looking. A
    // fiber dying is the single most important thing this system can have
    // to say, and it used to say it only to the console.
    fprintf(stderr, "[vxs] fiber died: %s\n", msg.c_str());
    std::string line = "[fiber died] " + msg;
    js_sink_write("terminal", line.c_str());
  }
  g_vm->fiber_errors.clear();
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
      ",\"total_yields\":" + std::to_string(g_vm->total_yields) +
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

// Settle an external future with a BYTES value, stamped with the frame the
// copy was submitted on.
//
// A pair — (frame . bytes) — rather than the bytes alone, because the
// snapshot is always lagged and silently pretending otherwise is how a
// feedback loop becomes mysterious. Callers that do not care write
// (cdr result) and ignore it.
EMSCRIPTEN_KEEPALIVE
int vxs_settle_bytes(int token, const unsigned char *data, int len, int frame) {
  if (!g_vm) return 0;
  size_t n = len > 0 ? static_cast<size_t>(len) : 0;
  Value b = g_vm->heap.make_bytes(n);
  g_vm->push_temp_root(&b);
  if (n > 0 && data) {
    std::memcpy(b.as_ptr<ObjBytes>()->data.data(), data, n);
  }
  Value stamped = g_vm->heap.cons(Value::from_int(frame), b);
  g_vm->pop_temp_root();
  g_vm->push_temp_root(&stamped);
  bool ok = g_vm->settle_external(static_cast<uint32_t>(token), stamped, false);
  g_vm->pop_temp_root();
  return ok ? 1 : 0;
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
