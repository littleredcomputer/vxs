;;----------------------------------------------------------------------
;; Threefry-4x32 — counter-based random numbers
;;
;; Random123 (Salmon, Moraes, Dror & Shaw, SC'11). A counter-based RNG is
;; a stateless function of (counter, key) rather than a stream with hidden
;; state: the nth value costs the same as the first, and independent
;; elements can draw independent randomness with no coordination. That is
;; what makes it usable per-point in a wrangle, and later per-invocation
;; in WGSL, where a shared mutable stream would be either a race or a
;; serialization point.
;;
;; Threefry rather than Philox because it is built from add / xor / rotate
;; only — no 32x32->64 high multiply, which WGSL cannot express cheaply.
;; 13 rounds is the Random123 authors' recommended reduced-strength
;; variant: it passes BigCrush with margin, at roughly two thirds the cost
;; of the conservative 20. JAX made the same choice.
;;
;; Built on the u32 layer (see testcases/suite/12_u32.scm). Note that
;; every u32 value above 2^31 is a FLONUM in this system, since exact
;; integers here are 32-bit signed; the u32 ops mask correctly on both
;; representations, so that stays invisible except when printing.
;;
;; Verified against the published known-answer vectors from the Random123
;; distribution — see testcases/suite/13_threefry.scm, which carries them
;; verbatim in hex.
;;
;; Allocation note: each call allocates the 5-word key schedule and the
;; 4-word result. The round state itself is register-resident (inlined
;; `let*`, no closure per scope). Worth revisiting only if this lands in
;; a hot host-side loop; the GPU port has no such cost.
;;----------------------------------------------------------------------

;; Skein key-schedule parity constant, 0x1BD11BDA.
(define threefry-parity 466688986)

;; Rotation constants, 8 pairs indexed by round mod 8, stored flat.
(define threefry-rot
  (vector 10 26  11 21  13 27  23 5  6 20  17 11  25 10  18 20))

;; The recommended reduced-strength round count.
(define threefry-default-rounds 13)

;; (threefry4x32/rounds ctr key rounds) -> 4-vector of u32
;; ctr and key are 4-vectors of u32. Pure: same inputs, same outputs.
(define (threefry4x32/rounds ctr key rounds)
  (let* ((k0 (vector-ref key 0)) (k1 (vector-ref key 1))
         (k2 (vector-ref key 2)) (k3 (vector-ref key 3))
         (k4 (u32-xor threefry-parity k0 k1 k2 k3))
         (ks (vector k0 k1 k2 k3 k4)))
    (let loop ((r 0)
               (x0 (u32+ (vector-ref ctr 0) k0))
               (x1 (u32+ (vector-ref ctr 1) k1))
               (x2 (u32+ (vector-ref ctr 2) k2))
               (x3 (u32+ (vector-ref ctr 3) k3)))
      ;; The key is re-injected every fourth round. Doing it at the TOP of
      ;; round r (rather than after round r-1) makes the final injection
      ;; fall out for free when `rounds` is itself a multiple of four —
      ;; that trailing injection is easy to drop, and it is the difference
      ;; between matching the published vectors at 20 rounds and not.
      (let* ((s       (quotient r 4))
             (inject? (and (> r 0) (= 0 (modulo r 4))))
             (y0 (if inject? (u32+ x0 (vector-ref ks (modulo s 5))) x0))
             (y1 (if inject? (u32+ x1 (vector-ref ks (modulo (+ s 1) 5))) x1))
             (y2 (if inject? (u32+ x2 (vector-ref ks (modulo (+ s 2) 5))) x2))
             (y3 (if inject? (u32+ x3 (vector-ref ks (modulo (+ s 3) 5)) s) x3)))
        (if (= r rounds)
            (vector y0 y1 y2 y3)
            (let* ((i (* 2 (modulo r 8)))
                   (p (vector-ref threefry-rot i))
                   (q (vector-ref threefry-rot (+ i 1)))
                   (even-round? (= 0 (modulo r 2)))
                   ;; Even rounds mix the pairs (0,1) and (2,3); odd rounds
                   ;; mix (0,3) and (2,1). Each half is add, rotate the OLD
                   ;; value, xor against the NEW sum.
                   (z0 (if even-round? (u32+ y0 y1) (u32+ y0 y3)))
                   (z2 (if even-round? (u32+ y2 y3) (u32+ y2 y1)))
                   (z1 (if even-round?
                           (u32-xor (u32-rotl y1 p) z0)
                           (u32-xor (u32-rotl y1 q) z2)))
                   (z3 (if even-round?
                           (u32-xor (u32-rotl y3 q) z2)
                           (u32-xor (u32-rotl y3 p) z0))))
              (loop (+ r 1) z0 z1 z2 z3)))))))

;; The variant we standardize on.
(define (threefry4x32 ctr key)
  (threefry4x32/rounds ctr key threefry-default-rounds))

;; u32 -> double in [0,1). Divides by 2^32, so 0xFFFFFFFF maps just below
;; one and never reaches it.
(define (u32->unit u) (/ u 4294967296.0))

;; One Threefry block as four independent doubles in [0,1) — the shape a
;; per-point wrangle actually consumes.
(define (threefry4x32-unit ctr key)
  (let ((w (threefry4x32 ctr key)))
    (vector (u32->unit (vector-ref w 0))
            (u32->unit (vector-ref w 1))
            (u32->unit (vector-ref w 2))
            (u32->unit (vector-ref w 3)))))
