//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// subr.cpp : C implementations of Scheme primitives.

#include "vx-scheme.h"
#include <cmath>
#include <errno.h>
#include <float.h>
#include <functional>
#include <limits>

//---------------------------------------------------------------------
// Utilities
//

static FILE *oport(Context *ctx, Cell *arglist) {
  if (arglist != nil)
    return car(arglist)->OportValue();
  else
    return ctx->current_output()->OportValue();
}

static FILE *iport(Context *ctx, Cell *arglist) {
  if (arglist != nil)
    return car(arglist)->IportValue();
  else
    return ctx->current_input()->IportValue();
}

// exact_list canvasses the given arglist.  If all the arguments
// are integer type, exact_p returns true (indicating that integer
// math is appropriate to combine them with.)  If at least one
// real is found, it returns false (suggesting that the args
// should be promoted to real type before combination.  If any
// other type is encountered, an error is thrown.

static bool exact_list(Cell *arglist) {
  for (Cell *a = arglist; a != nil; a = Cell::cdr(a)) {
    if (!Cell::car(a)->is<intptr_t>())
      return false;
  }
  return true;
}

inline static double asReal(Cell *c) {
  if (c->is<intptr_t>())
    return (double)c->IntValue();
  else
    return c->RealValue();
}

//---------------------------------------------------------------------
// THE PRIMITIVE PROCEDURES
//

Cell *skcons(Context *ctx, Cell *arglist) {
  return ctx->cons(car(arglist), cadr(arglist));
}

Cell *skplus(Context *ctx, Cell *arglist) {
  if (exact_list(arglist)) {
    intptr_t result = 0;

    for (Cell *p = arglist; p != nil; p = Cell::cdr(p))
      result += car(p)->IntValue();

    return ctx->make_int(result);
  } else {
    double result = 0;

    for (Cell *p = arglist; p != nil; p = Cell::cdr(p))
      result += asReal(car(p));

    return ctx->make_real(result);
  }
}

Cell *skminus(Context *ctx, Cell *arglist) {
  if (exact_list(arglist)) {
    intptr_t result = car(arglist)->IntValue();
    arglist = cdr(arglist);

    if (arglist == nil)
      return ctx->make_int(-result);

    for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
      result -= car(a)->IntValue();

    return ctx->make_int(result);
  } else {
    double result = asReal(car(arglist));
    arglist = cdr(arglist);

    if (arglist == nil)
      return ctx->make_real(-result);

    for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
      result -= asReal(car(a));

    return ctx->make_real(result);
  }
}

Cell *divide(Context *ctx, Cell *arglist) {
  double result;

  if (cdr(arglist) != nil) {
    // The usual case: there are at least 2 arguments.
    // (/ a b c ...) ==> ((a / b) / c ...)

    result = asReal(car(arglist));

    for (Cell *a = cdr(arglist); a != nil; a = Cell::cdr(a))
      result = result / asReal(car(a));
  } else {
    // A single argument means take its reciprocal.

    result = 1.0 / asReal(car(arglist));
  }

  return ctx->make_real(result);
}

Cell *times(Context *ctx, Cell *arglist) {
  if (exact_list(arglist)) {
    intptr_t result = 1;

    for (Cell *p = arglist; p != nil; p = Cell::cdr(p))
      result *= Cell::car(p)->IntValue();

    return ctx->make_int(result);
  } else {
    double result = 1.0;

    for (Cell *p = arglist; p != nil; p = Cell::cdr(p))
      result *= asReal(car(p));

    return ctx->make_real(result);
  }
}

Cell *skmax(Context *ctx, Cell *arglist) {
  if (exact_list(arglist)) {
    intptr_t m = std::numeric_limits<intptr_t>::min();
    intptr_t z;

    for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
      if ((z = Cell::car(a)->IntValue()) > m)
        m = z;

    return ctx->make_int(m);
  } else {
    double m = DBL_MIN;
    double z;

    for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
      if ((z = asReal(car(a))) > m)
        m = z;

    return ctx->make_real(m);
  }
}

Cell *skmin(Context *ctx, Cell *arglist) {
  if (exact_list(arglist)) {
    intptr_t m = std::numeric_limits<intptr_t>::max();
    intptr_t z;

    for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
      if ((z = car(a)->IntValue()) < m)
        m = z;

    return ctx->make_int(m);
  } else {
    double m = DBL_MAX;
    double z;

    for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
      if ((z = asReal(car(a))) < m)
        m = z;

    return ctx->make_real(m);
  }
}

Cell *skabs(Context *ctx, Cell *arglist) {
  Cell *c = car(arglist);
  if (c->is<intptr_t>())
    return ctx->make_int(std::abs(c->IntValue()));
  else if (c->is<double>())
    return ctx->make_real(fabs(c->RealValue()));
  else
    error("numeric type expected");
  return nil; // for compiler
}

// BINOP is a macro which constructs a binary operator
// out of a fragment of C code (OP).  This works on
// non numeric types (i.e., those that do not participate
// in coercion).

template <typename ValueExtractor, typename Op>
static Cell *typed_compare(Context *ctx, Cell *args, ValueExtractor extract,
                           Op op) {
  for (Cell *a = args; a != nil; a = Cell::cdr(a)) {
    if (Cell::cdr(a) != nil) {
      const auto &ia = extract(Cell::car(a));
      const auto &ib = extract(Cell::cadr(a));
      if (!op(ia, ib))
        return &Cell::Bool_F;
    }
  }
  return &Cell::Bool_T;
}

static int strcmp_ci(const std::string &s, const std::string &t) {
  size_t min_len = std::min(s.length(), t.length());
  for (size_t i = 0; i < min_len; ++i) {
    unsigned char u1 = static_cast<unsigned char>(tolower(s[i]));
    unsigned char u2 = static_cast<unsigned char>(tolower(t[i]));
    if (u1 != u2)
      return u1 - u2;
  }
  if (s.length() < t.length())
    return -1;
  if (s.length() > t.length())
    return 1;
  return 0;
}

struct CharCiEqual {
  bool operator()(char a, char b) const {
    return tolower(static_cast<unsigned char>(a)) ==
           tolower(static_cast<unsigned char>(b));
  }
};
struct CharCiLess {
  bool operator()(char a, char b) const {
    return tolower(static_cast<unsigned char>(a)) <
           tolower(static_cast<unsigned char>(b));
  }
};
struct CharCiLessEqual {
  bool operator()(char a, char b) const {
    return tolower(static_cast<unsigned char>(a)) <=
           tolower(static_cast<unsigned char>(b));
  }
};
struct CharCiGreater {
  bool operator()(char a, char b) const {
    return tolower(static_cast<unsigned char>(a)) >
           tolower(static_cast<unsigned char>(b));
  }
};
struct CharCiGreaterEqual {
  bool operator()(char a, char b) const {
    return tolower(static_cast<unsigned char>(a)) >=
           tolower(static_cast<unsigned char>(b));
  }
};

template <typename Op> static Cell *char_compare(Context *ctx, Cell *args) {
  return typed_compare(
      ctx, args, [](const Cell *c) { return c->CharValue(); }, Op{});
}

static constexpr subr_f char_eq = char_compare<std::equal_to<>>;
static constexpr subr_f char_le = char_compare<std::less_equal<>>;
static constexpr subr_f char_lt = char_compare<std::less<>>;
static constexpr subr_f char_ge = char_compare<std::greater_equal<>>;
static constexpr subr_f char_gt = char_compare<std::greater<>>;
static constexpr subr_f char_eq_ci = char_compare<CharCiEqual>;
static constexpr subr_f char_le_ci = char_compare<CharCiLessEqual>;
static constexpr subr_f char_lt_ci = char_compare<CharCiLess>;
static constexpr subr_f char_ge_ci = char_compare<CharCiGreaterEqual>;
static constexpr subr_f char_gt_ci = char_compare<CharCiGreater>;

struct StringCiEqual {
  bool operator()(const std::string &a, const std::string &b) const {
    return strcmp_ci(a, b) == 0;
  }
};
struct StringCiLess {
  bool operator()(const std::string &a, const std::string &b) const {
    return strcmp_ci(a, b) < 0;
  }
};
struct StringCiLessEqual {
  bool operator()(const std::string &a, const std::string &b) const {
    return strcmp_ci(a, b) <= 0;
  }
};
struct StringCiGreater {
  bool operator()(const std::string &a, const std::string &b) const {
    return strcmp_ci(a, b) > 0;
  }
};
struct StringCiGreaterEqual {
  bool operator()(const std::string &a, const std::string &b) const {
    return strcmp_ci(a, b) >= 0;
  }
};

template <typename Op> static Cell *string_compare(Context *ctx, Cell *args) {
  return typed_compare(
      ctx, args,
      [](const Cell *c) -> const std::string & { return c->StringValue(); },
      Op{});
}

static constexpr subr_f string_eq = string_compare<std::equal_to<>>;
static constexpr subr_f string_le = string_compare<std::less_equal<>>;
static constexpr subr_f string_lt = string_compare<std::less<>>;
static constexpr subr_f string_ge = string_compare<std::greater_equal<>>;
static constexpr subr_f string_gt = string_compare<std::greater<>>;
static constexpr subr_f string_eq_ci = string_compare<StringCiEqual>;
static constexpr subr_f string_le_ci = string_compare<StringCiLessEqual>;
static constexpr subr_f string_lt_ci = string_compare<StringCiLess>;
static constexpr subr_f string_ge_ci = string_compare<StringCiGreaterEqual>;
static constexpr subr_f string_gt_ci = string_compare<StringCiGreater>;

template <typename Op> static Cell *numeric_compare(Context *ctx, Cell *args) {
  Op op;
  bool exact = exact_list(args);
  for (Cell *a = args; a != nil; a = Cell::cdr(a)) {
    if (Cell::cdr(a) != nil) {
      if (exact) {
        intptr_t ia = Cell::car(a)->IntValue();
        intptr_t ib = Cell::cadr(a)->IntValue();
        if (!op(ia, ib))
          return &Cell::Bool_F;
      } else {
        double da = asReal(Cell::car(a));
        double db = asReal(Cell::cadr(a));
        if (!op(da, db))
          return &Cell::Bool_F;
      }
    }
  }
  return &Cell::Bool_T;
}

static constexpr subr_f number_equal = numeric_compare<std::equal_to<>>;
static constexpr subr_f le = numeric_compare<std::less_equal<>>;
static constexpr subr_f lt = numeric_compare<std::less<>>;
static constexpr subr_f ge = numeric_compare<std::greater_equal<>>;
static constexpr subr_f gt = numeric_compare<std::greater<>>;

template <int (*Predicate)(int)>
static Cell *char_class(Context *ctx, Cell *args) {
  Cell *charptr = Cell::car(args);
  return ctx->make_boolean(Predicate(charptr->CharValue()) != 0);
}

static constexpr subr_f alphabetic_p = char_class<isalpha>;
static constexpr subr_f lower_case_p = char_class<islower>;
static constexpr subr_f upper_case_p = char_class<isupper>;
static constexpr subr_f numeric_p = char_class<isdigit>;
static constexpr subr_f whitespace_p = char_class<isspace>;

Cell *negative_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist)->IntValue() < 0);
}

Cell *positive_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist)->IntValue() > 0);
}

Cell *even_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean((car(arglist)->IntValue() & 1) == 0);
}

Cell *odd_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean((car(arglist)->IntValue() & 1) == 1);
}

Cell *eq(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist)->eq(cadr(arglist)));
}

Cell *eqv(Context *ctx, Cell *arglist) {
  // If they're both real, compare them as numbers; else use eq
  if (car(arglist)->is<double>() && cadr(arglist)->is<double>())
    return ctx->make_boolean(car(arglist)->RealValue() ==
                             cadr(arglist)->RealValue());
  return eq(ctx, arglist);
}

Cell *equal_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist)->equal(cadr(arglist)));
}

Cell *length(Context *ctx, Cell *arglist) {
  return ctx->make_int(car(arglist)->length());
}

Cell *sknot(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(!car(arglist)->istrue());
}

Cell *display(Context *ctx, Cell *arglist) {
  car(arglist)->display(oport(ctx, cdr(arglist)));
  return unspecified;
}

Cell *display_star(Context *ctx, Cell *arglist) {
  for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
    car(a)->display(oport(ctx, nil));
  return unspecified;
}

Cell *write(Context *ctx, Cell *arglist) {
  car(arglist)->write(oport(ctx, cdr(arglist)));
  return unspecified;
}

Cell *write_char(Context *ctx, Cell *arglist) {
  fputc(car(arglist)->CharValue(), oport(ctx, cdr(arglist)));
  return unspecified;
}

Cell *skmake_vector(Context *ctx, Cell *arglist) {
  intptr_t n = car(arglist)->IntValue();

  if (cdr(arglist) != nil)
    return ctx->make_vector(n, cadr(arglist));

  return ctx->make_vector(n);
}

Cell *vector_ref(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();
  int n = cadr(arglist)->IntValue();

  return v->get(n);
}

Cell *vector_set(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();
  int n = cadr(arglist)->IntValue();

  v->set(n, caddr(arglist));
  return unspecified;
}

Cell *vector_fill(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();
  Cell *filler = cadr(arglist);
  int sz = v->size();

  for (int ix = 0; ix < sz; ++ix)
    v->set(ix, filler);

  return unspecified;
}

Cell *vector_length(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();

  return ctx->make_int(v->size());
}

// Flexible vector functions.  These are outside the Scheme standard,
// but very useful in practice.  Essentially the following four functions
// allow the resizing of vectors via the standard deque operations.
// We borrow the nomenclature from Perl:  "vector-push!" adds a new
// element to the right end of a vector; "vector-pop!" detaches the
// right-most element of a vector and returns it.  "vector-unshift!"
// and "vector-shift!" do the same thing at the left side of the vector.

Cell *vector_push(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();
  v->push(cadr(arglist));
  return unspecified;
}

Cell *vector_pop(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();
  return (v->pop());
}

Cell *vector_shift(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();
  return v->shift();
}

Cell *vector_unshift(Context *ctx, Cell *arglist) {
  cellvector *v = car(arglist)->VectorValue();
  v->unshift(cadr(arglist));
  return unspecified;
}

Cell *vector_from_list(Context *ctx, Cell *arglist) {
  int n = arglist->length();
  Cell *v = ctx->make_vector(n);
  cellvector *vec = v->VectorValue();
  int ix = 0;

  ctx->gc_protect(v);
  for (Cell *elt = arglist; elt != nil; elt = Cell::cdr(elt))
    vec->set(ix++, car(elt));
  ctx->gc_unprotect();
  return v;
}

Cell *vector_to_list(Context *ctx, Cell *arglist) {
  Cell::List list;
  Cell *elt;
  cellvector *vec = car(arglist)->VectorValue();
  int n = vec->size();

  ctx->gc_protect(list.head());
  for (int ix = 0; ix < n; ++ix) {
    elt = ctx->make(vec->get(ix));
    ctx->gc_protect(elt);
    list.append(elt);
    ctx->gc_unprotect(2);
    ctx->gc_protect(list.head());
  }
  ctx->gc_unprotect();
  return list.head();
}

Cell *list_ref(Context *ctx, Cell *arglist) {
  Cell *list = car(arglist);
  int n = cadr(arglist)->IntValue();
  int ix = 0;

  for (Cell *a = list; a != nil; a = Cell::cdr(a))
    if (ix++ == n)
      return car(a);

  error("index out of bounds");
  return unimplemented;
}

Cell *quotient(Context *ctx, Cell *arglist) {
  int d = cadr(arglist)->IntValue();
  if (d == 0)
    error("quotient /0");

  return ctx->make_int(car(arglist)->IntValue() / d);
}

Cell *remainder(Context *ctx, Cell *arglist) {
  int n = car(arglist)->IntValue();
  int d = cadr(arglist)->IntValue();
  if (d == 0)
    error("remainder /0");

  return ctx->make_int(n % d);
}

Cell *modulo(Context *ctx, Cell *arglist) {
  int n = car(arglist)->IntValue();
  int d = cadr(arglist)->IntValue();
  int m = n % d;
  if (m < 0 && d > 0)
    return ctx->make_int(m + d);
  if (m > 0 && d < 0)
    return ctx->make_int(m + d);
  return ctx->make_int(m);
}

//---------------------------------------------------------------------
// gcd2 (u,v)
//
// Computes the greates common divisor of the given two integers.
// This implementation is Knuth's Algorithm 4.5.2B (TAoCP 3ed. vol II
// p. 338).  The variables and label names are as in Knuth's
// presentation and we refer the reader there for further
// documentation.
//

static int gcd2(int u, int v) {
  if (u == 0)
    return abs(v);
  if (v == 0)
    return abs(u);
  u = abs(u);
  v = abs(v);

  // B1:
  int k = 0, t;
  while ((u & 1) + (v & 1) == 0) {
    k++;
    u >>= 1;
    v >>= 1;
  }
  // B2
  if (u & 1) {
    t = -v;
    goto B4;
  }
  t = u;
B3:
  t >>= 1;
B4:
  if ((t & 1) == 0)
    goto B3;
  // B5:
  if (t > 0)
    u = t;
  else
    v = -t;
  // B6:
  t = u - v;
  if (t)
    goto B3;
  return u << k;
}

Cell *gcd(Context *ctx, Cell *arglist) {
  int g = 0;

  for (Cell *i = arglist; i != nil; i = Cell::cdr(i))
    g = gcd2(g, car(i)->IntValue());

  return ctx->make_int(g);
}

Cell *lcm(Context *ctx, Cell *arglist) {
  int product = 1;
  int g = 0;

  for (Cell *ip = arglist; ip != nil; ip = Cell::cdr(ip)) {
    int i = car(ip)->IntValue();
    product *= i;
    g = gcd2(g, i);
  }

  return ctx->make_int(g == 0 ? 1 : abs(product / g));
}

Cell *null_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist) == nil);
}

Cell *zero_p(Context *ctx, Cell *arglist) {
  Cell *a = car(arglist);
  if (a->is<intptr_t>())
    return ctx->make_boolean(a->IntValue() == 0);
  else
    return ctx->make_boolean(a->RealValue() == 0.0);
}

Cell *skfalse(Context *ctx, Cell *arglist) { return ctx->make_boolean(false); }

template <Cell *(*Accessor)(const Cell *)>
static Cell *cons_accessor(Context *ctx, Cell *a) {
  return Accessor(Cell::car(a));
}

template <typename T> static Cell *type_predicate(Context *ctx, Cell *a) {
  return ctx->make_boolean(Cell::car(a)->is<T>());
}

static constexpr subr_f string_p = type_predicate<std::string>;
static constexpr subr_f symbol_p = type_predicate<Cell::Symbol>;
static constexpr subr_f vector_p = type_predicate<Cell::Vec>;
static constexpr subr_f char_p = type_predicate<char>;
static constexpr subr_f input_p = type_predicate<Cell::Iport>;
static constexpr subr_f output_p = type_predicate<Cell::Oport>;
static constexpr subr_f integer_p = type_predicate<intptr_t>;
static constexpr subr_f exact_p = type_predicate<intptr_t>;
static constexpr subr_f inexact_p = type_predicate<double>;

static Cell *number_p(Context *ctx, Cell *a) {
  Cell *c = Cell::car(a);
  return ctx->make_boolean(c->is<intptr_t>() || c->is<double>());
}

static constexpr subr_f rational_p = number_p;
static constexpr subr_f real_p = number_p;
static constexpr subr_f complex_p = number_p;

Cell *pair_p(Context *ctx, Cell *arglist) {
  Cell *a = car(arglist);

  return ctx->make_boolean(a->ispair());
}

Cell *boolean_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist)->isBoolean());
}

Cell *procedure_p(Context *ctx, Cell *arglist) {
  Cell *a = car(arglist);

  return ctx->make_boolean(a->is<Cell::Subr>() || a->is<Cell::Lambda>() ||
                           a->is<Cell::Cont>() || a->is<Cell::Cproc>() ||
                           (a->is<Cell::Builtin>() && !a->macro()));
}

Cell *primitive_procedure_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist)->is<Cell::Subr>());
}

Cell *list_p(Context *ctx, Cell *arglist) {
  Cell *p0 = car(arglist);
  Cell *p = p0;

  while (true) {
    if (p == nil)
      return ctx->make_boolean(true);

    if (!p->is<Cell::Cons>())
      return ctx->make_boolean(false);

    p = Cell::cdr(p);

    if (p == p0)
      return ctx->make_boolean(false);
  }
}

Cell *number_to_string(Context *ctx, Cell *arglist) {
  Cell *a = car(arglist);
  if (a->is<intptr_t>()) {
    const char *fmt = "%d";

    if (cdr(arglist) != nil) {
      int base = cadr(arglist)->IntValue();

      if (base == 16)
        fmt = "%x";
      else if (base == 8)
        fmt = "%o";
      else if (base == 10)
        fmt = "%d";
      else
        error("unsupported output base"); // XXX
    }
    char buf[40];
    snprintf(buf, sizeof(buf), fmt, a->IntValue());
    return ctx->make_string(buf);
  } else if (a->is<double>()) {
    char buf[40];
    Cell::real_to_string(a->RealValue(), buf, sizeof(buf));
    return ctx->make_string(buf);
  }
  error("expected a number");
  return nil;
}

Cell *string_length(Context *ctx, Cell *arglist) {
  return ctx->make_int(static_cast<int>(car(arglist)->StringLength()));
}

Cell *newline(Context *ctx, Cell *arglist) {
  fputc('\n', oport(ctx, arglist));
  return unspecified;
}

Cell *string_to_list(Context *ctx, Cell *arglist) {
  Cell::List l;
  Cell *elt;
  const std::string &s = car(arglist)->StringValue();

  ctx->gc_protect(l.head());
  for (char c : s) {
    elt = ctx->make(ctx->make_char(c));
    ctx->gc_protect(elt);
    l.append(elt);
    ctx->gc_unprotect(2);
    ctx->gc_protect(l.head());
  }

  ctx->gc_unprotect();
  return l.head();
}

Cell *sklist(Context *ctx, Cell *arglist) { return arglist; }

Cell *skmake_string(Context *ctx, Cell *arglist) {
  int n = car(arglist)->IntValue();
  char ch = ' ';

  if (cdr(arglist) != nil)
    ch = cadr(arglist)->CharValue();

  return ctx->make_string(n, ch);
}

Cell *string_ref(Context *ctx, Cell *arglist) {
  Cell *pstr = car(arglist);
  int ix = cadr(arglist)->IntValue();
  int n = static_cast<int>(pstr->StringLength());

  if (ix < 0 || ix >= n)
    error("string index out of bounds");

  return ctx->make_char(pstr->StringValue()[ix]);
}

Cell *append(Context *ctx, Cell *arglist) {
  Cell::List alist;
  Cell *elt;

  if (arglist == nil)
    return nil;

  ctx->gc_protect(alist.head());
  while (cdr(arglist) != nil) {
    for (Cell *a = car(arglist); a != nil; a = Cell::cdr(a)) {
      elt = ctx->make(car(a));
      alist.append(elt);
      ctx->gc_unprotect();
      ctx->gc_protect(alist.head());
    }
    arglist = cdr(arglist);
  }

  alist.append(car(arglist));
  ctx->gc_unprotect();

  return alist.head();
}

// Destructive concatenation.  Lists are spliced together and
// will arguments will share structure.  When it is usable, it
// is faster than append, which must clone all its arguments.

Cell *nconc(Context *ctx, Cell *arglist) {
  Cell::List alist;

  // For each argument list:  If this is the first
  // list, install it in alist.  Otherwise, splice
  // it to the tail of alist, by updating pointers.
  // Do not cons anything.

  if (arglist == nil)
    return nil;

  while (cdr(arglist) != nil) {
    Cell *list_head = car(arglist);
    if (list_head != nil) {
      Cell *list_tail = list_head;
      while (cdr(list_tail) != nil)
        list_tail = cdr(list_tail);

      alist.append_list(list_head, list_tail);
    }
    arglist = cdr(arglist);
  }

  alist.append(car(arglist));

  return alist.head();
}

static Cell *member_helper(Context *ctx, Cell *arglist,
                           bool (Cell::*equality)(Cell *)) {
  Cell *target = car(arglist);
  Cell *list = cadr(arglist);

  for (Cell *l = list; l != nil; l = Cell::cdr(l))
    if ((target->*equality)(Cell::car(l)))
      return l;

  return ctx->make_boolean(false);
}

Cell *memq(Context *ctx, Cell *arglist) {
  return member_helper(ctx, arglist, &Cell::eq);
}

Cell *memv(Context *ctx, Cell *arglist) {
  return member_helper(ctx, arglist, &Cell::eqv);
}

Cell *member(Context *ctx, Cell *arglist) {
  return member_helper(ctx, arglist, &Cell::equal);
}

static Cell *assoc_helper(Context *ctx, Cell *arglist,
                          bool (Cell::*equality)(Cell *)) {
  Cell *target = car(arglist);
  Cell *list = cadr(arglist);

  for (Cell *l = list; l != nil; l = Cell::cdr(l))
    if ((target->*equality)(Cell::caar(l)))
      return Cell::car(l);

  return ctx->make_boolean(false);
}

Cell *assq(Context *ctx, Cell *arglist) {
  return assoc_helper(ctx, arglist, &Cell::eq);
}

Cell *assv(Context *ctx, Cell *arglist) {
  return assoc_helper(ctx, arglist, &Cell::eqv);
}

Cell *assoc(Context *ctx, Cell *arglist) {
  return assoc_helper(ctx, arglist, &Cell::equal);
}

Cell *symbol_to_string(Context *ctx, Cell *arglist) {
  return ctx->make_string(car(arglist)->SymbolValue()->key);
}

Cell *string_to_symbol(Context *ctx, Cell *arglist) {
  return ctx->make_symbol(intern_stet(car(arglist)->StringValue()));
}

Cell *string_to_number(Context *ctx, Cell *arglist) {
  const std::string &s = car(arglist)->StringValue();
  char *t;
  int base = 0;

  if (s.empty())
    return ctx->make_boolean(false);

  // The standard requires that "." produce #f.  On VxWorks,
  // strtod would give "0.0", so we must treat "." as a special
  // case.

  if (s == ".")
    return ctx->make_boolean(false);

  if (cdr(arglist) != nil)
    base = cadr(arglist)->IntValue();

  errno = 0;
  int i = strtol(s.c_str(), &t, base);

  if (*t != '\0' || errno == ERANGE) {
    // It didn't work as an integer, but it might
    // be floating point.

    if (base == 0) {
      double d = strtod(s.c_str(), &t);
      if (*t == '\0')
        return ctx->make_real(d);
    }

    // Scheme considers it an error if we don't consume
    // the whole string in the conversion.

    return ctx->make_boolean(false);
  }

  return ctx->make_int(i);
}

Cell *string_chars(Context *ctx, Cell *arglist) {
  std::string s;
  for (Cell *chptr = arglist; chptr != nil; chptr = Cell::cdr(chptr))
    s.push_back(car(chptr)->CharValue());
  return ctx->make_string(std::move(s));
}

Cell *list_to_string(Context *ctx, Cell *arglist) {
  return string_chars(ctx, car(arglist));
}

Cell *list_to_vector(Context *ctx, Cell *arglist) {
  return vector_from_list(ctx, car(arglist));
}

Cell *string_set(Context *ctx, Cell *arglist) {
  Cell *pstr = car(arglist);
  std::string &s = pstr->mutable_string();
  size_t ix = cadr(arglist)->IntValue();

  if (ix >= s.length())
    error("string index out of bounds");

  char ch = caddr(arglist)->CharValue();
  s[ix] = ch;
  return unspecified;
}

Cell *string_copy(Context *ctx, Cell *arglist) {
  return ctx->make_string(car(arglist)->StringValue());
}

Cell *string_fill(Context *ctx, Cell *arglist) {
  Cell *pstr = car(arglist);
  std::string &s = pstr->mutable_string();
  char ch = cadr(arglist)->CharValue();

  s.assign(s.length(), ch);
  return unspecified;
}

Cell *string_append(Context *ctx, Cell *arglist) {
  std::string s;
  for (Cell *pstr = arglist; pstr != nil; pstr = Cell::cdr(pstr))
    s.append(car(pstr)->StringValue());
  return ctx->make_string(std::move(s));
}

Cell *substring(Context *ctx, Cell *arglist) {
  Cell *pstr = car(arglist);
  const std::string &s = pstr->StringValue();
  int n = static_cast<int>(s.length());
  int ix = cadr(arglist)->IntValue();
  int iy = caddr(arglist)->IntValue();

  if (ix < 0 || iy < ix || n < iy)
    error("string index out of bounds");

  return ctx->make_string(s.substr(ix, iy - ix));
}

Cell *char_upcase(Context *ctx, Cell *arglist) {
  return ctx->make_char(toupper(car(arglist)->CharValue()));
}

Cell *char_downcase(Context *ctx, Cell *arglist) {
  return ctx->make_char(tolower(car(arglist)->CharValue()));
}

Cell *set_cdr(Context *ctx, Cell *arglist) {
  Cell::setcdr(car(arglist), cadr(arglist));
  return unspecified;
}

Cell *set_car(Context *ctx, Cell *arglist) {
  Cell::setcar(car(arglist), cadr(arglist));
  return unspecified;
}

Cell *current_input_port(Context *ctx, Cell *arglist) {
  return ctx->current_input();
}

Cell *current_output_port(Context *ctx, Cell *arglist) {
  return ctx->current_output();
}

Cell *close_input_port(Context *ctx, Cell *arglist) {
  // car (arglist)->IportValue ().close ();
  return unspecified;
}

Cell *close_output_port(Context *ctx, Cell *arglist) {
  fflush(car(arglist)->OportValue());
  return unspecified;
}

Cell *integer_to_char(Context *ctx, Cell *arglist) {
  return ctx->make_char(car(arglist)->IntValue() & 255);
}

Cell *char_to_integer(Context *ctx, Cell *arglist) {
  return ctx->make_int(static_cast<intptr_t>(car(arglist)->CharValue()));
}

Cell *open_input_file(Context *ctx, Cell *arglist) {
  return ctx->make_iport(car(arglist)->StringValue());
}

Cell *open_output_file(Context *ctx, Cell *arglist) {
  return ctx->make_oport(car(arglist)->StringValue());
}

Cell *skread(Context *ctx, Cell *arglist) {
  Cell *r_nu = ctx->read(iport(ctx, arglist));
  return r_nu == 0 ? &Cell::Eof_Object : r_nu;
}

Cell *read_char(Context *ctx, Cell *arglist) {
  char ch;
  FILE *in = iport(ctx, arglist);

  ch = fgetc(in);

  if (feof(in))
    return &Cell::Eof_Object;

  return ctx->make_char(ch);
}

Cell *peek_char(Context *ctx, Cell *arglist) {
  FILE *in = iport(ctx, arglist);
  int ch = fgetc(in);
  ungetc(ch, in);
  return (ch == -1 ? &Cell::Eof_Object : ctx->make_char(ch));
}

Cell *eof_object_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(car(arglist) == &Cell::Eof_Object);
}

Cell *reverse(Context *ctx, Cell *arglist) {
  Cell *rlist = nil;

  ctx->gc_protect(rlist);
  for (Cell *elt = car(arglist); elt != nil; elt = Cell::cdr(elt)) {
    rlist = ctx->cons(car(elt), rlist);
    ctx->gc_unprotect();
    ctx->gc_protect(rlist);
  }
  ctx->gc_unprotect();

  return rlist;
}

Cell *exact_to_inexact(Context *ctx, Cell *arglist) {
  Cell *a = car(arglist);
  if (a->is<intptr_t>())
    return ctx->make_real((double)a->IntValue());
  return ctx->make_real(a->RealValue());
}

Cell *inexact_to_exact(Context *ctx, Cell *arglist) {
  Cell *a = car(arglist);
  if (a->is<intptr_t>())
    return ctx->make_int(a->IntValue());
  return ctx->make_int(static_cast<intptr_t>(a->RealValue()));
}

// Round to nearest int... which would be easy except that the Scheme
// standard insists that we "round toward even" when the fractional
// part is 0.5!  If it weren't for that, we could get away with
// floor(d+0.5).  As it is we're left with lots of cases.  This horrible
// if/else nest tries to get the job done quickly.

double _round(double d) {
  double frac_part, int_part;
  frac_part = modf(d, &int_part);
  if (frac_part == 0.0)
    return d;
  if (frac_part > 0.0)
    if (frac_part > 0.5)
      return int_part + 1.0;
    else if (frac_part == 0.5)
      if (fmod(int_part, 2.0) != 0)
        return int_part + 1.0;
      else
        return int_part;
    else
      return int_part;
  else // frac_part < 0.0
    if (frac_part < -0.5)
      return int_part - 1.0;
    else if (frac_part == -0.5)
      if (fmod(int_part, 2.0) != 0)
        return int_part - 1.0;
      else
        return int_part;
    else
      return int_part;
}

// Trunc: not ANSI, so rather than #ifdef it we just provide a
// version here that works.

double sktrunc(double d) {
  double int_part;
  modf(d, &int_part);
  return int_part;
}

// REAL_F1 and REAL_F2 are `impedance matching' macros that expose
// a C-library transcendental math function (like sin, cos) to
// scheme. F1 is for one-argument functions, F2 for two arguments.
// The subr-function name chosen is made different from the C
// library function to avoid name collisions.

template <double (*Func)(double)>
static Cell *real_f1(Context *ctx, Cell *arglist) {
  return ctx->make_real(Func(asReal(car(arglist))));
}

template <double (*Func)(double, double)>
static Cell *real_f2(Context *ctx, Cell *arglist) {
  return ctx->make_real(Func(asReal(car(arglist)), asReal(cadr(arglist))));
}

static constexpr subr_f skround = real_f1<_round>;
static constexpr subr_f sklog = real_f1<log>;
static constexpr subr_f sksqrt = real_f1<sqrt>;
static constexpr subr_f skexp = real_f1<exp>;
static constexpr subr_f sksin = real_f1<sin>;
static constexpr subr_f skcos = real_f1<cos>;
static constexpr subr_f sktan = real_f1<tan>;
static constexpr subr_f skasin = real_f1<asin>;
static constexpr subr_f skacos = real_f1<acos>;
static constexpr subr_f inexact_expt = real_f2<pow>;
static constexpr subr_f skfloor = real_f1<floor>;
static constexpr subr_f skceiling = real_f1<ceil>;
static constexpr subr_f sktruncate = real_f1<sktrunc>;

static Cell *expt(Context *ctx, Cell *arglist) {
  // Scheme requires expt to return an exact result, if
  // representible, when given exact arguments.  XXX:
  // we should detect overflow, and delegate to the
  // inexact version in that event.

  if (exact_list(arglist)) {
    // This is Knuth's Algorithm 4.6.3A (TAoCP 3ed. vol II p. 462).
    // The variable names and labels are as in Knuth's presentation;
    // the interested reader is referred there.

    // A1:

    int Z = car(arglist)->IntValue();
    int N = cadr(arglist)->IntValue();
    int Y = 1;
    int even;

    // handle Scheme's requirement that (expt 0 N) = 1
    // if N = 0 and 0 otherwise.  Also, handle the
    // trivial Z = 1 case.  If N < 0, that's inexact.

    if (Z == 0)
      return ctx->make_int(N == 0 ? 1 : 0);
    if (Z == 1)
      return ctx->make_int(1);
    if (N == 0)
      return ctx->make_int(0);
    if (N < 0)
      return inexact_expt(ctx, arglist);

  A2:
    even = !(N & 1);
    N >>= 1;
    if (even)
      goto A5;
    // A3:
    Y = Z * Y;
    // A4:
    if (N == 0)
      return ctx->make_int(Y);
  A5:
    Z = Z * Z;
    goto A2;
  } else
    return inexact_expt(ctx, arglist);
}

static Cell *skatan(Context *ctx, Cell *arglist) {
  // If one arg, then compute atan(x), else compute atan2(y,x).

  double x = asReal(car(arglist));

  if (cdr(arglist) != nil) {
    double y = asReal(cadr(arglist));
    return ctx->make_real(atan2(y, x));
  }

  return ctx->make_real(atan(x));
}

static Cell *logand(Context *ctx, Cell *arglist) {
  int value = ~0;

  for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
    value &= car(a)->IntValue();

  return ctx->make_int(value);
}

static Cell *logbit_p(Context *ctx, Cell *arglist) {
  return ctx->make_boolean(
      (cadr(arglist)->IntValue() & (1 << car(arglist)->IntValue())) != 0);
}

static Cell *logior(Context *ctx, Cell *arglist) {
  int value = 0;

  for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
    value |= car(a)->IntValue();

  return ctx->make_int(value);
}

static Cell *logxor(Context *ctx, Cell *arglist) {
  int value = 0;

  for (Cell *a = arglist; a != nil; a = Cell::cdr(a))
    value ^= car(a)->IntValue();

  return ctx->make_int(value);
}

static Cell *lognot(Context *ctx, Cell *arglist) {
  return ctx->make_int(~car(arglist)->IntValue());
}

static Cell *skerror(Context *ctx, Cell *arglist) {
  // Accumulate the arguments as though they were being
  // displayed

  error(car(arglist)->StringValue());
  return unimplemented; // satisfy compiler
}

static Cell *skgc(Context *ctx, Cell *arglist) {
  ctx->gc();
  return unspecified;
}

static Cell *sk_impl_type(Context *ctx, Cell *arglist) {
  return ctx->make_symbol(intern("vx-scheme"));
}

static Cell *vxs_impl_type(Context *ctx, Cell *arglist) {
  static psymbol const i_interp = intern("interp");
  static psymbol const i_vm = intern("vm");
  return ctx->make_symbol(ctx->using_vm() ? i_vm : i_interp);
}

#define __string(x) #x
#define __vstring(v) ("vx-scheme " __string(v))
#define VERSION_STRING __vstring(VERSION)

static Cell *sk_impl_ver(Context *ctx, Cell *arglist) {
  return ctx->make_string(VERSION_STRING);
}

static Cell *sk_impl_page(Context *ctx, Cell *arglist) {
  return ctx->make_string("http://colin-smith.net/vx-scheme/");
}

static Cell *sk_impl_platform(Context *ctx, Cell *arglist) {
  psymbol s;
#if defined(__CYGWIN__)
  s = intern("cygwin");
#elif defined(__unix__)
  s = intern("unix");
#else
  s = intern("unknown");
#endif
  return ctx->make_symbol(s);
}

static Cell *file_exists_p(Context *ctx, Cell *arglist) {
  FILE *ip = fopen(car(arglist)->StringValue().c_str(), "r");
  if (ip != NULL)
    fclose(ip);
  return ctx->make_boolean(ip != NULL);
}

//
// PROPERTY LIST SUPPORT
//

static Cell *put_property(Context *ctx, Cell *arglist) {
  psymbol p = car(arglist)->SymbolValue();
  psymbol q = cadr(arglist)->SymbolValue();
  Cell *value = caddr(arglist);

  if (p->plist)
    for (int ix = 0; ix < p->plist->size(); ++ix) {
      Cell *prop = p->plist->get(ix);
      if (car(prop)->SymbolValue() == q) {
        Cell::setcdr(prop, value); // hit: plist already contains q.
        return unspecified;
      }
    }
  else
    // time to add the plist.
    p->plist = cellvector::alloc(0);

  // miss: add a new property.  Create the plist if necessary.

  Cell *assoc = ctx->cons(cadr(arglist), value);
  p->plist->push(assoc);
  return unspecified;
}

static Cell *get_property(Context *ctx, Cell *arglist) {
  psymbol const p = car(arglist)->SymbolValue();
  psymbol const q = cadr(arglist)->SymbolValue();

  if (p->plist)
    for (int ix = 0; ix < p->plist->size(); ++ix) {
      Cell *elt = p->plist->get(ix);
      if (car(elt)->SymbolValue() == q)
        return cdr(elt);
    }

  return ctx->make_boolean(false);
}

// Imported from Common Lisp.  Returns #t if the given symbol is
// bound in the global environment (lexical bindings are not consulted),
// #f otherwise.

Cell *bound_p(Context *ctx, Cell *arglist) {
  psymbol s = car(arglist)->SymbolValue();
  return ctx->make_boolean(ctx->find_var(ctx->root(), s, 0) != NULL);
}

// Imported from Common Lisp.  Retrieves the value of a symbol in the
// global environment (not in any lexical binding).  Errors if the
// symbol is unbound there.

Cell *symbol_value(Context *ctx, Cell *arglist) {
  psymbol s = car(arglist)->SymbolValue();
  Cell *value = ctx->find_var(ctx->root(), s, 0);
  if (!value) {
    error("unbound symbol");
    return unspecified;
  }
  return cdr(value);
}

// Get/Set current working directory

static Cell *sk_getcwd(Context *ctx, Cell *arglist) {
  char buf[PATH_MAX];
  getcwd(buf, sizeof(buf));
  return ctx->make_string(buf);
}

static Cell *sk_chdir(Context *ctx, Cell *arglist) {
  const std::string &dir = car(arglist)->StringValue();
  bool ok = chdir(dir.c_str()) == 0;
  return ctx->make_boolean(ok);
}

//
// INITIALIZATION
//

void Context::provision() {
  struct {
    const char *n;
    subr_f i;
  } subr[] = {
      {"*", times},
      {"+", skplus},
      {"-", skminus},
      {"/", divide},
      {"<", lt},
      {"<=", le},
      {"=", number_equal},
      {">", gt},
      {">=", ge},
      {"abs", skabs},
      {"append", append},
      {"acos", skacos},
      {"asin", skasin},
      {"assoc", assoc},
      {"assq", assq},
      {"assv", assv},
      {"atan", skatan},
      {"boolean?", boolean_p},
      {"caaaar", cons_accessor<Cell::caaaar>},
      {"caaadr", cons_accessor<Cell::caaadr>},
      {"caaar", cons_accessor<Cell::caaar>},
      {"caadar", cons_accessor<Cell::caadar>},
      {"caaddr", cons_accessor<Cell::caaddr>},
      {"caadr", cons_accessor<Cell::caadr>},
      {"caar", cons_accessor<Cell::caar>},
      {"cadaar", cons_accessor<Cell::cadaar>},
      {"cadadr", cons_accessor<Cell::cadadr>},
      {"cadar", cons_accessor<Cell::cadar>},
      {"caddar", cons_accessor<Cell::caddar>},
      {"cadddr", cons_accessor<Cell::cadddr>},
      {"caddr", cons_accessor<Cell::caddr>},
      {"cadr", cons_accessor<Cell::cadr>},
      {"car", cons_accessor<Cell::car>},
      {"cdaaar", cons_accessor<Cell::cdaaar>},
      {"cdaadr", cons_accessor<Cell::cdaadr>},
      {"cdaar", cons_accessor<Cell::cdaar>},
      {"cdadar", cons_accessor<Cell::cdadar>},
      {"cdaddr", cons_accessor<Cell::cdaddr>},
      {"cdadr", cons_accessor<Cell::cdadr>},
      {"cdar", cons_accessor<Cell::cdar>},
      {"cddaar", cons_accessor<Cell::cddaar>},
      {"cddadr", cons_accessor<Cell::cddadr>},
      {"cddar", cons_accessor<Cell::cddar>},
      {"cdddar", cons_accessor<Cell::cdddar>},
      {"cddddr", cons_accessor<Cell::cddddr>},
      {"cdddr", cons_accessor<Cell::cdddr>},
      {"cddr", cons_accessor<Cell::cddr>},
      {"cdr", cons_accessor<Cell::cdr>},
      {"ceiling", skceiling},
      {"char->integer", char_to_integer},
      {"char-alphabetic?", alphabetic_p},
      {"char-ci<=?", char_le_ci},
      {"char-ci<?", char_lt_ci},
      {"char-ci=?", char_eq_ci},
      {"char-ci>=?", char_ge_ci},
      {"char-ci>?", char_gt_ci},
      {"char-downcase", char_downcase},
      {"char-lower-case?", lower_case_p},
      {"char-numeric?", numeric_p},
      {"char-upcase", char_upcase},
      {"char-upper-case?", upper_case_p},
      {"char-whitespace?", whitespace_p},
      {"char<=?", char_le},
      {"char<?", char_lt},
      {"char=?", char_eq},
      {"char>=?", char_ge},
      {"char>?", char_gt},
      {"char?", char_p},
      {"close-input-port", close_input_port},
      {"close-output-port", close_output_port},
      {"complex?", complex_p},
      {"cons", skcons},
      {"cos", skcos},
      {"current-input-port", current_input_port},
      {"current-output-port", current_output_port},
      {"display", display},
      {"eof-object?", eof_object_p},
      {"error", skerror},
      {"eq?", eq},
      {"equal?", equal_p},
      {"eqv?", eqv},
      {"even?", even_p},
      {"exact?", exact_p},
      {"exact->inexact", exact_to_inexact},
      {"exp", skexp},
      {"expt", expt},
      {"floor", skfloor},
      {"inexact->exact", inexact_to_exact},
      {"gcd", gcd},
      {"inexact?", inexact_p},
      {"input-port?", input_p},
      {"integer->char", integer_to_char},
      {"integer?", integer_p},
      {"lcm", lcm},
      {"length", length},
      {"list", sklist},
      {"list->string", list_to_string},
      {"list->vector", list_to_vector},
      {"list-ref", list_ref},
      {"list?", list_p},
      {"log", sklog},
      {"logand", logand},
      {"logbit?", logbit_p},
      {"logior", logior},
      {"lognot", lognot},
      {"logxor", logxor},
      {"make-string", skmake_string},
      {"make-vector", skmake_vector},
      {"max", skmax},
      {"member", member},
      {"memq", memq},
      {"memv", memv},
      {"min", skmin},
      {"modulo", modulo},
      {"negative?", negative_p},
      {"newline", newline},
      {"not", sknot},
      {"null?", null_p},
      {"number->string", number_to_string},
      {"number?", number_p},
      {"odd?", odd_p},
      {"open-input-file", open_input_file},
      {"open-output-file", open_output_file},
      {"output-port?", output_p},
      {"pair?", pair_p},
      {"peek-char", peek_char},
      {"positive?", positive_p},
      {"procedure?", procedure_p},
      {"quotient", quotient},
      {"rational?", rational_p},
      {"read", skread},
      {"read-char", read_char},
      {"real?", real_p},
      {"remainder", remainder},
      {"reverse", reverse},
      {"round", skround},
      {"set-car!", set_car},
      {"set-cdr!", set_cdr},
      {"sin", sksin},
      {"sqrt", sksqrt},
      {"string", string_chars},
      {"string-copy", string_copy},  // R5
      {"string-fill!", string_fill}, // R5
      {"string->list", string_to_list},
      {"string->number", string_to_number},
      {"string->symbol", string_to_symbol},
      {"string-append", string_append},
      {"string-ci<=?", string_le_ci},
      {"string-ci<?", string_lt_ci},
      {"string-ci=?", string_eq_ci},
      {"string-ci>=?", string_ge_ci},
      {"string-ci>?", string_gt_ci},
      {"string-length", string_length},
      {"string-ref", string_ref},
      {"string-set!", string_set},
      {"string<=?", string_le},
      {"string<?", string_lt},
      {"string=?", string_eq},
      {"string>=?", string_ge},
      {"string>?", string_gt},
      {"string?", string_p},
      {"substring", substring},
      {"symbol->string", symbol_to_string},
      {"symbol?", symbol_p},
      {"tan", sktan},
      {"truncate", sktruncate},
      {"vector", vector_from_list},
      {"vector->list", vector_to_list},
      {"vector-fill!", vector_fill}, // R5
      {"vector-length", vector_length},
      {"vector-ref", vector_ref},
      {"vector-set!", vector_set},
      {"vector?", vector_p},
      {"write", write},
      {"write-char", write_char},
      {"zero?", zero_p},
      //----------------------------------------------------------------
      //
      // The following functions are not part of the spec, but
      // are peculiar to this implementation.
      //
      {"bound?", bound_p},
      {"chdir", sk_chdir},
      {"display*", display_star},
      {"put", put_property},
      {"get", get_property},
      {"file-exists?", file_exists_p},
      {"gc", skgc},
      {"getcwd", sk_getcwd},
      {"nconc", nconc},
      {"primitive-procedure?", primitive_procedure_p},
      {"scheme-implementation-type", sk_impl_type},
      {"vx-scheme-implementation-type", vxs_impl_type},
      {"scheme-implementation-version", sk_impl_ver},
      {"scheme-implementation-home-page", sk_impl_page},
      {"scheme-implementation-platform", sk_impl_platform},
      {"symbol-value", symbol_value},
      {"vector-push!", vector_push},
      {"vector-pop!", vector_pop},
      {"vector-unshift!", vector_unshift},
      {"vector-shift!", vector_shift},
      //
      //----------------------------------------------------------------
  };

  for (unsigned int ix = 0; ix < sizeof(subr) / sizeof(*subr); ++ix)
    bind_subr(subr[ix].n, subr[ix].i);

  // Source code in SICP uses the symbols `true' and `false' for
  // boolean values instead of #t and #f as suggested by RxRS.
  // We add these symbol-bindings here.

  set_var(envt, intern("true"), make_boolean(true));
  set_var(envt, intern("false"), make_boolean(false));
  set_var(envt, intern("*version*"), make_string(VERSION_STRING));

  // Load extension bindings.

  SchemeExtension::RunInstall(this, envt);
}

void Context::bind_subr(const char *name, subr_f subr) {
  psymbol s = intern(name);
  set_var(envt, s, make_subr(subr, name));
}

cellvector *SchemeExtension::extensions = 0;
SchemeExtension *SchemeExtension::main = 0;

void SchemeExtension::Register(SchemeExtension *ext) {
  if (!extensions)
    extensions = new cellvector();

  extensions->push(reinterpret_cast<Cell *>(ext));
}

void SchemeExtension::RunInstall(Context *ctx, Cell *envt) {
  if (!extensions)
    return;
  for (int ix = 0; ix < extensions->size(); ++ix) {
    SchemeExtension *extension =
        reinterpret_cast<SchemeExtension *>(extensions->get(ix));
    extension->Install(ctx, envt);
  }
}
