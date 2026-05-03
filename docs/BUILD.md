# LibDeploy — 构建说明

## wxWidgets 版前提条件（只需这两个）

| 工具 | 版本要求 | 获取 |
| --- | --- | --- |
| CMake | 3.20+ | https://cmake.org/download/ |
| MinGW-w64 (g++) | 13.0+ | https://github.com/niXman/mingw-builds-binaries/releases |

> wxWidgets 版所需的其他依赖已经随仓库提供：wxWidgets、minizip、zlib、
> nlohmann/json、MinGW 运行时 DLL、NSIS 工具等均已放在 `third_party/`、
> `runtime/` 和 `tools/` 目录中。
>
> 构建 wxWidgets 版 **不需要安装其它依赖**，不需要联网，也不需要 vcpkg；
> 只需要 CMake 和 MinGW-w64 编译器。

## 构建步骤

```bat
cd LibDeploy
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CXX_COMPILER=<mingw64>/bin/g++.exe ^
      -DCMAKE_C_COMPILER=<mingw64>/bin/gcc.exe
cmake --build build -j4
```

将 `<mingw64>` 替换为你的 MinGW 安装路径，例如 `C:/mingw64`。

## 产物位置

```
build/bin/
  LibDeploy.exe              ← 主程序
  wxbase331u_gcc_custom.dll  ┐
  wxmsw331u_core_gcc_custom.dll  ├ wxWidgets 运行时（自动复制）
  wxmsw331u_adv_gcc_custom.dll   ┘
  libgcc_s_seh-1.dll         ┐
  libstdc++-6.dll             ├ MinGW 运行时（自动复制）
  libwinpthread-1.dll         ┘
```

直接将 `build/bin/` 目录整体复制到目标机器即可运行，无需安装任何依赖。

> 使用 LibDeploy 分析和部署程序时，自动资源目录扫描会跳过常见构建输出目录，
> 例如 `bin/`、`build/`、`Debug/`、`Release/`、`RelWithDebInfo/`、
> `MinSizeRel/`、`x64/`、`x86/`、`Win32/`、`Win64/` 和 `*_deploy/`。
> 这些目录不会被当作应用资源复制到部署目录、ZIP 或安装包中。

## Qt 前端构建

Qt 版前端位于 `qt_frontend/`，复用同一套 `engine/`、`config/`、`third_party/`
核心模块。

> Qt 版的核心依赖（minizip、zlib、nlohmann/json、NSIS 工具等）仍然来自仓库。
> 但构建 Qt 前端需要本机已安装 Qt SDK。
> 这是唯一额外要求；不需要 vcpkg，也不需要另外安装 zlib/minizip/json。
>
> 构建完成后，`windeployqt` 会把 Qt 运行时 DLL 和插件复制到发布目录。
> 最终用户运行 `build_qt/bin/Release/LibDeployQt.exe` 时不需要单独安装 Qt。

| 依赖 | 来源 | 用途 |
| --- | --- | --- |
| Qt Widgets | 本机 Qt SDK | Qt 前端界面 |
| Qt Concurrent | 本机 Qt SDK | 后台分析/部署任务 |
| Qt LinguistTools | 本机 Qt SDK | 编译 `.ts` 翻译文件 |
| windeployqt | 本机 Qt SDK | 复制 Qt 运行时 DLL 和插件 |
| MSVC runtime | Visual Studio / Build Tools | Qt 版发布运行库，构建后自动复制 |

```bat
cd LibDeploy
cmake -S .\qt_frontend -B .\build_qt ^
      -DCMAKE_PREFIX_PATH=<Qt>/5.12.12/msvc2017_64
cmake --build .\build_qt --config Release -j4
```

产物位于：

```
build_qt/bin/Release/LibDeployQt.exe
```

Qt 构建后会复制 Qt DLL、插件、MSVC 运行库、NSIS 工具和翻译文件到发布目录。

## 目录结构说明

```
LibDeploy/
├── CMakeLists.txt           ← 根构建文件
├── cmake/
│   └── BundledWxWidgets.cmake  ← 自定义 wx 导入目标（不依赖 vcpkg）
├── app/                     ← wxWidgets UI 层
├── engine/                  ← 核心逻辑（PE解析/分类/解析/部署）
├── config/                  ← JSON 配置管理
├── third_party/
│   ├── wxwidgets/           ← 预编译 wxWidgets 3.3.1 (x64-mingw-dynamic)
│   ├── minizip/             ← minizip 1.3.1 源码
│   ├── zlib/                ← zlib 1.3.1 源码
│   └── nlohmann/            ← nlohmann/json 3.12.0 头文件
└── runtime/                 ← MinGW 运行时 DLL（随 exe 发布）
```

```
app/                  wxWidgets 前端源码
qt_frontend/          Qt 前端源码与翻译源
engine/               核心依赖分析与部署逻辑
config/               配置管理
cmake/                CMake 辅助脚本
docs/                 项目文档
locale/               wxWidgets .po 翻译源
assets/               应用图标等源码资源
third_party/          项目内置第三方源码/预编译依赖
runtime/              随 wx 版发布所需 MinGW 运行时 DLL
tools/                内置 NSIS/Everything 等工具
CMakeLists.txt
libdeploy_config.json
.gitignore
```
