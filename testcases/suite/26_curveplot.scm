;;----------------------------------------------------------------------
;; Layer 26: the posterior plot follows the model
;;
;; lib/curveplot.scm draws a curve family's posterior by looping the
;; particles per pixel. The claim it exists to support is that ONE
;; define-dual serves the model, the inference and the picture — so what
;; is worth testing is not that a shader comes out, but that the shader
;; FOLLOWS: change the curve's arity or its body and the plot tracks it
;; with no edit of its own.
;;
;; Contrast lib/fitplot.scm, whose `plot-curve!` has the quadratic written
;; into it by hand. That is what a plotter must do when it cannot be told
;; what the curve is, and it is the binding this layer asserts is gone.
;;
;; NOTHING HERE RUNS WGSL. run-kernel-loop is stubbed so the real
;; plot-posterior! executes end to end and hands back the shader it would
;; have dispatched. What a device does with it still needs an eye.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/curveplot.scm")

(test-suite "26_curveplot: the picture follows the curve")

;; Capture instead of dispatch.
(define captured #f)
(set! run-kernel-loop (lambda (wgsl . opts) (set! captured wgsl) 'stubbed))

;;--- a two-parameter family ---------------------------------------------

(define-dual (line-elem (x :f32) (m :f32) (k :f32)) (+ (* m x) k))

(define-gen (line xs sigma npts)
  (let* ((m (at :m (normal 0 1.5)))
         (k (at :k (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (line-elem (view-ref xs j) m k) sigma)))))

(define N 8)
(define xs (bytes-view (make-bytes (* N 8)) :f64))
(let loop ((i 0))
  (if (< i N) (begin (view-set! xs i (+ -2.0 (* 0.5 i))) (loop (+ i 1)))))
(define ys (bytes-view (make-bytes (* N 8)) :f64))
(let loop ((i 0))
  (if (< i N)
      (begin (view-set! ys i (line-elem (view-ref xs i) 1.5 -0.5)) (loop (+ i 1)))))

(define (fit-and-draw gf name truth ndraw)
  (let* ((kk 400)
         (cols (importance gf {:ys ys} kk 1))
         (ws (:weights cols))
         (picks (bytes-view (make-bytes (* ndraw 4)) :i32)))
    (normalize-weights! ws kk)
    (rng-fill-categorical! (rng-make 0 1 0) picks 0 ndraw ws)
    (set! captured #f)
    (plot-posterior! gf name cols picks truth)
    captured))

(define two (fit-and-draw (line xs 0.5 N) 'line-elem '(1.5 -0.5) 16))

(assert-equal "the parameter list comes from the model's own choices"
              '(:m :k) (curveplot-scalars (stage (line xs 0.5 N))))
(assert-true "a region is allocated per parameter"
             (and (string-contains? two "fn shared_m(") (string-contains? two "fn shared_k(")))
(assert-true "and one more for each parameter's true value"
             (string-contains? two "fn shared_truth_m("))
(assert-true "the curve is called with exactly its parameters, from this particle"
             (string-contains? two "line_elem(px_1, shared_m(k_3), shared_k(k_3))"))
(assert-true "the truth is read from index zero as a u32, not a float"
             (string-contains? two "line_elem(px_1, shared_truth_m(0u), shared_truth_k(0u))"))
(assert-true "the particles are looped per pixel, not drawn as geometry"
             (string-contains? two "for (var k_3 : u32 = 0u; k_3 < 16u;"))
(assert-true "and the stroke is normalised by its screen-space gradient"
             (string-contains? two "length(vec2<f32>(dpdx(g_5), dpdy(g_5)))"))

;;--- the same plotter, a wider family -----------------------------------
;; THE ASSERTION THIS LAYER EXISTS FOR. Nothing in lib/curveplot.scm
;; changed between these two; the model did.

(define-dual (cubic-elem (x :f32) (a :f32) (b :f32) (c :f32) (d :f32))
  (+ (* a x x x) (* b x x) (* c x) d))

(define-gen (cubic xs sigma npts)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5)))
         (d (at :d (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (cubic-elem (view-ref xs j) a b c d) sigma)))))

(define four (fit-and-draw (cubic xs 1.0 N) 'cubic-elem '(0.8 2.0 -1.0 0.5) 16))

(assert-equal "four choices are found, in source order"
              '(:a :b :c :d) (curveplot-scalars (stage (cubic xs 1.0 N))))
(assert-true "the call gained a fourth argument without the plotter being edited"
             (string-contains?
              four "cubic_elem(px_1, shared_a(k_3), shared_b(k_3), shared_c(k_3), shared_d(k_3))"))
(assert-true "and a fourth region with it"
             (string-contains? four "fn shared_d("))
;; The regions are laid out end to end, so the fourth starts after three
;; runs of NDRAW. Worth pinning because an offset computed in two places
;; is one that will eventually disagree with itself.
(assert-true "laid out consecutively, NDRAW apart"
             (string-contains? four "fn shared_d(k : u32) -> f32 { return sdata[48u + k]; }"))

;;--- the window is derived, not declared --------------------------------
;; lib/fitplot.scm fixes y to [-5, 14] and defends it: a curve that leaves
;; the frame IS the news. That argument holds for a FIXED family with a
;; varying fit, and stops holding when the family is the thing being
;; edited — a sine in a quadratic's window is not news, it is bad framing.
;; So this frames the particles instead, which keeps the argument and
;; drops its hard-coded instance.

(define (index-of hay needle)
  (let ((h (string-length hay)) (n (string-length needle)))
    (let loop ((k 0))
      (cond ((> (+ k n) h) #f)
            ((string=? (substring hay k (+ k n)) needle) k)
            (else (loop (+ k 1)))))))

(define (window-of src)
  ;; The py line reads "let py_2 : f32 = (<top> + (<-span> * uv.y));"
  (let ((i (index-of src "let py_2 : f32 = ("))
        (j (index-of src "* uv.y")))
    (substring src (+ i 18) j)))

(assert-true "a line and a cubic do not get the same frame"
             (not (string=? (window-of two) (window-of four))))
;; A line through these points stays inside about ±4; a cubic with a
;; quadratic term of 2 reaches far higher. If the frame were fixed, one of
;; the two would be wrong.
(assert-true "and the wider family gets the wider frame"
             (> (string-length (window-of four)) 4))

;;--- what it refuses ----------------------------------------------------

(assert-true "a curve that is declared but not dual is refused: the plot must
              evaluate it here too, to frame the picture"
             (guard (e (#t #t))
               (begin (wgsl-declare! 'device-only-curve "doc" '(:f32 :f32 :f32) :f32)
                      (fit-and-draw (line xs 0.5 N) 'device-only-curve '(1.0 1.0) 8)
                      #f)))
(wgsl-forget-declaration! 'device-only-curve)

(assert-true "and a truth that does not match the model's parameters"
             (guard (e (#t #t))
               (begin (fit-and-draw (line xs 0.5 N) 'line-elem '(1.0 1.0 1.0) 8) #f)))

(suite-summary)
