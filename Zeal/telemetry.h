// OpenTelemetry SDK setup - the only place in Zeal that configures telemetry.
//
// This is the "application owner" half of OpenTelemetry's API/SDK split: it constructs the
// providers, exporters and Resource once, and registers them globally. Instrumentation elsewhere in
// Zeal calls the *API* only (opentelemetry::metrics::Provider::GetMeterProvider(), etc.) and must
// not include any sdk/ or exporters/ header. Per the client design principles: "Instrumentation
// authors MUST NOT directly reference any SDK package of any kind, only the API."
//
// The practical benefit is not purity. Because the API is a no-op until an SDK is registered,
// instrumented call sites compile and run whether or not telemetry is built in, so the game code
// carries no #ifdefs and no null checks.
#pragma once

#include <string>

namespace zeal::telemetry {

// Configures and registers the global MeterProvider, LoggerProvider and TracerProvider, all
// exporting OTLP/HTTP to `endpoint` (a base URL; the signal paths are appended) over one shared
// WinHTTP transport. Safe to call repeatedly - only the first call builds anything.
//
// The Resource is fixed for the lifetime of the process, which is what lets instruments be cached
// at call sites: a Resource is immutable and bound at provider creation, so anything that changes
// while the game runs - the character, above all - belongs on measurements, not here.
// `token` is the guild gateway's bearer credential, sent as an Authorization header on every
// export; empty means send none, which is what a local collector wants. It is passed in rather
// than read here because this file owns the SDK, not the settings.
//
// Both `endpoint` and `token` are captured when the providers are built and cannot be changed
// afterwards - the exporters hold their own copies. Changing either at runtime therefore means
// Stop() and a fresh Start(), which is what /otlp endpoint and /otlp token do.
bool Start(const std::string &endpoint, const std::string &token, const std::string &service_version,
           std::string &error);

// Flushes and shuts down all three providers. Must run before the DLL unloads: the metric reader
// and the batch processors own threads that would otherwise export from freed memory.
void Stop();

// True once Start() has succeeded.
bool Running();

// What the exporters' transport has actually delivered. Surfaced here rather than by including the
// transport header elsewhere: this file is the SDK boundary, and instrumentation must not reach
// past it.
struct ExportStats {
  unsigned long long posted = 0;
  unsigned long long failed = 0;
  int last_status = 0;
  std::string last_error;
};
ExportStats Stats();

}  // namespace zeal::telemetry
