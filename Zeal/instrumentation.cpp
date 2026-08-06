#include "instrumentation.h"

// API only - no sdk/ or exporters/ headers. See instrumentation.h.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

#include "opentelemetry/metrics/provider.h"

#pragma pop_macro("max")
#pragma pop_macro("min")

#include <mutex>
#include <utility>
#include <vector>

namespace metrics_api = opentelemetry::metrics;

namespace {

// The instrumentation scope: name and version identify *what produced the telemetry*, which is how
// a consumer tells our measurements from anything else in the process and reports problems against
// the right thing.
constexpr const char *kScopeName = "zeal.everquest";
constexpr const char *kScopeVersion = "1.0.0";

std::mutex g_mutex;
std::string g_character;

using Counter = opentelemetry::nostd::shared_ptr<metrics_api::Counter<uint64_t>>;

// Created on first use. If no SDK is registered the API hands back a no-op instrument, which is
// valid to call - so this never needs a null check at the call site.
Counter &DamageCounter() {
  static Counter counter = metrics_api::Provider::GetMeterProvider()
                               ->GetMeter(kScopeName, kScopeVersion)
                               ->CreateUInt64Counter("everquest.combat.damage",
                                                     "damage dealt or taken", "{hitpoint}");
  return counter;
}

Counter &HealCounter() {
  static Counter counter = metrics_api::Provider::GetMeterProvider()
                               ->GetMeter(kScopeName, kScopeVersion)
                               ->CreateUInt64Counter("everquest.combat.heal",
                                                     "hitpoints restored", "{hitpoint}");
  return counter;
}

using Attributes = std::vector<std::pair<std::string, opentelemetry::common::AttributeValue>>;

void Add(Counter &counter, long long amount, Attributes &attrs) {
  if (amount <= 0) return;
  std::string character;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    character = g_character;
  }
  if (!character.empty()) attrs.push_back({"everquest.character.name", character.c_str()});
  counter->Add(static_cast<uint64_t>(amount),
               opentelemetry::common::KeyValueIterableView<Attributes>(attrs));
}

}  // namespace

namespace zeal::instrumentation {

void SetCharacter(const std::string &name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_character = name;
}

void RecordDamage(const std::string &source, const std::string &source_type, const std::string &direction,
                  const std::string &damage_type, const std::string &zone, const std::string &target,
                  const std::string &group_leader, long long amount) {
  Attributes attrs = {
      {"everquest.combat.source", source.c_str()},
      {"everquest.combat.source.type", source_type.c_str()},
      {"everquest.combat.direction", direction.c_str()},
      {"everquest.combat.damage.type", damage_type.c_str()},
      {"everquest.zone.name", zone.c_str()},
      {"everquest.combat.target", target.c_str()},
  };
  if (!group_leader.empty()) attrs.push_back({"everquest.group.leader", group_leader.c_str()});
  Add(DamageCounter(), amount, attrs);
}

void RecordHeal(const std::string &source, const std::string &direction, const std::string &zone,
                const std::string &group_leader, long long amount) {
  Attributes attrs = {
      {"everquest.combat.source", source.c_str()},
      {"everquest.combat.direction", direction.c_str()},
      {"everquest.zone.name", zone.c_str()},
  };
  if (!group_leader.empty()) attrs.push_back({"everquest.group.leader", group_leader.c_str()});
  Add(HealCounter(), amount, attrs);
}

}  // namespace zeal::instrumentation
