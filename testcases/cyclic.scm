;;
;; Run all the tests we have in the same interpreter context, 
;; many times.
;; 

(let test-loop ((i 0))
  (if (= i 500)
      'ok
      (begin
	(map 
	 (lambda (test)
	   (load (string-append test ".scm"))
	   (display "#complete: ")
	   (display test)
	   (newline))
	 '("ack" "cf" "pi" "series" "sieve" "r4rstest"))
	(test-loop (+ i 1)))))
