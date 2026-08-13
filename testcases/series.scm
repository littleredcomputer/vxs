;;
;; Several infinite series computations from, e.g.,  SICP 2ed. s. 3.5.3
;;
(load "stream.scm")

(define (average x y)
  (/ (+ x y) 2))

(define (sqrt-improve guess x)
  (average guess (/ x guess)))

(define (sqrt-stream x)
  (define guesses
    (cons-stream 1.0
		 (stream-map (lambda (guess)
			       (sqrt-improve guess x))
			     guesses)))
  guesses)

(display-stream-n (sqrt-stream 2) 10)

(newline)

(define (partial-sums s)
  (cons-stream
   (stream-car s)
   (partial-sums (cons-stream (+ (stream-car s) (stream-car (stream-cdr s)))
			      (stream-cdr (stream-cdr s))))))


(display-stream-n (partial-sums integers) 10)

(newline)

(define (pi-summands n)
  (cons-stream (/ n)
	       (stream-map - (pi-summands (+ n 2)))))

(define pi-stream 
  (scale-stream (partial-sums (pi-summands 1)) 4))

(display-stream-n pi-stream 10)

(newline)

(define (square x) (* x x))

(define (euler-transform s)
  (let ((s0 (stream-ref s 0))
	(s1 (stream-ref s 1))
	(s2 (stream-ref s 2)))
    (cons-stream (- s2 (/ (square (- s2 s1))
			  (+ s0 (* -2 s1) s2)))
		 (euler-transform (stream-cdr s)))))

(display-stream-n (euler-transform pi-stream) 10)

(newline)

(define (make-tableau transform s)
  (cons-stream s 
	       (make-tableau transform
			     (transform s))))


(define (accelerated-sequence transform s)
  (stream-map stream-car 
	      (make-tableau transform s)))

(display-stream-n (accelerated-sequence euler-transform
					pi-stream)
		  10)
