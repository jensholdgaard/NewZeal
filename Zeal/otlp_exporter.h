#pragma once
#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "zeal_settings.h"

// OtlpExporter emits telemetry from Zeal directly over OTLP/HTTP using the JSON encoding, so the
// game client can talk to an OpenTelemetry backend (e.g. Ourios) or Collector without an external
// pipe-reading sidecar.
//
// This first iteration implements the logs signal only: game log/chat lines are queued on the game
// thread (non-blocking) and flushed by a background worker thread that POSTs an OTLP/HTTP JSON
// payload to <endpoint>/v1/logs. Metrics and traces will follow the same queue+worker pattern.
class OtlpExporter {
 public:
  OtlpExporter(class ZealService *zeal);
  ~OtlpExporter();

  bool is_enabled() const { return setting_enabled.get(); }

  // Queues a single log record. Thread-safe and non-blocking; returns immediately (and drops the
  // record) when disabled so callers can gate cheaply on is_enabled(). color_index is the EQ chat
  // color, recorded as a log attribute.
  void log(const std::string &body, int color_index);

 private:
  struct LogRecord {
    unsigned long long time_unix_nano = 0;
    std::string body;
    int color_index = 0;
    int zone_id = -1;
  };

  void worker_loop();
  bool post_json(const std::string &path, const std::string &json_body);
  std::string build_logs_payload(const std::vector<LogRecord> &records) const;

  // Accumulates the `eq.combat.damage` counter, keyed by {direction, damage_type}.
  void record_combat_damage(const std::string &direction, const std::string &type, long long amount);
  // Serializes the current cumulative counter snapshot as an OTLP metrics payload ("" if empty).
  std::string build_metrics_payload();

  ZealSetting<bool> setting_enabled = {false, "Zeal", "OtlpEnabled", false};
  ZealSetting<std::string> setting_endpoint = {"http://127.0.0.1:4318", "Zeal", "OtlpEndpoint", false};
  ZealSetting<int> setting_flush_ms = {2000, "Zeal", "OtlpFlushMs", false};
  ZealSetting<int> setting_max_batch = {512, "Zeal", "OtlpMaxBatch", false};

  std::deque<LogRecord> queue;
  mutable std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::thread worker;
  bool end_thread = false;

  // Fixed start time for cumulative metric streams (set at construction).
  unsigned long long start_time_unix_nano = 0;
  std::mutex metrics_mutex;
  std::map<std::pair<std::string, std::string>, long long> combat_damage;  // {direction, type} -> total

  // Lightweight send stats surfaced by `/otlp status`.
  std::atomic<unsigned long long> logs_posted{0};
  std::atomic<unsigned long long> metrics_posted{0};
  std::atomic<unsigned long long> failed_posts{0};
  std::atomic<int> last_http_status{0};
};
