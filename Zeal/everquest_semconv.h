// GENERATED FILE — do not edit.
// Produced by OpenTelemetry Weaver from the everquest-semconv registry:
//   weaver registry generate -r model --templates templates cpp <out>
// The registry is the source of truth for these names; a typo here becomes a compile error
// instead of a silently split timeseries.
#pragma once

namespace everquest_semconv {

// ---- attributes ----

inline constexpr const char *kEverquestCharacterAaUnspent = "everquest.character.aa.unspent";

inline constexpr const char *kEverquestCharacterClass = "everquest.character.class";

inline constexpr const char *kEverquestCharacterDeity = "everquest.character.deity";

inline constexpr const char *kEverquestCharacterLevel = "everquest.character.level";

inline constexpr const char *kEverquestCharacterName = "everquest.character.name";

inline constexpr const char *kEverquestCharacterStatName = "everquest.character.stat.name";

inline constexpr const char *kEverquestChatChannel = "everquest.chat.channel";

inline constexpr const char *kEverquestChatColor = "everquest.chat.color";

inline constexpr const char *kEverquestChatSender = "everquest.chat.sender";

inline constexpr const char *kEverquestCombatDamageAmount = "everquest.combat.damage.amount";

inline constexpr const char *kEverquestCombatDamageType = "everquest.combat.damage.type";

inline constexpr const char *kEverquestCombatDirection = "everquest.combat.direction";

inline constexpr const char *kEverquestCombatPetName = "everquest.combat.pet.name";

inline constexpr const char *kEverquestCombatSource = "everquest.combat.source";

inline constexpr const char *kEverquestCombatSourceType = "everquest.combat.source_type";

inline constexpr const char *kEverquestCombatTarget = "everquest.combat.target";

inline constexpr const char *kEverquestConsiderCon = "everquest.consider.con";

inline constexpr const char *kEverquestConsiderFaction = "everquest.consider.faction";

inline constexpr const char *kEverquestContentExpansion = "everquest.content.expansion";

inline constexpr const char *kEverquestContentGated = "everquest.content.gated";

inline constexpr const char *kEverquestDisciplineName = "everquest.discipline.name";

inline constexpr const char *kEverquestFightDamageDealt = "everquest.fight.damage.dealt";

inline constexpr const char *kEverquestFightDamageTaken = "everquest.fight.damage.taken";

inline constexpr const char *kEverquestFightOutcome = "everquest.fight.outcome";

inline constexpr const char *kEverquestGroupLeader = "everquest.group.leader";

inline constexpr const char *kEverquestHealCasterManaPercent = "everquest.heal.caster.mana.percent";

inline constexpr const char *kEverquestHealChainHandoffMs = "everquest.heal.chain.handoff.ms";

inline constexpr const char *kEverquestHealChainPosition = "everquest.heal.chain.position";

inline constexpr const char *kEverquestLoginResult = "everquest.login.result";

inline constexpr const char *kEverquestMerchantItemCount = "everquest.merchant.item_count";

inline constexpr const char *kEverquestNetworkArqAbandoned = "everquest.network.arq.abandoned";

inline constexpr const char *kEverquestNetworkArqUnackedDepth = "everquest.network.arq.unacked_depth";

inline constexpr const char *kEverquestPacketSize = "everquest.packet.size";

inline constexpr const char *kEverquestProfileReason = "everquest.profile.reason";

inline constexpr const char *kEverquestProfileSource = "everquest.profile.source";

inline constexpr const char *kEverquestRaidTarget = "everquest.raid.target";

inline constexpr const char *kEverquestScriptFile = "everquest.script.file";

inline constexpr const char *kEverquestScriptMethod = "everquest.script.method";

inline constexpr const char *kEverquestScriptReceiver = "everquest.script.receiver";

inline constexpr const char *kEverquestServerStore = "everquest.server.store";

inline constexpr const char *kEverquestSpawnCount = "everquest.spawn.count";

inline constexpr const char *kEverquestSpawnId = "everquest.spawn.id";

inline constexpr const char *kEverquestSpawnName = "everquest.spawn.name";

inline constexpr const char *kEverquestSpellName = "everquest.spell.name";

inline constexpr const char *kEverquestStreamKind = "everquest.stream.kind";

inline constexpr const char *kEverquestZoneId = "everquest.zone.id";

inline constexpr const char *kEverquestZoneName = "everquest.zone.name";


// ---- metrics ----

inline constexpr const char *kEverquestCharacterAttackMetric = "everquest.character.attack";

inline constexpr const char *kEverquestCharacterCreatedMetric = "everquest.character.created";

inline constexpr const char *kEverquestCharacterHasteMetric = "everquest.character.haste";

inline constexpr const char *kEverquestCharacterHpMetric = "everquest.character.hp";

inline constexpr const char *kEverquestCharacterHpLimitMetric = "everquest.character.hp.limit";

inline constexpr const char *kEverquestCharacterHpMinMetric = "everquest.character.hp.min";

inline constexpr const char *kEverquestCharacterStatMetric = "everquest.character.stat";

inline constexpr const char *kEverquestCombatDamageMetric = "everquest.combat.damage";

inline constexpr const char *kEverquestCombatHealMetric = "everquest.combat.heal";

inline constexpr const char *kEverquestFightActiveMetric = "everquest.fight.active";

inline constexpr const char *kEverquestFightDurationMetric = "everquest.fight.duration";

inline constexpr const char *kEverquestGroupMemberMetric = "everquest.group.member";

inline constexpr const char *kEverquestLoginAttemptsMetric = "everquest.login.attempts";

inline constexpr const char *kEverquestNetworkDatagramsMetric = "everquest.network.datagrams";

inline constexpr const char *kEverquestNetworkDatagramsAbandonedMetric = "everquest.network.datagrams.abandoned";

inline constexpr const char *kEverquestNetworkDatagramsLostMetric = "everquest.network.datagrams.lost";

inline constexpr const char *kEverquestNetworkDatagramsRetransmittedMetric = "everquest.network.datagrams.retransmitted";

inline constexpr const char *kEverquestRaidKillTimestampMetric = "everquest.raid.kill.timestamp";

inline constexpr const char *kEverquestRaidLockoutExpiryMetric = "everquest.raid.lockout.expiry";

inline constexpr const char *kEverquestScriptMissingBindingMetric = "everquest.script.missing_binding";

inline constexpr const char *kEverquestServerUpMetric = "everquest.server.up";

inline constexpr const char *kEverquestZoneMovementUpdatesMetric = "everquest.zone.movement.updates";


}  // namespace everquest_semconv