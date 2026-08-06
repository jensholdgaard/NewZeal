// SDK-backed metrics path, exporting over the WinHTTP transport from spike/winhttp-client.
//
// The hand-rolled exporter is untouched and keeps running; this exists to answer whether the SDK
// can carry the same telemetry, correctly, from inside the game process.
//
// Two things here are not obvious and are both spec consequences rather than preference:
//
//  1. The exporter is constructed with our transport *injected*. Going through
//     OtlpHttpMetricExporterFactory reaches for the SDK's default HTTP backend, which is not
//     compiled in when WITH_HTTP_CLIENT_CURL=OFF - and the SDK's response is to terminate the
//     process. In a game client that is the player losing their session with nothing written down.
//
//  2. The provider is rebuilt when the character changes. A Resource is immutable and bound at
//     MeterProvider creation, and this pipeline carries the character in service.instance.id, so a
//     character switch is a different service instance - not a mutation of the current one.
#include "otel_sdk_probe.h"

// The SDK's headers use std::min/max, which the Windows macros break. Zeal deliberately does NOT
// define NOMINMAX globally - 164 call sites rely on the macros - so it is scoped to just these
// includes and restored afterwards.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/resource/resource.h"

#pragma pop_macro("max")
#pragma pop_macro("min")

#include "winhttp_client.h"

#include <memory>
#include <mutex>
#include <string>

namespace otlp = opentelemetry::exporter::otlp;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace metrics_api = opentelemetry::metrics;

namespace {

std::mutex g_mutex;
std::string g_endpoint;
std::string g_identity;  // the character the current provider was built for
std::shared_ptr<metrics_sdk::MeterProvider> g_provider;
opentelemetry::nostd::shared_ptr<metrics_api::Counter<uint64_t>> g_probe_counter;

// Tears down the current provider. Shutdown() flushes what is pending and stops the reader thread;
// leaving it to the destructor would race the game's own teardown.
void ShutdownLocked() {
  if (!g_provider) return;
  g_provider->Shutdown(std::chrono::microseconds(2000000));
  metrics_api::Provider::SetMeterProvider(std::shared_ptr<metrics_api::MeterProvider>());
  g_probe_counter = opentelemetry::nostd::shared_ptr<metrics_api::Counter<uint64_t>>();
  g_provider.reset();
  g_identity.clear();
}

}  // namespace

namespace zeal_otel_sdk {

void Configure(const std::string &endpoint) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_endpoint = endpoint;
}

bool EnsureProvider(const std::string &character, const std::string &service_version, std::string &error) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_provider && g_identity == character) return true;  // already correct: the common path
  if (character.empty()) {
    error = "no character yet - identity is part of an immutable Resource, so nothing can be built";
    return false;
  }

  try {
    ShutdownLocked();  // character changed: this is a different service instance

    otlp::OtlpHttpMetricExporterOptions options;
    options.url = g_endpoint;
    options.content_type = otlp::HttpRequestContentType::kJson;
    // Cumulative is what the Prometheus receiver downstream expects; delta would be re-aggregated.
    options.aggregation_temporality = otlp::PreferredAggregationTemporality::kCumulative;

    // Injected, not built by the Factory - see the note at the top of this file.
    auto transport = std::make_shared<winhttp_client::HttpClient>();
    auto exporter = std::unique_ptr<metrics_sdk::PushMetricExporter>(
        new otlp::OtlpHttpMetricExporter(options, transport));

    metrics_sdk::PeriodicExportingMetricReaderOptions reader_options;
    reader_options.export_interval_millis = std::chrono::milliseconds(10000);
    reader_options.export_timeout_millis = std::chrono::milliseconds(5000);
    auto reader =
        metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), reader_options);

    // telemetry.sdk.{name,language,version} are supplied by the SDK itself - unlike the hand-rolled
    // path, which has to claim them. service.instance.id carries the character because that is what
    // this pipeline promotes to Prometheus's `instance` label.
    auto resource = opentelemetry::sdk::resource::Resource::Create({
        {"service.name", "everquest"},
        {"service.version", service_version},
        {"service.instance.id", character},
    });

    auto provider = metrics_sdk::MeterProviderFactory::Create(
        std::unique_ptr<metrics_sdk::ViewRegistry>(new metrics_sdk::ViewRegistry()), resource);
    provider->AddMetricReader(std::move(reader));

    g_provider = std::move(provider);
    metrics_api::Provider::SetMeterProvider(g_provider);
    g_identity = character;

    auto meter = g_provider->GetMeter("zeal.otlp", "1.0.0");
    g_probe_counter = meter->CreateUInt64Counter("everquest.sdk.probe", "probe emitted by the SDK", "1");
    return true;
  } catch (const std::exception &e) {
    error = e.what();
    return false;
  } catch (...) {
    error = "unknown exception initialising the SDK";
    return false;
  }
}

void Count(long long value) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_probe_counter) return;
  g_probe_counter->Add(static_cast<uint64_t>(value < 0 ? 0 : value));
}

void Stop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  ShutdownLocked();
}

bool Running() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return static_cast<bool>(g_provider);
}

}  // namespace zeal_otel_sdk
