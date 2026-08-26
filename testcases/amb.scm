;;----------------------------------------------------------------------
;; Backtracking search on fibers, without call/cc.
;;
;; A fiber SUSPENDS; it does not REWIND. There is one stack and no
;; snapshot of it, so the classic amb — where (fail) re-enters the middle
;; of a computation that already ran past its choice point — is not the
;; shape available here. What is available is better suited to this VM
;; anyway, and this file demonstrates both halves of it:
;;
;;   1. a generator is a producer you drive by hand, so producer and
;;      consumer can be written as two ordinary loops that take turns;
;;
;;   2. a search re-EXECUTES rather than rewinding. Every attempt runs the
;;      whole computation from the top, and a choice point asks an oracle
;;      what to pick. The driver changes the oracle's answers between
;;      runs. One run is one complete path through the search tree.
;;
;; The trade is explicit: rewinding is cheaper per node, re-execution
;; costs a full replay of the prefix. What re-execution buys is that the
;; program is ordinary code — no continuation capture, no restriction on
;; what a choice point may sit inside — and that every attempt is a real,
;; complete, inspectable execution rather than a resumed fragment.
;;
;; Run: ./vx-scheme testcases/amb.scm
;;----------------------------------------------------------------------

(define (say . xs) (for-each display xs) (newline))

;;--- 1. a generator is a producer -------------------------------------
;; No list is built. `naturals` has no idea how many of it will be
;; wanted, and stops existing when nobody resumes it.

(define (naturals from)
  (generator (lambda ()
               (let loop ((n from))
                 (yield n)
                 (loop (+ n 1))))))

(define (take g n)
  (let loop ((i 0) (acc '()))
    (if (= i n) (reverse acc) (loop (+ i 1) (cons (resume g) acc)))))

(say "first ten naturals: " (take (naturals 0) 10))

;;--- 2. generate and test, as a pair -----------------------------------
;; The consumer is also a generator, so it too is demand-driven: nothing
;; runs until something downstream asks. `filtered` pulls from `source`
;; only as far as it must to satisfy its own caller.

(define (filtered source keep?)
  (generator (lambda ()
               (let loop ()
                 (let ((x (resume source)))
                   (if (keep? x) (yield x))
                   (loop))))))

(define (divides? d n) (= 0 (modulo n d)))

(say "first eight multiples of 7: "
     (take (filtered (naturals 1) (lambda (n) (divides? 7 n))) 8))

;; Two filters stacked. Neither knows the other exists; each just resumes
;; whatever it was handed.
(say "...that are also odd:      "
     (take (filtered (filtered (naturals 1) (lambda (n) (divides? 7 n)))
                     odd?)
           8))

;;--- 3. amb, by re-execution -------------------------------------------
;; The oracle. `choose` is the choice point: it asks which branch to take
;; and records the width of the fan-out, so the driver knows afterwards
;; which choices still had alternatives left.
;;
;; `path` is what this run actually chose, as (index . width) pairs.
;; `plan` is what the driver wants chosen, from the previous run's path.
;; Beyond the end of the plan a run takes branch 0 — so the first attempt
;; is leftmost-first, and each subsequent one differs from the last in
;; exactly one place.

(define amb-path '())
(define amb-plan '())
(define amb-fail (list 'amb-fail))   ; a fresh object, so eq? identifies it

(define (choose choices)
  (if (null? choices) (raise amb-fail))
  (let* ((k (length amb-path))
         (i (if (< k (length amb-plan)) (car (list-ref amb-plan k)) 0)))
    (if (>= i (length choices)) (raise amb-fail))
    (set! amb-path (append amb-path (list (cons i (length choices)))))
    (list-ref choices i)))

(define (require ok) (if (not ok) (raise amb-fail)))

;; The rightmost choice with an untried alternative, bumped by one, with
;; everything to its right dropped. #f when the tree is exhausted. This is
;; ordinary odometer arithmetic — it is the whole search strategy.
(define (advance path)
  (let loop ((rev (reverse path)))
    (cond ((null? rev) #f)
          ((< (+ 1 (caar rev)) (cdar rev))
           (reverse (cons (cons (+ 1 (caar rev)) (cdar rev)) (cdr rev))))
          (else (loop (cdr rev))))))

;; Every solution, as a generator — so a caller can ask for the first and
;; walk away, and the rest of the tree is never explored.
;;
;; NOTE the shape: the guard wraps ONLY the attempt, and the yield sits
;; outside it. (yield) inside guard cannot work — guard's continuation
;; includes native frames, so the fiber cannot suspend through it. That is
;; a real constraint on this design, not an incidental style choice.
(define (solutions thunk)
  (generator
   (lambda ()
     (set! amb-plan '())
     (let loop ()
       (set! amb-path '())
       (let* ((attempt (guard (e ((eq? e amb-fail) amb-fail)) (thunk)))
              (taken   amb-path))
         (if (not (eq? attempt amb-fail))
             (yield attempt))
         (let ((next (advance taken)))
           (if next (begin (set! amb-plan next) (loop)) 'exhausted)))))))

;;--- Pythagorean triples ------------------------------------------------

(define (upto a b) (if (> a b) '() (cons a (upto (+ a 1) b))))

(define (pythagorean n)
  (lambda ()
    (let* ((a (choose (upto 1 n)))
           (b (choose (upto a n)))
           (c (choose (upto b n))))
      (require (= (+ (* a a) (* b b)) (* c c)))
      (list a b c))))

(say "")
(say "Pythagorean triples with sides up to 20:")
(let ((g (solutions (pythagorean 20))))
  (let loop ()
    (let ((s (resume g)))
      (if (generator-live? g)
          (begin (say "  " s) (loop))))))

;;--- a small placement puzzle -------------------------------------------
;; Five people on five floors, with the usual constraints. Written as
;; ordinary code: `choose` where there is a decision, `require` where
;; there is a rule. Nothing here knows it is being searched.

(define (distinct? xs)
  (cond ((null? xs) #t)
        ((memv (car xs) (cdr xs)) #f)
        (else (distinct? (cdr xs)))))

(define (floors)
  (lambda ()
    (let* ((levels '(1 2 3 4 5))
           (baker    (choose levels))
           (cooper   (choose levels))
           (fletcher (choose levels))
           (miller   (choose levels))
           (smith    (choose levels)))
      (require (distinct? (list baker cooper fletcher miller smith)))
      (require (not (= baker 5)))
      (require (not (= cooper 1)))
      (require (and (not (= fletcher 5)) (not (= fletcher 1))))
      (require (> miller cooper))
      (require (not (= 1 (abs (- smith fletcher)))))
      (require (not (= 1 (abs (- fletcher cooper)))))
      (list (list 'baker baker) (list 'cooper cooper)
            (list 'fletcher fletcher) (list 'miller miller)
            (list 'smith smith)))))

(say "")
(say "Floor assignment:")
(let ((g (solutions (floors))))
  (let ((first (resume g)))
    (say "  " first)
    ;; The generator is still live and the tree is not exhausted. Walking
    ;; away here is the point: nothing further is explored, and the
    ;; unresumed generator is collected with its fiber inside it.
    (say "  (found without searching the rest: still live? "
         (generator-live? g) ")")))

(say "")
(say "done.")
