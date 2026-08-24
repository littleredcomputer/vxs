;;----------------------------------------------------------------------
;; Gradient noise, host side
;;
;; The same construction as lib/noise.wgsl: the gradient at a lattice
;; point is one Threefry block with that point's integer coordinates as
;; the counter. No permutation table, no invented hash, and the generator
;; underneath is the one checked against published known-answer vectors.
;;
;; NOT BIT-IDENTICAL to the WGSL version, and not trying to be. That one
;; builds its unit floats by pasting bits into a mantissa; this one
;; divides. They agree to about 2^-24, which is far below anything a
;; decision depends on — but it does mean the two must not be used to draw
;; the same picture, only to make the same kind of decision.
;;
;; COST. About 70 microseconds per evaluation: eight Threefry blocks, each
;; thirteen rounds of u32 arithmetic through the VM. Ninety-six of them is
;; two thirds of a frame, so anything sampling per-agent per-frame wants to
;; stagger — see the ensemble demo, where each actor senses on its own
;; schedule and the cost comes out flat.
;;----------------------------------------------------------------------

(load "lib/threefry.scm")

(define (noise-u32-of-i32 n) (if (< n 0) (+ n 4294967296) n))

;; A unit vector, uniform on the sphere: z uniform on [-1,1] and the angle
;; uniform, by Archimedes' theorem. Two uniform angles would crowd the
;; poles and give the noise a visible axis.
(define (noise-gradient ix iy iz seed)
  (let* ((b (threefry4x32 (vector (noise-u32-of-i32 ix)
                                  (noise-u32-of-i32 iy)
                                  (noise-u32-of-i32 iz) 0)
                          (vector seed 0 0 0)))
         (z (- (* 2.0 (u32->unit (vector-ref b 0))) 1.0))
         (a (* 6.28318530718 (u32->unit (vector-ref b 1))))
         (r (sqrt (max 0.0 (- 1.0 (* z z))))))
    (list (* r (cos a)) (* r (sin a)) z)))

;; Perlin's quintic fade. Its first AND second derivatives vanish at both
;; ends, which is what stops the lattice showing up as creases; the cubic
;; smoothstep does not have that property.
(define (noise-fade t) (* t t t (+ (* t (- (* t 6.0) 15.0)) 10.0)))

;; Gradient noise at (x y z). Roughly [-0.6, 0.6] — NOT [-1, 1], measured
;; over 64000 samples — and exactly 0.0 at every integer lattice point,
;; which is the defining property and the sharpest test of it.
(define (perlin3 x y z seed)
  (let* ((fx (floor x)) (fy (floor y)) (fz (floor z))
         (ix (inexact->exact fx)) (iy (inexact->exact fy)) (iz (inexact->exact fz))
         (dx (- x fx)) (dy (- y fy)) (dz (- z fz))
         (ux (noise-fade dx)) (uy (noise-fade dy)) (uz (noise-fade dz)))
    (let loop ((c 0) (acc 0.0))
      (if (= c 8)
          acc
          (let* ((cx (modulo c 2))
                 (cy (modulo (quotient c 2) 2))
                 (cz (quotient c 4))
                 (g (noise-gradient (+ ix cx) (+ iy cy) (+ iz cz) seed))
                 (wx (if (= cx 1) ux (- 1.0 ux)))
                 (wy (if (= cy 1) uy (- 1.0 uy)))
                 (wz (if (= cz 1) uz (- 1.0 uz))))
            (loop (+ c 1)
                  (+ acc (* wx wy wz
                            (+ (* (car g)   (- dx cx))
                               (* (cadr g)  (- dy cy))
                               (* (caddr g) (- dz cz)))))))))))
