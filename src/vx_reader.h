#pragma once

#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include <string_view>
#include <cctype>
#include <sstream>

namespace vxs {

//=============================================================================
// Scheme S-Expression Reader / Lexer
//=============================================================================
class Reader {
public:
  Reader(VM &vm, std::string_view source)
      : vm(vm), src(source), cursor(0) {}

  Value read_all_forms() {
    std::vector<Value> forms;
    while (true) {
      skip_whitespace_and_comments();
      if (is_at_end()) break;
      forms.push_back(read_form());
    }
    if (forms.empty()) return Value::nil();
    if (forms.size() == 1) return forms[0];

    // Wrap multiple forms in (begin ...)
    Value begin_sym = Value::from_symbol_id(vm.intern("begin"));
    Value list = Value::nil();
    for (auto it = forms.rbegin(); it != forms.rend(); ++it) {
      list = vm.heap.cons(*it, list);
    }
    return vm.heap.cons(begin_sym, list);
  }

  Value read_form() {
    skip_whitespace_and_comments();
    if (is_at_end()) return Value::eof_obj();

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
  inline bool is_at_end() const {
    return cursor >= src.size();
  }

  inline char peek() const {
    if (is_at_end()) return '\0';
    return src[cursor];
  }

  inline char peek_next() const {
    if (cursor + 1 >= src.size()) return '\0';
    return src[cursor + 1];
  }

  inline char advance() {
    return src[cursor++];
  }

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
    advance(); // '('
    skip_whitespace_and_comments();
    if (peek() == ')') {
      advance();
      return Value::nil();
    }

    std::vector<Value> elements;
    Value tail = Value::nil();
    bool is_dotted = false;

    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == ')') {
        advance();
        break;
      }

      // Check for dotted pair '.'
      if (peek() == '.' && std::isspace(static_cast<unsigned char>(peek_next()))) {
        advance(); // '.'
        skip_whitespace_and_comments();
        tail = read_form();
        is_dotted = true;
        skip_whitespace_and_comments();
        if (peek() == ')') advance();
        break;
      }

      elements.push_back(read_form());
    }

    Value result = is_dotted ? tail : Value::nil();
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      result = vm.heap.cons(*it, result);
    }
    return result;
  }

  Value read_bracket_vector() {
    advance(); // '['
    skip_whitespace_and_comments();
    std::vector<Value> elements;
    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == ']') {
        advance();
        break;
      }
      elements.push_back(read_form());
    }
    Value list = Value::nil();
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      list = vm.heap.cons(*it, list);
    }
    Value vec_sym = Value::from_symbol_id(vm.intern("vector"));
    return vm.heap.cons(vec_sym, list);
  }

  Value read_brace_map() {
    advance(); // '{'
    skip_whitespace_and_comments();
    std::vector<Value> elements;
    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == '}') {
        advance();
        break;
      }
      elements.push_back(read_form());
    }
    Value list = Value::nil();
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
      list = vm.heap.cons(*it, list);
    }
    Value map_sym = Value::from_symbol_id(vm.intern("hash-map"));
    return vm.heap.cons(map_sym, list);
  }

  Value read_vector() {
    std::vector<Value> elements;
    while (!is_at_end()) {
      skip_whitespace_and_comments();
      if (peek() == ')') {
        advance();
        break;
      }
      elements.push_back(read_form());
    }
    Value vec = vm.heap.make_vector(static_cast<uint32_t>(elements.size()));
    ObjVector *ov = vec.as_ptr<ObjVector>();
    for (size_t i = 0; i < elements.size(); ++i) {
      ov->set(static_cast<uint32_t>(i), elements[i]);
    }
    return vec;
  }

  Value read_string() {
    advance(); // '"'
    std::string s;
    while (!is_at_end() && peek() != '"') {
      char c = advance();
      if (c == '\\' && !is_at_end()) {
        char esc = advance();
        if (esc == 'n') s += '\n';
        else if (esc == 't') s += '\t';
        else if (esc == 'r') s += '\r';
        else if (esc == '"') s += '"';
        else if (esc == '\\') s += '\\';
        else s += esc;
      } else {
        s += c;
      }
    }
    if (peek() == '"') advance();
    return vm.heap.make_string(s);
  }

  Value read_atom() {
    size_t start = cursor;
    while (!is_at_end()) {
      char c = peek();
      if (std::isspace(static_cast<unsigned char>(c)) ||
          c == '(' || c == ')' ||
          c == '[' || c == ']' ||
          c == '{' || c == '}' ||
          c == '"' || c == ';') {
        break;
      }
      advance();
    }

    std::string_view token = src.substr(start, cursor - start);

    // Booleans
    if (token == "#t" || token == "#true") return Value::boolean_true();
    if (token == "#f" || token == "#false") return Value::boolean_false();

    // Keywords (:foo)
    if (token.size() > 1 && token[0] == ':') {
      std::string kw_name = std::string(token.substr(1));
      uint32_t kw_id = vm.intern(kw_name);
      return Value::from_keyword_id(kw_id);
    }

    // Number (Integer or Real)
    char *endptr = nullptr;
    const char *begin = token.data();
    
    // Check if it looks like an integer
    long long int_val = std::strtoll(begin, &endptr, 10);
    if (endptr == begin + token.size()) {
      return Value::from_int(static_cast<int32_t>(int_val));
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
