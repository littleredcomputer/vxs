#include "../src/vx_value.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <limits>
#include <cassert>

using namespace vxs;

struct TestDummy {
  int x;
  double y;
};

int main() {
  std::cout << "=== RUNNING VX-VALUE (NAN-BOXING) COMPREHENSIVE TESTS ===" << std::endl;

  // 1. Float tests
  {
    std::vector<double> test_floats = {
      0.0, -0.0, 1.0, -1.0, 3.141592653589793, 2.718281828459045,
      1e-300, 1e300, -1e300,
      std::numeric_limits<double>::min(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity()
    };

    for (double d : test_floats) {
      Value v = Value::from_double(d);
      assert(v.is_double());
      assert(v.is_number());
      assert(!v.is_int());
      assert(!v.is_ptr());
      assert(!v.is_bool());
      assert(!v.is_symbol());
      assert(!v.is_nil());
      if (std::isnan(d)) {
        assert(std::isnan(v.as_double()));
      } else {
        assert(v.as_double() == d || (d == 0.0 && v.as_double() == 0.0));
      }
      assert(v.as_real() == v.as_double());
    }
    std::cout << "  [PASS] Floating point full IEEE-754 precision & extremes" << std::endl;
  }

  // 2. Integer tests
  {
    std::vector<int32_t> test_ints = {
      0, 1, -1, 42, -42, 1000000, -1000000,
      std::numeric_limits<int32_t>::max(),
      std::numeric_limits<int32_t>::min()
    };

    for (int32_t i : test_ints) {
      Value v = Value::from_int(i);
      assert(v.is_int());
      assert(v.is_number());
      assert(!v.is_double());
      assert(!v.is_ptr());
      assert(!v.is_bool());
      assert(!v.is_symbol());
      assert(!v.is_nil());
      assert(v.as_int() == i);
      assert(v.as_real() == static_cast<double>(i));
    }
    std::cout << "  [PASS] 32-bit signed integers (positive, negative, min/max)" << std::endl;
  }

  // 3. Pointer tests
  {
    std::vector<TestDummy*> dummies;
    for (int i = 0; i < 100; ++i) {
      dummies.push_back(new TestDummy{i, i * 1.5});
    }

    for (TestDummy *ptr : dummies) {
      Value v = Value::from_ptr(ptr);
      assert(v.is_ptr());
      assert(!v.is_int());
      assert(!v.is_double());
      assert(!v.is_number());
      assert(!v.is_bool());
      assert(!v.is_symbol());
      assert(!v.is_nil());
      TestDummy *recovered = v.as_ptr<TestDummy>();
      assert(recovered == ptr);
      assert(recovered->x == ptr->x);
      assert(recovered->y == ptr->y);
    }

    for (TestDummy *ptr : dummies) delete ptr;
    std::cout << "  [PASS] 48-bit heap pointers roundtrip exact object addresses" << std::endl;
  }

  // 4. Symbol tests
  {
    for (uint32_t sym_id : { 0u, 1u, 42u, 65535u, 1000000u }) {
      Value v = Value::from_symbol_id(sym_id);
      assert(v.is_symbol());
      assert(!v.is_int());
      assert(!v.is_double());
      assert(!v.is_ptr());
      assert(!v.is_bool());
      assert(!v.is_nil());
      assert(v.as_symbol_id() == sym_id);
    }
    std::cout << "  [PASS] 32-bit unboxed symbol identifiers" << std::endl;
  }

  // 5. Booleans and singletons
  {
    Value vt = Value::boolean_true();
    Value vf = Value::boolean_false();
    Value vnil = Value::nil();
    Value vund = Value::unspecified();
    Value veof = Value::eof_obj();

    assert(vt.is_bool() && vt.is_true() && !vt.is_false() && vt.as_bool() == true);
    assert(vf.is_bool() && !vf.is_true() && vf.is_false() && vf.as_bool() == false);
    assert(vnil.is_nil() && !vnil.is_bool() && !vnil.is_ptr());
    assert(vund.is_unspecified());
    assert(veof.is_eof());

    // Scheme truthiness: Everything except #f is true!
    assert(vt.is_true());
    assert(vnil.is_true());
    assert(Value::from_int(0).is_true()); // 0 is truthy in Scheme!
    assert(!vf.is_true());

    std::cout << "  [PASS] Booleans, Nil, Unspecified, EOF & Scheme truthiness semantics" << std::endl;
  }

  std::cout << "\n✨ ALL NAN-BOX VALUE UNIT TESTS PASSED WITH 100% ACCURACY! ✨\n" << std::endl;
  return 0;
}
