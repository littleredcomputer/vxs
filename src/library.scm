;; Library functions for Vx-Scheme
;;
;; Copyright (c) 2003,2006 and onwards Colin Smith
;;
;; These are procedures designed to run in the virtual machine.  They
;; cannot be implemented in C, because each of these arguments takes a
;; parameter of procedure type.  The C implementation would then be
;; forced to reenter the virtual machine, which is not allowed.  By
;; implementing these procedures in Scheme itself, we can produce
;; bytecode that the VM can execute.
;; 
;;

; =================
; LIBRARY FUNCTIONS
; =================

(define (map fn . arglists)
  (define (map0 fn arglists)
    (let loop ((results '())
	       (rest arglists))
      (if (null? (car rest))
	  results
	  (loop (append
		 results
		 (list
		  (apply fn 
			 (let car-loop ((rest1 rest)
					(args '()))
			   (if (null? rest1)
			       args
			       (car-loop (cdr rest1)
					 (append args (list (caar rest1)))))))))
		(let cdr-loop ((rest1 rest)
			       (args '()))

		  (if (null? rest1)
		      args
		      (cdr-loop (cdr rest1)
				(append args (list (cdar rest1))))))))))
  (map0 fn arglists))

(define (for-each fn . arglists)
  (define (for-each0 fn arglists)
    (let loop ((rest arglists))
      (if (null? (car rest))
	  (if #f #f) ; unspecified
	  (begin
	    (apply fn 
		   (let car-loop ((rest1 rest)
				  (args '()))
		     (if (null? rest1)
			 args
			 (car-loop (cdr rest1)
				   (append args (list (caar rest1)))))))
	    (loop (let cdr-loop ((rest1 rest)
				 (args '()))
		    (if (null? rest1)
			args
			(cdr-loop (cdr rest1)
				  (append args (list (cdar rest1)))))))))))
  (for-each0 fn arglists))

(define (call-with-input-file filename procedure)
  (let ((open-file (open-input-file filename)))
    (procedure open-file)))

(define (call-with-output-file filename procedure)
  (let* ((open-file (open-output-file filename))
	 (value (procedure open-file)))
    (close-output-port open-file)
    value))

(define (load file) 
  (let ((input (open-input-file file)))
    (do ((form (read input) (read input)))
	((eof-object? form) 'ok)
      (eval form))))
    


