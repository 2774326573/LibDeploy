<p align="center">
  <img src="assets/libdeploy.svg" width="96" height="96" alt="LibDeploy 图标">
</p>

<h1 align="center">LibDeploy</h1>

<p align="center">
  面向 Windows 桌面应用的依赖分析与打包工具。
</p>

<p align="center">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows-0078D7">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599C">
  <img alt="CMake" src="https://img.shields.io/badge/build-CMake-064F8C">
  <img alt="wxWidgets" src="https://img.shields.io/badge/UI-wxWidgets-2D7DD2">
  <img alt="Qt" src="https://img.shields.io/badge/UI-Qt%20Widgets-41CD52">
  <img alt="Installer" src="https://img.shields.io/badge/installer-NSIS-6E56CF">
  <img alt="i18n" src="https://img.shields.io/badge/i18n-en%20%7C%20zh--CN-24B47E">
</p>

LibDeploy 可以扫描 EXE/DLL，识别运行时依赖，收集相关资源目录，并生成可直接发布的目录、ZIP 包或 NSIS 安装包。

## 亮点

| 能力 | 说明 |
| --- | --- |
| 依赖分析 | 解析 PE 导入表并生成依赖树 |
| DLL 分类 | 区分系统 DLL、第三方 DLL、可再发行运行时和 ApiSet DLL |
| 资源打包 | 保留 `assets/`、`images/`、`scripts/`、`docs/`、`plugins/`、`packages/`、`webview2_runtime/` 等目录 |
| 部署复制 | 将所需文件复制到干净的部署目录 |
| 打包发布 | 生成 ZIP 包和 NSIS 安装包 |
| 安装体验 | 创建开始菜单快捷方式和桌面快捷方式 |
| 兼容性检查 | 对 Windows 7 / 8.1 不兼容的 WebView2 Runtime 给出警告 |
| 界面 | 支持中英文、浅色/深色/跟随系统主题 |

## 前端

LibDeploy 提供两个桌面前端，它们共用同一套核心引擎。

| 前端 | 路径 | 构建依赖 | 说明 |
| --- | --- | --- | --- |
| wxWidgets | `app/` | CMake + MinGW-w64 | wxWidgets 和运行时 DLL 已随仓库提供 |
| Qt Widgets | `qt_frontend/` | CMake + Qt SDK + MSVC 工具链 | Qt 运行时由 `windeployqt` 复制到发布目录 |

共享核心：

```text
engine/     PE 解析、DLL 分类、依赖解析、部署逻辑
config/     JSON 配置管理
```

## 构建

详细构建说明见 [docs/BUILD.md](docs/BUILD.md)。

### wxWidgets 版

需要安装的工具：

- CMake 3.20+
- MinGW-w64 GCC 13+

其它 wxWidgets 版构建依赖已随仓库提供：

```text
third_party/wxwidgets/
third_party/zlib/
third_party/minizip/
third_party/nlohmann/
runtime/
tools/
```

构建 wxWidgets 版不需要 vcpkg、不需要联网，也不需要安装其它第三方库。

```bat
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

产物：

```text
build/bin/LibDeploy.exe
```

### Qt 版

Qt 前端复用同一套仓库内核心依赖，但构建 Qt UI 需要本机安装 Qt SDK。

需要的 Qt 组件：

- Qt Widgets
- Qt Concurrent
- Qt LinguistTools
- `windeployqt`

当前本地验证套件：

```text
Qt 5.12.12 msvc2017_64
```

```bat
cmake -S .\qt_frontend -B .\build_qt ^
  -DCMAKE_PREFIX_PATH=C:\Qt\Qt5.12.12\5.12.12\msvc2017_64
cmake --build .\build_qt --config Release -j4
```

产物：

```text
build_qt/bin/Release/LibDeployQt.exe
```

构建完成后，Qt DLL、插件、MSVC 运行库、NSIS 工具、配置文件和翻译文件会复制到发布目录。最终用户运行 Qt 版时不需要单独安装 Qt。

## 目录结构

```text
LibDeploy/
├── app/                  wxWidgets 前端
├── qt_frontend/          Qt Widgets 前端
├── engine/               核心依赖分析与部署逻辑
├── config/               JSON 配置管理
├── cmake/                CMake 辅助脚本
├── docs/                 项目文档
├── locale/               wxWidgets .po 翻译源
├── assets/               应用图标等源码资源
├── third_party/          随仓库提供的第三方源码/预编译依赖
├── runtime/              wxWidgets 版发布所需 MinGW 运行时 DLL
├── tools/                内置部署工具
├── CMakeLists.txt
├── libdeploy_config.json
└── .gitignore
```

## 依赖

### wxWidgets 版随仓库提供

| 依赖 | 位置 | 用途 |
| --- | --- | --- |
| wxWidgets 3.3.1 | `third_party/wxwidgets/` | wxWidgets UI，预编译 x64 MinGW dynamic 包 |
| zlib | `third_party/zlib/` | 压缩支持 |
| minizip | `third_party/minizip/` | ZIP 打包 |
| nlohmann/json | `third_party/nlohmann/` | JSON 配置解析 |
| MinGW runtime | `runtime/` | 复制到 `LibDeploy.exe` 同目录的运行时 DLL |
| NSIS | `tools/nsis/` | 生成安装包 |

### Qt 版使用

| 依赖 | 来源 | 用途 |
| --- | --- | --- |
| zlib | `third_party/zlib/` | 共享压缩依赖 |
| minizip | `third_party/minizip/` | 共享 ZIP 打包依赖 |
| nlohmann/json | `third_party/nlohmann/` | 共享 JSON 配置解析 |
| NSIS | `tools/nsis/` | 生成安装包 |
| Qt Widgets | 本机 Qt SDK | Qt UI |
| Qt Concurrent | 本机 Qt SDK | 后台分析/部署任务 |
| Qt LinguistTools | 本机 Qt SDK | 翻译文件编译 |
| MSVC runtime | Visual Studio / Build Tools | 复制到 Qt 发布目录 |

## 使用到的开源项目

| 项目 | 用途 | 许可证 |
| --- | --- | --- |
| wxWidgets | wxWidgets 前端 | wxWindows Library Licence |
| Qt | Qt 前端 | LGPL/GPL/商业授权，取决于本机 Qt SDK 授权 |
| zlib | 压缩 | zlib License |
| minizip | ZIP 打包 | zlib License |
| nlohmann/json | JSON 配置 | MIT License |
| NSIS | 安装包生成 | zlib/libpng-style license |
| GCC / MinGW-w64 runtime | wxWidgets 版运行时 | GPL runtime exception / MinGW-w64 相关许可证 |
| CMake | 构建系统 | BSD 3-Clause License |

额外随仓库提供的工具：

| 工具 | 位置 | 说明 |
| --- | --- | --- |
| Everything tools | `tools/everything/` | 随仓库提供的辅助工具。Everything 本身不是开源项目。 |
