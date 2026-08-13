;;
;; Copyright (c) 2004,2006 Colin Smith.
;;
;; bootstrap.scm: bootstraps the Scheme compiler by compiling itself
;; and serializing it to C.
;;
;; Expected arguments:
;; 1) Directory to "chdir" to
;; 2) Name of output file

(chdir (vector-ref *argv* 0))
(load "compiler.scm")

(define (comp-run exp)
  (execute (link2 (compile exp))))

;; load a file via the compiler's execution path
;; essentially this is a REPL into comp-run

(define (comp-load file)
  (let ((input (open-input-file file)))
    (do ((form (read input) (read input)))
        ((eof-object? form) #t)
      (comp-run form))))

(define (emit-compiled-procedures proc-list filename)
  ; for-each is a library procedure that we must compile; therefore
  ; it can't be used in the bootstrapper.  We provide a local replacement
  ; here.
  (define (_for-each proc list)
    (let loop ((rest list))
      (if  (null? rest) #t
           (begin (proc (car rest))
                  (loop (cdr rest))))))

  ; replace characters in a string, under a mapping represened in
  ; association-list form (e.g, the mapping '((#\a . #\b)) would
  ; map a's to b's).  If the right had side of the association is
  ; #f, then the matching character is deleted.
  (define (remap-characters mapping str)
    (let loop ((result "")
               (rest (string->list str)))
      (if (null? rest) result
          (let ((ch (car rest))
                (map-entry (assq (car rest) mapping)))
            (if (not map-entry) (loop (string-append result (string ch))
                                      (cdr rest))
                (if (cdr map-entry)
                    (loop (string-append result (string (cdr map-entry)))
                          (cdr rest))
                    (loop result
                          (cdr rest))))))))

  ; C++ symbols can't have hyphens, so we map them to underscores.
  ; This is not a general solution to the problem that Scheme
  ; identifiers draw from a richer character set than C++ identifiers:
  ; but it is sufficient for our purpose of bootstrapping the compiler.
  (define (c-name-from-scheme-name str)
    (remap-characters '((#\- . #\_) (#\. . #\_)) str))

  (define (c-name-from-symbol sym)
    (c-name-from-scheme-name (symbol->string sym)))

  ; We need to compile an 'eval' procedure, but actually compiling
  ; an eval would prevent the bootstrap interpreter from using eval,
  ; and that turns out to be annoying.  Instead we compile _eval,
  ; and use this routine to strip _'s from symbol names when
  ; serializing them.  Thus, when the _eval procedure is loaded in
  ; by the non-bootstrap VM, it will be called 'eval'.

  (define (scheme-name-from-symbol sym)
    (remap-characters '((#\_ . #f)) (symbol->string sym)))

  (with-output-to-file filename
    (lambda ()
      (let ((module-name (string-append (c-name-from-scheme-name filename)
                                        "_ext")))
        (display "#include \"vx-scheme.h\"\n\n")
        (_for-each
         (lambda (proc)
           (write-compiled-procedure (eval proc) (c-name-from-symbol proc)))
         proc-list)
        (display* "class " module-name " : SchemeExtension { \n"
                  "public:\n"
                  "  " module-name "() {\n"
                  "    Register(this);\n"
                  "  }\n"
                  "  virtual void Install(Context* ctx, Cell* envt) { \n"
                  "    static struct {const char* n; vm_cproc* cp;} b[] = {\n")
        (_for-each
         (lambda (proc)
           (display* "      { \"" (scheme-name-from-symbol proc) "\", &"
                     (c-name-from-symbol proc) " },\n"))
         proc-list)
        (display*
         "    };\n    const int nb = sizeof(b) / sizeof(*b);\n"
         "    for (int ix = 0; ix < nb; ++ix) {\n"
         "      // NB: GC is disabled during the loading of extensions.\n"
         "      ctx->set_var(envt, intern(b[ix].n),\n"
         "                   ctx->load_compiled_procedure(b[ix].cp));\n"
         "    };\n"
         "  };\n"
         "};\n\n"
         "static " module-name " _ext;\n")
        ))))

(define apply-code '#((code #((apply.)
                              (return)))
                      (proc)
                      (return)))

(define apply (execute (link2 apply-code)))

(define callcc-code '#((code #((extend 1)
                               (cc)
                               (lref 0 0)
                               (apply 1)
                               (return)))
                       (proc)
                       (return)))


(define _call-with-current-continuation (execute (link2 callcc-code)))

(comp-load  "compiler.scm")
(comp-load  "library.scm")

(comp-run '(define (_eval expr) (execute (link2 (compile expr)))))
(emit-compiled-procedures '(compile
                            assemble
                            link2)
                          "_compiler.cpp")

(emit-compiled-procedures '(apply
                            map
                            call-with-input-file
                            call-with-output-file
                            load
                            _eval
                            _call-with-current-continuation
                            for-each)
                          "_library.cpp")
