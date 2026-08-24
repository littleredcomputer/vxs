#pragma once

#include "vx_heap.h"
#include "vx_value.h"
#include "vx_vm.h"
#include <cctype>
#include <cerrno>
#include <climits>
#include <sstream>
#include <string_view>

namespace vxs {

// A malformed source text, reported rather than absorbed.
//
// It derives from std::runtime_error, so every existing catch site — the
// wasm eval entry points and main.cpp's top-level eval_string — already
// unwinds it correctly and the VM stays usable afterward. That last part
// is the whole point: a typo must cost you the form you typed, not the
// session you typed it into.
//
// Deliberately NOT RaiseEscape. That type is raise/guard's, and its
// payload lives in VM::in_flight_raises, which the reader has no business
// pushing to; a syntax fault is also not a condition a running program
// should be able to intercept, since by definition there is no program.
struct ReaderError : std::runtime_error {
  explicit ReaderError(const std::string &msg) : std::runtime_error(msg) {}
};

//=============================================================================
// Scheme S-Expression Reader / Lexer
//=============================================================================
class Reader {
public:
  Reader(VM &vm, std::string_view source) : vm(vm), src(source), cursor(0) {}

  // How many bytes of `source` this reader has consumed so far — lets a
  // caller that fed it more than one form's worth of text (e.g. `read`
  // slurping a whole port's remaining stream to parse just one form)
  // know how far to rewind the underlying stream afterward.
  size_t position() const { return cursor; }

  Value read_all_forms() {
    GCGuard guard(vm.heap);
    std::vector<Value> forms;
    while (true) {
      skip_whitespace_and_comments();
      if (is_at_end())
        break;
      forms.push_back(read_form());
    }
    if (forms.empty())
      return Value::nil();
    if (forms.size() == 1)
      return forms[0];

    // Wrap multiple forms in (begin ...)
    Value begin_sym = Value::from_symbol_id(vm.intern("begin"));
    Value list = Value::nil();
    for (auto it = forms.rbegin(); it != forms.rend(); ++it) {
      list = vm.heap.cons(*it, list);
    }
    return vm.heap.cons(begin_sym, list);
  }

  Value read_form() {
    GCGuard guard(vm.heap);
    skip_whitespace_and_comments();
    if (is_at_end())
      return Value::eof_obj();

    char c = peek();

    // Quotes and abbreviations
    if (c == '\'') {
      advance();
      return make_quote("quote", read_form());
    }
    if (c == '`') {
      advance();
      return make_quote("quasiquote", read_form());
    }
    if (c == ',') {
      advance();
      if (peek() == '@') {
        advance();
        return make_quote("unquote-splicing", read_form());
      }
      return make_quote("unquote", read_form());
    }

    // List
    if (c == '(') {
      return read_list();
    }

    // Bracketed Vector [ ... ]
    if (c == '[') {
      return read_bracket_vector();
    }

    // Braced Map { ... }
    if (c == '{') {
      return read_brace_map();
    }

    // Character literal #\c, #\space, #\newline, etc.
    if (c == '#' && peek_next() == '\\') {
      advance(); // hash
      advance(); // backslash
      if (is_at_end())
        return Value::from_char('\0');
      // The character immediately after #\ is always the literal itself —
      // #\; #\( #\) #\space all just mean "that character" — regardless
      // of whether it looks like a delimiter elsewhere in the grammar.
      // Only when it's alphabetic can it possibly be the start of a
      // multi-character name like #\space or #\newline, so only then do
      // we keep scanning. (Previously the scan loop excluded delimiter
      // characters up front, so #\; produced an empty name — undefined
      // behavior on name[0] — and never consumed the ';', which then got
      // swallowed as a line comment by the next read, eating everything
      // up to the next newline including any closing parens.)
      size_t start = cursor;
      char first = advance();
      if (std::isalpha(static_cast<unsigned char>(first))) {
        while (!is_at_end() && std::isalpha(static_cast<unsigned char>(peek()))) {
          advance();
        }
      }
      std::string_view name = src.substr(start, cursor - start);
      // R4RS named character literals (#\Space, #\NEWLINE, ...) are
      // case-insensitive, unlike everything else in this dialect.
      if (name.size() > 1) {
        if (iequals(name, "space")) return Value::from_char(' ');
        if (iequals(name, "newline")) return Value::from_char('\n');
        if (iequals(name, "tab")) return Value::from_char('\t');
        if (iequals(name, "return")) return Value::from_char('\r');
      }
      return Value::from_char(name[0]);
    }

    // Vector #( ... )
    if (c == '#' && peek_next() == '(') {
      advance(); // #
      advance(); // (
      return read_vector();
    }

    // String
    if (c == '"') {
      return read_string();
    }

    // Atom (number, boolean, character, symbol, keyword)
    return read_atom();
  }

private:
  // Reports a fault at a byte offset, as line:column.
  //
  // The line and column are recomputed by walking the source from the
  // start, which is linear — and free, because this runs exactly once,
  // on the way out. Tracking a line counter through every advance() to
  // save a walk that only ever happens when the program is already
  // wrong would be paying on the fast path for the slow one.
  [[noreturn]] void fault(size_t at, const std::string &what) const {
    size_t line = 1, col = 1;
    for (size_t i = 0; i < at && i < src.size(); ++i) {
      if (src[i] == '\n') { line++; col = 1; } else { col++; }
    }
    std::ostringstream os;
    os << "read error at line " << line << ", column " << col << ": " << what;
    throw ReaderError(os.str());
  }

  static inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return true;
  }

  inline bool is_at_end() const { return cursor >= src.size(); }

  inline char peek() const {
    if (is_at_end())
      return '\0';
    return src[cursor];
  }

  inline char peek_next() const {
    if (cursor + 1 >= src.size())
      return '\0';
    return src[cursor + 1];
  }

  inline char advance() { return src[cursor++]; }

  void skip_whitespace_and_comments() {
    while (!is_at_end()) {
      char c = peek();
      if (std::isspace(static_cast<unsigned char>(c))) {
        advance();
      } else if (c == ';') {
        // Comment until newline
        while (!is_at_end() && peek() != '\n') {
          advance();
        }
      } else {
        break;
      }
    }
  }

  Value make_quote(const char *quote_name, Value inner) {
    Value sym = Value::from_symbol_id(vm.intern(quote_name));
    return vm.heap.cons(sym, vm.heap.cons(inner, Value::nil()));
  }

  Value read_list() {
    const size_t open_at = cursor;
    advance(); // '('
    skip_whitespace_and_comments();
    if (peek() == ')') {
      advance();
      return Value::nil();
    }

    std::vector<Value> elements;
    Value tail = Value::nil();
    bool is_dotted = false;
    bool closed = false;

    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == ')') {
        advance();
        closed = true;
        break;
      }

      // Check for dotted pair '.'
      if (peek() == '.' &&
          std::isspace(static_cast<unsigned char>(peek_next()))) {
        advance(); // '.'
        skip_whitespace_and_comments();
        tail = read_form();
        is_dotted = true;
        skip_whitespace_and_comments();
        if (peek() == ')') {
          advance();
          closed = true;
        }
        break;
      }

      elements.push_back(read_form());
    }

    // THE SILENT ONE. This loop used to end on is_at_end() and return the
    // elements it had, which is a perfectly well-formed list — so a file
    // missing one ')' did not fail to load, it loaded something else, and
    // the only symptom was an expression sitting one level deeper than it
    // was written. Nothing downstream can detect that, because by then it
    // is indistinguishable from what you meant to type.
    if (!closed)
      fault(open_at, "'(' is never closed");

    Value result = is_dotted ? tail : Value::nil();
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      result = vm.heap.cons(*it, result);
    }
    return result;
  }

  // [e1 e2 ...] desugars to a (%bracket-vector e1 e2 ...) call form, NOT a
  // literal ObjVector like read_vector() below builds for #(...) —
  // deliberately: Clojure-style bracket vectors evaluate their elements
  // when used as an expression (e.g. `[(v 1) (:y a)]`), unlike R4RS's
  // self-evaluating #(...) vectors. %bracket-vector is a reserved alias
  // for the `vector` procedure (see vx_vm.cpp) rather than the symbol
  // `vector` itself, so that a user's own quoted list headed by the
  // ordinary symbol `vector` (e.g. `'vector`, `'(vector 1 2)`) can't be
  // confused for a desugared bracket form. The compiler's
  // `quote`/`quasiquote`/`do`/`let` handling recognizes
  // (%bracket-vector ...)-headed lists specially and materializes a real
  // ObjVector (or binding list) out of them instead of leaving them as an
  // inert call form — see quote_materialize in vx_compiler.h.
  Value read_bracket_vector() {
    const size_t open_at = cursor;
    advance(); // '['
    skip_whitespace_and_comments();
    std::vector<Value> elements;
    bool closed = false;
    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == ']') {
        advance();
        closed = true;
        break;
      }
      elements.push_back(read_form());
    }
    if (!closed)
      fault(open_at, "'[' is never closed");
    Value list = Value::nil();
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      list = vm.heap.cons(*it, list);
    }
    Value vec_sym = Value::from_symbol_id(vm.intern("%bracket-vector"));
    return vm.heap.cons(vec_sym, list);
  }

  // {k1 v1 ...} desugars to a (%brace-map k1 v1 ...) call form — same
  // reasoning as read_bracket_vector above.
  Value read_brace_map() {
    const size_t open_at = cursor;
    advance(); // '{'
    skip_whitespace_and_comments();
    std::vector<Value> elements;
    bool closed = false;
    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == '}') {
        advance();
        closed = true;
        break;
      }
      elements.push_back(read_form());
    }
    if (!closed)
      fault(open_at, "'{' is never closed");
    Value list = Value::nil();
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      list = vm.heap.cons(*it, list);
    }
    Value map_sym = Value::from_symbol_id(vm.intern("%brace-map"));
    return vm.heap.cons(map_sym, list);
  }

  Value read_vector() {
    // The caller consumed both '#' and '(' before handing over, so the
    // opening is two bytes back from here.
    const size_t open_at = cursor >= 2 ? cursor - 2 : 0;
    std::vector<Value> elements;
    bool closed = false;
    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == ')') {
        advance();
        closed = true;
        break;
      }
      elements.push_back(read_form());
    }
    if (!closed)
      fault(open_at, "'#(' is never closed");
    Value vec = vm.heap.make_vector(static_cast<uint32_t>(elements.size()));
    ObjVector *ov = vec.as_ptr<ObjVector>();
    for (size_t i = 0; i < elements.size(); ++i) {
      ov->set(static_cast<uint32_t>(i), elements[i]);
    }
    return vec;
  }

  Value read_string() {
    const size_t open_at = cursor;
    advance(); // '"'
    std::string s;
    while (!is_at_end() && peek() != '"') {
      char c = advance();
      if (c == '\\' && !is_at_end()) {
        char esc = advance();
        if (esc == 'n')
          s += '\n';
        else if (esc == 't')
          s += '\t';
        else if (esc == 'r')
          s += '\r';
        else if (esc == '"')
          s += '"';
        else if (esc == '\\')
          s += '\\';
        else
          s += esc;
      } else {
        s += c;
      }
    }
    // An unterminated string used to swallow the entire rest of the file
    // and return it as a perfectly good string, so every definition after
    // the stray quote quietly vanished from the program.
    if (peek() != '"')
      fault(open_at, "string is never closed");
    advance();
    return vm.heap.make_string(s);
  }

  Value read_atom() {
    size_t start = cursor;
    while (!is_at_end()) {
      char c = peek();
      if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' ||
          c == '[' || c == ']' || c == '{' || c == '}' || c == '"' ||
          c == ';') {
        break;
      }
      advance();
    }

    // THE HANG. read_atom stops at a delimiter without consuming it, so
    // reaching here on a stray ')' produced a zero-length token and left
    // the cursor exactly where it was — and read_all_forms' loop, seeing
    // input remaining, called straight back in. Measured: 53 seconds of
    // appending the empty symbol to a vector, then std::bad_alloc, and a
    // VM too damaged to evaluate anything afterward. In a browser tab
    // that is a freeze followed by a dead interpreter, from one keystroke.
    if (cursor == start)
      fault(start, std::string("unexpected '") + peek() + "'");

    std::string_view token = src.substr(start, cursor - start);

    // Booleans
    if (token == "#t" || token == "#true")
      return Value::boolean_true();
    if (token == "#f" || token == "#false")
      return Value::boolean_false();

    // Keywords (:foo)
    if (token.size() > 1 && token[0] == ':') {
      std::string kw_name = std::string(token.substr(1));
      uint32_t kw_id = vm.intern(kw_name);
      return Value::from_keyword_id(kw_id);
    }

    // Number (Integer or Real)
    char *endptr = nullptr;
    const char *begin = token.data();

    // Check if it looks like an integer. Fixnums are 32-bit (see
    // Value::from_int), and anything wider becomes a flonum — the same
    // promotion arithmetic already performs on overflow. Truncating to
    // int32_t here instead, as this used to, silently read 4294967295 as
    // -1 and 2147483648 as -2147483648: a literal quietly becoming a
    // different number, which is about the worst failure a reader has.
    errno = 0;
    long long int_val = std::strtoll(begin, &endptr, 10);
    if (endptr == begin + token.size()) {
      if (errno != ERANGE &&
          int_val >= INT32_MIN && int_val <= INT32_MAX) {
        return Value::from_int(static_cast<int32_t>(int_val));
      }
      // Too wide for a fixnum — reparse as a double so magnitudes beyond
      // long long (1e30, say) land correctly rather than clamping.
      double wide = std::strtod(begin, &endptr);
      if (endptr == begin + token.size()) return Value::from_double(wide);
      return Value::from_double(static_cast<double>(int_val));
    }

    // Check if it's a float
    double double_val = std::strtod(begin, &endptr);
    if (endptr == begin + token.size()) {
      return Value::from_double(double_val);
    }

    // Otherwise it's a symbol
    uint32_t sym_id = vm.intern(std::string(token));
    return Value::from_symbol_id(sym_id);
  }

  VM &vm;
  std::string_view src;
  size_t cursor;
};

} // namespace vxs
