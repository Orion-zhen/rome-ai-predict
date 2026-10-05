# rome-ai-predict

Linux / Fcitx5 的 AI 下一 token 预测扩展。Rime 上屏后读取应用提供的光标前文，异步请求支持 `logprobs` 的 Completions API，按概率展示下一个位置的候选。

## 依赖与构建

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

## 共享后端

`rome-ai-backend` 是不依赖 Fcitx5 或 librime 的静态库，包含配置合并与校验、异步 Completions 客户端、UTF-8 校验和上下文快照。Linux 默认构建 Fcitx5 插件，其他平台默认只构建共享后端。目前尚未实现 Squirrel 插件、Accessibility 文本读取或 `_refresh_ui` 接入。

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

覆盖全局配置、可选用户覆盖、联想生命周期、菜单、API 协议、认证、超时、无效响应、Unicode、候选过滤及真实键码。安装测试验证仅安装三个文件、独立构建、用户安装、`--prefix`、`DESTDIR`、自定义 XDG 数据目录、系统库名加载、不覆盖用户配置，以及移走源码和构建目录后加载插件。

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
