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

; =================
; LIBRARY FUNCTIONS
; =================

(define (map fn list1 . more)
  (if (null? more)
      (let loop ((l list1) (acc '()))
        (if (null? l)
            (reverse acc)
            (loop (cdr l) (cons (fn (car l)) acc))))
      (let loop ((lists (cons list1 more)) (acc '()))
        (if (null? (car lists))
            (reverse acc)
            (let ((cars (let car-loop ((ls lists) (c-acc '()))
                          (if (null? ls)
                              (reverse c-acc)
                              (car-loop (cdr ls) (cons (caar ls) c-acc)))))
                  (cdrs (let cdr-loop ((ls lists) (d-acc '()))
                          (if (null? ls)
                              (reverse d-acc)
                              (cdr-loop (cdr ls) (cons (cdar ls) d-acc))))))
              (loop cdrs (cons (apply fn cars) acc)))))))

(define (for-each fn list1 . more)
  (if (null? more)
      (let loop ((l list1))
        (if (null? l)
            (if #f #f)
            (begin
              (fn (car l))
              (loop (cdr l)))))
      (let loop ((lists (cons list1 more)))
        (if (null? (car lists))
            (if #f #f)
            (let ((cars (let car-loop ((ls lists) (c-acc '()))
                          (if (null? ls)
                              (reverse c-acc)
                              (car-loop (cdr ls) (cons (caar ls) c-acc)))))
                  (cdrs (let cdr-loop ((ls lists) (d-acc '()))
                          (if (null? ls)
                              (reverse d-acc)
                              (cdr-loop (cdr ls) (cons (cdar ls) d-acc))))))
              (apply fn cars)
              (loop cdrs))))))

(define (call-with-input-file filename procedure)
  (let* ((open-file (open-input-file filename))
         (value (procedure open-file)))
    (close-input-port open-file)
    value))

(define (call-with-output-file filename procedure)
  (let* ((open-file (open-output-file filename))
         (value (procedure open-file)))
    (close-output-port open-file)
    value))

(define (load file) 
  (let ((input (open-input-file file)))
    (do ((form (read input) (read input)))
        ((eof-object? form)
         (close-input-port input)
         'ok)
      (eval form))))
