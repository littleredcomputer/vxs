;;
;; basic STREAM procedures as discussed in SICP ss. 3.5.1 - 3.5.3
;;

(defmacro (cons-stream a b)
  `(cons ,a (delay ,b)))

(define the-empty-stream '())

(define (stream-null? s) (eq? s the-empty-stream))

(define (stream-car stream)
  (car stream))

(define (stream-cdr stream)
  (force (cdr stream)))

(define (stream-filter pred stream)
  (cond ((null? stream) '())
	((pred (stream-car stream))
	 (cons-stream (stream-car stream)
		      (stream-filter pred
				     (stream-cdr stream))))
	(else (stream-filter pred (stream-cdr stream)))))

(define (stream-ref s n)
  (if (= n 0) 
      (stream-car s)
      (stream-ref (stream-cdr s) (- n 1))))

(define (stream-map proc . argstreams)
  (if (null? (car argstreams))
      '()
      (cons-stream (apply proc (map stream-car argstreams))
		   (apply stream-map proc (map stream-cdr argstreams)))))

(define (stream-append s1 s2)
  (if (stream-null? s1)
      s2
      (cons-stream (stream-car s1)
		   (stream-append (stream-cdr s1) s2))))

(define (interleave s1 s2)
  (if (stream-null? s1)
      s2
      (cons-stream (stream-car s1)
		   (interleave s2 (stream-cdr s1)))))

(define (stream-add stream addend)
  (stream-map (lambda (e) (+ e addend)) stream))

(define (scale-stream stream factor)
  (stream-map (lambda (x) (* x factor)) stream))

(define (stream+ s t)
  (stream-map + s t))

(define (stream/ s t)
  (stream-map / s t))

(define (display-stream s)
  (if (null? s)
      'ok
      (begin
	(newline)
	(display (stream-car s))
	(display-stream (stream-cdr s)))))

(define (display-stream-n s n)
  (let loop ((i 0) (rest s))
    (if (= i n) 'ok
	(begin
	  (display (stream-car rest))
	  (newline)
	  (loop (+ i 1) (stream-cdr rest))))))

(define (integers-starting-from n)
  (cons-stream n (integers-starting-from (+ n 1))))

(define integers (integers-starting-from 1))
