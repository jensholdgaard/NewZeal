// See otel_sdk_probe.cpp: proves the OpenTelemetry C++ SDK can initialise and export from inside
// the injected game DLL, over the WinHTTP transport. Compiled only when ZEAL_OTEL_SDK is defined,
// so the normal build is byte-for-byte unaffected.
#pragma once

#include <string>

namespace zeal_otel_sdk {

// Stands up a MeterProvider exporting OTLP/HTTP JSON to `endpoint`. False on failure, with `error`
// set - the SDK throws where the hand-rolled exporter returns codes, so this contains it.
bool Start(const std::string &endpoint, std::string &error);

// Adds to a counter the SDK owns, so the export path is exercised by real data.
void Count(long long value);

// Flushes and tears down. The game will not wait for a background export on unload.
void Stop();

}  // namespace zeal_otel_sdk
