#include "../src/vx_value.h"
#include "../src/vx_heap.h"
#include "../src/vx_vm.h"
#include "../src/vx_reader.h"
#include "../src/vx_compiler.h"
#include <iostream>
#include <cassert>

using namespace vxs;

Value eval_scheme(VM &vm, const std::string &code) {
  Reader reader(vm, code);
  Value form = reader.read_all_forms();
  Compiler compiler(vm);
  ObjClosure *closure = compiler.compile_top_level(form);

  Fiber fiber;
  fiber.push(Value::from_ptr(closure));
  fiber.frames.push_back({closure, closure->chunk->code.data(), 0});

  VM::StepResult res = vm.step_fiber(fiber);
  if (res == VM::StepResult::Error) {
    std::cerr << "Runtime Error: " << fiber.error_message << std::endl;
    assert(false);
  }
  return fiber.result;
}

int main() {
  std::cout << "=== RUNNING END-TO-END NAN-BOX SCHEME COMPILER & VM TESTS ===" << std::endl;

  VM vm;

  // 1. Basic arithmetic
  {
    Value res = eval_scheme(vm, "(+ 123 456)");
    assert(res.as_int() == 579);
    std::cout << "  [PASS] Arithmetic: (+ 123 456) => " << vm.format_value(res) << std::endl;
  }

  // 2. Recursive Factorial
  {
    eval_scheme(vm, "(define (fact n) (if (<= n 1) 1 (* n (fact (- n 1)))))");
    Value res = eval_scheme(vm, "(fact 10)");
    assert(res.as_int() == 3628800);
    std::cout << "  [PASS] Recursive Factorial: (fact 10) => " << vm.format_value(res) << std::endl;
  }

  // 3. Named Let Loop
  {
    Value res = eval_scheme(vm, "(let loop ((i 0) (sum 0)) (if (> i 10) sum (loop (+ i 1) (+ sum i))))");
    assert(res.as_int() == 55);
    std::cout << "  [PASS] Named Let Loop: 0..10 sum => " << vm.format_value(res) << std::endl;
  }

  // 4. Higher-order functions & closures
  {
    eval_scheme(vm, "(define (adder x) (lambda (y) (+ x y)))");
    Value res = eval_scheme(vm, "((adder 5) 37)");
    assert(res.as_int() == 42);
    std::cout << "  [PASS] Higher-order closure: ((adder 5) 37) => " << vm.format_value(res) << std::endl;
  }

  // 5. When macro
  {
    Value res = eval_scheme(vm, "(when #t 99)");
    assert(res.as_int() == 99);
    std::cout << "  [PASS] Macro: (when #t 99) => " << vm.format_value(res) << std::endl;
  }

  // 6. First-class Future & Touch
  {
    Value res = eval_scheme(vm, "(touch (future (* 6 7)))");
    assert(res.as_int() == 42);
    std::cout << "  [PASS] First-class Future & Touch: (touch (future (* 6 7))) => " << vm.format_value(res) << std::endl;
  }

  // 7. Background Fiber Yielding & Multi-step Scheduling
  {
    eval_scheme(vm, "(define bg-val 0)");
    eval_scheme(vm, "(future (begin (set! bg-val 1) (yield) (set! bg-val 2) (yield) (set! bg-val 3)))");
    assert(vm.active_fibers.size() == 1);
    assert(vm.get_global("bg-val").as_int() == 0);

    // Step 1
    vm.step_all_active_fibers(10);
    assert(vm.get_global("bg-val").as_int() == 1);

    // Step 2
    vm.step_all_active_fibers(10);
    assert(vm.get_global("bg-val").as_int() == 2);

    // Step 3
    vm.step_all_active_fibers(10);
    assert(vm.get_global("bg-val").as_int() == 3);
    assert(vm.active_fibers.empty());

    std::cout << "  [PASS] Cooperative Background Fiber stepping across multiple animation slices" << std::endl;
  }

  std::cout << "\n✨ ALL END-TO-END COMPILER, RUNTIME & CONCURRENCY TESTS PASSED! ✨\n" << std::endl;
  return 0;
}
