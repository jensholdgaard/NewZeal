#include "otlp_exporter.h"

#include <winhttp.h>

#include <cctype>
#include <chrono>
#include <cstdlib>

#include "callbacks.h"
#include "chat.h"
#include "commands.h"
#include "game_functions.h"
#include "game_structures.h"
#include "json.hpp"
#include "labels.h"
#include "string_util.h"
#include "zeal.h"

#pragma comment(lib, "winhttp.lib")

namespace {
// OTLP/HTTP JSON encodes 64-bit integer fields (timeUnixNano, AnyValue.intValue) as strings.
nlohmann::json string_attr(const char *key, const std::string &value) {
  return {{"key", key}, {"value", {{"stringValue", value}}}};
}
nlohmann::json int_attr(const char *key, long long value) {
  return {{"key", key}, {"value", {{"intValue", std::to_string(value)}}}};
}

unsigned long long now_unix_nano() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

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

// Builds the OTLP resource attributes shared by all signals: the service identity plus, when in
// game, the character context (identity + slowly-changing stats). Per OTLP guidance this belongs on
// the Resource — it is low cardinality, describes the entity producing telemetry, and enriches logs
// and metrics alike without inflating metric attribute cardinality.
nlohmann::json build_resource_attributes() {
  nlohmann::json attrs = nlohmann::json::array();
  attrs.push_back(string_attr("service.name", "everquest"));
  attrs.push_back(string_attr("service.version", ZEAL_VERSION));
  attrs.push_back(string_attr("telemetry.sdk.name", "zeal"));

  Zeal::GameStructures::GAMECHARINFO *ci = Zeal::Game::get_char_info();
  if (Zeal::Game::is_in_game() && ci) {
    // Character name doubles as the service instance id so a shared backend can tell players apart.
    attrs.push_back(string_attr("service.instance.id", ci->Name));
    attrs.push_back(string_attr("eq.character.name", ci->Name));
    attrs.push_back(string_attr("eq.character.class", class_name(ci->Class)));
    attrs.push_back(int_attr("eq.character.level", ci->Level));
    attrs.push_back(int_attr("eq.character.deity", ci->Deity));
    attrs.push_back(int_attr("eq.character.aa.unspent", ci->AlternateAdvancementUnspent));
    attrs.push_back(int_attr("eq.character.stat.strength", ci->BaseSTR));
    attrs.push_back(int_attr("eq.character.stat.stamina", ci->BaseSTA));
    attrs.push_back(int_attr("eq.character.stat.dexterity", ci->BaseDEX));
    attrs.push_back(int_attr("eq.character.stat.agility", ci->BaseAGI));
    attrs.push_back(int_attr("eq.character.stat.wisdom", ci->BaseWIS));
    attrs.push_back(int_attr("eq.character.stat.intelligence", ci->BaseINT));
    attrs.push_back(int_attr("eq.character.stat.charisma", ci->BaseCHA));
  }
  return attrs;
}

// Parses an EverQuest combat log line for damage the player is directly involved in and, if found,
// fills direction ("outgoing"/"incoming"), type (the melee verb or "spell"), and amount. Returns
// false for lines that aren't self-involved damage (e.g. a group member's hits) so they aren't
// double counted. EQ renders the local player as "You"/"YOU".
bool parse_combat_line(const std::string &line, std::string &direction, std::string &type, long long &amount) {
  size_t dmg = line.find("of damage");
  if (dmg == std::string::npos) return false;
  size_t pts = line.rfind("point", dmg);  // matches "point" and "points"
  if (pts == std::string::npos) return false;

  size_t end = pts;
  while (end > 0 && line[end - 1] == ' ') end--;
  size_t start = end;
  while (start > 0 && isdigit(static_cast<unsigned char>(line[start - 1]))) start--;
  if (start == end) return false;
  amount = atoll(line.substr(start, end - start).c_str());
  if (amount <= 0) return false;

  if (line.rfind("You have taken ", 0) == 0) {
    direction = "incoming";
    type = "spell";  // "You have taken N points of damage from ..."
  } else if (line.rfind("You ", 0) == 0) {
    direction = "outgoing";
    size_t vstart = 4;
    size_t vend = line.find(' ', vstart);
    type = (vend == std::string::npos) ? "melee" : line.substr(vstart, vend - vstart);
  } else if (line.find(" YOU ") != std::string::npos) {
    direction = "incoming";
    type = "melee";
  } else {
    return false;  // Not self-involved; skip.
  }
  for (auto &c : type) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return true;
}
}  // namespace

OtlpExporter::OtlpExporter(ZealService *zeal) {
  start_time_unix_nano = now_unix_nano();
  worker = std::thread([this]() { worker_loop(); });

  // Emit every print-to-chat line as a log record and mine it for combat damage. This is the live
  // source (the pipe uses the same callback); registering our own keeps OTLP independent of whether
  // the pipe has a reader connected.
  zeal->chat_hook->add_print_chat_callback([this](const char *data, int color_index) {
    if (!is_enabled() || !data) return;
    log(data, color_index);
    std::string direction, type;
    long long amount = 0;
    if (parse_combat_line(data, direction, type, amount)) record_combat_damage(direction, type, amount);
  });

  zeal->commands_hook->Add("/otlp", {}, "OTLP/HTTP telemetry export. Usage: /otlp on|off|status|endpoint <url>",
                           [this](std::vector<std::string> &args) {
                             if (args.size() == 2 && Zeal::String::compare_insensitive(args[1], "on")) {
                               setting_enabled.set(true);
                               Zeal::Game::print_chat("OTLP export enabled -> %s/v1/logs",
                                                      setting_endpoint.get().c_str());
                               return true;
                             }
                             if (args.size() == 2 && Zeal::String::compare_insensitive(args[1], "off")) {
                               setting_enabled.set(false);
                               Zeal::Game::print_chat("OTLP export disabled.");
                               return true;
                             }
                             if (args.size() == 3 && Zeal::String::compare_insensitive(args[1], "endpoint")) {
                               setting_endpoint.set(args[2]);
                               Zeal::Game::print_chat("OTLP endpoint set to %s", args[2].c_str());
                               return true;
                             }
                             if (args.size() == 3 && Zeal::String::compare_insensitive(args[1], "flush")) {
                               int ms = 0;
                               if (!Zeal::String::tryParse(args[2], &ms)) {
                                 Zeal::Game::print_chat("Usage: /otlp flush <milliseconds>");
                                 return true;
                               }
                               if (ms < kMinFlushMs) ms = kMinFlushMs;  // Clamp: metrics are periodic; 0 would just hammer.
                               setting_flush_ms.set(ms);
                               Zeal::Game::print_chat("OTLP flush interval set to %ims", ms);
                               return true;
                             }
                             Zeal::Game::print_chat("OTLP: %s, endpoint %s, flush %ims",
                                                    setting_enabled.get() ? "enabled" : "disabled",
                                                    setting_endpoint.get().c_str(), setting_flush_ms.get());
                             Zeal::Game::print_chat("  sent: %llu log batches-worth, %llu metric posts, %llu failed",
                                                    logs_posted.load(), metrics_posted.load(), failed_posts.load());
                             Zeal::Game::print_chat("  last HTTP status: %i", last_http_status.load());
                             Zeal::Game::print_chat("Usage: /otlp on|off|status|endpoint <url>|flush <ms>");
                             return true;
                           });
}

OtlpExporter::~OtlpExporter() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    end_thread = true;
  }
  queue_cv.notify_all();
  if (worker.joinable()) worker.join();
}

void OtlpExporter::log(const std::string &body, int color_index) {
  if (!setting_enabled.get() || body.empty()) return;

  LogRecord record;
  record.time_unix_nano = now_unix_nano();
  record.body = body;
  record.color_index = color_index;
  if (Zeal::Game::is_in_game() && Zeal::Game::get_self()) record.zone_id = Zeal::Game::get_self()->ZoneId;

  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    queue.push_back(std::move(record));
  }
  queue_cv.notify_one();
}

void OtlpExporter::worker_loop() {
  while (true) {
    std::vector<LogRecord> batch;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      const int configured = setting_flush_ms.get();
      const int interval = configured > 0 ? (configured < kMinFlushMs ? kMinFlushMs : configured) : 2000;
      queue_cv.wait_for(lock, std::chrono::milliseconds(interval),
                        [this]() { return end_thread || !queue.empty(); });
      if (end_thread && queue.empty()) return;

      const int max_batch = setting_max_batch.get() > 0 ? setting_max_batch.get() : 512;
      while (!queue.empty() && static_cast<int>(batch.size()) < max_batch) {
        batch.push_back(std::move(queue.front()));
        queue.pop_front();
      }
    }

    if (!setting_enabled.get()) continue;

    try {
      if (!batch.empty()) {
        if (post_json("/v1/logs", build_logs_payload(batch)))
          logs_posted += batch.size();
        else
          failed_posts++;
      }
      // Metrics use cumulative temporality, so emit the current snapshot every flush even when no
      // new log lines arrived this cycle.
      std::string metrics = build_metrics_payload();
      if (!metrics.empty()) {
        if (post_json("/v1/metrics", metrics))
          metrics_posted++;
        else
          failed_posts++;
      }
    } catch (const std::exception &) {
      // Never let telemetry take down the game; drop this cycle on failure.
    }
  }
}

std::string OtlpExporter::build_logs_payload(const std::vector<LogRecord> &records) const {
  nlohmann::json log_records = nlohmann::json::array();
  for (const auto &r : records) {
    // Character identity now lives on the Resource; keep only per-line context here.
    nlohmann::json attributes = nlohmann::json::array();
    attributes.push_back(int_attr("eq.chat.color", r.color_index));
    if (r.zone_id >= 0) attributes.push_back(int_attr("eq.zone.id", r.zone_id));

    log_records.push_back({{"timeUnixNano", std::to_string(r.time_unix_nano)},
                           {"severityNumber", 9},  // INFO
                           {"severityText", "INFO"},
                           {"body", {{"stringValue", r.body}}},
                           {"attributes", attributes}});
  }

  nlohmann::json payload = {
      {"resourceLogs",
       {{{"resource", {{"attributes", build_resource_attributes()}}},
         {"scopeLogs",
          {{{"scope", {{"name", "zeal"}, {"version", ZEAL_VERSION}}}, {"logRecords", log_records}}}}}}}};
  // EQ log lines can contain stray non-printable bytes; replace invalid UTF-8 rather than letting
  // dump() throw and drop the whole batch.
  return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void OtlpExporter::record_combat_damage(const std::string &direction, const std::string &type, long long amount) {
  std::lock_guard<std::mutex> lock(metrics_mutex);
  combat_damage[{direction, type}] += amount;
}

// Builds a single-value gauge metric (no attributes) as an OTLP metric object.
static nlohmann::json gauge_metric(const char *name, const char *unit, const std::string &now, long long value) {
  nlohmann::json point = {{"timeUnixNano", now}, {"asInt", std::to_string(value)}};
  nlohmann::json metric;
  metric["name"] = name;
  metric["unit"] = unit;
  metric["gauge"] = {{"dataPoints", nlohmann::json::array({point})}};
  return metric;
}

std::string OtlpExporter::build_metrics_payload() {
  const std::string now = std::to_string(now_unix_nano());
  const std::string start = std::to_string(start_time_unix_nano);
  nlohmann::json metrics = nlohmann::json::array();

  // Combat damage counter (cumulative monotonic Sum).
  nlohmann::json data_points = nlohmann::json::array();
  {
    std::lock_guard<std::mutex> lock(metrics_mutex);
    for (const auto &[key, total] : combat_damage) {
      data_points.push_back({{"attributes",
                              {string_attr("eq.combat.direction", key.first),
                               string_attr("eq.combat.damage.type", key.second)}},
                             {"startTimeUnixNano", start},
                             {"timeUnixNano", now},
                             {"asInt", std::to_string(total)}});
    }
  }
  if (!data_points.empty()) {
    nlohmann::json metric;
    metric["name"] = "eq.combat.damage";
    metric["unit"] = "{hitpoint}";
    metric["sum"] = {{"dataPoints", data_points}, {"aggregationTemporality", 2}, {"isMonotonic", true}};
    metrics.push_back(metric);
  }

  // Attack (offense) and haste gauges for DPS correlation, sampled at flush time.
  if (Zeal::Game::is_in_game() && Zeal::Game::get_char_info()) {
    ZealService *zeal = ZealService::get_instance();
    std::string offense;
    if (zeal->labels_hook && zeal->labels_hook->GetLabel(23, offense))  // 23 = CurrentOffense (attack rating).
      metrics.push_back(gauge_metric("eq.character.attack", "1", now, atoll(offense.c_str())));

    Zeal::GameStructures::Entity *self = Zeal::Game::get_self();
    if (self) {
      // ModifyAttackSpeed applies total effective haste (worn + spell + overhaste) to a reference
      // delay; derive the haste percentage from the ratio.
      unsigned int modified = self->ModifyAttackSpeed(1000, 0);
      long long haste = (modified > 0) ? static_cast<long long>((1000.0 - modified) * 100.0 / modified + 0.5) : 0;
      if (haste < 0) haste = 0;
      metrics.push_back(gauge_metric("eq.character.haste", "%", now, haste));
    }
  }

  if (metrics.empty()) return "";

  nlohmann::json scope_metrics;
  scope_metrics["scope"] = {{"name", "zeal"}, {"version", ZEAL_VERSION}};
  scope_metrics["metrics"] = metrics;

  nlohmann::json resource_metrics;
  resource_metrics["resource"] = {{"attributes", build_resource_attributes()}};
  resource_metrics["scopeMetrics"] = nlohmann::json::array({scope_metrics});

  nlohmann::json payload;
  payload["resourceMetrics"] = nlohmann::json::array({resource_metrics});
  return payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool OtlpExporter::post_json(const std::string &path, const std::string &json_body) {
  std::string url = setting_endpoint.get() + path;
  std::wstring wurl(url.begin(), url.end());

  URL_COMPONENTS uc = {0};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {0};
  wchar_t url_path[1024] = {0};
  uc.lpszHostName = host;
  uc.dwHostNameLength = _countof(host);
  uc.lpszUrlPath = url_path;
  uc.dwUrlPathLength = _countof(url_path);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;

  bool ok = false;
  HINTERNET session = WinHttpOpen(L"Zeal-OTLP/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
  if (session) {
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (connect) {
      DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
      HINTERNET request = WinHttpOpenRequest(connect, L"POST", url_path, nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
      if (request) {
        const wchar_t *headers = L"Content-Type: application/json";
        if (WinHttpSendRequest(request, headers, -1L, (LPVOID)json_body.data(),
                               static_cast<DWORD>(json_body.size()), static_cast<DWORD>(json_body.size()), 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
          DWORD status = 0, size = sizeof(status);
          WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                              &status, &size, WINHTTP_NO_HEADER_INDEX);
          last_http_status.store(static_cast<int>(status));
          ok = (status >= 200 && status < 300);
        }
        WinHttpCloseHandle(request);
      }
      WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
  }
  return ok;
}
