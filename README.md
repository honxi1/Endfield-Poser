# Endfield Poser

《明日方舟：终末地》的游戏内摆姿插件：把角色冻结在当前姿态，用 3D 旋转盘和参数面板直接摆姿势，
保存 / 载入姿态预设，方便游戏内取景与后续参考。

> ⚠️ 使用前请先读文末的[用户协议与免责声明](#用户协议与免责声明)（含开源许可、内容合规与账号风险说明）。

- 下载：[Releases](https://github.com/honxi1/Endfield-Poser/releases/latest)
- 依赖全部自包含在 `deps/`，运行时不联网；本仓库整体按 **AGPL-3.0** 发布（[LICENSE](LICENSE)），
  第三方来源与依赖见 [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)
- 功能概览与后续计划：[docs/roadmap.md](docs/roadmap.md)

## 下载与安装

从 [Releases](https://github.com/honxi1/Endfield-Poser/releases) 下载最新的 `EndfieldPoser-v*.zip`，解压后按目录对应放置（也可以直接双击包里的 `安全安装.bat` 走向导）：

| 包内文件 | 放到 |
|---|---|
| `d3dcompiler_47.dll` | 游戏根目录（**先备份游戏自带的那份**） |
| `vulkan-1.dll`（可选） | 游戏根目录——DX/Vulkan 代理放一个或都放均可 |
| `plugin\poser.dll` | 游戏根目录的 `plugin\` |
| `plugin\poser_config.txt` | 游戏根目录的 `plugin\` |

> 📁 **注意是 `plugin\`（单数，插件目录），不是游戏自带的 `plugins\`（复数）**。
> 后者是游戏的 Qt 插件目录（里面是 `imageformats/`、`platforms/` 这些），放进去不会生效。
> 如果游戏目录下没有 `plugin\` 文件夹，自己新建一个。

> ⚠️ **请用官方启动器（Hypergryph Launcher）或 XXMI 启动**。这两种都已实测可用
> （日志里能看到插件由哪个代理拉起、`[GUI] attached to IL2CPP domain`）。
> **不要直接双击 `Endfield.exe`**：那条路径会跳过官方初始化流程；
> 早期版本还会在 IL2CPP 就绪前 attach 而触发 Unity GC 致命错误
> （`Threads explicit registering is not previously enabled` / `Collecting from unknown thread`，
> 同时写进游戏根目录的 `Endfield.gc.log`）—— 现在插件会等运行时就绪再 attach，超时就安静不加载、
> 不再把游戏带崩，但直启仍不是推荐路径。

> 📜 **首次启动会弹出「用户协议与免责声明」**：条款要滚到底，「同意并继续」才会变成可点
> （**看不到鼠标就按住 Alt** 呼出游戏光标）。**没点同意之前，插件不会安装任何游戏侧 hook、
> 也不会读写游戏数据**；点「不同意」则本次不加载任何功能（按面板快捷键可把窗口叫回来，
> 不必重启游戏）。同意后会写一行 `terms_version` 进 `plugin\poser_config.txt`，以后不再打扰；
> 只有条款版本更新时才会再弹一次。

渲染 API 说明：插件与游戏用的 API 无关（面板是自建的 D3D11 + DirectComposition 透明窗口）。
**DX11 与 Vulkan 两种模式都已实测可用**；用 Vulkan 时请确保 `vulkan-1.dll`（本包的代理）也在
游戏根目录，并使用「窗口化 / 无边框全屏」——独占全屏会绕过 DWM 合成，面板会看不见。
日志里会标明插件由哪个代理拉起：`[PROXY] plugins loaded via d3dcompiler_47.dll (DX path)`
或 `[PROXY] plugins loaded via vulkan-1.dll (Vulkan path)`。

## 使用

> 完整图文流程见 **[docs/tutorial.md](docs/tutorial.md)**（安装 → 冻结 → 摆姿 → 表情 → 姿态库 → 排查）。

| 操作 | 说明 |
|---|---|
| `L` | 呼出 / 隐藏面板（主面板 `快捷键（可改）` 一键改键，或改 `poser_config.txt`） |
| `P` | 冻结 / 解冻 |
| 按住 `Alt` | 光标归面板（游戏自己放开光标时——例如摄影模式——直接点即可） |

> **为什么不用 F11/F12（连 Ctrl+F12 也不行）**：XXMI / 3DMigoto 是直接轮询 F11/F12 的
> 按键状态，你按 `Ctrl+F12` 它们照样会触发自己的动作 —— 只有完全不碰 F 键才躲得掉，所以默认用 `L` / `P`。
> 代价是游戏内文本框/聊天里打字可能误触发（插件自己面板的输入框已屏蔽）。想换键：点
> `快捷键（可改）` → `改键` → 直接按（自动写回 `poser_config.txt`，`Esc` 取消）。
> 单键（含字母）也允许绑，但游戏内打字会误触发——绑了单键主面板会提醒，建议用带 Ctrl 的组合。

典型流程：进游戏 → `Ctrl+F11` 冻结 → 在 3D 视图里点选骨骼（勾「全量骨骼(微调)」可点到从骨与手指）→
拖旋转盘或调参数 → 命名并保存姿态。

- 姿态文件：`<游戏目录>\plugin\poses\*.poser.json`（含 humanoid 骨、从骨与面部形态键）
- 日志：`<游戏目录>\plugin\poser_log.txt` —— **排查问题先看这里**
- WebUI：插件启动后监听 `http://127.0.0.1:18923`

## 功能

- **角色冻结**：关闭 Animator 并抑制 FinalIK / 布料等写者，每帧维持；可选"冻结飘带/裙子/头发"（默认开），取消勾选则从骨保持实时演算。
- **多角色**：冻结状态按角色记忆——切到没冻过的角色时它保持默认（正常动），切回冻过的角色会**恢复你离开时的姿势**；已经在后台的冻结角色不会被游戏重新启用。当前只有"当前角色"可编辑。
- **摆姿编辑**：3D 点选骨骼 + ImGuizmo 旋转盘（点空白处取消选中）；
  旋转 / 位置参数支持滑条、数值输入、± 步进与复位。
- **人物位置**：Root XYZ 的滑条、精确输入，以及可调步长的 ± 步进。
- **从骨控制**：头发、裙子、飘带等从骨随冻结钉住；手动编辑会同步冻结基线，不会被每帧回写打回。
- **姿态预设**：命名保存 / 覆盖 / 加载 / 删除，格式含从骨与形态键，旧格式文件仍可读。
- **形态键与表情**：面部 BlendShape 面板 + 游戏原生 SMC 表情。滑条值恒为**相对"中性默认脸"**的权重
  （默认脸等价于面部 A-pose，自动采样，不是冻结那一刻的脸）；**冻结会保持当前表情**（把此刻的脸
  反解成滑条值），`全部归零` 回到默认脸，`读入当前表情` 把游戏当前表情读进滑条。
- **输入路由**：覆盖层常驻并真穿透（`click_through=1`），只有指针落在面板 / 关节上且光标可用时才接管；点击输入框可直接打字，失焦后键盘立刻还给游戏。

## 配置（`plugin\poser_config.txt`）

```
gui_toggle_key=L          # 支持 L / CTRL+L / VK_F12 / 0x7B 这类写法
freeze_key=P              # 冻结 / 解冻（写法同上）
click_through=1           # 1=覆盖层常驻并真穿透（推荐）；0=按住 Alt 才显示面板
overlay_mode=0            # 0=自动（检测到 XXMI/3DMigoto 时改用分层窗口）；1=强制 DComp；2=强制分层窗口
overlay_fps=60            # 分层窗口路径的呈现帧率上限（0=不限；mod 多/机器吃紧可降到 30）
default_pose_dir=plugin\poses
```

`default_pose_dir` 支持绝对路径，或相对游戏根目录的路径；面板底部会显示当前保存位置。

## 构建

### Windows（插件本体，MSVC）

```powershell
# 本机（VS 18 Insiders、无 cmake、无系统 Windows SDK）：
powershell -ExecutionPolicy Bypass -File tools\setup_winsdk.ps1   # 首次：拉取 Windows SDK 到 deps/（需联网）
powershell -ExecutionPolicy Bypass -File tools\build_msvc.ps1     # 编译 plugin/ 并跑三个数学单测

# 有 cmake + VS 工具链时：
build.bat
```

两种方式产物都落在 `plugin/`：`poser.dll`（插件）、`d3dcompiler_47.dll`、`vulkan-1.dll`（代理）。

### Linux / 沙箱（数学层单测）

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

测试覆盖 `math/` 层（`test_quat` / `test_ik` / `test_pose_file`）；插件本体只能在 Windows + 游戏内验证。

## 目录结构

```
endfield-poser/
├── CMakeLists.txt        # Windows: 插件 DLL + 代理 DLL；tests: 数学单测
├── build.bat             # 有 cmake 时的一键构建（否则回退 build_msvc.ps1）
├── deps/                 # 自包含第三方：imgui / imguizmo / minhook_lib / json
├── tools/                # build_msvc.ps1、setup_winsdk.ps1、screenshot.ps1 等
├── src/
│   ├── poser.cpp         # DLL 入口 + 插件宿主协议 + 每帧调度 + 主面板
│   ├── config.h          # poser_config.txt 读写、热键解析
│   ├── core/             # base / il2cpp_api / game_hooks / gui_overlay / web_server / 代理 DLL
│   ├── math/             # quat_math / ik_two_bone / pose_file（纯 C++，可单测）
│   ├── game/             # skeleton / accessory / freeze / cloth / morph / smc_morph
│   └── editor/           # selection / rig_gizmo / panel_bones / panel_library / panel_morph
├── tests/                # math 层单测（g++ 亦可跑）
└── docs/                 # tutorial.md（图文教程）、roadmap.md（功能与计划）
```

## 已知问题

- **必须经启动器启动**（原因见上）。
- **表情不能跨角色通用**：本作面部由 SMC（骨骼变形）驱动，网格上没有 BlendShape 目标，
  而 SMC 权重目前不写入姿态文件——所以保存下来的姿态只含骨骼与从骨，表情需要在新角色上重调；
  强行套用其它角色的面部骨数据会得到错位或夸张的脸（姿态面板默认勾选「不保存/不套用表情」，
  姿态文件里不再包含眼/下巴等表情骨；旧文件里若带这类数据，载入时也会被跳过）。
- **表情滑条只覆盖游戏自带的那 5 个口型 + 19 个表情**：角色表情里用到的其它 morph 滑条表示不了，
  但会被内部基准保住（脸不会跳变、不会错位）。若「读入当前表情」在面板上显示
  `no usable source`，把日志里 `[SMC] weight source probe` 那几行发出来即可继续适配。
- 相机参数不随姿态文件保存。
- **装了很多 XXMI/3DMigoto mod 的机器上面板会卡顿**：分层窗口路径每帧要做一次
  GPU→CPU 回读，而 `Map()` 会等 GPU 队列跑完，mod 越多等得越久。本版已做两项缓解：
  面板内容没变时跳过整轮回读，以及 `overlay_fps` 限帧（默认 60，吃紧可降到 30）。
- **冻结后骨架有轻微颤抖**（老机制遗留：逐帧钉姿势会和游戏侧仍在写的系统轻微打架，
  表现为在静止姿态与当前摆姿之间来回）。不影响摆姿与保存，未修。
- **四肢 IK 控制器是实验性功能，默认整体关闭**：代码在 `src/editor/ik_control.h`，实测"骨骼不跟随
  目标点"，所以面板里不显示、也不参与解算（开关 `g_ikFeatureEnabled = false`），请别按可用功能来预期。
- 撤销 / 重做、骨骼层级树、骨骼镜像、外置姿态导出未包含在本版。

## 交流 / 反馈

非官方粉丝项目与交流群，与 Hypergryph 无关。

- QQ 群：**终末地影棚爱好者**（群号 1126684901）
- 邮箱：**king_time@foxmail.com**（版权 / 内容下架等问题优先发邮件）
- 扫码入群：

![QQ 群](docs/qq-group.jpg)

遇到问题欢迎带上 `plugin\poser_log.txt` 与复现步骤，在 issue 或群里反馈。

## 用户协议与免责声明

<details>
<summary>在下载、安装或使用本插件（Endfield Poser）之前，请先读完以下条款；<b>继续使用即表示你已阅读、理解并同意全部条款。</b></summary>

> 首次启动时，同一份条款会以弹窗形式出现：需要把内容读到末尾、点「同意并继续」之后，
> 插件才会安装游戏侧 hook 并开始工作；未同意前不加载任何游戏相关功能。

### 1. 开源许可与最终用户权利

- 本仓库整体按 **AGPL-3.0** 发布：许可全文见 [LICENSE](LICENSE)，第三方来源与依赖见
  [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)（`src/core/` 含对上游开源项目的衍生内容，出处声明保留在各文件头部）。
- 本项目与所使用的上游开源项目及其作者**没有隶属、赞助或背书关系**，相关作者未参与本项目；
  本项目的条款与责任范围只涉及本项目（出处与责任边界见 [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES)）。
- 在**不修改**本插件的前提下，最终用户使用、复制与分发本插件不受额外限制。
- 公开分发修改版时需按 AGPL 提供完整源码，不可改为 MIT / 专有许可发布。

### 2. 反欺诈声明

- 本项目在 GitHub 上**免费开源**。如果你是通过付费渠道获取的，请知悉你可以在
  [本仓库](https://github.com/honxi1/Endfield-Poser)免费获取。
- 请勿在任何平台售卖本插件本体、却不提供仓库地址与售后支持。

### 3. 内容合规与行为约束

- 本项目是**非官方第三方工具**，与《明日方舟：终末地》的开发商 / 发行商（Hypergryph、鹰角网络）
  **没有任何隶属、合作或授权关系**，也未获得其认可。
- 本插件本身不包含任何游戏美术资产。游戏内置的官方动画、场景、模型等资产其版权完全隶属于
  鹰角网络，并不适用 AGPL-3.0 协议。
- 请**仅在单人 / 摄影模式**下使用；**不要在多人联机、竞技或任何会影响他人游戏体验的场景使用**。
- **不得**利用本插件或游戏内置的官方动画、场景、模型等资产，制作、播放或传播任何不合适的
  动作 / 动画（包括但不限于色情、暴力、政治敏感等违反法律法规或引起社区不适的内容）。
- 请勿使用本工具**修改或传播任何付费内容与游戏资源**（模型、贴图、音频等）；
  姿态文件只应包含你自己摆出的骨骼数据。

### 4. 风险与免责声明

- 本项目以**学习、技术研究与交流**为目的；作者不提供商业支持，也不对其商业使用负责
  （许可条款以 LICENSE 的 AGPL-3.0 为准，该许可本身不限制商业使用）。
- 本工具通过与游戏客户端同进程运行来提供面板与摆姿功能。这类使用方式**可能违反游戏服务条款**，
  存在**账号被限制或封禁**的风险；请自行评估风险并承担全部后果，**建议在测试账号上运行**。
- 作者不对使用本工具造成的任何损失负责（包括但不限于账号封禁、数据丢失、设备异常）。
- 如相关权利人、游戏官方或平台认为本仓库 / 发布包中的任何内容不妥，请通过
  **king_time@foxmail.com**（或 [issue](https://github.com/honxi1/Endfield-Poser/issues) /
  文末交流群）联系作者，**收到通知后会第一时间处理（包括删除相关内容、停止分发）**。
- **如果你不接受以上任何一条，请立即停止使用并删除本工具**：用包内 `安全安装.bat` 卸载，
  或手动删除 `plugin\poser.dll`、`plugin\poser_config.txt` 与游戏根目录的两个代理 DLL 并还原备份。

</details>
