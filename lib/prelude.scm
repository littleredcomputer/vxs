;;----------------------------------------------------------------------
;; vxs prelude — the standard library that ships inside the VM
;;
;; This file is EMBEDDED into the binary at build time (the Makefile
;; generates src/vx_prelude.h from it) and evaluated at the end of
;; VM::init_primitives, once every primitive exists. That placement is
;; what makes it different from lib/*.scm: those need `load`, which the
;; wasm build does not have, so they cannot reach the browser. The
;; prelude reaches every host — native, AOT, and wasm — without one.
;;
;; It is also the alternative to baking things into the compiler in C++.
;; A form earns a place in vx_compiler.h by needing binding structure or
;; a temporary — that is where gensyms and tail-position care are needed,
;; and where the capture bugs in `case` and `or` came from. Anything that
;; is a plain rearrangement or an ordinary procedure belongs here, in
;; Scheme, where it can be read and changed without a rebuild of both the
;; native and wasm targets.
;;
;; Pass --no-prelude to run on the bare kernel: primitives and compiler
;; special forms only, nothing defined below. Useful for measuring what
;; the prelude costs at startup, and for confirming that nothing in the
;; kernel has quietly grown a dependency on it.
;;----------------------------------------------------------------------

;; `force` is deliberately NOT here. It was a bootstrap snippet in
;; init_primitives reading (if (procedure? p) (p) p), placed BEFORE the C++
;; def_global("force", ...) that then silently overrode it — so it never
;; took effect, and moving the prelude to the end of init_primitives is
;; what exposed that. It should stay overridden: `procedure?` in vxs is
;; broad (vectors, maps and keywords are callable), so the Scheme version
;; calls anything callable rather than returning it, and
;; (force (vector 1 2 3)) evaluates to () instead of the vector. The C++
;; primitive tests is_closure, which is the right question. See layer 14.

;; assert as a macro rather than a procedure so the failing EXPRESSION —
;; not just its runtime-false value — appears in the error, the way most
;; Lisps' assert does. Not an R7RS procedure (R7RS doesn't standardize one
;; at all), but common enough across Schemes to be worth having in the
;; "things people reflexively reach for" sense.
(defmacro (assert expr)
  `(if (not ,expr) (error 'assert "assertion failed:" ',expr)))

;;--- threading macros ---------------------------------------------------
;; Clojure's -> and ->>. Pure syntactic rearrangement: every subform
;; appears exactly once, in source order, evaluated once, so there is no
;; temporary to capture and nothing for hygiene to get wrong. Threading
;; into an ordinary call also means tail position falls out of the normal
;; compiler path for free.
;;
;;   (-> x (f a) (g b))   =>  (g (f x a) b)      ; thread FIRST
;;   (->> x (f a) (g b))  =>  (g b (f a x))      ; thread LAST
;;
;; A bare symbol threads as a one-argument call, so (-> x f g) is (g (f x)).
;;
;; Both expand fully in a single macro call rather than re-emitting
;; themselves one step at a time — same result, one expansion instead of n,
;; and no dependence on the expander re-walking its own output.
;;
;; Note these are not the same thing as a `pipe` combinator, and should not
;; be merged with one: `pipe` returns a PROCEDURE you can pass around and
;; switch on, which is what makes a node graph compositional. -> is syntax
;; that applies immediately.

(defmacro (-> x . forms)
  (let loop ((acc x) (fs forms))
    (if (null? fs)
        acc
        (loop (let ((f (car fs)))
                (if (pair? f)
                    (cons (car f) (cons acc (cdr f)))
                    (list f acc)))
              (cdr fs)))))

(defmacro (->> x . forms)
  (let loop ((acc x) (fs forms))
    (if (null? fs)
        acc
        (loop (let ((f (car fs)))
                (if (pair? f)
                    (append f (list acc))
                    (list f acc)))
              (cdr fs)))))

;;--- case-lambda --------------------------------------------------------
;; R7RS-small (originally SRFI 16, also in R6RS). A procedure with several
;; declared arities, dispatching on how many arguments it was given.
;;
;; The reason to want it over a rest argument is that a rest argument is
;; PERMISSIVE: (lambda (op . rest) ...) accepts anything and silently
;; ignores the extras. case-lambda names the shapes it accepts and refuses
;; the rest, which is the difference between an interface and a suggestion.
;;
;; Clauses are tried in order, so put the specific ones first — a clause
;; with a rest argument accepts every larger count and would shadow
;; anything after it.
;;
;; EXPLICIT GENSYMS rather than the sym# convention, and it is not a
;; stylistic choice: auto-gensym rewrites within ONE backquote template,
;; and the clause tests below are built by a second, nested one. Written
;; as n#, the binding and the references would land on different symbols —
;; which fails as an unbound-variable error at expansion, loudly, but only
;; once someone calls the macro.
(defmacro (case-lambda . clauses)
  (let ((args (gensym)) (n (gensym)))
    `(lambda ,args
       (let ((,n (length ,args)))
         (cond
           ,@(map (lambda (clause)
                    (let* ((formals (car clause))
                           (body    (cdr clause))
                           ;; (a b) accepts exactly 2; (a . r) accepts 1 or
                           ;; more; a bare symbol accepts anything.
                           (spec (let loop ((f formals) (k 0))
                                   (cond ((null? f)   (list '= k))
                                         ((symbol? f) (list '>= k))
                                         (else (loop (cdr f) (+ k 1)))))))
                      (list (list (car spec) n (cadr spec))
                            (list 'apply (cons 'lambda (cons formals body)) args))))
                  clauses)
           (else (error "case-lambda: no clause accepts this many arguments" ,n)))))))

;;--- file and string port wrappers --------------------------------------
;; These are Scheme rather than C++ subrs because unwind-protect provides
;; exactly the restore-on-every-exit guarantee they need, so the C++-RAII
;; version of that guarantee (ScopedOutPortRebind/ScopedPortCloser) could
;; go: the language has the feature natively. open-*-file still returns #f
;; on failure; these friendly wrappers signal a clear error instead of
;; silently returning unspecified, per the "no silent failures" policy.

(define (with-output-to-file filename thunk)
  (let ((port (open-output-file filename)))
    (if (not port)
        (error 'with-output-to-file "could not open file:" filename)
        (let ((prev (current-output-port)))
          (%set-current-output-port! port)
          (unwind-protect (thunk)
            (%set-current-output-port! prev)
            (close-output-port port))))))

;; Same shape as with-output-to-file: rebind, run, restore under
;; unwind-protect so an escape or a raise still restores the port. No close
;; needed — a string port owns no OS resource.
(define (with-output-to-string thunk)
  (let ((port (open-output-string))
        (prev (current-output-port)))
    (%set-current-output-port! port)
    (unwind-protect (begin (thunk) (get-output-string port))
      (%set-current-output-port! prev))))

(define (call-with-output-string proc)
  (let ((port (open-output-string)))
    (proc port)
    (get-output-string port)))

(define (call-with-input-file filename proc)
  (let ((port (open-input-file filename)))
    (if (not port)
        (error 'call-with-input-file "could not open file:" filename)
        (unwind-protect (proc port) (close-input-port port)))))

(define (call-with-output-file filename proc)
  (let ((port (open-output-file filename)))
    (if (not port)
        (error 'call-with-output-file "could not open file:" filename)
        (unwind-protect (proc port) (close-output-port port)))))
