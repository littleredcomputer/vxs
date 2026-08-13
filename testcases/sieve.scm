;;
;; The prime-stream example [SICP 2ed. p. 327]
;;

(load "stream.scm")

(define (divisible? x y) (= (remainder x y) 0))

(define (sieve stream)
  (cons-stream
   (stream-car stream)
   (sieve (stream-filter
	   (lambda (x)
	     (not (divisible? x (stream-car stream))))
	   (stream-cdr stream)))))

(define primes (sieve (integers-starting-from 2)))

(display (stream-ref primes 300))
(newline)
