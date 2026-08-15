//----------------------------------------------------------------------
// vx-scheme : Modern 64-Bit NaN-Boxed Scheme Engine
// Copyright (c) 2002-2026 Colin Smith and Antigravity contributors.
//----------------------------------------------------------------------

#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include "vx_reader.h"
#include "vx_compiler.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace vxs;

static Value eval_string(VM &vm, const std::string &code, bool &ok, std::string &err_msg) {
  try {
    Reader reader(vm, code);
    Value form = reader.read_all_forms();
    Compiler compiler(vm);
    ObjClosure *closure = compiler.compile_top_level(form);

    Fiber fiber;
    fiber.push(Value::from_ptr(closure));
    size_t frame_slots = std::max<size_t>(1, closure->max_locals);
    fiber.stack.resize(frame_slots, Value::unspecified());
    fiber.frames.push_back({closure, closure->chunk->code.data(), 0});

    VM::StepResult res = vm.step_fiber(fiber, 100000000);
    if (res == VM::StepResult::Error || fiber.state == Fiber::State::Error) {
      ok = false;
      err_msg = fiber.error_message.empty() ? "[VM Error] Execution error" : fiber.error_message;
      return Value::unspecified();
    }
    ok = true;
    return fiber.result;
  } catch (const std::exception &e) {
    ok = false;
    err_msg = std::string("[Error] ") + e.what();
    return Value::unspecified();
  }
}

static void run_repl(VM &vm) {
  std::cout << "Vx-Scheme 0.8 (64-bit NaN-Boxed Bytecode Engine)" << std::endl;
  std::cout << "Type (exit) or Ctrl+D to quit." << std::endl;

  std::string line;
  while (true) {
    std::cout << "vxs> ";
    std::cout.flush();
    if (!std::getline(std::cin, line)) break;

    // Skip empty lines
    if (line.empty()) continue;
    if (line == "(exit)" || line == ":quit" || line == "quit") break;

    bool ok = false;
    std::string err;
    Value res = eval_string(vm, line, ok, err);
    if (ok) {
      if (!res.is_unspecified()) {
        std::cout << "=> " << vm.format_value(res) << std::endl;
      }
    } else {
      std::cerr << err << std::endl;
    }
  }
}

#include <unordered_set>
#include <iomanip>

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

  std::vector<ObjClosure*> closures;
  std::unordered_set<ObjClosure*> seen;
  collect_closures(root_closure, closures, seen);

  std::string tmp_cpp = "_standalone_build.cpp";
  std::ofstream out(tmp_cpp);

  out << R"(// Auto-generated AOT Bytecode by Vx-Scheme Compiler
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
      Value v = cl->chunk->constants[c];
      out << "  chunk_" << i << "->constants.push_back(";
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
      } else if (v.is_symbol()) {
        std::string sym_name = compile_vm.get_symbol_name(v.as_symbol_id());
        out << "Value::from_symbol_id(vm.intern(\"" << sym_name << "\"))";
      } else if (v.is_keyword()) {
        std::string kw_name = compile_vm.get_symbol_name(v.as_keyword_id());
        out << "Value::from_keyword_id(vm.intern(\"" << kw_name << "\"))";
      } else if (Heap::is_string(v)) {
        std::string str_val = std::string(v.as_ptr<ObjString>()->view());
        out << "vm.heap.make_string(\"" << str_val << "\")";
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
      out << ");\n";
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
  out << "  VM::StepResult res = vm.step_fiber(fiber, 100000000);\n";
  out << "  if (res == VM::StepResult::Error || fiber.state == Fiber::State::Error) {\n";
  out << "    std::cerr << (fiber.error_message.empty() ? \"[VM Error] Execution error\" : fiber.error_message) << std::endl;\n";
  out << "    return 1;\n";
  out << "  }\n";
  out << "  if (!fiber.result.is_unspecified()) {\n";
  out << "    std::cout << vm.format_value(fiber.result) << std::endl;\n";
  out << "  }\n";
  out << "  return 0;\n";
  out << "}\n";
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
      std::vector<ObjClosure*> closures;
      std::unordered_set<ObjClosure*> seen;
      collect_closures(root_closure, closures, seen);

      std::stringstream out;
      out << "// Auto-generated AOT Bytecode by Vx-Scheme Compiler\n";
      out << "#include \"vx_value.h\"\n#include \"vx_heap.h\"\n#include \"vx_vm.h\"\n#include <iostream>\n#include <iomanip>\n#include <vector>\n#include <iterator>\n\nusing namespace vxs;\n\n";
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
      out << "int main(int argc, char **argv) {\n  (void)argc; (void)argv;\n  VM vm;\n\n";
      for (size_t i = 0; i < closures.size(); ++i) {
        ObjClosure *cl = closures[i];
        out << "  BytecodeChunk *chunk_" << i << " = new BytecodeChunk();\n";
        out << "  chunk_" << i << "->code.assign(std::begin(CHUNK_" << i << "_CODE), std::end(CHUNK_" << i << "_CODE));\n";
        for (size_t c = 0; c < cl->chunk->constants.size(); ++c) {
          Value v = cl->chunk->constants[c];
          out << "  chunk_" << i << "->constants.push_back(";
          if (v.is_int()) out << "Value::from_int(" << v.as_int() << ")";
          else if (v.is_double()) out << "Value::from_double(" << std::setprecision(16) << v.as_double() << ")";
          else if (v.is_bool()) out << (v.as_bool() ? "Value::boolean_true()" : "Value::boolean_false()");
          else if (v.is_nil()) out << "Value::nil()";
          else if (v.is_unspecified()) out << "Value::unspecified()";
          else if (v.is_symbol()) out << "Value::from_symbol_id(vm.intern(\"" << compile_vm.get_symbol_name(v.as_symbol_id()) << "\"))";
          else if (v.is_keyword()) out << "Value::from_keyword_id(vm.intern(\"" << compile_vm.get_symbol_name(v.as_keyword_id()) << "\"))";
          else if (Heap::is_string(v)) out << "vm.heap.make_string(\"" << std::string(v.as_ptr<ObjString>()->view()) << "\")";
          else if (Heap::is_closure(v)) {
            int child_ix = -1;
            for (size_t k = 0; k < closures.size(); ++k) {
              if (closures[k] == v.as_ptr<ObjClosure>()) { child_ix = static_cast<int>(k); break; }
            }
            out << "Value::from_ptr(closure_" << child_ix << ")";
          } else out << "Value::unspecified()";
          out << ");\n";
        }
        out << "  ObjClosure *closure_" << i << " = vm.heap.allocate<ObjClosure>(chunk_" << i << ", "
            << cl->arity << ", " << (cl->is_variadic ? "true" : "false") << ", "
            << cl->env_size << ", " << cl->max_locals << ");\n\n";
      }
      int root_ix = static_cast<int>(closures.size()) - 1;
      out << "  Fiber fiber;\n  fiber.push(Value::from_ptr(closure_" << root_ix << "));\n";
      out << "  size_t frame_slots = std::max<size_t>(1, closure_" << root_ix << "->max_locals);\n";
      out << "  fiber.stack.resize(frame_slots, Value::unspecified());\n";
      out << "  fiber.frames.push_back({closure_" << root_ix << ", closure_" << root_ix << "->chunk->code.data(), 0});\n\n";
      out << "  VM::StepResult res = vm.step_fiber(fiber, 100000000);\n";
      out << "  if (res == VM::StepResult::Error || fiber.state == Fiber::State::Error) {\n";
      out << "    std::cerr << (fiber.error_message.empty() ? \"[VM Error] Execution error\" : fiber.error_message) << std::endl;\n    return 1;\n  }\n";
      out << "  if (!fiber.result.is_unspecified()) std::cout << vm.format_value(fiber.result) << std::endl;\n";
      out << "  return 0;\n}\n";

      if (output_file.empty()) {
        std::cout << out.str();
      } else {
        std::ofstream fout(output_file);
        fout << out.str();
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

  // Interactive REPL
  run_repl(vm);
  return 0;
}
