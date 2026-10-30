# mpv-joc — mpv 的 E-AC-3 JOC（Dolby Atmos）播放插件

mpv C 插件（cplugins）。它把 E-AC-3 JOC 码流里的对象音频交给
[JustOneCacophony](https://github.com/TheM14/JustOneCacophony) 的渲染内核重建，再作为一条普通的音频轨
交给 mpv 播放。渲染走的是与上游前端
上游项目的命令行前端相同的引擎、相同的设置、相同的 `joc_core.dll`，
因此同一个文件在两个前端上得到的采样**逐字节一致**。

- 输出：双耳（HRTF）或 2.0 – 9.1.6 的扬声器布局；
- 输出设备：照常走 mpv 的音频输出，Windows 上即 WASAPI（可强制 `ao=wasapi`、可开独占）；
- 配置：mpv 自己的配置文件，新增 `joc-*` 这一个选项命名空间（见下）；
- 输入：裸流 `.eac3` / `.ec3`，或 MP4 / Matroska 里的 E-AC-3 JOC 轨（有视频也照常播）；
- 拖放/跳转：mpv 的 seek 正常可用，跳转按目标采样重新进入码流。

## 许可

本仓库（插件、补丁、脚本、文档）采用 **MIT**，见 [LICENSE](LICENSE)。

`fork/` 的代码会编译进 mpv，所以最终播放器的分发还受 mpv 自身许可约束：
LGPL-2.1-or-later（用 `-Dgpl=true` 构建时为 GPL-2.0-or-later）。mpv 的许可全文随仓库
提供，见 [LICENSE-mpv](LICENSE-mpv)。渲染内核 `joc_core` 不在本仓库分发，其许可见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

## 与上游项目的关系

本仓库只包含 **mpv 侧的接入代码**：cplugin（`src/`）和 ad_joc 补丁（`fork/`）。

- 对象音频的渲染内核、E-AC-3/EMDF 解析、SOFA 与 Rosella 支持来自
  [TheM14/JustOneCacophony](https://github.com/TheM14/JustOneCacophony)；
- 行为基准（设置、跳转、逐字节一致的对比对象）是
  上游项目自带的命令行前端 `joc_cli`（对比基准）；
- `joc_core.dll` 不在本仓库分发，由上游构建；这里不含、也不改写内核代码。

## 仓库结构

同一个渲染内核（`joc_core.dll`）有两种接法，按是否需要自建 mpv 选一个：

| 形态 | 位置 | 对 mpv 的要求 |
|---|---|---|
| **cplugin**：官方构建即插即用 | `src/` `config/` `tools/` | 带 cplugins（官方 Windows 构建默认有） |
| **ad_joc 补丁**：把解码器编进 mpv | `fork/` | 从 mpv 源码构建，见 [fork/README.md](fork/README.md) |

```text
mpv-joc/
  src/                    cplugin 源码（C++）
  config/                 插件配置示例（cplugin-mpv.conf.example）
  tools/                  插件构建 / 安装 / 一致性校验脚本
  third_party/            mpv 客户端头、joc 公共头
  fork/                   ad_joc 补丁仓库
    files/audio/decode/   新增的解码器与 E-AC-3 帧工具
    patches/              同一改动的 diff（精确复现用）
    tools/                锚点应用、构建、DLL 收集脚本
    docs/                 Android（libmpv.so）可行性调研
  .github/workflows/      补丁的 CI：构建产物 + 每日跟最新 mpv 检查
```

## 环境要求

* mpv 需要带 **cplugins** 支持。官方 Windows 构建默认开启（`mpv --msg-level=all=v`
  的 `List of enabled features` 里能看到 `cplugins`）。
* 一个 `ffmpeg` 可执行文件（默认从 `PATH` 找，`joc-ffmpeg` 可以指定）。
* 双耳输出需要 HRTF 数据：SOFA 测量集，或 Rosella `.personalized_headphone` 模型。
  **本仓库不附带**；扬声器布局不需要。

## 安装

把三个东西放到 mpv 的目录里（便携版就是 mpv.exe 所在目录）：

```text
<mpv>\\scripts\\joc_mpv.dll                     插件本体
<mpv>\\joc_core.dll                      渲染内核（与 JustOneCacophonyCXX 同一个二进制）
<mpv>\\HRTF\\binaural.sofa               双耳渲染默认数据
<mpv>\\HRTF\\binaural.personalized_headphone   （Rosella 模型，二选一）
```

`joc_core.dll` 不能放进 `scripts\\`：mpv 会把该目录下的每个 `.dll` 都当插件加载，
内核会被当成一个没有入口的插件而报错。插件自己的查找顺序是
`joc-core` 配置 → `<插件目录>\\joc_core.dll` → `<插件目录>\\..\\joc_core.dll` → `PATH`。

然后编辑 `mpv.conf`（同一目录）：

```ini
script-opts=joc-output=binaural,joc-hrtf-source=sofa
```

也可以显式加载：

```powershell
mpv --script="C:\\path\\to\\joc_mpv.dll" --script-opts=joc-output=speaker,joc-layout=7.1 movie.mkv
```

## 配置项

全部写在 mpv 的 `--script-opts` 里，键名统一以 `joc-` 开头（`mpv.conf` 里一行写完，
用逗号分隔）。未知键会被忽略并在 `joc_decoder.log` 里记一条。

| 键 | 取值 | 默认 | 说明 |
|---|---|---|---|
| `joc-enabled` | yes / no | yes | 关掉后插件完全不介入 |
| `joc-output` | binaural / speaker | binaural | 输出形式 |
| `joc-layout` | 2.0 3.1 4.0 5.1 7.1 5.1.2 5.1.4 7.1.2 7.1.4 9.1.4 9.1.6 | 7.1 | 扬声器布局；仅 `speaker` 使用 |
| `joc-hrtf-source` | sofa / rosella | sofa | 双耳用哪种 HRTF 输入 |
| `joc-rosella-filterbank` | 1 / 2（或 public / captured） | 1 | Rosella 滤波器组：1=13 taps/6 slots 的公开版，2=11 taps/3 slots 的捕获版（与参考渲染器一致）；仅 `joc-hrtf-source=rosella` 使用 |
| `joc-hrtf` | 路径 | 空 | 空则用 `<插件目录>\\HRTF\\` 或 `<插件目录>\\..\\HRTF\\` 下的默认文件名 |
| `joc-mode` | near / mid / far | mid | 双耳模式 |
| `joc-gain-db` | 浮点（dB） | 0 | 渲染增益；双耳可能超过满刻度，需要时在这里衰减 |
| `joc-tail` | 秒 | 5.0 | 双耳房间尾音的长度上限（不作为文件时长交付） |
| `joc-int24` | yes / no | no | 交付 24bit 定点而不是 float32 |
| `joc-object-delay` | 采样 | 1473 | OAMD 对象延迟，与既有实现保持一致 |
| `joc-threads` | 整数 | 0 | 内核线程数，0 = 自选 |
| `joc-ffmpeg` | 路径 | ffmpeg | 核心 PCM 解码用的 ffmpeg |
| `joc-core` | 路径 | 空 | `joc_core.dll` 的显式路径 |
| `joc-wasapi` | auto / yes / no | auto | yes 时把 `ao` 设为 `wasapi` |
| `joc-exclusive` | yes / no | no | 设置 `audio-exclusive=yes`（WASAPI 独占） |
| `joc-chunk` | 秒 | 2.0 | 每次写入 mpv 的音频块大小（0.25–10） |
| `joc-mute-until-ready` | yes / no | yes | 渲染轨就绪前先把文件自带音轨静音，片头不会先播一遍普通 bed |
| `joc-hrtf-cache` | auto / memory / disk / none | auto | 编译后的 HRTF（`.jochrtf`）缓存位置；auto = `<mpv 配置目录>\\joc-hrtf-cache` |
| `joc-hrtf-cache-dir` | 路径 | 空 | 显式指定缓存目录 |
| `joc-attach` | yes / no | yes | no = 只提供音频端点、不向播放器加轨（抓取/验证用） |
| `joc-verbose` | yes / no | no | 在日志里记录每个 HTTP 请求与跳转 |
| `joc-log` | 路径 | 空 | 日志文件；空则写 `<mpv 配置目录>\\joc_decoder.log` |

## 扬声器布局与声道顺序

`joc-layout` 只接受下表 11 个布局，插件交给 mpv 的 PCM 顺序就是表里的顺序：

| 布局 | 声道顺序 |
|---|---|
| 2.0 | L, R |
| 3.1 | L, R, C, LFE |
| 4.0 | L, R, Lrs, Rrs |
| 5.1 | L, R, C, LFE, Ls, Rs |
| 7.1 | L, R, C, LFE, Ls, Rs, Lrs, Rrs |
| 5.1.2 | L, R, C, LFE, Ls, Rs, Ltm, Rtm |
| 5.1.4 | L, R, C, LFE, Ls, Rs, Ltf, Rtf, Ltr, Rtr |
| 7.1.2 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Ltm, Rtm |
| 7.1.4 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Ltf, Rtf, Ltr, Rtr |
| 9.1.4 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Lw, Rw, Ltf, Rtf, Ltr, Rtr |
| 9.1.6 | L, R, C, LFE, Ls, Rs, Lrs, Rrs, Lw, Rw, Ltf, Rtf, Ltm, Rtm, Ltr, Rtr |

`4.0` 是本项目补充的 Quad 布局（传统 Quad 的 `FL FR BL BR`），声道为 `L R Lrs Rrs`，不在杜比
文档的布局列表里；除它以外的布局及声道顺序均依据杜比相关规范定义。

插件通过回环 HTTP 把渲染结果作为一条普通 WAV 轨交给 mpv。这个 WAV 使用
`WAVE_FORMAT_EXTENSIBLE` 且 `dwChannelMask = 0`：它不声明 Windows 扬声器位置，PCM 顺序严格按
上表，不按掩码位序重排。**插件不改动 mpv 的任何声道布局设置**，也不影响 mpv 对其他音频格式的
原生布局支持。

没有掩码时 FFmpeg 会按声道数给这条流套一个默认布局（4 声道是 `fl-fr-fc-bc`，8 声道是
`fl-fr-fc-lfe-bl-br-sl-sr`，更多声道保持未指定），mpv 据此解释它的声道。也就是说，回环 WAV 这条
路只保证“PCM 逐声道与上表一致”；扬声器位置取决于 mpv 自己的默认布局。

要让 mpv 按真实布局播放，请用 `ad_joc` 解码器：它逐帧设置 chmap（顺序与上表一致），mpv 的滤镜
链、重混与 WASAPI 掩码都按正确位置处理。实测 `ao=pcm` 输出与 `joc_cli` 的 WAV 逐字节一致。

设备端的输出映射（WASAPI 共享/独占、ASIO、HDMI）仍由用户按声道编号自行确认；本项目保证各 PCM
声道的内容和编号符合上表，不保证下游播放器或设备自动识别杜比扬声器位置。WASAPI 端点最多 8 个
声道，`9.1.4` / `9.1.6` 这类布局通常需要 ASIO 或外部渲染器。

`joc-layout` 只接受上表 11 个名字；其它值记一条警告并保持默认 `7.1`。

## 实现

```text
mpv ── 视频/字幕照常
 │
 └─ file-loaded ──► 探测（裸流直接走码流；容器先问 ffmpeg 哪条轨是 E-AC-3、是不是 JOC）
                     │
                     ▼
             joc_core.dll（joc_stream.h）
                     ▲ E-AC-3 同步帧 + ffmpeg 解出的核心 PCM
                     │
         ┌───────────┴────────────┐
         │  渲染成 WAV（float32 / int24，dwChannelMask = 0）
         │  由进程内的 loopback HTTP 端点提供，支持 Range
         └───────────┬────────────┘
                     ▼
        mpv: audio-add http://127.0.0.1:<port>/joc.wav select
```

mpv 把这条轨道当成一个**可 seek 的普通 WAV 文件**来读：它自己的 demuxer、缓存、A/V 同步、
WASAPI 输出和跳转都不用改。

启动顺序：`start-file` 时就开始探测并创建渲染器（此时 mpv 还在打开文件），同时把文件自带的
音轨静音；`file-loaded` 时把渲染轨 `audio-add ... select` 进去，然后恢复原来的静音状态。
探测判定不是 JOC 的输入立刻恢复，不会留下静音（带 E-AC-3 轨但不是 JOC 的容器会经历一次
约 100 ms 的 ffmpeg 探测，期间是静音；`joc-mute-until-ready=no` 可关掉这一行为）。实测：冷启动约 0.6 秒、
HRTF 编译结果命中磁盘缓存后约 0.37 秒；这段是静音而不是普通 bed，且 mpv 在切轨时会暂停音频时钟，
所以不会跳过内容。插件只在请求的字节位置和渲染位置不一致时让渲染器重新进入码流
（与组件的 seek 策略相同：目标帧减去 64 帧预滚，再丢掉目标之前的输出）。

跳转与从头播放相比有极低的噪声底差（与组件一致，实测 −65 dBFS 以下），位置本身不漂移。

## 与「fork mpv」方案的关系

同类成熟项目走的是另一条路：`mpv-omniphony` 是 **mpv 的 fork**，加了一个 `--ad=orender`
解码模块（`ad_orender`），把 demuxer 给出的访问单元交给 `liborender` 做空间渲染；真正的
编解码在可动态加载的 bridge DLL 里（`harletty_dolby_bridge.dll` 等，E-AC-3 JOC 也在其中）。
这条路拿到的是"原生"轨道：没有额外音轨、没有启动切换、codec 信息正确。

本插件刻意不走 fork：它要求 stock mpv（cplugins），代价是启动时 0.3–0.6 秒的准备窗口，以及
一条外部 PCM 轨（OSD 里显示为 pcm_f32le）。

需要"零妥协"时可以照 mpv-omniphony 的结构给 mpv 加一个 `ad_joc` 模块——mpv 的 AD 层拿得到
AVPacket，所以不需要改 FFmpeg，内核仍是 `joc_core.dll`。这条路线已经有可构建的原型，见
[`fork/`](fork/README.md)：基于 mpv master（2026-10-17）的 patch、新增源码与构建说明，实测
`--ad=joc` 的双耳与 7.1 输出都与 `joc_cli` 逐字节一致。代价是要自己维护一个 mpv 构建
（Windows 上需要 MSYS2/MinGW 工具链）。

## 一致性验证

`tools/verify_parity.ps1` 用 `joc_cli` 生成参考 WAV，再从插件取回实际交付的字节逐字节比较。
本机（mpv v0.41.0，FFmpeg 8.0）的结果：

| 组合 | 结果 |
|---|---|
| 双耳 float32（SOFA） | 逐字节一致 |
| 双耳 int24（Rosella） | 逐字节一致 |
| 扬声器 7.1 float32 | 逐字节一致 |

参考输出比交付多出的部分是双耳房间尾音：组件和本插件都按文件时长截断，`joc_cli` 的
`--binaural-output` 不截断。

## 构建

需要一个 JustOneCacophonyCXX 检出（用它的 `joc_core` 目标）：

```powershell
pwsh -File tools/build.ps1 -JocCoreSource C:\\path\\to\\JustOneCacophonyCXX
```

产物在 `build\\Release\\joc_mpv.dll`，`joc_core.dll` 会被复制到同一目录。只想要插件、不重建内核时：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

编译选项固定为 `/fp:precise`、静态 CRT：浮点行为是逐字节验收的一部分。

## 排错

`joc_decoder.log` 写在 mpv 的配置目录（`mpv.conf` 所在目录）里，记录插件版本、核心版本、
探测结论、每次跳转与 HTTP 请求；ffmpeg 的诊断在同目录的 `joc_ffmpeg_*.log`。
不能写在 `scripts\\` 里——mpv 会把该目录下的每个文件都当脚本加载。`--msg-level=all=info`
会在终端看到 `JOC: ...` 状态行。

常见问题：

* **没有任何 JOC 提示、听起来是普通 E-AC-3** —— 文件没有 JOC 元数据，或者容器的音频轨不是
  E-AC-3；插件不接管时 mpv 会照常播放原始轨。
* **双耳报错"找不到 HRTF 文件"** —— 把 `binaural.sofa` 放到 `<mpv>\\HRTF\\`，或用
  `joc-hrtf` 指定路径。
* **想确认用的是哪条轨道** —— `joc-verbose=yes` 后看日志里的 `probe:` 与 `start:` 行。

## 许可

MIT，见 `LICENSE`。渲染内核与解码引擎来自 JustOneCacophonyCXX，
第三方说明见 `THIRD_PARTY_NOTICES.md`。
