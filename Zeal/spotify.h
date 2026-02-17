#pragma once
#include <Windows.h>

#include <string>

#include "zeal_settings.h"

// Manages an external librespot process to provide Spotify Connect playback,
// replacing the legacy in-game MP3 player with a modern streaming integration.
// Librespot (https://github.com/librespot-org/librespot) must be available as
// an executable (librespot.exe) in the game directory or system PATH.
class SpotifyController {
 public:
  SpotifyController(class ZealService *zeal);
  ~SpotifyController();

  // Starts the librespot background process if not already running.
  bool Start();

  // Stops the librespot background process if running.
  void Stop();

  // Returns true if the librespot process is currently running.
  bool IsRunning() const;

  // Prints the current Spotify integration status to game chat.
  void PrintStatus() const;

  // Setting to enable/disable the Spotify integration (persisted in zeal.ini).
  ZealSetting<bool> Enabled = {false, "Zeal", "SpotifyEnabled", false};

  // The Spotify Connect device name advertised by librespot.
  ZealSetting<std::string> DeviceName = {"Zeal EverQuest", "Zeal", "SpotifyDeviceName", false};

  // Bitrate for audio playback (96, 160, or 320).
  ZealSetting<int> Bitrate = {160, "Zeal", "SpotifyBitrate", false};

 private:
  void callback_main();
  bool LaunchProcess();
  void CleanupProcess();

  HANDLE process_handle_ = nullptr;
  DWORD process_id_ = 0;
};
