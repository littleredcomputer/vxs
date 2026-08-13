(load "vx-scheme.init")
(defmacro (slib-test module expression expected-result)
  `(begin
     (require ,module)
     (display (if (equal? ,expression ,expected-result)
		  "PASS"
		  "FAIL"))
     (display ": ") (display ,module)
     (newline)))

(slib-test 'sort           (sorted? (sort '(6 5 7 4 8 3 9 1) <) <) #t)
(slib-test 'factor         (factor 105) '(7 3 5))
(slib-test 'object->string (object->string '(2 3)) "(2 3)")



 
