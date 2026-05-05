# LibDeployQt

Qt Widgets frontend for LibDeploy.

This project is intentionally separate from the original wxWidgets frontend.
It reuses the existing core modules from the parent repository:

- `../engine`
- `../config`
- `../third_party/zlib`
- `../third_party/minizip`
- `../third_party/nlohmann`
- `../tools/nsis`

## Build

```bat
cd D:\Documents\LibDeploy
cmake -S .\qt_frontend -B .\build_qt ^
  -DCMAKE_PREFIX_PATH=C:\Qt\Qt5.12.12\5.12.12\msvc2017_64
cmake --build .\build_qt --config Release -j4
```

The executable is generated at:

```text
build_qt\bin\Release\LibDeployQt.exe
```

If `windeployqt.exe` is available in the Qt kit, CMake runs it after build and
copies the Qt runtime DLLs and plugins next to the executable.
For MSVC builds, CMake also copies the required Visual C++ runtime DLLs into the
same directory so the app can start on machines without a developer environment.

## Current Feature Set

- Open EXE/DLL target
- Analyze dependency tree
- Search path management
- Drag dependency items to Excluded Directories
- Redist warning panel
- Full report dialog
- Deploy/copy dependencies
- ZIP packaging
- NSIS installer generation
- Optional Qt deploy tool integration
- English / Simplified Chinese UI, with runtime language switching

## Notes

- The original wxWidgets frontend remains unchanged in `../app`.
- The Qt project uses Visual Studio/MSVC Qt in the detected local environment.
- The shared core still owns dependency analysis and packaging behavior.
- UI language is read from `ui_language` in `libdeploy_config.json`. Empty value
  auto-detects the system language; the in-app Language menu saves the selected
  language and refreshes the visible UI immediately.
