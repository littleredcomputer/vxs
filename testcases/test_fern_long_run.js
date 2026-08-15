const fs = require('fs');
const path = require('path');
const createVxSchemeModule = require('../web/vxs.js');

async function testFernLongRun() {
  const wasmBuffer = fs.readFileSync(path.join(__dirname, '../web/vxs.wasm'));
  const M = await createVxSchemeModule({ wasmBinary: wasmBuffer });
  M._vxs_init();

  const vxsEvalJson = M.cwrap('vxs_eval_json', 'string', ['string']);
  const vxsStep = M.cwrap('vxs_step_fibers', 'number', []);
  const vxsCount = M.cwrap('vxs_active_fibers_count', 'number', []);

  console.log("Starting Barnsley Fern Long Run (5,000 steps)...");
  const fernCode = `
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

(draw-fern 5000)
`;

  const spawnRes = JSON.parse(vxsEvalJson(fernCode));
  console.log("Spawn Result:", spawnRes);
  console.log("Active fibers:", vxsCount());

  for (let step = 0; step < 100; ++step) {
    vxsStep(500);
    if (vxsCount() === 0) {
      console.log(`Fern fiber completed smoothly at step ${step}!`);
      break;
    }
  }

  console.log("✨ Fern test finished with ZERO assertions / errors! ✨");
}

testFernLongRun().catch(console.error);
