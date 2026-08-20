#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include "vx_reader.h"
#include "vx_compiler.h"
#include "vx_embedded_libs.h"
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
EM_JS(int, js_gpu_run_kernel, (int deviceId, const char *wgslPtr, const char *canvasIdPtr, double time), {
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

    globalThis.vxsKernelCache = globalThis.vxsKernelCache || {};
    var key = canvasId + " " + wgsl;
    var entry = globalThis.vxsKernelCache[key];
    if (!entry || entry.device !== device) {
      var format = navigator.gpu.getPreferredCanvasFormat();
      ctx.configure({ device: device, format: format, alphaMode: 'opaque' });
      var module = device.createShaderModule({ code: wgsl });
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
EM_JS(int, js_gpu_draw_instances, (int deviceId, const char *wgslPtr, const char *canvasIdPtr, const unsigned char *dataPtr, int dataLen, int instances, double time), {
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

    globalThis.vxsInstanceCache = globalThis.vxsInstanceCache || {};
    var key = canvasId + " " + wgsl;
    var entry = globalThis.vxsInstanceCache[key];
    if (!entry || entry.device !== device) {
      var format = navigator.gpu.getPreferredCanvasFormat();
      ctx.configure({ device: device, format: format, alphaMode: 'opaque' });
      var module = device.createShaderModule({ code: wgsl });
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
        size: 16,
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

    device.queue.writeBuffer(entry.ubuf, 0,
      new Float32Array([time, canvas.width, canvas.height, instances]));
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
static int js_gpu_run_kernel(int, const char *, const char *, double) { return -1; }
static int js_gpu_draw_instances(int, const char *, const char *, const unsigned char *, int, int, double) { return -1; }
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
    if (!Heap::is_string(args[1])) {
      vm.raise_contract("gpu-run-kernel!: expected WGSL source as a string");
    }
    if (!args[2].is_int() && !args[2].is_double()) {
      vm.raise_contract("gpu-run-kernel!: expected a number for time, got " +
                        vm.format_value(args[2]));
    }
    std::string wgsl(args[1].as_ptr<ObjString>()->view());
    double t = args[2].is_int() ? static_cast<double>(args[2].as_int())
                                : args[2].as_double();
    std::string canvas_id = (argc > 3 && Heap::is_string(args[3]))
        ? std::string(args[3].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_run_kernel(static_cast<int>(d->id), wgsl.c_str(), canvas_id.c_str(), t);
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-run-kernel!: " + detail);
    }
    return Value::boolean_true();
  }, 3, 4));

  // (gpu-draw-instances! device wgsl bytes count time [canvas-id])
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
    if (!Heap::is_string(args[1])) {
      vm.raise_contract("gpu-draw-instances!: expected WGSL source as a string");
    }
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
    std::string wgsl(args[1].as_ptr<ObjString>()->view());
    std::string canvas_id = (argc > 5 && Heap::is_string(args[5]))
        ? std::string(args[5].as_ptr<ObjString>()->view())
        : std::string("gpu-canvas");
    int rc = js_gpu_draw_instances(static_cast<int>(d->id), wgsl.c_str(),
                                   canvas_id.c_str(), b->data.data(),
                                   static_cast<int>(b->data.size()),
                                   instances, t);
    if (rc != 0) {
      char *msg = js_gpu_last_error();
      std::string detail = msg ? msg : "unknown";
      if (msg) std::free(msg);
      vm.raise_contract("gpu-draw-instances!: " + detail);
    }
    return Value::boolean_true();
  }, 5, 6));

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
static size_t g_step_calls = 0;
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
  size_t preempted;
  if (instructions_per_fiber > 0) {
    preempted = g_vm->step_all_active_fibers(static_cast<size_t>(instructions_per_fiber));
  } else {
    preempted = g_vm->step_all_active_fibers(VM::UNBOUNDED, std::chrono::milliseconds(8));
  }
  ++g_step_calls;
  g_preempt_total += preempted;
  // A fiber that died this frame would otherwise vanish without a word —
  // the scheduler reaps an errored fiber exactly like a finished one. Say
  // so, loudly, on the same channel as the frame-budget warning.
  for (const std::string &msg : g_vm->fiber_errors) {
    fprintf(stderr, "[vxs] fiber died: %s\n", msg.c_str());
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
