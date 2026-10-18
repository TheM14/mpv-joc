# Third-party notices / 第三方通知

本仓库是 mpv 与 JustOneCacophony 渲染内核之间的一层前端。它自己不含 DSP 实现，也不分发
HRTF 数据；下面列出随仓库分发的代码与头文件的来源。

## 渲染内核（运行期依赖）

本仓库自身为 MIT（`LICENSE`）；`fork/` 编译进 mpv 后，最终二进制随 mpv 的许可（`LICENSE-mpv`）。

`joc_core.dll` 由 [JustOneCacophonyCXX](https://github.com/TheM14/JustOneCacophony) 的 `joc_core` 目标构建
（MIT，Copyright (c) 2026 TheM14）。本仓库不分发该二进制，`tools/build.ps1` 从给定的
检出构建并放到插件旁边，`tools/install.ps1` 复制它。JOC 码流解析、双耳/扬声器渲染、
HRTF 编译与 SIMD 派发全部在它里面，插件只调用 `joc_stream.h` 的 push/pull 接口。

## 解码引擎（源码副本）

`src/joc_decode.{h,cpp}`、`src/container_scan.{h,cpp}`、`src/eac3_scan.{h,cpp}`、
`src/log.{h,cpp}`、`src/win_path.h` 取自上游项目的前端代码（JustOneCacophony，MIT，
Copyright (c) 2026 TheM14）。它们是渲染与解析部分：码流遍历、容器探测、
ffmpeg 取核心 PCM、跳转与双耳尾音截断。为在 mpv 里复用，只改了两处：

* `joc_decode.cpp` 的 `load_api()` 从"链接进同一个二进制"改为按路径
  `LoadLibraryExW` 加载 `joc_core.dll` 并解析 `joc_stream_*` 导出；
* `joc_decode.h` 的 `Settings` 增加 `core_path`（内核 DLL 的显式路径）；
* `stderr_path_for()` 在设置了 `JOC_LOG` 时把 ffmpeg 诊断放到日志同目录（插件把它指向
  mpv 的配置目录，避免往 `scripts\\` 里写文件）。

## 公共头文件

`third_party/joc/include/{joc_core.h,joc_stream.h,joc_version.h}` 是
JustOneCacophonyCXX 公开 C ABI 的逐字节副本（MIT），供插件与内核按同一 ABI 编译。
`tools/verify_kernel.ps1` 用 SHA-256 校验它们与源检出一致。

`third_party/mpv/client.h` 是 mpv 的 client API 头（ISC，Copyright (c) 2017 the mpv
developers），定义 `MPV_CPLUGIN_DYNAMIC_SYM` 的动态符号表；没有它插件无法在 Windows 上
找到宿主导出的 `mpv_*` 符号。

## HRTF 数据

SOFA 测量集与 Rosella `.personalized_headphone` 模型由使用插件的人自行提供，不在本仓库中，
且已写入 `.gitignore`。数据集的许可与再分发条件跟数据一起走，本仓库不做转授。

## 专利说明

标准可公开获取不等于获准实施相关专利。
