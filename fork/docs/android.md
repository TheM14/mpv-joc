# Android：把 ad_joc 编进 libmpv.so 的可行性调研

结论：**可行**，arm64-v8a 是主路径。没有架构性障碍；工作量集中在把 `joc_core` 用 NDK 编出来、
和把它随 APK 一起分发。

## 现成的构建体系

`mpv-android/mpv-android` 的 `buildscripts/` 就是为这件事准备的：

- NDK（clang）交叉编译 ffmpeg、libass、libplacebo、luajit、mpv，目标 `android`，API 23；
- mpv 用 meson + 生成的 crossfile：

  ```sh
  meson setup build --cross-file "$prefix/crossfile.txt" --default-library shared \
      -Dlibmpv=true -Dcplayer=false -D{lua,libcurl}=enabled
  ninja -C build && DESTDIR="$prefix" ninja -C build install
  ```

- 产物是 `prefix/<arch>/lib/libmpv.so`，架构可选 `armv7l / arm64 / x86 / x86_64`；
- 脚本只支持 Linux/macOS，GitHub Actions 的 ubuntu runner 正好合适。整个 ffmpeg+mpv 构建
  约 30–60 分钟，可以缓存。

## 我们的补丁落在哪里

和 Windows 一样，只是多一个共享库：

1. 在 mpv 源码树里跑 `tools/apply_edits.py`（纯 Python，与平台无关）；
2. 用 NDK 的 CMake toolchain 编 `joc_core` → `libjoc_core.so`；
3. meson 加 `-Djoc-core-dir=<prefix>/lib`；
4. 运行时 `libjoc_core.so` 必须和 `libmpv.so` 一起进 APK 的 `jniLibs/<abi>/`，
   这样 `libmpv.so` 的 `DT_NEEDED` 才能解析。

## joc_core 的可移植性（源码层面）

- CMake 已有 `aarch64` 分支和 `src/simd/kernels_intrin_neon.cpp`，`cpu_probe` 用
  `getauxval(AT_HWCAP)` 探测；`JOC_SIMD=auto|scalar|neon` 可强制。
- 平台相关代码都有 POSIX 分支（`fs_utf8.cpp`、`io/process.cpp` 里的 `#if defined(_WIN32) ... #else`）。
- 依赖只有 `Threads::Threads`，没有 HDF5 之类的外部库。
- 导出宏 `JOC_API` 在非 Windows 走 visibility，和 Android 默认的 `-fvisibility=hidden` 兼容。

## 需要处理的点

| 点 | 说明 |
|---|---|
| armv7l | `JOC_ARCH` 只认 x86_64/x86_32/aarch64，armv7 会退回标量内核：能播，慢。首选 arm64 |
| `std::filesystem` | NDK 的 libc++ 在 API 23 支持；core 里用到的地方（SOFA 缓存、task、io）需要实际编一遍确认 |
| HRTF 路径 | Android 上 `exe_dir()` 为空，必须由 app 传 `--ad-joc-hrtf=/data/user/0/<pkg>/files/binaural.sofa` |
| 缓存目录 | `mp_find_user_file(global, "cache", ...)` 在 Android 上可能拿不到 → 代码已自动退回内存缓存 |
| 许可 | LGPL：动态链接 libmpv.so + libjoc_core.so 合规；发布需附对应源码/补丁 |
| 未验证 | NDK 编 joc_core 的真实报错、NEON 内核在手机上的数值正确性、实时性能 |

## 建议的落地顺序

1. CI 先只做 **arm64 的 joc_core 交叉编译**，产出 `libjoc_core.so`。这一步最快暴露平台问题。
2. 再跑 mpv-android 的 buildscripts + 我们的 patch，产出 arm64 的 `libmpv.so`。
3. 在 mpv-android 里加一个最小 demo：`jniLibs/arm64-v8a/libjoc_core.so`，启动参数加
   `--ad=joc --ad-joc-hrtf=...`，用 `adb logcat -s mpv` 验证开始渲染。
4. 对拍：手机上把输出写成 WAV（`--ao=pcm`），拉回 PC 与 `joc_cli` 比较，确认 NEON 路径正确。

## 参考

- mpv-android 构建脚本：https://github.com/mpv-android/mpv-android/tree/master/buildscripts
- 交叉编译用的 crossfile 由 `buildall.sh` 生成（`system = 'android'`，`cpu_family` 按架构）
- 现成 libmpv Android 发行版可作对照：https://github.com/jarnedemeulemeester/libmpv-android
