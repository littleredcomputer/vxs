;;; REPRODUCTION: a scan model's staged log-joint loses its state bindings
;;; under sustained allocation.
;;;
;;; NOT part of the suite. It fails, and where it fails moves, which is the
;;; point: run it a few times and the iteration number changes.
;;;
;;; SYMPTOM
;;;   [stage "unbound index" th]   (or om)
;;;
;;; raised from lib/stage.scm's `staged-value`, which means (assq 'th idx)
;;; returned #f for an `idx` that lib/stage.scm's scan-over branch had just
;;; built and was still using. A live association list became unreachable.
;;;
;;; WHY IT LOOKS LIKE THE COLLECTOR RATHER THAN THE READER
;;;   - The same staged object, the same choices and the same seed reach
;;;     different iterations on different runs: 5, 23, 40 and 108 have all
;;;     been seen.
;;;   - Which variants fail CHANGES when unrelated code before them
;;;     changes, because that shifts the allocation history. Any "control
;;;     that passes" here is passing on timing.
;;;   - `staged-logpdf` called repeatedly on ONE map is stable for
;;;     thousands of iterations; so is `assess`. What is not stable is a
;;;     loop that keeps allocating between calls.
;;;
;;; WHY THE SUITE DOES NOT CATCH IT. Layer 25 stages this very pendulum and
;;; compares it against assess bit-for-bit — four times, at four fixed
;;; values of w. Four calls never allocate enough to collect.
;;;
;;; WHERE TO LOOK. lib/stage.scm's scan-over branch builds
;;;
;;;   (cons (cons name j) (map cons names s))
;;;
;;; and holds it across `staged-term`, which calls a dual's SCHEME half
;;; through `staged-procedure`. The RK4 bodies below allocate heavily (a
;;; seven-binding let* with four `sin` calls, twice per step, 24 steps), so
;;; a collection is very likely to land inside one — with that alist live
;;; only from the interpreter's frame.

(load "lib/mh.scm")

(define-dual (rk4-theta (th :f32) (om :f32) (w :f32) (h :f32))
  (let* ((k1t om)                   (k1o (* (- 0.0 (* w w)) (sin th)))
         (k2t (+ om (* 0.5 h k1o))) (k2o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k1t)))))
         (k3t (+ om (* 0.5 h k2o))) (k3o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k2t)))))
         (k4t (+ om (* h k3o))))
    (+ th (* (/ h 6.0) (+ k1t (* 2.0 k2t) (* 2.0 k3t) k4t)))))

(define-dual (rk4-omega (th :f32) (om :f32) (w :f32) (h :f32))
  (let* ((k1t om)                   (k1o (* (- 0.0 (* w w)) (sin th)))
         (k2t (+ om (* 0.5 h k1o))) (k2o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k1t)))))
         (k3t (+ om (* 0.5 h k2o))) (k3o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k2t)))))
         (k4o (* (- 0.0 (* w w)) (sin (+ th (* h k3t))))))
    (+ om (* (/ h 6.0) (+ k1o (* 2.0 k2o) (* 2.0 k3o) k4o)))))

(define NOBS 24)
(define ys (bytes-view (make-bytes (* NOBS 8)) :f64))

(define-gen (pendulum sigma nobs th0 h)
  (let ((w (at :w (uniform 0.5 4.0))))
    (at :ys (scan-i nobs (j)
              ((th th0 (rk4-theta th om w h))
               (om 0.0 (rk4-omega th om w h)))
              (normal th sigma)))))

(define st (stage (pendulum 0.05 NOBS 1.0 0.05)))

;; A chain, which is the shape that allocates between calls: each sweep
;; copies a map, draws, and scores twice.
(define (attempt seed limit)
  (let ((r (rng-make 0 seed 0)))
    (let loop ((i 0) (ch {:w 1.0 :ys ys}))
      (if (= i limit)
          (list 'survived limit)
          (let ((res (guard (e (#t (list 'failed 'at i e)))
                       (mh-sweep st ch 0.05 r))))
            (if (and (pair? res) (eq? (car res) 'failed))
                res
                (loop (+ i 1) (car res))))))))

(display "the staged log-joint agrees with assess, as layer 25 asserts: ")
(display (< (abs (- (car (assess (pendulum 0.05 NOBS 1.0 0.05) {:w 2.0 :ys ys}))
                    (staged-logpdf st {:w 2.0 :ys ys})))
            1e-12))
(newline)
(display "so the READING is right; what follows is the collector.") (newline)
(newline)

(for-each (lambda (seed)
            (display "  seed ") (display seed) (display ": ")
            (display (attempt seed 3000)) (newline))
          (list 4242 7 99 12345))
