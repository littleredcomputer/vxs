#include "../src/vx_vm.h"
#include <iostream>
#include <cassert>

using namespace vxs;

int main() {
  std::cout << "=== RUNNING VX-VM (BYTECODE & FIBERS) UNIT TESTS ===" << std::endl;

  VM vm;

  // 1. Simple arithmetic bytecode test: (+ 123 456)
  {
    BytecodeChunk chunk;
    chunk.constants.push_back(Value::from_int(123));
    chunk.constants.push_back(Value::from_int(456));

    // Bytecode: CONST 0, CONST 1, ADD, RETURN
    chunk.code = {
      OP_CONST, 0, 0,
      OP_CONST, 0, 1,
      OP_ADD,
      OP_RETURN
    };

    ObjClosure *closure = vm.heap.allocate<ObjClosure>(&chunk, 0, false);
    Fiber fiber;
    fiber.push(Value::from_ptr(closure));
    fiber.frames.push_back({closure, chunk.code.data(), 0});

    VM::StepResult res = vm.step_fiber(fiber);
    assert(res == VM::StepResult::Completed);
    assert(fiber.result.as_int() == 579);
    std::cout << "  [PASS] Direct bytecode execution: (+ 123 456) => 579" << std::endl;
  }

  // 2. Global Subr Call: (sin 0.0)
  {
    BytecodeChunk chunk;
    uint32_t sin_id = vm.intern("sin");
    chunk.constants.push_back(Value::from_symbol_id(sin_id));
    chunk.constants.push_back(Value::from_double(0.0));

    // Bytecode: GET_GLOBAL "sin", CONST 0.0, CALL 1, RETURN
    chunk.code = {
      OP_GET_GLOBAL, 0, 0,
      OP_CONST, 0, 1,
      OP_CALL, 1,
      OP_RETURN
    };

    ObjClosure *closure = vm.heap.allocate<ObjClosure>(&chunk, 0, false);
    Fiber fiber;
    fiber.push(Value::from_ptr(closure));
    fiber.frames.push_back({closure, chunk.code.data(), 0});

    VM::StepResult res = vm.step_fiber(fiber);
    assert(res == VM::StepResult::Completed);
    assert(fiber.result.as_double() == 0.0);
    std::cout << "  [PASS] Native Subr call dispatch: (sin 0.0) => 0.0" << std::endl;
  }

  // 3. Coroutine Fiber Stepping with Yield
  {
    BytecodeChunk chunk;
    chunk.constants.push_back(Value::from_int(100));
    chunk.constants.push_back(Value::from_int(200));

    // Bytecode: CONST 100, YIELD, CONST 200, ADD, RETURN
    chunk.code = {
      OP_CONST, 0, 0,
      OP_YIELD,
      OP_CONST, 0, 1,
      OP_ADD,
      OP_RETURN
    };

    ObjClosure *closure = vm.heap.allocate<ObjClosure>(&chunk, 0, false);
    Fiber fiber;
    fiber.push(Value::from_ptr(closure));
    fiber.frames.push_back({closure, chunk.code.data(), 0});

    // Step 1: Should yield after pushing 100
    VM::StepResult res1 = vm.step_fiber(fiber, 2);
    assert(res1 == VM::StepResult::Yielded);
    assert(fiber.state == Fiber::State::Suspended);
    assert(fiber.stack.size() == 2); // callee + 100

    // Step 2: Resume and complete
    VM::StepResult res2 = vm.step_fiber(fiber, 10);
    assert(res2 == VM::StepResult::Completed);
    assert(fiber.result.as_int() == 300);
    std::cout << "  [PASS] Cooperative fiber stepping across Yield boundaries" << std::endl;
  }

  std::cout << "\n✨ ALL BYTECODE VM & FIBER TESTS PASSED! ✨\n" << std::endl;
  return 0;
}
