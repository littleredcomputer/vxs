//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// cell.cpp : cell creation, storage management, garbage collection.

#include "vx-scheme.h"

static const char *nomem_error = "out of memory";

Cell *Context::make() {
  Cell *c = alloc(Cell::Cons);
  c->set_unsafe_car(&Cell::Nil);
  c->set_unsafe_cdr(&Cell::Nil);
  return c;
}

Cell *Context::make_int(intptr_t i) {
// SHORT INTEGER support: if the integer fits in 24 bits,
// then return a phony pointer with the short flag set and
// the integer in the upper 24.  This avoids storage allocation
// and the attendant eventual garbage.
#if 1
  if ((i << 8) >> 8 == i) {
    return reinterpret_cast<Cell *>((i << 8) | Cell::SHORT | Cell::ATOM);
  }
#endif
  Cell *c = alloc(Cell::Int);
  c->init_int(i);

  return c;
}

Cell *Context::make_char(char ch) {
  Cell *c = alloc(Cell::Char);
  c->init_char(ch);
  return c;
}

Cell *Context::make_real(double d) {
  Cell *c = alloc(Cell::Real);
  c->init_real(d);
  return c;
}

// Context::make_string
//   Makes a string of the indicated length -- it is UNINITIALIZED

Cell *Context::make_string(size_t len) {
  Cell *c = alloc(Cell::String);
  c->init_string(std::string(len, '\0'));
  return c;
}

Cell *Context::make_string(int len, char ch) {
  Cell *c = make_string(len);
  memset(c->StringValue(), ch, len);
  c->StringValue()[len] = '\0';
  return c;
}

Cell *Context::make_string(const char *s) { return make_string(s, strlen(s)); }

Cell *Context::make_string(const char *s, size_t len) {
  Cell *c = make_string(len);
  strncpy(c->StringValue(), s, len);
  c->StringValue()[len] = '\0';
  return c;
}

Cell *Context::make_subr(subr_f s, const char *name) {
  Cell *c = alloc(Cell::Subr);
  c->init_subr(s, name);
  return c;
}

Cell *Context::make_builtin(psymbol y) {
  Cell *c = alloc(Cell::Builtin);
  c->init_builtin(y);
  return c;
}

Cell *Context::make_symbol(psymbol y) {
  Cell *c = alloc(Cell::Symbol);
  c->init_symbol(y);

  return c;
}

Cell *Context::make_boolean(bool b) {
  return b ? &Cell::Bool_T : &Cell::Bool_F;
}

Cell *Context::make_vector(int n, Cell *init /* = &Unspecified */) {
  Cell *c = alloc(Cell::Vec);
  c->val = Cell::VecVal{cellvector::alloc(n)};
  c->flag(Cell::VREF, true);

  for (int ix = 0; ix < n; ++ix)
    c->unsafe_vector_value()->set(ix, init);

  return c;
}

Cell *Context::make_iport(const char *fname) {
  FILE *ip = fopen(fname, "r");
  if (ip)
    return make_iport(ip);

  error("unable to open stream for reading");
  return nil;
}

Cell *Context::make_iport(FILE *ip) {
  Cell *c = alloc(Cell::Iport);
  c->init_iport(ip);

  return c;
}

Cell *Context::make_oport(const char *fname) {
  FILE *ofs = fopen(fname, "w");
  if (ofs)
    return make_oport(ofs);

  error("unable to open stream for writing");
  return nil;
}

Cell *Context::make_oport(FILE *op) {
  Cell *c = alloc(Cell::Oport);
  c->init_oport(op);

  return c;
}

Cell *Context::make(Cell *ca, Cell *cd /* = &Nil*/) {
  Cell *c = alloc(Cell::Cons);
  c->val = Cell::ConsPair{ca, cd};
  return c;
}

Cell *Context::make_magic(void *key, magic_set_f set_f, magic_get_f get_f) {
  Cell *c = alloc(Cell::Magic);
  c->init_magic(key, set_f, get_f);
  return c;
}

Cell *Cell::notcons() {
  error("expecting a Cons");
  return nil;
}

bool Cell::ispair() {
  return type() == Cell::Cons && this != unspecified && this != nil;
}

void Cell::sanity_check() {
  int bad = 0;

  // Make sure that there are enough typebits to contain
  // all the types we know about.

  if ((1 << TYPEBITS) < NUM_ATOMS)
    ++bad, printf("Not enough typebits!\n");

  // Make sure that the size of a cell has not become greater
  // than two machine pointers (car & cdr).

  if (sizeof(Cell) > 2 * sizeof(void *))
    printf("Cell (%zu) is larger than CAR+CDR!\n", sizeof(Cell));

  // Make sure that the "zero zone" (the least significant
  // bits of a pointer to a cell) is wide enough to accomodate
  // the type and GC information stored there, assuming that
  // a Cell is aligned to its own size in memory

  if (sizeof(Cell) < (1 << TAGBITS))
    ++bad, printf("Too many tag bits for cell size\n");

  if (bad)
    exit(bad);
};

bool Cell::eq(Cell *that) {
  if (this == that)
    return true;
  if (short_atom(this) || short_atom(that))
    return false;
  if (!atomic(this) || !atomic(that))
    return false; // Conses must have the exact same pointer to be eq
  if (type() != that->type())
    return false;
  return val == that->val;
}

bool Cell::equal(Cell *c) {
  if (this == c)
    return true;
  if (short_atom(this) || short_atom(c))
    return false;
    
  Type t0 = type();
  Type t1 = c->type();

  if (this == &Nil && c == &Nil)
    return true;
  else if (t0 == Cons && t1 == Cons)
    return std::get<ConsPair>(val).car->equal(std::get<ConsPair>(c->val).car) &&
           std::get<ConsPair>(val).cdr->equal(std::get<ConsPair>(c->val).cdr);
  else if (t0 == Vec && t1 == Vec) {
    cellvector *cv = VectorValue();
    cellvector *ocv = c->VectorValue();
    int s = cv->size();

    if (s != ocv->size())
      return false;

    for (int ix = 0; ix < s; ++ix)
      if (!cv->get(ix)->equal(ocv->get(ix)))
        return false;

    return true;
  } else if (t0 == String && t1 == String)
    return !strcmp(StringValue(), c->StringValue());
  else if (t0 == Real && t1 == Real)
    return RealValue() == c->RealValue();
  else
    return eq(c);
}

//------------------------------------------------------------------------
//
// Access/Mutate Cons Cells.  These are checked calls, in that they
// will verify that they are traversing a set of cons cells at each
// step, using "assert_cons", which throws a C++ exception if this is
// not found to be true.

Cell *Cell::caar(const Cell *c) { return Cell::car(Cell::car(c)); }
Cell *Cell::cadr(const Cell *c) { return Cell::car(Cell::cdr(c)); }
Cell *Cell::cdar(const Cell *c) { return Cell::cdr(Cell::car(c)); }
Cell *Cell::cddr(const Cell *c) { return Cell::cdr(Cell::cdr(c)); }
Cell *Cell::caaar(const Cell *c) { return Cell::car(Cell::caar(c)); }
Cell *Cell::caadr(const Cell *c) { return Cell::car(Cell::cadr(c)); }
Cell *Cell::cadar(const Cell *c) { return Cell::car(Cell::cdar(c)); }
Cell *Cell::caddr(const Cell *c) { return Cell::car(Cell::cddr(c)); }
Cell *Cell::cdaar(const Cell *c) { return Cell::cdr(Cell::caar(c)); }
Cell *Cell::cdadr(const Cell *c) { return Cell::cdr(Cell::cadr(c)); }
Cell *Cell::cddar(const Cell *c) { return Cell::cdr(Cell::cdar(c)); }
Cell *Cell::cdddr(const Cell *c) { return Cell::cdr(Cell::cddr(c)); }
Cell *Cell::caaaar(const Cell *c) { return Cell::car(Cell::caaar(c)); }
Cell *Cell::caaadr(const Cell *c) { return Cell::car(Cell::caadr(c)); }
Cell *Cell::caadar(const Cell *c) { return Cell::car(Cell::cadar(c)); }
Cell *Cell::caaddr(const Cell *c) { return Cell::car(Cell::caddr(c)); }
Cell *Cell::cadaar(const Cell *c) { return Cell::car(Cell::cdaar(c)); }
Cell *Cell::cadadr(const Cell *c) { return Cell::car(Cell::cdadr(c)); }
Cell *Cell::caddar(const Cell *c) { return Cell::car(Cell::cddar(c)); }
Cell *Cell::cadddr(const Cell *c) { return Cell::car(Cell::cdddr(c)); }
Cell *Cell::cdaaar(const Cell *c) { return Cell::cdr(Cell::caaar(c)); }
Cell *Cell::cdaadr(const Cell *c) { return Cell::cdr(Cell::caadr(c)); }
Cell *Cell::cdadar(const Cell *c) { return Cell::cdr(Cell::cadar(c)); }
Cell *Cell::cdaddr(const Cell *c) { return Cell::cdr(Cell::caddr(c)); }
Cell *Cell::cddaar(const Cell *c) { return Cell::cdr(Cell::cdaar(c)); }
Cell *Cell::cddadr(const Cell *c) { return Cell::cdr(Cell::cdadr(c)); }
Cell *Cell::cdddar(const Cell *c) { return Cell::cdr(Cell::cddar(c)); }
Cell *Cell::cddddr(const Cell *c) { return Cell::cdr(Cell::cdddr(c)); }

psymbol Cell::SymbolValue() const {
  typecheck(Symbol);
  return std::get<SymbolVal>(val).s;
}

void Cell::stats() {
  for (int ix = 0; ix < NUM_TYPES; ++ix)
    printf("%s %d ", typeName[ix], typeCount[ix]);

  printf("\n");
}

//======================================================================
//
//              Value Extractors
//
//======================================================================

intptr_t Cell::IntValue() const {
  if (short_atom(this)) return reinterpret_cast<intptr_t>(this) >> 8;
  typecheck(Int);
  return std::get<intptr_t>(val);
}

char Cell::CharValue() const {
  typecheck(Char);
  return std::get<char>(val);
}

const Cell::SubrVal *Cell::SubrValue() const {
  typecheck(Subr);
  return &std::get<SubrVal>(val);
}

char *Cell::StringValue() const {
  typecheck(String);
  return const_cast<char*>(std::get<std::string>(val).c_str());
}

size_t Cell::StringLength() const {
  typecheck(String);
  return std::get<std::string>(val).length();
}

FILE *Cell::IportValue() const {
  typecheck(Iport);
  return std::get<IportVal>(val).f;
}

FILE *Cell::OportValue() const {
  typecheck(Oport);
  return std::get<OportVal>(val).f;
}



cellvector *Cell::VectorValue() const {
  typecheck(Vec);
  return std::get<VecVal>(val).cv;
}

cellvector *Cell::CProcValue() const {
  typecheck(Cproc);
  return std::get<CprocVal>(val).cv;
}

Cell *Cell::PromiseValue() const {
  typecheck(Promise);
  return std::get<PromiseVal>(val).cv->get(0);
}

Cell *Cell::CPromiseValue() const {
  typecheck(Cpromise);
  return std::get<CpromiseVal>(val).cv->get(0);
}

psymbol Cell::BuiltinValue() const {
  typecheck(Builtin);
  return std::get<BuiltinVal>(val).s;
}

Cell::Procedure Cell::LambdaValue() const {
  typecheck(Lambda);
  cellvector* cv = std::get<LambdaVal>(val).cv;
  return Procedure(cv->get(0), cv->get(1), cv->get(2));
}

double Cell::RealValue() const {
  typecheck(Real);
  return std::get<double>(val);
}

const char *Cell::name() const { return typeName[type()]; }

void Cell::typefail(Type actual, Type wanted) const {
  fprintf(stderr, "caught: type check failure: wanted %s, got %s\n",
          typeName[wanted], typeName[actual]);
  abort();
}

void Cell::dump(FILE *out) {
  Type t = type();
  fprintf(out, "[%p ", this);

  if (m_gc_mark)
    fputs("mark ", out);

  if (flag(FORCED)) fputs("forced ", out);
  if (flag(QUICK)) fputs("quick ", out);
  if (flag(MACRO)) fputs("macro ", out);
  if (flag(VREF)) fputs("vref ", out);

  fputs(typeName[t], out);

  switch (t) {
  case Cons:
    fputs(" ", out);
    if (std::get<ConsPair>(val).car == nil)
      fputs("nil", out);
    else
      fprintf(out, "%p", std::get<ConsPair>(val).car);
    fputs(" ", out);
    if (std::get<ConsPair>(val).cdr == nil)
      fputs("nil", out);
    else
      fprintf(out, "%p", std::get<ConsPair>(val).cdr);
    break;

  case Int:
    fprintf(out, " %" PRIdPTR, std::get<intptr_t>(val));
    break;
  case Real:
    fprintf(out, " %g", std::get<double>(val));
    break;
  case Unique:
    fprintf(out, " %s", std::get<Cell::UniqueVal>(val).s);
    break;
  case Symbol:
    fprintf(out, " %s", std::get<Cell::SymbolVal>(val).s->key);
    break;
  default:
    break;
  }
  fputc(']', out);
}

//======================================================================
//
//              Cell Vectors
//
//======================================================================

cellvector::cellvector(int size /* = 0 */) {
  int allocate = (size == 0 ? 10 : size);
  make_cv(size, allocate);
}

cellvector::cellvector(int size, int alloc) { make_cv(size, alloc); }

void cellvector::make_cv(int size, int alloc) {
  v = (Cell **)malloc(alloc * sizeof(Cell *));
  if (!v)
    error(nomem_error);
  allocated = alloc;

  for (int ix = 0; ix < alloc; ++ix)
    v[ix] = nil;

  gc_index = 0;
  gc_uplink = 0;
  sz = size;
}

Cell *&cellvector::operator[](int ix) {
  if (ix < 0 || ix >= sz)
    vref_error();

  return v[ix];
}

void cellvector::set(int ix, Cell *c)

{
  if (ix < 0 || ix >= sz)
    vref_error();

  v[ix] = c;
}

void cellvector::expand() {
  // Must expand vector: double size.
  int new_alloc = 2 * allocated;
  Cell **v2 = (Cell **)malloc(new_alloc * sizeof(Cell *));

  if (!v2)
    error(nomem_error);

  memcpy(v2, v, allocated * sizeof(Cell *));
  ::free(v);
  v = v2;
  allocated = new_alloc;
}

Cell *cellvector::shift() {
  Cell *val = v[0];
  for (int ix = 0; ix < sz - 1; ++ix)
    v[ix] = v[ix + 1];
  pop();
  return val;
}

void cellvector::unshift(Cell *val) {
  push(nil);
  for (int ix = sz - 1; ix > 0; --ix)
    v[ix] = v[ix - 1];
  v[0] = val;
}

void cellvector::vref_error() const { error("vector reference out of bounds"); }

void cellvector::clear() { sz = 0; }

cellvector::~cellvector() {
  ::free(v);
  sz = 0;
  allocated = 0;
  v = 0;
}

// Cellvector freelist management

cellvector *cellvector::freelist_head[cellvector::keep_size + 1];
int cellvector::freelist_count[cellvector::keep_size + 1];

cellvector *cellvector::alloc(int size) {
  int allocate = size;
  if (allocate == 0)
    allocate = 2;
  return alloc(size, allocate);
}

cellvector *cellvector::alloc(int size, int allocate) {
  cellvector *result;
  if (allocate <= keep_size) {
    if ((result = freelist_head[allocate])) {
      freelist_head[allocate] = result->next_free;
      for (int ix = 0; ix < allocate; ++ix)
        result->v[ix] = nil;
      result->sz = size;
      result->next_free = 0;
      --freelist_count[allocate];
      return result;
    }
  }
  return new cellvector(size, allocate);
}

void cellvector::free() {
  if (allocated <= keep_size && freelist_count[allocated] <= keep_count) {
    next_free = freelist_head[allocated];
    ++freelist_count[allocated];
    freelist_head[allocated] = this;
  } else {
    delete this;
  }
}

//======================================================================
//
//              Memory Allocation and Garbage Collection
//
//======================================================================

class Slab {
public:
  Cell *alloc() {
    if (next + 1 > end)
      return 0;

    Cell *r = next;
    ++next;
    return r;
  }

  int remaining() { return static_cast<int>(end - next); }

  void reset() { next = start; }

  void sweep(Context *);

  Slab(Context *ctx) {
    // We avoid the temptation to call new Cell [slabsize],
    // since that would invoke the constructor on each cell,
    // which we don't need (alloc will take care of preparing
    // cells for use).
    //
    // It is essential that Cells be 8-aligned to preserve
    // three bits for type and GC information.  If new has
    // stiffed us with 4-aligned memory, we "burn" 4 bytes
    // of it.

    int storage_size = slabsize * sizeof(Cell) + 4;
    storage = (char *)malloc(storage_size);

    if (!storage)
      error("out of memory");

    // Supposedly the ANSI library guarantees that storage
    // is 4-aligned!

    if ((reinterpret_cast<intptr_t>(storage)) & 3)
      abort();

    // But if it's not 8-aligned we can fix that using the
    // extra 4 bytes we allocated.

    if ((reinterpret_cast<intptr_t>(storage)) & 7)
      start = reinterpret_cast<Cell *>(storage + 4);
    else
      start = reinterpret_cast<Cell *>(storage);

    memset(storage, 0, storage_size);

    ctx->cellsTotal += slabsize;
    end = start + slabsize;
    reset();
  }

  ~Slab() { free(storage); }

  static int slabsize;

private:
  Cell *start;
  Cell *end;
  Cell *next;
  char *storage;
};

int Slab::slabsize = 10000;

Cell *Context::alloc(Cell::Type t) {
  Cell *a;

  mem.last_alloc_gc = false;
  // Select a cell from the free list if one is available.

TOP:
  if ((a = mem.free)) {
    ++cellsAlloc;
    mem.free = a->next_free();
    
    a->set_type(t);
    a->m_flags = 0;
    a->m_gc_mark = false;
    a->m_gc_alt_bit = false;
    a->m_gc_traverse_cdr = false;
    --mem.c_free;
    return a;
  }

  // IF there aren't any slabs in the active pool,
  // we must never have allocated any slabs at all
  // yet, so allocate the first one.

  if (mem.active.size() == 0) {
    // Configurable slabsize

    char *c;
    if ((c = getenv("SLABSIZE")) != NULL)
      Slab::slabsize = atoi(c);

    mem.active.push((Cell *)new Slab(this));
    mem.free = 0;
    mem.low_water = false;
    mem.no_inline_gc = debug_flag(DEBUG_NO_INLINE_GC);
  }

  // Check the "top" slab to see if there's any room
  // left in it.

  if ((a = mem.current()->alloc())) {
    ++cellsAlloc;
    a->set_type(t);
    a->m_flags = 0;
    a->m_gc_mark = false;
    a->m_gc_alt_bit = false;
    a->m_gc_traverse_cdr = false;
    return a;
  }

  // There wasn't any room in the top slab.  We can try
  // to GC.  If we do, and still 80% of the allocated
  // memory is occupied, we set a flag admitting that
  // the last GC was "unproductive", and next time 'round
  // we'll allocate a new slab.

  if (mem.no_inline_gc || mem.last_alloc_gc || mem.low_water) {
    mem.active.push((Cell *)new Slab(this)); // trip to the well
    mem.low_water = false;                   // low_water is a one-shot
  } else {
    mem.last_alloc_gc = true;
    gc();
  }

  goto TOP;
}

//----------------------------------------------------------------------
// GARBAGE COLLECTION
//



inline 

inline 

//----------------------------------------------------------------------
// Marking for Garbage Collection
//
// This implementation is Knuth's Algorithm 2.3.5E (TAoCP 3ed. vol I
// p. 418) We follow Knuth's presentation carefully (using the same
// variable names and statement labels).  Like the evaluator, this
// code has to take some care to avoid recursion: we want to be able
// to perform a GC mark wihtout allocating any additional space (not
// even C stack space).  That accounts for some of the complexity in
// this routine.  The other part is that, due to vectors, we have to
// support n-way marking instead of just 2-way marking.

void Context::mark(Cell *P) {
  bool traceall = false; // OS::flag(TRACE_GC_ALL);
  if (P == nil || P == 0 || Cell::short_atom(P) || P->m_gc_mark)
    return;

  Cell *T = nil;
  Cell *Q = nil;

E2:
  P->m_gc_mark = true;
  if (traceall) {
    printf("m ");
    P->dump(stdout);
    putchar('\n');
  }

  if (Cell::atomic(P)) {
    if (P->flag(Cell::VREF)) {
      auto cv = P->unsafe_vector_value();
      if (cv->size() > 0) {
        cv->gc_uplink = T;
        cv->gc_index = 0;
        T = P;
      }
    } else if (P->type() == Cell::Symbol) {
      psymbol ps = std::get<Cell::SymbolVal>(P->val).s;
      if (ps->plist && ps->plist->gc_uplink == 0 && ps->plist->size() > 0) {
        ps->plist->gc_uplink = T;
        ps->plist->gc_index = 0;
        T = P;
      }
    }
    goto E6; // E3
  }

  if (!Cell::atomic(P)) {
    Q = std::get<Cell::ConsPair>(P->val).car; // E4
    if (Q != nil && !Cell::short_atom(Q) && !Q->m_gc_mark) {
      P->m_gc_alt_bit = true;
      std::get<Cell::ConsPair>(P->val).car = T;
      T = P;
      P = Q;
      goto E2;
    }
  }

E5:
  if (!Cell::atomic(P)) {
    Q = std::get<Cell::ConsPair>(P->val).cdr;
    if (Q != nil && !Cell::short_atom(Q) && !Q->m_gc_mark) {
      std::get<Cell::ConsPair>(P->val).cdr = T;
      T = P;
      P = Q;
      goto E2;
    }
  }

E6:
  if (T == nil)
    return;

  Q = T;

  if (Q->flag(Cell::VREF)) {
  next_element:
    auto cv = Q->unsafe_vector_value();
    int i = cv->gc_index++;
    if (i >= cv->size()) {
      T = cv->gc_uplink;
      cv->gc_index = 0; 
      P = Q;
      goto E6;
    } else {
      P = cv->get(i);
      if (P == nil || Cell::short_atom(P) || P->m_gc_mark)
        goto next_element;
      goto E2;
    }
  } else if (Q->type() == Cell::Symbol) {
    psymbol ps = std::get<Cell::SymbolVal>(Q->val).s;
  next_property:
    int i = ps->plist->gc_index++;
    if (i >= ps->plist->size()) {
      T = ps->plist->gc_uplink;
      ps->plist->gc_index = 0;
      ps->plist->gc_uplink = 0;
      P = Q;
      goto E6;
    } else {
      P = ps->plist->get(i);
      if (P == nil || Cell::short_atom(P) || P->m_gc_mark)
        goto next_property;
      goto E2;
    }
  }

  if (Q->m_gc_alt_bit) {
    Q->m_gc_alt_bit = false;
    T = std::get<Cell::ConsPair>(Q->val).car;
    std::get<Cell::ConsPair>(Q->val).car = P;
    P = Q;
    goto E5;
  } else {
    T = std::get<Cell::ConsPair>(Q->val).cdr;
    std::get<Cell::ConsPair>(Q->val).cdr = P;
    P = Q;
    goto E6;
  }
}

void Slab::sweep(Context *ctx) {
  bool traceall = false; // OS::flag(TRACE_GC_ALL);

  for (Cell *p = start; p < next; ++p) {
    if (p->m_gc_mark) {
      p->m_gc_mark = false;
    } else if (p->type() != Cell::Free && !std::holds_alternative<Cell::FreeVal>(p->val)) {
      if (traceall) {
        printf("s ");
        p->dump(stdout);
        putchar('\n');
      }
      Cell::Type t = p->type();

      switch (t) {
      case Cell::Cont:
      case Cell::Promise:
      case Cell::Cproc:
      case Cell::Cpromise:
      case Cell::Lambda:
      case Cell::Vec: 
        p->unsafe_vector_value()->free();
        break;
      case Cell::Iport: 
        if (FILE *f = std::get<Cell::IportVal>(p->val).f) fclose(f);
        break;
      case Cell::Oport:
        if (FILE *f = std::get<Cell::OportVal>(p->val).f) fclose(f);
        break;
      case Cell::Magic:
        delete std::get<Cell::MagicBox*>(p->val);
        break;
      default:
        break;
      }

      --ctx->cellsAlloc;
      p->set_type(Cell::Free);
      std::get<Cell::FreeVal>(p->val).next = ctx->mem.free;
      ctx->mem.free = p;
      ++ctx->mem.c_free;
    }
  }
}

void Context::gc() {
  bool gc_verbose = debug_flag(TRACE_GC);
  Cell *p;

  if (!ok_to_gc) {
    fprintf(stderr,
            "initial memory budget insufficient to set up VM\n"
            "Try setting the environment variable SLABSIZE to\n"
            "something greater than %d\n",
            Slab::slabsize);
    exit(1);
  }
  if (gc_verbose)
    printf("; start gc: %d/%d\n", cellsAlloc, cellsTotal);

  //
  // MARK PHASE
  //
  // We have to mark everything reachable from the "register machine"
  // registers.

  mark(root_envt);
  mark(r_env);
  mark(Cell::car(&r_argl));
  mark(Cell::cdr(&r_argl));
  mark(Cell::car(&r_varl));
  mark(Cell::cdr(&r_varl));
  mark(r_proc);
  mark(r_exp);
  mark(r_unev);
  mark(r_val);
  mark(r_tmp);
  mark(r_elt);
  mark(r_nu);
  mark(cc_procedure);
  mark(empty_vector);

  // Mark the things is the compiler VM.
  //

  mark(r_cproc);
  mark(r_envt);

  // Mark everything reachable from the machine stack.  Watch out
  // for integers hiding in the machine stack, though!  They are
  // marked with the ATOM flag.

  for (int ix = 0; ix < m_stack.size(); ++ix)
    if ((reinterpret_cast<intptr_t>((p = m_stack[ix])) & Cell::ATOM) == 0)
      mark(p);

  // Mark the I/O ports referenced in this environment stack.

  for (int ix = 0; ix < istack.size(); ++ix)
    mark(istack[ix]);
  for (int ix = 0; ix < ostack.size(); ++ix)
    mark(ostack[ix]);

  // Mark the things that "C" implementations of Scheme functions
  // have requested protection for.

  for (int ix = 0; ix < r_gcp.size(); ++ix)
    mark(r_gcp[ix]);

  //
  // SWEEP PHASE
  //

  for (int ix = 0; ix < mem.active.size(); ++ix)
    ((Slab *)mem.active[ix])->sweep(this);

  // If this mark/sweep phase managed to reduce the cell utilization
  // to <= 80% of the allocated cells, we consider that success.  On
  // the other hand, if the GC produced less than 20% free cells, we
  // set a flag which will provoke the allocation of a new slab at
  // the next allocation failure.  In this way we hope to avoid
  // "grinding away" at the last few cells in a slab.

  if ((double)cellsAlloc / cellsTotal > 0.8)
    mem.low_water = true;

  if (gc_verbose)
    printf(";   end gc: %d/%d %s\n", cellsAlloc, cellsTotal,
           mem.low_water ? " low" : " ok");
}

void Context::gc_if_needed() {
  if (cellsAlloc >= cellsTotal / 4 * 3)
    gc();
}

void Context::print_mem_stats(FILE *out) {
  fprintf(out, "; mem %d/%d\n", cellsAlloc, cellsTotal);
}


bool Cell::is_symbol(psymbol s) const {
  if (short_atom(this)) return false;
  return type() == Cell::Symbol && std::get<Cell::SymbolVal>(val).s == s;
}

void *Context::xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p) {
    fprintf(stderr, "Virtual memory exhausted\\n");
    exit(1);
  }
  return p;
}


const Cell::InsnVal* Cell::InsnValue() const {
  typecheck(Insn);
  return &std::get<InsnVal>(val);
}

Cell::InsnVal* Cell::InsnValue() {
  typecheck(Insn);
  return &std::get<InsnVal>(val);
}
