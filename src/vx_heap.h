#pragma once

#include "vx_value.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <string_view>
#include <functional>

namespace vxs {

// Forward declarations
struct VM;
struct Fiber;

enum class ObjType : uint8_t {
  Cons,
  Vector,
  String,
  Symbol,
  Closure,
  Subr,
  Fiber,
  Future,
  Map
};

// Base object header for all heap-allocated objects
struct Obj {
  ObjType type;
  bool gc_mark;
  uint16_t flags;
  Obj *next_all; // Intrusive linked list of all heap objects

  inline explicit Obj(ObjType t)
      : type(t), gc_mark(false), flags(0), next_all(nullptr) {}

  template <typename T>
  inline bool is() const {
    return type == T::TYPE_TAG;
  }

  template <typename T>
  inline T *as() {
    assert(is<T>() && "Object type mismatch");
    return static_cast<T *>(this);
  }

  template <typename T>
  inline const T *as() const {
    assert(is<T>() && "Object type mismatch");
    return static_cast<const T *>(this);
  }
};

//-----------------------------------------------------------------------------
// 1. Cons Cell
//-----------------------------------------------------------------------------
struct ObjCons : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Cons;
  Value car;
  Value cdr;

  inline ObjCons(Value a, Value d)
      : Obj(ObjType::Cons), car(a), cdr(d) {}
};

//-----------------------------------------------------------------------------
// 2. Vector
//-----------------------------------------------------------------------------
struct ObjVector : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Vector;
  uint32_t size;
  Value *data;

  inline ObjVector(uint32_t sz, Value fill)
      : Obj(ObjType::Vector), size(sz), data(nullptr) {
    if (size > 0) {
      data = static_cast<Value *>(std::malloc(size * sizeof(Value)));
      for (uint32_t i = 0; i < size; ++i) data[i] = fill;
    }
  }

  inline ~ObjVector() {
    if (data) std::free(data);
  }

  inline Value get(uint32_t ix) const {
    assert(ix < size && "Vector index out of bounds");
    return data[ix];
  }

  inline void set(uint32_t ix, Value v) {
    assert(ix < size && "Vector index out of bounds");
    data[ix] = v;
  }
};

//-----------------------------------------------------------------------------
// 3. String
//-----------------------------------------------------------------------------
struct ObjString : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::String;
  uint32_t length;
  char *chars;

  inline ObjString(const char *s, uint32_t len)
      : Obj(ObjType::String), length(len), chars(nullptr) {
    chars = static_cast<char *>(std::malloc(len + 1));
    std::memcpy(chars, s, len);
    chars[len] = '\0';
  }

  inline explicit ObjString(std::string_view sv)
      : ObjString(sv.data(), static_cast<uint32_t>(sv.size())) {}

  inline ~ObjString() {
    if (chars) std::free(chars);
  }

  inline std::string_view view() const {
    return std::string_view(chars, length);
  }
};

//-----------------------------------------------------------------------------
// 4. Symbol
//-----------------------------------------------------------------------------
struct ObjSymbol : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Symbol;
  uint32_t id;
  const char *name; // Points to interned string

  inline ObjSymbol(uint32_t sym_id, const char *s_name)
      : Obj(ObjType::Symbol), id(sym_id), name(s_name) {}
};

//-----------------------------------------------------------------------------
// 5. C++ Native Subr (Primitive Function)
//-----------------------------------------------------------------------------
typedef Value (*NativeSubrFn)(VM &vm, uint32_t argc, Value *args);

struct ObjSubr : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Subr;
  const char *name;
  NativeSubrFn fn;
  uint32_t min_args;
  uint32_t max_args; // UINT32_MAX for variadic

  inline ObjSubr(const char *n, NativeSubrFn f, uint32_t min_a, uint32_t max_a)
      : Obj(ObjType::Subr), name(n), fn(f), min_args(min_a), max_args(max_a) {}
};

//-----------------------------------------------------------------------------
// 6. Bytecode Closure
//-----------------------------------------------------------------------------
struct BytecodeChunk {
  std::vector<uint8_t> code;
  std::vector<Value> constants;
  std::vector<uint32_t> lines;
};

struct ObjClosure : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Closure;
  BytecodeChunk *chunk;
  uint32_t arity;
  bool is_variadic;
  uint32_t env_size;
  uint32_t max_locals;
  Value *env; // Captured closure environment

  inline ObjClosure(BytecodeChunk *ch, uint32_t ar, bool var, uint32_t e_sz = 0, uint32_t mx_loc = 1)
      : Obj(ObjType::Closure), chunk(ch), arity(ar), is_variadic(var),
        env_size(e_sz), max_locals(mx_loc), env(nullptr) {
    if (env_size > 0) {
      env = static_cast<Value *>(std::malloc(env_size * sizeof(Value)));
      for (uint32_t i = 0; i < env_size; ++i) env[i] = Value::unspecified();
    }
  }

  inline ~ObjClosure() {
    if (env) std::free(env);
  }
};

//-----------------------------------------------------------------------------
// 7. Future & Fiber Concurrency
//-----------------------------------------------------------------------------
// 7. Future & Fiber Concurrency
//-----------------------------------------------------------------------------
struct ObjFuture : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Future;
  Fiber *fiber;
  Value result;
  bool is_completed;

  inline explicit ObjFuture(Fiber *f)
      : Obj(ObjType::Future), fiber(f), result(Value::unspecified()), is_completed(false) {}
};

//-----------------------------------------------------------------------------
// 8. Associative Map (Key-Value Dict)
//-----------------------------------------------------------------------------
struct ObjMap : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Map;
  std::vector<std::pair<Value, Value>> entries;

  inline ObjMap() : Obj(ObjType::Map) {}
  inline explicit ObjMap(std::vector<std::pair<Value, Value>> kvs)
      : Obj(ObjType::Map), entries(std::move(kvs)) {}

  inline Value get(Value key, Value default_val = Value::nil()) const {
    for (const auto &p : entries) {
      if (p.first == key) return p.second;
    }
    return default_val;
  }

  inline void set(Value key, Value val) {
    for (auto &p : entries) {
      if (p.first == key) {
        p.second = val;
        return;
      }
    }
    entries.push_back({key, val});
  }

  inline bool has(Value key) const {
    for (const auto &p : entries) {
      if (p.first == key) return true;
    }
    return false;
  }
};

//=============================================================================
// Heap & Slab Allocator
//=============================================================================
class Heap {
public:
  Heap() : head_obj(nullptr), bytes_allocated(0), gc_threshold(1024 * 1024) {}

  ~Heap() {
    free_all();
  }

  // Allocation helpers
  inline Value cons(Value car, Value cdr) {
    ObjCons *c = allocate<ObjCons>(car, cdr);
    return Value::from_ptr(c);
  }

  inline Value make_vector(uint32_t size, Value fill = Value::unspecified()) {
    ObjVector *v = allocate<ObjVector>(size, fill);
    return Value::from_ptr(v);
  }

  inline Value make_map(std::vector<std::pair<Value, Value>> entries = {}) {
    ObjMap *m = allocate<ObjMap>(std::move(entries));
    return Value::from_ptr(m);
  }

  inline Value make_string(std::string_view sv) {
    ObjString *s = allocate<ObjString>(sv);
    return Value::from_ptr(s);
  }

  inline Value make_subr(const char *name, NativeSubrFn fn, uint32_t min_a, uint32_t max_a) {
    ObjSubr *subr = allocate<ObjSubr>(name, fn, min_a, max_a);
    return Value::from_ptr(subr);
  }

  inline Value make_closure(BytecodeChunk *chunk, uint32_t arity, bool is_variadic, uint32_t env_size = 0, uint32_t max_locals = 1) {
    ObjClosure *cl = allocate<ObjClosure>(chunk, arity, is_variadic, env_size, max_locals);
    return Value::from_ptr(cl);
  }

  inline Value make_future(Fiber *fiber) {
    ObjFuture *fut = allocate<ObjFuture>(fiber);
    return Value::from_ptr(fut);
  }

  // Fast object accessors
  static inline bool is_cons(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Cons;
  }

  static inline bool is_vector(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Vector;
  }

  static inline bool is_map(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Map;
  }

  static inline bool is_string(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::String;
  }

  static inline bool is_closure(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Closure;
  }

  static inline bool is_subr(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Subr;
  }

  static inline bool is_future(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Future;
  }

  static inline Value car(Value v) {
    if (!is_cons(v)) return Value::nil();
    return v.as_ptr<ObjCons>()->car;
  }

  static inline Value cdr(Value v) {
    if (!is_cons(v)) return Value::nil();
    return v.as_ptr<ObjCons>()->cdr;
  }

  static inline void set_car(Value v, Value ncar) {
    if (is_cons(v)) v.as_ptr<ObjCons>()->car = ncar;
  }

  static inline void set_cdr(Value v, Value ncdr) {
    if (is_cons(v)) v.as_ptr<ObjCons>()->cdr = ncdr;
  }

  // Tracking
  size_t get_bytes_allocated() const { return bytes_allocated; }
  size_t get_gc_threshold() const { return gc_threshold; }
  void set_gc_threshold(size_t t) { gc_threshold = t; }
  size_t get_object_count() const {
    size_t count = 0;
    for (Obj *cur = head_obj; cur; cur = cur->next_all) ++count;
    return count;
  }

  // Direct allocator
  template <typename T, typename... Args>
  inline T *allocate(Args &&...args) {
    void *mem = std::malloc(sizeof(T));
    T *obj = new (mem) T(std::forward<Args>(args)...);
    obj->next_all = head_obj;
    head_obj = obj;
    bytes_allocated += sizeof(T);
    return obj;
  }

private:

  void free_all() {
    Obj *cur = head_obj;
    while (cur) {
      Obj *next = cur->next_all;
      destroy_obj(cur);
      cur = next;
    }
    head_obj = nullptr;
    bytes_allocated = 0;
  }

  void destroy_obj(Obj *obj) {
    switch (obj->type) {
      case ObjType::Cons:    static_cast<ObjCons*>(obj)->~ObjCons(); break;
      case ObjType::Vector:  static_cast<ObjVector*>(obj)->~ObjVector(); break;
      case ObjType::String:  static_cast<ObjString*>(obj)->~ObjString(); break;
      case ObjType::Symbol:  static_cast<ObjSymbol*>(obj)->~ObjSymbol(); break;
      case ObjType::Closure: static_cast<ObjClosure*>(obj)->~ObjClosure(); break;
      case ObjType::Subr:    static_cast<ObjSubr*>(obj)->~ObjSubr(); break;
      case ObjType::Fiber:   break;
      case ObjType::Future:  static_cast<ObjFuture*>(obj)->~ObjFuture(); break;
      case ObjType::Map:     static_cast<ObjMap*>(obj)->~ObjMap(); break;
    }
    std::free(obj);
  }

  Obj *head_obj;
  size_t bytes_allocated;
  size_t gc_threshold;
};

} // namespace vxs
