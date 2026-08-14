//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// vx-scheme.h : class definitions

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <coroutine>
#include <string>
#include <string_view>
#include <unistd.h>
#include <variant>

template <class... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct Step {
  struct promise_type {
    Step get_return_object() {
      return Step{std::coroutine_handle<promise_type>::from_promise(*this)};
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    std::suspend_always yield_value(bool) noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() { std::terminate(); }
  };

  std::coroutine_handle<promise_type> handle = nullptr;

  bool done() const { return !handle || handle.done(); }
  bool next() {
    if (handle && !handle.done()) {
      handle.resume();
      return !handle.done();
    }
    return false;
  }

  Step(std::coroutine_handle<promise_type> h = nullptr) : handle(h) {}
  Step(const Step &) = delete;
  Step &operator=(const Step &) = delete;
  Step(Step &&o) noexcept : handle(o.handle) { o.handle = nullptr; }
  Step &operator=(Step &&o) noexcept {
    if (this != &o) {
      if (handle)
        handle.destroy();
      handle = o.handle;
      o.handle = nullptr;
    }
    return *this;
  }
  ~Step() {
    if (handle)
      handle.destroy();
  }
};

class OS;
class Cell;
class Slab;
class Context;

// Execution / Debug flags

enum class DebugFlag : uint32_t {
  TraceEval = 1 << 0,
  TraceGc = 1 << 1,
  NoInlineGc = 1 << 2,
  MemstatsAtExit = 1 << 3,
  PrintProcedures = 1 << 4,
  TraceGcAll = 1 << 5,
  TraceVm = 1 << 6,
  TraceVmStack = 1 << 7,
  CountInsns = 1 << 8,
};

uint32_t debug_flags();
inline bool debug_flag(DebugFlag bit) {
  return (debug_flags() & static_cast<uint32_t>(bit)) != 0;
}
double vx_get_time();

using subr_f = Cell *(*)(Context *ctx, Cell *arglist);
extern Cell *nil;
extern Cell *unspecified;
extern Cell *unassigned;
extern Cell *unimplemented;

void error(std::string_view s1, std::string_view s2 = {});

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

  Cell *get(int ix) const {
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

  int size() const { return sz; }
  void discard(int n = 1) {
    if (n < 0 || n > sz)
      vref_error();
    sz -= n;
  }

  void clear();

private:
  void make_cv(int size, int alloc);
  void expand();
  void vref_error() const;
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

struct symbol {
  std::string name;
  std::string truename_str;
  const char *key = nullptr; // Search key (symbol name, points to name.c_str())
  const char *truename = nullptr; // case-sensitive name, if diff
  cellvector *plist = nullptr; // property list
};
using psymbol = symbol *;

psymbol intern(std::string_view name);
psymbol intern(const char *name);
psymbol intern(const std::string &name);
psymbol intern_stet(std::string_view name);
psymbol intern_stet(const char *name);
psymbol intern_stet(const std::string &name);
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
  file_sio(FILE *_fp) : fp(_fp), lastch(-1) {};

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
// An "sstring" is an extensible string with I/O streaming semantics.

class sstring : public sio {
public:
  sstring() = default;
  explicit sstring(std::string_view sv) : m_str(sv) {}

  const char *str() const { return m_str.c_str(); }
  char *str() { return m_str.data(); }
  const std::string &string() const { return m_str; }
  std::string_view view() const { return m_str; }

  char &operator[](size_t ix) { return m_str[ix]; }
  const char &operator[](size_t ix) const { return m_str[ix]; }

  void append(std::string_view sv) { m_str.append(sv); }
  void append(char ch) { m_str.push_back(ch); }
  void append(const char *s) {
    if (s)
      m_str.append(s);
  }
  void append(const char *s, size_t len) {
    if (s)
      m_str.append(s, len);
  }

  size_t length() const { return m_str.size(); }
  size_t size() const { return m_str.size(); }
  bool empty() const { return m_str.empty(); }
  void clear() {
    m_str.clear();
    m_pos = 0;
  }

  bool operator==(std::string_view sv) const { return m_str == sv; }

  // I/O behavior
  int get() override {
    if (m_pos < m_str.size())
      return static_cast<unsigned char>(m_str[m_pos++]);
    return EOF;
  }
  int peek() override {
    if (m_pos < m_str.size())
      return static_cast<unsigned char>(m_str[m_pos]);
    return EOF;
  }
  bool eof() const { return m_pos >= m_str.size(); }
  void unget() override {
    if (m_pos > 0)
      --m_pos;
  }
  void ignore() override {
    if (m_pos < m_str.size())
      ++m_pos;
  }

private:
  std::string m_str;
  size_t m_pos = 0;
};

//----------------------------------------------------------------------
// class Cell
//
// The Cell is the heart of the Scheme implementation.  It is the
// universal container for all Scheme data types and also the central
// structure supporting Scheme's garbage-collected memory model.
//
// In modern vxs, Cell is a compact std::variant-backed value type
// managed within Slab memory pools. Immediate integers are encoded
// as tagged pointers (ATOM bit tag) requiring zero heap allocation.
// Slabs ensure 2-byte minimum alignment for tagged pointer safety.

class Cell {
  // friend class Context;
  // friend class Slab;
  // friend class InterpreterExt;

public:
  void display(FILE *);
  void write(FILE *) const;
  void write(sstring &) const;

  bool eq(Cell *c);
  bool eqv(Cell *c) { return eq(c); }
  bool equal(Cell *c);
  bool is_symbol(psymbol s) const;

  struct Procedure {
    Procedure(Cell *_envt, Cell *_body, Cell *_arglist)
        : body(_body), arglist(_arglist), envt(_envt) {}
    Procedure() : body(nullptr), arglist(nullptr), envt(nullptr) {}
    bool operator==(const Procedure &o) const {
      return body == o.body && arglist == o.arglist && envt == o.envt;
    }
    Cell *body;
    Cell *arglist;
    Cell *envt;
  };

  // Static pre-allocated cells
  static Cell Nil;
  static Cell Unspecified;
  static Cell Unassigned;
  static Cell Eof_Object;
  static Cell Bool_T;
  static Cell Bool_F;
  static Cell Apply;
  static Cell Error;
  static Cell Halt;
  static Cell Unimplemented;

  struct Cons {
    Cell *car = nullptr;
    Cell *cdr = nullptr;
    bool operator==(const Cons &o) const = default;
  };
  struct Subr {
    subr_f subr = nullptr;
    const char *name = nullptr;
    bool operator==(const Subr &o) const = default;
  };
  struct LexAddr {
    static constexpr int16_t UNQUICKENED = -2;
    static constexpr int16_t GLOBAL_ENV = -1;

    int16_t e_skip = UNQUICKENED;
    int16_t b_skip = UNQUICKENED;
    bool operator==(const LexAddr &o) const = default;
  };

  struct Insn {
    using Payload = std::variant<std::monostate, // OP_NONE
                                 intptr_t,       // OP_INT
                                 psymbol,        // OP_SYMBOL
                                 const Subr *,   // OP_SUBR (quickened)
                                 LexAddr         // OP_LEXADDR
                                 >;

    unsigned int opcode = 0;
    unsigned int count = 0;
    Payload payload = std::monostate{};

    bool operator==(const Insn &o) const = default;

    psymbol Symbol() const {
      if (std::holds_alternative<psymbol>(payload))
        return std::get<psymbol>(payload);
      return nullptr;
    }

    intptr_t int_val() const {
      if (std::holds_alternative<intptr_t>(payload))
        return std::get<intptr_t>(payload);
      return 0;
    }

    LexAddr lex_addr() const {
      if (std::holds_alternative<LexAddr>(payload))
        return std::get<LexAddr>(payload);
      return LexAddr{};
    }

    const Subr *subr_val() const {
      if (std::holds_alternative<const Subr *>(payload))
        return std::get<const Subr *>(payload);
      return nullptr;
    }

    bool is_quickened_subr() const {
      return std::holds_alternative<const Subr *>(payload);
    }
  };

  struct Symbol {
    static constexpr int16_t UNQUICKENED = -2;
    static constexpr int16_t GLOBAL_ENV = -1;

    psymbol s = nullptr;
    int16_t e_skip = UNQUICKENED;
    int16_t b_skip = UNQUICKENED;

    bool is_quickened() const { return e_skip != UNQUICKENED; }
    bool is_global() const { return e_skip == GLOBAL_ENV; }
    bool operator==(const Symbol &o) const { return s == o.s; }
  };
  struct Builtin {
    psymbol s = nullptr;
    bool operator==(const Builtin &o) const { return s == o.s; }
  };
  struct Unique {
    const char *s = nullptr;
    bool operator==(const Unique &o) const { return s == o.s; }
  };
  struct Lambda {
    cellvector *cv = nullptr;
    bool operator==(const Lambda &o) const { return cv == o.cv; }
  };
  struct Vec {
    cellvector *cv = nullptr;
    bool operator==(const Vec &o) const { return cv == o.cv; }
  };
  struct Iport {
    FILE *f = nullptr;
    bool operator==(const Iport &o) const { return f == o.f; }
  };
  struct Oport {
    FILE *f = nullptr;
    bool operator==(const Oport &o) const { return f == o.f; }
  };
  struct Promise {
    cellvector *cv = nullptr;
    bool operator==(const Promise &o) const { return cv == o.cv; }
  };
  struct Cont {
    cellvector *cv = nullptr;
    bool operator==(const Cont &o) const { return cv == o.cv; }
  };
  struct Cproc {
    cellvector *cv = nullptr;
    bool operator==(const Cproc &o) const { return cv == o.cv; }
  };
  struct Cpromise {
    cellvector *cv = nullptr;
    bool operator==(const Cpromise &o) const { return cv == o.cv; }
  };
  struct Free {
    Cell *next = nullptr;
    bool operator==(const Free &o) const { return next == o.next; }
  };

  using CellValue = std::variant<intptr_t,    // 0: Int
                                 Symbol,      // 1: Symbol
                                 Unique,      // 2: Unique
                                 std::string, // 3: String
                                 double,      // 4: Real
                                 Subr,        // 5: Subr
                                 Lambda,      // 6: Lambda
                                 Vec,         // 7: Vec
                                 char,        // 8: Char
                                 Iport,       // 9: Iport
                                 Oport,       // 10: Oport
                                 Promise,     // 11: Promise
                                 Cont,        // 12: Cont
                                 Builtin,     // 13: Builtin
                                 Insn,        // 14: Insn
                                 Cproc,       // 15: Cproc
                                 Cpromise,    // 16: Cpromise
                                 Free,        // 17: Free
                                 Cons         // 18: Cons
                                 >;

  CellValue val;
  bool m_gc_mark = false;
  bool m_gc_traverse_cdr = false;
  bool m_gc_alt_bit = false;
  uint16_t m_flags = 0;

  template <typename T> bool is() const {
    if constexpr (std::is_same_v<T, intptr_t>) {
      if (short_atom(this))
        return true;
    } else {
      if (short_atom(this))
        return false;
    }
    return std::holds_alternative<T>(val);
  }

  template <typename T> const T &as() const { return std::get<T>(val); }

  template <typename T> T &as() { return std::get<T>(val); }

  template <typename T> const T *get_if() const {
    if (short_atom(this))
      return nullptr;
    return std::get_if<T>(&val);
  }
  template <typename T> T *get_if() {
    if (short_atom(this))
      return nullptr;
    return std::get_if<T>(&val);
  }

  template <typename Visitor> decltype(auto) visit(Visitor &&visitor) const {
    return std::visit(std::forward<Visitor>(visitor), val);
  }
  template <typename Visitor> decltype(auto) visit(Visitor &&visitor) {
    return std::visit(std::forward<Visitor>(visitor), val);
  }

  enum class Flag : uint8_t {
    Forced = 1 << 0,
    Macro = 1 << 1,
    VRef = 1 << 2,
  };

  static constexpr uintptr_t ATOM = 0x1;

  static const int GLOBAL_ENV = -1;

  bool macro() const { return flag(Flag::Macro); }

  static inline bool short_atom(const Cell *c) {
    return (reinterpret_cast<uintptr_t>(c) & ATOM) != 0;
  }
  static inline bool long_atom(const Cell *c) {
    return (reinterpret_cast<uintptr_t>(c) & ATOM) == 0 && !c->is<Cons>();
  }
  static inline bool atomic(const Cell *c) {
    return (reinterpret_cast<uintptr_t>(c) & ATOM) != 0 || !c->is<Cons>();
  }
  static Cell *notcons();

  Cell() : val(Cons{&Nil, &Nil}) {}
  Cell(const char *unique_name) : val(Unique{unique_name}) {}

  inline intptr_t e_skip() const { return std::get<Symbol>(val).e_skip; }
  inline intptr_t b_skip() const { return std::get<Symbol>(val).b_skip; }
  inline bool is_quickened() const {
    if (auto *s = get_if<Symbol>())
      return s->is_quickened();
    return false;
  }
  void set_lexaddr(intptr_t e_skip, intptr_t b_skip) {
    auto &s = std::get<Symbol>(val);
    s.e_skip = static_cast<int16_t>(e_skip);
    s.b_skip = static_cast<int16_t>(b_skip);
  }

  void flag(Flag f, bool b) {
    if (b)
      m_flags |= static_cast<uint8_t>(f);
    else
      m_flags &= ~static_cast<uint8_t>(f);
  }
  void set_flag(Flag f, bool b = true) { flag(f, b); }
  bool flag(Flag f) const { return (m_flags & static_cast<uint8_t>(f)) != 0; }

  void dump(FILE *);

  // Cons accessors
  Cell *unsafe_car() const { return std::get<Cons>(val).car; }
  void set_unsafe_car(Cell *c) { std::get<Cons>(val).car = c; }
  Cell *unsafe_cdr() const { return std::get<Cons>(val).cdr; }
  void set_unsafe_cdr(Cell *c) { std::get<Cons>(val).cdr = c; }
  Cell *next_free() const { return std::get<Free>(val).next; }

  static void setcar(Cell *c, Cell *car) {
    if (atomic(c))
      notcons();
    else
      std::get<Cons>(c->val).car = car;
  }
  static void setcdr(Cell *c, Cell *cdr) {
    if (atomic(c))
      notcons();
    else
      std::get<Cons>(c->val).cdr = cdr;
  }
  static Cell *car(const Cell *c) {
    return atomic(c) ? notcons() : std::get<Cons>(c->val).car;
  }
  static Cell *cdr(const Cell *c) {
    return atomic(c) ? notcons() : std::get<Cons>(c->val).cdr;
  }

  static Cell *caar(const Cell *c);
  static Cell *cadr(const Cell *c);
  static Cell *cdar(const Cell *c);
  static Cell *cddr(const Cell *c);
  static Cell *caaar(const Cell *c);
  static Cell *caadr(const Cell *c);
  static Cell *cadar(const Cell *c);
  static Cell *caddr(const Cell *c);
  static Cell *cdaar(const Cell *c);
  static Cell *cdadr(const Cell *c);
  static Cell *cddar(const Cell *c);
  static Cell *cdddr(const Cell *c);
  static Cell *caaaar(const Cell *c);
  static Cell *caaadr(const Cell *c);
  static Cell *caadar(const Cell *c);
  static Cell *caaddr(const Cell *c);
  static Cell *cadaar(const Cell *c);
  static Cell *cadadr(const Cell *c);
  static Cell *caddar(const Cell *c);
  static Cell *cadddr(const Cell *c);
  static Cell *cdaaar(const Cell *c);
  static Cell *cdaadr(const Cell *c);
  static Cell *cdadar(const Cell *c);
  static Cell *cdaddr(const Cell *c);
  static Cell *cddaar(const Cell *c);
  static Cell *cddadr(const Cell *c);
  static Cell *cdddar(const Cell *c);
  static Cell *cddddr(const Cell *c);

  intptr_t IntValue() const {
    if (short_atom(this))
      return reinterpret_cast<intptr_t>(this) >> 1;
    return as<intptr_t>();
  }
  char CharValue() const { return as<char>(); }
  const Subr *SubrValue() const { return &as<Subr>(); }
  const std::string &StringValue() const { return as<std::string>(); }
  std::string &mutable_string() { return as<std::string>(); }
  size_t StringLength() const { return as<std::string>().length(); }
  FILE *IportValue() const { return as<Iport>().f; }
  FILE *OportValue() const { return as<Oport>().f; }
  cellvector *VectorValue() const { return as<Vec>().cv; }
  cellvector *CProcValue() const { return as<Cproc>().cv; }
  Cell *PromiseValue() const { return as<Promise>().cv->get(0); }
  Cell *CPromiseValue() const { return as<Cpromise>().cv->get(0); }
  psymbol SymbolValue() const { return as<Symbol>().s; }
  psymbol BuiltinValue() const { return as<Builtin>().s; }
  Procedure LambdaValue() const {
    cellvector *cv = as<Lambda>().cv;
    return Procedure(cv->get(0), cv->get(1), cv->get(2));
  }
  double RealValue() const { return as<double>(); }
  const char *name() const;
  void free_contents();
  const Insn *InsnValue() const { return &as<Insn>(); }
  Insn *InsnValue() { return &as<Insn>(); }

  cellvector *vector_payload() const {
    if (short_atom(this))
      error("expecting a vector-backed cell");
    return std::visit(overloaded{[](const Vec &v) { return v.cv; },
                                 [](const Lambda &l) { return l.cv; },
                                 [](const Cproc &cp) { return cp.cv; },
                                 [](const Promise &p) { return p.cv; },
                                 [](const Cpromise &cp) { return cp.cv; },
                                 [](const Cont &k) { return k.cv; },
                                 [](const auto &) -> cellvector * {
                                   error("expecting a vector-backed cell");
                                   return nullptr;
                                 }},
                      val);
  }
  cellvector *unsafe_vector_value() const { return vector_payload(); }

  static void real_to_string(double, char *, int);
  double asReal() const {
    return is<intptr_t>() ? (double)IntValue() : RealValue();
  }
  bool isBoolean() { return this == &Bool_T || this == &Bool_F; }
  bool istrue() { return this != &Bool_F; }
  bool ispair();
  static Cell *untagged(Cell *c) { return c; }

  int length() {
    int i = 0;
    for (Cell *p = this; p != nil; p = Cell::cdr(p))
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
    Cell *h, *t;
  };

  void list_append(Cell *&head, Cell *&tail) {
    if (tail == &Nil)
      head = tail = this;
    else {
      setcdr(tail, this);
      tail = this;
    }
  }

  static void sanity_check();
};

static_assert(alignof(Cell) >= 2,
              "Cell must be at least 2-byte aligned for tagged pointers");

// class Environment
//
// At the simplest level, an Environment is a mapping from symbols
// to values. Symbols are the hash codes maintained by the symbol table,
// and the value of any symbol is a pointer to a Scheme cell.
//
// Environments are created by binding constructs (like let and lambda),
// and a new environment is linked to the enclosing environment in force
// when it was created. The enclosure chain terminates at the global
// environment.
//
// In Scheme, all variables are lexically bound. This binding model
// creates the possibility of lexical addressing, a system in which
// a variable reference can be replaced by the coordinate pair (e_skip, b_skip):
// the number of enclosing environments to traverse, and the index within
// that environment. Once resolved, the lexical address is cached directly
// in the AST for fast subsequent lookup.

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

  Step eval_coro(Cell *form);
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

  Cell *raw_alloc();

  template <typename T, typename... Args> Cell *alloc(Args &&...args) {
    Cell *c = raw_alloc();
    c->val.emplace<T>(std::forward<Args>(args)...);
    return c;
  }

  // Manufacture Cells and Atoms

  Cell *cons(Cell *ca, Cell *cd = &Cell::Nil) {
    return alloc<Cell::Cons>(ca, cd);
  }
  Cell *make() { return alloc<Cell::Cons>(&Cell::Nil, &Cell::Nil); }
  Cell *make(Cell *ca, Cell *cd = &Cell::Nil) {
    return alloc<Cell::Cons>(ca, cd);
  }
  Cell *make_int(intptr_t i);
  Cell *make_char(char ch) { return alloc<char>(ch); }
  Cell *make_real(double d) { return alloc<double>(d); }
  Cell *make_string(std::string s) { return alloc<std::string>(std::move(s)); }
  Cell *make_string(size_t len, char ch = '\0') {
    return alloc<std::string>(len, ch);
  }
  Cell *make_string(const char *s) { return alloc<std::string>(s); }
  Cell *make_string(const char *s, size_t len) {
    return alloc<std::string>(s, len);
  }
  Cell *make_subr(subr_f s, const char *name) {
    return alloc<Cell::Subr>(s, name);
  }
  Cell *make_builtin(psymbol y) { return alloc<Cell::Builtin>(y); }
  Cell *make_symbol(psymbol y) { return alloc<Cell::Symbol>(y); }
  Cell *make_boolean(bool b) { return b ? &Cell::Bool_T : &Cell::Bool_F; }
  Cell *make_vector(int n, Cell *init = &Cell::Unspecified);
  Cell *make_iport(const std::string &fname);
  Cell *make_iport(const char *fname);
  Cell *make_iport(FILE *ip) { return alloc<Cell::Iport>(ip); }
  Cell *make_oport(const std::string &fname);
  Cell *make_oport(const char *fname);
  Cell *make_oport(FILE *op) { return alloc<Cell::Oport>(op); }
  Cell *make_procedure(Cell *env, Cell *body, Cell *arglist);
  Cell *make_promise(Cell *env, Cell *body);
  Cell *make_macro(Cell *env, Cell *body, Cell *arglist);
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
  void print_insn(int pc, const Cell *insn);
  Cell *write_compiled_procedure(Cell *arglist);
  Cell *load_compiled_procedure(struct vm_cproc *);
  Cell *load_instructions(vm_cproc *);

  // ------------------------------------------------------------

  void with_input(const std::string &fname) { istack.push(make_iport(fname)); }
  void with_input(const char *fname) { istack.push(make_iport(fname)); }

  void with_output(const std::string &fname) { ostack.push(make_oport(fname)); }
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
    m_stack.push(rc.unsafe_car());
    m_stack.push(rc.unsafe_cdr());
  }
  void save_i(intptr_t i) {
    m_stack.push(reinterpret_cast<Cell *>((i << 1) | Cell::ATOM));
  }
  void restore(Cell *&c) { c = m_stack.pop(); }
  void restore(Cell &rc) {
    rc.set_unsafe_cdr(m_stack.pop());
    rc.set_unsafe_car(m_stack.pop());
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
    if (l.unsafe_car() == nil) {
      l.set_unsafe_car(t);
      l.set_unsafe_cdr(t);
    } else {
      l.unsafe_cdr()->set_unsafe_cdr(t);
      l.set_unsafe_cdr(t);
    }
  }

  void l_append(Cell &l, Cell *t) {
    r_elt = make(t);
    l_appendtail(l, r_elt);
  }

  void clear(Cell &c) {
    c.set_unsafe_car(nil);
    c.set_unsafe_cdr(nil);
  }

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

  ~VxSchemeInit() {}
};

class SchemeExtension {
public:
  virtual ~SchemeExtension() {}
  static void Register(SchemeExtension *ext);
  static void RunInstall(Context *, Cell *);
  static void MainProcedure(SchemeExtension *m) { main = m; }
  static bool HaveMain() { return main != nullptr; }
  static Cell *RunMain(Context *ctx) { return main->Run(ctx); }

  virtual void Install(Context *, Cell *) = 0;

private:
  virtual Cell *Run(Context *) { return &Cell::Bool_F; }
  static cellvector *extensions;
  static SchemeExtension *main;
};

// Simple accessors to avoid the Cell:: scope, which we don't
// really need for simple things like 'car'.

inline Cell *car(const Cell *c) { return Cell::car(c); }
inline Cell *caar(const Cell *c) { return Cell::caar(c); }
inline Cell *cdr(const Cell *c) { return Cell::cdr(c); }
inline Cell *cdar(const Cell *c) { return Cell::cdar(c); }
inline Cell *cadr(const Cell *c) { return Cell::cadr(c); }
inline Cell *cddr(const Cell *c) { return Cell::cddr(c); }
inline Cell *cadar(const Cell *c) { return Cell::cadar(c); }
inline Cell *caddr(const Cell *c) { return Cell::caddr(c); }
inline Cell *caadr(const Cell *c) { return Cell::caadr(c); }
inline Cell *cdadr(const Cell *c) { return Cell::cdadr(c); }
inline Cell *cddar(const Cell *c) { return Cell::cddar(c); }
inline Cell *caddar(const Cell *c) { return Cell::caddar(c); }
inline Cell *cadaar(const Cell *c) { return Cell::cadaar(c); }

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

// Typedefs for compiled procedures in C form.  It's possible to serialize
// a compiled procedure into a C data structure that can be used to load
// the bytecode.

using byte = uint8_t;

struct vm_insn {
  byte opcode;
  byte count;
  const void *operand;
};

struct vm_cproc {
  vm_insn *insns;
  unsigned int n_insns;
  const char **literals;
  unsigned int n_literals;
  int entry;
};
