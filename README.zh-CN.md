<p align="center">
  <img src="assets/libdeploy.svg" width="96" height="96" alt="LibDeploy logo">
</p>

<h1 align="center">LibDeploy</h1>

<p align="center">
  <strong>Windows 桌面应用的依赖分析与打包工具</strong>
</p>

<p align="center">
  <a href="#核心功能">核心功能</a> •
  <a href="#前端界面">前端界面</a> •
  <a href="#构建指南">构建指南</a> •
  <a href="#仓库结构">仓库结构</a> •
  <a href="#依赖说明">依赖说明</a> •
  <a href="#使用的开源项目">使用的开源项目</a> •
  <a href="#流程图">流程图</a>
</p>

<p align="center">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

<p align="center">
  <img alt="Platform" src="https://img.shields.io/badge/平台-Windows-0078D7">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599C">
  <img alt="CMake" src="https://img.shields.io/badge/构建-CMake-064F8C">
  <img alt="wxWidgets" src="https://img.shields.io/badge/UI-wxWidgets-2D7DD2">
  <img alt="Qt" src="https://img.shields.io/badge/UI-Qt%20Widgets-41CD52">
  <img alt="Installer" src="https://img.shields.io/badge/打包-NSIS-6E56CF">
  <img alt="i18n" src="https://img.shields.io/badge/多语言-中%20%7C%20英-24B47E">
  <img alt="License" src="https://img.shields.io/badge/许可证-MIT-blue">
</p>

> **LibDeploy** 扫描 EXE/DLL，分类其运行时依赖，收集相关的资源文件夹，并生成可部署目录、ZIP 压缩包或 NSIS 安装程序。

---

## 界面预览

|            wxWidgets 前端             |                 Qt 前端                 |
| :-----------------------------------: | :-------------------------------------: |
| ![LibDeployWx](./image/LibDeploy.png) | ![LibDeployQt](./image/LibDeployQt.png) |

---

## 核心功能

- **依赖分析** — 解析 PE 导入表，通过 BFS 构建完整依赖树。
- **DLL 分类** — 区分系统核心 DLL、网络 DLL、第三方 DLL、可再发行 DLL 和 ApiSet DLL。
- **资源打包** — 保留 `assets/`、`images/`、`scripts/`、`docs/`、`plugins/`、`packages/`、`webview2_runtime/` 等文件夹。
- **目录智能跳过** — 自动忽略大小写精确匹配的构建输出目录（`bin/`、`release/`、`debug/`、`x64/`、`_deploy/` 等），避免污染部署结果。
- **部署** — 将所需文件复制到干净的部署目录。
- **ZIP 打包** — 生成 ZIP 压缩包，并实时显示逐文件压缩进度（0 → 100%）。
- **NSIS 安装程序** — 生成含开始菜单和桌面快捷方式的 NSIS 安装程序。
- **异步操作** — 所有耗时任务（分析、部署、ZIP、安装程序）均在后台线程运行，同时显示实时进度对话框，界面不卡顿。
- **最近文件** — 文件菜单记录最近打开的 10 个可执行文件，一键重新打开。
- **日志存档与历史** — 每次分析会话自动保存至 `logs/YYYY-MM-DD_HH-MM-SS_App.log`，可在应用内浏览和导出。
- **兼容性检查** — 提示 WebView2 Runtime 版本与 Windows 7 / 8.1 的兼容性问题。
- **界面特性** — 支持在应用内通过"语言"菜单切换中/英文，以及亮色/暗色/跟随系统主题。

---

## 前端界面

LibDeploy 提供了两套桌面前端，共享同一个核心引擎。

| 前端           | 路径           | 构建依赖                     | 备注                                          |
| -------------- | -------------- | ---------------------------- | --------------------------------------------- |
| **wxWidgets**  | `app/`         | CMake + MinGW-w64            | wxWidgets 和运行库已打包在仓库中。            |
| **Qt Widgets** | `qt_frontend/` | CMake + Qt SDK + MSVC 工具链 | 使用 `windeployqt` 自动复制 Qt 运行库。       |

**共享核心模块：**

```text
engine/     PE 解析、DLL 分类、依赖解析、部署逻辑
config/     JSON 配置管理
```

---

## 构建指南

详细构建说明请参考 [docs/BUILD.md](docs/BUILD.md)。

### 🔧 wxWidgets 构建

**所需工具：**

- CMake 3.20+
- MinGW-w64 GCC 13+

所有 wxWidgets 构建依赖均已**内嵌**：

```text
third_party/wxwidgets/   # wxWidgets UI 库
third_party/zlib/        # 压缩库
third_party/minizip/     # ZIP 打包
third_party/nlohmann/    # JSON 解析
runtime/                 # MinGW 运行库
tools/nsis/              # NSIS 安装程序生成工具（完整打包）
```

> ✅ 无需 vcpkg、无需网络、无需安装 NSIS、无需额外第三方库。

```bat
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

**输出：** `build/bin/LibDeploy.exe`

---

### ⚙️ Qt 构建

Qt 前端复用相同的内嵌核心依赖，但构建 Qt UI 需要**本地 Qt SDK**。

**需要的 Qt 组件：**

- Qt Widgets
- Qt Concurrent
- Qt LinguistTools
- `windeployqt`

**已验证的本地套件：** `Qt 5.12.12 msvc2017_64`

```bat
cmake -S .\qt_frontend -B .\build_qt ^
  -DCMAKE_PREFIX_PATH=C:\Qt\Qt5.12.12\5.12.12\msvc2017_64
cmake --build .\build_qt --config Release -j4
```

**输出：** `build_qt/bin/Release/LibDeployQt.exe`

> 💡 构建完成后，Qt 的 DLL、插件、MSVC 运行库、NSIS 工具、配置文件和翻译文件会自动复制到发布文件夹。**最终用户无需单独安装 Qt。**

---

## 使用方法

1. **打开** — 点击"浏览"或使用"文件 → 打开可执行文件"选择 `.exe` 或 `.dll`。
2. **分析** — 点击"分析"。后台线程解析完整依赖树，进度对话框实时显示状态。
3. **查看** — 在依赖树中查看各分类（已找到 / 缺失 / 系统 / 可再发行），右侧面板显示详细信息。
4. **部署** — 点击"部署"，将所有所需文件复制到指定目录。
5. **打包** — 使用"打包 ZIP"或"生成安装程序"创建可分发的压缩包或 NSIS 安装程序。

**使用提示：**

- 如果 DLL 不在可执行文件旁边，可在"搜索路径"中添加额外的搜索目录。
- **系统路径搜索** — 默认启用"跟随系统 PATH"，自动在 Windows 系统目录和 PATH 变量中搜索 DLL（如 System32 中的系统库）。可通过编辑 `libdeploy_config.json` 修改 `follow_system_path` 字段进行开关：
  ```json
  {
    "follow_system_path": true,   // 启用系统 PATH 搜索（推荐 true）
    ...
  }
  ```
- **NSIS 工具配置** — NSIS 已**完全内嵌**在 `tools/nsis/` 中，构建时自动复制到发布目录。生成安装程序时会**自动优先使用内嵌版本**，无需系统安装 NSIS。不同开发环境**完全无需修改配置**，开箱即用。若要使用系统安装的 NSIS 或自定义路径，可在 `libdeploy_config.json` 中指定：
  ```json
  {
    "nsis": {
      "makensis_path": ""  // 空字符串 = 使用内嵌版本（推荐）
      // 或指定系统路径：
      // "makensis_path": "C:/Program Files (x86)/NSIS/makensis.exe"
    }
  }
  ```
- 只有看起来像应用资源的目录会被复制；内置跳过项如 `bin/`、`build/`、`debug/`、`release/`、`x64/`、`x86/` 等按大小写精确匹配。
- 可通过 `classifier.extra_excluded_dirs` 增加自定义资源扫描排除目录（目录名匹配区分大小写），适合排除 Conda/Python 运行环境目录。
- 两套前端的"排除目录"面板支持逗号批量输入（支持英文逗号`,` 和中文逗号`，`），并按大小写区分不同目录名。
- 用户添加的自定义资源扫描路径会把匹配的资源子目录和顶层相关资源文件纳入分析结果。
- 分析完成后，可在依赖树中选中依赖、资源目录或数据文件，点击"移除选中项"从本次部署/ZIP 结果中剔除。
- 两套前端均支持将依赖树中的 DLL 项拖拽到"排除目录"面板，快速加入目录名。
- 使用"文件 → 最近文件"快速重新打开之前分析过的目标。
- 打开"文件 → 历史日志"可浏览或导出任意历史分析会话日志。

---

## 仓库结构

```text
LibDeploy/
├── app/                  # wxWidgets 前端
├── qt_frontend/          # Qt Widgets 前端
├── engine/               # 核心依赖分析与部署逻辑
├── config/               # JSON 配置管理
├── cmake/                # CMake 辅助脚本
├── docs/                 # 项目文档
├── locale/               # wxWidgets .po 翻译源文件
├── assets/               # 应用图标和原始素材
├── third_party/          # 内嵌的第三方源码/预编译库
├── runtime/              # wxWidgets 发布所需的 MinGW 运行库
├── tools/                # 内嵌的部署工具
├── CMakeLists.txt
├── libdeploy_config.json
└── .gitignore
```

---

## 依赖说明

### wxWidgets 前端的内嵌依赖

| 依赖            | 位置                     | 用途                                    |
| --------------- | ------------------------ | --------------------------------------- |
| wxWidgets 3.3.1 | `third_party/wxwidgets/` | wxWidgets UI，预编译 x64 MinGW 动态库包 |
| zlib            | `third_party/zlib/`      | 压缩支持                                |
| minizip         | `third_party/minizip/`   | ZIP 打包                                |
| nlohmann/json   | `third_party/nlohmann/`  | JSON 配置解析                           |
| MinGW runtime   | `runtime/`               | 复制到 `LibDeploy.exe` 旁边的运行库 DLL |
| NSIS            | `tools/nsis/`            | 安装程序生成                            |

### Qt 前端使用的依赖

| 依赖             | 来源                        | 用途                 |
| ---------------- | --------------------------- | -------------------- |
| zlib             | `third_party/zlib/`         | 共享压缩依赖         |
| minizip          | `third_party/minizip/`      | 共享 ZIP 打包依赖    |
| nlohmann/json    | `third_party/nlohmann/`     | 共享 JSON 配置解析   |
| NSIS             | `tools/nsis/`               | 安装程序生成         |
| Qt Widgets       | 本地 Qt SDK                 | Qt UI                |
| Qt Concurrent    | 本地 Qt SDK                 | 后台分析/部署任务    |
| Qt LinguistTools | 本地 Qt SDK                 | 翻译编译             |
| MSVC runtime     | Visual Studio / Build Tools | 复制到 Qt 发布文件夹 |

---

## 使用的开源项目

| 项目                    | 用途             | 许可证                                     |
| ----------------------- | ---------------- | ------------------------------------------ |
| wxWidgets               | wxWidgets 前端   | wxWindows Library Licence                  |
| Qt                      | Qt 前端          | LGPL/GPL/商业（取决于 Qt SDK）             |
| zlib                    | 压缩             | zlib License                               |
| minizip                 | ZIP 打包         | zlib License                               |
| nlohmann/json           | JSON 配置        | MIT License                                |
| NSIS                    | 安装程序生成     | zlib/libpng-style license                  |
| GCC / MinGW-w64 runtime | wxWidgets 运行库 | GPL runtime exception / MinGW-w64 licenses |
| CMake                   | 构建系统         | BSD 3-Clause License                       |

**额外内嵌工具：**

| 工具             | 位置                | 备注                                    |
| ---------------- | ------------------- | --------------------------------------- |
| Everything tools | `tools/everything/` | 辅助工具。Everything **不是**开源项目。 |

---

## 流程图

### 系统架构

两个前端（wxWidgets 和 Qt Widgets）共享相同的核心引擎、配置和第三方库。

```mermaid
flowchart TD
    subgraph 前端
        A[wxWidgets 前端<br/>app/]
        B[Qt Widgets 前端<br/>qt_frontend/]
    end

    subgraph 核心
        C[引擎<br/>engine/]
        D[配置<br/>config/]
        E[第三方库<br/>third_party/]
    end

    subgraph 输出
        F[部署目录]
        G[ZIP 压缩包]
        H[NSIS 安装程序]
    end

    A --> C
    B --> C
    C --> D
    C --> E
    C --> F
    F --> G
    F --> H
```

### 依赖分析流程

```mermaid
flowchart LR
    Input[目标 EXE/DLL] --> PE[PE 导入表解析器]
    PE --> Tree[构建依赖树<br/>BFS]
    Tree --> Classify[DLL 分类]
    Classify --> OS[系统/网络 DLL]
    Classify --> ThirdParty[第三方 DLL]
    Classify --> Redist[可再发行 DLL]
    Classify --> ApiSet[ApiSet DLL]

    ThirdParty --> Copy[复制到部署目录]
    Redist --> Copy
    OS --> Ignore[忽略]
    ApiSet --> Ignore

    Copy --> Resources[收集资源文件夹<br/>assets, images, scripts, ...]
    Resources --> Package[生成 ZIP / NSIS]
```

### DLL 分类决策树

```mermaid
flowchart TD
    Start[发现 DLL] --> IsKnownOS{是否在系统/网络白名单？}
    IsKnownOS -->|是| OS[标记为系统 DLL<br/>不部署]
    IsKnownOS -->|否| IsThirdParty{是否在第三方列表？}
    IsThirdParty -->|是| ThirdParty[标记为第三方 DLL<br/>部署]
    IsThirdParty -->|否| IsRedist{是否可再发行？}
    IsRedist -->|是| Redist[标记为可再发行<br/>按需部署]
    IsRedist -->|否| IsApiSet{是否为 ApiSet DLL？}
    IsApiSet -->|是| ApiSet[标记为 ApiSet<br/>不部署]
    IsApiSet -->|否| Unknown[标记为未知<br/>警告用户]
```

### 异步操作流程

```mermaid
flowchart LR
    UI[用户操作] --> BG[后台线程]
    BG -->|进度回调| PD[进度对话框]
    BG -->|完成| Main[主线程]
    Main --> Result[更新树视图 / 日志 / 状态]
```

### 构建流程对比（wxWidgets vs Qt）

```mermaid
flowchart LR
    subgraph WxBuild[wxWidgets 构建]
        WC[CMake + MinGW] --> WB[构建 engine + app] --> WOut[LibDeploy.exe<br/>+ 内嵌运行库]
    end

    subgraph QtBuild[Qt 构建]
        QC[CMake + MSVC<br/>+ Qt SDK] --> QB[构建 qt_frontend] --> QOut[LibDeployQt.exe]
        QOut --> Windeployqt[windeployqt] --> QRelease[发布文件夹<br/>包含 Qt DLL 与插件]
    end
```

## 📊 项目统计

#### 提交看板

<p align="center">
  <img src="https://repobeats.axiom.co/api/embed/ec4a67b7db8fa562e4b268110907cb346e362101.svg" alt="Repository Beats">
</p>
