// Minimal proof that the OpenTelemetry C++ SDK runs *inside the game process*, exporting over the
// WinHTTP transport from spike/winhttp-client.
//
// Deliberately not a port of the exporter. The open question is narrower and comes first: does the
// SDK link into a 32-bit /MT DLL injected into a 2003 game client, initialise there, and get a
// payload out? Answer that before rewriting a working exporter around it.
//
// Reached from `/otlp sdkprobe`; the hand-rolled exporter is untouched and keeps running.
#include "otel_sdk_probe.h"

// The SDK's headers use std::min/max, which the Windows macros break. Zeal deliberately does NOT
// define NOMINMAX globally - 164 call sites rely on the macros - so it is scoped to just these
// includes and restored afterwards.
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max

#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h"
#include "opentelemetry/metrics/provider.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/resource/resource.h"

#pragma pop_macro("max")
#pragma pop_macro("min")

#include <memory>
#include <string>

namespace otlp = opentelemetry::exporter::otlp;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace metrics_api = opentelemetry::metrics;

namespace {
std::shared_ptr<metrics_api::MeterProvider> g_provider;
}  // namespace

namespace zeal_otel_sdk {

bool Start(const std::string &endpoint, std::string &error) {
  try {
    otlp::OtlpHttpMetricExporterOptions options;
    options.url = endpoint;
    options.content_type = otlp::HttpRequestContentType::kJson;
    options.aggregation_temporality = otlp::PreferredAggregationTemporality::kCumulative;

    auto exporter = otlp::OtlpHttpMetricExporterFactory::Create(options);

    metrics_sdk::PeriodicExportingMetricReaderOptions reader_options;
    reader_options.export_interval_millis = std::chrono::milliseconds(2000);
    reader_options.export_timeout_millis = std::chrono::milliseconds(1500);
    auto reader =
        metrics_sdk::PeriodicExportingMetricReaderFactory::Create(std::move(exporter), reader_options);

    auto resource = opentelemetry::sdk::resource::Resource::Create({
        {"service.name", "everquest"},
        {"service.version", "sdk-probe"},
        {"telemetry.sdk.name", "opentelemetry-cpp"},
    });

    auto provider = metrics_sdk::MeterProviderFactory::Create(
        std::unique_ptr<metrics_sdk::ViewRegistry>(new metrics_sdk::ViewRegistry()), resource);
    provider->AddMetricReader(std::move(reader));

    g_provider = std::move(provider);
    metrics_api::Provider::SetMeterProvider(g_provider);
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
  if (!g_provider) return;
  auto meter = metrics_api::Provider::GetMeterProvider()->GetMeter("zeal.sdk.probe", "sdk-probe");
  static auto counter = meter->CreateUInt64Counter("everquest.sdk.probe", "probe emitted by the SDK", "1");
  counter->Add(static_cast<uint64_t>(value < 0 ? 0 : value));
}

void Stop() {
  if (!g_provider) return;
  // Force a final export before the DLL unloads; the game will not wait for us.
  static_cast<metrics_sdk::MeterProvider *>(g_provider.get())->ForceFlush(std::chrono::microseconds(2000000));
  metrics_api::Provider::SetMeterProvider(std::shared_ptr<metrics_api::MeterProvider>());
  g_provider.reset();
}

}  // namespace zeal_otel_sdk
