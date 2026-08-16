#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace vxs {

//=============================================================================
// 64-Bit NaN-Boxed Value Representation
//
// Layout of 64-bit words:
//
// Double:   Any 64-bit IEEE-754 float where (bits & QNAN_MASK) != QNAN_MASK
//
// Tags (Upper 16 bits = 0xFFF8..0xFFFF):
// 0xFFF8: NIL          (empty list '())
// 0xFFF9: FALSE        (#f)
// 0xFFFA: TRUE         (#t)
// 0xFFFB: UNSPECIFIED  (#<unspecified>)
// 0xFFFC: EOF_OBJ      (#<eof>)
// 0xFFFD: SYMBOL       (lower 32 bits = uint32_t symbol ID)
// 0xFFFE: INTEGER      (lower 32 bits = int32_t signed integer)
// 0xFFFF: POINTER      (lower 48 bits = heap Cell* address)
//=============================================================================

struct Value {
  uint64_t raw;

  // Constants
  static constexpr uint64_t QNAN_MASK = 0xFFF8000000000000ULL;
  static constexpr uint64_t TAG_MASK = 0xFFFF000000000000ULL;
  static constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;
  static constexpr uint64_t INT_MASK = 0x00000000FFFFFFFFULL;

  static constexpr uint64_t TAG_CHAR = 0xFFF7000000000000ULL;
  static constexpr uint64_t TAG_NIL = 0xFFF8000000000000ULL;
  static constexpr uint64_t TAG_FALSE = 0xFFF9000000000000ULL;
  static constexpr uint64_t TAG_TRUE = 0xFFFA000000000000ULL;
  static constexpr uint64_t TAG_UNDEF = 0xFFFB000000000000ULL;
  static constexpr uint64_t TAG_EOF = 0xFFFC000000000000ULL;
  static constexpr uint64_t TAG_SYMBOL = 0xFFFD000000000000ULL;
  static constexpr uint64_t TAG_INT = 0xFFFE000000000000ULL;
  static constexpr uint64_t TAG_PTR = 0xFFFF000000000000ULL;
  static constexpr uint64_t KEYWORD_BIT = 0x0000000080000000ULL;

  // Constructors
  constexpr Value() : raw(TAG_UNDEF) {}
  constexpr explicit Value(uint64_t r) : raw(r) {}

  // Double
  static inline Value from_double(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    // If it's a NaN, canonicalize to a positive quiet NaN so it doesn't collide
    // with tags
    if ((bits & 0x7FF8000000000000ULL) == 0x7FF0000000000000ULL &&
        (bits & 0x0007FFFFFFFFFFFFULL) != 0) {
      bits = 0x7FF8000000000000ULL;
    }
    return Value(bits);
  }

  // Integer
  static constexpr inline Value from_int(int32_t i) {
    return Value(TAG_INT | static_cast<uint64_t>(static_cast<uint32_t>(i)));
  }

  // Pointer
  template <typename T> static inline Value from_ptr(const T *ptr) {
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    assert((p & ~PTR_MASK) == 0 && "Pointer must fit in 48 bits");
    return Value(TAG_PTR | static_cast<uint64_t>(p));
  }

  // Symbol ID
  static constexpr inline Value from_symbol_id(uint32_t sym_id) {
    return Value(TAG_SYMBOL | (static_cast<uint64_t>(sym_id) & ~KEYWORD_BIT));
  }

  // Keyword ID
  static constexpr inline Value from_keyword_id(uint32_t kw_id) {
    return Value(TAG_SYMBOL | KEYWORD_BIT |
                 (static_cast<uint64_t>(kw_id) & ~KEYWORD_BIT));
  }

  // Character
  static constexpr inline Value from_char(char c) {
    return Value(TAG_CHAR | static_cast<uint64_t>(static_cast<uint8_t>(c)));
  }

  // Booleans
  static constexpr inline Value from_bool(bool b) {
    return b ? Value(TAG_TRUE) : Value(TAG_FALSE);
  }

  // Singletons
  static constexpr inline Value nil() { return Value(TAG_NIL); }
  static constexpr inline Value boolean_true() { return Value(TAG_TRUE); }
  static constexpr inline Value boolean_false() { return Value(TAG_FALSE); }
  static constexpr inline Value unspecified() { return Value(TAG_UNDEF); }
  static constexpr inline Value eof_obj() { return Value(TAG_EOF); }

  // Predicates
  inline bool is_double() const { return (raw & QNAN_MASK) != QNAN_MASK; }

  inline bool is_int() const { return (raw & TAG_MASK) == TAG_INT; }

  inline bool is_char() const { return (raw & TAG_MASK) == TAG_CHAR; }

  inline char as_char() const { return static_cast<char>(raw & 0xFF); }

  inline bool is_number() const { return is_int() || is_double(); }

  inline bool is_ptr() const { return (raw & TAG_MASK) == TAG_PTR; }

  inline bool is_symbol() const {
    return (raw & TAG_MASK) == TAG_SYMBOL && !(raw & KEYWORD_BIT);
  }

  inline bool is_keyword() const {
    return (raw & TAG_MASK) == TAG_SYMBOL && (raw & KEYWORD_BIT);
  }

  inline bool is_bool() const { return raw == TAG_FALSE || raw == TAG_TRUE; }

  inline bool is_true() const { return raw != TAG_FALSE; }

  inline bool is_false() const { return raw == TAG_FALSE; }

  inline bool is_nil() const { return raw == TAG_NIL; }

  inline bool is_unspecified() const { return raw == TAG_UNDEF; }

  inline bool is_eof() const { return raw == TAG_EOF; }

  // Unboxers
  inline double as_double() const {
    assert(is_double() && "Value is not a double");
    double d;
    std::memcpy(&d, &raw, sizeof(d));
    return d;
  }

  inline int32_t as_int() const {
    if (is_int())
      return static_cast<int32_t>(raw & INT_MASK);
    if (is_double())
      return static_cast<int32_t>(as_double());
    return 0;
  }

  inline double as_real() const {
    if (is_int())
      return static_cast<double>(as_int());
    if (is_double())
      return as_double();
    return 0.0;
  }

  template <typename T> inline T *as_ptr() const {
    assert(is_ptr() && "Value is not a pointer");
    return reinterpret_cast<T *>(raw & PTR_MASK);
  }

  inline uint32_t as_symbol_id() const {
    assert(is_symbol() && "Value is not a symbol");
    return static_cast<uint32_t>(raw & ~KEYWORD_BIT & INT_MASK);
  }

  inline uint32_t as_keyword_id() const {
    assert(is_keyword() && "Value is not a keyword");
    return static_cast<uint32_t>(raw & ~KEYWORD_BIT & INT_MASK);
  }

  inline bool as_bool() const {
    assert(is_bool() && "Value is not a boolean");
    return raw == TAG_TRUE;
  }

  // Equality
  inline bool operator==(const Value &o) const { return raw == o.raw; }

  inline bool operator!=(const Value &o) const { return raw != o.raw; }
};

static_assert(sizeof(Value) == 8, "Value must be exactly 8 bytes (64 bits)");
static_assert(std::is_trivially_copyable_v<Value>,
              "Value must be trivially copyable for register/memcpy speed");

} // namespace vxs
