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

// Damage dealt or taken. `source_type` distinguishes player/pet/npc; `group_leader` is empty when
// solo, and is then omitted entirely - an absent attribute is a missing label in Prometheus, which
// keeps solo play out of group-scoped queries.
void RecordDamage(const std::string &source, const std::string &source_type, const std::string &direction,
                  const std::string &damage_type, const std::string &zone, const std::string &target,
                  const std::string &group_leader, long long amount);

// Healing delivered or received.
void RecordHeal(const std::string &source, const std::string &direction, const std::string &zone,
                const std::string &group_leader, long long amount);

}  // namespace zeal::instrumentation
