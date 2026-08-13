;;
;; Copyright (c) 2005,2006 and onwards Colin Smith.
;;
;; compile-file.scm: reads a file and compiles it to bytecode; dumps the 
;; bytecode which can then be linked to a compiler-free VM.  This essentially
;; allows for the the translation of Scheme code to VM code with the minimum
;; runtime (that this implementation allows for, anyway).
;;
;; Arguments expected:
;;
;; 1) Name of source file

(display "#include \"vx-scheme.h\"\n\n")

(define form-counter 0)

(define (get-form-name) 
  (set! form-counter (+ form-counter 1))
  (string-append "__f" (number->string form-counter)))

(define filename (vector-ref *argv* 0))

(with-input-from-file filename
  (lambda ()
    (let loop ((form (read)))
      (if (eof-object? form) '/**/
	  (begin
	   (write-compiled-procedure (link2 (compile form)) (get-form-name))
	   (loop (read)))))))

;; rewind the form counter and generate an expression that will invoke each 
;; of the forms in turn.

(define form-count form-counter)

(set! form-counter 0)
(define executor-form 
  (do
      ((form '(begin) (append form
			      (list (list (string->symbol (get-form-name))))))
       (i 0 (+ i 1)))
      ((= i form-count) form)))

(write-compiled-procedure (link2 (compile executor-form)) "__RUN")
  
;; Now write a load-and-go routine.

(set! form-counter 0)

(let ((module-name "module"))
  (display*
   "class " module-name " : SchemeExtension { \n"
   "public:\n"
   "  " module-name "() {\n"
   "    Register(this);\n"
   "  }\n"
   "  virtual ~" module-name "() {}\n"
   "  struct expression_table {\n"
   "    const char* n;      // expression name\n"
   "    vm_cproc* cp;       // compiled form of expression\n"
   "    Cell* c;            // cell holding procedure object\n"
   "  };\n"
   "  static expression_table exptab[];\n"
   "  static const int num_exps;\n"
   "  virtual void Install(Context* ctx, Cell* envt) { \n"
   "    for (int ix = 0; ix < num_exps; ++ix) {\n"
   "      exptab[ix].c = ctx->load_compiled_procedure(exptab[ix].cp);\n"
   "      ctx->set_var(envt, intern(exptab[ix].n), exptab[ix].c);\n"
   "    }\n"
   "    MainProcedure(this);\n"
   "  }\n"
   "  virtual Cell* Run(Context* ctx) {\n"
   "    return ctx->execute(exptab[num_exps-1].c, nil);\n"
   "  };\n"
   "};\n\n"
   "static " module-name " _mod;\n\n"
   "module::expression_table module::exptab[] = {\n")

  (do
      ((i 0 (+ i 1))
       (form-name (get-form-name) (get-form-name)))
      ((= i form-count) (display* "  { \"__RUN\", &__RUN, 0 }, \n"))
    (display* "  { \"" form-name "\", &" form-name ", 0 },\n"))
  
  (display*
   "};\n"
   "const int module::num_exps = "
   "sizeof(module::exptab) / sizeof(*module::exptab);\n"))


