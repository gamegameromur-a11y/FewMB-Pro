cmake_minimum_required(VERSION 3.20)
project(FewMB VERSION 1.0.0 LANGUAGES CXX)

# ── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Output directory ──────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ── Sources ───────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/browser.cpp
)

add_executable(FewMB WIN32 ${SOURCES})

# ── Windows defines ───────────────────────────────────────────────────────────
target_compile_definitions(FewMB PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    WINVER=0x0A00 _WIN32_WINNT=0x0A00
)

# ── Include dirs ──────────────────────────────────────────────────────────────
target_include_directories(FewMB PRIVATE
    src
    # NuGet-fetched WebView2 SDK drops headers here when using vcpkg or manual install:
    # ${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2/build/native/include
)

# ── 1 MB binary target: MSVC release flags ────────────────────────────────────
# These flags together routinely produce sub-1MB Release binaries for
# Win32 GUI apps without stripping or compression.
if (MSVC)
    target_compile_options(FewMB PRIVATE
        $<$<CONFIG:Release>:
            /O2         # Full optimisation
            /Ob3        # Aggressive inlining
            /Gy         # Enable function-level linking
            /GF         # Eliminate duplicate strings
            /GL         # Whole-program optimisation
            /GR-        # Disable RTTI (saves ~30 KB)
            /EHsc       # Standard exception handling
            /W4         # Warnings level 4
        >
        $<$<CONFIG:Debug>:/Od /Zi /W4>
    )

    target_link_options(FewMB PRIVATE
        $<$<CONFIG:Release>:
            /LTCG           # Link-time code generation (pairs with /GL)
            /OPT:REF        # Remove unused functions/data
            /OPT:ICF        # Fold identical COMDATs
            /SUBSYSTEM:WINDOWS
            /ENTRY:wWinMainCRTStartup
        >
        $<$<CONFIG:Debug>:/DEBUG /SUBSYSTEM:WINDOWS>
    )
endif()

# ── Link libraries ────────────────────────────────────────────────────────────
target_link_libraries(FewMB PRIVATE
    winhttp
    urlmon
    ole32
    uuid
    comctl32
    shell32
    # WebView2: link the loader stub (WebView2Loader.dll is runtime)
    # Adjust path to wherever your WebView2 SDK is installed:
    # "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2/build/native/x64/WebView2LoaderStatic.lib"
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS FewMB DESTINATION bin)
install(
    FILES
        # WebView2Loader.dll must ship alongside the .exe
        # "${WV2_LOADER_DLL}"
    DESTINATION bin
    OPTIONAL
)
