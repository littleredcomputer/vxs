;;----------------------------------------------------------------------
;; The heat ramp, in Scheme
;;
;; A twin of heat_colour in lib/colour.wgsl, for the CPU side. Actors
;; choose their own colour as they run, and they run in Scheme, so the same
;; ramp has to exist in both languages.
;;
;; That is a duplication, and duplication drifts. Layer 20 asserts that the
;; five control points here appear verbatim in the WGSL file, so a change
;; to one and not the other fails a test rather than producing two
;; different-looking demos that are each individually plausible.
;;
;; Stored FLAT — five stops of three channels — so that a channel can be
;; read without building an intermediate colour. That matters: the actors
;; call this several hundred times a frame, and a fresh 3-vector per call
;; would have been the single largest allocation source in the system.
;;----------------------------------------------------------------------

(define heat-stops
  [0.08 0.04 0.22      ; dim violet — cold, nearly spent
   0.48 0.06 0.32      ; magenta
   0.87 0.24 0.10      ; red-orange
   0.99 0.68 0.15      ; amber
   1.00 0.98 0.88])    ; white-hot

;; (heat-ref t k) -> channel k (0=r, 1=g, 2=b) of the ramp at t in [0,1].
;; Allocates nothing.
(define (heat-ref t k)
  (let* ((u (max 0.0 (min 1.0 t)))
         (s (* u 4.0))
         ;; min against 3 matters at exactly t = 1: without it the last
         ;; segment degenerates to its own lower endpoint and the hottest
         ;; value comes out amber rather than white. The WGSL twin has the
         ;; same guard, for the same reason.
         (i (min 3 (inexact->exact (floor s))))
         (f (- s i))
         (a (vector-ref heat-stops (+ (* i 3) k)))
         (b (vector-ref heat-stops (+ (* (+ i 1) 3) k))))
    (+ a (* (- b a) f))))

;; A fresh colour vector. Convenient, and allocates — so not for a hot loop.
(define (heat-colour t)
  (vector (heat-ref t 0) (heat-ref t 1) (heat-ref t 2)))
