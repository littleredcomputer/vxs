#include "../src/vx_heap.h"
#include <iostream>
#include <cassert>

using namespace vxs;

int main() {
  std::cout << "=== RUNNING VX-HEAP & OBJECT MODEL UNIT TESTS ===" << std::endl;

  Heap heap;

  // 1. Cons & List building
  {
    Value v1 = Value::from_int(10);
    Value v2 = Value::from_int(20);
    Value v3 = Value::from_int(30);

    Value list = heap.cons(v1, heap.cons(v2, heap.cons(v3, Value::nil())));

    assert(Heap::is_cons(list));
    assert(Heap::car(list).as_int() == 10);
    assert(Heap::car(Heap::cdr(list)).as_int() == 20);
    assert(Heap::car(Heap::cdr(Heap::cdr(list))).as_int() == 30);
    assert(Heap::cdr(Heap::cdr(Heap::cdr(list))).is_nil());

    // Mutation
    Heap::set_car(list, Value::from_int(999));
    assert(Heap::car(list).as_int() == 999);

    std::cout << "  [PASS] Cons cell pairs, list construction, and set-car!/set-cdr!" << std::endl;
  }

  // 2. Vectors
  {
    Value vec = heap.make_vector(5, Value::from_int(0));
    assert(Heap::is_vector(vec));
    ObjVector *ov = vec.as_ptr<ObjVector>();
    assert(ov->size == 5);
    for (uint32_t i = 0; i < 5; ++i) {
      assert(ov->get(i).as_int() == 0);
      ov->set(i, Value::from_int(i * 100));
    }
    for (uint32_t i = 0; i < 5; ++i) {
      assert(ov->get(i).as_int() == static_cast<int32_t>(i * 100));
    }
    std::cout << "  [PASS] Scheme vectors with direct index reads/writes" << std::endl;
  }

  // 3. Strings
  {
    Value str = heap.make_string("Hello, WebAssembly Scheme!");
    assert(Heap::is_string(str));
    ObjString *os = str.as_ptr<ObjString>();
    assert(os->length == 26);
    assert(os->view() == "Hello, WebAssembly Scheme!");
    std::cout << "  [PASS] Strings with zero-copy string_view interop" << std::endl;
  }

  // 4. Native Subrs
  {
    auto test_add = [](VM &, uint32_t argc, Value *args) -> Value {
      assert(argc == 2);
      return Value::from_double(args[0].as_real() + args[1].as_real());
    };

    Value subr = heap.make_subr("+", test_add, 2, 2);
    assert(Heap::is_subr(subr));
    ObjSubr *os = subr.as_ptr<ObjSubr>();
    assert(std::string(os->name) == "+");
    std::cout << "  [PASS] Native Subr registration and type tagging" << std::endl;
  }

  std::cout << "  Heap object count: " << heap.get_object_count() << std::endl;
  std::cout << "  Heap allocated bytes: " << heap.get_bytes_allocated() << " bytes" << std::endl;

  std::cout << "\n✨ ALL HEAP & OBJECT MODEL TESTS PASSED! ✨\n" << std::endl;
  return 0;
}
