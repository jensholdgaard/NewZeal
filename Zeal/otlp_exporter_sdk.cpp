#ifdef ZEAL_OTLP_SDK
// opentelemetry-cpp backed implementation of OtlpExporter — the A/B counterpart to the hand-rolled
// JSON exporter in otlp_exporter.cpp. Same commands, same metric names and attributes; the SDK owns
// serialization, batching, retry and the periodic export cadence (so no worker thread of our own).
//
// Built only when ZEAL_OTLP_SDK is defined (see .github/workflows/build-sdk.yml, which vcpkg-installs
// opentelemetry-cpp[otlp-http]:x86-windows-static).
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>

#include "otlp_exporter.h"

// Zeal relies on the Windows min/max macros elsewhere, but they break the otel/protobuf headers
// (std::min/std::max, numeric_limits::max()). Suppress them for these includes only, then restore.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/logs/provider.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_factory.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include "opentelemetry/sdk/resource/resource.h"

#pragma pop_macro("max")
#pragma pop_macro("min")

#include "callbacks.h"
#include "chat.h"
#include "commands.h"
#include "entity_manager.h"
#include "game_functions.h"
#include "game_structures.h"
#include "labels.h"
#include "string_util.h"
#include "zeal.h"

namespace metrics_api = opentelemetry::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace logs_api = opentelemetry::logs;
namespace logs_sdk = opentelemetry::sdk::logs;
namespace otlp = opentelemetry::exporter::otlp;
namespace nostd = opentelemetry::nostd;

namespace {
const char *class_name(int class_id) {
  switch (class_id) {
    case 1: return "Warrior";
    case 2: return "Cleric";
    case 3: return "Paladin";
    case 4: return "Ranger";
    case 5: return "Shadowknight";
    case 6: return "Druid";
    case 7: return "Monk";
    case 8: return "Bard";
    case 9: return "Rogue";
    case 10: return "Shaman";
    case 11: return "Necromancer";
    case 12: return "Wizard";
    case 13: return "Magician";
    case 14: return "Enchanter";
    case 15: return "Beastlord";
    default: return "Unknown";
  }
}

const char *melee_type_name(unsigned short skill) {
  using S = Zeal::GameEnums::SkillType;
  switch (skill) {
    case S::Skill1HSlashing:
    case S::Skill2HSlashing: return "slash";
    case S::Skill1HBlunt:
    case S::Skill2HBlunt: return "crush";
    case S::Skill1HPiercing: return "pierce";
    case S::SkillBash: return "bash";
    case S::SkillKick:
    case S::SkillFlyingKick:
    case S::SkillRoundKick: return "kick";
    case S::SkillHandtoHand: return "hth";
    case S::SkillBackstab: return "backstab";
    case S::SkillArchery: return "archery";
    default: return "melee";
  }
}

std::string current_zone_name() {
  Zeal::GameStructures::Entity *self = Zeal::Game::get_self();
  if (!Zeal::Game::is_in_game() || !self) return "";
  return Zeal::Game::get_full_zone_name(self->ZoneId);
}

long long read_number(const std::string &line, size_t pos) {
  size_t end = pos;
  while (end < line.size() && isdigit(static_cast<unsigned char>(line[end]))) end++;
  return (end > pos) ? atoll(line.substr(pos, end - pos).c_str()) : 0;
}
}  // namespace

// SDK objects live here so opentelemetry headers never leak into zeal.h consumers.
struct OtlpExporter::Impl {
  std::shared_ptr<metrics_api::MeterProvider> meter_provider;
  std::shared_ptr<logs_api::LoggerProvider> logger_provider;
  nostd::shared_ptr<metrics_api::Meter> meter;
  nostd::shared_ptr<logs_api::Logger> logger;
  nostd::unique_ptr<metrics_api::Counter<uint64_t>> damage;
  nostd::unique_ptr<metrics_api::Counter<uint64_t>> heal;
  nostd::shared_ptr<metrics_api::ObservableInstrument> attack_gauge;
  nostd::shared_ptr<metrics_api::ObservableInstrument> haste_gauge;
};

// Observable-gauge callbacks run on the SDK's reader thread, so they read ONLY the cached snapshot —
// never live game memory (same discipline as the hand-rolled build).
static void attack_callback(metrics_api::ObserverResult result, void *state);
static void haste_callback(metrics_api::ObserverResult result, void *state);

OtlpExporter::OtlpExporter(ZealService *zeal) : impl(std::make_unique<Impl>()) {
  const std::string base = setting_endpoint.get();

  auto resource = opentelemetry::sdk::resource::Resource::Create({
      {"service.name", "everquest"},
      {"service.version", ZEAL_VERSION},
      {"telemetry.sdk.name", "opentelemetry-cpp"},
  });

  // --- metrics ---
  otlp::OtlpHttpMetricExporterOptions mopts;
  mopts.url = base + "/v1/metrics";
  mopts.aggregation_temporality = opentelemetry::exporter::otlp::PreferredAggregationTemporality::kCumulative;
  auto mexporter = otlp::OtlpHttpMetricExporterFactory::Create(mopts);
  metrics_sdk::PeriodicExportingMetricReaderOptions ropts;
  ropts.export_interval_millis = std::chrono::milliseconds(setting_flush_ms.get() > 0 ? setting_flush_ms.get() : 2000);
  ropts.export_timeout_millis = std::chrono::milliseconds(5000);
  auto reader = metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(mexporter), ropts);
  auto mp = metrics_sdk::MeterProviderFactory::Create(metrics_sdk::ViewRegistryFactory::Create(), resource);
  mp->AddMetricReader(std::move(reader));
  impl->meter_provider = std::shared_ptr<metrics_api::MeterProvider>(std::move(mp));
  impl->meter = impl->meter_provider->GetMeter("zeal", ZEAL_VERSION);
  impl->damage = impl->meter->CreateUInt64Counter("eq.combat.damage", "combat damage", "{hitpoint}");
  impl->heal = impl->meter->CreateUInt64Counter("eq.combat.heal", "healing", "{hitpoint}");
  impl->attack_gauge = impl->meter->CreateInt64ObservableGauge("eq.character.attack", "attack rating", "1");
  impl->haste_gauge = impl->meter->CreateInt64ObservableGauge("eq.character.haste", "haste", "%");
  impl->attack_gauge->AddCallback(attack_callback, this);
  impl->haste_gauge->AddCallback(haste_callback, this);

  // --- logs ---
  otlp::OtlpHttpLogRecordExporterOptions lopts;
  lopts.url = base + "/v1/logs";
  auto lexporter = otlp::OtlpHttpLogRecordExporterFactory::Create(lopts);
  auto lprocessor = logs_sdk::BatchLogRecordProcessorFactory::Create(std::move(lexporter), {});
  auto lp = logs_sdk::LoggerProviderFactory::Create(std::move(lprocessor), resource);
  impl->logger_provider = std::shared_ptr<logs_api::LoggerProvider>(std::move(lp));
  impl->logger = impl->logger_provider->GetLogger("zeal", "", ZEAL_VERSION);

  // --- game hooks (identical to the hand-rolled build) ---
  zeal->chat_hook->add_print_chat_callback([this](const char *data, int color_index) {
    if (!is_enabled() || !data) return;
    log(data, color_index);
    parse_dot_or_heal(data);
  });

  zeal->callbacks->AddReportSuccessfulHit([this](Zeal::GameStructures::Entity *source,
                                                 Zeal::GameStructures::Entity *target, WORD type, short spell_id,
                                                 short damage, char) {
    if (!is_enabled() || !source || damage <= 0) return;
    Zeal::GameStructures::Entity *attacker = source;
    if (attacker->PetOwnerSpawnId != 0) {
      auto *owner = ZealService::get_instance()->entity_manager->Get(attacker->PetOwnerSpawnId);
      if (owner) attacker = owner;
    }
    std::string src = attacker->Name;
    const char *src_type = (attacker->Type == Zeal::GameEnums::EntityTypes::Player) ? "player" : "npc";
    const char *direction = (target && target == Zeal::Game::get_self()) ? "incoming" : "outgoing";
    const char *dtype = (spell_id > 0) ? "spell" : melee_type_name(type);
    const std::string tgt = target ? target->Name : "";
    if (in_combat_scope(src)) record_combat_damage(src, src_type, direction, dtype, tgt, damage);
  });

  zeal->callbacks->AddGeneric([this]() { sample_game_state(); });

  zeal->commands_hook->Add(
      "/otlp", {}, "OTLP telemetry export (SDK build). Usage: /otlp on|off|status|scope self|all",
      [this](std::vector<std::string> &args) {
        if (args.size() == 2 && Zeal::String::compare_insensitive(args[1], "on")) {
          setting_enabled.set(true);
          Zeal::Game::print_chat("OTLP (SDK) export enabled -> %s", setting_endpoint.get().c_str());
          return true;
        }
        if (args.size() == 2 && Zeal::String::compare_insensitive(args[1], "off")) {
          setting_enabled.set(false);
          Zeal::Game::print_chat("OTLP (SDK) export disabled.");
          return true;
        }
        if (args.size() == 3 && Zeal::String::compare_insensitive(args[1], "scope")) {
          setting_combat_scope.set(Zeal::String::compare_insensitive(args[2], "all") ? "all" : "self");
          Zeal::Game::print_chat("OTLP combat scope: %s", setting_combat_scope.get().c_str());
          return true;
        }
        Zeal::Game::print_chat("OTLP (opentelemetry-cpp SDK build): %s, endpoint %s, flush %ims, scope %s",
                               setting_enabled.get() ? "enabled" : "disabled", setting_endpoint.get().c_str(),
                               setting_flush_ms.get(), setting_combat_scope.get().c_str());
        Zeal::Game::print_chat("  recorded: %llu log records, %llu damage/heal measurements",
                               logs_posted.load(), metrics_posted.load());
        Zeal::Game::print_chat("  (the SDK owns batching/retry; failures surface in its own logs)");
        return true;
      });
}

OtlpExporter::~OtlpExporter() {
  // Flush and stop the SDK's exporter threads before the DLL unloads.
  if (impl) {
    if (impl->meter_provider) {
      auto *mp = static_cast<metrics_sdk::MeterProvider *>(impl->meter_provider.get());
      mp->ForceFlush(std::chrono::microseconds(1000000));
      mp->Shutdown();
    }
    if (impl->logger_provider) {
      auto *lp = static_cast<logs_sdk::LoggerProvider *>(impl->logger_provider.get());
      lp->ForceFlush(std::chrono::microseconds(1000000));
      lp->Shutdown();
    }
  }
}

void OtlpExporter::log(const std::string &body, int color_index) {
  if (!setting_enabled.get() || body.empty() || !impl->logger) return;
  auto rec = impl->logger->CreateLogRecord();
  if (!rec) return;
  rec->SetSeverity(logs_api::Severity::kInfo);
  rec->SetBody(body);
  rec->SetAttribute("eq.chat.color", static_cast<int64_t>(color_index));
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex);
    if (snapshot.in_game) {
      rec->SetAttribute("eq.character.name", snapshot.name);
      rec->SetAttribute("eq.zone.name", snapshot.zone);
    }
  }
  impl->logger->EmitLogRecord(std::move(rec));
  logs_posted++;
}

void OtlpExporter::record_combat_damage(const std::string &source, const std::string &source_type,
                                        const std::string &direction, const std::string &type,
                                        const std::string &target, long long amount) {
  if (amount <= 0 || !impl->damage) return;
  const std::string zone = current_zone_name();
  impl->damage->Add(static_cast<uint64_t>(amount), {{"eq.combat.source", source},
                                                    {"eq.combat.source_type", source_type},
                                                    {"eq.combat.direction", direction},
                                                    {"eq.combat.damage.type", type},
                                                    {"eq.zone.name", zone},
                                                    {"eq.combat.target", target}});
  metrics_posted++;
}

void OtlpExporter::record_heal(const std::string &source, const std::string &direction, long long amount) {
  if (amount <= 0 || !impl->heal) return;
  const std::string zone = current_zone_name();
  impl->heal->Add(static_cast<uint64_t>(amount),
                  {{"eq.combat.source", source}, {"eq.combat.direction", direction}, {"eq.zone.name", zone}});
  metrics_posted++;
}

void OtlpExporter::parse_dot_or_heal(const std::string &line) {
  const char *self = Zeal::Game::get_self() ? Zeal::Game::get_self()->Name : nullptr;
  if (!self) return;

  size_t p = line.find(" has taken ");
  if (p != std::string::npos) {
    long long amount = read_number(line, p + 11);
    size_t from = line.find(" damage from ", p);
    if (amount > 0 && from != std::string::npos) {
      const std::string dot_target = line.substr(0, p);
      std::string rest = line.substr(from + 13);
      if (rest.rfind("your ", 0) == 0) record_combat_damage(self, "player", "outgoing", "dot", dot_target, amount);
    }
    return;
  }
  if (line.rfind("You have healed ", 0) == 0) {
    size_t f = line.find(" for ", 16);
    if (f != std::string::npos) {
      long long amount = read_number(line, f + 5);
      if (amount > 0) record_heal(self, "outgoing", amount);
    }
    return;
  }
  if (line.rfind("You have been healed for ", 0) == 0) {
    long long amount = read_number(line, 25);
    if (amount > 0) record_heal(self, "incoming", amount);
  }
}

bool OtlpExporter::in_combat_scope(const std::string &source) const {
  if (setting_combat_scope.get() != "self") return true;
  Zeal::GameStructures::GAMECHARINFO *ci = Zeal::Game::get_char_info();
  if (ci && source == ci->Name) return true;
  std::string pet;
  if (ZealService::get_instance()->labels_hook && ZealService::get_instance()->labels_hook->GetLabel(68, pet) &&
      !pet.empty() && source == pet)
    return true;
  return false;
}

void OtlpExporter::sample_game_state() {
  if (!setting_enabled.get()) return;
  Snapshot s;
  Zeal::GameStructures::GAMECHARINFO *ci = Zeal::Game::get_char_info();
  if (Zeal::Game::is_in_game() && ci) {
    s.in_game = true;
    s.name = ci->Name;
    s.zone = current_zone_name();
    s.class_name = class_name(ci->Class);
    s.level = ci->Level;
    ZealService *zeal = ZealService::get_instance();
    std::string offense;
    if (zeal->labels_hook && zeal->labels_hook->GetLabel(23, offense)) {
      s.have_attack = true;
      s.attack = atoll(offense.c_str());
    }
    Zeal::GameStructures::Entity *self = Zeal::Game::get_self();
    if (self) {
      unsigned int modified = self->ModifyAttackSpeed(1000, 0);
      long long haste = (modified > 0) ? static_cast<long long>((1000.0 - modified) * 100.0 / modified + 0.5) : 0;
      s.have_haste = true;
      s.haste = haste < 0 ? 0 : haste;
    }
  }
  std::lock_guard<std::mutex> lock(snapshot_mutex);
  snapshot = std::move(s);
}

static void attack_callback(metrics_api::ObserverResult result, void *state) {
  auto *self = static_cast<OtlpExporter *>(state);
  if (!self || !self->is_enabled()) return;
  self->observe_attack(&result);
}

static void haste_callback(metrics_api::ObserverResult result, void *state) {
  auto *self = static_cast<OtlpExporter *>(state);
  if (!self || !self->is_enabled()) return;
  self->observe_haste(&result);
}

void OtlpExporter::observe_attack(void *result_ptr) {
  auto &result = *static_cast<metrics_api::ObserverResult *>(result_ptr);
  Snapshot s;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex);
    s = snapshot;
  }
  if (!s.have_attack) return;
  auto obs = nostd::get<nostd::shared_ptr<metrics_api::ObserverResultT<int64_t>>>(result);
  obs->Observe(s.attack, {{"eq.zone.name", s.zone}});
}

void OtlpExporter::observe_haste(void *result_ptr) {
  auto &result = *static_cast<metrics_api::ObserverResult *>(result_ptr);
  Snapshot s;
  {
    std::lock_guard<std::mutex> lock(snapshot_mutex);
    s = snapshot;
  }
  if (!s.have_haste) return;
  auto obs = nostd::get<nostd::shared_ptr<metrics_api::ObserverResultT<int64_t>>>(result);
  obs->Observe(s.haste, {{"eq.zone.name", s.zone}});
}

#endif  // ZEAL_OTLP_SDK
