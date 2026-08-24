//----------------------------------------------------------------------
// Every GPU preset, driven headlessly against a fake WebGPU.
//
// These six presets were previously verifiable only by opening a browser,
// picking one from a dropdown, and looking at a canvas — which meant a
// regression could be described but never pasted, and that the render
// loops in lib/gpu.scm were the least-tested code in the system despite
// being the code every demo runs through.
//
// The source is EXTRACTED FROM web/app.js rather than copied here. A test
// holding its own duplicate of a preset passes happily while the real one
// is broken, which is the failure mode that matters most for a file this
// long.
//
// What a pass means: the preset evaluates, spawns fibers, and survives ten
// scheduler frames with no fiber dying. The fake computes nothing, so
// nothing here says the picture is right — it says the program gets as far
// as asking for a picture, which is exactly the part that kept breaking.
//----------------------------------------------------------------------

const fs = require('fs');
const path = require('path');
const { installFakeWebGPU } = require(path.join(__dirname, 'fake_webgpu.js'));

// Before the module loads: js_ensure_handle_table only builds a table if
// none exists, and navigator.gpu is read at adapter request.
installFakeWebGPU({ compileMessages: () => [] });

const createVxsModule = require(path.join(__dirname, '..', 'web', 'vxs.js'));

const GPU_PRESETS = ['plasma', 'rings', 'points', 'wrangle', 'actors', 'cubes', 'field', 'ensemble'];

// Does web/app.js actually PARSE as JavaScript?
//
// Everything else here extracts preset text by hand and evaluates it as
// Scheme, which means a JavaScript syntax error in the file sails straight
// through: the extractor finds its markers, the preset runs, every test
// passes, and the browser then refuses to load the page at all. That
// happened — a backtick inside a Scheme comment, `pose`, closed the
// template literal early and killed the whole application while this suite
// stayed green.
//
// One `node --check` costs nothing and catches the entire class.
function checkAppParses() {
  const { execFileSync } = require('child_process');
  const app = path.join(__dirname, '..', 'web', 'app.js');
  try {
    execFileSync(process.execPath, ['--check', app], { stdio: 'pipe' });
  } catch (e) {
    throw new Error('web/app.js is not valid JavaScript:\n' +
                    String(e.stderr || e.message).split('\n').slice(0, 6).join('\n'));
  }
}

// A preset body lives inside a JS template literal, so a backtick or a
// ${ } in Scheme source terminates it early. The Scheme is comment-heavy
// and backticks are natural punctuation in prose, which makes this an easy
// mistake and an invisible one.
function checkPresetQuoting(presets) {
  for (const [name, code] of Object.entries(presets)) {
    if (code.indexOf('`') >= 0) {
      throw new Error(`preset ${name} contains a backtick, which ends its template literal`);
    }
    if (code.indexOf('${') >= 0) {
      throw new Error(`preset ${name} contains \${, which interpolates inside its template literal`);
    }
  }
}

function extractPresets(names) {
  const src = fs.readFileSync(path.join(__dirname, '..', 'web', 'app.js'), 'utf8');
  const body = src.slice(src.indexOf('const PRESETS = {'));
  const out = {};
  for (const n of names) {
    const open = '\n    ' + n + ': `';
    const i = body.indexOf(open);
    if (i < 0) throw new Error(`preset ${n} not found in web/app.js`);
    const s = i + open.length;
    const e = body.indexOf('`,', s);
    if (e < 0) throw new Error(`preset ${n} is unterminated in web/app.js`);
    out[n] = body.slice(s, e);
  }
  return out;
}

(async () => {
  checkAppParses();
  const presets = extractPresets(GPU_PRESETS);
  checkPresetQuoting(presets);

  // Presets talk to the page. Keep their chatter out of the report, but
  // watch it: a dead fiber announces itself here and nowhere else, and a
  // preset whose fiber dies on frame one otherwise looks identical to one
  // that ran perfectly.
  let chatter = [];
  globalThis.vxsPrint = (t) => chatter.push(String(t));

  const M = await createVxsModule();
  M._vxs_init();
  const ev = M.cwrap('vxs_eval', 'string', ['string']);
  const step = M.cwrap('vxs_step_fibers', 'number', ['number']);
  const count = M.cwrap('vxs_active_fibers_count', 'number', []);
  const clear = M.cwrap('vxs_clear_fibers', null, []);

  console.log(`=== RUNNING ${GPU_PRESETS.length} GPU PRESETS (fake WebGPU) ===`);
  let passed = 0, failed = 0;
  const fail = (name, why) => {
    console.log(`  ❌ [FAIL] ${name.padEnd(9)} ${why}`);
    failed++;
  };

  for (const name of GPU_PRESETS) {
    clear();
    chatter = [];

    const out = ev(presets[name]);
    if (typeof out === 'string' && /error|exception/i.test(out)) {
      fail(name, `evaluation reported: ${out.trim()}`);
      continue;
    }
    if (count() === 0) {
      fail(name, 'spawned no fibers — a render loop that is not running');
      continue;
    }

    // The device arrives through a promise, so the scheduler has to be
    // pumped with the event loop given a turn in between — the same shape
    // requestAnimationFrame provides in the browser. Ten frames is past
    // adapter, device, compile and the first several draws.
    let stepped = 0;
    for (let i = 0; i < 10; i++) {
      step(0);
      stepped++;
      await new Promise((r) => setTimeout(r, 1));
    }

    const died = chatter.filter((l) => /fiber died/.test(l));
    if (died.length) { fail(name, died[0]); continue; }
    if (count() === 0) {
      fail(name, `all fibers gone after ${stepped} frames`);
      continue;
    }
    const wgsl = chatter.filter((l) => /WGSL (error|warning)/.test(l));
    if (wgsl.length) { fail(name, wgsl[0]); continue; }

    console.log(`  ✅ [PASS] ${name.padEnd(9)} ${count()} fiber(s), ${stepped} frames, no deaths`);
    passed++;
  }

  console.log('\n────────────────────────────────────────────────────────────────');
  console.log(`GPU presets: ${GPU_PRESETS.length} | Passed: ${passed} | Failed: ${failed}`);
  console.log(failed ? '💥 GPU PRESET VERIFICATION FAILED' : '✨ ALL GPU PRESETS VERIFIED ✨');
  process.exit(failed ? 1 : 0);
})();
