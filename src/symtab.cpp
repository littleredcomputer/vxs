//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// symtab.cpp : symbol table with copied strings in an AVL tree.

#include <stddef.h>
#include "vx-scheme.h"

static inline char *string_dup(const char *s) {
  char *x = (char *)malloc(strlen(s) + 1);
  strcpy(x, s);
  return x;
}

static psymbol newnode(const char *key) {
  psymbol n = (psymbol)malloc(sizeof(symbol));
  memset(n, 0, sizeof(symbol));
  n->key = string_dup(key);
  return n;
}

// This is unguarded global data.  Symbols stored in this tree
// could be shared among multiple threads, though, so it would
// be easy to protect this table with a mutex.

static symbol _head = {0, 0, 0, 0};
static psymbol head = &_head;
static psymbol symtab_insert(const char *);

// intern: place the string in the symbol table in standard case.

psymbol intern(const char *name) {
  sstring ss;
  size_t l;
  psymbol q;

  ss.append(name);
  l = ss.length();

  for (size_t ix = 0; ix < l; ++ix)
    ss[ix] = tolower(ss[ix]);

  if (strcmp(ss.str(), name)) {
    // Name was not given in standard case!  We store it both as
    // it was given and in standard case.

    q = intern_stet(ss.str());
    q->truename = string_dup(name);
    return q;
  }

  q = intern_stet(name);
  q->truename = q->key;
  return q;
}

psymbol intern(const std::string &name) { return intern(name.c_str()); }

// intern_stet: place the string in the symbol table exactly as given.

psymbol intern_stet(const char *name) { return symtab_insert(name); }
psymbol intern_stet(const std::string &name) { return symtab_insert(name.c_str()); }

// An implementation of Knuth's Algorithm 6.2.3A "Balanced Tree Search
// and Insertion," from [TAoCP (3ed.) vol III p.462].  The insert
// function follows the structure of Knuth's algorithm closely, even
// using the same variable names and labels he chooses.  Hence, we
// refer the reader his book for further documentation of this
// routine.

static psymbol symtab_insert(const char *K) {
  psymbol P, Q, R, S, T;
  int c, a;

  if (!head->rlink) {
    return (head->rlink = newnode(K));
  }

  // A1:                                   /* Initialize. */
  T = head;
  S = P = head->rlink;

A2:
  c = strcmp(K, P->key); /* Compare. */
  if (c < 0)
    goto A3;
  if (c > 0)
    goto A4;
  return P;

A3:
  Q = P->llink; /* Move left. */
  if (Q == 0) {
    Q = newnode(K);
    P->llink = Q;
    goto A5;
  }

proceed:
  if (Q->b != 0) {
    T = P;
    S = Q;
  }

  P = Q;
  goto A2;

A4:
  Q = P->rlink; /* Move right. */
  if (Q == 0) {
    Q = newnode(K);
    P->rlink = Q;
    goto A5;
  }

  goto proceed;

A5: /* The "Insert" step is handled in the newnode function. */

  // A6:   /* Adjust balance factors. */

  a = (strcmp(K, S->key) < 0) ? -1 : +1;
  R = P = (a < 0 ? S->llink : S->rlink);
  while (P != Q) {
    c = strcmp(K, P->key);
    if (c < 0) {
      P->b = -1;
      P = P->llink;
    } else if (c > 0) {
      P->b = +1;
      P = P->rlink;
    }
  }

  // A7:
  if (S->b == 0) /* Balancing act. */
  {
    S->b = a;
    ++head->b; /* Keep track of tree height: */
    return Q;  /* Knuth uses LLINK but we use B. */
  }

  if (S->b == -a) {
    S->b = 0;
    return Q;
  }

  if (S->b == a) {
    if (R->b == a)
      goto A8;
    else if (R->b == -a)
      goto A9;
  }

A8:
  P = R; /* Single rotation. */
  if (a < 0) {
    S->llink = R->rlink;
    R->rlink = S;
  } else {
    S->rlink = R->llink;
    R->llink = S;
  }
  S->b = R->b = 0;
  goto A10;

A9:
  if (a < 0) /* Double rotation. */
  {
    P = R->rlink;
    R->rlink = P->llink;
    P->llink = R;
    S->llink = P->rlink;
    P->rlink = S;
  } else {
    P = R->llink;
    R->llink = P->rlink;
    P->rlink = R;
    S->rlink = P->llink;
    P->llink = S;
  }

  if (P->b == a) {
    S->b = -a;
    R->b = 0;
  } else if (P->b == -a) {
    S->b = 0;
    R->b = a;
  } else {
    S->b = R->b = 0;
  }

  P->b = 0;

A10:
  if (S == T->rlink) /* Finishing touch. */
    T->rlink = P;
  else
    T->llink = P;

  return Q;
}

// sstring class
//
// simple, extensible string.  Tries to work efficiently for small
// strings by using a static buffer, which "spills" into a region on
// the heap if necessary.  Aims for compactness.  We maintain
// null-termination at all times.  A "claim" operation is supported,
// which means that malloc'd string storage won't be freed when the
// sstring is destructed.  This can help avoid excess strdup's for the
// consumers of strings created this way.  STL strings are fine, but
// their template-based implementation leads to "bloat" (in the
// context of an application like this which is aiming for embedded
// compactness).
//
// ** We maintain null termination at all times.

sstring::sstring() {
  // Initially, we use our static buffer, abandoning it for storage
  // obtained with malloc if we need to.

  sz = 0;
  base = c; // base points to base of allocation.
  *base = '\0';
  alloc = stat_size; // How much allocated (statically or otherwise).
  end = base;        // End will point to the first free character.
  pos = base;        // read position
  claimed = false;   // freeing the storage is our job
}

sstring::~sstring() {
  // If we've spilled, and the caller hasn't claimed the buffer,
  // free it.
  if (base != c && !claimed)
    free(base);
}

void sstring::append(const char ch) { append(&ch, 1); }

void sstring::append(const char *s) { append(s, strlen(s)); }

void sstring::append(const char *s, size_t len) {
  size_t required = sz + len + 1;
  ptrdiff_t read_offset = pos - base;

// Will it fit in the current allocation?

TOP:
  if (required < alloc) {
    memcpy(end, s, len);
    end += len;
    sz += len;
    end[0] = '\0';
    return;
  }

  // No.  We will have to allocate more storage (perhaps for
  // the first time).  Double the size (unless that won't be
  // enough to accept this append.

  size_t new_alloc = 2 * alloc;
  if (required > new_alloc)
    new_alloc = required;

  char *new_buf = (char *)malloc(new_alloc);
  if (!new_buf)
    error("out of memory");

  memcpy(new_buf, base, sz);

  if (base != c)
    free(base);

  base = new_buf;
  end = base + sz;
  end[0] = '\0';
  alloc = new_alloc;
  pos = base + read_offset;

  // It should work now.
  goto TOP;
}

void sstring::claim() {
  if (base == c) {
    // We were getting away with using the little static buffer,
    // but now the user wants to hand off the buffer as if it were
    // malloc'd.  We have to clone it now.
    base = (char *)malloc(sz + 1);
    strcpy(base, c);
  }

  claimed = true;
}

int sstring::get() {
  if (pos >= base && pos < end)
    return *pos++;
  return EOF;
}

int sstring::peek() {
  if (pos >= base && pos < end)
    return *pos;
  return EOF;
}

void sstring::unget() {
  if (pos > base)
    --pos;
}

void sstring::ignore() {
  if (pos < end)
    ++pos;
}

#ifdef TEST

// Unit test: with TEST defined, this module becomes a standalone
// program which will sort its input. It prints the height of the tree
// at the end, which ought to be about log2 (number of input records).

void print(psymbol n) {
  // traverse the tree inorder, and print.

  if (!n)
    return;

  print(n->llink);
  printf("%s\n", n->key);
  print(n->rlink);
}

int main() {
  char buf[80];
  int i = 1;
  psymbol Q;

  while (fgets(buf, sizeof(buf), stdin)) {
    int l = strlen(buf);
    if (buf[l - 1] == '\n')
      buf[l - 1] = '\0'; /* chomp newline */

    Q = intern_stet(buf);
  }

  print(head->rlink);
  printf("height: %d\n", head->b);
  return 0;
}

void error(const char *s1, const char *s2) {
  fputs(s1, stderr);
  fputs(s2, stderr);
  exit(1);
}

#endif
