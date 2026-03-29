#include "spotify.h"

#include <Windows.h>

#include <sstream>
#include <string>

#include "callbacks.h"
#include "game_functions.h"
#include "string_util.h"
#include "zeal.h"

// Attempts to launch the librespot executable as a child process.
bool SpotifyController::LaunchProcess() {
  std::string device_name = DeviceName.get();
  int bitrate = Bitrate.get();

  // Validate bitrate to one of the supported values.
  if (bitrate != 96 && bitrate != 160 && bitrate != 320) bitrate = 160;

  // Build the command line for librespot.
  std::ostringstream cmd;
  cmd << "librespot.exe"
      << " --name \"" << device_name << "\""
      << " --bitrate " << bitrate
      << " --backend rodio"
      << " --enable-volume-normalisation";

  std::string cmd_str = cmd.str();

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;  // Run hidden in the background.
  PROCESS_INFORMATION pi = {};

  // CreateProcessA needs a mutable command line buffer.
  std::vector<char> cmd_buf(cmd_str.begin(), cmd_str.end());
  cmd_buf.push_back('\0');

  BOOL success = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  if (!success) {
    DWORD error = GetLastError();
    Zeal::Game::print_chat("[Spotify] CreateProcess failed (error %lu).", error);
    return false;
  }

  process_handle_ = pi.hProcess;
  process_id_ = pi.dwProcessId;

  // We don't need the thread handle.
  if (pi.hThread) CloseHandle(pi.hThread);
  return true;
}

// Cleans up the librespot child process and associated handles.
void SpotifyController::CleanupProcess() {
  if (process_handle_) {
    // Attempt graceful shutdown first, then force terminate if needed.
    GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process_id_);
    if (WaitForSingleObject(process_handle_, 2000) != WAIT_OBJECT_0) {
      TerminateProcess(process_handle_, 0);
      WaitForSingleObject(process_handle_, 1000);
    }
    CloseHandle(process_handle_);
    process_handle_ = nullptr;
    process_id_ = 0;
  }
}

bool SpotifyController::Start() {
  if (IsRunning()) {
    Zeal::Game::print_chat("[Spotify] Already running.");
    return true;
  }

  if (!LaunchProcess()) {
    Zeal::Game::print_chat("[Spotify] Failed to start librespot. Make sure librespot.exe is in the game directory.");
    return false;
  }

  Enabled.set(true);
  Zeal::Game::print_chat("[Spotify] Started. Use Spotify Connect to select device: %s",
                         DeviceName.get().c_str());
  return true;
}

void SpotifyController::Stop() {
  CleanupProcess();
  Enabled.set(false);
  Zeal::Game::print_chat("[Spotify] Stopped.");
}

bool SpotifyController::IsRunning() const {
  if (!process_handle_) return false;
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process_handle_, &exit_code)) return false;
  return exit_code == STILL_ACTIVE;
}

void SpotifyController::PrintStatus() const {
  if (IsRunning()) {
    Zeal::Game::print_chat("[Spotify] Running (device: %s, bitrate: %d, pid: %lu)",
                           DeviceName.get().c_str(), Bitrate.get(), process_id_);
  } else {
    Zeal::Game::print_chat("[Spotify] Not running. Use /spotify start");
  }
}

void SpotifyController::callback_main() {
  // Periodic check: if the setting is enabled but the process died, notify the user once.
  if (Enabled.get() && process_handle_ && !IsRunning()) {
    if (!notified_crash_) {
      Zeal::Game::print_chat("[Spotify] librespot process has exited unexpectedly.");
      notified_crash_ = true;
    }
    CleanupProcess();
    Enabled.set(false);
  }
  if (!Enabled.get()) notified_crash_ = false;
}

SpotifyController::SpotifyController(ZealService *zeal) {
  zeal->callbacks->AddGeneric([this]() { callback_main(); });
}

SpotifyController::~SpotifyController() { CleanupProcess(); }
