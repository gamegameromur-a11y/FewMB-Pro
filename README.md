# FewMB Browser

A lightweight Windows browser built on WebView2 targeting a **< 1 MB** executable.

## Architecture

```
src/
├── browser.hpp   — all types (Bookmark, HistoryItem, DownloadItem, Tab, Browser)
├── browser.cpp   — implementation
└── main.cpp      — wWinMain entry point
```

## Prerequisites

| Tool | Version |
|---|---|
| Visual Studio 2022 (MSVC v143) | 17.x |
| CMake | >= 3.20 |
| NuGet | any |
| WebView2 SDK | via NuGet (auto-fetched in CI) |

Manual SDK install:
```powershell
nuget install Microsoft.Web.WebView2 -OutputDirectory packages
```

## Build

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel $env:NUMBER_OF_PROCESSORS
```

Binary lands at `build\bin\Release\FewMB.exe`.

## 1 MB target

Enabled by default in `CMakeLists.txt` Release config:

| Flag | Effect |
|---|---|
| `/GL` + `/LTCG` | Whole-program / link-time optimisation |
| `/Gy` + `/OPT:REF` | Strip dead functions |
| `/GF` + `/OPT:ICF` | Merge duplicate strings & COMDATs |
| `/GR-` | Remove RTTI (~30 KB savings) |
| `/Ob3` | Aggressive inlining |

CI prints the binary size after every build and warns if it exceeds 1 MB.

## Key improvements over v1

- All data structs (`Bookmark`, `HistoryItem`, `DownloadItem`) declared in header — no forward-declaration issues
- `Theme` enum replaces magic integers
- `PanelFlags` struct replaces five separate `std::atomic<bool>` members
- Tab WebView visibility managed correctly (only active tab's WebView is visible)
- `setActiveTab` hides/shows WebViews instead of re-creating them
- History deduplicates consecutive identical URLs; capped at 10 000 entries
- URL resolver percent-encodes query strings
- `w2a` / `a2w` trim the trailing NUL that `WideCharToMultiByte` appends
- `nproc` replaced with `$env:NUMBER_OF_PROCESSORS` (Windows-correct)
- GitHub workflow: NuGet WebView2, proper MSVC setup, binary-size check, build summary, pre-release detection
