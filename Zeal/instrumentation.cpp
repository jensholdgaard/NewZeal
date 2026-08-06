#include "instrumentation.h"

#include "everquest_semconv.h"  // generated from the registry: a typo becomes a compile error

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

// What the observable gauges report. Sampled on the game thread, read on the SDK's collection
// thread - which is exactly why asynchronous instruments suit these: the value is behind an
// accessor, so it is observed when the SDK asks rather than polled on a timer of our own.
struct CharacterState {
  std::string zone;
  long long attack = 0;
  long long haste = 0;
  bool have_attack = false;
  bool have_haste = false;
  bool valid = false;
};
CharacterState g_state;

using Counter = opentelemetry::nostd::shared_ptr<metrics_api::Counter<uint64_t>>;

// Created on first use. If no SDK is registered the API hands back a no-op instrument, which is
// valid to call - so this never needs a null check at the call site.
Counter &DamageCounter() {
  static Counter counter = metrics_api::Provider::GetMeterProvider()
                               ->GetMeter(kScopeName, kScopeVersion)
                               ->CreateUInt64Counter(everquest_semconv::kEverquestCombatDamageMetric,
                                                     "damage dealt or taken", "{hitpoint}");
  return counter;
}

Counter &HealCounter() {
  static Counter counter = metrics_api::Provider::GetMeterProvider()
                               ->GetMeter(kScopeName, kScopeVersion)
                               ->CreateUInt64Counter(everquest_semconv::kEverquestCombatHealMetric,
                                                     "hitpoints restored", "{hitpoint}");
  return counter;
}

using Attributes = std::vector<std::pair<std::string, opentelemetry::common::AttributeValue>>;

// Observing nothing is meaningful: an asynchronous instrument only exports the attribute sets its
// callback reports, so a character who is not in game simply stops producing points rather than
// flatlining at a stale value.
void ObserveGauge(opentelemetry::metrics::ObserverResult result, bool haste) {
  CharacterState state;
  std::string character;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    state = g_state;
    character = g_character;
  }
  if (!state.valid || character.empty()) return;
  if (haste ? !state.have_haste : !state.have_attack) return;

  Attributes attrs = {
      {everquest_semconv::kEverquestCharacterName, character.c_str()},
      {everquest_semconv::kEverquestZoneName, state.zone.c_str()},
  };
  auto observer = opentelemetry::nostd::get<
      opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<int64_t>>>(result);
  observer->Observe(haste ? state.haste : state.attack,
                    opentelemetry::common::KeyValueIterableView<Attributes>(attrs));
}

void AttackCallback(opentelemetry::metrics::ObserverResult result, void *) { ObserveGauge(result, false); }
void HasteCallback(opentelemetry::metrics::ObserverResult result, void *) { ObserveGauge(result, true); }

// Registered once. Holding the instruments alive matters: dropping them would unregister the
// callbacks and the gauges would silently stop reporting.
void EnsureGauges() {
  static auto attack = [] {
    auto g = metrics_api::Provider::GetMeterProvider()
                 ->GetMeter(kScopeName, kScopeVersion)
                 ->CreateInt64ObservableGauge(everquest_semconv::kEverquestCharacterAttackMetric,
                                              "current offense rating", "1");
    g->AddCallback(AttackCallback, nullptr);
    return g;
  }();
  static auto haste = [] {
    auto g = metrics_api::Provider::GetMeterProvider()
                 ->GetMeter(kScopeName, kScopeVersion)
                 ->CreateInt64ObservableGauge(everquest_semconv::kEverquestCharacterHasteMetric,
                                              "total effective melee haste", "%");
    g->AddCallback(HasteCallback, nullptr);
    return g;
  }();
  (void)attack;
  (void)haste;
}

void Add(Counter &counter, long long amount, Attributes &attrs) {
  if (amount <= 0) return;
  std::string character;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    character = g_character;
  }
  if (!character.empty()) attrs.push_back({everquest_semconv::kEverquestCharacterName, character.c_str()});
  counter->Add(static_cast<uint64_t>(amount),
               opentelemetry::common::KeyValueIterableView<Attributes>(attrs));
}

}  // namespace

namespace zeal::instrumentation {

void SetCharacter(const std::string &name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_character = name;
}

void SetCharacterState(const std::string &zone, long long attack, bool have_attack, long long haste,
                       bool have_haste) {
  EnsureGauges();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_state.zone = zone;
  g_state.attack = attack;
  g_state.have_attack = have_attack;
  g_state.haste = haste;
  g_state.have_haste = have_haste;
  g_state.valid = true;
}

void RecordDamage(const std::string &source, const std::string &source_type, const std::string &direction,
                  const std::string &damage_type, const std::string &zone, const std::string &target,
                  const std::string &group_leader, long long amount) {
  Attributes attrs = {
      {everquest_semconv::kEverquestCombatSource, source.c_str()},
      {everquest_semconv::kEverquestCombatSourceType, source_type.c_str()},
      {everquest_semconv::kEverquestCombatDirection, direction.c_str()},
      {everquest_semconv::kEverquestCombatDamageType, damage_type.c_str()},
      {everquest_semconv::kEverquestZoneName, zone.c_str()},
      {everquest_semconv::kEverquestCombatTarget, target.c_str()},
  };
  if (!group_leader.empty()) attrs.push_back({everquest_semconv::kEverquestGroupLeader, group_leader.c_str()});
  Add(DamageCounter(), amount, attrs);
}

void RecordHeal(const std::string &source, const std::string &direction, const std::string &zone,
                const std::string &group_leader, long long amount) {
  Attributes attrs = {
      {everquest_semconv::kEverquestCombatSource, source.c_str()},
      {everquest_semconv::kEverquestCombatDirection, direction.c_str()},
      {everquest_semconv::kEverquestZoneName, zone.c_str()},
  };
  if (!group_leader.empty()) attrs.push_back({everquest_semconv::kEverquestGroupLeader, group_leader.c_str()});
  Add(HealCounter(), amount, attrs);
}

}  // namespace zeal::instrumentation
