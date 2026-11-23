#pragma once
#include <Windows.h>

#include <string>
#include <vector>

// Raid layouts: a saved arrangement of the twelve raid groups that a raid leader can apply in one
// command (/raidbars layout ...). Layouts live in zeal_raidlayouts.ini in the game folder, shared
// rather than per character, so officers can pass one file around.
//
// A layout describes slots by class, not by name, so it survives alts and absences:
//   [potime]
//   1=WAR CLR CLR SHM BRD ENC
//   2=WAR CLR|DRU PRIEST BRD ROG *
// A slot token is a class (WAR, CLR, ...), alternatives joined by '|', a role (TANK, PRIEST, MELEE,
// CASTER, HYBRID), '*' for anyone, or a character name to pin a specific person.
//
// Applying sends one #raidmove per member that has to move. The server allows that only for a guild
// officer who is the raid leader, and only one per second, so the moves are queued and paced.
//
// "apply <name> keep" is "apply <name> go" run again every few seconds: each round re-reads the
// layout from the file and re-plans against the live raid, moving whoever has joined, come back or
// drifted - and following any edit to the layout itself. Members already in a
// matching group stay put, so a settled raid produces no moves.
class RaidLayout {
 public:
  RaidLayout() = default;

  // Handles /raidbars layout ...; returns true if args[1] is "layout".
  bool ParseArgs(const std::vector<std::string> &args);

  // Main loop: sends the next queued move when its time has come.
  void Tick();

  // Zoning drops anything still queued; a kept layout resumes after a short grace period.
  void Clean();

  // Ends the layout entirely: queued moves and keep mode.
  void Stop();

 private:
  static constexpr int kNumGroups = 12;
  static constexpr int kGroupSize = 6;
  static constexpr int kUngrouped = -1;
  static constexpr ULONGLONG kMoveSpacingMs = 1200;  // Server rate limit is one move per second.
  static constexpr ULONGLONG kReconcileMs = 5000;    // How often keep mode re-plans.
  static constexpr ULONGLONG kZoneGraceMs = 10000;   // Let raid info settle after zoning.
  static constexpr int kMaxAttempts = 3;             // Moves of one member that didn't take.

  struct Member {
    std::string name;
    int class_id = 0;
    int group = kUngrouped;  // 0-11, or kUngrouped.
  };

  struct Move {
    std::string name;
    int to = kUngrouped;  // 0-11, or kUngrouped.
  };

  struct Plan {
    std::vector<std::vector<std::string>> groups;  // Per group: "Name (CLS)" in slot order, for display.
    std::vector<std::string> unfilled;              // "group 3: WIZ CLR", per group; open '*' slots left out.
    std::vector<Move> moves;                        // In an order that never moves into a full group.
    int staying = 0;
  };

  static std::vector<Member> ReadRaid();
  static bool TokenMatches(const std::string &token, int class_id);
  static int TokenSpecificity(const std::string &token);
  static bool IsNameToken(const std::string &token);

  bool LoadLayout(const std::string &name, std::vector<std::vector<std::string>> &layout, bool quiet = false) const;
  void Save(const std::string &name, bool by_name);
  void List() const;
  void Show(const std::string &name) const;
  void Apply(const std::string &name, bool go, bool keep);
  void Reconcile();
  bool BuildPlan(const std::vector<std::vector<std::string>> &layout, Plan &plan) const;
  void PrintUsage() const;

  std::vector<Move> queue;
  size_t queue_index = 0;
  ULONGLONG next_move_ms = 0;

  // Keep mode: the layout being held, and how often each member has been sent to the same group
  // without arriving there (the server refused, or someone else moved them back).
  std::string keep_name;
  ULONGLONG next_reconcile_ms = 0;
  std::vector<std::pair<Move, int>> attempts;
};
