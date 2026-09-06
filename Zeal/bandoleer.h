#pragma once

#include <string>
#include <vector>

#include "game_structures.h"
#include "io_ini.h"

// Bandoleer integrates with Melody to automatically swap instruments for each song gem.
// During the last second of a song's cast, it swaps the configured instrument in so the
// song lands with the instrument bonus. After the song lands and before the next song
// starts casting, it swaps the original weapons back.
//
// An item step in a melody ("i22", "iBreath") can have a set of its own, filed under that
// token: it is swapped in right before the click, since an instant clicky has no cast to
// wait out, and restored before the next song as usual.
class Bandoleer {
 public:
  Bandoleer(class ZealService *zeal);
  ~Bandoleer();

  // Called by Melody right BEFORE casting a new song to restore weapons from any previous swap.
  void restore_if_swapped();

  // Called by Melody right AFTER a new song starts casting to begin monitoring the cast timer.
  void notify_song_start(int gem_index);
  // Called by Melody right BEFORE clicking an item step: swaps that step's set in now, if any.
  void notify_item_step(const std::string &key);

  // Called by Melody when it ends to ensure weapons are restored.
  void notify_melody_stop();
  // True while a set is swapped in and the weapons are still to be restored.
  bool is_swapped() const { return state == State::Swapped; }

 private:
  // Equipment slot indices managed by the bandoleer (0-based InventoryItem indices).
  static constexpr int kPrimarySlot = 12;
  static constexpr int kSecondarySlot = 13;
  static constexpr int kRangeSlot = 10;
  static constexpr int kNumManagedSlots = 3;
  static constexpr int kManagedSlots[kNumManagedSlots] = {kPrimarySlot, kSecondarySlot, kRangeSlot};

  // Threshold in milliseconds before song finishes to swap instruments in.
  static constexpr DWORD kSwapThresholdMs = 300;

  enum class State { Idle, Monitoring, Swapped };

  // Tracks a single equipment slot that was swapped.
  struct SwapRecord {
    int equip_slot = -1;      // Equipment slot index (0-based).
    std::string orig_name;    // Name of the item that was displaced.
    int orig_id = 0;          // ID of the item that was displaced (0 if slot was empty).
  };

  void tick();
  void swap_instruments_in();
  void swap_weapons_back();

  void initialize_ini_filename();
  void save(const std::string &section, const std::string &label);
  void clear(const std::string &section, const std::string &label);
  void list();
  // The ini section for a gem ("Gem3") or an item step ("Item_i22"); "" for a bad token.
  static std::string section_for(const std::string &token);
  // Item-step sections are not enumerable from the ini, so their keys are kept in one list.
  std::vector<std::string> item_keys();
  void remember_item_key(const std::string &key, bool keep);

  // Searches inventory bags for an item matching the id and name. Returns bag slot id (250+) or -1.
  int find_item_in_bags(int item_id, const std::string &item_name);

  // Swaps an item from a bag slot into an equipment slot using the InvSlot click mechanism.
  bool swap_item(int bag_slot_id, int equip_slot_index);

  // Returns a display label for an equipment slot index.
  static const char *get_slot_label(int slot_index);

  // Returns true if the given gem (0-based) has any bandoleer items configured.
  bool has_config(const std::string &section);
  bool has_config_for_gem(int gem_index) { return has_config("Gem" + std::to_string(gem_index + 1)); }

  State state = State::Idle;
  std::string active_section;             // The ini section being monitored / swapped ("" if none).
  std::vector<SwapRecord> active_swaps;   // Tracks what was swapped for restoration.

  IO_ini ini = IO_ini(".\\bandoleer.ini");  // Filename updated later to per character.
};
