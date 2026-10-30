# mpv + ad_joc — 原生轨道的 E-AC-3 JOC 解码器（实验）

这是 [TheM14/JustOneCacophony](https://github.com/TheM14/JustOneCacophony) 渲染内核在 mpv 里的解码器接入（补丁形式）。内核本身
不在这里，构建时链接上游的 `joc_core`；行为基准是上游项目自带的 `joc_cli`。

这是"接管文件"那条路的最小可行实现：给 **mpv 本身**加一个音频解码模块 `ad_joc`，用
`--ad=joc` 启用。mpv 的 AD 层拿到的是 demuxer 给的 **AVPacket**（原始 E-AC-3 访问单元），
所以不需要改 FFmpeg；解码出来的对象音频直接作为 mpv 自己的音频流，走它原本的滤镜链、
重采样、A/V 同步与音频输出。

这和插件方案（`mpv-joc` 的 cplugin + loopback 音频源）是两条路：

| | cplugin + 回环音频源 | `ad_joc`（本目录） |
|---|---|---|
| 改动对象 | 零，stock mpv | mpv 源码，需要自己构建 |
| 轨道 | 额外一条 `pcm_f32le` 外部轨 | 文件自己的轨，codec 显示正确 |
| 片头 | 0.3–0.6 s 准备（静音） | 无 |
| 跳转 | 渲染器可按 64 帧预滚重入 | mpv 不给预滚包，冷启动约 1 s 收敛 |
| 维护 | 跟随 mpv 的 cplugin ABI | 跟随 mpv 的 AD 接口 |

## 基于哪个 mpv

mpv master，提交 `01e2533f3143b8940857ca419b0b367f583e05ca`（2026-10-17，`v0.41.0-UNKNOWN`）。
本目录的 `patches/mpv-ad-joc.patch` 是针对这个提交的 diff，也在 `fffe7a23a8`（2026-10-18）
上无冲突应用并完成构建验证；再新的版本如果碰到上下文变化，需要重新对齐
`filters/f_decoder_wrapper.c` 里的注册位置。

## 组成

```text
files/audio/decode/
  ad_joc.c / ad_joc.h       解码器：libavcodec 解 bed + joc_stream 渲染 + 输出 aframe
  joc_eac3.c / joc_eac3.h   E-AC-3 同步帧/音频帧边界（上游 eac3_scan 的 C 子集）
  joc_core.h                joc_core 公共头（与 JustOneCacophonyCXX 逐字节一致）
  joc_stream.h
  joc_version.h
patches/mpv-ad-joc.patch    对 meson.build / meson.options / f_decoder_wrapper / options 的改动
```

注册方式与 mpv-omniphony 的 `ad_orender` 相同：`f_decoder_wrapper.c` 里当 `--ad` 列表包含
`joc` 且编码是 `eac3` 时切换 driver；`meson.build` 里加源码、链接 `joc_core`、定义
`HAVE_JOC`；`options/*` 加 `--ad-joc-*` 选项组。

## 维护方式：文件 + 锚点，而不是行号补丁

仓库里保存的是**新增文件**加**对上游的少量改动**，不是一整份 mpv：

```text
files/audio/decode/    7 个新文件（ad_joc / joc_eac3 / 三个 joc 头）
tools/apply_edits.py   把 6 处改动按"锚点"写进 mpv 源码树
patches/              同一改动的 diff，供精确复现
tools/build_fork.ps1   一键：clone 指定 ref → 应用 → 构建 → 打包
```

`apply_edits.py` 不依赖行号：它找 `static bool reinit_decoder`、`if (!driver)`、
`{"ad-lavc", OPT_SUBSTRUCT(...)}` 这类锚点插入代码，并且是幂等的。上游挪了行、改了上下文都不会
影响；只有当锚点本身消失（上游重构了这段）时，它会**在改动任何文件之前**报出文件名退出——那就是
需要人工适配的地方，通常只是换一个新锚点。

适配新 mpv 的流程：

```powershell
.\tools\build_fork.ps1              # clone 最新 master、应用、构建
.\tools\build_fork.ps1 -MpvRef v0.42.0
.\tools\build_fork.ps1 -SkipBuild   # 只验证能否应用
```

### 自动化（GitHub Actions）

- 仓库根目录的 `.github/workflows/build.yml`：`workflow_dispatch`（可填 mpv 的 branch/tag/commit）或推 `v*` 标签时，
  在 windows-latest 上 clone 最新 mpv、应用改动、用 MSVC 构建 joc_core、用 MSYS2 构建 mpv、
  收集 DLL 并上传 `mpv-ad-joc-windows` 产物。
- 仓库根目录的 `.github/workflows/drift.yml`：每天定时 clone 最新 mpv master，只跑 `apply_edits.py`。锚点失效会直接
  让这个 job 变红，比发布时才发现要好。

> 这两个 workflow 是在本机准备、按本地验证过的步骤写的；GitHub 上还没实际跑过，第一次运行可能
> 要调依赖路径（尤其 joc_core 的 CMake 产物目录）。

## 跨平台

代码本身是可移植的：`ad_joc.c` 只有取 exe 目录那段在 `_WIN32` 下（其它平台走 mpv 配置目录搜索），
`joc_core` 的 CMake 里本来就有 `if(UNIX)` 分支（ELF/Mach-O 的 rpath），导出宏也区分了 `_WIN32`。

目前只在 Windows 上验证过。移植到 Linux/macOS 需要：

1. `joc_core` 用系统的 CMake 构建成 `libjoc_core.so`/`.dylib`；
2. meson 找到它，并给它加 rpath（或运行时装 `LD_LIBRARY_PATH`）；
3. 构建脚本换成 bash（`build_fork.ps1` 是 Windows 版），CI 加一个 ubuntu/macos job。

Android（`libmpv.so`）的可行性调研见 [docs/android.md](docs/android.md)。

## 构建（Windows / MSYS2）

需要 msys2 的 **UCRT64** 环境（本机验证版本：gcc 16.2.0、ffmpeg 9.0.2、meson 1.12.1、ninja 1.13.2）。
LuaJIT 只发布在 ucrt64 / clang64，MINGW64 环境里已经没有这个包了，而 mpv 的脚本需要 LuaJIT 或
Lua 5.1/5.2：

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-meson \
    mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-ffmpeg \
    mingw-w64-ucrt-x86_64-luajit
```

把本目录的 `files/` 覆盖到 mpv 源码树，然后：

```bash
cd mpv-src
git apply /path/to/patches/mpv-ad-joc.patch
export PATH="${MINGW_PREFIX:-/ucrt64}/bin:$PATH"
meson setup build --buildtype=release -Dgpl=false -Dlua=luajit \
    -Djoc-core-dir=/c/path/to/joc_core_import_lib_dir
ninja -C build
```

`-Dlua=luajit` 是必须的：mpv 的内置 OSC（进度条 / 播放控制）和所有脚本都编译在 Lua 之上，
不带 Lua 时 `--osc` 选项本身都不存在，界面就是空的。

`-Djoc-core-dir` 指向含 `joc_core.lib` 的目录（JustOneCacophonyCXX 的 `joc_core` 构建产物；
本机是 `mpv-joc/build/joc_core/Release`）。运行前把 `joc_core.dll` 放到 `mpv.exe` 旁边。

## 用法

所有设置都是普通 mpv 选项，写进 `mpv.conf` 就对每次播放生效：

```ini
# mpv.conf
ad = joc
ad-joc-binaural = yes          # 默认就是 yes（双声道输出）
ad-joc-hrtf = C:/HRTF/binaural.sofa   # 可省：会自动找
ad-joc-layout = 7.1            # 仅 binaural=no 时使用
ad-joc-mode = mid
ad-joc-rosella-filterbank = 1  # Rosella 输入：1=公开版（默认），2=捕获版
ad-joc-gain-db = 0
```

命令行等价：`mpv --ad=joc movie.mkv`。

| 选项 | 取值 | 默认 |
|---|---|---|
| `--ad-joc-binaural` | yes / no | yes |
| `--ad-joc-hrtf` | SOFA / Rosella / `.jochrtf` 路径 | 空（自动搜索） |
| `--ad-joc-layout` | `2.0` `3.1` `4.0` `5.1` `7.1` `5.1.2` `5.1.4` `7.1.2` `7.1.4` `9.1.4` `9.1.6`| 7.1 |
| `--ad-joc-mode` | near / far / mid | mid |
| `--ad-joc-rosella-filterbank` | 1 / 2（或 public / captured） | 1（公开版） |
| `--ad-joc-gain-db` | -60 … 12 | 0 |
| `--ad-joc-int24` | yes / no | no |

`--ad-joc-rosella-filterbank=2` 只在 `--ad-joc-hrtf` 指向 `.personalized_headphone` 时生效：
`1` 是 13 taps / 6 slots 的公开滤波器组，`2` 是 11 taps / 3 slots 的捕获版（与参考渲染器
一致、延迟 448 samples）。SOFA 与 `.jochrtf` 输入忽略该选项并给一条 warning。

`--ad-joc-int24` 把输出量化到渲染器的 24-bit 规则（与上游前端的 int24 交付一致）：
clip 到 [-1,1]、乘 8388607、向零截断、取低 24 位，再按 `n / 2^23` 读回。默认关闭，
输出就是渲染器的 float32。

本仓库**不附带任何 HRTF 文件**：SOFA、Rosella 模型、编译出的 `.jochrtf` 缓存都不随仓库或发布包
分发，需要自备。

### HRTF 的三种输入与自动判断

`--ad-joc-hrtf` 不需要说明类型，按文件内容判断（扩展名只作兜底）：

| 输入 | 判断依据 | 交给渲染器 |
|---|---|---|
| SOFA | HDF5 魔数 `89 48 44 46 0D 0A 1A 0A` | `hrtf_sofa_path` |
| Rosella 个性化模型 | 首字符 `{` 且含 `"personalized_hrtf"` | `personalized_headphone_path` |
| 已编译核 | 扩展名 `.jochrtf` | `hrtf_path` |

SOFA 编译结果会缓存成 `.jochrtf`（本机首帧 320 ms 左右，命中缓存后 110 ms 左右），
缓存目录是 `<config>/cache/joc-hrtf-cache`，没有 config 目录时用 `<mpv.exe>/joc-hrtf-cache`。

### 不写 `--ad-joc-hrtf` 时

按下面的顺序找，找到即用：

1. `<mpv.exe>/HRTF/binaural.sofa`
2. `<mpv.exe>/HRTF/binaural.personalized_headphone`
3. `<mpv.exe>/HRTF/binaural.jochrtf`
4. `<config>/joc-hrtf/binaural.sofa`
5. `<config>/joc-hrtf/binaural.personalized_headphone`

都找不到就打印一行提示，退回扬声器布局渲染（默认 7.1），由 mpv 按输出设备下混——双声道
设备上仍然是双声道输出，不会没声音。

普通 E-AC-3（没有 JOC 元数据）不会失败：解码器先看前 8 个音频帧，渲染器一帧都没输出就判定
这不是 JOC 流，打印一行 `this E-AC-3 stream carries no JOC metadata; decoding it with the
default decoder instead`，然后把已经读掉的包补喂给 mpv 自己的 E-AC-3 解码器，后续照常播放
（`--ad=joc` 因此可以一直开着，混合片库也能用）。

### 便携化（不装 MSYS2 也能双击运行）

MinGW 构建的 `mpv.exe` 依赖 `/mingw64/bin` 下的一批 DLL。把依赖闭包复制到 `mpv.exe` 旁边即可：

```bash
bash tools/bundle_dlls.sh /c/path/to/mpv-src/build   # 递归 ldd，把 /mingw64 下的依赖拷到 exe 旁边
```

之后 `mpv.exe` 只靠系统目录就能启动（本机实测：`PATH=C:\Windows\System32` 下 `--version` 与播放均正常）。
`tools/bundle_dlls.sh` 是本目录里的脚本，路径按构建目录改一下即可。

## 验证结果（本机）

用 `--ao=pcm --ao-pcm-file=out.wav` 抓解码器实际输出的 PCM，与 `joc_cli` 的参考 WAV 逐字节比较：

| 模式 | 结果 |
|---|---|
| 双耳 float32（SOFA，自动发现） | 8 s（3,071,992 B）**逐字节一致** |
| 双耳 float32（Rosella，自动识别） | 8 s（3,071,992 B）**逐字节一致** |
| 扬声器 7.1 float32 | 8 s（12,287,968 B）**逐字节一致** |
| 扬声器 7.1.4 float32 | 8 s **逐字节一致** |
| 扬声器 9.1.4 / 9.1.6 float32 | 能播、通道数正确（14 / 16）；通道内容与 `joc_cli` 相同，但 mpv 按自己的标准声道顺序排列，所以字节序列不是逐字节相同 |
| 双耳 int24 | 与 24-bit 规则逐样本一致（256,000 样本 0 差异） |
| 非 JOC 的普通 5.1 E-AC-3 | 打印回退提示，按原生 5.1 播放（exit 0） |
| 默认 `--ad=lavc` | 不受影响，仍是原生 8ch downmix 行为 |

## 已知限制

* **跳转时渲染器会重建**，不复用旧状态：重建前渲染器里排队的输出属于旧位置，若沿用会在 seek 后
  被当成新位置播放（实测约 1 s，与新旧内容都对不上，RMS ~0.25）。重建后输出就是目标位置的内容，
  与“从该位置新建”的渲染在 1 s 窗口内 RMS ~0.005。
* 由于 mpv seek 后只给 1 个（有时 0 个）预滚包，重建后的前 ~1 s 与“从头连续渲染”仍有残差
  （末段 ~7e-5）：这是缺少历史状态所致，内容本身正确。带文件访问的前端能消除它，是因为
  它们可以预滚 64 帧，AD 拿不到这些帧。
* 只注册 `eac3`（`ac3` 没有 JOC）。非 JOC 的 E-AC-3 会自动回退，代价是开头约 0.26 s
  （8 帧）的判定窗口。
* 双耳需要 HRTF：给 `--ad-joc-hrtf`，或把 `binaural.sofa` 放在 `mpv.exe` 旁边；都没有时
  退回扬声器布局，不再报错退出。
* 每换一个 mpv 版本都要重新对齐 patch（AD 接口和注册点在 mpv 内部，不在稳定 ABI 里）。

## 许可

`ad_joc.c` / `ad_joc.h` / `joc_eac3.*` 都是 MIT（见仓库根的 [LICENSE](../LICENSE)）：
`joc_eac3.*` 改写自上游的 `eac3_scan.cpp`，`ad_joc.*` 是新增代码。
它们编译进 mpv，因此最终播放器的分发随 mpv 的许可（LGPL-2.1-or-later，`-Dgpl=true`
时为 GPL-2.0-or-later），全文见 [LICENSE-mpv](../LICENSE-mpv)。

`joc_core.h` / `joc_stream.h` / `joc_version.h` 来自 JustOneCacophonyCXX（MIT），逐字节复制。
`joc_core.dll` 本身仍是 MIT，不在本仓库分发。
