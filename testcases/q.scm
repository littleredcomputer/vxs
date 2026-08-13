(define l '(-5 -4 -3 -2 -1 1 2 3 4 5))

(for-each
 (lambda (n) 
   (for-each
    (lambda (d)
      (display (quotient n d))
      (newline))
    l))
 l)


