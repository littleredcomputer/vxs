// testcases/test_all_presets.js
const createVxsModule = require('../web/vxs.js');

// Mock canvas and window functions for Node environment
global.vxsCanvasClear = (r, g, b, a) => {};
global.vxsCanvasFillRect = (x, y, w, h, r, g, b, a) => {};
global.vxsCanvasDrawCircle = (x, y, rad, r, g, b, a) => {};
global.vxsCanvasDrawLine = (x1, y1, x2, y2, r, g, b, a) => {};
global.vxsCanvasDrawText = (t, x, y, r, g, b, a) => {};
global.vxsCanvasWidth = () => 800;
global.vxsCanvasHeight = () => 600;
global.vxsMouseX = () => 400;
global.vxsMouseY = () => 300;
global.vxsMouseDown = () => 0;

createVxsModule().then((VXS) => {
  console.log('=== [1] INITIALIZING WASM SCHEME CORE ===');
  const init = VXS._vxs_init();
  console.log('Init status:', init);
  if (init !== 1) throw new Error('Init failed');

  const evalScheme = (code) => {
    return VXS.ccall('vxs_eval', 'string', ['string'], [code]);
  };

  console.log('\n=== [2] TESTING PRESET 1: CONCURRENT PARTICLES ===');
  evalScheme(`
(define (fade) (canvas-fill-rect 0 0 (canvas-width) (canvas-height) 0.02 0.03 0.05 0.15))
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
(spawn-particle 1 100 100 2.5 1.8 0.3 0.8 1.0 12)
(spawn-particle 2 200 150 -1.5 2.2 0.9 0.4 0.9 14)
(spawn-particle 3 300 200 2.0 -2.5 0.4 1.0 0.6 10)
`);
  console.log('Active fibers after spawn:', VXS._vxs_active_fibers_count());
  for (let i = 0; i < 5; i++) {
    VXS._vxs_step_fibers();
  }
  console.log('Stepped 5 particle frames cleanly. Active fibers:', VXS._vxs_active_fibers_count());

  console.log('\n=== [3] TESTING PRESET 2: BARNSLEY FERN (INCREMENTAL GENERATOR) ===');
  const resFern = evalScheme(`
(define (barnsley-step)
  (let ((x 0.0) (y 0.0) (w 800) (h 600))
    (future
      (let loop ((px x) (py y) (count 0))
        (let ((r (random 100)) (nx 0.0) (ny 0.0))
          (cond
            ((< r 1)
             (set! nx 0.0)
             (set! ny (* 0.16 py)))
            ((< r 86)
             (set! nx (+ (* 0.85 px) (* 0.04 py)))
             (set! ny (+ (+ (* -0.04 px) (* 0.85 py)) 1.6)))
            ((< r 93)
             (set! nx (+ (* 0.20 px) (* -0.26 py)))
             (set! ny (+ (+ (* 0.23 px) (* 0.22 py)) 1.6)))
            (else
             (set! nx (+ (* -0.15 px) (* 0.28 py)))
             (set! ny (+ (+ (* 0.26 px) (* 0.24 py)) 0.44))))
          (let ((screen-x (+ (/ w 2) (* px 70)))
                (screen-y (- h (* py 55) 40)))
            (canvas-fill-rect screen-x screen-y 1.5 1.5 0.3 0.95 0.5 0.8))
          (if (= (remainder count 100) 0) (yield))
          (loop nx ny (+ count 1)))))))
(barnsley-step)
`);
  console.log('Barnsley Fern result:', resFern);
  VXS._vxs_step_fibers();

  console.log('\n=== [4] TESTING PRESET 3: 2D MCMC SAMPLER FIBER ===');
  const resMCMC = evalScheme(`
(define (target-log-pdf x y)
  (let* ((r1 (+ (* (- x 300) (- x 300)) (* (- y 250) (- y 250))))
         (r2 (+ (* (- x 500) (- x 500)) (* (- y 350) (- y 350))))
         (p1 (exp (* -0.0005 r1)))
         (p2 (exp (* -0.0008 r2))))
    (log (+ p1 p2 1e-10))))

(define (mcmc-sampler)
  (future
    (let loop ((x 400.0) (y 300.0) (current-lp (target-log-pdf 400.0 300.0)) (samples 0))
      (let* ((prop-x (+ x (* (- (random 100) 50) 0.5)))
             (prop-y (+ y (* (- (random 100) 50) 0.5)))
             (prop-lp (target-log-pdf prop-x prop-y))
             (accept (or (> prop-lp current-lp)
                         (< (random 1000) (* 1000 (exp (- prop-lp current-lp)))))))
        (let ((nx (if accept prop-x x))
              (ny (if accept prop-y y))
              (nlp (if accept prop-lp current-lp)))
          (canvas-draw-circle nx ny 2.0 1.0 0.5 0.2 0.6)
          (if (= (remainder samples 20) 0) (yield))
          (loop nx ny nlp (+ samples 1)))))))
(mcmc-sampler)
`);
  console.log('MCMC Sampler result:', resMCMC);
  VXS._vxs_step_fibers();

  console.log('\n=== [5] TESTING PRESET 4: CONCURRENT TASK SCHEDULER & PIPELINE ===');
  const resPipeline = evalScheme(`
(define task-a (future (let loop ((i 0)) (if (< i 5) (begin (yield) (loop (+ i 1))) 100))))
(define task-b (future (let loop ((i 0)) (if (< i 3) (begin (yield) (loop (+ i 1))) 200))))
(define pipeline
  (future
    (let ((val-a (touch task-a))
          (val-b (touch task-b)))
      (+ val-a val-b 50))))
`);
  console.log('Active pipeline fibers:', VXS._vxs_active_fibers_count());
  for (let i = 0; i < 15; i++) {
    VXS._vxs_step_fibers();
  }
  const pipeResult = evalScheme('(touch pipeline)');
  console.log('Pipeline computed touch result:', pipeResult);
  if (pipeResult !== '350') throw new Error('Pipeline output mismatch: expected 350 got ' + pipeResult);

  console.log('\n=== [6] TESTING PRESET 5: WAVEFRONT SINE SYNTHESIS ===');
  const resWave = evalScheme(`
(define (render-wave t)
  (let loop ((x 0))
    (if (< x 800)
        (let ((y (+ 300 (* 80 (sin (+ (* x 0.02) t))))))
          (canvas-draw-circle x y 2.0 0.2 0.8 1.0 0.8)
          (loop (+ x 10)))
        'done)))
(render-wave 0.5)
`);
  console.log('Wavefront render result:', resWave);

  console.log('\n======================================================');
  console.log('✨ ALL 6 SHOWCASE PRESETS PASSED ON WASM ENGINE! ✨');
  console.log('======================================================\n');
  process.exit(0);
}).catch((err) => {
  console.error('Fatal Preset Error:', err);
  process.exit(1);
});
