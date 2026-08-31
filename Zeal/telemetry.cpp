#include "telemetry.h"

#include <algorithm>

// The SDK's headers use std::min/max, which the Windows macros break. Zeal deliberately does NOT
// define NOMINMAX globally - 164 call sites rely on the macros - so it is scoped to these includes
// and restored afterwards.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

#include "opentelemetry/exporters/otlp/otlp_http_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h"
#include "opentelemetry/logs/provider.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_factory.h"
#include "opentelemetry/sdk/logs/batch_log_record_processor_options.h"
#include "opentelemetry/sdk/logs/logger_provider.h"
#include "opentelemetry/sdk/logs/logger_provider_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/batch_span_processor_factory.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/sdk/trace/tracer_provider_factory.h"
#include "opentelemetry/trace/provider.h"

#pragma pop_macro("max")
#pragma pop_macro("min")

#include "winhttp_client.h"

#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>

namespace otlp = opentelemetry::exporter::otlp;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace logs_sdk = opentelemetry::sdk::logs;

namespace {

std::mutex g_mutex;
bool g_running = false;
std::shared_ptr<metrics_sdk::MeterProvider> g_meter_provider;
std::shared_ptr<logs_sdk::LoggerProvider> g_logger_provider;
std::shared_ptr<trace_sdk::TracerProvider> g_tracer_provider;

// service.instance.id must be unique per instance and stable for its lifetime. The semconv registry
// recommends a random v4 UUID, and deliberately warns against deriving it from something meaningful:
// the underlying data "should be treated as confidential, being the user's choice to expose it".
// A character name would leak identity into every series for no benefit - the character travels as a
// measurement attribute instead, where it can change without redefining the entity.
std::string NewInstanceId() {
  std::random_device rd;
  std::uniform_int_distribution<unsigned> hex(0, 15);
  std::ostringstream out;
  out << std::hex;
  for (int i = 0; i < 32; ++i) {
    if (i == 8 || i == 12 || i == 16 || i == 20) out << '-';
    if (i == 12) { out << '4'; continue; }                      // version 4
    if (i == 16) { out << (8 + (hex(rd) & 3)); continue; }      // variant 1
    out << hex(rd);
  }
  return out.str();
}

}  // namespace

namespace zeal::telemetry {

bool Start(const std::string &endpoint, const std::string &token, int export_interval_ms,
           const std::string &service_version, std::string &error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_running) return true;

  try {
    // One transport for all three signals. Constructing the exporters with it explicitly is
    // required, not stylistic: the *Factory helpers reach for the SDK's default HTTP backend, which
    // is not compiled in with WITH_HTTP_CLIENT_CURL=OFF, and the SDK's response to its absence is to
    // terminate the process - which in a game client means the player loses their session.
    auto transport = std::make_shared<winhttp_client::HttpClient>();

    // The gateway authenticates per member. Every signal carries the same credential, so the
    // headers are built once and copied into each exporter's options below.
    otlp::OtlpHeaders auth_headers;
    if (!token.empty()) auth_headers.insert(std::make_pair("Authorization", "Bearer " + token));

    // telemetry.sdk.{name,language,version} are filled in by the SDK itself.
    auto resource = opentelemetry::sdk::resource::Resource::Create({
        {"service.name", "everquest"},
        {"service.version", service_version},
        {"service.instance.id", NewInstanceId()},
    });

    // --- metrics ---------------------------------------------------------------------------------
    otlp::OtlpHttpMetricExporterOptions metric_options;
    metric_options.url = endpoint + "/v1/metrics";
    metric_options.content_type = otlp::HttpRequestContentType::kJson;
    metric_options.http_headers = auth_headers;
    // Cumulative, despite what it costs in retained state - measured against live combat, delta was
    // worse in a way that matters more.
    //
    // Under delta the SDK only exports attribute sets that saw activity in the cycle, so a mob killed
    // inside one 10s window produces a single point. rate() needs two points in its window to return
    // anything at all, so those fights contributed *zero* DPS while the dashboard looked healthy.
    // That is the same silent-undercount failure the cardinality limit would cause, arriving sooner
    // and on ordinary short fights rather than only on long sessions.
    //
    // Cumulative re-exports every attribute set every cycle, which is what keeps rate() well defined
    // and matches what the hand-rolled exporter did. The cost is that state is never reclaimed: at
    // 2000 attribute sets the SDK folds the rest into otel.metric.overflow=true, dropping target and
    // source. That ceiling is monitorable - alert on otel_metric_overflow - whereas a wrong rate() is
    // not visible at all.
    metric_options.aggregation_temporality = otlp::PreferredAggregationTemporality::kCumulative;
    auto metric_exporter = std::unique_ptr<metrics_sdk::PushMetricExporter>(
        new otlp::OtlpHttpMetricExporter(metric_options, transport));

    metrics_sdk::PeriodicExportingMetricReaderOptions reader_options;
    // Comparisons, not std::min/max: windows.h arrives with the transport header below the
    // push/pop guard above, so min and max are macros by the time this line is compiled.
    int interval = export_interval_ms;
    if (interval < kMinExportIntervalMs) interval = kMinExportIntervalMs;
    if (interval > kMaxExportIntervalMs) interval = kMaxExportIntervalMs;
    reader_options.export_interval_millis = std::chrono::milliseconds(interval);
    // Must stay below the interval or the reader can start an export while the previous one is
    // still running; half of it, capped, leaves room for a slow round trip without overlapping.
    const int timeout = interval / 2 < 5000 ? interval / 2 : 5000;
    reader_options.export_timeout_millis = std::chrono::milliseconds(timeout);
    auto reader = metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(metric_exporter),
                                                                            reader_options);
    auto meter_provider = metrics_sdk::MeterProviderFactory::Create(
        std::unique_ptr<metrics_sdk::ViewRegistry>(new metrics_sdk::ViewRegistry()), resource);
    meter_provider->AddMetricReader(std::move(reader));
    g_meter_provider = std::move(meter_provider);
    std::shared_ptr<opentelemetry::metrics::MeterProvider> meter_api = g_meter_provider;
    opentelemetry::metrics::Provider::SetMeterProvider(meter_api);

    // --- logs (events) ---------------------------------------------------------------------------
    // Chat is not collected. The only log records Zeal emits are events - named occurrences such as
    // a character profile snapshot - through EmitEvent() below. Batched, so an event costs the
    // game thread a record, not a round trip.
    otlp::OtlpHttpLogRecordExporterOptions log_options;
    log_options.url = endpoint + "/v1/logs";
    log_options.content_type = otlp::HttpRequestContentType::kJson;
    log_options.http_headers = auth_headers;
    auto log_exporter = std::unique_ptr<logs_sdk::LogRecordExporter>(
        new otlp::OtlpHttpLogRecordExporter(log_options, transport));
    logs_sdk::BatchLogRecordProcessorOptions log_batch;
    log_batch.schedule_delay_millis = std::chrono::milliseconds(5000);
    auto log_processor =
        logs_sdk::BatchLogRecordProcessorFactory::Create(std::move(log_exporter), log_batch);
    g_logger_provider = logs_sdk::LoggerProviderFactory::Create(std::move(log_processor), resource);
    std::shared_ptr<opentelemetry::logs::LoggerProvider> logger_api = g_logger_provider;
    opentelemetry::logs::Provider::SetLoggerProvider(logger_api);

    // --- traces ----------------------------------------------------------------------------------
    // Fight spans.
    otlp::OtlpHttpExporterOptions trace_options;
    trace_options.url = endpoint + "/v1/traces";
    trace_options.content_type = otlp::HttpRequestContentType::kJson;
    trace_options.http_headers = auth_headers;
    auto span_exporter = std::unique_ptr<trace_sdk::SpanExporter>(
        new otlp::OtlpHttpExporter(trace_options, transport));
    trace_sdk::BatchSpanProcessorOptions span_batch;
    span_batch.schedule_delay_millis = std::chrono::milliseconds(5000);
    auto span_processor =
        trace_sdk::BatchSpanProcessorFactory::Create(std::move(span_exporter), span_batch);
    g_tracer_provider = trace_sdk::TracerProviderFactory::Create(std::move(span_processor), resource);
    std::shared_ptr<opentelemetry::trace::TracerProvider> tracer_api = g_tracer_provider;
    opentelemetry::trace::Provider::SetTracerProvider(tracer_api);

    g_running = true;
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  } catch (...) {
    error = "unknown exception initialising OpenTelemetry";
    return false;
  }
}

void Stop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_running) return;
  const auto timeout = std::chrono::microseconds(2000000);

  // Shutdown, not just flush: each of these owns a thread, and the game will not wait for them.
  if (g_meter_provider) g_meter_provider->Shutdown(timeout);
  if (g_logger_provider) g_logger_provider->Shutdown(timeout);
  if (g_tracer_provider) g_tracer_provider->Shutdown(timeout);

  opentelemetry::metrics::Provider::SetMeterProvider(
      std::shared_ptr<opentelemetry::metrics::MeterProvider>());
  opentelemetry::trace::Provider::SetTracerProvider(
      std::shared_ptr<opentelemetry::trace::TracerProvider>());
  opentelemetry::logs::Provider::SetLoggerProvider(
      std::shared_ptr<opentelemetry::logs::LoggerProvider>());

  g_meter_provider.reset();
  g_logger_provider.reset();
  g_tracer_provider.reset();
  g_running = false;
}

void EmitEvent(const std::string &event_name, const std::string &body_json,
               const std::vector<std::pair<std::string, std::string>> &attributes) {
  // API only from here: a no-op provider answers when Start() has not run, so callers need no
  // guard and the game code carries no #ifdef.
  auto provider = opentelemetry::logs::Provider::GetLoggerProvider();
  auto logger = provider->GetLogger("zeal", "zeal", "");
  auto record = logger->CreateLogRecord();
  if (!record) return;
  record->SetSeverity(opentelemetry::logs::Severity::kInfo);
  record->SetTimestamp(std::chrono::system_clock::now());
  record->SetBody(opentelemetry::nostd::string_view(body_json));
  record->SetAttribute("event.name", opentelemetry::nostd::string_view(event_name));
  for (const auto &kv : attributes) {
    record->SetAttribute(opentelemetry::nostd::string_view(kv.first),
                         opentelemetry::nostd::string_view(kv.second));
  }
  logger->EmitLogRecord(std::move(record));
}

ExportStats Stats() {
  const auto raw = winhttp_client::GetStats();
  ExportStats out;
  out.posted = raw.posted;
  out.failed = raw.failed;
  out.last_status = raw.last_status;
  out.last_error = raw.last_error;
  return out;
}

bool Running() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_running;
}

}  // namespace zeal::telemetry
