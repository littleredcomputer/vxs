//----------------------------------------------------------------------
// A minimal fake WebGPU, enough to exercise the host paths under Node.
//
// It computes nothing — no shader ever runs — and that is the point. What
// it makes testable is everything AROUND the shader: handle lifetimes,
// buffer uploads, the readback staging dance, error settling, and whether
// a compile diagnostic reaches the terminal. Those are exactly the places
// failures have been silent, and none of them needed a GPU to be wrong.
//
// Buffers hold real bytes, so a write followed by a read must return what
// was written. copyBufferToBuffer really copies. mapAsync really resolves
// on a later turn of the event loop, which is what makes the lag in a
// readback observable rather than theoretical.
//----------------------------------------------------------------------

// The constant enums. These are globals in a browser, and their absence
// is not a subtle failure — "GPUBufferUsage is not defined" arrives as a
// dead fiber with the name in it. Values from the WebGPU specification.
function installConstants() {
  globalThis.GPUBufferUsage = {
    MAP_READ: 0x0001, MAP_WRITE: 0x0002, COPY_SRC: 0x0004, COPY_DST: 0x0008,
    INDEX: 0x0010, VERTEX: 0x0020, UNIFORM: 0x0040, STORAGE: 0x0080,
    INDIRECT: 0x0100, QUERY_RESOLVE: 0x0200,
  };
  globalThis.GPUMapMode = { READ: 0x0001, WRITE: 0x0002 };
  globalThis.GPUShaderStage = { VERTEX: 0x1, FRAGMENT: 0x2, COMPUTE: 0x4 };
  globalThis.GPUTextureUsage = {
    COPY_SRC: 0x01, COPY_DST: 0x02, TEXTURE_BINDING: 0x04,
    STORAGE_BINDING: 0x08, RENDER_ATTACHMENT: 0x10,
  };
}

function makeBuffer(size, usage) {
  const bytes = new Uint8Array(size);
  return {
    size,
    usage,
    _bytes: bytes,
    _mapped: null,
    mapAsync(_mode) {
      // Deliberately asynchronous. A fake that resolved synchronously
      // would hide the very lag the real API forces on callers.
      return new Promise((resolve) => setTimeout(resolve, 0));
    },
    getMappedRange() {
      this._mapped = bytes.buffer.slice(0, size);
      return this._mapped;
    },
    unmap() { this._mapped = null; },
    destroy() { this._destroyed = true; },
  };
}

function makeDevice(opts) {
  const listeners = [];
  return {
    // Real devices report this; the wrangle path spaces its per-substep
    // uniform slices by it. 256 is the guaranteed maximum and the value
    // essentially every implementation uses.
    limits: { minUniformBufferOffsetAlignment: 256 },
    _listeners: listeners,
    addEventListener(kind, fn) { listeners.push([kind, fn]); },
    createBuffer({ size, usage }) { return makeBuffer(size, usage); },
    createShaderModule({ code }) {
      this._compiles = (this._compiles || 0) + 1;
      return {
        code,
        getCompilationInfo() {
          const messages = (opts && opts.compileMessages) ? opts.compileMessages(code) : [];
          return Promise.resolve({ messages });
        },
      };
    },
    createBindGroupLayout() { return { _kind: 'bgl' }; },
    createBindGroup(desc) {
      // Keep the entries: with a dynamic offset the uniform binding MUST
      // declare an explicit size, or it runs to the end of the buffer and
      // every offset past the first overruns it.
      this._lastBindGroup = desc;
      // Carry the descriptor on the object: pipelines are cached, so a
      // later dispatch may BIND a group without CREATING one, and a test
      // that reads the last-created group would be reading a stale one.
      return { _kind: 'bindgroup', _desc: desc };
    },
    createPipelineLayout() { return { _kind: 'layout' }; },
    createComputePipeline() {
      this._computePipelines = (this._computePipelines || 0) + 1;
      return { _kind: 'compute' };
    },
    createRenderPipeline() { return { _kind: 'render' }; },
    createTexture() { return { createView: () => ({}), destroy() {} }; },
    createCommandEncoder() {
      const cmds = [];
      const self = this;
      self._encoders = (self._encoders || 0) + 1;
      return {
        copyBufferToBuffer(src, srcOff, dst, dstOff, n) {
          cmds.push(() => dst._bytes.set(src._bytes.subarray(srcOff, srcOff + n), dstOff));
        },
        beginComputePass() {
          // Record what the pass was told to do. Substeps are invisible
          // otherwise: N dispatches at N distinct dynamic offsets inside
          // one pass look exactly like one dispatch unless someone counts.
          const log = (self._passLog = { offsets: [], dispatches: 0 });
          return {
            setPipeline() {},
            setBindGroup(_i, bg, dyn) {
              if (dyn) log.offsets.push(dyn[0]);
              if (bg && bg._desc) self._boundGroup = bg._desc;
            },
            dispatchWorkgroups() { log.dispatches++; },
            end() {},
          };
        },
        beginRenderPass() {
          return { setPipeline() {}, setBindGroup() {}, draw() {}, end() {} };
        },
        finish() { return { _cmds: cmds }; },
      };
    },
    queue: {
      writeBuffer(buf, offset, data) {
        // Record the last uniform write so a test can read back what the
        // kernel would have seen, without running a kernel.
        this._lastWrite = { size: buf.size, bytes: null };
        const src = data instanceof Uint8Array
          ? data
          : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        buf._bytes.set(src, offset);
        this._lastWrite.bytes = buf._bytes.slice(0, buf.size);
      },
      submit(list) {
        this._submits = (this._submits || 0) + 1;
        for (const c of list) for (const f of c._cmds) f();
      },
    },
  };
}

// Install as the platform. Must run BEFORE vxs_init, because
// js_ensure_handle_table only creates a handle table if none exists.
function installFakeWebGPU(opts) {
  installConstants();
  const device = makeDevice(opts);
  globalThis.navigator = globalThis.navigator || {};
  globalThis.navigator.gpu = {
    getPreferredCanvasFormat() { return 'bgra8unorm'; },
    requestAdapter() { return Promise.resolve({ requestDevice: () => Promise.resolve(device) }); },
  };
  globalThis.document = {
    getElementById(id) {
      return { width: 800, height: 600, getContext: () => ({ configure() {}, getCurrentTexture: () => ({ createView: () => ({}) }) }) };
    },
  };
  return device;
}

module.exports = { installFakeWebGPU };
