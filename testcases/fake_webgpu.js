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
    _listeners: listeners,
    addEventListener(kind, fn) { listeners.push([kind, fn]); },
    createBuffer({ size, usage }) { return makeBuffer(size, usage); },
    createShaderModule({ code }) {
      return {
        code,
        getCompilationInfo() {
          const messages = (opts && opts.compileMessages) ? opts.compileMessages(code) : [];
          return Promise.resolve({ messages });
        },
      };
    },
    createBindGroupLayout() { return { _kind: 'bgl' }; },
    createBindGroup() { return { _kind: 'bindgroup' }; },
    createPipelineLayout() { return { _kind: 'layout' }; },
    createComputePipeline() { return { _kind: 'compute' }; },
    createRenderPipeline() { return { _kind: 'render' }; },
    createTexture() { return { createView: () => ({}), destroy() {} }; },
    createCommandEncoder() {
      const cmds = [];
      return {
        copyBufferToBuffer(src, srcOff, dst, dstOff, n) {
          cmds.push(() => dst._bytes.set(src._bytes.subarray(srcOff, srcOff + n), dstOff));
        },
        beginComputePass() {
          return { setPipeline() {}, setBindGroup() {}, dispatchWorkgroups() {}, end() {} };
        },
        beginRenderPass() {
          return { setPipeline() {}, setBindGroup() {}, draw() {}, end() {} };
        },
        finish() { return { _cmds: cmds }; },
      };
    },
    queue: {
      writeBuffer(buf, offset, data) {
        const src = data instanceof Uint8Array
          ? data
          : new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
        buf._bytes.set(src, offset);
      },
      submit(list) { for (const c of list) for (const f of c._cmds) f(); },
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
