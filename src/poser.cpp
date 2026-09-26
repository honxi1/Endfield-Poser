// Endfield Poser 插件壳。
// 依赖: core/base.h, core/il2cpp_api.h, core/gui_overlay.h, config.h
// 负责：DLL 入口、Applepie 插件协议导出、IL2CPP 解析与 GUI 线程启动。
// 后续阶段的冻结/角色捕获/相机等通过 GameFrameTick() 接入（见 Task 2.1+）。

#include <cstdint>

#include "core/base.h"
#include "core/il2cpp_api.h"
#include "core/gui_overlay.h"
#include "core/game_hooks.h"
#include "game/skeleton.h"
#include "game/accessory.h"
#include "game/freeze.h"
#include "game/char_state.h"
#include "game/morph.h"
#include "game/smc_morph.h"
#include "editor/selection.h"
#include "editor/rig_gizmo.h"
#include "editor/panel_bones.h"
#include "editor/ik_control.h"
#include "game/roster.h" // 多角色列表（依赖 ik_control.h 的 IkOnCharacterChanged）
#include "editor/panel_library.h"
#include "editor/panel_morph.h"
#include "editor/panel_agreement.h"
#include "config.h"

// 手动刷新骨骼（面板按钮 / WebUI /api/refresh 共用）
// 面板里的「打开日志」：弹资源管理器并选中 poser_log.txt —— 让非技术用户
// 一步就能把日志拖给作者（路径：<游戏目录>\plugin\poser_log.txt）
static void OpenLogInExplorer() {
  char path[MAX_PATH] = {};
  HMODULE m = GetModuleHandleA("poser.dll");
  if (!m || !GetModuleFileNameA(m, path, MAX_PATH))
    return;
  char *slash = strrchr(path, '\\');
  if (!slash)
    return;
  *slash = 0; // ...\plugin
  char cmd[MAX_PATH + 64] = {};
  snprintf(cmd, sizeof(cmd), "explorer.exe /select,\"%s\\poser_log.txt\"", path);
  Log("[POSER] open log folder: %s", path);
  WinExec(cmd, SW_SHOWNORMAL);
}

static void RefreshCharacterBones();

#include "core/web_server.h"

// ---- Applepie 插件协议（与 {EIEM}/src/applepie_mgr.h 一致）----
#define APPLEPIE_PLUGIN_API_VERSION 1
#ifdef APPLEPIE_PLUGIN_IMPL
  #define APPLEPIE_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
  #define APPLEPIE_PLUGIN_EXPORT extern "C" __declspec(dllimport)
#endif

struct AP_PluginInfo {
  int apiVersion;
  const char *id;
  const char *displayName;
  const char *description;
  const char *configFile;
  bool supportsHotDisable;
};
struct AP_HotkeyInfo {
  const char *name;
  const char *configKey;
  int currentVK;
};

static AP_PluginInfo g_info = {
    APPLEPIE_PLUGIN_API_VERSION, "poser", "Endfield Poser",
    "Game photography posing tool (FK posing + morph keys + camera; IK is experimental, off by default)",
    "plugin\\poser_config.txt", true};

APPLEPIE_PLUGIN_EXPORT AP_PluginInfo *AP_GetPluginInfo() { return &g_info; }
APPLEPIE_PLUGIN_EXPORT bool AP_PluginEnable() {
  StartGuiThread();
  return true;
}
APPLEPIE_PLUGIN_EXPORT bool AP_PluginDisable() {
  StopGuiThread();
  return true;
}
APPLEPIE_PLUGIN_EXPORT bool AP_ReloadConfig() {
  bool ok = LoadPoserConfig();
  g_ikFeatureEnabled = g_ikEnabled; // 重载配置时 IK 开关一起生效
  return ok;
}
APPLEPIE_PLUGIN_EXPORT int AP_GetHotkeys(AP_HotkeyInfo *out, int max) {
  // 向管理器声明的热键：管理器面板会列出它们，并按其 configKey 写回 poser_config.txt
  // （改完由管理器调用 AP_ReloadConfig 生效）。
  // 截图热键不声明：插件内没有实现（截图走 tools/screenshot.ps1），
  // 免得管理器里出现一个按了没反应的键；实现好了再加回来。
  const int n = 2;
  if (max < n) return n;
  out[0] = {"Toggle Poser GUI", "gui_toggle_key", g_guiToggleVK};
  out[1] = {"Freeze / Unfreeze", "freeze_key", g_freezeVK};
  return n;
}
APPLEPIE_PLUGIN_EXPORT void AP_SetLanguage(const char *) {}

// ---- 光标状态：只读游戏状态（游戏自带 Alt 呼出光标），不再强行改写 ----

static int CursorLockState() {
  if (!g_cursor_get_lockState)
    return 0;
  __try {
    void *boxed = Invoke(g_cursor_get_lockState, nullptr);
    return boxed ? *(int *)((char *)boxed + 16) : 0;
  } __except (1) {
    return 0;
  }
}

static bool CursorVisible() {
  if (!g_cursor_get_visible)
    return true;
  __try {
    void *boxed = Invoke(g_cursor_get_visible, nullptr);
    return boxed ? *(bool *)((char *)boxed + 16) : true;
  } __except (1) {
    return true;
  }
}

// 游戏侧初始化：只在 **GUI 线程**（已 attach 到 IL2CPP 域）、且用户已同意协议之后执行一次。
// 这是全插件唯一安装 hook / 起 WebUI 的地方 —— InitThread 不再 attach、不再装 hook，
// 免得抢在 Unity 运行时（GC 线程注册）就绪之前调 il2cpp_thread_attach，把游戏打成
// 「Fatal error in GC / Threads explicit registering is not previously enabled」。
static bool g_gameSideReady = false;
static bool g_legalWaitLogged = false;
// 启动方式是否可信（父进程是官方启动器 / XXMI 之类）。不可信时我们**不 attach**，
// 因为直启游戏的运行时就绪时机无法判断，attach 早一步就会把游戏打成
// "Fatal error in GC"。宁可本次不加载功能，也不能崩玩家的游戏。
static bool g_launchTrusted = true;

bool LaunchTrusted() { return g_launchTrusted; }

// 启动方式不可信时的提示窗：显示在面板上，玩家按一下面板热键即可关掉。
static bool g_launchWarnDismissed = false;
bool LaunchWarningVisible() { return !g_launchTrusted && !g_launchWarnDismissed && !g_guiAttached; }
void DismissLaunchWarning() { g_launchWarnDismissed = true; }

// 看**进程链**里有没有启动器。
//
// 不能只看直接父进程：官方启动器的链条是
//   Launcher.exe -> 1.6.0\Games.exe -> 游戏
// 直接父进程叫 Games.exe，既不含 "Launcher" 也不含 "Hypergryph"，只看一层就会把
// 正常的官方启动误判成"直启"，插件于是整场 inert（v0.3.6 的实际故障）。
// 现在改为向上最多找 6 层：命中启动器名字或启动器目录即算可信；
// 一层都命中不到、且**直接父进程是 explorer.exe**（手动双击）时才算不可信；
// 其它未知宿主放行，宁可多加载一次也不误伤。
static bool ParentLooksLikeLauncher() {
  const int kMaxProc = 4096;
  const int kMaxDepth = 6;
  static DWORD s_pids[kMaxProc];
  static DWORD s_parents[kMaxProc];
  int count = 0;

  DWORD self = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return true;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (count < kMaxProc) {
        s_pids[count] = pe.th32ProcessID;
        s_parents[count] = pe.th32ParentProcessID;
        count++;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);

  DWORD cur = 0;
  for (int i = 0; i < count; i++) {
    if (s_pids[i] == self) {
      cur = s_parents[i];
      break;
    }
  }
  DWORD firstParent = cur;

  char chain[192] = {};
  bool trusted = false;
  for (int depth = 0; cur && depth < kMaxDepth; depth++) {
    char path[MAX_PATH] = {};
    bool got = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, cur);
    if (h) {
      DWORD sz = MAX_PATH;
      got = QueryFullProcessImageNameA(h, 0, path, &sz) != 0;
      CloseHandle(h);
    }
    const char *name = "?";
    if (got) {
      const char *slash = strrchr(path, '\\');
      name = slash ? slash + 1 : path;
    }
    if (chain[0])
      strncat(chain, " <- ", sizeof(chain) - strlen(chain) - 1);
    strncat(chain, name, sizeof(chain) - strlen(chain) - 1);

    if (got) {
      if (_strnicmp(name, "Hypergryph", 9) == 0 || strstr(name, "Launcher") ||
          strstr(name, "launcher") || _strnicmp(name, "XXMI", 4) == 0 ||
          strstr(name, "xxmi") || strstr(path, "Hypergryph Launcher") ||
          strstr(path, "XXMI") || strstr(path, "3DMigoto")) {
        trusted = true;
        break;
      }
    }

    DWORD next = 0;
    for (int i = 0; i < count; i++) {
      if (s_pids[i] == cur) {
        next = s_parents[i];
        break;
      }
    }
    cur = next;
  }

  if (!trusted) {
    bool manual = false;
    if (firstParent) {
      char p[MAX_PATH] = {};
      bool got = false;
      HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, firstParent);
      if (h) {
        DWORD sz = MAX_PATH;
        got = QueryFullProcessImageNameA(h, 0, p, &sz) != 0;
        CloseHandle(h);
      }
      if (got) {
        const char *slash = strrchr(p, '\\');
        const char *n = slash ? slash + 1 : p;
        manual = _stricmp(n, "explorer.exe") == 0;
      }
    }
    trusted = !manual;
  }

  Log("[LEGAL] process chain = %s -> %s", chain[0] ? chain : "(unknown)",
      trusted ? "trusted" : "manual launch (plugin will stay inert)");
  return trusted;
}

static void MaybeInitGameSideOnce() {
  if (g_gameSideReady || !g_guiAttached)
    return;
  if (!TermsAccepted()) {
    if (!g_legalWaitLogged) {
      g_legalWaitLogged = true;
      Log("[LEGAL] waiting for the user to accept the terms before touching the game");
    }
    return;
  }
  g_gameSideReady = true;
  Log("[LEGAL] terms accepted -> installing game hooks");
  if (MH_Initialize() != MH_OK)
    Log("[POSER] WARN: MH_Initialize failed");
  InitGameHooks();       // Task 2.1：SetMainCharacter hook → 捕获 Animator/Entity
  InstallSMCFaceHooks(); // Task 4.2：SkeletalMorph 表情 hook
  StartWebServer();      // 独立 UI：localhost HTTP 服务器（浏览器打开控制窗口）
}

// 协议闸门（声明在 core/gui_overlay.h，那边要用它决定是否强制显示弹窗）：
// 未同意且用户没点「不同意」→ true；点过不同意 → false（等他按热键叫回来）。
bool TermsPending() { return !TermsAccepted() && !g_termsDeclined; }
void TermsReopen() { g_termsDeclined = false; }
// 收起弹窗并保持惰性（用户按面板热键时用）：给一条随时把游戏拿回来的退路。
void TermsDecline() { g_termsDeclined = true; }
// ---- 已同意之后的「回看」：面板里点「用户协议」打开只读窗口 ----
// 只读展示，不影响游戏侧（已装好的 hook 照常工作），按钮是「关闭」。
static bool g_termsReview = false;
bool TermsReviewVisible() { return g_termsReview; }
void TermsOpenReview() {
  g_termsReview = true;
  g_guiVisible = true;
  Log("[LEGAL] terms opened for review from the panel");
}
void TermsCloseReview() { g_termsReview = false; }
// 弹窗类窗口（首次同意流程 或 回看）是否正开着 —— 覆盖层据此决定吃不吃鼠标。
bool TermsWindowActive() { return TermsPending() || g_termsReview; }
// 指针是否落在协议窗口上（见 panel_agreement.h）
bool TermsDialogHovered() { return g_termsHover; }

// ---- 每帧更新（阶段 2+：冻结维持、骨骼列表维护、IK 写回、相机）----
void GameFrameTick() {
  // 两道闸门，缺一不可：
  //   1) 没同意协议前不碰游戏；
  //   2) 本线程还没 attach 到 IL2CPP 域时也绝对不能碰 —— 否则 Unity 会
  //      直接报 "Fatal error in GC / Collecting from unknown thread" 把游戏打崩。
  //      （面板隐藏时也会走这里，所以必须在函数入口挡，不能只在 DrawPoserGui 里挡。）
  if (!TermsAccepted() || !g_guiAttached)
    return;
  MaybeInitGameSideOnce(); // 第一次跑到这里时，在已 attach 的 GUI 线程上装 hook / 起 WebUI
  __try {
    // 光标完全交给游戏自己管：按 Alt 显示光标是游戏自带行为，插件不改它的
    // lockState/visible（强行改写会和系统按线程计数的 ShowCursor 状态打架，
    // 表现为"光标没了"）。我们只读取状态：lockState==None 视为光标可用，
    // 面板/关节才吃鼠标。
    {
      int lockNow = CursorLockState();
      bool visNow = CursorVisible();
      g_cursorFreeNow = (lockNow == 0); // 0 = CursorLockMode.None
      (void)visNow;
      // 系统光标被游戏隐藏时补一个软光标（Windows 的显示计数按线程算，指针压在
      // 游戏窗口上时可能画不出来）；游戏自己显示了就不重复画。
      bool osShown = false;
      __try {
        CURSORINFO ci;
        ci.cbSize = sizeof(ci);
        osShown = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
      } __except (1) {
      }
      ImGui::GetIO().MouseDrawCursor = g_cursorFreeNow && !osShown;
    }
    // 角色捕获自愈：SetMainCharacter hook 漏触发/时机错过时，
    // 周期性从 PlayerController 补捞当前角色（约每 2 秒一次）。
    // 骨骼数为 0 也要补捞：角色切换/场景变化后 g_charAnimator 可能残留
    // 失效指针（非空），此时重建出来是 0 根骨，必须强制重新捕获。
    static int s_captureRetry = 0;
    // 周期确认当前 Animator 还活着：场景切换/换实例后旧对象会被 Destroy，
    // 此时指针非空但已失效，骨骼列表里全是死变换 —— 画面就是"骨架钉在原地"。
    // 侦测到就丢掉捕获，交给下面的补捞逻辑重新抓当前角色。
    static int s_aliveCheck = 0;
    static int s_deadStrikes = 0;
    if (++s_aliveCheck >= 60) { // 约 1 秒（隐藏时循环 30ms 一次）
      s_aliveCheck = 0;
      bool animatorDead = g_charAnimator && !CharAnimatorAlive();
      bool bonesDead = !animatorDead && !CachedBonesAlive();
      if (animatorDead || bonesDead) {
        // 连续两次（约 2 秒）都判死才动手，避免场景加载瞬间的误判
        if (++s_deadStrikes >= 2) {
          s_deadStrikes = 0;
          Log("[POSER] capture invalid (%s) -> unfreeze + drop capture",
              animatorDead ? "animator destroyed" : "bone transforms destroyed");
          // 关键：**先解冻**。否则冻结时关掉的 Animator/IK/物理不会被还原，
          // 游戏侧的角色会一直僵在原地（而插件又已经抓不到它，无法自救）。
          if (g_frozen) {
            UnfreezeCharacter();
            RestoreBlendShapes();
          }
          ReleaseAllGrips(); // 死实例的冻结 grip 也一起清掉，避免每帧去写死对象
          g_charAnimator = nullptr;
          g_mainCharEntity = nullptr;
          g_charChanged = false; // 死实例不需要保存状态，也别触发重建双消费
          s_humanBoneCount = 0;
          s_allBones.clear();
          s_captureRetry = 60; // 下一次循环立刻补捞
        }
      } else {
        s_deadStrikes = 0;
      }
    }
    if (!g_charAnimator || s_humanBoneCount == 0) {
      if (++s_captureRetry >= 60) {
        s_captureRetry = 0;
        TryCaptureFromPlayerController();
        // 实体/动画器未变化但骨骼仍为 0：强制重建一次，等角色恢复后接上
        if (s_humanBoneCount == 0) {
          RebuildAllBones();   // 先刷全骨列表：humanoid 缺失骨按名回退依赖它
          RebuildHumanBones();
          Log("[POSER] Re-capture retry: animator=%p bones=%d",
              g_charAnimator, s_humanBoneCount);
        }
      }
    } else {
      s_captureRetry = 0;
    }
    // 冻结热键（默认 P）：面板按钮万一点不到时的可靠通道（隐藏面板时也生效）。
    // 边沿检测在 HotkeyPollThread 里做——直接用 GetAsyncKeyState 的 bit0 会被
    // 游戏/XXMI 的同键轮询抢掉锁存位（"有时有用有时没用"的根因）。
    if (TakeHotkeyFreeze()) {
      Log("[CTRL] freeze hotkey -> toggle freeze");
      if (g_frozen) {
        UnfreezeCharacter();
        RestoreBlendShapes();
      } else {
        FreezeCharacter();
      }
    }
    // 角色切换 → 统一重建 Humanoid + 从骨列表（单一消费点，避免双消费）
    if (g_charChanged) {
    g_charChanged = false;
    SaveCharStateOnSwitch(); // 先把旧角色的冻结状态存进内存表（必须在重建之前）
    s_restCaptured = false; // 新角色：A-pose 基线作废，下次重建时重捕
    RebuildAllBones();   // 先刷全骨列表：humanoid 缺失骨按名回退依赖它
    RebuildHumanBones();
    RebuildAccessories();
      RebuildBlendShapes(); // Task 4.1：形态键列表随角色重建
      ResetSMCState();      // Task 4.2：SMC 表情状态随角色重置
      ResetSkirtState();    // 裙子碰撞：清空旧角色布料采集
      if (!s_restCaptured)
        CaptureRestPose();  // 角色最初姿态 = A-pose 基线
      RestoreCharStateOnSwitch(); // 冻过的角色：恢复姿态并重新压制写者；没冻过：保持默认
      IkOnCharacterChanged();     // IK 控制器跟着换到新角色（目标点重新吸附）
    }
    // 冻结态维持：每帧强制关闭 Animator/动画组件/IK 组件（游戏会重新启用）
    MaintainFreeze();
  } __except (1) {
    Log("[POSER] GameFrameTick SEH exception caught");
  }
}

// 手动刷新：重跑角色骨骼/从骨/形态键重建链（某些场景无法切换角色时用）
static void RefreshCharacterBones() {
  if (!g_charAnimator || s_humanBoneCount == 0)
    TryCaptureFromPlayerController();
  bool newChar = g_charChanged;
  g_charChanged = false;
  if (newChar)
    s_restCaptured = false; // 新角色：重捕 A-pose 基线
  RebuildAllBones(); // 先刷全骨列表：humanoid 缺失骨按名回退依赖它
  RebuildHumanBones();
  RebuildAccessories();
  RebuildBlendShapes();
  ResetSMCState();
  ResetSkirtState();
  if (!s_restCaptured)
    CaptureRestPose();
  Log("[POSER] Manual bone refresh: human=%d", s_humanBoneCount);
}

// ---- 主面板：控制（冻结）+ 姿态编辑（Task 3.1）----
void DrawPoserGui() {
  // 启动方式不对（直启游戏）：只显示一条提示，其它什么都不做。
  // 这时插件的 IL2CPP attach 已被跳过，游戏侧完全没被碰过。
  if (LaunchWarningVisible()) {
    __try {
      ImGuiIO &io = ImGui::GetIO();
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(560.0f, 240.0f), ImGuiCond_Always);
      ImGui::Begin(u8"启动方式不对，插件已停用", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(
          u8"检测到游戏不是通过官方启动器（Hypergryph Launcher）或 XXMI 启动的。");
      ImGui::Spacing();
      ImGui::TextUnformatted(
          u8"这种启动方式下，插件接入运行时可能触发 Unity 的致命 GC 错误"
          u8"（Fatal error in GC），所以本次不加载任何功能，以免把游戏打崩。");
      ImGui::Spacing();
      ImGui::TextUnformatted(u8"请退出游戏，用官方启动器或 XXMI 重新启动。");
      ImGui::Spacing();
      {
        char hk[48] = {};
        HotkeyDisplay(g_guiToggleVK, g_guiToggleCtrl, hk, sizeof(hk));
        ImGui::TextDisabled(u8"（按面板快捷键 %s 可关闭本提示）", hk);
      }
      ImGui::PopTextWrapPos();
      ImGui::End();
    } __except (1) {
    }
    return;
  }
  // 首次使用（或条款版本更新后）：只画协议弹窗，其它面板一个都不画。
  // 同意前 GUI 线程也不会碰游戏（GameFrameTick 直接返回）。
  if (!TermsAccepted() || g_termsReview) {
    bool review = g_termsReview;
    __try {
      char hk[48] = {};
      HotkeyDisplay(g_guiToggleVK, g_guiToggleCtrl, hk, sizeof(hk));
      int r = DrawAgreementDialog(hk, review);
      if (r != 0) {
        if (review) {
          g_termsReview = false; // 只是关掉回看窗口，面板继续显示
          Log("[LEGAL] terms review closed");
        } else if (r > 0) {
          g_guiVisible = false; // 收起弹窗，回到"按热键才显示面板"的常态
          char v[16] = {};
          snprintf(v, sizeof(v), "%d", POSER_TERMS_VERSION);
          g_termsAcceptedVersion = POSER_TERMS_VERSION;
          SaveConfigValue("terms_version", v);
          Log("[LEGAL] terms v%d accepted; game hooks will be installed",
              POSER_TERMS_VERSION);
        } else {
          g_guiVisible = false;
          g_termsDeclined = true;
          Log("[LEGAL] terms declined; plugin stays inert (press the panel hotkey "
              "to show the dialog again)");
        }
      }
    } __except (1) {
      Log("[POSER] agreement dialog exception code=0x%X", GetExceptionCode());
    }
    return;
  }
  __try { GameFrameTick(); } __except (1) {
    Log("[POSER] GameFrameTick exception code=0x%X", GetExceptionCode());
  }
  ImGuizmo::BeginFrame(); // ImGuizmo 每帧初始化（draw list / 内部窗口），否则轮盘不绘制
  __try {
    DrawSkeletonOverlay();
  } __except (1) {
    Log("[POSER] DrawSkeletonOverlay exception code=0x%X", GetExceptionCode());
  }
  __try {
    HandleRigClick();
  } __except (1) {
    Log("[POSER] HandleRigClick exception code=0x%X", GetExceptionCode());
  }
  __try {
    IkSolveAll();        // 冻结态解算四肢 IK（启用中的控制器）
    DrawIkControllers(); // 目标点渲染 + 选中 + 命中标记
  } __except (1) {
    Log("[POSER] IK controllers exception code=0x%X", GetExceptionCode());
  }
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
  // 见 panel_bones.h：AlwaysAutoResize 与 SetNextItemWidth(-1) 并用时需要最小宽度，
  // 否则窗口宽度塌陷、右侧标签（步长等）被挤出可视区。
  ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 100.0f),
                                      ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::Begin("Endfield Poser", nullptr,
                   ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       (g_pinPanels ? ImGuiWindowFlags_NoMove : 0))) {
    ImGui::Text("v%s", POSER_VERSION);
    // 当前实际生效的热键（配置可能是老版本留下的值，别让用户以为"默认就是 L/P"）
    {
      char hk1[48] = {}, hk2[48] = {};
      HotkeyDisplay(g_guiToggleVK, g_guiToggleCtrl, hk1, sizeof(hk1));
      HotkeyDisplay(g_freezeVK, g_freezeCtrl, hk2, sizeof(hk2));
      ImGui::TextDisabled("\u547c\u51fa %s   \u51bb\u7ed3 %s", hk1, hk2);
      ImGui::SameLine();
      if (ImGui::SmallButton(u8"\u6253\u5f00\u65e5\u5fd7"))
        OpenLogInExplorer();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"\u5f39\u51fa\u8d44\u6e90\u7ba1\u7406\u5668\u5e76\u9009\u4e2d"
                          u8" plugin\\poser_log.txt\uff08\u53d1\u7ed9\u4f5c\u8005\u5c31"
                          u8"\u62d6\u8fd9\u4e2a\u6587\u4ef6\uff09");
      ImGui::SameLine();
      if (ImGui::SmallButton(u8"\u7528\u6237\u534f\u8bae"))
        TermsOpenReview();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"\u518d\u6b21\u67e5\u770b\u300a\u7528\u6237\u534f\u8bae\u4e0e"
                          u8"\u514d\u8d23\u58f0\u660e\u300b\uff08\u53ea\u8bfb\uff0c"
                          u8"\u4e0d\u5f71\u54cd\u4f7f\u7528\uff09");
      ImGui::SameLine();
      if (ImGui::SmallButton(u8"\u626b\u63cf\u89d2\u8272"))
        ScanSceneForCharacters();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(u8"\u626b\u63cf\u573a\u4e0a\u6240\u6709 Animator \u4e0e\u7ec4\u4ef6\uff0c"
                          u8"\u5199\u5165 plugin\\poser_roster.txt\uff08\u591a\u89d2\u8272"
                          u8"\u7f16\u8f91\u7684\u63a2\u9488\uff09");
    }
    // 只在真的装了 XXMI/3DMigoto 时才提示撞键，避免没装的用户被无谓打扰
    if (g_hotkeyConflict && g_xxmiDetected)
      ImGui::TextDisabled("\u26a0 %s", g_hotkeyConflictMsg);
    if (ImGui::CollapsingHeader(u8"\u5feb\u6377\u952e\uff08\u53ef\u6539\uff09")) {
      DrawHotkeySetting(u8"\u547c\u51fa / \u9690\u85cf\u9762\u677f",
                        "gui_toggle_key", &g_guiToggleVK, &g_guiToggleCtrl, 1);
      DrawHotkeySetting(u8"\u51bb\u7ed3 / \u89e3\u51bb", "freeze_key",
                        &g_freezeVK, &g_freezeCtrl, 2);
      if (g_hotkeyRiskyMsg[0])
        ImGui::TextDisabled("\u26a0 %s", g_hotkeyRiskyMsg);
      ImGui::TextDisabled(u8"\u70b9\u201c\u6539\u952e\u201d\u540e\u6309\u4e0b"
                          u8"\u4f60\u60f3\u7528\u7684\u7ec4\u5408\uff08\u81ea\u52a8"
                          u8"\u5199\u56de poser_config.txt\uff09");
    }
    ImGui::SameLine();
    ImGui::Checkbox(u8"\u56fe\u9489", &g_pinPanels);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"\u9501\u5b9a\u9762\u677f\u4f4d\u7f6e\uff1a\u62d6\u706b\u67f4\u4eba\u65f6\u7a97\u53e3\u4e0d\u8ddf\u7740\u52a8\uff1b\u53d6\u6d88\u540e\u53ef\u62d6\u6807\u9898\u79fb\u52a8");
    ImGui::Separator();
    ImGui::Text("Animator=%p  Bones=%d", g_charAnimator, s_humanBoneCount);
    ImGui::Separator();
    ImGui::Checkbox(u8"\u663e\u793a\u9aa8\u9abc", &g_showBones);
    ImGui::SameLine();
    ImGui::Checkbox(u8"\u9aa8\u9abc\u53c2\u6570", &g_showBoneParams);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"\u6253\u5f00\u9aa8\u9abc\u53c2\u6570\u7a97\u53e3\uff08\u65cb\u8f6c/\u4f4d\u7f6e\u6ed1\u6761\u3001\u6570\u503c\u8f93\u5165\u3001\u590d\u4f4d\u3001\u64a4\u9500\uff09");
    ImGui::SameLine();
    ImGui::TextDisabled(
        g_selectedName[0] ? g_selectedName : u8"\u672a\u9009\u4e2d");
    ImGui::Text("Bones=%d  Overlay: %s", s_humanBoneCount, g_overlayStatus);
    ImGui::Checkbox(u8"\u5168\u91cf\u9aa8\u9abc(\u5fae\u8c03)", &g_fullBones);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"\u9ed8\u8ba4\u53ea\u663e\u793a\u4e3b\u8981\u9aa8\u9abc\uff1b\u52fe\u9009\u540e\u53e0\u52a0\u5c42\u5c55\u793a/\u53ef\u62fe\u53d6\u6240\u6709\u9aa8\u9abc\uff08\u542b\u624b\u6307\u7b49\uff09\uff0c\u7528\u4e8e\u7cbe\u7ec6\u5fae\u8c03\u3002");
    if (ImGui::Checkbox(u8"IK \u63a7\u5236\u5668\uff08\u5b9e\u9a8c\u6027\uff09",
                        &g_ikFeatureEnabled)) {
      g_ikEnabled = g_ikFeatureEnabled;
      SaveConfigValue("ik_enabled", g_ikFeatureEnabled ? "1" : "0");
      Log("[IK] feature %s (toggled from the panel)", g_ikFeatureEnabled ? "ON" : "OFF");
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"\u5b9e\u9a8c\u6027\uff1a\u53ef\u4ee5\u9009\u4e2d / \u62d6\u52a8"
                        u8"\u624b\u67c4\uff1b\u9aa8\u9abc\u8ddf\u968f\u5c1a\u672a\u5b8c\u6210");
    ImGui::SameLine();
    ImGui::Checkbox(u8"\u89d2\u8272\u5217\u8868", &g_showRoster);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"\u5217\u51fa\u573a\u4e0a\u7684\u89d2\u8272\uff0c\u70b9\u8c01"
                        u8"\u7f16\u8f91\u8c01\uff08\u4e0d\u6539\u53d8\u6e38\u620f\u81ea\u5df1"
                        u8"\u64cd\u63a7\u7684\u89d2\u8272\uff09");
    if (g_fullBones && s_allBones.empty())
      RebuildAllBones();
    if (!g_fullBones && FindTransformIndex(g_selectedTransform) < 0)
      SelectTransform(nullptr, nullptr);
    ImGui::Separator();
    if (ImGui::Button(g_frozen ? "Unfreeze" : "Freeze Character")) {
      Log("[GUI] Freeze button clicked (frozen=%d animator=%p bones=%d)",
          (int)g_frozen, g_charAnimator, s_humanBoneCount);
      if (g_frozen) {
        UnfreezeCharacter();
        RestoreBlendShapes();     // 形态键恢复冻结前原始值
      } else {
        FreezeCharacter();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(u8"\u5237\u65b0\u9aa8\u9abc")) // 刷新骨骼
      RefreshCharacterBones();
    if (ImGui::Button(u8"\u5168\u90e8\u91cd\u7f6e")) // 所有骨回到冻结瞬间
      PoseOpResetToFreeze();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"\u6240\u6709\u9aa8\uff08\u542b\u4ece\u9aa8\uff09\u56de\u5230\u51bb\u7ed3\u77ac\u95f4\u59ff\u6001\uff0c\u76f8\u5f53\u4e8e\u64a4\u9500\u5168\u90e8\u624b\u52a8\u6446\u59ff");
    bool accPrev = g_freezeAccessories;
    ImGui::Checkbox(u8"\u51bb\u7ed3\u98d8\u5e26/\u88d9\u5b50/\u5934\u53d1",
                    &g_freezeAccessories);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(u8"\u9ed8\u8ba4\u5f00\uff1a\u51bb\u7ed3\u65f6\u98d8\u5e26/\u88d9\u5b50/\u5934\u53d1\u8ddf\u7740\u4e00\u8d77\u51bb\u4f4f\uff1b\u53d6\u6d88\u52fe\u9009\u5219\u4ece\u9aa8\u4fdd\u6301\u5b9e\u65f6\u6f14\u7b97");
    if (g_frozen && accPrev != g_freezeAccessories) {
      if (g_freezeAccessories) {
        if (s_accessoryChains.empty())
          RebuildAccessories();
        SetAllPhysicsEnabled(false);
      } else {
        SetAllPhysicsEnabled(true);
      }
    }
    // 快捷键一览（默认展开，可折叠）
    ImGui::Separator();
    if (ImGui::CollapsingHeader(u8"\u5feb\u6377\u952e",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      char vkbuf[16];
      ImGui::Text(u8"\u9762\u677f\u663e\u793a/\u9690\u85cf\uff1a%s",
                  VkName(g_guiToggleVK, vkbuf, sizeof(vkbuf)));
      char fbuf[16];
      ImGui::Text(u8"\u51bb\u7ed3 / \u89e3\u51bb\uff1a%s",
                  VkName(g_freezeVK, fbuf, sizeof(fbuf)));
      ImGui::Text(u8"\u9762\u677f\u4ea4\u4e92\uff1a\u6309\u4f4f Alt\uff08\u6216\u6e38\u620f\u653e\u5f00\u5149\u6807\u65f6\u76f4\u63a5\u70b9\uff09");
    }
    // 根骨骼位置微调（整体位移；冻结态直接写回）
    void *rootT = nullptr;
    for (size_t i = 0; i < s_allBones.size(); i++)
      if (s_allBones[i].parentIdx < 0) {
        rootT = s_allBones[i].transform;
        break;
      }
    if (!rootT && s_humanBoneCount > 0)
      rootT = s_humanBones[0].transform; // 回退：Hips
    if (g_frozen && rootT) {
      Vec3 lp = GetBoneLocalPos(rootT);
      // 必须用连续数组：SliderFloat3/InputFloat3 是按 &v[0] 连续写 3 个 float，
      // 之前用三个独立局部变量（&vx/&vy/&vz）不保证在栈上相邻 → 显示与写回错位。
      float rp[3] = {lp.x, lp.y, lp.z};
      bool changed = false;
      ImGui::TextDisabled(u8"\u4eba\u7269\u4f4d\u7f6e (Root XYZ)");
      ImGui::SetNextItemWidth(-1);
      changed |= ImGui::SliderFloat3(u8"##rootpos", rp, -10.0f, 10.0f, "%.2f");
      ImGui::SetNextItemWidth(-1);
      changed |= ImGui::InputFloat3(u8"##rootposin", rp, "%.4f");
      static float s_rootStep = 0.05f;
      ImGui::TextDisabled(u8"\u6b65\u957f");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(90);
      ImGui::InputFloat(u8"##rootstep", &s_rootStep, 0.0f, 0.0f, "%.3f");
      ImGui::TextDisabled("X");
      ImGui::SameLine();
      changed |= AxisStepper("rootx", &rp[0], s_rootStep);
      ImGui::SameLine();
      ImGui::TextDisabled("Y");
      ImGui::SameLine();
      changed |= AxisStepper("rooty", &rp[1], s_rootStep);
      ImGui::SameLine();
      ImGui::TextDisabled("Z");
      ImGui::SameLine();
      changed |= AxisStepper("rootz", &rp[2], s_rootStep);
      if (changed) {
        SetBoneLocalPos(rootT, Vec3{rp[0], rp[1], rp[2]});
      }
    }
  }
  ImGui::End();

  // 旋转盘：全屏无交互窗口内绘制，避免被小窗口裁剪
  {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin(u8"##rig_gizmo_layer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoInputs)) {
      __try {
        DrawBoneRotationGizmo();
      } __except (1) {
        Log("[POSER] DrawBoneRotationGizmo exception code=0x%X",
            GetExceptionCode());
      }
      __try {
        DrawIkGizmo(); // 控制器目标点的平移手柄
      } __except (1) {
        Log("[POSER] DrawIkGizmo exception code=0x%X", GetExceptionCode());
      }
    }
    ImGui::End();
    ImGui::PopStyleVar();
  }

  // 姿态预设库（独立窗口）
  ImGui::SetNextWindowPos(ImVec2(340, 500), ImGuiCond_FirstUseEver);
  // 与主面板一致：宽度有下限、高度自适应，避免内容被截断
  ImGui::SetNextWindowSizeConstraints(ImVec2(340.0f, 120.0f),
                                      ImVec2(FLT_MAX, FLT_MAX));
  if (ImGui::Begin(u8"\u59ff\u6001\u5e93", nullptr,
                   ImGuiWindowFlags_NoCollapse |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       (g_pinPanels ? ImGuiWindowFlags_NoMove : 0))) {
    DrawLibraryPanel();
  }
  ImGui::End();

  // 形态键面板（面部 BlendShape，Task 4.1）
  ImGui::SetNextWindowPos(ImVec2(680, 10), ImGuiCond_FirstUseEver);
  // 形态键面板内部用 BeginChild(size=(0,0)) 填满可用空间，和 AlwaysAutoResize 冲突
  // （子区域会塌成 0 → 内容看不见），所以这里保持固定初始尺寸、允许手动调整。
  ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
  if (ImGui::Begin(u8"\u5f62\u6001\u952e", nullptr,
                   ImGuiWindowFlags_NoCollapse |
                       (g_pinPanels ? ImGuiWindowFlags_NoMove : 0))) {
    DrawMorphPanel();
  }
  ImGui::End();

  // 骨骼层级面板（Blender 风格：树 + 搜索 + 选中骨参数）
  DrawBoneTreePanel();

  // 角色列表（多角色编辑：点谁编辑谁）
  DrawRosterPanel();
}

// 外部控制（PostMessage WM_APP+90 触发，走普通窗口消息通道）：
// 1=冻结/解冻 2=T-pose
static void ExtControl(int code) {
  // 外部控制（控制文件 / PostMessage）同样要过协议闸门：未同意前不动游戏。
  if (!TermsAccepted())
    return;
  switch (code) {
  case 1:
    if (g_frozen) {
      UnfreezeCharacter();
      RestoreBlendShapes();
    } else {
      FreezeCharacter();
    }
    break;
  case 2:
    ApplyTPose();
    break;
  default:
    break;
  }
}

// GUI 线程退出前收尾（在已 attach IL2CPP 的线程上执行）：
// 冻结状态下禁用插件/卸载时，把 Animator、IK、布料物理、形态键都还原回去，
// 否则头发布料会一直僵在冻结姿态。
static void OnGuiShutdownRestore() {
  if (!g_frozen)
    return;
  Log("[POSER] shutdown: unfreeze + restore (frozen=%d)", (int)g_frozen);
  UnfreezeCharacter();
  RestoreBlendShapes();
  ReleaseAllGrips(); // 后台还冻结着的角色也要把写者还回去
}

// 姿态文件扩展：从骨 + 形态键（skeleton.h 通过钩子调用，避免底层反向包含）
static void PoseCaptureExtras(PoseDoc &doc) {
  CollectAccessoryPoseEntries(doc.accBones);
  for (const BlendShapeSlot &s : s_blendShapes) {
    PoseMorph pm;
    pm.name = s.name;
    pm.value = s.value;
    doc.morphs.push_back(pm);
  }
}

static void PoseApplyExtras(const PoseDoc &doc) {
  int accApplied = ApplyAccessoryPoseEntries(doc.accBones);
  int morphApplied = 0;
  for (const PoseMorph &pm : doc.morphs) {
    for (BlendShapeSlot &s : s_blendShapes) {
      if (strcmp(s.name, pm.name.c_str()) != 0)
        continue;
      SetBlendShapeWeight(s, pm.value);
      morphApplied++;
      break;
    }
  }
  if (!doc.accBones.empty() || !doc.morphs.empty())
    Log("[POSER] Applied pose extras: %d/%d accessory bones, %d/%d morphs",
        accApplied, (int)doc.accBones.size(), morphApplied,
        (int)doc.morphs.size());
}

// 控制文件通道：外部（Codex）往 plugin\poser_control.txt 写命令，每帧执行后清空。
// 命令：toggle / freeze / tpose / reset
static void ProcessControlFile() {
  // 未同意协议前连控制文件都不读（它可能触发冻结/解冻等游戏侧动作）
  if (!TermsAccepted())
    return;
  FILE *f = fopen("plugin\\poser_control.txt", "r");
  if (!f)
    return;
  char line[64];
  while (fgets(line, sizeof(line), f)) {
    char *e = line + strlen(line) - 1;
    while (e > line && (*e == '\n' || *e == '\r' || *e == ' '))
      *e-- = 0;
    if (!*line)
      continue;
    if (strcmp(line, "toggle") == 0) {
      g_guiVisible = !g_guiVisible;
      Log("[CTRL] file toggle -> %d", (int)g_guiVisible);
    } else if (strcmp(line, "freeze") == 0) {
      if (g_frozen) {
        UnfreezeCharacter();
        RestoreBlendShapes();
      } else {
        FreezeCharacter();
      }
    } else if (strcmp(line, "tpose") == 0) {
      ApplyTPose();
    } else if (strcmp(line, "reset") == 0) {
      ApplyPoseSnapshot();
    } else if (strncmp(line, "select ", 7) == 0) {
      const char *boneName = line + 7;
      if (SelectBoneByName(boneName))
        Log("[CTRL] selected bone '%s' -> idx %d", boneName, g_selectedBone);
      else
        Log("[CTRL] bone not found: '%s'", boneName);
    } else if (strcmp(line, "bones") == 0) {
      Log("[CTRL] %d human bones:", s_humanBoneCount);
      for (int i = 0; i < s_humanBoneCount; i++)
        Log("[CTRL]   [%d] name='%s' human=%s", i, s_humanBones[i].name,
            HumanBoneName(s_humanBones[i].humanBone));
    } else {
      Log("[CTRL] unknown command: %s", line);
    }
  }
  fclose(f);
  remove("plugin\\poser_control.txt");
}

static DWORD WINAPI InitThread(LPVOID) {
  LoadPoserConfig();
  g_ikFeatureEnabled = g_ikEnabled; // 配置文件里的 ik_enabled 落到 IK 的功能开关上
  // 注册外部控制回调：PostMessage 通道（只发普通窗口消息，不模拟键盘 / 鼠标输入）
  SetExtControl(ExtControl);
  SetExtPollFn(ProcessControlFile);
  SetGuiShutdownFn(OnGuiShutdownRestore);
  g_poseCaptureExtras = PoseCaptureExtras;
  g_poseApplyExtras = PoseApplyExtras;
  // 等待 GameAssembly.dll 加载并让 IL2CPP 域初始化（参照 {EIEM}/src/init.h）
  while (!GetModuleHandleW(L"GameAssembly.dll"))
    Sleep(500);
  Log("[POSER] Resolving IL2CPP...");
  if (!Resolve()) {
    Log("[POSER] ERROR: GameAssembly.dll not found or exports missing");
    return 0;
  }
  // 启动方式检查：直启游戏时运行时就绪时机不可判，attach 早一步就会把游戏打崩。
  // 不可信就完全不 attach（GUI 线程仍会起，用面板告诉玩家该怎么启动）。
  g_launchTrusted = ParentLooksLikeLauncher();
  // 本线程到此为止：不 attach、不装 hook —— 解析导出只用到 GetModuleHandle/GetProcAddress，
  // 不碰托管对象。所有游戏侧动作都在 GUI 线程（先等窗口、再等域、attach 之后）执行，
  // 见 MaybeInitGameSideOnce()。
  Log("[POSER] Starting GUI thread.");
  StartGuiThread();

  return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(0);
    OpenLog("plugin\\poser_log.txt");
    // 带上编译时间：一版一测时用来确认跑的是哪次构建
    Log("[POSER] === Endfield Poser v%s attached (build %s %s) ===",
        POSER_VERSION, __DATE__, __TIME__);
    CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
  }
  return TRUE;
}
