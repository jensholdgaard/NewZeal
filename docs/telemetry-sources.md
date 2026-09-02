# What the client can actually observe — from the server code

Read against EQMacEmu (`zone/attack.cpp`, `zone/mob.cpp`, `zone/groups.cpp`,
`zone/string_ids.h`, `common/eq_packet_structs.h`), which Project Quarm derives
from. This is the authority on what a Zeal client is ever *sent*; a value the
server never sends has no memory address to read.

| Thing | How it reaches the client | Exact? | Zeal source |
|---|---|---|---|
| Melee hit | `OP_Damage` (`Damage_Struct`: source, target, type=SkillType, spellid, damage) to attacker, defender and everyone in range | yes | hit callback (packet) |
| Spell/nuke damage | `OP_Damage` with `spellid`, type=Abjuration | yes | hit callback (packet) |
| **Damage shield** | `OP_Damage` from `Mob::DamageShield`: type=`DamageShieldType` (244–249), spellid, **`damage = DS` — negative** in the `DS < 0` branch. Queued to close clients (`Range:DamageMessages`) | yes | hit callback — after the sign fix in `48dd401`; the old `damage <= 0` guard dropped every one, which is why shields "never fired" |
| Healing shield (DS > 0) | no packet; `attacker->HealDamage(DS)` → the attacker gets `YOU_HEALED` | amount only to the healed | string id 419 |
| Heal received (you) | `YOU_HEALED` = **419**, "You have been healed for %1 points of damage." — the only heal message EQMacEmu sends, to the target | yes | matched by string id |
| Heal cast (you healed X for N) | **not in EQMacEmu at all**; the line exists on Quarm, so it is a Quarm addition. Whether it comes through the string table is what `/otlp debug`'s `sid=` line reveals | yes if present | text (template) until the id is known |
| Heal observed on someone else | nothing — no message, no packet with an amount | — | not observable |
| Anyone's HP but your own | `SpawnHPUpdate_Struct.cur_hp` is a **percentage** for everyone but self (`max_hp` = 100); group members get the same via `Group::SendHPPacketsFrom` | no | percent only — no heal amounts derivable from memory |
| Your own HP | exact `cur_hp` in `SpawnHPUpdate_Struct` for self | yes | memory (self struct) |
| DoT tick (yours) | `YOUR_HIT_DOT` = **9072**, "%1 has taken %2 damage from your %3." | yes | text; id available |
| Non-melee announcement | `OTHER_HIT_NONMELEE` = **434** "%1 was hit by non-melee for %2 points of damage."; `YOU_HIT_NONMELEE` = **12481** to the victim | yes | ids available; the packet already carries the same hit |
| Death | `OP_Death` (`Death_Struct`: spell_id, damage, killer) | yes | slain-line + packet |

## Consequences

- **Damage shields are a packet problem, now solved.** Nothing to pair from text.
- **Heals are structurally per-client.** Each reporter only knows heals it cast
  (Quarm's caster line) and heals it received (419). Guild-wide healing means
  every healer reporting — there is no observer channel to fill the gap.
- **String ids beat text where the server uses them.** Zeal's chat filter already
  hooks `serverGetString` (the string-table lookup) and the exporter reads the
  id in its print callback; plain messages have id 0 and keep their text parser.
- **No "memory address for heals".** Everyone else's HP arrives as a percentage.
