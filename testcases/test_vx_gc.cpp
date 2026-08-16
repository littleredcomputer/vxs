#include "../src/vx_value.h"
#include "../src/vx_heap.h"
#include "../src/vx_vm.h"
#include "../src/vx_reader.h"
#include "../src/vx_compiler.h"
#include <iostream>
#include <cassert>

using namespace vxs;

static Value eval_scheme(VM &vm, const std::string &code) {
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
    std::cerr << "Runtime Error: " << fiber.error_message << std::endl;
    assert(false);
  }
  return fiber.result;
}

int main() {
  std::cout << "=== RUNNING VX-SCHEME MARK-AND-SWEEP GC UNIT & STRESS TESTS ===" << std::endl;

  // Test 1: Transient Garbage Reclamation
  {
    VM vm;
    size_t initial_bytes = vm.heap.get_bytes_allocated();
    size_t initial_objs = vm.heap.get_object_count();

    // Allocate 10,000 unrooted cons cells
    for (int i = 0; i < 10000; ++i) {
      vm.heap.cons(Value::from_int(i), Value::from_int(i + 1));
    }
    assert(vm.heap.get_object_count() >= initial_objs + 10000);
    assert(vm.heap.get_bytes_allocated() > initial_bytes);

    // Collect garbage
    vm.collect_garbage();

    // All unrooted cons cells should have been swept!
    assert(vm.heap.get_object_count() == initial_objs);
    assert(vm.heap.get_bytes_allocated() == initial_bytes);
    std::cout << "  [PASS] Transient unrooted cons cells fully reclaimed" << std::endl;
  }

  // Test 2: Global Root Preservation
  {
    VM vm;
    eval_scheme(vm, "(define preserved-list (cons 10 (cons 20 (cons 30 '()))))");
    eval_scheme(vm, "(define preserved-vec [100 200 300])");
    eval_scheme(vm, "(define preserved-map {:a 1 :b 2})");

    // Generate lots of transient garbage
    for (int i = 0; i < 5000; ++i) {
      vm.heap.make_string("temporary string junk");
      vm.heap.cons(Value::from_int(i), Value::nil());
    }

    vm.collect_garbage();

    // Globals must still be intact and valid
    Value res_list = eval_scheme(vm, "(car (cdr preserved-list))");
    assert(res_list.as_int() == 20);

    Value res_vec = eval_scheme(vm, "(vector-ref preserved-vec 1)");
    assert(res_vec.as_int() == 200);

    Value res_map = eval_scheme(vm, "(:b preserved-map)");
    assert(res_map.as_int() == 2);

    std::cout << "  [PASS] Preserved global lists, vectors, and maps survive GC sweep" << std::endl;
  }

  // Test 3: Cyclic Reference Reclamation
  {
    VM vm;
    size_t initial_objs = vm.heap.get_object_count();

    // Create orphaned cyclic structures
    {
      eval_scheme(vm, "(let ((p (cons 1 2))) (set-car! p p) (set-cdr! p p) 42)");
    }

    vm.collect_garbage();
    assert(vm.heap.get_object_count() == initial_objs);
    std::cout << "  [PASS] Orphaned cyclic structures cleanly swept" << std::endl;
  }

  // Test 4: GC Stress Under Low Threshold (Active Fiber Stack Rooting)
  {
    VM vm;
    // Set threshold very low to trigger frequent GC during evaluation
    vm.heap.set_gc_threshold(1024);

    // Compute sum of 1..500 using heavy cons list building
    eval_scheme(vm, R"(
      (define (iota n)
        (let loop ((i n) (acc '()))
          (if (<= i 0) acc (loop (- i 1) (cons i acc)))))

      (define (sum-list lst)
        (let loop ((l lst) (acc 0))
          (if (null? l) acc (loop (cdr l) (+ acc (car l))))))
    )");

    Value res = eval_scheme(vm, "(sum-list (iota 500))");
    assert(res.as_int() == (500 * 501) / 2);

    std::cout << "  [PASS] Aggressive GC under low threshold with active recursive loops (result = " << res.as_int() << ")" << std::endl;
  }

  // Test 5: Scheme (gc) primitive invocation
  {
    VM vm;
    eval_scheme(vm, "(gc)");
    Value res = eval_scheme(vm, "(begin (gc) (+ 40 2))");
    assert(res.as_int() == 42);
    std::cout << "  [PASS] Scheme (gc) primitive executes cleanly" << std::endl;
  }

  std::cout << "\n✨ ALL GC MARK-AND-SWEEP UNIT & STRESS TESTS PASSED! ✨\n" << std::endl;
  return 0;
}
