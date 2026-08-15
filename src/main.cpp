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

int main(int argc, char **argv) {
  VM vm;

  if (argc > 1) {
    std::string arg1 = argv[1];
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
