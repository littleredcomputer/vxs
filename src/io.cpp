//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// io.cpp : reading and printing S-expressions.

#include "vx-scheme.h"
#include <errno.h>

// --------------------------------------------------------------------------
// token - return the next token (sequence of characters until delimiter).
// the delimiter is left on the stream.
//

void token(sio &in, sstring &ss) {
  static const char *delim = "\t\n\r) ";
  int c;
TOP:
  if ((c = in.get()) < 0)
    return;

  if (strchr(delim, c)) {
    in.unget();
    return;
  }

  ss.append(c);

  if (c == '\\')
    ss.append(in.get());
  goto TOP;
}

#define READ_RETURN(value)                                                     \
  do {                                                                         \
    retval = value;                                                            \
    goto FINISH;                                                               \
  } while (0)

// --------------------------------------------------------------------------
// read: convert source text to internal form
//

Cell *Context::read(sio &in) {
  char c;
  Cell *retval = unimplemented;

  save(r_nu);
  save(r_tmp);

TOP:
  c = in.get();
  if (c == EOF)
    READ_RETURN(0);
  if (isspace(c))
    goto TOP;
  if (c == ';') {
    // ';' introduces a comment.  Text up to the next newline
    // is discarded, and the parser restarts at the top.
    while (c != '\n') {
      c = in.get();
      if (c == EOF)
        READ_RETURN(0);
    }
    goto TOP;
  }
  if (c == '(') {
    // '(' introduces a list.  We invoke the parser recursively,
    // accumulating elements until we see a matching ')'.
    // One wrinkle is improper lists, formed by placing a `.'
    // before the last element; this has the effect of placing
    // the tail element directly in the cdr instead of in the
    // car of a node pointed to by the cdr.  (In particular,
    // this allows the syntax `(a . b)' to produce a "raw
    // cons."

    clear(r_argl);
    int dotmode = 0;

  LISTLOOP:
    save(r_argl);
    r_nu = read(in);
    restore(r_argl);
    if (r_nu == NULL)
      READ_RETURN(Cell::car(&r_argl));
    if (dotmode == 1) {
      l_appendtail(r_argl, r_nu);
      dotmode = 2; // expecting: )
    } else if (r_nu->is_symbol(s_dot)) {
      dotmode = 1; // expecting: cdr
    } else if (dotmode == 2) {
      // Uh-oh: something came between `. cdr' and `)'
      error("bad . list syntax");
    } else
      l_append(r_argl, r_nu);

    goto LISTLOOP;
  } else if (c == ')') {
    READ_RETURN(0);
  } else if (c == '\'') {
    r_nu = read(in);
    if (r_nu) {
      r_nu = make(r_nu);
      r_tmp = make_symbol(s_quote);
      READ_RETURN(cons(r_tmp, r_nu));
    }

    error("unexpected eof");
  } else if (c == '`') {
    if ((r_nu = read(in)) != NULL) {
      r_tmp = make_symbol(s_quasiquote);
      r_nu = make(r_nu);
      READ_RETURN(cons(r_tmp, r_nu));
    }

    error("unexpected eof");
  } else if (c == ',') {
    psymbol wrap = s_unquote;

    if (in.peek() == '@') {
      in.ignore();
      wrap = s_unquote_splicing;
    }

    if ((r_nu = read(in)) != NULL) {
      r_nu = make(r_nu);
      r_tmp = make_symbol(wrap);
      READ_RETURN(cons(r_tmp, r_nu));
    }

    error("unexpected eof");
  } else if (c == '#') {
    // First we must treat the read-syntax for vectors #(...) .

    if (in.peek() == '(') {
      // Vector.
      int vl = 0;
      clear(r_argl);
      in.get(); // drop the '('. It's cleaner.
    VECLOOP:
      save(r_argl);
      r_nu = read(in);
      restore(r_argl);

      if (r_nu == NULL) {
        r_nu = make_vector(vl);
        cellvector *vec = r_nu->VectorValue();
        int ix = 0;
        FOR_EACH(elt, Cell::car(&r_argl))
        vec->set(ix++, Cell::car(elt));
        READ_RETURN(r_nu);
      }
      l_append(r_argl, r_nu);
      ++vl;
      goto VECLOOP;
    }

    sstring lexeme;
    token(in, lexeme);

    if (lexeme == "t")
      READ_RETURN(make_boolean(true));
    else if (lexeme == "f")
      READ_RETURN(make_boolean(false));
    else if (lexeme[0] == '\\') {
      // This is #\a syntax for characters.  But
      // we must also be careful to recognize
      // #\space and #\newline.

      if (lexeme == "\\newline")
        READ_RETURN(make_char('\n'));
      if (lexeme == "\\space" || lexeme == "\\Space")
        READ_RETURN(make_char(' '));
      if (lexeme.length() == 2)
        READ_RETURN(make_char(lexeme[1]));

      error("indecipherable #\\ constant: ", lexeme.str());
    } else if (lexeme[0] == 'x' || lexeme[0] == 'X') {
      // hex constant.  Drop the 'x' and convert with strtoul.

      char *endptr;
      uintptr_t ul = strtoul(lexeme.str() + 1, &endptr, 16);

      if (*endptr == '\0')
        READ_RETURN(make_int(ul));

      error("indecipherable #x constant");
    } else if (lexeme[0] == 'o' || lexeme[0] == 'O') {
      // octal constant.  Drop the 'o' and convert with stroul.

      char *endptr;
      unsigned long ul = strtoul(lexeme.str() + 1, &endptr, 8);

      if (*endptr == '\0')
        READ_RETURN(make_int(ul));
      error("indecipherable #o constant");
    }

    error("indecipherable #constant:", lexeme.str());
  } else if (c == '"') {
    bool quote = false;
    bool done = false;
    sstring ss;

    while (!done) {
      c = in.get();
      if (c == EOF)
        done = true;
      else {
        if (quote) {
          switch (c) {
          case 'r':
            ss.append('\r');
            break;
          case 'n':
            ss.append('\n');
            break;
          case 'a':
            ss.append('\a');
            break;
          case 't':
            ss.append('\t');
            break;
          // XXX deal with \octal, \hex for i18n
          default:
            ss.append(c);
          }
          quote = false;
        } else {
          if (c == '\\')
            quote = true;
          else if (c == '"')
            done = true;
          else
            ss.append(c);
        }
      }
    }

    READ_RETURN(make_string(ss.str()));
  } else {
    // At this point it is either a number or an identifier.
    // Scheme's syntax for identifiers is _very_ loose
    // (e.g., 3.14f is a perfectly good variable name.)
    // So we must be precise about what we accept as a number.
    // The following is a state machine meant to recognize
    // the following regular expression for a floating-point
    // or integer number (`2' stands for any decimal digit):
    //
    //   -?2*(.2*)?([Ee][+-]?2+)?
    //
    // State 0 is the initial state, and state X rejects
    // (i.e., classifies the lexeme as an identifier--there
    // may be more of it to read!).  States 3, 4, and 6 are
    // accepting.
    //
    //        CLASS
    // STATE   +/-   [0-9]    .     E/e    comment
    // -------------------------------------------------------------
    //   0      1      3      2      X     Initial state.
    //   1      X      3      2      X     Saw sign; read digits or .
    //   2      X      4      X      X     Saw .; read a digit
    //  (3)     X      3      4      5     Read digits, e, or '.'
    //  (4)     X      4      X      5     Have .; read digits or 'e'
    //   5      6      6      X      X     Have e, read a digit or sign
    //  (6)     X      6      X      X     Have e, read digits

    static const unsigned char tmatrix[][4] = {
        {1, 3, 2, 0}, {0, 3, 2, 0}, {0, 4, 0, 0}, {0, 3, 4, 5},
        {0, 4, 0, 5}, {6, 6, 0, 0}, {0, 6, 0, 0},
    };
    static const bool accept[] = {false, false, false, true, true, false, true};
    sstring lexeme;
    lexeme.append(c);
    token(in, lexeme);

    int state = 0;
    bool inexact = false;
    for (size_t ix = 0; ix < lexeme.length(); ++ix) {
      char lch = lexeme[ix];

      if (lch == '-' || lch == '+')
        state = tmatrix[state][0];
      else if (isdigit(lch))
        state = tmatrix[state][1];
      else if (lch == '.') {
        inexact = true;
        state = tmatrix[state][2];
      } else if (lch == 'e' || lch == 'E') {
        inexact = true;
        state = tmatrix[state][3];
      }
      if (state == 0)
        break;
    }
    // Did the state machine land in an accepting state?
    // if so, we have a number.
    if (accept[state]) {
      if (inexact) {
        READ_RETURN(make_real(strtod(lexeme.str(), 0)));
      } else {
        errno = 0;
        // TODO : fixme
        long l = strtol(lexeme.str(), 0, 0);
        if (errno == ERANGE)
          READ_RETURN(make_real(strtod(lexeme.str(), 0)));
        READ_RETURN(make_int(l));
      }
    }

    // If the machine lands in a non-accepting state,
    // then we have an identifier.

    READ_RETURN(make_symbol(intern(lexeme.str())));
  }

FINISH:

  restore(r_tmp);
  restore(r_nu);
  return retval;
}

Cell *Context::read(FILE *fp) {
  file_sio fsio(fp);
  return read(fsio);
}

void Cell::real_to_string(double d, char *buf, int nbytes) {
  snprintf(buf, sizeof(buf), "%.15g", d);

  // Now if buf contains neither a `.' nor an `e', then
  // the number was whole, and it won't "read back" as
  // a Real, as desired.  We tack on a decimal point in
  // that event.

  if (!strpbrk(buf, ".eE"))
    strcat(buf, ".");
}

void Cell::write(FILE *out) const {
  sstring output;
  write(output);
  fputs(output.str(), out);
}

void Cell::write(sstring &ss) const {
  if (this == &Nil)
    ss.append("()");
  else {
    Type t = type();
    switch (t) {
    case Int: {
      char buf[40];
      snprintf(buf, sizeof(buf), "%" PRIdPTR, IntValue());
      ss.append(buf);
      break;
    }
    case Symbol:
      ss.append(SymbolValue()->key);
      break;
    case Builtin:
      ss.append("#<builtin ");
      ss.append(BuiltinValue()->key);
      ss.append(">");
      break;
    case Char:
      ss.append("#\\");
      // XXX escaping?
      ss.append(CharValue());
      break;
    case Iport:
      ss.append("#<input-port>");
      break;
    case Oport:
      ss.append("#<output-port>");
      break;
    case Subr:
      ss.append("#<subr ");
      ss.append(SubrValue()->name);
      ss.append('>');
      break;
    case Cont:
      ss.append("#<continuation>");
      break;
    case Real: {
      char buf[80];
      real_to_string(RealValue(), buf, sizeof(buf));
      ss.append(buf);
      break;
    }
    case Unique:
      // "Unique" objects (like #t and EOF) keep their
      // printed representations in their cdrs.
      ss.append(cd.u);
      break;
    case Cons: {
      const Cell *d;
      ss.append('(');
      for (d = this; d->type() == Cons; d = cdr(d)) {
        if (d == nil) {
          ss.append(')');
          return;
        }
        car(d)->write(ss);
        if (cdr(d) != nil)
          ss.append(' ');
      }
      ss.append(". ");
      d->write(ss);
      ss.append(')');
      break;
    }
    case String: {
      char *p = StringValue();
      char ch;
      ss.append('"');
      while ((ch = *p++)) {
        if (ch == '"')
          ss.append("\\\"");
        else if (ch == '\\')
          ss.append("\\\\");
        else if (ch == '\n')
          ss.append("\\n");
        else
          ss.append(ch);
      }
      ss.append('"');
      break;
    }
    case Vec: {
      cellvector *v = VectorValue();
      ss.append("#(");
      for (int ix = 0; ix < v->size(); ++ix) {
        if (ix != 0)
          ss.append(' ');
        v->get(ix)->write(ss);
      }
      ss.append(')');
      break;
    }
    case Lambda: {
      Procedure proc = LambdaValue();
      ss.append(flag(MACRO) ? "#<macro " : "#<lambda ");
      if (debug_flag(DEBUG_PRINT_PROCEDURES)) {
        proc.arglist->write(ss);
        ss.append(' ');
        proc.body->write(ss);
        ss.append('>');
      } else {
        proc.arglist->write(ss);
        ss.append(" ...>");
      }
      break;
    }
    case Promise:
      ss.append("#<promise ");
      PromiseValue()->write(ss);
      ss.append('>');
      break;
    case Cproc:
      ss.append("#<compiled-procedure>");
      break;
    case Cpromise:
      if (flag(FORCED))
        CPromiseValue()->write(ss);
      else
        ss.append("#<compiled-promise>");
      break;
    case Insn:
      ss.append("#<vm-instruction>");
      break;
    default:
      ss.append("#<?>");
    }
  }
}

void Cell::display(FILE *out) {
  switch (type()) {
  case Char:
    fputc(CharValue(), out);
    break;
  case String:
    fputs(StringValue(), out);
    break;
  default:
    write(out);
  }
  fflush(out);
}

bool Context::read_eval_print(FILE *in, FILE *out, bool interactive) {
  Cell *result;
  Cell *expr;
  sstring text;
  file_sio sio(in);

  if (interactive) {
    fputs("=> ", out);
    fflush(out);
  }
  while ((expr = read(sio))) {
    // Don't bother printing the unspecified value as result.
    if ((result = eval(expr)) != unspecified) {
      result->write(out);
      fputc('\n', out);
      fflush(out);
    }
    gc_if_needed();
    return true;
  }
  return false;
}
