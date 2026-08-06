// SDK-backed metrics path for Zeal, over the WinHTTP transport (spike/winhttp-client).
//
// Compiled only when ZEAL_OTEL_SDK is defined, so the shipping build is byte-for-byte unaffected
// and the hand-rolled exporter keeps running alongside it.
//
// The shape here is driven by one constraint from the spec: a Resource is immutable and is bound to
// a MeterProvider at creation, and identifying attributes MUST NOT change during the lifetime of
// the entity. This client puts the character name in service.instance.id - so camping to character
// select and logging back in as someone else is a genuinely different service instance, and the
// provider has to be rebuilt rather than mutated.
#pragma once

#include <string>

namespace zeal_otel_sdk {

// Endpoint only; no provider is built until a character is known, because the character is part of
// the Resource and the Resource cannot be changed afterwards.
void Configure(const std::string &endpoint);

// Builds the MeterProvider for this character, or rebuilds it if the identity changed. Cheap and
// idempotent when the identity is unchanged, so it is safe to call before recording. False on
// failure with `error` set - the SDK throws where the hand-rolled exporter returns codes.
bool EnsureProvider(const std::string &character, const std::string &service_version, std::string &error);

// Adds to a counter the SDK owns, so the export path is exercised by real data.
void Count(long long value);

// Mirrors what the hand-rolled exporter records for combat damage, under a separate metric name so
// the two can be compared in Prometheus rather than one trusted over the other.
void RecordDamage(const std::string &source, const std::string &source_type, const std::string &direction,
                  const std::string &damage_type, const std::string &zone, const std::string &target,
                  const std::string &group_leader, long long amount);

// Shuts the provider down, which flushes pending exports and stops the reader thread. The game will
// not wait for a background export on unload.
void Stop();

// True once a provider exists, for status output.
bool Running();

}  // namespace zeal_otel_sdk
