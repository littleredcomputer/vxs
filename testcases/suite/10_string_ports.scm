;;----------------------------------------------------------------------
;; Layer 10: string ports and format
;;
;; The idiomatic Scheme answer to building a string incrementally, and the
;; right substrate for generating source text (WGSL, shortly). The point
;; of a string PORT rather than a StringBuilder-style object is that it
;; needs no new API: display, write, newline, write-char and any user
;; procedure that already takes a port argument work on it unchanged.
;;
;; format is deliberately four directives — ~a ~s ~% ~~ — and not Common
;; Lisp's FORMAT. The iteration directive (~{~^~}) is genuinely useful for
;; emitting comma-separated argument lists and is also the thin end of a
;; write-only mini-language; a `join` in Scheme reads better.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "10_string_ports: string ports, format, source emission")

;; --- accumulation -------------------------------------------------------

(define p (open-output-string))
(display "hello" p)
(display " " p)
(display "world" p)
(assert-equal "display accumulates into a string port"
              "hello world" (get-output-string p))

;; Reading is non-destructive and the port stays open: a caller that peeks
;; mid-build should not silently truncate what it is building.
(display "!" p)
(assert-equal "port remains writable after get-output-string"
              "hello world!" (get-output-string p))
(assert-equal "get-output-string is repeatable"
              "hello world!" (get-output-string p))

(assert-equal "a fresh string port is empty"
              "" (get-output-string (open-output-string)))

;; --- every existing writer targets it, with no new API ------------------

(define q (open-output-string))
(write "quoted" q)
(newline q)
(write-char #\x q)
(display 42 q)
(assert-equal "write / newline / write-char / display all work on a port"
              "\"quoted\"\nx42" (get-output-string q))

;; --- format -------------------------------------------------------------

(assert-equal "~a displays"                "v = 42"     (format "v = ~a" 42))
(assert-equal "~s writes"                  "s = \"hi\"" (format "s = ~s" "hi"))
(assert-equal "~a renders a string bare"   "hi"         (format "~a" "hi"))
(assert-equal "~% is a newline"            "a\nb"       (format "a~%b"))
(assert-equal "~~ is a literal tilde"      "100~"       (format "100~~"))
(assert-equal "no directives is identity"  "plain"      (format "plain"))
(assert-equal "several directives in order"
              "let x = a * b;\n"
              (format "let ~a = ~a * ~a;~%" 'x 'a 'b))
;; Unknown directives and missing arguments pass through rather than
;; aborting: a half-written format string during live editing should show
;; you what you typed, not kill the fiber emitting a shader.
(assert-equal "unknown directive passes through verbatim" "~q" (format "~q"))
(assert-equal "missing arguments are skipped, not fatal"  "x=" (format "x=~a"))

(define r (open-output-string))
(format r "fn ~a() {~%" 'main)
(format r "  return ~a;~%" 42)
(format r "}~%")
(assert-equal "format writes to a leading port argument"
              "fn main() {\n  return 42;\n}\n" (get-output-string r))

;; --- rebinding the current output port ----------------------------------

(assert-equal "with-output-to-string captures default output"
              "captured 7"
              (with-output-to-string (lambda () (display "captured ") (display 7))))

(assert-equal "with-output-to-string restores the port when the thunk raises"
              'raised
              (guard (e (#t 'raised))
                (with-output-to-string (lambda () (display "x") (error "boom")))))
(assert-equal "default output still works after a raised thunk"
              "still fine"
              (with-output-to-string (lambda () (display "still fine"))))

(assert-equal "nested with-output-to-string"
              "outer[inner]"
              (with-output-to-string
                (lambda ()
                  (display "outer")
                  (display (string-append
                            "[" (with-output-to-string (lambda () (display "inner"))) "]")))))

(assert-equal "call-with-output-string"
              "3 items"
              (call-with-output-string (lambda (port) (format port "~a items" 3))))

;; --- input string ports -------------------------------------------------

(define ip (open-input-string "(a b c) 42"))
(assert-equal "read a form from a string port"  '(a b c) (read ip))
(assert-equal "read the next form"              42       (read ip))

;; --- the use this exists for --------------------------------------------

(define (emit-fn port name body-lines)
  (format port "fn ~a() {~%" name)
  (for-each (lambda (l) (format port "  ~a~%" l)) body-lines)
  (format port "}~%"))

(assert-equal "emitting a source function"
              "fn drift() {\n  let a = 1;\n  let b = 2;\n  return a + b;\n}\n"
              (call-with-output-string
                (lambda (port)
                  (emit-fn port 'drift '("let a = 1;" "let b = 2;" "return a + b;")))))

;; --- port predicates ----------------------------------------------------
;; port? is the direction-agnostic one. Only input-port? and output-port?
;; existed for a long time, so "is this a port at all" had to be spelled
;; (or (input-port? x) (output-port? x)).

(define op (open-output-string))
(assert-equal "port? accepts an output port" #t (port? op))
(assert-equal "port? accepts an input port"  #t (port? (open-input-string "x")))
(assert-equal "port? rejects a non-port"     #f (port? 42))
(assert-equal "port? rejects a string"       #f (port? "not a port"))
(assert-equal "port? accepts the current output port" #t (port? (current-output-port)))

(assert-equal "output-port? is direction-sensitive" #f
              (output-port? (open-input-string "x")))
(assert-equal "input-port? is direction-sensitive" #f (input-port? op))

;; flush-output-port is a no-op on a port that does not buffer, and must
;; not disturb what has already been written.
(define fp (open-output-string))
(display "before" fp)
(flush-output-port fp)
(display "/after" fp)
(assert-equal "flush-output-port leaves buffered content alone"
              "before/after" (get-output-string fp))

(suite-summary)
