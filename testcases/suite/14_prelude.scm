;;----------------------------------------------------------------------
;; Layer 14: the prelude — lib/prelude.scm, embedded and run at VM startup
;;
;; The prelude is the third home for a definition, alongside lib/*.scm
;; (needs `load`, so it cannot reach the wasm build) and the compiler
;; itself (reaches everywhere, but costs a rebuild of both targets to
;; change). It is embedded into the binary as a string at build time and
;; evaluated at the end of init_primitives, once every primitive exists.
;;
;; The rule for what goes where: a form earns a place in the C++ compiler
;; by needing binding structure or a temporary — that is where gensyms and
;; tail-position care are needed, and where the capture bugs in `case` and
;; `or` came from. `->` needs neither, so it lives here.
;;
;; This file checks both that the prelude's contents work and that the
;; properties claimed for them actually hold — single evaluation and tail
;; position in particular, since those are the reasons it was safe to
;; write these as macros rather than compiler forms.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "14_prelude: threading macros and the embedded standard library")

(define (sq x) (* x x))
(define (add a b) (+ a b))

;;--- thread-first --------------------------------------------------------

(assert-equal "-> threads into the first argument position"
              24 (-> 5 sq (add 1) (- 2)))          ; (- (add (sq 5) 1) 2)
(assert-equal "-> with a bare symbol calls it on one argument"
              1 (-> '(1 2 3) car))
(assert-equal "-> with an empty call form"
              25 (-> 5 (sq)))
(assert-equal "-> with no forms is the value itself" 7 (-> 7))
(assert-equal "-> chains bare symbols" 3 (-> '((3 4)) car car))

;;--- thread-last ---------------------------------------------------------

(assert-equal "->> threads into the last argument position"
              -24 (->> 5 sq (add 1) (- 2)))        ; (- 2 (add 1 (sq 5)))
(assert-equal "->> suits the sequence procedures"
              '(2 4 6)
              (->> '(1 2 3) (map (lambda (x) (* 2 x)))))
(assert-equal "->> with a bare symbol" 1 (->> '(1 2 3) car))

;; The two differ exactly where the argument lands, which is the whole
;; reason both exist.
(assert-equal "-> and ->> disagree on a non-commutative operator"
              '(8 2) (list (-> 10 (- 2)) (->> 10 (- 12))))

;;--- the properties that made these safe as macros ------------------------

;; Pure rearrangement means each subform appears ONCE. If the expansion
;; ever grows a temporary that is used twice, this catches it.
(define eval-count 0)
(define (bump!) (set! eval-count (+ eval-count 1)) 10)
(assert-equal "-> evaluates the threaded value once" 13 (-> (bump!) (add 3)))
(assert-equal "and only once" 1 eval-count)

(define eval-count2 0)
(define (bump2!) (set! eval-count2 (+ eval-count2 1)) 10)
(assert-equal "->> evaluates the threaded value once" 13 (->> (bump2!) (add 3)))
(assert-equal "and only once" 1 eval-count2)

;; Threading into an ordinary call should leave tail position intact — it
;; is a plain rearrangement, so the normal compiler path handles it. If
;; that were wrong this blows the stack rather than failing an assertion.
(define (countdown n) (if (= n 0) 'done (-> n (- 1) countdown)))
(assert-equal "-> preserves tail position" 'done (countdown 200000))
;; Note the (+ -1): threaded LAST, (- 1) would expand to (- 1 n), which
;; counts away from zero rather than toward it. The first draft of this
;; test used it and looped forever — the two macros are not
;; interchangeable, which is the point of having both.
(define (countdown* n) (if (= n 0) 'done (->> n (+ -1) countdown*)))
(assert-equal "->> preserves tail position" 'done (countdown* 100000))

;; No temporary means nothing to capture, so a user variable with any name
;; at all passes through untouched — the failure mode `case` and `or` had.
(assert-equal "-> captures nothing" 12
              (let ((acc 2) (x 10)) (-> x (add acc))))
(assert-equal "nesting -> inside -> is fine" 30
              (-> 5 (add (-> 10 (add 10))) (add 5)))
(assert-equal "-> and ->> nest together" 7
              (-> 3 (add (->> 1 (add 3)))))

;;--- the rest of the prelude ---------------------------------------------

(assert-equal "force is defined" 3 (force (delay (+ 1 2))))
(assert-equal "force on a non-procedure is the identity" 4 (force 4))

;; force must test is_closure, not `procedure?`. The latter is broad here —
;; vectors, maps and keywords are all callable — so a Scheme force written
;; as (if (procedure? p) (p) p) CALLS a vector instead of returning it.
;; Such a definition existed as dead code in init_primitives for a long
;; time, overridden by the C++ primitive; when the prelude moved to the end
;; of initialization it briefly won, and this is the assertion that caught
;; it. It returned () here.
(assert-equal "force returns a callable non-closure unchanged"
              (vector 1 2 3) (force (vector 1 2 3)))
(assert-equal "force leaves a list alone" '(1 2) (force '(1 2)))

(assert-equal "with-output-to-string captures output"
              "hello"
              (with-output-to-string (lambda () (display "hello"))))
(assert-equal "call-with-output-string captures output"
              "42"
              (call-with-output-string (lambda (p) (display 42 p))))

;; assert is a macro so the failing EXPRESSION reaches the error, not just
;; its false value. Here we only check the passing path plus that it is
;; bound at all — the message shape is exercised by the conditions layer.
(assert-equal "assert passes a true expression" #t
              (begin (assert (= 1 1)) #t))


;;--- case-lambda --------------------------------------------------------
;; R7RS-small, and wanted here for a specific reason rather than
;; conformance: a rest argument is PERMISSIVE. (lambda (op . rest) ...)
;; accepts any count and silently drops the extras, so a caller passing
;; one argument too many gets no complaint. case-lambda names the shapes
;; it accepts and refuses the rest.

(define cl2 (case-lambda ((a) (list 'one a)) ((a b) (list 'two a b))))
(assert-equal "dispatches on one argument"  '(one 9)   (cl2 9))
(assert-equal "and on two"                  '(two 9 8) (cl2 9 8))
(assert-equal "and REFUSES a count no clause declares"
              'raised (guard (e (#t 'raised)) (cl2 1 2 3)))

;; The refusal is the whole point, so contrast it with what a rest
;; argument does with the same call.
(define permissive (lambda (a . rest) (list 'took a)))
(assert-equal "where a rest argument would have accepted it silently"
              '(took 1) (permissive 1 2 3))

(define cl-rest (case-lambda (() 'none) ((a . r) (list a r))))
(assert-equal "an empty clause matches no arguments" 'none (cl-rest))
(assert-equal "a rest clause matches one"      '(1 ())    (cl-rest 1))
(assert-equal "and matches more"               '(1 (2 3)) (cl-rest 1 2 3))

(define cl-any (case-lambda ((a) 'exactly-one) (whatever (list 'any whatever))))
(assert-equal "a bare symbol formal accepts anything"
              '(any (1 2 3)) (cl-any 1 2 3))
(assert-equal "but an earlier specific clause still wins"
              'exactly-one (cl-any 1))

;; Expansion uses explicit gensyms, not the sym# convention: auto-gensym
;; rewrites within one backquote template and the clause tests are built
;; by a nested one, so sym# would bind and reference different symbols.
;; This asserts the macro can be used twice in one scope without the two
;; expansions colliding.
(define cl-a (case-lambda ((x) (* x 2))))
(define cl-b (case-lambda ((x) (* x 3))))
(assert-equal "two expansions in one scope do not collide"
              '(10 15) (list (cl-a 5) (cl-b 5)))

(suite-summary)
