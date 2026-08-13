//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// vx-scheme.h : class definitions

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#ifndef WIN32
#include <unistd.h>
#else
#include <io.h>
// We need to do bit manipulations on pointers in order to
// implement our storage model (garbage collection bits, etc.)
// MSVC quite properly complains about this, but since it's
// necessary in this case we squelch the warnings.
#pragma warning(disable : 4311)
#pragma warning(disable : 4312)
#endif
#if __GNUG__ >= 3
#endif
#if defined(__GNUC__)
// Statically allocated cells must lie upon an 8-byte
// boundary, so that the lower three bits of pointers
// to such objects are free for our use.
#define ALIGN8 __attribute__((aligned(8)))
#define PACKED __attribute__((packed))
#elif defined(WIN32)
#define PACKED
#define ALIGN8 __declspec(align(8))
#else
#error "must have a way of aligning Cells to 8-byte boundary"
#endif

class OS;
class Cell;
class Slab;
class Context;

// OS abstraction layer

class OS {
public:
  static double get_time();        // get timestamp
  static bool interactive(int fd); // terminal input?
  // supply value for undef symbol
  static Cell *undef(Context *, const char *);
  // report exception and restart
  static void exception();
  // manage debug flags
  static unsigned int flags();
  static bool flag(int bit) { return (flags() & bit) != 0; }

  // XXX global error buffer, set just before a longjmp to the
  // REPL.  This should be made context local.
  static const int ebufsize = 256;
  static char errbuf[ebufsize];
};

typedef Cell *(*subr_f)(Context *ctx, Cell *arglist);
typedef void (*magic_set_f)(Context *, void *key, Cell *rhs);
typedef Cell *(*magic_get_f)(Context *, void *key);
extern Cell *nil;
extern Cell *unspecified;
extern Cell *unassigned;
extern Cell *unimplemented;

void error(const char *, const char * = 0);

// FOR_EACH is a macro that can be used to traverse a standard Scheme
// list.  The variable `var' is bound for the duration of the traversal
// to each node in the list.

#define FOR_EACH(var, list)                                                    \
  for (Cell *var = list; var != nil; var = Cell::cdr(var))

#define INTERN_SYM(sym, symname) psymbol sym = intern(symname);

class cellvector {
public:
  // Acquire from Freelist
  static cellvector *alloc(int size);
  static cellvector *alloc(int size, int allocate);
  // Return to freelist
  void free();

  cellvector(int size = 0);
  cellvector(int size, int alloc);
  ~cellvector();

  Cell *get(int ix) {
    if (ix < 0 || ix >= sz)
      vref_error();
    return v[ix];
  }
  void set(int, Cell *);

  // used when you know the reference is in bounds.
  Cell *get_unchecked(int ix) { return v[ix]; }
  void set_unchecked(int ix, Cell *c) { v[ix] = c; }

  Cell *&operator[](int);
  Cell *top() {
    if (sz <= 0)
      vref_error();
    return v[sz - 1];
  }
  void push(Cell *c) {
    if (sz == allocated)
      expand();
    v[sz++] = c;
  }
  Cell *pop() {
    if (sz <= 0)
      vref_error();
    return v[--sz];
  }
  Cell *shift();
  void unshift(Cell *);

  int size() { return sz; }
  void discard(int n = 1) {
    if (n < 0 || n > sz)
      vref_error();
    sz -= n;
  }

  void clear();

private:
  void make_cv(int size, int alloc);
  void expand();
  void vref_error();
  int sz;
  int allocated;
  friend class Context; // Context::gc needs to see our gc_* members
  int gc_index;
  union {
    Cell *gc_uplink;
    cellvector *next_free;
  };
  Cell **v;

  // Freelist.  We keep allocated storage for "short" cellvectors.

  static const int keep_size = 4;
  static const int keep_count = 100;
  static cellvector *freelist_head[keep_size + 1];
  static int freelist_count[keep_size + 1];
};

// The symbol table is implemented as an AVL tree of these nodes.
// There's no repetition, so the address of one of these nodes can
// serve as a unique hashcode for a symbol for equality-testing
// purposes.  There's one call, intern(), for introducing a new
// string to the collection.
//
// The Scheme standard, however, introduces one complication: the
// requirement that symbols be stored in a "standard case."  This
// is in conflict with our desire to have case-sensitive symbol
// matching (for integration with underlying symbol tables).  (Scheme
// also provides the primitive string->symbol, which can be used to
// create symbols outside of standard case, but the REPL is not
// expected to use this.)
//
// In the end I decided to spend some extra memory to achieve standard
// compliance and VxWorks symbol table integration at the same time.
// We choose to consider lower-case symbols as "canonical".  (The standard
// says we must choose upper or lower case, but not which one).  In the event
// that a symbol arrives in mixed case, we store it both ways: canonically
// (that is, with lowered case) for Scheme symbol lookup, and unmolested
// so that, when we try the VxWorks symbol table after all else has failed,
// we can respect the case of the sybmol as written.

typedef struct _symbol {
  struct _symbol *llink; // Left binary tree link */
  struct _symbol *rlink; // Right binary tree link */
  const char *key;       // Search key (symbol name) */
  const char *truename;  // case-sensitive name, if diff. */
  cellvector *plist;     // property list */
  short b;               // Balance factor */
} symbol, *psymbol;

psymbol intern(const char *name);
psymbol intern_stet(const char *name);
Cell *vector_from_list(Context *ctx, Cell *);
Cell *vector_to_list(Context *ctx, Cell *);

// ------------------------------------------------------------------------
// class sio: input/output behavior we expect from strings or streams.
//            interface class.

class sio {
public:
  virtual ~sio() {}
  virtual int get() = 0;
  virtual int peek() = 0;
  virtual void unget() = 0;
  virtual void ignore() = 0;
};

// ------------------------------------------------------------------------
// class file_sio: This wraps a FILE* into an object that answers to
//                 the above interface.

class file_sio : public sio {
public:
  file_sio(FILE *_fp) : fp(_fp), lastch(-1){};

  virtual int get() { return lastch = fgetc(fp); }
  virtual int peek() {
    int c = get();
    ungetc(c, fp);
    return c;
  }
  virtual void unget() { ungetc(lastch, fp); }
  virtual void ignore() { get(); }

private:
  FILE *fp;
  int lastch;
};

// ------------------------------------------------------------------------
// An "sstring" is a simple extensible string.  It reallocates storage
// as necessary to support arbitrary growth.  It is a poor cousin to
// STL's string, but with considerably less code-bloat since there's no
// template expansion or nontrivial inlining.
//
// In order to avoid involving the strstream class, we also extend
// our sstream with a small amount of I/O semantics.  This allows
// sstrings to be passed to the lexical analyzer.

class sstring : public sio {
public:
  sstring();
  virtual ~sstring();

  char *str() { return base; }
  char &operator[](size_t ix) { return base[ix]; }

  void append(const char *);
  void append(const char *, size_t len);
  void append(const char);
  size_t length() { return sz; }
  void claim(); // claim dynamic storage
  bool operator==(const char *s) { return !strcmp(base, s); }

  // I/O behavior

  int get();
  int peek();
  bool eof();
  void unget();
  void ignore();

private:
  static const int stat_size = 32;
  char c[stat_size];

  size_t sz;
  size_t alloc;
  char *base;
  char *end;
  char *pos; // I/O read position
  bool claimed;
};

//----------------------------------------------------------------------
// class Cell
//
// The Cell is the heart of the Scheme implementation.  It is the
// universal container for all Scheme data types and also the central
// structure supporting Scheme's garbage-collected memory model.
// The economy of a Cell's realization is the single most significant
// factor influencing the speed and space efficiency of a Scheme
// system (with the possible exception of compilation, beyind the
// scope of this header file).
//
// We consider it imperative that an ordinary cell be no larger than
// two machine pointers (car and cdr); if a data object requires
// more storage than this, we allocate extension words.
//
// For our implementation, we expect that a machine pointer is at
// least four bytes, so that two of these (car,cdr) will occupy
// eight bytes.  In consequence, we may therefore insist that the
// storage for cells be 8-byte aligned, which gives us three bits at
// the least-significant end of a cell pointer to use as type-tagging
// information.

class Cell {
  friend class Context;
  friend class Slab;
  friend class InterpreterExt;

public:
  void display(FILE *);
  void write(FILE *) const;
  void write(sstring &) const;

  bool eq(Cell *c);
  bool eqv(Cell *c) { return eq(c); }

  bool equal(Cell *c);
  bool is_symbol(psymbol s) {
    return type() == Cell::Symbol && SymbolValue() == s;
  }

  struct Procedure {
    Procedure(Cell *_envt, Cell *_body, Cell *_arglist)
        : body(_body), arglist(_arglist), envt(_envt) {}

    Procedure() : body(nil), arglist(nil), envt(nil) {}

    Cell *body;
    Cell *arglist;
    Cell *envt;
  };

  // Certain cells we have heard of

  ALIGN8 static Cell Nil;
  ALIGN8 static Cell Unspecified;
  ALIGN8 static Cell Unassigned;
  ALIGN8 static Cell Eof_Object;
  ALIGN8 static Cell Bool_T;
  ALIGN8 static Cell Bool_F;
  ALIGN8 static Cell Apply;
  ALIGN8 static Cell Error;
  ALIGN8 static Cell Halt;
  ALIGN8 static Cell Unimplemented;

  // Access/Mutate Cons Cells.  These are checked calls, in
  // that they will verify that they are traversing a set of
  // cons cells at each step, using "assert_cons", which
  // throws a C++ exception if this is not found to be true.

  static void setcar(Cell *c, Cell *car) {
    atomic(c) ? notcons() : (c->ca.p = car);
  }
  static void setcdr(Cell *c, Cell *cdr) {
    atomic(c) ? notcons() : (c->cd.p = cdr);
  }
  static Cell *car(const Cell *c) { return atomic(c) ? notcons() : c->ca.p; }
  static Cell *cdr(const Cell *c) { return atomic(c) ? notcons() : c->cd.p; }
  static Cell *caar(Cell *c);
  static Cell *cadr(Cell *c);
  static Cell *cdar(Cell *c);
  static Cell *cddr(Cell *c);
  static Cell *caaar(Cell *c);
  static Cell *caadr(Cell *c);
  static Cell *cadar(Cell *c);
  static Cell *caddr(Cell *c);
  static Cell *cdaar(Cell *c);
  static Cell *cdadr(Cell *c);
  static Cell *cddar(Cell *c);
  static Cell *cdddr(Cell *c);
  static Cell *caaaar(Cell *c);
  static Cell *caaadr(Cell *c);
  static Cell *caadar(Cell *c);
  static Cell *caaddr(Cell *c);
  static Cell *cadaar(Cell *c);
  static Cell *cadadr(Cell *c);
  static Cell *caddar(Cell *c);
  static Cell *cadddr(Cell *c);
  static Cell *cdaaar(Cell *c);
  static Cell *cdaadr(Cell *c);
  static Cell *cdadar(Cell *c);
  static Cell *cdaddr(Cell *c);
  static Cell *cddaar(Cell *c);
  static Cell *cddadr(Cell *c);
  static Cell *cdddar(Cell *c);
  static Cell *cddddr(Cell *c);

  // "Boxes" to hold things related to atoms that won't fit in a cell.
  // We need one of these whenever the atom has two words or more of
  // data.  They are allocated from the heap and are freed when a gc'd
  // atom is finalized.

  struct SubrBox {
    subr_f subr;
    const char *name;
  };

  struct MagicBox {
    void *key;
    magic_set_f set_f;
    magic_get_f get_f;
  };

  // We store length with strings.  When these are allocated we
  // preallocate the string space; freeing this object discards
  // both box and string.

  struct StringBox {
    size_t length;
    char s[1];
  };

  // Value extractors

  intptr_t IntValue() const;
  char CharValue() const;
  SubrBox *SubrValue() const;
  char *StringValue() const;
  size_t StringLength() const;
  FILE *IportValue() const;
  FILE *OportValue() const;
  void *ContValue() const;
  cellvector *VectorValue() const;
  cellvector *CProcValue() const;
  Cell *PromiseValue() const;
  Cell *CPromiseValue() const;
  psymbol SymbolValue() const;
  psymbol BuiltinValue() const;
  Procedure LambdaValue() const;
  double RealValue() const;
  const char *name() const;

  // unsafe accessors: use when you have prior knowledge that the
  // cell contains an atom of the proper type.

  cellvector *unsafe_vector_value() const { return cd.cv; }

  static void real_to_string(double, char *, int);

  double asReal() const {
    if (type() == Cell::Int)
      return (double)IntValue();
    else
      return RealValue();
  }

  // In scheme, the only two values of type `boolean' are #t and
  // #f.  However, from the point of view of truth valuation,
  // anything other than #f is considered `true'.  We follow the
  // Scheme standard strictly, and so do not consider nil to have
  // a false connotation as it would in other dialects of Lisp.

  bool isBoolean() { return this == &Bool_T || this == &Bool_F; }
  bool istrue() { return this != &Bool_F; }
  bool ispair();

  static Cell *untagged(Cell *);

  // Utilities
  int length() {
    int i = 0;

    FOR_EACH(p, this)
    ++i;

    return i;
  }

  class List {
  public:
    List() : h(&Nil), t(&Nil) {}

    void append(Cell *c) {
      if (t == &Nil)
        h = t = c;
      else {
        Cell::setcdr(t, c);
        t = c;
      }
    }

    void append_list(Cell *list_head, Cell *list_tail) {
      if (h == &Nil) {
        h = list_head;
        t = list_tail;
      } else {
        Cell::setcdr(t, list_head);
        t = list_tail;
      }
    }

    Cell *head() { return h; }
    Cell *tail() { return t; }

  private:
    Cell *h; // head
    Cell *t; // tail
  };

  void list_append(Cell *&head, Cell *&tail) {
    if (tail == &Nil) {
      head = tail = this;
    } else {
      setcdr(tail, this);
      tail = this;
    }
  }

  static void stats();
  static void sanity_check();

  enum Type {
    //------------
    Int = 0, // The Atoms...
    Symbol = 1,
    Unique = 2,
    String = 3,
    Real = 4,
    Subr = 5,
    Lambda = 6,
    Vec = 7,
    Char = 8,
    Iport = 9,
    Oport = 10,
    Promise = 11,
    Cont = 12,
    Builtin = 13,
    Magic = 14,
    Insn = 15,
    Cproc = 16,
    Cpromise = 17,

    NUM_ATOMS = 18,
    //------------
    Cons = NUM_ATOMS, // A cell.
    NUM_TYPES = Cons + 1
    //------------
  };

  // If the ATOM bit is clear, it's a cons.  Otherwise, the type
  // is stored in the TYPEBITS field, unless it's a short integer.

  Type type() const {
    if (short_atom(this))
      return Int;
    return (Type)(((ca.i & (ATOM | SHORT)) == ATOM)
                      ? ((ca.i >> TAGBITS) & TYPEMASK)
                      : Cons);
  }

  void typecheck(Type t) const {
    if (type() != t)
      typefail(type(), t);
  }

  bool macro() const { return flag(MACRO); }

private:
  static inline bool short_atom(const Cell *c) {
    return (reinterpret_cast<uintptr_t>(c) & (ATOM | SHORT)) == (ATOM | SHORT);
  }
  static inline bool long_atom(const Cell *c) {
    return (reinterpret_cast<uintptr_t>(c) & (ATOM | SHORT)) == ATOM;
  }
  static inline bool atomic(const Cell *c) {
    return short_atom(c) || ((c->ca.i & (ATOM | SHORT)) == ATOM);
  }

  void gc_set_car(Cell *);
  void gc_set_cdr(Cell *);
  static Cell *notcons();

  Cell() { ca.p = cd.p = &Nil; }

  Cell(const char *unique_name) {
    ca.i = 0;
    set_type(Unique);
    cd.u = unique_name;
  }

  void typefail(Type t1, Type t2) const;

  // The lowest order three bits of a pointer are called the
  // tagbits.  They are always free for our use, since a cell
  // consists of two words, each at least 32 bits, with the
  // natural alignment (8 bytes for a 32-bit machine).

  static const uintptr_t TAGBITS = 3;
  static const uintptr_t ATOM = 0x1;
  static const uintptr_t MARK = 0x2;
  static const uintptr_t SHORT = 0x4;

  static const uintptr_t TYPEBITS = 5;
  static const uintptr_t TYPEMASK = (1 << TYPEBITS) - 1;
  static const uintptr_t TAGMASK = (1 << TAGBITS) - 1;
  // Make sure flag bits are disjoint from TYPE and TAG bits.
  static const uintptr_t FLAGBASE = 1 << (TYPEBITS + TAGBITS);
  static const uintptr_t FORCED = FLAGBASE;
  static const uintptr_t QUICK = FLAGBASE << 1;
  static const uintptr_t GLOBAL = FLAGBASE << 2;
  static const uintptr_t MACRO = FLAGBASE << 3;
  static const uintptr_t VREF = FLAGBASE << 4;
  static const uintptr_t FREE = FLAGBASE << 5;
  static const uintptr_t FLAGBITS = 6;

  static const int GLOBAL_ENV = -1;

// Warning! The virtual machine instructions use the upper
// 16 bits of the car for the opcode, and count field,
// so space for types, tags, and flags is limited to 16 bits.

#if TAGBITS + TYPEBITS + FLAGBITS > 16
#error too many atom bits used
#endif

  inline intptr_t e_skip() {
    // If global symbol, return -1.  Else number of environments
    // to skip is in highest-order byte
    return (ca.i & GLOBAL) ? GLOBAL_ENV
                           : (int)((ca.i >> (8 * (sizeof(ca.i) - 1))) & 0xff);
  }

  inline intptr_t b_skip() {
    // If global symbol, number of bindings to skip is in upper 16
    // bits; else, it's in 2nd-highest-order byte
    return (ca.i & GLOBAL) ? (ca.i >> (8 * (sizeof(ca.i) - 2)) & 0xffff)
                           : ((ca.i >> (8 * (sizeof(ca.i) - 2))) & 0xff);
  }

  void set_lexaddr(intptr_t e_skip, intptr_t b_skip) {
    // If global, set flag and store b_skip in upper 16 bits.
    // Else set e_skip in upper 8 bits, and set b_skip in
    // next 8 bits.
    const intptr_t start_bit = 8 * (sizeof(ca.i) - 2);
    const intptr_t two_bytes = (1 << 16) - 1;
    ca.i &= ~(two_bytes << start_bit);
    if (e_skip == -1)
      ca.i |= (b_skip << start_bit) | GLOBAL | QUICK;
    else
      ca.i |= ((e_skip << 8 | b_skip) << start_bit) | QUICK;
  }

  // The set of bits which should be ignored when
  // comparing two cells in the sense of "eq?".  We ignore the
  // pieces having to do with lexical addresses.

  static const unsigned int IGNORE =
      (unsigned int)(QUICK | GLOBAL | (~0u << 16));
  static const unsigned int IGN_MASK = ~IGNORE;

  static const char *typeName[NUM_TYPES];
  static int typeCount[NUM_TYPES];

  void flag(unsigned int f, bool b) {
    if (b)
      ca.i |= f;
    else
      ca.i &= ~f;
  }

  void dump(FILE *);
  bool flag(unsigned int f) const {
    // only non-short atoms can have flags.  All requested bits must be set
    return (ca.i & (f | SHORT | ATOM)) == (f | ATOM);
  }

  void set_type(Type t) {
    if (t != Cons)
      ca.i |= (t << TAGBITS) | ATOM;
    ++typeCount[t];
  }

  // The actual data for an Atom/Cell is here.

  union _car {
    uintptr_t i;
    Cell *p;
  } ca;
  union _cdr {
    uintptr_t i;
    double *d;
    Cell *p;
    const char *u;
    SubrBox *f;
    MagicBox *m;
    StringBox *s;
    psymbol y;
    Cell *e;
    cellvector *cv;
    FILE *ip;
    FILE *op;
    char c;
    void *vp;
    void *j;
  } cd;
};

//----------------------------------------------------------------------
// class Environment
//
// At the simplest level, an Environment is a mapping from symbols
// to values.  Symbols are the hash codes maintained by the SymbolTable
// class, and the value of any symbol is simply a pointer to a Scheme
// cell.  To implement this simple data structure, we use an STL vector
// of <symbol, Cell*> pairs.  This choice of data structure is guided
// by some particularities of evaluation in Scheme (discussed below).
//
// Environments are created by binding constructs (like let and lambda),
// and a new environment is always linked to the environment in force
// when it was created (this is called the "enclosing environment").
// The enclosure chain always terminates at the global environment, which
// is where the symbols representing the language's standard features
// are bound.
//
// In Scheme, all variables are "lexically bound."  This means that
// when a variable is mentioned in source code, one can determine the
// binding for that variable at "compile time" by looking through the
// stack of bindings crated by special forms capable of creating such
// bindings (e.g., lambda, let, et al.).  The innermost matching binding
// found represents the storage for the value of the variable, and
// this can never change.
//
// This binding model creates the possibility of lexcial addressing, a
// system in which a variable reference can be replaced by the "index"
// of the storage in terms of the number of enclosing environments
// that must be traversed together with the index of the target
// variable within that environment.  This represents an extremely
// efficient shortcut for variable value lookup.  This is why we
// choose the vector data structure rather than an STL map: while a
// vector is slower to search the first time a variable is referenced,
// that initial search will reveal the "lexical address" of the
// variable, which we can then store in place of the referring symbol.
// It is therefore necessary that variable storage in an environment
// never move, once allocated.  The simplest way to guarantee this is
// to manage the bindings ourselves in a vector; the lexical address
// can then be stored in the simple form of two integers and does not
// depend on peculiarities of the data-structure implementation.
//
// We overload the concept of Environment with other data needed to
// evaluate Scheme expressions.  For example, Scheme I/O primitives
// like `with-input-from-file' provide for the presence of a stack
// of open files which we maintain in this structure.

class Context {

public:
  friend class Cell;
  friend class Slab;
  friend class VmLibExtension;

  Context();

  // Argument and environment manipulation for the VM.

  Cell *extend(Cell *env);
  Cell *extend(Cell *env, Cell *blist);
  Cell *extend_from_vector(Cell *env, cellvector *cv, int n);
  void adjoin(Cell *env, Cell *val);
  Cell *pop_list(int n);
  int push_list(Cell *);

  // "Binding" is the process of asserting a value for a
  // variable in the given environment.  That is, we do
  // not search upward in the enclosure chain for an
  // existing binding; we create one in the current environement.
  // (The contrast is with `set', which does perform such
  // a search.

  void bind(Cell *env, Cell *c, Cell *value);
  void bind_arguments(Cell *env, Cell *vars, Cell *values);
  void bind_subr(const char *name, subr_f subr);
  Cell *find_var(Cell *env, psymbol var, unsigned int *index);
  void set_var(Cell *env, psymbol var, Cell *value) {
    set_var(env, var, value, 0);
  }
  void set_var(Cell *env, psymbol var, Cell *value, unsigned int *index);
  void set_var(psymbol var, Cell *value, unsigned int *index) {
    set_var(root_envt, var, value, index);
  }

  // When new bindings are created, the existing environment
  // is _extended_ with a vector of new {variable,value} bindings
  // provided in parallel-list form.

  // Getting and Setting values in an environment is slightly
  // different from binding: `get' will search the enclosure
  // chain if necessary, returning the innermost matching binding.
  // Set does the same.  Both of these will signal an error if
  // a binding cannot be found (they will not establish one: only
  // bind can do that).

  Cell *get(Cell *env, Cell *c);
  void set(Cell *env, Cell *var, Cell *value);

  // root : find the "root" (i.e., parentless) environment
  // which contains this one.

  bool read_eval_print(FILE *in, FILE *out, bool);
  Cell *root() { return root_envt; }
  void gc();
  void gc_if_needed();
  void print_mem_stats(FILE *);

  // "Switching" evaluator: calls the interpreter to evaluate if
  // present; else the compiler.

  Cell *eval(Cell *form);

  // Returns true if we are using the bytecode VM.
  bool using_vm() const;

  // Interpreting evaluator

  Cell *interp_evaluator(Cell *form);
  Cell *(Context::*interp_eval)(Cell *form);

  // VM for compiled code.
  // It might not be linked in, in an interpreter-only
  // build.  The function pointer is used to connect it
  // if it is present.

  Cell *execute(Cell *form, Cell *args);
  Cell *(Context::*vm_execute)(Cell *form, Cell *args);
  Cell *vm_evaluator(Cell *form);
  Cell *(Context::*vm_eval)(Cell *form);

  // Convert text to live cells

  Cell *read(sio &);
  Cell *read(FILE *);

  // Manufacture Cells and Atoms

  Cell *make();
  Cell *make_int(intptr_t i);
  Cell *make_char(char ch);
  Cell *make_real(double d);
  Cell *make_string(size_t len);
  Cell *make_string(int len, char ch);
  Cell *make_string(const char *s);
  Cell *make_string(const char *s, size_t len);
  Cell *make_subr(subr_f s, const char *name);
  Cell *make_builtin(psymbol y);
  Cell *make_symbol(psymbol y);
  Cell *make_boolean(bool b);
  Cell *make_vector(int n, Cell *init = &Cell::Unspecified);
  Cell *make_iport(const char *fname);
  Cell *make_iport(FILE *);
  Cell *make_oport(const char *fname);
  Cell *make_oport(FILE *op);
  Cell *make_procedure(Cell *env, Cell *body, Cell *arglist);
  Cell *make_promise(Cell *env, Cell *body);
  Cell *make_macro(Cell *env, Cell *body, Cell *arglist);
  Cell *make_magic(void *, magic_set_f, magic_get_f);
  Cell *make(Cell *ca, Cell *cd = &Cell::Nil);
  Cell *make_list1(Cell *);
  Cell *make_list2(Cell *, Cell *);
  Cell *make_list3(Cell *, Cell *, Cell *);
  Cell *make_instruction(Cell *insn);
  Cell *make_instruction(int opcode, Cell *operands);
  Cell *make_compiled_procedure(Cell *insns, Cell *literals, Cell *envt,
                                int start);
  Cell *make_compiled_promise(Cell *procedure);
  Cell *force_compiled_promise(Cell *promise);
  Cell *make_continuation();
  void load_continuation(Cell *cont);
  void print_insn(int pc, Cell *insn);
  Cell *write_compiled_procedure(Cell *arglist);
  Cell *load_compiled_procedure(struct vm_cproc *);
  Cell *load_instructions(vm_cproc *);

  Cell *cons(Cell *_car, Cell *_cdr) { return make(_car, _cdr); }

  // ------------------------------------------------------------

  void with_input(const char *fname) { istack.push(make_iport(fname)); }

  void with_output(const char *fname) { ostack.push(make_oport(fname)); }

  void without_output() { fflush(ostack.pop()->OportValue()); }

  void without_input() { istack.pop(); }

  Cell *current_output() { return ostack.top(); }
  Cell *current_input() { return istack.top(); }

  // Protection from garbage collection (cell pointers not contained
  // in "register machine" variables need to be treated this way.
  // The variables are protected/unprotected in strict LIFO order.

  Cell *gc_protect(Cell *c) {
    r_gcp.push(c);
    return c;
  }
  void gc_unprotect(int ncells = 1) { r_gcp.discard(ncells); }

  // If the VM has a main procedure linked in, run it and return
  // the result; otherwise return NULL (a signal that the driver
  // program should enter interactive mode).   In the event that
  // a value is returned, the caller will probably want to print
  // it.

  Cell *RunMain();

private:
  Cell *alloc(Cell::Type t);
  void mark(Cell *);
  Cell *find(Cell *env, Cell *s);
  void quicken(Cell *, int, int);
  Cell *eval_list(Cell *list);
  void provision();
  void init_machine();
  void print_vm_state();
  void *xmalloc(size_t);

  // ===========================
  // Machine Stack Operations

  // The machine stack is just a cellvector, with one difference:
  // it can hold integers (marked with the ATOM flag) as well as
  // cell pointers.  (There are thus only 31 bits in these integers,
  // but that's way more than enough to hold the virtual machine
  // state.

  void save(Cell *c) { m_stack.push(c); }
  void save(Cell &rc) {
    m_stack.push(rc.ca.p);
    m_stack.push(rc.cd.p);
  }
  void save_i(intptr_t i) {
    m_stack.push(reinterpret_cast<Cell *>((i << 1) | Cell::ATOM));
  }
  void restore(Cell *&c) { c = m_stack.pop(); }
  void restore(Cell &rc) {
    rc.cd.p = m_stack.pop();
    rc.ca.p = m_stack.pop();
  }
  void restore_i(intptr_t &i) {
    i = (reinterpret_cast<intptr_t>(m_stack.pop()) &
         static_cast<intptr_t>(~Cell::ATOM)) >>
        1;
  }

  // ===========================
  // REGISTER MACHINE
  // ===========================

  Cell *r_exp;        // expression to evaluate
  Cell *r_env;        // evaluation environment
  Cell *r_unev;       // args awaiting evaluation
  Cell r_argl;        // (head,tail) of argument list
  Cell r_varl;        // (head,tail) of binding list
  Cell *r_proc;       // procedure to apply
  Cell *r_val;        // value resulting from evaluation
  Cell *r_tmp;        // temporary values
  Cell *r_elt;        // elements assembled into lists
  Cell *r_nu;         // reference to objects being created
  int r_qq;           // quasiquotation depth
  cellvector r_gcp;   // extra cells protected from GC
  intptr_t r_cont;    // current continuation
  cellvector m_stack; // recursion/evaluation stack
  int state;          // current machine state

  // We added a different set of registers for the compiler VM.
  // this avoids GC collisions when the interpreter is invoking
  // compiled procedures.  In the event vx-scheme is configured
  // to use only one of the interpreter or compiler, there are
  // some slots here that will be unused, but only one per execution
  // context.

  Cell *r_envt;  // environment
  Cell *r_cproc; // current compiled procedure.

  // The assembled instructions to resume a saved continuation
  Cell *cc_procedure;
  Cell *empty_vector;

  // ===========================

  // routines to append elements to lists (used with r_argl and r_varl).
  // Note: r_argl and r_varl MUST be maintained as correctly-formed
  // lists, since we use unsafe car/cdr to traverse them.

  void l_appendtail(Cell &l, Cell *t) {
    if (l.ca.p == nil)
      l.ca.p = l.cd.p = t;
    else {
      l.cd.p->cd.p = t; // l.cd.p->setcdr (t);
      l.cd.p = t;
    }
  }

  void l_append(Cell &l, Cell *t) {
    r_elt = make(t);
    l_appendtail(l, r_elt);
  }

  void clear(Cell &c) { c.ca.p = c.cd.p = nil; }

  Cell *envt;
  Cell *root_envt;
  Cell *eval_cproc;

  cellvector istack; // stack of input ports (with-input...)
  cellvector ostack; // stack of output ports (with-output...)

  struct Memory {
    cellvector active; // list of allocated Slabs
    Cell *free;        // freelist of cells
    int c_free;        // count of free cells
    Slab *current() { return (Slab *)active.top(); }
    bool low_water;     // true if next exhaustion should alloc
    bool last_alloc_gc; // true if last allocation provoked gc
    bool no_inline_gc;  // don't try gc on allocation failure

    Memory() : active() {
      free = 0;
      c_free = 0;
      low_water = last_alloc_gc = no_inline_gc = false;
    }
  };

  bool ok_to_gc;
  Memory mem;

  int cellsAlloc;
  int cellsTotal;
};

class VxSchemeInit {
public:
  VxSchemeInit() {
    // Do sanity checks before scheme runs
    Cell::sanity_check();
  }

  ~VxSchemeInit() {
    // Print statistics when scheme exits.
    Cell::stats();
  }
};

class SchemeExtension {
public:
  virtual ~SchemeExtension() {}
  static void Register(SchemeExtension *ext);
  static void RunInstall(Context *, Cell *);
  static void MainProcedure(SchemeExtension *m) { main = m; }
  static bool HaveMain() { return main != NULL; }
  static Cell *RunMain(Context *ctx) { return main->Run(ctx); }

  virtual void Install(Context *, Cell *) = 0;

private:
  virtual Cell *Run(Context *) { return &Cell::Bool_F; }
  static cellvector *extensions;
  static SchemeExtension *main;
};

// Simple accessors to avoid the Cell:: scope, which we don't
// really need for simple things like 'car'.

inline Cell *car(Cell *c) { return Cell::car(c); }
inline Cell *caar(Cell *c) { return Cell::caar(c); }
inline Cell *cdr(Cell *c) { return Cell::cdr(c); }
inline Cell *cdar(Cell *c) { return Cell::cdar(c); }
inline Cell *cadr(Cell *c) { return Cell::cadr(c); }
inline Cell *cddr(Cell *c) { return Cell::cddr(c); }
inline Cell *cadar(Cell *c) { return Cell::cadar(c); }
inline Cell *caddr(Cell *c) { return Cell::caddr(c); }
inline Cell *caadr(Cell *c) { return Cell::caadr(c); }
inline Cell *cdadr(Cell *c) { return Cell::cdadr(c); }
inline Cell *cddar(Cell *c) { return Cell::cddar(c); }
inline Cell *caddar(Cell *c) { return Cell::caddar(c); }
inline Cell *cadaar(Cell *c) { return Cell::cadaar(c); }

// Certain syntactic features of Scheme (so-called "syntactic sugar"
// like the `else' clause in a cond statement, the use of `.' to
// construct improper lists and "varargs lambdas", and some of the
// mechanics of quasiquotation) are most easily implemented if we have
// predefined symbols for these tokens.  They are not part of the
// global environment, however, and have no definitions themselves.
// We create them with global scope (in the `C' sense) as they can
// serve as invariant hashcodes throughout any universe of Scheme
// execution: there is never any need to compute their values more
// than once, even for multiple threads.

extern psymbol s_dot;
extern psymbol s_quote;
extern psymbol s_quasiquote;
extern psymbol s_unquote;
extern psymbol s_unquote_splicing;
extern psymbol s_passto;
extern psymbol s_else;
extern psymbol s_time;
extern psymbol s_eval;
extern psymbol s_foreach;
extern psymbol s_load;
extern psymbol s_map;
extern psymbol s_apply;
extern psymbol s_force;
extern psymbol s_delay;
extern psymbol s_defmacro;
extern psymbol s_withinput;
extern psymbol s_withoutput;
extern psymbol s_callwof;
extern psymbol s_callwif;

// We treat special forms similarly.

extern psymbol s_if;
extern psymbol s_define;
extern psymbol s_quote;
extern psymbol s_begin;
extern psymbol s_set;
extern psymbol s_or;
extern psymbol s_and;
extern psymbol s_lambda;
extern psymbol s_let;
extern psymbol s_letstar;
extern psymbol s_letrec;
extern psymbol s_do;
extern psymbol s_cond;
extern psymbol s_case;
extern psymbol s_callcc;

// Execution flags

#define TRACE_EVAL 0x01
#define TRACE_GC 0x02
#define DEBUG_NO_INLINE_GC 0x04
#define DEBUG_MEMSTATS_AT_EXIT 0x08
#define DEBUG_PRINT_PROCEDURES 0x10
#define TRACE_GC_ALL 0x20
#define TRACE_VM 0x40
#define TRACE_VMSTACK 0x80
#define COUNT_INSNS 0x100

// Typedefs for compiled procedures in C form.  It's possible to serialize
// a compiled procedure into a C data structure that can be used to load
// the bytecode.

typedef unsigned char byte;

#if defined(WIN32)
#pragma pack(push, 1)
#endif
struct vm_insn {
  byte opcode;
  byte count;
  const void *operand;
} PACKED;
#if defined(WIN32)
#pragma pack(pop)
#endif

struct vm_cproc {
  vm_insn *insns;
  unsigned int n_insns;
  const char **literals;
  unsigned int n_literals;
  int entry;
};
