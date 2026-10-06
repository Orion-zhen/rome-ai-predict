# rome-ai-predict

Rime 的 AI 下一 token 预测扩展。Linux 使用 Fcitx5 插件，macOS 提供 librime 扩展，Windows 使用官方小狼毫的 Lua + DLL 接口；均不修改输入法前端。Rime 上屏后读取应用提供的光标前文，异步请求支持 `logprobs` 的 Completions API，按概率展示下一个位置的候选。macOS 适配层尚未在 macOS 编译或实机验证，构建和使用条件见下文。

## 目录结构

```text
src/
  shared/       # 配置、API 客户端、Unicode、上下文快照
  fcitx5/       # Linux / Fcitx5 插件
  rime/         # librime 组件、模块注册、宿主接口
  squirrel/     # macOS / Squirrel 的 AX 适配实现
  weasel/       # Windows / 小狼毫的 Lua、UIA 和 F24 适配
tests/
  shared/       # 共享后端测试
  fcitx5/       # Fcitx5 集成测试和安装测试
  rime/         # 真实 librime 与模拟文本控件的隔离测试
  weasel/       # Windows 宿主边界、组字范围和安装加载测试
tools/          # 开发诊断工具 probe.cpp
data/
  fcitx5/       # Fcitx5 描述文件模板
  rome-ai-predict.yaml  # 各平台共用的默认配置
cmake/          # librime 构建配置和 Fcitx5 安装脚本模板
```

根目录保留项目构建入口、README、许可证和 `PKGBUILD`。`src/rime/squirrel_host.h` 定义宿主接口，macOS 实现位于 `src/squirrel/squirrel_host.mm`，librime 隔离测试使用模拟实现。

## Linux / Fcitx5：依赖与构建

本软件和依赖统一使用最新版本，不锁定包版本。

- 构建：支持 C++20 的编译器、CMake、pkg-config、Ninja 或其他 CMake 构建工具。
- 功能依赖：Fcitx5、Fcitx5-Rime、libcurl、jsoncpp、yaml-cpp。构建时需要相应开发文件。
- 测试依赖：Python。启用 Fcitx5 集成测试时还需要 librime 开发文件。

在项目根目录执行：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build
```

默认关闭测试，安装到 `$HOME/.local`，不需要 `sudo`。仅安装以下文件：

| 内容 | 默认安装位置 |
| --- | --- |
| 插件动态库 | `~/.local/lib/fcitx5/librome-ai-predict.so` |
| 扩展描述文件 | `~/.local/share/fcitx5/addon/rome-ai-predict.conf` |
| 全局默认配置 | `~/.local/share/fcitx5/conf/rome-ai-predict.yaml` |

安装文件不依赖源码或构建目录。用户安装的描述文件记录动态库的绝对路径，不需要设置 `FCITX_ADDON_DIRS`。更新已加载的插件时，先保存输入并退出 Fcitx5，再安装，最后按桌面原有方式启动 Fcitx5。

### 自定义安装路径

配置时可指定 `-DCMAKE_INSTALL_PREFIX=/path/to/prefix`，安装时也可使用 `cmake --install build --prefix /path/to/prefix`。用户安装的描述文件使用实际安装前缀。

如果设置了 `XDG_DATA_HOME`，配置时添加 `-DCMAKE_INSTALL_DATADIR="$XDG_DATA_HOME"`。其他自定义前缀需要将其数据目录加入桌面 Fcitx5 进程的 `XDG_DATA_DIRS`。设置了 `FCITX_DATA_HOME` 或 `FCITX_DATA_DIRS` 时，以 Fcitx5 实际使用的数据路径为准。

## 配置 API

插件先读取 Fcitx5 数据目录中的 `conf/rome-ai-predict.yaml`，再读取可选用户配置：

```text
${XDG_CONFIG_HOME:-$HOME/.config}/fcitx5/conf/rome-ai-predict.yaml
```

用户配置按字段覆盖全局配置，未填写的字段保持全局值。用户配置不存在或为空时，直接使用全局配置。安装不会创建或覆盖用户配置。

全局默认关闭 AI，模型名为空。全局参数已配置完整时，无需用户配置也可从菜单启用。个人设置只需填写要覆盖的字段，例如：

```yaml
enabled: true
model: "你的服务端模型名"
```

如需其他服务地址或认证，再添加 `base_url`、`api_key`。空字符串可以清除全局密钥，`enabled: false` 可以覆盖全局开启状态。

| 字段 | 全局默认值 | 含义 |
| --- | --- | --- |
| `enabled` | `false` | Fcitx5 启动时是否开启 |
| `base_url` | `http://127.0.0.1:8000/v1` | API 根路径，自动追加 `/completions` |
| `model` | 空字符串 | 服务端实际模型名，启用前必须填写 |
| `api_key` | 空字符串 | 非空时使用 Bearer 认证，空值不发送 Authorization |
| `candidates` | `5` | 1–9，对应 `logprobs`，不能超过服务端上限 |
| `temperature` | `0.7` | 0–2 |
| `context_chars` | `1024` | 1–8192，按 Unicode 码点截取光标前文 |
| `timeout_ms` | `2000` | 100–30000，单次 HTTP 请求总超时 |

配置是平面 YAML 映射，由插件直接读取，不会自动探测或下载模型。配置无效或模型名为空时，AI 保持不可用，不阻止 Rime 输入。修改配置后从菜单关闭再开启即可重读。

设置了 `FCITX_CONFIG_HOME` 时，用户配置位于该目录的 `conf/rome-ai-predict.yaml`。全局配置由安装提供，更新可能替换该文件，个人设置应写入用户配置。

## 使用

安装后保存输入并重启 Fcitx5，例如 `fcitx5 -r -d`。桌面以其他方式管理 Fcitx5 时，按原有方式重启。切到 Rime，点击状态菜单中的“AI 联想”启停。关闭会立即取消请求并清除候选。

开关作用于本 Fcitx5 进程中的所有 Rime 输入框。菜单切换不写回配置，重启后以合并配置的 `enabled` 为准。

在能提供光标前文的编辑器或浏览器文本框中输入并上屏一段文字，等待候选返回：

- `Tab` 接受首项，数字行、小键盘数字或鼠标选择对应项。选择后继续预测下一个 token。
- `Esc` 关闭当前候选，不关闭总开关。
- 继续打字取消联想，按键交给 Rime。空格不接受首项。
- 按逻辑按键匹配，不依赖物理键码。Ctrl/Alt 组合键和超出范围的数字不用于选词。
- 松开上屏键或 AI 选词键不会关闭仍有效的候选，松键事件仍交给 Rime。
- 文本或光标变化、失焦、切换输入法、输入框销毁会使旧结果失效。

没有可靠光标前文时不请求 API。API 候选直接提交，不经过 librime 学习或简繁转换。

## API 协议

请求非流式 HTTP(S) Completions，不跟随 HTTP 重定向：

```json
{
  "model": "你的服务端模型名",
  "prompt": "今天晚上一起",
  "n": 1,
  "max_tokens": 1,
  "logprobs": 3,
  "temperature": 0.7,
  "stream": false,
  "echo": false
}
```

固定 `n=1`、`max_tokens=1`。支持两种返回结构：

- llama.cpp：`choices[0].logprobs.content[0].top_logprobs`，每项包含 `token`、`bytes`、`logprob`。
- 标准 Completions：`choices[0].logprobs.top_logprobs[0]`，内容为 token 到 logprob 的映射。

只读取第一个生成位置，按 logprob 降序取前 N 项。llama.cpp 的 `bytes` 用于实际提交，避免上屏 tokenizer 的展示标记。

取前 N 项后，去重并跳过空白、控制字符、超过 128 个码点、缺少字节或不能单独解码为完整 UTF-8 的 token。不裁剪 token，不用更低概率项补位，保留英文前导空格，因此实际候选数可能少于 N。

**token 不等于字或词**，候选可能是单字、子词或多个字符。

## macOS / Squirrel：实验性扩展

本扩展使用 Squirrel 已合并的 `_refresh_ui` 协议。需要包含该协议的最新 Squirrel、支持 C++20 `std::jthread` 的工具链、CMake、pkg-config、Boost、libcurl、jsoncpp 和 yaml-cpp。插件使用 librime 的 C++ 内部接口，必须与 Squirrel 内嵌 librime 的源码、生成头文件、架构和 C++/Boost 环境匹配，不要链接另一个 Homebrew librime 实例。

### 构建与安装

以下路径是示例，`RIME_SOURCE_DIR` 和 `RIME_BUILD_DIR` 应指向你构建 Squirrel 时使用的 librime。后者必须包含 `src/rime/build_config.h`。`RIME_LIBRARY` 必须是实际加载的内嵌动态库。

```sh
cmake -S . -B build-squirrel -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_FCITX5=OFF -DBUILD_SQUIRREL=ON \
  -DRIME_SOURCE_DIR="/path/to/squirrel/librime" \
  -DRIME_BUILD_DIR="/path/to/squirrel/librime/build" \
  -DRIME_LIBRARY="/Library/Input Methods/Squirrel.app/Contents/Frameworks/librime.1.dylib" \
  -DCMAKE_INSTALL_PREFIX="$PWD/stage" \
  -DSQUIRREL_PLUGIN_DIR=rime-plugins
cmake --build build-squirrel
cmake --install build-squirrel
codesign --force --sign - stage/rime-plugins/librime-rome-ai-predict.dylib
```

先保存输入并退出 Squirrel，再把 `stage/rime-plugins/` 下的动态库和 `rome-ai-predict.defaults.yaml` 放到该 Squirrel 的 `Contents/Frameworks/rime-plugins/`。插件目录取决于宿主的 librime 构建配置，上述路径适用于标准 Squirrel 布局。没有指定 `SQUIRREL_PLUGIN_DIR` 时，安装会直接指向系统 Squirrel 包内目录，上述示例使用暂存目录以避免自动修改输入法安装。

添加插件会改变 `.app` 包内容。插件自身的签名不保证整个应用包的签名仍有效，需要按你原来的源码构建和签名方式检查、必要时重新签名 Squirrel。更新输入法可能移除额外插件。运行时仍依赖构建时使用的 libcurl、jsoncpp 和 yaml-cpp。

安装不修改用户方案、词库或 API 配置。完成配置后重新启动 Squirrel，必要时注销再登录。不需要维护 Squirrel 的源码分支。

### 配置与启用

在实际的 Rime 用户目录中创建 `rome-ai-predict.yaml`，标准位置为 `~/Library/Rime/rome-ai-predict.yaml`，例如：

```yaml
enabled: true
model: "你的服务端模型名"
# base_url: "http://127.0.0.1:8000/v1"
# api_key: "你的密钥"
```

该文件按字段覆盖插件旁的 `rome-ai-predict.defaults.yaml`，字段含义及 API 协议与 Linux 相同。不要把个人密钥写入插件目录的默认文件。无效配置只禁用 AI，不阻止普通 Rime 输入。

在所用方案的 `.custom.yaml` 中合并以下补丁，不要覆盖原有补丁：

```yaml
patch:
  "engine/processors/@before 0": rome_ai_predictor
  "switches/@next":
    name: rome_ai_predict
    states: [AI关, AI开]
    reset: 0
```

Processor 必须放在其他按键处理器之前。Rime 的普通词库 Translator 和 Filter 无需替换。模块名为 `rome_ai_predict`，动态库文件名必须保留 `librime-rome-ai-predict.dylib`，以匹配 librime 的动态插件加载规则。

在 `squirrel.custom.yaml` 中关闭行内候选预览：

```yaml
patch:
  "style/inline_candidate": false
```

方案级外观设置也不得重新开启 `inline_candidate`。该功能会把候选预览写入编辑器，与“只读取已上屏文字”的约束冲突，本扩展遇到上下文变化会停止联想。普通行内拼音预编辑可以保留，基线在开始组字前获取。

重新部署后，可从 Rime 方案菜单切换“AI关 / AI开”。开关属于当前 Rime 会话，启动状态以 API 配置中的 `enabled` 为准。关闭再开启会重读配置。`Tab`、数字行、小键盘数字或鼠标可接受候选，`Esc` 关闭当前候选，继续输入会取消联想。AI token 直接提交，不经过词库学习或 formatter。

### AX 文本读取与限制

启用 AI 后，首次尝试读取会提示辅助功能授权。请在系统设置的“隐私与安全性 → 辅助功能”中授权 **Squirrel**，不是动态库。重新签名或重新安装输入法后，可能需要重新授权。扩展不安装常驻辅助服务，不监听全局键盘，也不使用剪贴板或上屏历史作为前文。

扩展核对当前输入源、前台应用和聚焦控件，只读取 AX 提供的文本范围。窗口包含选区起点前最多 `2 × context_chars` 个 UTF-16 单元、当前选区及最多 128 个后文单元，实际请求仍按码点限制前文。选区超过 16384 个 UTF-16 单元或回读窗口超过 40000 个单元时不预测。双次文本和选区读取必须一致，窗口边缘的半个代理对不会发送给模型。

上屏前保存的快照只用于计算预期结果，扩展必须在 1 秒内从应用回读确认实际文字和光标后才请求 API。显示和选词前再次同步复核。监测间隔为 50 毫秒，AX 同步 IPC 的单次消息超时设为 50 毫秒。监测无法保证观察到每次短暂变化，AX 也没有原子快照接口。

已知的密码框、系统安全输入状态、无权限、未知文本角色、不完整或不一致的文本范围均不会触发 API 请求。文本框若不正确声明敏感属性，扩展不能替应用识别内容是否敏感。自绘编辑器、部分终端、浏览器控件可能不支持这些 AX 属性，缺少可靠文字时停止联想，不猜测前文。因此这不保证所有应用都可用。

本扩展不记录上下文正文或密钥。可检查 Rime Context 属性 `rome_ai_predict/status`，其中 `surrounding-unavailable` 表示无法取得前文，`waiting-commit` 表示等应用确认，`commit-timeout` 表示确认超时，`generating` / `showing` 表示请求或显示阶段，`configuration-error` / `api-error` 表示配置或请求失败。宿主自己的调试日志设置不受扩展控制。

### 验证范围

Linux 上的隔离测试使用真实 librime 和模拟的文本控件，验证模块注册、上屏时序、候选、选词和生命周期。它们不验证 `src/squirrel/squirrel_host.mm`、macOS 链接与签名、TCC 权限或真实应用的 AX 实现。安装后应先在可提供完整 AX 文本范围的普通编辑器中验证，再扩大应用范围。

## Windows / 小狼毫

`src/weasel/` 复用共享配置、Completions 客户端和候选解析。Lua 处理 Rime 通知与候选，DLL 在 MTA 工作线程读取 UI Automation，通过 F24 唤醒小狼毫。只使用官方 `rime.dll` 导出的 Lua 5.4 C API，不链接第二份 Lua，不依赖 librime 内部 C++ ABI，无辅助进程。

### 构建与安装

要求 Windows x64、Visual Studio 2022 C++ 工具链、Windows SDK、CMake 和近期 vcpkg。当前验证版本为小狼毫 0.17.4 / librime 1.13.1。在 Developer PowerShell 中执行，按实际位置调整路径：

```powershell
cmake -S . -B build-weasel -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md -DVCPKG_MANIFEST_FEATURES=weasel `
  -DBUILD_FCITX5=OFF -DBUILD_WEASEL=ON -DBUILD_TESTING=ON `
  "-DWEASEL_RIME_DLL=C:/Program Files/Rime/weasel-0.17.4/rime.dll"
cmake --build build-weasel --config Release --parallel
ctest --test-dir build-weasel -C Release --output-on-failure
```

`WEASEL_RIME_DLL` 仅用于安装加载测试。安装前退出小狼毫，并将前缀设为实际 Rime 用户目录：

```powershell
cmake --install build-weasel --config Release --prefix "$env:APPDATA/Rime"
```

只安装 `lua/rome_ai_predict.lua`、`lua/rome_ai_predict_native.lua`、`rome-ai-predict/rome-ai-predict-weasel.dll` 和 `rome-ai-predict/rome-ai-predict.defaults.yaml`。不会替换官方 DLL、修改方案、词库、皮肤或用户配置，也不会自动重启、部署。

在用户目录的 `rome-ai-predict.yaml` 中填写现有配置字段：`enabled`、`base_url`、`model` 等；默认关闭，模型必须支持前述 Top-K 概率接口。将以下补丁合并进所用方案的 `.custom.yaml`，保留原有补丁，再手动重新部署：

```yaml
patch:
  "engine/processors/@before 0": lua_processor@*rome_ai_predict
  "switches/@next":
    name: rome_ai_predict
    reset: 0
    states: [AI关, AI开]
```

启动开关在方案初始化后的首次按键应用；切换 AI 开关会重读配置。Tab 选首项、数字键选对应项、Esc 取消；继续输入或文本、选区、目标变化会使旧结果失效。启用时保留 F24 用于唤醒。停用时移除上述方案补丁并重新部署；退出小狼毫后可删除四个安装文件，用户配置自行保留。

### 支持边界与验证

- 只接入 TSF 会话。前台进程必须匹配小狼毫的 `client_app`，焦点、键盘布局和 UIA 元素身份须保持一致。密码、只读控件及缺少可靠文本或选区的控件不预测。当前文档上限为 40000 个 UTF-16 单元。
- 组字状态来自 Rime，UIA 负责核对实际正文和选区。不要求 UIA 组字范围在提交后消失，普通回读也不要求 TextEditPattern。Rime 仍在组字时，即使正文与预期相同也不请求模型。
- 空闲期间预采集真实基线，在开始输入时冻结。跨越输入边界的晚到读取不作为基线。首次输入尚无缓存时，仅在 Rime 正在组字且 UIA 提供稳定范围提示时投影预编辑。提示缺失或无法确认则跳过该轮，不猜测空格、不阻塞普通输入。范围提示也只可用于当前插件拥有的 AI 菜单投影。
- 提交通知后须在 1 秒内核对正文、光标和控件身份。这不是应用级原子提交回执。Lua 提交通知不是最终 formatter 输出，若其改写上屏内容，将因正文核对失败而放弃。不支持绕过通知直接上屏的标点预测。
- UIA 调用设有 500 ms 超时，但不能保证第三方 provider 总能及时退出。焦点检查与 SendInput、正文核对与提交均非原子操作；共用同一键盘布局的不同 TSF 输入源也不能仅靠布局值完全区分。
- 自动测试验证共享后端、组字投影、无绑定目标的拒绝，以及实际 Session 配合内存控件和本地 HTTP 的基线、组字状态、过期结果与连续选词。临时安装测试使用官方 librime 验证方案部署、Lua/DLL 加载、Rime 组字通知、配置开关和中文输入，不连接桌面输入法。已测空格和中文安装路径，当前独立宿主 ACP 不能表示的 Emoji 路径不算通过。
- 先前专用窗口已人工验证预测、展示、选词及连续预测；本次通用 Windows 目标适配尚未完成日常应用实机验收，不能视为所有应用都支持。

## 共享后端

`rome-ai-backend` 是不依赖 Fcitx5 或 librime 的静态库，包含配置合并与校验、异步 Completions 客户端、UTF-8 校验和上下文快照。Linux 默认构建 Fcitx5 插件，其他平台默认只构建共享后端。Squirrel 和小狼毫适配分别需显式设置 `BUILD_SQUIRREL=ON`、`BUILD_WEASEL=ON`。

只构建共享后端并运行独立测试：

```sh
cmake -S . -B build-shared -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_FCITX5=OFF -DBUILD_TESTING=ON
cmake --build build-shared
ctest --test-dir build-shared --output-on-failure --parallel 4
```

此模式需要支持 C++20 `std::jthread` 的工具链、libcurl、jsoncpp、yaml-cpp 和 Python，不需要输入法依赖。共享后端不单独安装。上述测试可在 Linux 运行，不代表已经验证 macOS 工具链或应用兼容性。

共享接口的职责：

- `loadSettings(defaults, overrides)`：读取调用方定位的配置文件，不探测平台路径。默认配置必须存在，用户覆盖可省略。
- `CompletionClient(dispatch, callback)`：后台线程请求 API，通过调用方提供的调度函数投递结果。调度函数必须将任务排入宿主事件循环，不得内联执行。提交、析构和结果回调在同一个宿主事件线程执行，调度器必须存活至客户端析构完成。客户端取消、替换请求或析构后，已排队任务不调用结果回调。
- `SurroundingSnapshot::fromUtf8(text, cursor, anchor, unit)`：校验文本和选区，并把码点或 UTF-16 偏移统一为码点。偏移必须相对于传入的文本窗口，UTF-16 偏移不得落在代理对中间。`prefix()` 按码点截取前文，`afterCommit()` 计算替换选区后的预期快照。快照比较包含文字、光标和选区。

`afterCommit()` 的计算结果不是应用接收提交的证据。前端仍需确认上屏后的实际文本，并检查焦点、控件身份、预编辑状态和敏感输入。共享后端不读取应用文本，也不使用上屏历史补齐缺失的前文。

## 开发测试

```sh
cmake -S . -B build-test -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure --parallel 4
```

共享测试覆盖配置、UTF-8/UTF-16 偏移、选区替换、提交确认用的快照比较、API 协议、候选过滤，以及请求取消、替换和析构后的回调失效。

启用 `BUILD_FCITX5` 后，还会构建诊断工具 `rome-ai-probe` 和集成测试，不安装诊断工具。集成测试使用临时 HOME/XDG、真实系统 Fcitx5-Rime、隔离码表和本地 HTTP 服务器。所有测试均不访问模型服务、不连接桌面会话、不重启现有输入法、不安装到真实用户和系统目录。

覆盖全局配置、可选用户覆盖、联想生命周期、菜单、API 协议、认证、超时、无效响应、Unicode、候选过滤及真实键码。Fcitx5 安装测试验证仅安装三个文件、独立构建、用户安装、`--prefix`、`DESTDIR`、自定义 XDG 数据目录、系统库名加载、不覆盖用户配置，以及移走源码和构建目录后加载插件。

另可启用 Squirrel 组件的 librime 隔离测试，需要与系统 librime 匹配的源码头文件及 Boost：

```sh
cmake -S . -B build-rime-test -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_FCITX5=OFF \
  -DBUILD_TESTING=ON -DBUILD_RIME_TESTS=ON \
  -DRIME_SOURCE_DIR=/path/to/matching/librime
cmake --build build-rime-test
ctest --test-dir build-rime-test --output-on-failure --parallel 4
```

这些测试在临时目录加载最小方案和词库，模拟 AX 的正文、选区、焦点及事件循环，不读取真实应用文字。

可选 ASan/UBSan 检查：

```sh
cmake -S . -B build-sanitize -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined' \
  -DCMAKE_MODULE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-sanitize --output-on-failure --parallel 4
```

不要将此构建安装到桌面进程。上述命令关闭泄漏检查，仍检查内存访问和未定义行为。安装测试自行构建 Release 插件，不继承外层 sanitizer 参数。

## 排查与卸载

保存输入后，可以以前台方式重启查看日志：

```sh
fcitx5 -r --verbose default=5
```

搜索 `rome-ai-predict`。`request=` 表示已发起请求，`shown` 表示候选面板已刷新，`surrounding-timeout` 表示未确认上屏后文本，`unexpected-edit` 表示回传文本不符合预期，`composition-active` 表示候选栏被其他输入内容占用。

API 错误只记录状态或错误类型，不记录响应正文或密钥，不弹出错误候选，也不自动重试。结束前台命令后需要重新启动 Fcitx5。

用户安装卸载时，先在菜单关闭 AI 联想，删除描述文件，再重启 Fcitx5：

```sh
rm -- "$HOME/.local/share/fcitx5/addon/rome-ai-predict.conf"
fcitx5 -r -d
```

重启后删除插件动态库和全局默认配置。具体路径见构建目录中的 `install_manifest.txt`，保留用户配置。未来通过 AUR 安装时，由包管理器移除系统安装文件。
