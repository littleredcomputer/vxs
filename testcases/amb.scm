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
;; The oracle, and all of its state closed over by the search that owns
;; it. As globals these worked exactly once: a second search alive at the
;; same time would share one path, and a search nested inside another's
;; choice point would overwrite its parent's. Neither fails loudly — the
;; outer search would just quietly explore the wrong tree.
;;
;; `path` is what this run actually chose, as (index . width) pairs, held
;; INNERMOST FIRST because that is the end it grows at. `advance` reverses
;; once, on the way to producing the next plan; `plan` is in the order
;; choose consumes it, and is cdr'd down as it goes.
;; Beyond the end of the plan a run takes branch 0 — so the first attempt
;; is leftmost-first, and each subsequent one differs from the last in
;; exactly one place.
;;
;; The failure token is a FRESH object per search, which is what lets
;; searches nest: an inner guard tests it with eq?, so an outer search's
;; failure passing through an inner one is not caught by mistake — it is
;; re-raised and reaches the search that actually owns it.

;; The rightmost choice with an untried alternative, bumped by one, with
;; everything to its right dropped. #f when the tree is exhausted. This is
;; ordinary odometer arithmetic — it is the whole search strategy, and it
;; holds no state, so it stays a plain procedure.
;;
;; Takes the path innermost-first, the order choose builds it in, and
;; returns a plan in the order choose consumes it. That single reverse is
;; what buys the O(1) append on the hot side.
(define (advance rev)
  (cond ((null? rev) #f)
        ((< (+ 1 (caar rev)) (cdar rev))
         (reverse (cons (cons (+ 1 (caar rev)) (cdar rev)) (cdr rev))))
        (else (advance (cdr rev)))))

;; Every solution, as a generator — so a caller can ask for the first and
;; walk away, and the rest of the tree is never explored.
;;
;; `proc` is handed its own `choose` and `require`. Passing them in rather
;; than defining them globally is the whole fix: they close over this
;; search's state and nothing else can reach it.
;;
;; NOTE the shape: the guard wraps ONLY the attempt, and the yield sits
;; outside it. This began as a constraint — (yield) inside guard used to
;; be impossible, because guard ran its body through a native call the
;; fiber could not suspend through. It compiles inline now, so the shape
;; is a CHOICE, and still the right one: a guard that stayed open across
;; the yield would also be open while the consumer runs, and a failure
;; token raised out there would be read as this search failing. Scoping
;; the handler to the attempt says what is meant.
(define (solutions proc)
  (let ((path '())
        (plan '())
        (fail (list 'amb-fail)))   ; fresh, so eq? identifies THIS search

    ;; O(1) in the depth, which the obvious version is not. Appending each
    ;; decision to the END of `path` to keep it in forward order costs
    ;; O(k) and allocates k fresh conses, once per choice — quadratic in
    ;; the depth of the search, measured at 3.8x per doubling. Consing
    ;; onto the front and reversing once, in `advance`, is the standard
    ;; remedy and is also the shorter code. Likewise `plan` is CONSUMED
    ;; rather than indexed, which makes the depth counter unnecessary.
    (define (choose choices)
      (if (null? choices) (raise fail))
      (let ((i (if (pair? plan) (caar plan) 0)))
        (if (pair? plan) (set! plan (cdr plan)))
        ;; A replayed index can overshoot if an earlier choice narrowed
        ;; this fan-out. Fail the branch rather than index off the end.
        (if (>= i (length choices)) (raise fail))
        (set! path (cons (cons i (length choices)) path))
        (list-ref choices i)))

    (define (require ok) (if (not ok) (raise fail)))

    (generator
     (lambda ()
       ;; The plan is consumed as it is replayed, so each run starts from
       ;; the one the previous run's path produced — carried as a loop
       ;; argument rather than rebuilt from a variable choose has eaten.
       (let loop ((next-plan '()))
         (set! path '())
         (set! plan next-plan)
         (let* ((attempt (guard (e ((eq? e fail) fail)) (proc choose require)))
                (taken   path))
           (if (not (eq? attempt fail))
               (yield attempt))
           (let ((further (advance taken)))
             (if further (loop further) 'exhausted))))))))

;;--- Pythagorean triples ------------------------------------------------

(define (upto a b) (if (> a b) '() (cons a (upto (+ a 1) b))))

(define (pythagorean n)
  (lambda (choose require)
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
  (lambda (choose require)
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
(say "Two searches interleaved, a step at a time:")
;; The reason the state had to be closed over. As globals these two shared
;; one path and one plan, so interleaving them silently explored a tree
;; that was neither one's.
(let ((p (solutions (pythagorean 20)))
      (q (solutions (pythagorean 12))))
  ;; The LAST resume returns the thunk's return value, not a solution,
  ;; which is what generator-live? afterwards is for. `q` runs out first.
  (define (next g)
    (let ((v (resume g))) (if (generator-live? g) v '--none--)))
  (let loop ((n 0))
    (if (< n 3)
        (begin
          (say "  up to 20: " (next p) "    up to 12: " (next q))
          (loop (+ n 1))))))

(say "")
(say "A search nested inside another search's choice point:")
;; The inner search runs to completion inside every attempt of the outer
;; one. Each has its own failure token, so an outer failure crossing the
;; inner guard is re-raised rather than swallowed — which is what makes
;; the tokens fresh objects compared with eq?, not a shared symbol.
(let ((outer (solutions
              (lambda (choose require)
                (let* ((c (choose '(5 13 25)))
                       (inner (solutions
                               (lambda (ch rq)
                                 (let* ((a (ch (upto 1 c)))
                                        (b (ch (upto a c))))
                                   (rq (= (+ (* a a) (* b b)) (* c c)))
                                   (list a b)))))
                       (legs (resume inner)))
                  (require (generator-live? inner))   ; fails when none exist
                  (list c 'from legs))))))
  (let loop ()
    (let ((s (resume outer)))
      (if (generator-live? outer) (begin (say "  " s) (loop))))))

(say "")
(say "done.")
