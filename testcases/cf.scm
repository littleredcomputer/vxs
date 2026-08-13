
(load "stream.scm")

;
; Given a continued fraction in the form of a stream of 
; integers, return the stream of convergents.  (The stream
; actually returns a list (num denom quotient) ).
;

(define (cf->convergents cf-stream)
  (define (produce n-2 n-1 rest)
    (let ((nextval (+ (* n-1 (stream-car rest)) n-2)))
      (cons-stream nextval (produce n-1 nextval (stream-cdr rest)))))
  (define (cf-num cf-stream)
    (let* ((a0 (stream-car cf-stream))
	   (a1 (stream-car (stream-cdr cf-stream)))
	   (n1 (+ (* a0 a1) 1))
	   (rest (stream-cdr (stream-cdr cf-stream))))
      (cons-stream a0 (cons-stream n1 (produce a0 n1 rest)))))
  (define (cf-denom cf-stream)
    (let ((a0 1)
	  (a1 (stream-car (stream-cdr cf-stream)))
	  (rest (stream-cdr (stream-cdr cf-stream))))
      (cons-stream a0 (cons-stream a1 (produce a0 a1 rest)))))
  (stream-map (lambda (n d) (list n d (/ n d)))
	      (cf-num cf-stream)
	      (cf-denom cf-stream)))

(define ones (cons-stream 1 ones))
(define twos (cons-stream 2 twos))
(define onetwo (interleave ones twos))

(display-stream-n ones 5)

(display-stream-n (cf->convergents ones) 40)

(display (/ (+ 1 (sqrt 5)) 2))

(display-stream-n (cf->convergents twos) 10)
(display-stream-n (cf->convergents onetwo) 10)
