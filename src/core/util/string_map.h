#pragma once

#include <pointcaster/core.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pc {

// transparent hashing so lookups from a string_view don't allocate a key
struct PathHash {
  using is_transparent = void;
  [[nodiscard]] std::size_t operator()(std::string_view path) const noexcept {
    return std::hash<std::string_view>{}(path);
  }
};

// TODO this is a polyfill to get a std::string_view overload into
// the index operator for std::unordered_map until c++26
template <typename T>
struct StringMap
    : std::unordered_map<std::string, T, PathHash, std::equal_to<>> {
  using base = std::unordered_map<std::string, T, PathHash, std::equal_to<>>;
  using base::base;

  T &operator[](std::string_view key) {
    if (auto it = this->find(key); it != this->end()) return it->second;
    return this->try_emplace(std::string{key}).first->second;
  }
};

} // namespace pc
