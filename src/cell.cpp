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
  c->ca.p = c->cd.p = &Cell::Nil;
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
  c->cd.i = i;

  return c;
}

Cell *Context::make_char(char ch) {
  Cell *c = alloc(Cell::Char);
  c->cd.c = ch;
  return c;
}

Cell *Context::make_real(double d) {
  Cell *c = alloc(Cell::Real);
  double *pd = (double *)malloc(sizeof(double));
  *pd = d;
  c->cd.d = pd;
  return c;
}

// Context::make_string
//   Makes a string of the indicated length -- it is UNINITIALIZED

Cell *Context::make_string(size_t len) {
  Cell *c = alloc(Cell::String);
  size_t boxsize = sizeof(Cell::StringBox) + len + 1;
  Cell::StringBox *pbox = (Cell::StringBox *)xmalloc(boxsize);
  pbox->length = len;
  c->cd.s = pbox;
  return c;
}

Cell *Context::make_string(int len, char ch) {
  Cell *c = make_string(len);
  memset(c->cd.s->s, ch, len);
  c->cd.s->s[len] = '\0';
  return c;
}

Cell *Context::make_string(const char *s) { return make_string(s, strlen(s)); }

Cell *Context::make_string(const char *s, size_t len) {
  Cell *c = make_string(len);
  strncpy(c->cd.s->s, s, len);
  c->cd.s->s[len] = '\0';
  return c;
}

Cell *Context::make_subr(subr_f s, const char *name) {
  Cell *c = alloc(Cell::Subr);
  Cell::SubrBox *psubr = new Cell::SubrBox();
  psubr->subr = s;
  psubr->name = name;
  c->cd.f = psubr;
  return c;
}

Cell *Context::make_builtin(psymbol y) {
  Cell *c = alloc(Cell::Builtin);
  c->cd.y = y;
  return c;
}

Cell *Context::make_symbol(psymbol y) {
  Cell *c = alloc(Cell::Symbol);
  c->cd.y = y;

  return c;
}

Cell *Context::make_boolean(bool b) {
  return b ? &Cell::Bool_T : &Cell::Bool_F;
}

Cell *Context::make_vector(int n, Cell *init /* = &Unspecified */) {
  Cell *c = alloc(Cell::Vec);
  c->cd.cv = cellvector::alloc(n);
  c->flag(Cell::VREF, true);

  for (int ix = 0; ix < n; ++ix)
    c->cd.cv->set(ix, init);

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
  c->cd.ip = ip;

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
  c->cd.op = op;

  return c;
}

Cell *Context::make(Cell *ca, Cell *cd /* = &Nil*/) {
  Cell *c = alloc(Cell::Cons);
  c->ca.p = ca;
  c->cd.p = cd;
  return c;
}

Cell *Context::make_magic(void *key, magic_set_f set_f, magic_get_f get_f) {
  Cell *c = alloc(Cell::Magic);
  Cell::MagicBox *mbox = (Cell::MagicBox *)xmalloc(sizeof(Cell::MagicBox));
  mbox->key = key;
  mbox->set_f = set_f;
  mbox->get_f = get_f;
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
  if (this == that) // the easy case
    return true;
  if (short_atom(this) || short_atom(that))
    return false; // then the above case would have detected equality
  if (long_atom(ca.p) && long_atom(that->ca.p)) {
    bool part1 =
        (ca.i & IGN_MASK) == (that->ca.i & IGN_MASK) && cd.i == that->cd.i;

    return part1;
  }
  // If both are conses, they are eq iff they are the same cons.
  // But that would have been detected by the first test.
  return false;
}

bool Cell::equal(Cell *c) {
  Type t0 = type();
  Type t1 = c->type();

  if (this == &Nil && c == &Nil)
    return true;
  else if (t0 == Cons && t1 == Cons)
    return ca.p->equal(c->ca.p) && cd.p->equal(c->cd.p);
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

Cell *Cell::caar(Cell *c) { return Cell::car(Cell::car(c)); }
Cell *Cell::cadr(Cell *c) { return Cell::car(Cell::cdr(c)); }
Cell *Cell::cdar(Cell *c) { return Cell::cdr(Cell::car(c)); }
Cell *Cell::cddr(Cell *c) { return Cell::cdr(Cell::cdr(c)); }
Cell *Cell::caaar(Cell *c) { return Cell::car(Cell::caar(c)); }
Cell *Cell::caadr(Cell *c) { return Cell::car(Cell::cadr(c)); }
Cell *Cell::cadar(Cell *c) { return Cell::car(Cell::cdar(c)); }
Cell *Cell::caddr(Cell *c) { return Cell::car(Cell::cddr(c)); }
Cell *Cell::cdaar(Cell *c) { return Cell::cdr(Cell::caar(c)); }
Cell *Cell::cdadr(Cell *c) { return Cell::cdr(Cell::cadr(c)); }
Cell *Cell::cddar(Cell *c) { return Cell::cdr(Cell::cdar(c)); }
Cell *Cell::cdddr(Cell *c) { return Cell::cdr(Cell::cddr(c)); }
Cell *Cell::caaaar(Cell *c) { return Cell::car(Cell::caaar(c)); }
Cell *Cell::caaadr(Cell *c) { return Cell::car(Cell::caadr(c)); }
Cell *Cell::caadar(Cell *c) { return Cell::car(Cell::cadar(c)); }
Cell *Cell::caaddr(Cell *c) { return Cell::car(Cell::caddr(c)); }
Cell *Cell::cadaar(Cell *c) { return Cell::car(Cell::cdaar(c)); }
Cell *Cell::cadadr(Cell *c) { return Cell::car(Cell::cdadr(c)); }
Cell *Cell::caddar(Cell *c) { return Cell::car(Cell::cddar(c)); }
Cell *Cell::cadddr(Cell *c) { return Cell::car(Cell::cdddr(c)); }
Cell *Cell::cdaaar(Cell *c) { return Cell::cdr(Cell::caaar(c)); }
Cell *Cell::cdaadr(Cell *c) { return Cell::cdr(Cell::caadr(c)); }
Cell *Cell::cdadar(Cell *c) { return Cell::cdr(Cell::cadar(c)); }
Cell *Cell::cdaddr(Cell *c) { return Cell::cdr(Cell::caddr(c)); }
Cell *Cell::cddaar(Cell *c) { return Cell::cdr(Cell::cdaar(c)); }
Cell *Cell::cddadr(Cell *c) { return Cell::cdr(Cell::cdadr(c)); }
Cell *Cell::cdddar(Cell *c) { return Cell::cdr(Cell::cddar(c)); }
Cell *Cell::cddddr(Cell *c) { return Cell::cdr(Cell::cdddr(c)); }

psymbol Cell::SymbolValue() const {
  typecheck(Symbol);
  return cd.y;
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
  if (short_atom(this))
    return reinterpret_cast<intptr_t>(this) >> 8;

  typecheck(Int);
  return cd.i;
}

char Cell::CharValue() const {
  typecheck(Char);
  return cd.c;
}

Cell::SubrBox *Cell::SubrValue() const {
  typecheck(Subr);
  return cd.f;
}

char *Cell::StringValue() const {
  typecheck(String);
  return cd.s->s;
}

size_t Cell::StringLength() const {
  typecheck(String);
  return cd.s->length;
}

FILE *Cell::IportValue() const {
  typecheck(Iport);
  return cd.ip;
}

FILE *Cell::OportValue() const {
  typecheck(Oport);
  return cd.op;
}

void *Cell::ContValue() const {
  typecheck(Cont);
  return cd.j;
}

cellvector *Cell::VectorValue() const {
  typecheck(Vec);
  return cd.cv;
}

cellvector *Cell::CProcValue() const {
  typecheck(Cproc);
  return cd.cv;
}

Cell *Cell::PromiseValue() const {
  typecheck(Promise);
  return cd.cv->get(0);
}

Cell *Cell::CPromiseValue() const {
  typecheck(Cpromise);
  return cd.cv->get(0);
}

psymbol Cell::BuiltinValue() const {
  typecheck(Builtin);
  return cd.y;
}

Cell::Procedure Cell::LambdaValue() const {
  typecheck(Lambda);
  return Procedure(cd.cv->get(0), cd.cv->get(1), cd.cv->get(2));
}

double Cell::RealValue() const {
  typecheck(Real);
  return *(cd.d);
}

const char *Cell::name() const { return typeName[type()]; }

void Cell::typefail(Type t1, Type t2) const {
  sprintf(OS::errbuf, "type check failure: wanted %s, got %s", typeName[t2],
          typeName[t1]); /* XXX sprintf into fixed buf */

  OS::exception();
}

void Cell::dump(FILE *out) {
  Type t = type();
  fprintf(out, "[%p ", this);
  if (ca.i == (FREE | ATOM))
    fputs("free ", out);
  else {
    if (ca.i & MARK)
      fputs("mark ", out);
    if (short_atom(ca.p)) {
      printf("short %" PRIdPTR " ", ca.p->IntValue());
    } else {
      if (ca.i & ATOM) {
        printf("atom %04" PRIxPTR " ", ca.i);
        if (ca.i & FORCED)
          fputs("forced ", out);
        if (ca.i & QUICK)
          fputs("quick ", out);
        if (ca.i & MACRO)
          fputs("macro ", out);
        if (ca.i & VREF)
          fputs("vref ", out);
      }

      fputs(typeName[t], out);

      switch (t) {
      case Cons:
        fputs(" ", out);
        if (ca.p == nil)
          fputs("nil", out);
        else
          fprintf(out, "%p", ca.p);
        fputs(" ", out);
        if (cd.p == nil)
          fputs("nil", out);
        else
          fprintf(out, "%p", cd.p);
        break;

      case Int:
        fprintf(out, " %" PRIdPTR, cd.i);
        break;
      case Real:
        fprintf(out, " %g", RealValue());
        break;
      case Unique:
        fprintf(out, " %s", cd.u);
        break;
      case Symbol:
        fprintf(out, " %s", SymbolValue()->key);
      default:
        break;
      }
    }
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

void cellvector::vref_error() { error("vector reference out of bounds"); }

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
    mem.free = a->cd.p;
    a->ca.i = a->cd.i = 0;
    a->set_type(t);
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
    mem.no_inline_gc = OS::flag(DEBUG_NO_INLINE_GC);
  }

  // Check the "top" slab to see if there's any room
  // left in it.

  if ((a = mem.current()->alloc())) {
    ++cellsAlloc;
    a->cd.i = 0;
    a->set_type(t);
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

inline Cell *Cell::untagged(Cell *c) {
  static const uintptr_t not_tagmask = ~Cell::TAGMASK;

  return reinterpret_cast<Cell *>(reinterpret_cast<uintptr_t>(c) & not_tagmask);
}

inline void Cell::gc_set_car(Cell *src) {
  unsigned int tagbits = ca.i & TAGMASK;
  ca.p = src;
  ca.i |= tagbits;
}

inline void Cell::gc_set_cdr(Cell *src) {
  unsigned int tagbits = cd.i & TAGMASK;
  setcdr(this, src);
  cd.i |= tagbits;
}

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
  bool traceall = OS::flag(TRACE_GC_ALL);
  if (P == nil || P == 0 || Cell::short_atom(P) || P->ca.i & Cell::MARK)
    return;

  // In Knuth's presentation, a NODE contains two pointers
  // (which he calls ALINK and BLINK, we car and cdr), and
  // MARK and ATOM fields.  In his layout, the MARK and
  // ATOM fields can be manipulated easily without changing
  // ALINK and BLINK, but in our case we store MARK and ATOM
  // in the lower three bits of ALINK.  We must therefore
  // be cautious when transcribing the algorithm to avoid,
  // e.g., clearing MARK and ATOM when copying a `car' pointer.
  // We use "gc_set_car" and "gc_set_cdr" for this purpose.

  // Secondly, Knuth occasionally sets the ATOM bit of a CONS
  // to determine which of the pointers has been placed on
  // the stack of deferred objects.  But short atoms makes this
  // difficult for us, as it's possible to have a cons of two
  // short integers, say: then we need both ATOM fields of the
  // CONS to contain that information.  Instead of using the ATOM
  // field, we use the MARK field of the cdr, which is not used
  // for GC purposes.

  // E1:
  Cell *T = nil;
  Cell *Q = nil;

E2:
  P->ca.i |= Cell::MARK;
  if (traceall) {
    printf("m ");
    P->dump(stdout);
    putchar('\n');
  }

  // -- EXTENSION to Knuth's Step E2
  //
  // If the cell is a cons, Knuth's algorithm will take care of
  // marking the things referenced as a result quite handily.  But
  // there are some atoms that can hold references too.  Knuth's
  // algorithm works for binary trees, but to deal with vectors et
  // al. we need to make it work for n-way trees.
  //
  // When an atom can hold references to other cells, we organize
  // these into a cellvector.  In this way, we can treat all of them
  // the same way.
  //
  // Whereas Knuth uses an atom bit to tell which side of a cons
  // (car or cdr) he has stashed the pointer back to the
  // as-yet-unmarked cells, when we traverse a vector we use an
  // auxiliary integer field to tell us how many vector slots we
  // have marked so far.

  if (Cell::atomic(P)) {
    if (P->flag(Cell::VREF)) {
      // Getting "here" in the code means that we're seeing the
      // vector of additional cell references for the first time
      // (otherwise the mark bit will already be set).  Our job is
      // to kick off the iteration by stashing the back-link and
      // starting the mark counter.  The rest of the iteration will
      // be handled in the "up" step below.

      if (P->cd.cv->size() > 0) {
        P->cd.cv->gc_uplink = T;
        P->cd.cv->gc_index = 0;
        T = P;
      }
    } else if (P->type() == Cell::Symbol) {
      // Symbols have property-list vectors, and so receive
      // similar treatment to the above.  But don't do this
      // if we've already started to mark the properties
      // (gc_uplink will be non-NULL in that case).

      psymbol ps = P->SymbolValue();
      if (ps->plist && ps->plist->gc_uplink == 0 && ps->plist->size() > 0) {
        ps->plist->gc_uplink = T;
        ps->plist->gc_index = 0;
        T = P;
      }
    }

    goto E6; // E3
  }

  if (!Cell::short_atom(P->ca.p)) {
    Q = Cell::untagged(P->ca.p); // E4
    if (Q != nil && !(Q->ca.i & Cell::MARK)) {
      // if (!Cell::short_atom(P->cd.p))
      // {
      // P->cd.i |= Cell::ATOM;
      P->cd.i |= Cell::MARK;
      // END
      P->gc_set_car(T);
      T = P;
      // }
      P = Q;
      goto E2;
    }
  }

E5:
  if (!Cell::short_atom(P->cd.p)) {
    Q = Cell::untagged(P->cd.p);
    if (Q != nil && !(Q->ca.i & Cell::MARK)) {
      P->gc_set_cdr(T);
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
  // We are popping a vector cell from the GC stack.
  // If there are more cells to mark within it, keep
  // going.

  next_element:

    int i = Q->cd.cv->gc_index++;

    if (i >= Q->cd.cv->size()) // all done?
    {
      T = Q->cd.cv->gc_uplink;
      Q->cd.cv->gc_index = 0; // reset for next time
      P = Q;
      goto E6;
    } else // resume iteration
    {
      P = Q->cd.cv->get(i); // with next element

      // One wrinkle: captured continuations are implemented
      // as vectors, and like the machine stack, these vectors
      // can contain integer VM codes as well as cell pointers.
      // These latter are marked with the ATOM flag.

      if (reinterpret_cast<intptr_t>(P) & Cell::ATOM)
        goto next_element;

      // Otherwise we mark, if not marked already.
      if (P->ca.i & Cell::MARK)
        goto next_element;

      P = Cell::untagged(P);
      if (P == nil)
        goto next_element;

      goto E2;
    }
  } else if (Q->type() == Cell::Symbol) {
    // Continue iterating over the property list of a symbol.
    psymbol ps = Q->SymbolValue();

  next_property:
    int i = ps->plist->gc_index++;
    if (i >= ps->plist->size()) // all done?
    {
      T = ps->plist->gc_uplink;
      ps->plist->gc_index = 0;
      ps->plist->gc_uplink = 0;
      P = Q;
      goto E6;
    } else {
      P = ps->plist->get(i);
      if (P->ca.i & Cell::MARK)
        goto next_property;
      P = Cell::untagged(P);
      if (P == nil)
        goto next_property;
      goto E2;
    }
  }

  //    if (Q->cd.i & Cell::ATOM)
  if (Q->cd.i & Cell::MARK) {
    //    Q->cd.i &= ~Cell::ATOM;
    Q->cd.i &= ~Cell::MARK;
    T = Cell::untagged(Q->ca.p);
    Q->gc_set_car(P);
    P = Q;
    goto E5;
  } else {
    T = Cell::untagged(Q->cd.p);
    Q->gc_set_cdr(P);
    P = Q;
    goto E6;
  }
}

void Slab::sweep(Context *ctx) {
  bool traceall = OS::flag(TRACE_GC_ALL);

  for (Cell *p = start; p < next; ++p) {
    unsigned int word = p->ca.i;

    if (word & Cell::MARK) {
      p->ca.i &= ~Cell::MARK;
    } else if (word != (Cell::FREE | Cell::ATOM)) {
      // FINALIZATION
      //

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
      case Cell::Vec: // Free the vector of cell pointers.
        p->cd.cv->free();
        // XXX delete p->cd.cv;
        p->cd.cv = 0;
        break;

      case Cell::Iport: // Ports hold streams
        fclose(p->cd.ip);
        break;

      case Cell::Oport: // Ports hold streams
        fclose(p->cd.op);
        break;

      case Cell::Real: // Reals hold a malloc'd double
        free(p->cd.d);
        break;

      case Cell::Subr: // Subrs hold a SubrBox
        free(p->cd.f);
        break;

      case Cell::Magic: // Magic cells hold a MagicBox
        free(p->cd.m);
        break;

      case Cell::String: // Strings hold StringBoxes
        free(p->cd.s);
        break;

      default: // Ordinarily cells hold no other storage.
               ;
      }

      --ctx->cellsAlloc;
      p->ca.i = Cell::FREE | Cell::ATOM;
      p->cd.p = ctx->mem.free;
      ctx->mem.free = p;
      ++ctx->mem.c_free;
    }
  }
}

void Context::gc() {
  bool gc_verbose = OS::flag(TRACE_GC);
  Cell *p;

  if (!ok_to_gc) {
    fprintf(stderr, "initial memory budget insufficient to set up VM\n"
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

void *Context::xmalloc(size_t sz) {
  void *v = malloc(sz);
  if (!v)
    error("out of heap memory");
  return v;
}
