;; This harness once picked its reference-output directory (good/,
;; vx-good/, c-good/, w32-good/) by probing scheme-implementation-type/
;; -platform — a relic of targeting SCM, VxWorks, and Win32 builds
;; alongside our own. One engine, one set of reference files now.
(define gf-prefix "good/")

;; some of our testcases use notation like 'bitwise-and' for 'logand';
;; we supply the needed bindings

(define bitwise-and logand)
(define bitwise-not lognot)

(define (file=? f1 f2)                          ; compare two open files for 
  (let loop ((c1 (read-char f1))                ; bytewise equality.
	     (c2 (read-char f2)))
    (cond ((eof-object? c1)                     ; if both files EOF at the 
	   (eof-object? c2))                    ; same time, we win, else
	  ((eof-object? c2)                     ; the streams aren't equal.
	   #f)
	  (else
	   (if (eqv? c1 c2)                     ; two equal chars? keep going
	       (loop (read-char f1)
		     (read-char f2))
	       #f)))))                          ; unequal characters: lose.

(define testcases '("r4rstest" "pi" "scheme" "dynamic" "earley" "maze" 
                    "dderiv" "boyer" "puzzle" "ack" "sieve" "cf" "series"))

(define (run-testcase t)                        ; run one testcase
  (gc)                                          ; give each test a clean start
  (let* ((infile   (string-append t ".scm"))
	 (outfile  (string-append t ".out"))
	 (goodfile (string-append gf-prefix t ".good"))
	 (result
	  (time (lambda ()
		  (with-output-to-file outfile
		    (lambda () (load infile))))))
	 (ok (file=? (open-input-file outfile)   ; compare it with good output
		     (open-input-file goodfile))))
    (cons ok (car result))))                     ; return (pass? . elapsed time)
  
(let ((total-time 0.0))
  (for-each                                       ; run all testcases
   (lambda (testcase)
     (let ((result (run-testcase testcase)))
       (if (car result) 
	   (begin
	     (display "PASS: ")
	     (display (cdr result))
	     (display " ")
	     (set! total-time (+ total-time (cdr result))))
	   (display "FAIL: "))
       (display testcase)
       (newline)))
   testcases)
  (display "total time: ")
  (display total-time)
  (newline))



