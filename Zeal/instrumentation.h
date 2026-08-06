// Instrumentation for EverQuest gameplay, written against the OpenTelemetry **API** only.
//
// Nothing here includes an sdk/ or exporters/ header, and nothing here knows where telemetry goes.
// If no SDK has been registered (see telemetry.h) every call is a no-op that costs a virtual call -
// so call sites need no #ifdef and no "is telemetry on?" check.
//
// Instruments are created once, on first use, and cached. That is only sound because the providers
// live for the process: the Resource is fixed at startup and everything that varies while the game
// runs - the character above all - travels as a measurement attribute.
#pragma once

#include <string>

namespace zeal::instrumentation {

// The character the local player is currently on. Recorded as an attribute on every measurement
// rather than as part of the Resource, because it changes when camping to character select and
// identifying attributes of a Resource must not change during its lifetime.
void SetCharacter(const std::string &name);

// Slowly-changing character state, read by the observable gauges when the SDK collects. Called from
// the game thread; the gauges' callbacks run on the SDK's collection thread and read the snapshot
// this keeps, so neither touches game memory the other might be freeing.
void SetCharacterState(const std::string &zone, long long attack, bool have_attack, long long haste,
                       bool have_haste);

// Damage dealt or taken. `source_type` distinguishes player/pet/npc; `group_leader` is empty when
// solo, and is then omitted entirely - an absent attribute is a missing label in Prometheus, which
// keeps solo play out of group-scoped queries.
void RecordDamage(const std::string &source, const std::string &source_type, const std::string &direction,
                  const std::string &damage_type, const std::string &zone, const std::string &target,
                  const std::string &group_leader, long long amount);

// A zone session: the parent of every fight span recorded while you are in that zone, so a trace
// viewer shows a night's pulls on one timeline instead of one disconnected trace per mob.
void BeginZoneSession(const std::string &zone);
void EndZoneSession();

// A completed fight, recorded as a span under the current zone session. `duration_ms` is used to
// place the span in the past: the fight ended when this is called, so the span covers the window
// that just closed rather than an instant.
void RecordFight(const std::string &target, const std::string &zone, const std::string &outcome,
                 long long damage_dealt, long long damage_taken, unsigned long long duration_ms);

// Healing delivered or received.
void RecordHeal(const std::string &source, const std::string &direction, const std::string &zone,
                const std::string &group_leader, long long amount);

}  // namespace zeal::instrumentation
