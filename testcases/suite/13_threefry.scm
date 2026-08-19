;;----------------------------------------------------------------------
;; Layer 13: Threefry-4x32 counter-based RNG
;;
;; The implementation is in lib/threefry.scm. This file is what makes it
;; trustworthy: the known-answer vectors published with the Random123
;; distribution, carried here VERBATIM IN HEX so they can be compared
;; against that file by eye. Both sides of every KAT assertion are hex
;; strings for the same reason — a decimal expectation is a transcription
;; waiting to go wrong, and it did: the first draft of this file failed
;; all nine vectors because 0x531c7e4f had been hand-converted to
;; 1394191951 instead of 1394376271. The implementation was correct the
;; whole time. Same lesson as layer 12, learned again the same way.
;;
;; Nine vectors: three counter/key pairs (all-zero, all-ones, and digits
;; of pi) at three round counts. The 13-round set is the variant vxs
;; standardizes on; 20 and 72 exercise the identical round loop and key
;; schedule with different injection counts, which is what pins down the
;; trailing key injection that 13 rounds never performs.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/threefry.scm")

(test-suite "13_threefry: counter-based RNG against published vectors")

;; Hex in, hex out — the vectors never pass through decimal.
(define (hex-block a b c d)
  (vector (string->number a 16) (string->number b 16)
          (string->number c 16) (string->number d 16)))

(define (pad8 s)
  (if (>= (string-length s) 8) s (pad8 (string-append "0" s))))

(define (block->hex v)
  (list (pad8 (number->string (vector-ref v 0) 16))
        (pad8 (number->string (vector-ref v 1) 16))
        (pad8 (number->string (vector-ref v 2) 16))
        (pad8 (number->string (vector-ref v 3) 16))))

;; Guard the harness itself: if hex formatting regressed, every KAT below
;; would fail in a way that looks like an RNG bug.
(assert-equal "hex helper formats a full-width word" '("ffffffff")
              (list (pad8 (number->string 4294967295 16))))
(assert-equal "hex helper zero-pads" '("0000002a")
              (list (pad8 (number->string 42 16))))

(define ctr-zero (hex-block "00000000" "00000000" "00000000" "00000000"))
(define key-zero (hex-block "00000000" "00000000" "00000000" "00000000"))
(define ctr-ones (hex-block "ffffffff" "ffffffff" "ffffffff" "ffffffff"))
(define key-ones (hex-block "ffffffff" "ffffffff" "ffffffff" "ffffffff"))
(define ctr-pi   (hex-block "243f6a88" "85a308d3" "13198a2e" "03707344"))
(define key-pi   (hex-block "a4093822" "299f31d0" "082efa98" "ec4e6c89"))

;;--- 13 rounds: the variant we standardize on --------------------------

(assert-equal "threefry4x32-13, ctr=0 key=0"
              '("531c7e4f" "39491ee5" "2c855a92" "3d6abf9a")
              (block->hex (threefry4x32/rounds ctr-zero key-zero 13)))
(assert-equal "threefry4x32-13, ctr=~0 key=~0"
              '("c4189358" "1c9cc83a" "d5881c67" "6a0a89e0")
              (block->hex (threefry4x32/rounds ctr-ones key-ones 13)))
(assert-equal "threefry4x32-13, ctr/key from pi"
              '("4aa71d8f" "734738c2" "431fc6a8" "ae6debf1")
              (block->hex (threefry4x32/rounds ctr-pi key-pi 13)))

;;--- 20 rounds: adds a trailing key injection 13 never reaches ---------

(assert-equal "threefry4x32-20, ctr=0 key=0"
              '("9c6ca96a" "e17eae66" "fc10ecd4" "5256a7d8")
              (block->hex (threefry4x32/rounds ctr-zero key-zero 20)))
(assert-equal "threefry4x32-20, ctr=~0 key=~0"
              '("2a881696" "57012287" "f6c7446e" "a16a6732")
              (block->hex (threefry4x32/rounds ctr-ones key-ones 20)))
(assert-equal "threefry4x32-20, ctr/key from pi"
              '("59cd1dbb" "b8879579" "86b5d00c" "ac8b6d84")
              (block->hex (threefry4x32/rounds ctr-pi key-pi 20)))

;;--- 72 rounds: eighteen injections, key schedule wraps mod 5 ----------

(assert-equal "threefry4x32-72, ctr=0 key=0"
              '("93171da6" "9220326d" "b392b7b1" "ff58a002")
              (block->hex (threefry4x32/rounds ctr-zero key-zero 72)))
(assert-equal "threefry4x32-72, ctr=~0 key=~0"
              '("60743f3d" "9961e684" "aab21c34" "8c65fb7d")
              (block->hex (threefry4x32/rounds ctr-ones key-ones 72)))
(assert-equal "threefry4x32-72, ctr/key from pi"
              '("09930adf" "7f27bd55" "9ed68ce1" "97f803f6")
              (block->hex (threefry4x32/rounds ctr-pi key-pi 72)))

;;--- the default entry point -------------------------------------------

(assert-equal "threefry4x32 defaults to 13 rounds"
              '("4aa71d8f" "734738c2" "431fc6a8" "ae6debf1")
              (block->hex (threefry4x32 ctr-pi key-pi)))
(assert-equal "the default round count is 13" 13 threefry-default-rounds)

;;--- properties the vectors alone do not pin down ----------------------

;; Statelessness is the whole point: two calls with the same arguments
;; must agree, with no hidden stream position between them.
(assert-equal "the same counter and key always give the same block"
              (block->hex (threefry4x32 ctr-pi key-pi))
              (block->hex (threefry4x32 ctr-pi key-pi)))

;; Independent draws come from moving the counter, not from sequencing.
(define (ctr-at n) (vector n 0 0 0))
(assert-equal "adjacent counters give unrelated blocks" #f
              (equal? (block->hex (threefry4x32 (ctr-at 0) key-zero))
                      (block->hex (threefry4x32 (ctr-at 1) key-zero))))
(assert-equal "counter 0 with different keys differs" #f
              (equal? (block->hex (threefry4x32 ctr-zero key-zero))
                      (block->hex (threefry4x32 ctr-zero (vector 1 0 0 0)))))

;; Avalanche: flipping ONE counter bit should change about half of the 128
;; output bits. A structural mistake — a dropped round, a rotation applied
;; to the wrong word, a key injection that never fires — shows up here as
;; a count far outside the band, even when it happens to pass a weak
;; "outputs differ" check.
(define (u32-popcount x)
  (let loop ((x x) (n 0))
    (if (= x 0) n (loop (u32-shr x 1) (+ n (u32-and x 1))))))

(define (block-diff-bits a b)
  (let loop ((i 0) (n 0))
    (if (= i 4)
        n
        (loop (+ i 1)
              (+ n (u32-popcount (u32-xor (vector-ref a i) (vector-ref b i))))))))

(define avalanche
  (block-diff-bits (threefry4x32 (ctr-at 0) key-zero)
                   (threefry4x32 (ctr-at 1) key-zero)))
(assert-equal "one counter bit flips roughly half of 128 output bits" #t
              (and (> avalanche 40) (< avalanche 88)))

(define key-avalanche
  (block-diff-bits (threefry4x32 ctr-zero key-zero)
                   (threefry4x32 ctr-zero (vector 1 0 0 0))))
(assert-equal "one key bit does the same" #t
              (and (> key-avalanche 40) (< key-avalanche 88)))

;; popcount itself, since the two assertions above lean on it.
(assert-equal "popcount of 0xFFFFFFFF" 32 (u32-popcount 4294967295))
(assert-equal "popcount of 0" 0 (u32-popcount 0))
(assert-equal "popcount of 0x80000000" 1 (u32-popcount 2147483648))

;;--- conversion to unit doubles ----------------------------------------

(assert-equal "zero maps to zero" 0.0 (u32->unit 0))
(assert-equal "the largest u32 stays below one" #t (< (u32->unit 4294967295) 1.0))
(assert-equal "and is close to one" #t (> (u32->unit 4294967295) 0.9999999))
(assert-equal "half scale" 0.5 (u32->unit 2147483648))

(define units (threefry4x32-unit ctr-pi key-pi))
(define (all-in-unit-range? v)
  (let loop ((i 0))
    (cond ((= i (vector-length v)) #t)
          ((and (>= (vector-ref v i) 0.0) (< (vector-ref v i) 1.0)) (loop (+ i 1)))
          (else #f))))
(assert-equal "a unit block gives four values in [0,1)" #t
              (all-in-unit-range? units))
(assert-equal "a unit block has four entries" 4 (vector-length units))

(suite-summary)
