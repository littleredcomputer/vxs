//----------------------------------------------------------------------
// vxs — command-line driver for the vx-scheme binary
// Copyright (c) 2002-2026 Colin Smith.
//----------------------------------------------------------------------

#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include "vx_reader.h"
#include "vx_compiler.h"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace vxs;

static Value eval_string(VM &vm, const std::string &code, bool &ok, std::string &err_msg) {
  try {
    Reader reader(vm, code);
    Value last_res = Value::unspecified();
    while (true) {
      Value form = reader.read_form();
      if (form.is_eof()) break;
      Compiler compiler(vm);
      ObjClosure *closure = compiler.compile_top_level(form);

      Fiber fiber;
      fiber.push(Value::from_ptr(closure));
      size_t frame_slots = std::max<size_t>(1, closure->max_locals);
      fiber.stack.resize(frame_slots, Value::unspecified());
      fiber.frames.push_back({closure, closure->chunk->code.data(), 0});

      // UNBOUNDED: a top-level script that never terminates is the
      // program's bug, same as in any interpreter — not the VM's to cap.
      VM::StepResult res = vm.step_fiber(fiber);
      if (res == VM::StepResult::Error || fiber.state == Fiber::State::Error) {
        ok = false;
        err_msg = fiber.error_message.empty() ? "[VM Error] Execution error" : fiber.error_message;
        return Value::unspecified();
      }
      last_res = fiber.result;
    }
    ok = true;
    return last_res;
  } catch (const RaiseEscape &e) {
    // RaiseEscape's own message is already fully formatted (see
    // format_raised_value: "[Scheme Error] ..." for an error-object,
    // "uncaught exception: ..." otherwise) — an uncaught raise/error,
    // not a VM-internal fault, so it doesn't want the generic "[Error]"
    // wrapper below (that's for things like a stale continuation, whose
    // std::runtime_error message has no formatting of its own).
    ok = false;
    err_msg = e.what();
    return Value::unspecified();
  } catch (const std::exception &e) {
    ok = false;
    err_msg = std::string("[Error] ") + e.what();
    return Value::unspecified();
  }
}

static bool is_balanced(const std::string &code) {
  int parens = 0;
  int brackets = 0;
  int braces = 0;
  bool in_string = false;
  bool in_comment = false;
  bool escape = false;

  for (size_t i = 0; i < code.size(); ++i) {
    char c = code[i];
    if (in_comment) {
      if (c == '\n') in_comment = false;
      continue;
    }
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == ';') {
      in_comment = true;
      continue;
    }
    if (c == '"') {
      in_string = true;
      continue;
    }
    if (c == '(') ++parens;
    else if (c == ')') --parens;
    else if (c == '[') ++brackets;
    else if (c == ']') --brackets;
    else if (c == '{') ++braces;
    else if (c == '}') --braces;
  }
  return parens <= 0 && brackets <= 0 && braces <= 0 && !in_string;
}

static void run_repl(VM &vm) {
  std::cout << "vxs 0.8 (64-bit NaN-Boxed Bytecode Engine)" << std::endl;
  std::cout << "Type (exit) or Ctrl+D to quit." << std::endl;

  std::string buffer;
  while (true) {
    if (buffer.empty()) {
      std::cout << "vxs> ";
    } else {
      std::cout << "...> ";
    }
    std::cout.flush();

    std::string line;
    if (!std::getline(std::cin, line)) break;

    if (buffer.empty()) {
      if (line.empty()) continue;
      if (line == "(exit)" || line == ":quit" || line == "quit") break;
    }

    if (!buffer.empty()) buffer += "\n";
    buffer += line;

    if (!is_balanced(buffer)) {
      continue;
    }

    bool ok = false;
    std::string err;
    Value res = eval_string(vm, buffer, ok, err);
    if (ok) {
      if (!res.is_unspecified()) {
        std::cout << "=> " << vm.format_value(res) << std::endl;
      }
    } else {
      std::cerr << err << std::endl;
    }
    buffer.clear();
  }
}

#include <unordered_set>
#include <iomanip>

// Escapes a Scheme string constant for embedding in generated C++ source
// (AOT compile_standalone / --emit-cpp) — without this, a `"` or `\` in
// the original string, or a raw control character like newline, would
// land in the generated make_string("...") call and either break the
// C++ literal or silently change the string's contents.
static std::string escape_cpp_string(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default: out += static_cast<char>(c); break;
    }
  }
  return out;
}

static void collect_closures(ObjClosure *cl, std::vector<ObjClosure*> &list, std::unordered_set<ObjClosure*> &seen) {
  if (seen.count(cl)) return;
  seen.insert(cl);
  for (Value c : cl->chunk->constants) {
    if (Heap::is_closure(c)) {
      collect_closures(c.as_ptr<ObjClosure>(), list, seen);
    }
  }
  list.push_back(cl);
}

// Recursively emits a C++ expression that reconstructs `v` inside the
// generated AOT binary. Scalars (int/double/bool/nil/unspecified/char/
// symbol/keyword/string) are plain constructor calls; compound heap
// values (cons/vector/map) recurse into their own elements, since a
// quoted constant can nest arbitrarily deeply (e.g. an association list,
// or vx-test.scm's `(define testcases '("r4rstest" "pi" ...))`).
// Closures are the one case that can't be an inline expression — they're
// pre-declared as `closure_N` C++ locals earlier in generated main(), so
// this just references that variable by looking up its index.
static std::string emit_value_expr(VM &compile_vm, Value v, const std::vector<ObjClosure*> &closures) {
  std::ostringstream out;
  if (v.is_int()) {
    out << "Value::from_int(" << v.as_int() << ")";
  } else if (v.is_double()) {
    out << "Value::from_double(" << std::setprecision(16) << v.as_double() << ")";
  } else if (v.is_bool()) {
    out << (v.as_bool() ? "Value::boolean_true()" : "Value::boolean_false()");
  } else if (v.is_nil()) {
    out << "Value::nil()";
  } else if (v.is_unspecified()) {
    out << "Value::unspecified()";
  } else if (v.is_char()) {
    // Emit by code point rather than a C++ char literal — sidesteps
    // escaping entirely (works uniformly for space, quote, backslash,
    // newline, etc.).
    out << "Value::from_char(static_cast<char>(" << static_cast<int>(v.as_char()) << "))";
  } else if (v.is_symbol()) {
    out << "Value::from_symbol_id(vm.intern(\"" << compile_vm.get_symbol_name(v.as_symbol_id()) << "\"))";
  } else if (v.is_keyword()) {
    out << "Value::from_keyword_id(vm.intern(\"" << compile_vm.get_symbol_name(v.as_keyword_id()) << "\"))";
  } else if (Heap::is_string(v)) {
    out << "vm.heap.make_string(\"" << escape_cpp_string(std::string(v.as_ptr<ObjString>()->view())) << "\")";
  } else if (Heap::is_cons(v)) {
    out << "vm.heap.cons(" << emit_value_expr(compile_vm, Heap::car(v), closures)
        << ", " << emit_value_expr(compile_vm, Heap::cdr(v), closures) << ")";
  } else if (Heap::is_vector(v)) {
    ObjVector *ov = v.as_ptr<ObjVector>();
    out << "vm.heap.make_vector_from({";
    for (uint32_t i = 0; i < ov->size; ++i) {
      if (i > 0) out << ", ";
      out << emit_value_expr(compile_vm, ov->get(i), closures);
    }
    out << "})";
  } else if (Heap::is_map(v)) {
    ObjMap *m = v.as_ptr<ObjMap>();
    out << "vm.heap.make_map({";
    for (size_t i = 0; i < m->entries.size(); ++i) {
      if (i > 0) out << ", ";
      out << "{" << emit_value_expr(compile_vm, m->entries[i].first, closures)
          << ", " << emit_value_expr(compile_vm, m->entries[i].second, closures) << "}";
    }
    out << "})";
  } else if (Heap::is_closure(v)) {
    ObjClosure *child_cl = v.as_ptr<ObjClosure>();
    int child_ix = -1;
    for (size_t k = 0; k < closures.size(); ++k) {
      if (closures[k] == child_cl) { child_ix = static_cast<int>(k); break; }
    }
    out << "Value::from_ptr(closure_" << child_ix << ")";
  } else {
    out << "Value::unspecified()";
  }
  return out.str();
}

// Generates the complete, self-contained C++ source for an AOT-compiled
// binary of `root_closure` (compiled from `compile_vm`) — static bytecode
// arrays plus a main() that reconstructs each closure's chunk (including
// every constant, via emit_value_expr above) and runs the root one.
// Shared by compile_standalone (-c/--compile) and the --emit-cpp path so
// the two can't drift out of sync with each other the way the constant
// emission logic already had (twice: missing char and string-escaping
// cases were fixed once each, in only one of the two copies).
static std::string generate_aot_source(VM &compile_vm, ObjClosure *root_closure) {
  std::vector<ObjClosure*> closures;
  std::unordered_set<ObjClosure*> seen;
  collect_closures(root_closure, closures, seen);

  std::ostringstream out;
  out << R"(// Auto-generated AOT Bytecode by vxs Compiler
#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include <iostream>
#include <iomanip>
#include <vector>
#include <iterator>

using namespace vxs;

)";

  // 1. Emit static bytecode arrays
  for (size_t i = 0; i < closures.size(); ++i) {
    ObjClosure *cl = closures[i];
    out << "// Chunk " << i << " bytecode (" << cl->chunk->code.size() << " bytes)\n";
    out << "static const uint8_t CHUNK_" << i << "_CODE[] = {";
    for (size_t b = 0; b < cl->chunk->code.size(); ++b) {
      if (b % 16 == 0) out << "\n  ";
      out << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(cl->chunk->code[b]) << ", ";
    }
    out << std::dec << "\n};\n\n";
  }

  // 2. Emit main() constructor and execution
  out << R"(int main(int argc, char **argv) {
  (void)argc; (void)argv;
  VM vm;

)";

  for (size_t i = 0; i < closures.size(); ++i) {
    ObjClosure *cl = closures[i];
    out << "  // Closure " << i << " (arity=" << cl->arity << ", env_size=" << cl->env_size << ", max_locals=" << cl->max_locals << ")\n";
    out << "  BytecodeChunk *chunk_" << i << " = new BytecodeChunk();\n";
    out << "  chunk_" << i << "->code.assign(std::begin(CHUNK_" << i << "_CODE), std::end(CHUNK_" << i << "_CODE));\n";
    for (size_t c = 0; c < cl->chunk->constants.size(); ++c) {
      out << "  chunk_" << i << "->constants.push_back(" << emit_value_expr(compile_vm, cl->chunk->constants[c], closures) << ");\n";
    }
    out << "  ObjClosure *closure_" << i << " = vm.heap.allocate<ObjClosure>(chunk_" << i << ", "
        << cl->arity << ", " << (cl->is_variadic ? "true" : "false") << ", "
        << cl->env_size << ", " << cl->max_locals << ");\n\n";
  }

  int root_ix = static_cast<int>(closures.size()) - 1;
  out << "  // Root execution fiber\n";
  out << "  Fiber fiber;\n";
  out << "  fiber.push(Value::from_ptr(closure_" << root_ix << "));\n";
  out << "  size_t frame_slots = std::max<size_t>(1, closure_" << root_ix << "->max_locals);\n";
  out << "  fiber.stack.resize(frame_slots, Value::unspecified());\n";
  out << "  fiber.frames.push_back({closure_" << root_ix << ", closure_" << root_ix << "->chunk->code.data(), 0});\n\n";
  out << "  VM::StepResult res = vm.step_fiber(fiber);\n";
  out << "  if (res == VM::StepResult::Error || fiber.state == Fiber::State::Error) {\n";
  out << "    std::cerr << (fiber.error_message.empty() ? \"[VM Error] Execution error\" : fiber.error_message) << std::endl;\n";
  out << "    return 1;\n";
  out << "  }\n";
  out << "  if (!fiber.result.is_unspecified()) {\n";
  out << "    std::cout << vm.format_value(fiber.result) << std::endl;\n";
  out << "  }\n";
  out << "  return 0;\n";
  out << "}\n";
  return out.str();
}

static bool compile_standalone(const std::string &input_path, const std::string &output_path) {
  std::ifstream file(input_path);
  if (!file.is_open()) {
    std::cerr << "Error: Cannot open source file: " << input_path << std::endl;
    return false;
  }
  std::stringstream buf;
  buf << file.rdbuf();
  std::string scheme_code = buf.str();

  VM compile_vm;
  Reader reader(compile_vm, scheme_code);
  Value form = reader.read_all_forms();
  Compiler compiler(compile_vm);
  ObjClosure *root_closure = compiler.compile_top_level(form);

  std::string tmp_cpp = "_standalone_build.cpp";
  std::ofstream out(tmp_cpp);
  out << generate_aot_source(compile_vm, root_closure);
  out.close();

  std::string cmd = "c++ -std=c++20 -O3 -fno-strict-aliasing -fexceptions -fno-rtti " + tmp_cpp + " vx_vm.cpp -o " + output_path;
  int sys_res = std::system(cmd.c_str());
  std::remove(tmp_cpp.c_str());
  if (sys_res == 0) {
    std::cout << "Successfully compiled AOT bytecode standalone binary: " << output_path << std::endl;
    return true;
  }
  return false;
}

int main(int argc, char **argv) {
  VM vm;

  // --gc-threshold <bytes>: override the initial auto-collection trigger
  // (default 512KB) — mainly useful for shaking out GC-rooting bugs by
  // forcing collections to fire far more (or less) often than they would
  // organically. Can appear anywhere in argv; consumed here so the rest
  // of argument dispatch below doesn't need to know about it. `filtered`
  // must outlive this function (argv is repointed at its buffer), so it's
  // declared at main()'s scope rather than in a nested block.
  static std::vector<char *> filtered;
  filtered.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--gc-threshold" && i + 1 < argc) {
      vm.heap.set_gc_threshold(std::strtoull(argv[i + 1], nullptr, 10));
      ++i;
    } else {
      filtered.push_back(argv[i]);
    }
  }
  argc = static_cast<int>(filtered.size());
  argv = filtered.data();

  if (argc > 1) {
    std::string arg1 = argv[1];

    // Standalone executable compiler: ./vx-scheme --compile script.scm -o output
    if ((arg1 == "--compile" || arg1 == "-c") && argc > 2) {
      std::string input_file = argv[2];
      std::string output_file = "a.out";
      for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc) {
          output_file = argv[i + 1];
          break;
        }
      }
      return compile_standalone(input_file, output_file) ? 0 : 1;
    }

    // Emit C++ AOT code only: ./vx-scheme --emit-cpp script.scm [-o output.cpp]
    if ((arg1 == "--emit-cpp" || arg1 == "-S") && argc > 2) {
      std::string input_file = argv[2];
      std::string output_file = "";
      for (int i = 3; i < argc; ++i) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc) {
          output_file = argv[i + 1];
          break;
        }
      }
      std::ifstream file(input_file);
      if (!file.is_open()) {
        std::cerr << "Error: Cannot open source file: " << input_file << std::endl;
        return 1;
      }
      std::stringstream buf;
      buf << file.rdbuf();
      std::string scheme_code = buf.str();
      VM compile_vm;
      Reader reader(compile_vm, scheme_code);
      Value form = reader.read_all_forms();
      Compiler compiler(compile_vm);
      ObjClosure *root_closure = compiler.compile_top_level(form);

      std::string source = generate_aot_source(compile_vm, root_closure);
      if (output_file.empty()) {
        std::cout << source;
      } else {
        std::ofstream fout(output_file);
        fout << source;
        std::cout << "Emitted C++ AOT code to: " << output_file << std::endl;
      }
      return 0;
    }

    if (arg1 == "-e" && argc > 2) {
      bool ok = false;
      std::string err;
      Value res = eval_string(vm, argv[2], ok, err);
      if (ok) {
        if (!res.is_unspecified()) std::cout << vm.format_value(res) << std::endl;
        return 0;
      } else {
        std::cerr << err << std::endl;
        return 1;
      }
    }

    // Treat as script file
    std::ifstream file(arg1);
    if (!file.is_open()) {
      std::cerr << "Error: Cannot open file: " << arg1 << std::endl;
      return 1;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    bool ok = false;
    std::string err;
    Value res = eval_string(vm, buf.str(), ok, err);
    if (!ok) {
      std::cerr << err << std::endl;
      return 1;
    }
    if (!res.is_unspecified()) {
      std::cout << vm.format_value(res) << std::endl;
    }
    return 0;
  }

  // Non-interactive piped / redirected stdin (e.g. ./vx-scheme < script.scm)
  if (!isatty(STDIN_FILENO)) {
    std::stringstream buf;
    buf << std::cin.rdbuf();
    bool ok = false;
    std::string err;
    Value res = eval_string(vm, buf.str(), ok, err);
    if (!ok) {
      std::cerr << err << std::endl;
      return 1;
    }
    if (!res.is_unspecified()) {
      std::cout << vm.format_value(res) << std::endl;
    }
    return 0;
  }

  // Interactive REPL
  run_repl(vm);
  return 0;
}
