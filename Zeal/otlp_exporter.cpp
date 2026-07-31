#include "otlp_exporter.h"

#include <winhttp.h>

#include <chrono>

#include "callbacks.h"
#include "commands.h"
#include "game_functions.h"
#include "json.hpp"
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
}  // namespace

OtlpExporter::OtlpExporter(ZealService *zeal) {
  worker = std::thread([this]() { worker_loop(); });

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
                             Zeal::Game::print_chat("OTLP: %s, endpoint %s, flush %ims",
                                                    setting_enabled.get() ? "enabled" : "disabled",
                                                    setting_endpoint.get().c_str(), setting_flush_ms.get());
                             Zeal::Game::print_chat("Usage: /otlp on|off|status|endpoint <url>");
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
  record.time_unix_nano =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  record.body = body;
  record.color_index = color_index;
  if (Zeal::Game::is_in_game() && Zeal::Game::get_self()) {
    record.character = Zeal::Game::get_self()->Name;
    record.zone_id = Zeal::Game::get_self()->ZoneId;
  }

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
      queue_cv.wait_for(lock, std::chrono::milliseconds(setting_flush_ms.get() > 0 ? setting_flush_ms.get() : 2000),
                        [this]() { return end_thread || !queue.empty(); });
      if (end_thread && queue.empty()) return;

      const int max_batch = setting_max_batch.get() > 0 ? setting_max_batch.get() : 512;
      while (!queue.empty() && static_cast<int>(batch.size()) < max_batch) {
        batch.push_back(std::move(queue.front()));
        queue.pop_front();
      }
    }

    if (batch.empty() || !setting_enabled.get()) continue;

    try {
      post_json("/v1/logs", build_logs_payload(batch));
    } catch (const std::exception &) {
      // Never let telemetry take down the game; drop the batch on failure.
    }
  }
}

std::string OtlpExporter::build_logs_payload(const std::vector<LogRecord> &records) const {
  nlohmann::json log_records = nlohmann::json::array();
  for (const auto &r : records) {
    nlohmann::json attributes = nlohmann::json::array();
    attributes.push_back(int_attr("eq.chat.color", r.color_index));
    if (!r.character.empty()) attributes.push_back(string_attr("eq.character.name", r.character));
    if (r.zone_id >= 0) attributes.push_back(int_attr("eq.zone.id", r.zone_id));

    log_records.push_back({{"timeUnixNano", std::to_string(r.time_unix_nano)},
                           {"severityNumber", 9},  // INFO
                           {"severityText", "INFO"},
                           {"body", {{"stringValue", r.body}}},
                           {"attributes", attributes}});
  }

  nlohmann::json payload = {
      {"resourceLogs",
       {{{"resource",
          {{"attributes",
            {string_attr("service.name", "everquest"), string_attr("service.version", ZEAL_VERSION),
             string_attr("telemetry.sdk.name", "zeal")}}}},
         {"scopeLogs",
          {{{"scope", {{"name", "zeal"}, {"version", ZEAL_VERSION}}}, {"logRecords", log_records}}}}}}}};
  // EQ log lines can contain stray non-printable bytes; replace invalid UTF-8 rather than letting
  // dump() throw and drop the whole batch.
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
