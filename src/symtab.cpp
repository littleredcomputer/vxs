//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// symtab.cpp : symbol table with transparent string_view hashing in an unordered map.

#include "vx-scheme.h"
#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
  size_t operator()(const std::string &s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
  size_t operator()(const char *s) const noexcept {
    return std::hash<std::string_view>{}(s);
  }
};

struct StringEqual {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

using SymtabMap = std::unordered_map<std::string, std::unique_ptr<symbol>, StringHash, StringEqual>;

static SymtabMap &get_symtab() {
  static SymtabMap symtab;
  return symtab;
}

psymbol intern_stet(std::string_view name) {
  auto &symtab = get_symtab();
  auto it = symtab.find(name);
  if (it != symtab.end())
    return it->second.get();

  auto sym = std::make_unique<symbol>();
  sym->name = std::string(name);
  sym->key = sym->name.c_str();
  sym->truename = sym->key;
  psymbol ptr = sym.get();
  symtab.emplace(sym->name, std::move(sym));
  return ptr;
}

psymbol intern(std::string_view name) {
  bool is_all_lower = true;
  for (char c : name) {
    if (std::isupper(static_cast<unsigned char>(c))) {
      is_all_lower = false;
      break;
    }
  }

  if (is_all_lower)
    return intern_stet(name);

  std::string lower;
  lower.reserve(name.size());
  for (char c : name)
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

  auto &symtab = get_symtab();
  auto it = symtab.find(lower);
  if (it != symtab.end()) {
    psymbol sym = it->second.get();
    if (sym->truename == sym->key) {
      sym->truename_str = std::string(name);
      sym->truename = sym->truename_str.c_str();
    }
    return sym;
  }

  auto sym = std::make_unique<symbol>();
  sym->name = std::move(lower);
  sym->truename_str = std::string(name);
  sym->key = sym->name.c_str();
  sym->truename = sym->truename_str.c_str();
  psymbol ptr = sym.get();
  symtab.emplace(sym->name, std::move(sym));
  return ptr;
}

psymbol intern(const char *name) {
  return intern(std::string_view(name));
}

psymbol intern(const std::string &name) {
  return intern(std::string_view(name));
}

psymbol intern_stet(const char *name) {
  return intern_stet(std::string_view(name));
}

psymbol intern_stet(const std::string &name) {
  return intern_stet(std::string_view(name));
}
