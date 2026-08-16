;;----------------------------------------------------------------------
;; Layer 02: Reader Syntax & Macro System
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "02_reader_macros: Reader Syntax & Metaprogramming")

;; 1. Reader Literals & Escapes
(assert-equal "character literal alpha" #\x (string-ref "x" 0))
(assert-equal "character newline" #\newline (string-ref "\n" 0))
(assert-equal "character space" #\space (string-ref " " 0))
(assert-equal "character tab" #\tab (string-ref "\t" 0))

(assert-equal "string with escaped quotes" "hello \"world\"" (string-append "hello " "\"world\""))
(assert-equal "string with escaped backslash" "a\\b" (string-append "a" "\\" "b"))
(assert-equal "string with newline escape" "line1\nline2" (string-append "line1\n" "line2"))

;; Dotted pair notation
(assert-equal "dotted pair" '(a . b) (cons 'a 'b))
(assert-equal "improper list" '(1 2 . 3) (cons 1 (cons 2 3)))
(assert-equal "nested dotted pairs" '((1 . 2) . (3 . 4)) (cons (cons 1 2) (cons 3 4)))

;; Bracketed vector & map literals
(assert-equal "bracket vector literal" (vector 1 2 3) [1 2 3])
(assert-equal "nested bracket vectors" [[1 2] [3 4]] (vector (vector 1 2) (vector 3 4)))
(assert-equal "keyword literal" :test (string->keyword "test"))

;; Extended symbol characters
(define ++ 100)
(define --> 200)
(define ?why 300)
(define !bang 400)
(assert-equal "symbol ++" 100 ++)
(assert-equal "symbol -->" 200 -->)
(assert-equal "symbol ?why" 300 ?why)
(assert-equal "symbol !bang" 400 !bang)

;; 2. Quasiquotation & Splicing
(assert-equal "quasiquote literal" '(1 2 3) `(1 2 3))
(assert-equal "quasiquote with unquote" '(1 5 6) `(1 ,(+ 2 3) 6))
(assert-equal "quasiquote with unquote-splicing" '(1 2 3 4) `(1 ,@(list 2 3) 4))
(assert-equal "quasiquote splicing at start" '(10 20 30 40) `(,@(list 10 20) 30 40))
(assert-equal "quasiquote splicing at end" '(1 2 3 4) `(1 2 ,@(list 3 4)))
(assert-equal "quasiquote vector with unquote" [1 5 3] `[1 ,(+ 2 3) 3])

;; 3. Procedural Defmacro
(defmacro (infix a op b)
  (list op a b))

(assert-equal "defmacro infix addition" 30 (infix 10 + 20))
(assert-equal "defmacro infix multiplication" 200 (infix 10 * 20))

(defmacro (my-when test . body)
  `(if ,test (begin ,@body)))

(assert-equal "my-when true branch" 99 (my-when #t 1 2 99))
(assert-equal "my-when false branch" (void) (my-when #f 1 2 99))

(defmacro (my-unless test . body)
  `(if (not ,test) (begin ,@body)))

(assert-equal "my-unless false branch" 'executed (my-unless #f 'executed))
(assert-equal "my-unless true branch" (void) (my-unless #t 'executed))

;; Auto-gensym hygiene (sym# syntax)
(defmacro (swap! a b)
  `(let ((tmp# ,a))
     (set! ,a ,b)
     (set! ,b tmp#)))

(let ((x 111) (y 222))
  (swap! x y)
  (assert-equal "swap! hygienic macro x" 222 x)
  (assert-equal "swap! hygienic macro y" 111 y))

;; Macro Introspection
(assert-equal "macroexpand-1 on infix" '(+ 10 20) (macroexpand-1 '(infix 10 + 20)))

(suite-summary)
