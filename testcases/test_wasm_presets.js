const fs = require('fs');
const path = require('path');
const createVxsModule = require('../web/vxs.js');

const presets = {
  particles: `
;; Trail fade background
(define (fade)
  (canvas-fill-rect 0 0 (canvas-width) (canvas-height) 0.02 0.03 0.05 0.15))

;; Spawn a single particle fiber
(define (spawn-particle id x y vx vy r g b radius)
  (future
    (let loop ((px x) (py y) (vx vx) (vy vy) (life 0))
      (let* ((w (canvas-width))
             (h (canvas-height))
             (nvx (if (or (< px radius) (> px (- w radius))) (- vx) vx))
             (nvy (if (or (< py radius) (> py (- h radius))) (- vy) vy))
             (nx (+ px nvx))
             (ny (+ py nvy))
             (pulse (+ 0.6 (* 0.4 (sin (* life 0.05))))))
        (canvas-draw-circle nx ny (* radius pulse) (* r pulse) (* g pulse) (* b pulse) 0.9)
        (yield)
        (loop nx ny nvx nvy (+ life 1))))))

(future
  (let loop ()
    (fade)
    (yield)
    (loop)))

(do ((i 0 (+ i 1)))
    ((= i 10) 'particles-spawned)
  (spawn-particle i 400 300 2 2 1.0 0.5 0.2 4))
`,

  attractor: `
(define (draw-fern n-points)
  (future
    (let loop ((x 0.0) (y 0.0) (count 0))
      (let ((w (canvas-width))
            (h (canvas-height)))
        (let ((sx (+ (/ w 2) (* x 55)))
              (sy (- (- h 30) (* y 55))))
          (canvas-draw-circle sx sy 1.0 0.25 0.95 0.45 0.75))
        
        (let* ((r (random 100))
               (next-coords
                (cond
                  ((< r 1)  (list 0.0 (* 0.16 y)))
                  ((< r 86) (list (+ (* 0.85 x) (* 0.04 y))
                                  (+ (* -0.04 x) (* 0.85 y) 1.6)))
                  ((< r 93) (list (- (* 0.20 x) (* 0.26 y))
                                  (+ (* 0.23 x) (* 0.22 y) 1.6)))
                  (else     (list (+ (* -0.15 x) (* 0.28 y))
                                  (+ (* 0.26 x) (* 0.24 y) 0.44))))))
          
          (if (= (remainder count 50) 0)
              (yield))
          
          (if (< count n-points)
              (loop (car next-coords) (cadr next-coords) (+ count 1))
              'fern-complete))))))

(draw-fern 500)
`,

  mcmc: `
(define (target-log-p x y)
  (let* ((d1 (+ (* (- x 250) (- x 250)) (* (- y 250) (- y 250))))
         (d2 (+ (* (- x 550) (- x 550)) (* (- y 350) (- y 350))))
         (p1 (exp (/ (- d1) 6000.0)))
         (p2 (* 1.5 (exp (/ (- d2) 8000.0)))))
    (+ p1 p2)))

(future
  (let loop ((curr-x 400.0) (curr-y 300.0) (samples 0) (accepted 0))
    (let* ((prop-x (+ curr-x (- (random 40.0) 20.0)))
           (prop-y (+ curr-y (- (random 40.0) 20.0)))
           (curr-p (target-log-p curr-x curr-y))
           (prop-p (target-log-p prop-x prop-y))
           (alpha (if (> curr-p 0) (/ prop-p curr-p) 1.0))
           (accept? (< (random 1.0) alpha))
           (nx (if accept? prop-x curr-x))
           (ny (if accept? prop-y curr-y)))
      
      (if accept?
          (canvas-draw-circle nx ny 2.2 0.2 0.8 1.0 0.45)
          (canvas-draw-circle prop-x prop-y 1.0 0.9 0.2 0.3 0.15))
      
      (if (= (remainder samples 5) 0)
          (yield))
      
      (if (< samples 100)
          (loop nx ny (+ samples 1) (if accept? (+ accepted 1) accepted))
          'mcmc-done))))
`,

  wave: `
(future
  (let loop ((t 0))
    (canvas-fill-rect 0 0 (canvas-width) (canvas-height) 0.03 0.04 0.07 0.2)
    (let ((mx (mouse-x))
          (my (mouse-y)))
      (let ring-loop ((i 0))
        (when (< i 12)
          (let* ((phase (+ (* t 0.05) (* i 0.5)))
                 (radius (remainder (floor (* phase 40)) 350))
                 (alpha (- 1.0 (/ radius 350.0)))
                 (r (+ 0.3 (* 0.7 (sin phase))))
                 (g (+ 0.5 (* 0.5 (cos phase))))
                 (b 0.9))
            (canvas-draw-circle mx my radius r g b (* alpha 0.4))
            (ring-loop (+ i 1))))))
    (yield)
    (if (< t 10) (loop (+ t 1)) 'wave-done)))
`,

  fibers: `
(define (task name steps base-val)
  (future
    (let loop ((i 1) (acc base-val))
      (yield)
      (if (< i steps)
          (loop (+ i 1) (+ acc i))
          acc))))

(define f1 (task 'alpha 4 100))
(define f2 (task 'beta 6 200))
(define f3 (task 'gamma 8 300))

(display "Spawned tasks.\n")
(display (touch f1)) (newline)
(display (touch f2)) (newline)
(display (touch f3)) (newline)
(list (touch f1) (touch f2) (touch f3))
`,

  repl: `
(define (fact n)
  (if (<= n 1) 1 (* n (fact (- n 1)))))
(display "Factorial 10 = ")
(display (fact 10))
(newline)

(define (make-adder x) (lambda (y) (+ x y)))
(define add10 (make-adder 10))
(display "add10(32) = ")
(display (add10 32))
(newline)

(when (= (+ 2 2) 4)
  (display "When macro works beautifully!\n"))
`
};

// How many fibers each preset is expected to leave running after load.
// null = don't care (the preset finishes synchronously or its count is
// not the interesting property).
const expectedFibers = {
  particles: 11,   // 10 spawned + 1 fade controller
  attractor: 1,
  mcmc: 1,
  wave: 1,
  fibers: null,
  repl: 0,
};

async function run() {
  const wasmPath = path.join(__dirname, '../web/vxs.wasm');
  const wasmBuffer = fs.readFileSync(wasmPath);

  // The repl/fibers presets `display` their own output. That is the
  // program working, not information about the test, so swallow it —
  // otherwise a passing run buries its own result in program chatter.
  global.vxsPrint = () => {};

  const M = await createVxsModule({
    wasmBinary: wasmBuffer
  });
  M._vxs_init();
  const vxsEval = M.cwrap('vxs_eval', 'string', ['string']);
  const vxsStep = M.cwrap('vxs_step_fibers', 'number', ['number']);
  const vxsCount = M.cwrap('vxs_active_fibers_count', 'number', []);
  const vxsClear = M.cwrap('vxs_clear_fibers', null, []);

  const names = Object.keys(presets);
  console.log(`=== RUNNING ALL ${names.length} PRESETS IN WEBASSEMBLY ENGINE ===`);

  let passed = 0;
  let failed = 0;
  const fail = (name, why) => {
    console.log(`  ❌ [FAIL] ${name.padEnd(12)} ${why}`);
    failed++;
  };

  for (const [name, code] of Object.entries(presets)) {
    vxsClear();
    const out = vxsEval(code);

    // `timeout` is in this list because it was NOT, and a preset that blew
    // the 750ms evaluation cap reported "[Timeout] evaluation exceeded
    // 750ms and was stopped" — which contains neither "error" nor
    // "exception", so it sailed through as a pass. A preset that never
    // finished looked identical to one that finished perfectly.
    if (typeof out === 'string' && /error|exception|timeout/i.test(out)) {
      fail(name, `evaluation reported: ${out}`);
      continue;
    }

    const active = vxsCount();
    const want = expectedFibers[name];
    if (want !== null && want !== undefined && active !== want) {
      fail(name, `expected ${want} active fiber(s), got ${active}`);
      continue;
    }

    // Pump the real scheduler path (0 = run each fiber to its own yield
    // under the wall-clock backstop), not the legacy instruction cap.
    let stepped = 0;
    for (let i = 0; i < 10 && vxsCount() > 0; i++) { vxsStep(0); stepped++; }

    if (vxsCount() > 0 && stepped === 0) {
      fail(name, 'fibers active but scheduler made no progress');
      continue;
    }

    const detail = active > 0
      ? `${active} fiber(s), stepped ${stepped} frame(s) cleanly`
      : `completed synchronously`;
    console.log(`  ✅ [PASS] ${name.padEnd(12)} ${detail}`);
    passed++;
  }

  console.log(`\n────────────────────────────────────────────────────────────────`);
  console.log(`Presets: ${names.length} | Passed: ${passed} | Failed: ${failed}`);
  if (failed === 0) {
    console.log('✨ ALL PRESETS VERIFIED WORKING ON WASM! ✨');
  } else {
    console.log('💥 PRESET VERIFICATION FAILED');
    process.exit(1);
  }
}

run().catch(e => {
  console.error("FATAL ERROR IN RUN():", e);
  process.exit(1);
});
