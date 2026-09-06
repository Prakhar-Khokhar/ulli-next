cmake_minimum_required(VERSION 3.21)
project(ulli-qt
    VERSION 1.0.0
    DESCRIPTION "USB-less Linux Installer (Qt 6.x port)"
    LANGUAGES CXX
)

# ─── Standards & build options ───────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(ULLI_BUILD_TESTS "Build unit tests" ON)
option(ULLI_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# ─── Qt ──────────────────────────────────────────────────────────────────────
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Test)

qt_standard_project_setup(REQUIRES 6.5)

# ─── Compiler warnings ───────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
    if(ULLI_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(ULLI_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# ─── Sources ─────────────────────────────────────────────────────────────────
set(ULLI_CORE_SOURCES
    src/core/Result.h
    src/core/Catalog.h
    src/core/Catalog.cpp
    src/core/Distro.h
    src/core/InstallPlan.h
    src/core/InstallPlan.cpp
    src/core/InstallEngine.h
    src/core/InstallEngine.cpp
    src/core/DiskInfo.h
    src/core/ProgressLog.h
    src/core/ProgressLog.cpp
)

set(ULLI_PLATFORM_SOURCES
    src/platform/Platform.h
)

if(WIN32)
    list(APPEND ULLI_PLATFORM_SOURCES
        src/platform/Platform_windows.cpp
        src/platform/windows/DiskOps.h
        src/platform/windows/DiskOps.cpp
        src/platform/windows/BcdStore.h
        src/platform/windows/BcdStore.cpp
        src/platform/windows/EspOps.h
        src/platform/windows/EspOps.cpp
        src/platform/windows/BitLocker.h
        src/platform/windows/BitLocker.cpp
        src/platform/windows/Wmi.h
        src/platform/windows/Wmi.cpp
        src/platform/windows/Sha256.h
        src/platform/windows/Sha256.cpp
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND ULLI_PLATFORM_SOURCES
        src/platform/Platform_linux.cpp
        src/platform/linux/DiskOps.h
        src/platform/linux/DiskOps.cpp
    )
endif()

set(ULLI_UI_SOURCES
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/PlanDialog.h
    src/ui/PlanDialog.cpp
    src/ui/DistroSelector.h
    src/ui/DistroSelector.cpp
    src/ui/PartitionEditor.h
    src/ui/PartitionEditor.cpp
    src/ui/LogView.h
    src/ui/LogView.cpp
    src/ui/RestartCountdownDialog.h
    src/ui/RestartCountdownDialog.cpp
)

set(ULLI_APP_SOURCES
    src/app/main.cpp
)

# ─── Core library (no Qt GUI dependency; used by both UI and tests) ─────────
add_library(ulli-core STATIC ${ULLI_CORE_SOURCES} ${ULLI_PLATFORM_SOURCES})
target_include_directories(ulli-core PUBLIC src)
target_link_libraries(ulli-core PUBLIC Qt6::Core Qt6::Network)
if(WIN32)
    target_link_libraries(ulli-core PUBLIC ole32 oleaut32 advapi32 wbemuuid)
endif()

# ─── Executable ──────────────────────────────────────────────────────────────
qt_add_executable(ulli-qt ${ULLI_APP_SOURCES} ${ULLI_UI_SOURCES})
target_link_libraries(ulli-qt PRIVATE ulli-core Qt6::Gui Qt6::Widgets)
if(WIN32)
    set_target_properties(ulli-qt PROPERTIES WIN32_EXECUTABLE TRUE)
    # Embed distros.json as a resource so the binary works even if the user
    # drops just the .exe. The external distros.json in the same directory
    # still wins if present.
    target_sources(ulli-qt PRIVATE resources/fallback_distros.qrc)
endif()

# ─── Tests ───────────────────────────────────────────────────────────────────
if(ULLI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ─── Install ─────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS ulli-qt
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Ship distros.json next to the binary on install
if(EXISTS "${CMAKE_SOURCE_DIR}/../distros.json")
    install(FILES "${CMAKE_SOURCE_DIR}/../distros.json"
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ulli-qt
        RENAME distros.json
    )
endif()
