#pragma once
#include <atomic>
#include <mutex>

// Editor mutations can race the game's expression callbacks. Callbacks never
// wait for this lock: Unity must be free to complete editor-side invocations.
static std::recursive_mutex g_poseMutex;
static std::atomic<bool> s_smcClosing{false}, s_smcEnabled{true};
static std::atomic<bool> g_characterCapturePending{false};
static std::atomic<unsigned> g_characterSwitchDepth{0};
static std::atomic<void *> g_pendingPlayerController{nullptr};
static bool SMCCharacterSwitchPending() {
  return g_characterCapturePending.load() || g_characterSwitchDepth.load()!=0;
}
static bool SMCRuntimeClosing() { return s_smcClosing.load(); }
static bool SMCEnabled() { return s_smcEnabled.load() && !SMCRuntimeClosing(); }
