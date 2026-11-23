#include "raid_layout.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "game_addresses.h"
#include "game_functions.h"
#include "game_structures.h"
#include "io_ini.h"
#include "string_util.h"

namespace {

constexpr char kLayoutFilename[] = ".\\zeal_raidlayouts.ini";

std::string upper(std::string s) {
  for (auto &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string lower(std::string s) {
  for (auto &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> split_tokens(const std::string &line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == ' ' || c == '\t' || c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Class ids are the game's: Warrior = 1 through Beastlord = 15.
int class_from_short(const std::string &token) {
  for (int id = Zeal::GameEnums::ClassTypes::Warrior; id <= Zeal::GameEnums::ClassTypes::Beastlord; ++id)
    if (upper(token) == Zeal::Game::class_name_short(id)) return id;
  return 0;
}

// Role words cover the usual raid vocabulary. They are deliberately broad; a slot that has to be one
// class should name it.
bool role_matches(const std::string &role, int class_id) {
  using namespace Zeal::GameEnums;
  switch (class_id) {
    case ClassTypes::Warrior:
      return role == "TANK" || role == "MELEE";
    case ClassTypes::Paladin:
    case ClassTypes::Shadowknight:
      return role == "TANK" || role == "MELEE" || role == "HYBRID";
    case ClassTypes::Ranger:
    case ClassTypes::Bard:
    case ClassTypes::Beastlord:
      return role == "MELEE" || role == "HYBRID";
    case ClassTypes::Monk:
    case ClassTypes::Rogue:
      return role == "MELEE";
    case ClassTypes::Cleric:
    case ClassTypes::Druid:
    case ClassTypes::Shaman:
      return role == "PRIEST";
    case ClassTypes::Wizard:
    case ClassTypes::Magician:
    case ClassTypes::Necromancer:
    case ClassTypes::Enchanter:
      return role == "CASTER";
  }
  return false;
}

bool is_role(const std::string &token) {
  const std::string t = upper(token);
  return t == "TANK" || t == "PRIEST" || t == "MELEE" || t == "CASTER" || t == "HYBRID";
}

}  // namespace

bool RaidLayout::IsNameToken(const std::string &token) {
  if (token == "*" || is_role(token) || token.find('|') != std::string::npos) return false;
  return class_from_short(token) == 0;
}

bool RaidLayout::TokenMatches(const std::string &token, int class_id) {
  if (token == "*") return true;
  for (const auto &alt : Zeal::String::split(token, "|")) {
    if (alt.empty()) continue;
    if (is_role(alt) ? role_matches(upper(alt), class_id) : class_from_short(alt) == class_id) return true;
  }
  return false;
}

// Lower fills first: a single class before alternatives, alternatives before roles, '*' last, so a
// broad slot never takes the only member who could fill a narrow one.
int RaidLayout::TokenSpecificity(const std::string &token) {
  if (token == "*") return 3;
  if (token.find('|') != std::string::npos) return 1;
  if (is_role(token)) return 2;
  return 0;
}

std::vector<RaidLayout::Member> RaidLayout::ReadRaid() {
  std::vector<Member> members;
  Zeal::GameStructures::RaidInfo *raid_info = Zeal::Game::RaidInfo;
  if (!raid_info || !raid_info->is_in_raid()) return members;
  for (int i = 0; i < Zeal::GameStructures::RaidInfo::kRaidMaxMembers; ++i) {
    const auto &m = raid_info->MemberList[i];
    if (!m.Name[0]) continue;
    const DWORD g = m.GroupNumber;
    members.push_back(Member{.name = m.Name,
                             .class_id = m.ClassValue,
                             .group = (g < static_cast<DWORD>(kNumGroups)) ? static_cast<int>(g) : kUngrouped});
  }
  std::sort(members.begin(), members.end(), [](const Member &a, const Member &b) { return a.name < b.name; });
  return members;
}

bool RaidLayout::LoadLayout(const std::string &name, std::vector<std::vector<std::string>> &layout,
                            bool quiet) const {
  IO_ini ini(kLayoutFilename);
  layout.assign(kNumGroups, {});
  bool any = false;
  for (int g = 0; g < kNumGroups; ++g) {
    const std::string key = std::to_string(g + 1);
    if (!ini.exists(name, key)) continue;
    auto tokens = split_tokens(ini.getValue<std::string>(name, key));
    if (tokens.size() > kGroupSize) {
      if (!quiet)
        Zeal::Game::print_chat("Layout %s: group %d lists %d slots, only the first %d count.", name.c_str(), g + 1,
                             static_cast<int>(tokens.size()), kGroupSize);
      tokens.resize(kGroupSize);
    }
    if (!tokens.empty()) any = true;
    layout[g] = tokens;
  }
  if (!any && !quiet) Zeal::Game::print_chat("No layout named %s in zeal_raidlayouts.ini.", name.c_str());
  return any;
}

// Assigns members to slots. Pinned names first, then slots from narrow to broad. Each slot first
// looks for a match already in that group, so a raid that is nearly right only moves the difference.
bool RaidLayout::BuildPlan(const std::vector<std::vector<std::string>> &layout, Plan &plan) const {
  std::vector<Member> members = ReadRaid();
  if (members.empty()) {
    Zeal::Game::print_chat("You are not in a raid.");
    return false;
  }

  struct Slot {
    int group;
    std::string token;
    int member = -1;
  };
  std::vector<Slot> slots;
  for (int g = 0; g < kNumGroups; ++g)
    for (const auto &t : layout[g]) slots.push_back(Slot{.group = g, .token = t});

  std::vector<int> target(members.size(), -2);  // -2: not placed by the layout.

  for (auto &slot : slots) {
    if (!IsNameToken(slot.token)) continue;
    for (size_t i = 0; i < members.size(); ++i) {
      if (target[i] == -2 && lower(members[i].name) == lower(slot.token)) {
        slot.member = static_cast<int>(i);
        target[i] = slot.group;
        break;
      }
    }
  }

  std::vector<Slot *> order;
  for (auto &slot : slots)
    if (!IsNameToken(slot.token)) order.push_back(&slot);
  std::stable_sort(order.begin(), order.end(), [](const Slot *a, const Slot *b) {
    return TokenSpecificity(a->token) < TokenSpecificity(b->token);
  });

  for (int pass = 0; pass < 2; ++pass) {  // Pass 0: only members already in the slot's group.
    for (Slot *slot : order) {
      if (slot->member >= 0) continue;
      for (size_t i = 0; i < members.size(); ++i) {
        if (target[i] != -2 || !TokenMatches(slot->token, members[i].class_id)) continue;
        if (pass == 0 && members[i].group != slot->group) continue;
        slot->member = static_cast<int>(i);
        target[i] = slot->group;
        break;
      }
    }
  }

  // Members the layout did not place leave the groups it defines; groups it leaves blank are not
  // touched, so a layout for groups 1-6 does not disturb whoever sits in 7-12.
  for (size_t i = 0; i < members.size(); ++i) {
    if (target[i] != -2) continue;
    const int g = members[i].group;
    target[i] = (g != kUngrouped && !layout[g].empty()) ? kUngrouped : g;
  }

  // Unfilled slots are reported one line per group. An open '*' slot is just spare room, not a
  // gap, so it isn't reported at all (a big layout's filler groups otherwise flood the chat).
  plan.groups.assign(kNumGroups, {});
  std::vector<std::string> missing(kNumGroups);
  for (const auto &slot : slots) {
    if (slot.member < 0) {
      if (slot.token != "*") missing[slot.group] += (missing[slot.group].empty() ? "" : " ") + slot.token;
      continue;
    }
    const auto &m = members[slot.member];
    plan.groups[slot.group].push_back(m.name + " (" + Zeal::Game::class_name_short(m.class_id) + ")");
  }
  for (int g = 0; g < kNumGroups; ++g)
    if (!missing[g].empty()) plan.unfilled.push_back("group " + std::to_string(g + 1) + ": " + missing[g]);

  // Order the moves against a running count per group: a move only goes out once its destination
  // has room. Leaving moves (to ungrouped, or out of a group) come first and make that room.
  std::array<int, kNumGroups> count{};
  std::vector<int> pending;
  for (size_t i = 0; i < members.size(); ++i) {
    if (members[i].group != kUngrouped) count[members[i].group]++;
    if (target[i] == members[i].group)
      plan.staying++;
    else
      pending.push_back(static_cast<int>(i));
  }
  std::vector<int> current(members.size());
  for (size_t i = 0; i < members.size(); ++i) current[i] = members[i].group;

  while (!pending.empty()) {
    bool progressed = false;
    for (auto it = pending.begin(); it != pending.end();) {
      const int i = *it;
      const int to = target[i];
      if (to == kUngrouped || count[to] < kGroupSize) {
        if (current[i] != kUngrouped) count[current[i]]--;
        if (to != kUngrouped) count[to]++;
        current[i] = to;
        plan.moves.push_back(Move{.name = members[i].name, .to = to});
        it = pending.erase(it);
        progressed = true;
      } else {
        ++it;
      }
    }
    if (progressed) continue;
    // Every remaining move waits on a full group: park one waiting member as ungrouped to break
    // the cycle. It costs that member one extra move.
    const int i = pending.front();
    if (current[i] != kUngrouped) {
      count[current[i]]--;
      current[i] = kUngrouped;
      plan.moves.push_back(Move{.name = members[i].name, .to = kUngrouped});
    } else {
      pending.erase(pending.begin());  // Destination full and nothing to free: give up on this one.
    }
  }
  return true;
}

void RaidLayout::Apply(const std::string &name, bool go, bool keep) {
  std::vector<std::vector<std::string>> layout;
  if (!LoadLayout(name, layout)) return;
  Plan plan;
  if (!BuildPlan(layout, plan)) return;

  for (int g = 0; g < kNumGroups; ++g) {
    if (layout[g].empty() || plan.groups[g].empty()) continue;  // Nobody to show.
    std::string line;
    for (const auto &entry : plan.groups[g]) line += (line.empty() ? "" : ", ") + entry;
    Zeal::Game::print_chat("Group %d: %s", g + 1, line.c_str());
  }
  for (const auto &u : plan.unfilled) Zeal::Game::print_chat("Unfilled: %s", u.c_str());
  Zeal::Game::print_chat("Layout %s: %d moves, %d already in place.", name.c_str(),
                         static_cast<int>(plan.moves.size()), plan.staying);

  if (!go) {
    if (!plan.moves.empty())
      Zeal::Game::print_chat("Preview only. /raidbars layout apply %s go sends the moves.", name.c_str());
    return;
  }

  Zeal::GameStructures::RaidInfo *raid_info = Zeal::Game::RaidInfo;
  if (!raid_info->IsLeader) {
    Zeal::Game::print_chat("Only the raid leader can move raid members.");
    return;
  }
  Stop();
  queue = plan.moves;
  if (!queue.empty())
    Zeal::Game::print_chat("Sending %d moves, about %d seconds. /raidbars layout stop cancels.",
                           static_cast<int>(queue.size()),
                           static_cast<int>((queue.size() * kMoveSpacingMs + 999) / 1000));
  if (keep) {
    keep_name = name;
    next_reconcile_ms = GetTickCount64() + kReconcileMs;
    Zeal::Game::print_chat("Keeping layout %s: members who join or return are placed automatically. "
                           "/raidbars layout stop ends it.",
                           name.c_str());
  }
}

// Keep mode: re-plan against the live raid and queue whatever moved out of line. Runs only while
// nothing is queued, so a round always finishes before the next is planned.
void RaidLayout::Reconcile() {
  Zeal::GameStructures::RaidInfo *raid_info = Zeal::Game::RaidInfo;
  if (!raid_info->is_in_raid() || !raid_info->IsLeader) {
    Zeal::Game::print_chat("Raid layout %s no longer kept: you are not the raid leader.", keep_name.c_str());
    Stop();
    return;
  }
  // Re-read the file every round, exactly as "apply <name> go" would, so edits to the layout take
  // effect mid-raid. Warnings were shown by the first apply; they aren't repeated every round.
  std::vector<std::vector<std::string>> layout;
  if (!LoadLayout(keep_name, layout, true)) {
    Zeal::Game::print_chat("Raid layout %s no longer kept: it is gone from zeal_raidlayouts.ini.", keep_name.c_str());
    Stop();
    return;
  }
  Plan plan;
  if (!BuildPlan(layout, plan)) return;

  // A member sent to the same group kMaxAttempts times without arriving is left alone, so a
  // refused move (or a fight with someone else's /raidbars autoregroup) doesn't loop forever.
  std::vector<std::pair<Move, int>> next_attempts;
  queue.clear();
  queue_index = 0;
  for (const auto &move : plan.moves) {
    int count = 0;
    for (const auto &[m, c] : attempts)
      if (m.name == move.name && m.to == move.to) count = c;
    if (count < kMaxAttempts) queue.push_back(move);
    if (count == kMaxAttempts) {  // Said once, on the first round it is skipped.
      if (move.to == kUngrouped)
        Zeal::Game::print_chat("Raid layout: %s won't leave their group, leaving them.", move.name.c_str());
      else
        Zeal::Game::print_chat("Raid layout: %s won't move to group %d, leaving them.", move.name.c_str(),
                               move.to + 1);
    }
    next_attempts.push_back({move, (std::min)(count + 1, kMaxAttempts + 1)});
  }
  attempts = std::move(next_attempts);  // Members now in place drop out and start fresh.
  if (!queue.empty())
    Zeal::Game::print_chat("Raid layout %s: placing %d member%s.", keep_name.c_str(), static_cast<int>(queue.size()),
                           queue.size() == 1 ? "" : "s");
}

void RaidLayout::Tick() {
  const ULONGLONG now = GetTickCount64();
  if (queue_index >= queue.size()) {
    if (!keep_name.empty() && now >= next_reconcile_ms) {
      next_reconcile_ms = now + kReconcileMs;
      Reconcile();
    }
    return;
  }
  if (now < next_move_ms) return;

  Zeal::GameStructures::RaidInfo *raid_info = Zeal::Game::RaidInfo;
  if (!raid_info->is_in_raid() || !raid_info->IsLeader) {
    Zeal::Game::print_chat("Raid layout stopped: no longer the raid leader.");
    Stop();
    return;
  }

  const Move move = queue[queue_index++];
  // The raid may have changed since the plan: skip a member who left, and a move into a group
  // that filled up meanwhile, rather than spend the rate limit on a rejection.
  bool present = false;
  for (const auto &m : ReadRaid())
    if (m.name == move.name) present = true;
  if (!present) return;
  if (move.to != kUngrouped && Zeal::Game::get_raid_group_count(move.to) >= kGroupSize) {
    Zeal::Game::print_chat("Raid layout: group %d is full, %s not moved.", move.to + 1, move.name.c_str());
    return;
  }

  Zeal::Game::do_say(true, "#raidmove %s %d", move.name.c_str(), move.to == kUngrouped ? 0 : move.to + 1);
  next_move_ms = now + kMoveSpacingMs;
  if (queue_index >= queue.size()) {
    if (keep_name.empty()) Zeal::Game::print_chat("Raid layout: all moves sent.");
    queue.clear();
    queue_index = 0;
    // Give the server time to report the last moves before keep mode re-plans.
    next_reconcile_ms = now + kReconcileMs;
  }
}

void RaidLayout::Clean() {
  queue.clear();
  queue_index = 0;
  next_move_ms = 0;
  next_reconcile_ms = GetTickCount64() + kZoneGraceMs;
}

void RaidLayout::Stop() {
  Clean();
  keep_name.clear();
  attempts.clear();
}

void RaidLayout::Save(const std::string &name, bool by_name) {
  std::vector<Member> members = ReadRaid();
  if (members.empty()) {
    Zeal::Game::print_chat("You are not in a raid.");
    return;
  }
  IO_ini ini(kLayoutFilename);
  ini.deleteSection(name);
  int saved = 0;
  for (int g = 0; g < kNumGroups; ++g) {
    std::string line;
    for (const auto &m : members) {
      if (m.group != g) continue;
      line += (line.empty() ? "" : " ") + (by_name ? m.name : Zeal::Game::class_name_short(m.class_id));
    }
    if (line.empty()) continue;
    ini.setValue<std::string>(name, std::to_string(g + 1), line);
    saved++;
  }
  Zeal::Game::print_chat("Saved %d groups as layout %s (%s) in zeal_raidlayouts.ini.", saved, name.c_str(),
                         by_name ? "by name" : "by class");
}

void RaidLayout::List() const {
  IO_ini ini(kLayoutFilename);
  auto names = ini.getSectionNames();
  if (names.empty()) {
    Zeal::Game::print_chat("No layouts yet. /raidbars layout save <name> stores the current raid.");
    return;
  }
  std::string line;
  for (const auto &n : names) line += (line.empty() ? "" : ", ") + n;
  Zeal::Game::print_chat("Raid layouts: %s", line.c_str());
}

void RaidLayout::Show(const std::string &name) const {
  std::vector<std::vector<std::string>> layout;
  if (!LoadLayout(name, layout)) return;
  for (int g = 0; g < kNumGroups; ++g) {
    if (layout[g].empty()) continue;
    std::string line;
    for (const auto &t : layout[g]) line += (line.empty() ? "" : " ") + t;
    Zeal::Game::print_chat("%s group %d: %s", name.c_str(), g + 1, line.c_str());
  }
}

void RaidLayout::PrintUsage() const {
  Zeal::Game::print_chat("Usage: /raidbars layout list");
  Zeal::Game::print_chat("Usage: /raidbars layout show <name>");
  Zeal::Game::print_chat("Usage: /raidbars layout save <name> [names] (by class unless 'names')");
  Zeal::Game::print_chat("Usage: /raidbars layout apply <name> [go|keep] (preview unless go; keep holds the layout)");
  Zeal::Game::print_chat("Usage: /raidbars layout stop");
}

bool RaidLayout::ParseArgs(const std::vector<std::string> &args) {
  if (args.size() < 2 || args[1] != "layout") return false;

  const std::string sub = args.size() >= 3 ? args[2] : "";
  if (sub == "list" && args.size() == 3) {
    List();
  } else if (sub == "show" && args.size() == 4) {
    Show(args[3]);
  } else if (sub == "save" && (args.size() == 4 || (args.size() == 5 && args[4] == "names"))) {
    Save(args[3], args.size() == 5);
  } else if (sub == "apply" && (args.size() == 4 || (args.size() == 5 && (args[4] == "go" || args[4] == "keep")))) {
    const bool keep = args.size() == 5 && args[4] == "keep";
    Apply(args[3], args.size() == 5, keep);
  } else if (sub == "stop" && args.size() == 3) {
    const bool running = queue_index < queue.size() || !keep_name.empty();
    Stop();
    Zeal::Game::print_chat(running ? "Raid layout stopped." : "No raid layout running.");
  } else {
    PrintUsage();
  }
  return true;
}
