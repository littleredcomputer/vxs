;; Compiler for Vx-Scheme
;;
;; Copyright (c) 2003,2006 and onwards Colin Smith
;;
;; You may distribute under the terms of the Artistic License, 
;; as specified in the LICENSE file.
;;
;; Based on ideas from [PAIP]: "Paradigms of Artificial Intelligence
;; Programming: Case Studies in Common Lisp," 1992, Peter Norvig, and
;; [SICP]: "Structure and Interpretation of Computer Programs," 2ed.,
;; 1996, Harold Abelson and Gerald Jay Sussman with Julie Sussman, MIT
;; Press,



; =========
; ASSEMBLER
; =========

(define (assemble insns)
  (define (branch? opcode)
    (memq opcode '(goto false? false?p true? true?p save)))
  (let ((nonlabels '())
	(labelmap '())
	(counter 0))
    ;; pass 1: count non label instructions and memorize label positions.
    (let pass1 ((insn insns))
      (let* ((i (car insn))
	     (opcode (car i)))
	(if (eq? opcode 'label)
	    (set! labelmap (cons (cons (cadr i) counter) labelmap))
	    (set! counter (+ counter 1)))
	(if (not (null? (cdr insn)))
	    (pass1 (cdr insn)))))
    ;; pass 2: pack instructions into vector, while replacing labels with 
    ;; indices.
    (let pass2 ((outseq (make-vector counter))
		(insn insns)
		(ix 0))
      (let* ((i (car insn))
	     (opcode (car i)))
	(if (not (eq? opcode 'label))
	    (begin
	      (cond 
	       ((branch? opcode)
		(vector-set! outseq ix
			     (list opcode (cdr (assq (cadr i) labelmap)))))
	       (else
		(vector-set! outseq ix i)))
	      (if (not (null? (cdr insn)))
		  (pass2 outseq (cdr insn) (+ ix 1))))
	    (if (not (null? (cdr insn)))
		(pass2 outseq (cdr insn) ix))))

      outseq)))

;;; ========
;;; COMPILER
;;; ========

;; This is an association list of macro definitions:
;; ((name . (arglist . body))...)
(define __macro_table '())

  
(define (compile form) 
  
  (define *inline-procedures* '(+ * - quotient remainder
				  vector-ref vector-set! car cdr
				  zero? not null? eq? pair? cons))

  (define (builtin? proc)
    (memq proc '(if quote cond begin lambda or and let set! define letrec
		    let* do case quasiquote delay defmacro define-macro)))

  ;; We provide two simplified replacements for the library function map
  ;; (one for one arguments, the other for two), neither of which uses
  ;; the 'apply' primitive.  The reason: map must apply its procedure 
  ;; argument to the input list(s).  While the interpreter knows how to 
  ;; apply a compiled procedure, compiled Scheme code cannot invoke a 
  ;; procedure in the interpreter, as this would reenter the interpreter
  ;; when the compiler compiles itself.  We avoid this by supplying 
  ;; two apply-less map substitutes here.

  (define (_map func lst)
    (let loop ((result '())
	       (rest lst))
      (if (null? rest)
	  result
	  (loop (append result (list (func (car rest)))) (cdr rest)))))

  (define (_map2 func lst1 lst2)
    (let loop ((result '())
	       (rest1 lst1)
	       (rest2 lst2))
      (if (null? rest1)
	  result
	  (loop (append result
			(list (func (car rest1) (car rest2))))
		(cdr rest1) (cdr rest2)))))

  ;; starts-with: frequently used in [PAIP]; we define it here.
  ;; 
  ;; Return #t if l is a list whose first element is x.

  (define (starts-with l x)
    (and (pair? l) (eq? (car l) x)))

  (define unspecified (if #f #f))

  (define make-label
    (let ((label-counter 0))
      (lambda (name)
	(set! label-counter (+ label-counter 1))
	(string->symbol (string-append
			 (symbol->string name)
			 (number->string label-counter))))))

  (define (extend-environment env args)
    (cons args env))
  
  (define (form-returning value more? val? . args)
    (append
     args
     (if val? 
	 (cond
	  ((null? value) '((nil)))
	  ((eq? value (if #f #f)) '((unspc)))
	  ((eq? value #f) '((false)))
	  ((eq? value #t) '((true)))
	  ((integer? value) `((int ,value)))
	  ;; ((symbol? value) `((const ,value)))
	  (else `((const ,value))))
	 '())
     (if (not more?) `((return)) '())))

  ; emit insns if condition? is true.
  ;
  (define (code-if condition? . insns)
    (if condition? insns '()))
	

  (define (compile-compound form env more? val?)
    (let ((proc (car form))
	  (args (cdr form)))
      (cond 
       ((builtin? proc)
	;; SPECIAL FORM
	(compile-builtin proc args env more? val?))
       ((assq proc __macro_table) 
	;; MACRO
	=> (lambda (macro) (compile-macro macro args env more? val?)))
       (else
	;; PROCEDURE APPLICATION
	(compile-apply   proc args env more? val?)))))

  (define (locate-local-variable env var)
    (define (locate-within env var)
      (let var-loop ((v env)
		     (nv 0))
	(if (null? v) #f
	    (if (eq? (car v) var)
		nv
		(var-loop (cdr v) (+ nv 1))))))
    (let env-loop ((e env)
		   (ne 0))
      (if (null? e) #f ; game over: ran out of environments without finding it.
	  (let ((location (locate-within (car e) var)))
	    (if location 
		(cons ne location)
		(env-loop (cdr e) (+ ne 1)))))))

  ;; -------------------------
  ;; THE BUILTIN SPECIAL FORMS
  ;; -------------------------

  (define (compile-builtin proc args env more? val?)
    (cond
     ((eq? proc 'quote)
      (form-returning (car args) more? val?))
     ((eq? proc 'if)
      (let* ((test (car args))
	     (then-part (cadr args))
	     (have-else-part (not (null? (cddr args))))
	     (else-part (if have-else-part (caddr args) #f))
	     (label1 (make-label 'if))
	     (rendezvous (if more? (make-label 'if) #f)))
	(append
	 (compile-exp test env #t #t)
	 (list `(false?p ,label1))
	 (compile-exp then-part env more? val?)
	 (code-if rendezvous `(goto ,rendezvous))
	 (list `(label ,label1))
	 (if have-else-part
	     (compile-exp else-part env more? val?)
	     (form-returning unspecified more? val?))
	 (code-if rendezvous `(label ,rendezvous)))))
		  
     ((eq? proc 'cond)
      (let ((rendezvous (make-label 'cond-x)))
	(append
	 (let clause-loop ((clauses args)
			   (code '()))
	   (if (null? clauses)
	       ;; if we get here, there was no else clause.  We need to 
	       ;; arrange it so a evaluating a cond none of whose tests
	       ;; are satisfied returns an unspecified value.
	       (append code (form-returning unspecified more? val?))
	       ;; Continue compiling clauses.
	       (clause-loop
		(cdr clauses)
		(append 
		 code
		 ;; Generate the code for one clause.
		 (let* ((clause (car clauses))
			(test (car clause))
			(actions (cdr clause))
			(skip-label (make-label 'cond)))
		   (append 
		    (if (eq? test 'else)
			;; An else clause is always executed.
			(begin
			  (if (not (null? (cdr clauses)))
			      (error "else must be the last clause of a cond"))
			  (compile-sequence actions env more? val?))
			;; Consider the action list.  Look for => in the 
			;; first slot.
			(if (starts-with actions '=>)
			    ;; a => clause.
			    (let ((t-label (make-label 'cond-t))
				  (continuation (and more? (make-label 'cont))))
			      (append
			       (compile-exp test env #t #t)
			       `((true? ,t-label)
				 (pop)
				 (goto ,skip-label)
				 (label ,t-label))
			       ;; XXX We now have the magic number '3'
			       ;; to apologize for here.
			       (code-if continuation
					`(save ,continuation)
					'(take 3)) ; cont goes before argument
			       (compile-exp (cadr actions) env #t #t)
			       `((apply 1))
			       (code-if continuation `(label ,continuation))
			       (code-if (not val?) '(pop))
			       (code-if (and more? (not (null? (cdr clauses))))
					`(goto ,rendezvous))))
			    ;; a regular clause.
			    (begin
			      (append
			       (compile-exp test env #t #t)
			       `((false?p ,skip-label))
			       (compile-sequence actions env more? val?)))))
		    ;; Now we have the value.
		    (code-if more? `(goto ,rendezvous))
		    `((label ,skip-label))))))))
	 (code-if rendezvous `(label ,rendezvous)))))
     ((eq? proc 'case)
      ;; Accomplished by rewriting:
      ;; 
      ;; (case m                    -> (let ((value m))
      ;;   ((u1 u2...) x1 x2...)... ->  (cond ((member? m (u1 u2...)) x1 x2...)
      ;;   (else y1 y2...))         ->        (else y1 y2...))
      ;;
      (let* ((selector (car args))
	     (clauses (cdr args))
	     (value (make-label 'case-var))
	     (cond-clauses (let loop ((code '())
				      (rest clauses))
	       (if (null? rest)
		   code
		   (loop 
		    (append code 
			    (if (eq? (caar rest) 'else)
				`((else ,@(cdar rest)))
				`(((member ,value ',(caar rest)) ,@(cdar rest)))))
		    (cdr rest)))))
	     (augmented-code `(let ((,value ,selector)) 
				(cond ,@cond-clauses))))
	(compile-exp augmented-code env more? val?)))
     ;; (let [name]? ((u1 v1) (u2 v2)...) x1 x2...)
     ((eq? proc 'let)
      (let* ((named (and (symbol? (car args))
			 (car args)))             ; if named let, record name 
	     (args (if named (cdr args) args)))   ; and advance to bindings
	(let* ((bindings (car args))
	       (variables (_map car bindings))
	       (initializers (_map cadr bindings))
	       (body (cdr args)))
	  (compile-let named variables initializers body env more? val?))))
     ((eq? proc 'letrec)
      (let* ((bindings (car args))
	     (variables (_map car bindings))
	     (initializers (_map cadr bindings))
	     (body (cdr args)))
	(compile-letrec variables initializers body env more? val?)))
     ((eq? proc 'let*)
      ;; Accomplished by rewriting:
      ;;
      ;; (let* ((u1 v1) (u2 v2)...) x1 x2...) -> (let ((u1 v1))
      ;;                                           (let* ((u2 v2)...)
      ;;                                             x1 x2...)) 
      ;; When we're down to the last binding, we just compile as a 
      ;; simple let.
      (let* ((bindings (car args))
	     (nbindings (length bindings))
	     (variables (_map car bindings))
	     (initializers (_map cadr bindings))
	     (body (cdr args)))
	(cond ((= nbindings 0) ; (let* () ...) --> (begin ...)
	      (compile-sequence body env more? val?))
	      ((= nbindings 1) ; only one binding (left); simple let.
	      (compile-let #f variables initializers body env more? val?))
	      (else            ; reduce one step.
	       (compile-let #f
			    (list (car variables))
			    (list (car initializers))
			    `((let* ,(cdr bindings)
				,@body))
			    env more? val?)))))
     ((eq? proc 'begin)
      ;; Note: according to R4RS, internal definitions are not recognized
      ;; in a begin (only lambda, let, let*, letrec, define).  This is 
      ;; why we call compile-simple-sequence instead of compile-sequence.
      (compile-simple-sequence args env more? val?))
     ((eq? proc 'lambda)
      (append (compile-procedure-body #f (car args) (cdr args) env #f #t)
	      '((proc))
	      (code-if (not val?) '(pop))
	      (code-if (not more?) '(return))))
     ((eq? proc 'or)
      (if (null? args)
	  (form-returning #f more? val?)
	  (let ((end-label (make-label 'or)))
	    (append 
	     (let or-loop ((rest args)
			   (code '()))
	       (if (null? (cdr rest))
		   (append code (compile-exp (car rest) env more? val?))
		   (or-loop (cdr rest)
			    (append code
				    (compile-exp (car rest) env #t #t)
				    `((true? ,end-label)
				      (pop))))))
	     `((label ,end-label))
	     (code-if (not val?) '(pop))
	     (code-if (not more?) '(return))))))
     ((eq? proc 'and)
      (if (null? args)
	  (form-returning #t more? val?)
	  (let ((end-label (make-label 'and)))
	    (append
	     (let and-loop ((rest args)
			    (code '()))
	       (if (null? (cdr rest))
		   (append code (compile-exp (car rest) env more? val?))
		   (and-loop (cdr rest)
			     (append code
				     (compile-exp (car rest) env #t #t)
				     `((false? ,end-label)
				       (pop))))))
	     `((label ,end-label))
	     (code-if (not val?) '(pop))
	     (code-if (not more?) '(return))))))
     ((eq? proc 'set!)
      (let ((var (car args))
	    (value (cadr args)))
	(append
	 (compile-exp value env #t #t)
	 (compile-assignment env var more? val?))))
     ((eq? proc 'define)
      (append
       (let ((target (car args)))
	 (cond ((symbol? target)       ; (define v x...)
		(append
		 (compile-exp (cadr args) env #t #t)
		 `((gset ,target))))
	       ((pair? target)         ; (define (f v...) x...)
		(let ((proc (car target))
		      (args (cdr target))
		      (body (cdr args)))
		  (append
		   (compile-procedure-body #f args body env #f #t)
		   `((proc)
		     (gset ,proc)))))
	       (else (error "incomprehensible definition"))))
       (form-returning unspecified more? val?)))

     ;; Defmacro.  We expand the quasiquotation at compile time, and
     ;; then compile the result, for evaluation at runtime.

     ((or (eq? proc 'defmacro)     ; XXX this is deprecated; or, use CL syntax
	  (eq? proc 'define-macro) )
      (let* ((name (caar args))
	     (arglist (cdar args))
	     (body (cdr args))
	     (new-macro (cons arglist body)))
	;; Find out if we already have a definition for this macro, and
	;; if so, supersede it; else prepend the new definition to the 
	;; list.
	(cond ((assq name __macro_table) => (lambda (assoc)
					      (set-cdr! assoc new-macro)))
	      (else
	       (set! __macro_table (cons (cons name new-macro)
					 __macro_table)))))
      ;; 
      (form-returning unspecified more? val?))
	
     ((eq? proc 'do)
      (compile-do args env more? val?))

     ((eq? proc 'quasiquote)
      ;; expand quasiquotation and compile result.
      (let ((expansion (expand-quasiquotation (car args))))
	(compile-exp expansion env more? val?)))

     ((eq? proc 'delay)
      ;; (delay X) is a bit like (lambda () X).  Compile the code for X,
      ;; and emit an instruction to wrap it in promise form.
      (append (compile-procedure-body #f '() args env #f #t)
	      '((promise))
	      (code-if (not val?) '(pop))
	      (code-if (not more?) '(return))))
     (else
      (error "unknown builtin"))))

  (define (compile-assignment env var more? val?)
    (let ((location (locate-local-variable env var)))
      (form-returning unspecified more? val?
		      (if location
			  `(lset ,(car location) ,(cdr location))
			  `(gset ,var)))))
  
  (define (compile-sequence body env more? val?)
    (let* ((result (scan-out-defines body))
	   (definitions (car result))
	   (simple-body (cdr result)))
      (if (not (null? definitions))
	  ;; wrap simple-body in a letrec that establishes the 
	  ;; definitions.
	  (let ((items 
		 (let clause-loop ((variables '())
				   (initializers '())
				   (rest definitions))
		   (if (null? rest)
		       (cons variables initializers)
		       (let ((clause (cdar rest))) ; skip past 'define
			 (if (pair? (car clause))  ; procedure definition
			     (clause-loop
			      (append variables (list (caar clause)))
			      (append initializers
				      (list `(lambda ,(cdar clause) ,@(cdr clause))))
			      (cdr rest))
			     (clause-loop          ; scalar definition
			      (append variables (list (car clause)))
			      (append initializers (list (cadr clause)))
			      (cdr rest))))))))
	    (compile-letrec (car items) (cdr items) simple-body env more? val?))
	  ;; if no internal definitions, immediately delegate to
	  ;; compile-simple-sequence.
	  (compile-simple-sequence body env more? val?))))

  ;; scan-out-defines: given a sequence of forms, separate the internal
  ;; definitions from the body forms.  Return the twain in a cons.
  ;; (This way of handling internal definitions comes from [SICP].
  ;; The compiler in [PAIP] doesn't handle internal definitions.  Norvig
  ;; uses letrec in the examples where this would matter.)
  
  (define (scan-out-defines body)
    (let loop ((defines '())
	       (simple-body '())
	       (rest body))
      (cond
       ((null? rest)
	(cons defines simple-body))
       ((starts-with (car rest) 'define)
	(loop (append defines (list (car rest)))
	      simple-body
	      (cdr rest)))
       (else
	(loop defines
	      (append simple-body (list (car rest)))
	      (cdr rest))))))

  ;; compile-simple-sequence: compile a body sequence (list of forms)
  ;; known not to contain any internal definitions (these will have 
  ;; been removed with (scan-out-defines).
  
  (define (compile-simple-sequence body env more? val?)
    (if (null? body)
	(form-returning unspecified more? val?)
	(append
	 (let loop ((code '())
		    (rest body))
	   (if (null? (cdr rest)) ; last in sequence
	       (append code (compile-exp (car rest) env more? val?))
	       (loop (append code (compile-exp (car rest) env #t #f))
		     (cdr rest))))
	 ; Q: why do we need this return? 
	 ; (code-if (not more?) '(return))
	 )))
  
  (define (compile-let name variables initializers body env more? val?)
    ; in the event of named-let, we add a new variable binding to 
    ; contain the procedure value itself.
    (let ((let-env (if name (extend-environment env (list name)) env))
	  (nvars (length variables))
	  (continuation (and more? (make-label 'let))))
      (append
       ; The body of the let will be in the form of a compiled procedure we
       ; will invoke with APPLY.  If we're not in tail context, we need to 
       ; catch that apply so that execution can proceed in line.
       (if continuation `((save ,continuation)) '())
       (let init-loop ((rest initializers) ; generate code for all the 
		       (code '()))         ; initializers.
	 (if (null? rest)
	     code
	     (init-loop (cdr rest)
			(append code
				; NB: for named let, the initializers
				; are _not_ compiled in an evironment
				; containing the procedure body.
				(compile-exp (car rest) env #t #t)))))
       (compile-procedure-body #f variables body let-env #f #t)
       ; Named-let: the closure must be created in an environment where
       ; the let-name is bound, but we can't bind the value until the
       ; closure is created.
       (code-if name '(unassn)
		     '(extend!))           ; reserve envt space
       `((proc))                           ; create closure
       (if name `((dup) (lset 0 0)) '())   ; install in env, if named
       `((apply ,nvars))                   ; invoke procedure
       (code-if (not val?) `(pop))         ; discard value if it's not wanted
       (code-if continuation `(label ,continuation))
       (code-if (not more?) '(return)))))
  
  ; compile-letrec: letrec is tricky.  We compile the form
  ;
  ;    (letrec ((u1 v1) (u2 v2)...) x1 x2...)
  ;
  ; as though it were written
  ;
  ;    (let ((u1 *) (u2 *)...)
  ;      (set! u1 v1)
  ;      (set! u2 v2)...
  ;      x1 x2...)
  ;
  ; The *'s represent values which will signal if an lref
  ; instruction tries to fetch them out of the environment.

  (define (compile-letrec variables initializers body env more? val?)    
    (let ((prologue (_map2 (lambda (var init) `(set! ,var ,init))
			 variables
			 initializers))
	  (continuation (and more? (make-label 'letrec))))
      (append
       ;; We will call the letrec body with apply, so we must 
       ;; save a continuation if we are not in tail context.
       (code-if continuation `(save ,continuation))
       ;; push enough unspecified values to bind all the letrec
       ;; values
       (_map (lambda (_) '(unassn)) variables)
       (compile-procedure-body prologue variables body env #f #t)
       `((proc)
	 (apply ,(length variables)))
       (code-if continuation `(label ,continuation))
       (code-if (not val?) '(pop))
       (code-if (not more?) '(return)))))

  (define (compile-arguments args env)
    (let loop ((rest args)
	       (code '()))
      (if (null? rest)
	  code
	  (loop (cdr rest)
		; an argument slot cannot be tail-recursive, so we 
		; set more? to #t when compiling arguments.  Likewise,
		; their values are always needed.
		(append code (compile-exp (car rest) env #t #t))))))

  ; Arg-shape: analyze an argument list.  Returns a list; the 
  ; first element is the number of mandatory arguments and
  ; the second is #t if there are optional arguments.  The
  ; third element is the 'smoothed' list of argument names.
  ;
  ; arg list   shape
  ; x          (0 #t (x))
  ; (u v)      (2 #f (u v))
  ; (u . x)    (1 #t (u x))

  (define (arg-shape args)
    (let loop ((regular-args 0)
	       (rest args)
	       (flat '()))
      (cond
       ((null? rest)
	(list regular-args #f flat))
       ((pair? rest)
	(loop (+ regular-args 1) (cdr rest) (append flat (list (car rest)))))
       (else
	(list regular-args #t (append flat (list rest)))))))

  ; compile-procedure-body
  ; 
  ; Generate code to leave a compiled procedure on the top of the stack.
  ;
  ; Args is the argument list.  This can be improper, as can the first
  ; argument of (lambda).  Prologue contains a code sequence that should
  ; logically precede the execution of the body, but be evaluated in an
  ; environment in which all arguments and internal definitions are
  ; accessible (example: installation of the values of a letrec
  ; expression).  Body is the instruction sequence itself, which can 
  ; contain internal defines.  Env, more?, val? are used in the typical
  ; way.

  (define (compile-procedure-body prologue args body env more? val?)
    (let* ((shape (arg-shape args))
	   (nargs (car shape))
	   (extender (if (cadr shape) 'extend. 'extend))
	   (extended-env (extend-environment env (caddr shape))))
      ; Do we need to scan out defines?
      (list `(code ,(assemble
		     (append
		      `((,extender ,nargs))
		      (if prologue
			  (compile-simple-sequence prologue extended-env #t #f)
			  '())
		      (compile-sequence body extended-env more? val?)
		      ; procedures end with 'return': it's just that simple!
		      '((return))
		      ))))))

  ;; determine whether the code fragment in proc-code is merely a 
  ;; one-instruction reference to a symbol in the global environment,
  ;; and that symbol is a member of the set of inline procedures
  ;; we wish to invoke using the subr opcode (or a dedicated opcode).
  ;; If so, return the invoking code, else #f.

  (define (inline-procedure-exp? proc-code n-args) 
    (and
     (= (length proc-code) 1)
     (eq? (caar proc-code) 'gref)
     (let ((symbol (cadar proc-code)))
       (cond ((memq symbol *inline-procedures*)
	      `((,symbol ,n-args)))
	     ; Check to see if the function is a primitive procedure 
	     ; in this implementation: we can use a shortcut form 
	     ; of function invocation in that case.
	     ((and (bound? symbol)
		   (primitive-procedure? (symbol-value symbol)))
	      `((subr ,symbol ,n-args)))
	     (else #f)))))
  
  (define (compile-apply proc args env more? val?)
    (let* ((proc-code (compile-exp proc env #t #t))
	   (n-args (length args))
	   (inline-procedure (inline-procedure-exp? proc-code n-args))
	   (continuation (and more? 
			      (not inline-procedure)
			      (make-label 'cont))))
      (append 
       (code-if continuation `(save ,continuation))
       (compile-arguments args env)
       (or inline-procedure
	   (append
	    proc-code
	    `((apply ,n-args))))
       (code-if continuation `(label ,continuation))
       (if (not val?) `((pop)) '())
       (if (not more?) 
	   `((return))
	   '()))))
  
  (define (compile-do args env more? val?)
    (let* ((bindings (car args))
	   (test-exit (cadr args))
	   (test (car test-exit))
	   (exit (cdr test-exit))
	   (iterate (cddr args))
	   (loop-symbol 'do-loop)) ; XXX (gensym)
      (let* ((increment (let loop ((rest bindings)
				   (code '()))
			  (if (null? rest)
			      code
			      (if (null? (cddar rest))
				  (loop (cdr rest)
					  ; no step-expression: continue with 
					  ; variable name 
					(append code (list (caar rest))))
				  (loop (cdr rest)
					  ; insert step expression
					(append code (list (caddar rest))))))))
	     (augmented-body `((if ,test
				  (begin ,@exit)
				  (begin ,@iterate
					 (,loop-symbol ,@increment))))))

	(compile-let loop-symbol
		     (_map car bindings) 
		     (_map cadr bindings)
		     augmented-body
		     env more? val?))))

  ;;; =========================
  ;;; MACROS AND QUASIQUOTATION
  ;;; =========================

  ;; compile a macro: construct a let-expression which will bind the 
  ;; formals to the unevaluated actuals, including the body of the 
  ;; macro.  Evaluate this, and then compile the resulting code.

  (define (compile-macro macro args env more? val?)
    (let* ((formals (cadr macro))
	   (body (caddr macro))
	   (let-bindings (let loop ((bindings '())
				    (rest-formals formals)
				    (rest-actuals args))
			   (if (null? rest-formals) bindings
			       (loop
				(append bindings
					`((,(car rest-formals)
					   (quote ,(car rest-actuals)))))
				(cdr rest-formals)
				(cdr rest-actuals)))))
	   (macro-form `(let ,let-bindings ,body))
	   (expansion (eval macro-form)))
      (compile-exp expansion env more? val?)))
	      
  ;;; This Quasiquotation expander is based on that given in [PAIP p. 824], 
  ;;; translated into Scheme.  That implementation does not keep track of the
  ;;; "quasiquotation depth" as required by the R4/5 standard; that's fixed
  ;;; in the version here.

  (define (expand-quasiquotation form)

    (define (quasi-q depth x)
      (cond
       ((vector? x)
	(list 'list->vector (quasi-q depth (vector->list x))))
       ((not (pair? x))
	(if (constant? x) x (list 'quote x)))
       ((starts-with x 'unquote)
	(if (= depth 0)
	    (cadr x)
	    (combine-quasiquote (list 'quote 'unquote)
				(quasi-q (- depth 1) (cdr x)) x)))
       ((starts-with x 'quasiquote)
	; PAIP: (quasi-q (quasi-q (cadr x))))
	(combine-quasiquote (list 'quote 'quasiquote)
			    (quasi-q (+ depth 1) (cdr x)) x))

       ((starts-with (car x) 'unquote-splicing)
	; XXX respect QQ depth for unquote-splicing too!
	(if (null? (cdr x))
	    (cadr (car x))
	    (list 'append (cadr (car x)) (quasi-q depth (cdr x)))))
       (else
	(combine-quasiquote (quasi-q depth (car x)) 
			    (quasi-q depth (cdr x)) x))))
    
    (define (combine-quasiquote left right x)
      (cond ((and (constant? left) (constant? right))
	     (let ((eval-left (eval left))
		   (eval-right (eval right)))
	     (if (and (eqv? eval-left (car x))
		      (eqv? eval-right (cdr x)))
		 (list 'quote x)
		 (list 'quote (cons eval-left eval-right)))))
	    ((null? right)
	     (list 'list left))
	    ((starts-with right 'list)
	     (apply list 'list left (cdr right)))
	    (else
	     (list 'cons left right))))
    

    ;; Main entry point: Initiate quasiquotation expansion at depth zero.
    (quasi-q 0 form))

  ;;; Quasi-q refers to Common Lisp's (constantp); we implement
  ;;; that here as (constant?).

  (define (self-evaluating? form)
    (not (or (pair? form)
	     (symbol? form))))

  ;; For the purposes of quasiquotation, a form is Constant if
  ;; it's self-evaluating but not a symbol, or is the trivially 
  ;; constant form (quote <something>).

  (define (constant? x)
    (or (self-evaluating? x)
	(starts-with x 'quote)))

  ;;; ----------------------------------------------------------------------
  
  (define (compile-exp form env more? val?)
    (cond
     ((pair? form)
      ;; we must compute a compound's value no matter what, 
      ;; in the event there are side-effects; if the value
      ;; is not wanted, we discard it.
      (append (compile-compound form env more? #t)
	      (if val? '() '((pop)))))
     ((symbol? form)
      (append 
       (if val?
	   (let ((location (locate-local-variable env form)))
	     (if location
		 (list `(lref ,(car location) ,(cdr location)))
		 (list `(gref ,form))))
	   '())
       (if (not more?)
	   '((return))
	   '())))
     (else   ;self-evaluating
      (form-returning form more? val?))))
  
  (assemble (compile-exp form '() #f #t)))


;; ======
;; LINKER
;; ======
;;
;; The code produced from the compiler in the form of a tree of vectors,
;; and each instruction is represented in the form '(op arg...).  The 
;; "linker" phase collapses the nested vectors into a single linear
;; vector, fixing up offsets as it goes.  It also stores instructions
;; in a compact atom format using by calling into make-instruction.
;; The instruction-vector returned from this procedure is suitable for
;; execution by the C-language virtual machine.

;; XXX add instruction factory parameter and unify with link2.
(define (link program)
  (let ((output (make-vector 0))
	(output-index 0)
	(procedure-queue (make-vector 1 (cons program #f)))
	(literal-queue (make-vector 0)))
    (define (segment-relative-operand? opcode)
      (memq opcode '(save true? true?p false? false?p goto)))
    (define (process-one-procedure proc)
      (let* ((insns (car proc))
	     (n-insns (vector-length insns))
	     (section-offset (vector-length output))
	     (fixup (cdr proc)))
	(if fixup
	    ;; verify that the indicated slot has the fixup 
	    ;; token in it, then install the current output
	    ;; index.
	    (if (eq? (vector-ref output fixup) 'fixup)
		(vector-set! output fixup (list
					   'consti
					   (vector-length output)))
		(begin
		  (display (vector-ref output fixup))
		  (error "bad fixup"))))
	;; process instructions
	(do ((i 0 (+ i 1)))
	    ((= i n-insns) 'ok)
	  (let* ((insn (vector-ref insns i))
		 (opcode (car insn)))
	    (cond
	     ((eq? opcode 'code)
	      ;; we found another vector of instructions: add it
	      ;; to the queue to be flattened, consed with this 
	      ;; instruction's address, so the address can be 
	      ;; patched later.  Leave a fixup token in this insn
	      ;; slot.
	      (vector-push! output 'fixup)
	      (vector-push! procedure-queue (cons (cadr insn)
						  (- (vector-length output) 1))))
	     ((segment-relative-operand? opcode)
	      ;; if it's a branch or save instruction, the operand
	      ;; is an index relative to this segment, which must 
	      ;; be fixed up.
	      (vector-push! output (list opcode
					 (+ (cadr insn) section-offset))))
	     (else
	      ;; ordinary instruction
	      (vector-push! output insn)))))))
    ;; while there are still procedures on the queue, process them.
    (let loop ()
      (if (> (vector-length procedure-queue) 0)
	  (begin
	    (process-one-procedure (vector-shift! procedure-queue))
	    (loop))))
    output))

(define (link2 program)
  (let ((output (make-vector 0))
	(output-index 0)
	(procedure-queue (make-vector 1 (cons program #f)))
	(literal-queue (make-vector 0)))
    (define (segment-relative-operand? opcode)
      (memq opcode '(save true? true?p false? false?p goto)))
    (define (add-literal literal-queue literal)
      ;; Add the given literal to the vector and return the index.
      ;; Re-use an entry if one is already there.  XXX: linear search.
      (let loop ((index 0))
	(cond
	 ((= index (vector-length literal-queue))
	  ;; item wasn't found.  Add it.
	  (vector-push! literal-queue literal)
	  (- (vector-length literal-queue) 1))
	 ((equal? literal (vector-ref literal-queue index))
	  ;; found item: return index
	  index)
	 (else 
	  ;; keep looking
	  (loop (+ index 1))))))
    
    (define (process-one-procedure proc)
      (let* ((insns (car proc))
	     (n-insns (vector-length insns))
	     (section-offset (vector-length output))
	     (fixup (cdr proc)))
	(if fixup
	    ;; verify that the indicated slot has the fixup 
	    ;; token in it, then install the current output
	    ;; index.
	    (if (eq? (vector-ref output fixup) 'fixup)
		(vector-set! output fixup 
			     (make-instruction 'consti (vector-length output)))
		(begin
		  (display (vector-ref output fixup))
		  (error "bad fixup"))))
	;; process instructions
	(do ((i 0 (+ i 1)))
	    ((= i n-insns) 'ok)
	  (let* ((insn (vector-ref insns i))
		 (opcode (car insn)))
	    (cond
	     ((eq? opcode 'code)
	      ;; we found another vector of instructions: add it
	      ;; to the queue to be flattened, consed with this 
	      ;; instruction's address, so the address can be 
	      ;; patched later.  Leave a fixup token in this insn
	      ;; slot.
	      (vector-push! output 'fixup)
	      (vector-push! procedure-queue
			    (cons (cadr insn)
				  (- (vector-length output) 1))))
	     ((segment-relative-operand? opcode)
	      ;; if it's a branch or save instruction, the operand
	      ;; is an index relative to this segment, which must 
	      ;; be fixed up.
	      (vector-push! output (make-instruction
				    opcode
				    (+ (cadr insn) section-offset))))
	     ((eq? opcode 'const)
	      ;; pushing a literal value.  Add the value to the literal
	      ;; queue, and substitute and instruction that will reference
	      ;; it.
	      (let* ((operand (cadr insn))
		     (literal-index (add-literal literal-queue operand)))
		(vector-push! output (make-instruction 'lit literal-index))))
	     (else
	      ;; ordinary instruction
	      (vector-push! output (apply make-instruction insn))))))))
    ;; while there are still procedures on the queue, process them.
    (let loop ()
      (if (> (vector-length procedure-queue) 0)
	  (begin
	    (process-one-procedure (vector-shift! procedure-queue))
	    (loop))))
    ;; The internal format of a compiled procedure is a vector 
    ;; containing the instruction vector and the literal pool.
    (make-compiled-procedure output literal-queue)))

