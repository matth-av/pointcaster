#include "config_registry.h"

#include <algorithm>
#include <core/logger/logger.h>
#include <set>
#include <string>
#include <variant>

namespace pc {

void ConfigRegistry::register_field(std::string path, Field field) {
  std::unique_lock lock(_mutex);
  _fields.insert_or_assign(std::move(path), std::move(field));
}

void ConfigRegistry::unregister(std::string_view prefix) {
  std::unique_lock lock(_mutex);
  std::erase_if(_fields,
                [&](const auto &kv) { return kv.first.starts_with(prefix); });
}

void ConfigRegistry::clear() {
  std::unique_lock lock(_mutex);
  _fields.clear();
}

bool ConfigRegistry::set(std::string_view path, ConfigValue value) {
  std::function<void(ConfigValue)> setter;
  {
    std::shared_lock lock(_mutex);
    auto it = _fields.find(path);
    if (it == _fields.end()) {
      return false;
    }
    setter = it->second.set;
  }
  if (!setter) return false;
  setter(value);
  notify(path);
  return true;
}

bool ConfigRegistry::is_readonly(std::string_view path) const {
  std::shared_lock lock(_mutex);
  auto it = _fields.find(path);
  return it != _fields.end() && !it->second.set;
}

std::optional<ConfigValue> ConfigRegistry::get(std::string_view path) const {
  std::shared_lock lock(_mutex);
  auto it = _fields.find(path);
  if (it == _fields.end()) return std::nullopt;
  return it->second.get();
}

template <typename StringCollection>
void ConfigRegistry::snapshot(const StringCollection &paths,
                              StringMap<ConfigValue> &out) {
  out.clear();
  out.reserve(std::size(paths));
  std::shared_lock lock(_mutex);
  for (const auto &path : paths) {
    const auto it = _fields.find(path);
    if (it == _fields.end()) continue;
    out.emplace(it->first, it->second.get());
  }
}

template POINTCASTER_CONFIG_EXPORT void
ConfigRegistry::snapshot<std::set<std::string>>(
    const std::set<std::string> &paths, StringMap<ConfigValue> &out);

ConfigRegistry::SubscriptionId
ConfigRegistry::on_change(std::string_view prefix, ChangeCallback cb) {
  std::scoped_lock lock(_mutex);
  const auto id = _next_subscription_id++;
  _subs.push_back(Subscription{id, std::string(prefix), std::move(cb)});
  return id;
}

void ConfigRegistry::remove_subscription(SubscriptionId id) {
  std::scoped_lock lock(_mutex);
  std::erase_if(_subs, [&](const auto &sub) { return sub.id == id; });
}

void ConfigRegistry::notify(std::string_view path) {
  std::vector<ChangeCallback> matching;
  {
    std::shared_lock lock(_mutex);
    for (const auto &sub : _subs) {
      if (path.starts_with(sub.prefix)) matching.push_back(sub.callback);
    }
  }
  for (auto &cb : matching) cb(path);
}

} // namespace pc