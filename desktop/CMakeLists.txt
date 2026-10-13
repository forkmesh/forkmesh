cmake_minimum_required(VERSION 3.16)
project(ForkMesh VERSION 1.2.8 LANGUAGES CXX)

# Packages own their desktop build descriptors and resources. Reconfiguring
# discovers every catalogued package, so adding/removing a package never
# requires another source/include/resource list in this core CMake file.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(FORKMESH_MARKETPLACE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../marketplace")
set(FORKMESH_MARKETPLACE_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/marketplace")
file(GLOB_RECURSE FORKMESH_MARKETPLACE_CONFIGURE_INPUTS CONFIGURE_DEPENDS
    "${FORKMESH_MARKETPLACE_ROOT}/catalog.json"
    "${FORKMESH_MARKETPLACE_ROOT}/packs.json"
    "${FORKMESH_MARKETPLACE_ROOT}/guides.json"
    "${FORKMESH_MARKETPLACE_ROOT}/catalog.schema.json"
    "${FORKMESH_MARKETPLACE_ROOT}/manifest.schema.json"
    "${FORKMESH_MARKETPLACE_ROOT}/agent.schema.json"
    "${FORKMESH_MARKETPLACE_ROOT}/packages/*/manifest.json"
    "${FORKMESH_MARKETPLACE_ROOT}/packages/*/content/*"
    "${FORKMESH_MARKETPLACE_ROOT}/packages/*/preview/*")
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
            "${FORKMESH_MARKETPLACE_ROOT}/tools/generate_desktop_packages.py"
            --catalog "${FORKMESH_MARKETPLACE_ROOT}/catalog.json"
            --qrc "${FORKMESH_MARKETPLACE_GENERATED_DIR}/marketplace.qrc"
            --cmake "${FORKMESH_MARKETPLACE_GENERATED_DIR}/packages.cmake"
            --registry-cpp
                "${FORKMESH_MARKETPLACE_GENERATED_DIR}/DesktopPackageFactories.cpp"
    RESULT_VARIABLE FORKMESH_MARKETPLACE_GENERATE_RESULT
    ERROR_VARIABLE FORKMESH_MARKETPLACE_GENERATE_ERROR)
if(NOT FORKMESH_MARKETPLACE_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Marketplace package discovery failed: ${FORKMESH_MARKETPLACE_GENERATE_ERROR}")
endif()
include("${FORKMESH_MARKETPLACE_GENERATED_DIR}/packages.cmake")
if(FORKMESH_MARKETPLACE_RESOURCE_GLOBS)
    file(GLOB_RECURSE FORKMESH_MARKETPLACE_RESOURCE_INPUTS CONFIGURE_DEPENDS
        ${FORKMESH_MARKETPLACE_RESOURCE_GLOBS})
endif()

# The version stamped into the binary. It tracks the project() version above on
# every configure, so bumping that line (e.g. cutting a release) immediately
# updates the version the app reports — including the window title — even when an
# existing build dir is reconfigured in place by the in-app quick-update. The
# release workflow can still pin an exact tag with -DFORKMESH_VERSION_OVERRIDE=<tag>
# so the published artifact reports the release tag it was cut from (see
# .forkmesh/release.yml).
#
# FORKMESH_VERSION itself must NOT be a CACHE variable: a cached default is
# written once at the first configure and never overwritten on later configures
# (set(... CACHE ...) leaves an existing cache entry alone), so a reused build
# dir would keep baking the original version into the binary and the title would
# keep showing the old version after a release. The override is a separate cache
# variable, and dereferencing FORKMESH_VERSION below reads this normal variable,
# which shadows any stale FORKMESH_VERSION left in an older build dir's cache.
set(FORKMESH_VERSION_OVERRIDE "" CACHE STRING
    "Pin the version stamped into the binary (release workflow); empty tracks project()")
if(FORKMESH_VERSION_OVERRIDE)
    set(FORKMESH_VERSION "${FORKMESH_VERSION_OVERRIDE}")
else()
    set(FORKMESH_VERSION "${PROJECT_VERSION}")
endif()

# Embed exact code-source provenance independently of semver. Metadata-only
# commits under .forkmesh/releases are excluded because they land after the
# artifact exists. The generated header is refreshed before every build, so a
# dirty tracked OR untracked relevant file produces "dirty" rather than letting
# a development binary falsely claim the last committed revision.
set(FORKMESH_BUILD_COMMIT_OVERRIDE "" CACHE STRING
    "Exact Git commit embedded in the binary; required for archive release builds")
if(FORKMESH_BUILD_COMMIT_OVERRIDE)
    string(TOLOWER "${FORKMESH_BUILD_COMMIT_OVERRIDE}"
           FORKMESH_BUILD_COMMIT_OVERRIDE)
    string(LENGTH "${FORKMESH_BUILD_COMMIT_OVERRIDE}"
           FORKMESH_BUILD_COMMIT_OVERRIDE_LENGTH)
    if(NOT FORKMESH_BUILD_COMMIT_OVERRIDE MATCHES "^[0-9a-f]+$" OR
       (NOT FORKMESH_BUILD_COMMIT_OVERRIDE_LENGTH EQUAL 40 AND
        NOT FORKMESH_BUILD_COMMIT_OVERRIDE_LENGTH EQUAL 64))
        message(FATAL_ERROR
            "FORKMESH_BUILD_COMMIT_OVERRIDE must be an exact 40- or 64-hex Git commit")
    endif()
endif()

# Baked into the binaries for rebuild-only developer actions and diagnostics.
# Update & restart deliberately uses an app-managed source clone instead, so it
# cannot pull, replace, or build inside this working copy. Global is fine here —
# the value never changes between builds of the same checkout, so it never
# invalidates incremental builds. (It also must stay global: the inline
# workingClientDir() in MainWindowInternal.h reads it, so every TU has to see the
# same value or the ODR is violated.)
add_compile_definitions(
    FORKMESH_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# FORKMESH_VERSION changes on every release bump, so it must NOT be on compile
# command lines at all — as a global definition it turned every bump into a
# full rebuild of every target, and even a per-source COMPILE_DEFINITIONS
# property rewrites the target's build.make, which every object in the target
# depends on under the Makefile generator (same full rebuild). Instead it goes
# into a generated header that only the handful of TUs reading the version
# include; configure_file rewrites it only when the version actually changes,
# so a bump recompiles just those files. The build commit, which changes with
# every commit anywhere in the checkout, is in ForkMeshBuildCommit.h, read by
# main.cpp and MainWindowHostInstall.cpp only; in ForkMeshVersion.h it made
# every commit recompile the 17 files (the heaviest MainWindow units among
# them) that only want the version.
configure_file(
    cmake/GenerateForkMeshVersion.cmake.in
    GenerateForkMeshVersion.cmake
    @ONLY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -P
            "${CMAKE_CURRENT_BINARY_DIR}/GenerateForkMeshVersion.cmake"
    RESULT_VARIABLE FORKMESH_VERSION_GENERATE_RESULT)
if(NOT FORKMESH_VERSION_GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not generate ForkMesh build provenance")
endif()
add_custom_target(forkmesh-version-header ALL
    COMMAND "${CMAKE_COMMAND}" -P
            "${CMAKE_CURRENT_BINARY_DIR}/GenerateForkMeshVersion.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/ForkMeshVersion.h"
               "${CMAKE_CURRENT_BINARY_DIR}/ForkMeshBuildCommit.h"
    COMMENT "Refreshing ForkMesh source provenance")
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Wrap compilation in ccache when available: object files survive build-dir
# wipes (run.sh rebuild) and branch switches, so those come back warm.
find_program(FORKMESH_CCACHE ccache)
if(FORKMESH_CCACHE)
    # ccache deliberately refuses PCH-backed compilations unless these two
    # safety assumptions are explicit. CMake force-includes the PCH before any
    # source-level definitions, and the client does not use the clock macros,
    # so both assumptions hold here. Pass them per invocation instead of
    # changing the developer's global ccache configuration.
    set(CMAKE_CXX_COMPILER_LAUNCHER
        "${FORKMESH_CCACHE};sloppiness=pch_defines,time_macros")
endif()

if(UNIX AND NOT APPLE)
    find_program(FORKMESH_FLOCK flock)
    if(FORKMESH_FLOCK)
        set(FORKMESH_COMPILE_GATE
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/forkmesh-compile-gate.sh")
        list(PREPEND CMAKE_CXX_COMPILER_LAUNCHER "${FORKMESH_COMPILE_GATE}")
        if(NOT CMAKE_VERSION VERSION_LESS 3.21)
            set(CMAKE_CXX_LINKER_LAUNCHER "${FORKMESH_COMPILE_GATE}")
        endif()
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# The MainWindow*.cpp translation units are enormous (MainWindow.h +
# MainWindowInternal.h alone are ~800 KB of headers), and cc1plus peaks between
# 0.6 GB and 1.6 GB compiling each one. A plain `ninja`/`cmake --build -j$(nproc)`
# runs dozens of them at once on a many-core box, which swamps physical RAM and
# shoves the whole machine into swap — and several builds (agent worktrees, PR
# previews, the quick-update) can be running concurrently. Cap how many of the
# heavy targets' compiles may run at the same time with a Ninja job pool sized
# by physical RAM and never above four concurrent heavy compiles, regardless
# of what -j the invoker passes. Four measured worst-case translation units
# use roughly 6.4 GiB, leaving headroom for the running desktop and deployment
# tools. Targets without the app sources keep full parallelism. The Makefile
# generator ignores job pools; there the live-memory-capped -j of run.sh /
# ramCappedBuildJobs() is the only guard.
cmake_host_system_information(RESULT FORKMESH_TOTAL_RAM_MB
                              QUERY TOTAL_PHYSICAL_MEMORY)
cmake_host_system_information(RESULT FORKMESH_LOGICAL_CORES
                              QUERY NUMBER_OF_LOGICAL_CORES)
if(NOT FORKMESH_TOTAL_RAM_MB GREATER 0)
    set(FORKMESH_TOTAL_RAM_MB 8192) # query failed: assume a small machine
endif()
if(NOT FORKMESH_LOGICAL_CORES GREATER 0)
    set(FORKMESH_LOGICAL_CORES 2)
endif()
# Size the pool from physical RAM: keep 6 GiB outside the build and budget
# 2 GiB per heavy compile (the measured peak is ~1.6 GiB), never above twelve.
# The old hard ceiling of four made a 36-core, 32 GiB box compile the 22
# giant units four at a time, which is what made "Rebuild & restart" take
# four to eleven minutes. Live memory pressure is still the run-time guard:
# run.sh / ramCappedBuildJobs() pass a -j derived from MemAvailable, and the
# pool only ever narrows that.
math(EXPR FORKMESH_HEAVY_JOBS "(${FORKMESH_TOTAL_RAM_MB} - 6144) / 2048")
if(FORKMESH_HEAVY_JOBS LESS 1)
    set(FORKMESH_HEAVY_JOBS 1)
elseif(FORKMESH_HEAVY_JOBS GREATER FORKMESH_LOGICAL_CORES)
    set(FORKMESH_HEAVY_JOBS ${FORKMESH_LOGICAL_CORES})
endif()
if(FORKMESH_HEAVY_JOBS GREATER 12)
    set(FORKMESH_HEAVY_JOBS 12)
endif()
# CI sizes its own pool (codemagic.yaml macos-desktop): the budget above is
# GCC's, and clang on a small Mac fits more jobs than it allows.
set(FORKMESH_HEAVY_JOBS_OVERRIDE "" CACHE STRING
    "Size of the heavy-compile job pool; empty derives it from RAM")
if(FORKMESH_HEAVY_JOBS_OVERRIDE)
    set(FORKMESH_HEAVY_JOBS ${FORKMESH_HEAVY_JOBS_OVERRIDE})
endif()
set_property(GLOBAL PROPERTY JOB_POOLS forkmesh_heavy=${FORKMESH_HEAVY_JOBS})

# RelWithDebInfo (-O2 -g) is what the agent-session builds use, and at -O2 -g
# GCC's variable-location tracking is the single biggest cc1plus memory
# consumer on the giant translation units. Function and line debug info — what
# the StallWatchdog's and gdb's backtraces actually read — survives without it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -fno-var-tracking-assignments")
endif()
# Those backtraces need only line tables and function (and inline) entries,
# which is exactly -g1. Full -g also emits every type and local variable; on
# MainWindowChat.cpp that measured 141 s / 2.7 GB peak / 48 MB object against
# 104 s / 2.0 GB / 22 MB with -g1, paid on every self-rebuild.
if(NOT MSVC)
    string(REGEX REPLACE "(^| )-g( |$)" "\\1-g1\\2"
           CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network Svg Concurrent)
# OpenSSL 3.0 builds; ML-KEM-768 (private mirrors, owner envelopes, Password
# Vault) and Argon2id (Password Vault) need 3.5 at run time. See
# docs/desktop/build-and-test.md "OpenSSL".
option(FORKMESH_BUNDLED_OPENSSL
       "Build a pinned OpenSSL 3.5 with tools/openssl-local.sh and link it" OFF)
option(FORKMESH_REQUIRE_OPENSSL_3_5
       "Refuse to configure against an OpenSSL older than 3.5" OFF)
if(FORKMESH_BUNDLED_OPENSSL)
    if(NOT EXISTS "${FORKMESH_BUNDLED_OPENSSL_PREFIX}/.complete")
        execute_process(
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/../tools/openssl-local.sh"
            OUTPUT_VARIABLE _fm_openssl_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _fm_openssl_result)
        if(NOT _fm_openssl_result EQUAL 0
           OR NOT EXISTS "${_fm_openssl_prefix}/.complete")
            message(FATAL_ERROR "tools/openssl-local.sh could not build OpenSSL")
        endif()
        set(FORKMESH_BUNDLED_OPENSSL_PREFIX "${_fm_openssl_prefix}"
            CACHE INTERNAL "")
    endif()
    string(FIND "${OPENSSL_CRYPTO_LIBRARY}" "${FORKMESH_BUNDLED_OPENSSL_PREFIX}/"
           _fm_openssl_at)
    if(NOT _fm_openssl_at EQUAL 0)
        foreach(_fm_openssl_cache OPENSSL_INCLUDE_DIR OPENSSL_CRYPTO_LIBRARY
                OPENSSL_SSL_LIBRARY)
            unset(${_fm_openssl_cache} CACHE)
        endforeach()
    endif()
    set(OPENSSL_ROOT_DIR "${FORKMESH_BUNDLED_OPENSSL_PREFIX}")
    set(OPENSSL_USE_STATIC_LIBS OFF)
    list(APPEND CMAKE_BUILD_RPATH "${FORKMESH_BUNDLED_OPENSSL_PREFIX}/lib")
    list(APPEND CMAKE_INSTALL_RPATH "${FORKMESH_BUNDLED_OPENSSL_PREFIX}/lib")
elseif(FORKMESH_BUNDLED_OPENSSL_PREFIX)
    string(FIND "${OPENSSL_CRYPTO_LIBRARY}" "${FORKMESH_BUNDLED_OPENSSL_PREFIX}/"
           _fm_openssl_at)
    if(_fm_openssl_at EQUAL 0)
        foreach(_fm_openssl_cache OPENSSL_INCLUDE_DIR OPENSSL_CRYPTO_LIBRARY
                OPENSSL_SSL_LIBRARY)
            unset(${_fm_openssl_cache} CACHE)
        endforeach()
    endif()
endif()
find_package(OpenSSL 3.0 REQUIRED)
string(FIND "${OPENSSL_CRYPTO_LIBRARY}" "${FORKMESH_BUNDLED_OPENSSL_PREFIX}/"
       _fm_openssl_at)
if(FORKMESH_BUNDLED_OPENSSL AND NOT _fm_openssl_at EQUAL 0)
    message(FATAL_ERROR
        "FORKMESH_BUNDLED_OPENSSL found ${OPENSSL_CRYPTO_LIBRARY}, not the "
        "OpenSSL built under ${FORKMESH_BUNDLED_OPENSSL_PREFIX}")
endif()
if(OPENSSL_VERSION VERSION_LESS "3.5")
    if(FORKMESH_REQUIRE_OPENSSL_3_5)
        message(FATAL_ERROR
            "OpenSSL ${OPENSSL_VERSION} is older than 3.5 and "
            "FORKMESH_REQUIRE_OPENSSL_3_5 is ON. Pass "
            "-DFORKMESH_BUNDLED_OPENSSL=ON or -DOPENSSL_ROOT_DIR=<OpenSSL 3.5>.")
    endif()
    message(WARNING
        "OpenSSL ${OPENSSL_VERSION}: this build runs, but private mirrors, "
        "owner-sealed agent sync and the Password Vault stay unavailable "
        "(they need ML-KEM-768 and Argon2id from OpenSSL 3.5). Reconfigure "
        "with -DFORKMESH_BUNDLED_OPENSSL=ON for every feature.")
endif()
include(GNUInstallDirs)

# Link with mold when it is installed. The app links a ~330 MB RelWithDebInfo
# binary with -rdynamic; GNU ld takes ~14 s for that on the deploy box and
# mold ~3 s, on every rebuild. mold need not be on PATH for the compiler:
# -B points GCC/Clang at the directory holding ld.mold, so a copy under
# ~/.local/bin works for the desktop's own rebuild whose service PATH is
# minimal. The flag is only added once a test link with it succeeds, so a
# broken or incompatible install falls back to the default linker silently.
if(UNIX AND NOT APPLE AND NOT DEFINED FORKMESH_DISABLE_MOLD)
    find_program(FORKMESH_MOLD NAMES mold
                 HINTS "$ENV{HOME}/.local/bin" "$ENV{HOME}/.local/share/forkmesh-toolchain/mold/bin")
    if(FORKMESH_MOLD)
        get_filename_component(FORKMESH_MOLD_DIR "${FORKMESH_MOLD}" DIRECTORY)
        include(CheckLinkerFlag)
        check_linker_flag(CXX "-fuse-ld=mold;-B${FORKMESH_MOLD_DIR}"
                          FORKMESH_HAVE_MOLD)
        if(FORKMESH_HAVE_MOLD)
            add_link_options("-fuse-ld=mold" "-B${FORKMESH_MOLD_DIR}")
            message(STATUS "Linking with mold: ${FORKMESH_MOLD}")
        endif()
    endif()
endif()

# On Linux the screen-region capture must go through the XDG Desktop Portal
# (direct grabs return black under Wayland), which needs Qt's D-Bus module.
# Other platforms grab the screen directly, so it's a Linux-only dependency.
if(UNIX AND NOT APPLE)
    find_package(Qt6 6.4 REQUIRED COMPONENTS DBus)
endif()

# Qt decodes WebP only when the optional qtimageformats package is installed,
# which many Linux desktops do not ship. libwebp's decoder is a much smaller and
# far more widely present dependency, so the diff view's before/after image
# preview falls back to it for .webp (see diffDecodeWebp). Entirely optional:
# without it those previews degrade to a notice, exactly as they do today.
find_path(FORKMESH_WEBP_INCLUDE_DIR webp/decode.h)
find_library(FORKMESH_WEBP_LIBRARY NAMES webpdecoder webp)
mark_as_advanced(FORKMESH_WEBP_INCLUDE_DIR FORKMESH_WEBP_LIBRARY)
if(FORKMESH_WEBP_INCLUDE_DIR AND FORKMESH_WEBP_LIBRARY)
    message(STATUS "libwebp: ${FORKMESH_WEBP_LIBRARY} (WebP diff previews enabled)")
else()
    message(STATUS "libwebp not found: WebP diff previews need Qt's optional "
                   "qtimageformats plugin instead")
endif()

# Applied to every target that compiles the MainWindow sources.
function(forkmesh_link_webp target)
    if(FORKMESH_WEBP_INCLUDE_DIR AND FORKMESH_WEBP_LIBRARY)
        target_include_directories(${target} PRIVATE ${FORKMESH_WEBP_INCLUDE_DIR})
        target_link_libraries(${target} PRIVATE ${FORKMESH_WEBP_LIBRARY})
        target_compile_definitions(${target} PRIVATE FORKMESH_HAVE_WEBP)
    endif()
endfunction()

# Chat video attachments play inside the app when Qt Multimedia is available.
# It is a separate module that many distro Qt6 installs (and this project's own
# REQUIRED component list) leave out, so it stays entirely optional: without it
# MainWindow::showChatVideoDetail hands the file to whatever player the desktop
# opens that media type with, and a video still plays from the chat.
find_package(Qt6 6.4 QUIET COMPONENTS Multimedia MultimediaWidgets)
if(TARGET Qt6::MultimediaWidgets)
    message(STATUS "Qt6 Multimedia: chat videos play in an in-app player")
else()
    message(STATUS "Qt6 Multimedia not found: chat videos open in the system "
                   "player instead")
endif()

find_package(Qt6 6.4 QUIET COMPONENTS WebEngineWidgets)
if(TARGET Qt6::WebEngineWidgets)
    message(STATUS "Qt6 WebEngine: agents open browser windows inside the app")
else()
    message(STATUS "Qt6 WebEngine not found: agent browser windows open in the "
                   "system browser instead")
endif()

function(forkmesh_link_webengine target)
    if(TARGET Qt6::WebEngineWidgets)
        target_link_libraries(${target} PRIVATE Qt6::WebEngineWidgets)
        target_compile_definitions(${target} PRIVATE FORKMESH_HAVE_WEBENGINE)
    endif()
endfunction()

# Applied to every target that compiles the MainWindow sources.
function(forkmesh_link_multimedia target)
    if(TARGET Qt6::MultimediaWidgets)
        target_link_libraries(${target}
            PRIVATE Qt6::Multimedia Qt6::MultimediaWidgets)
        target_compile_definitions(${target} PRIVATE FORKMESH_HAVE_MULTIMEDIA)
    endif()
endfunction()

# The Agent activity window paints its field through OpenGL when Qt's OpenGL
# widgets are there (part of qtbase, but split into their own package on some
# distros), which lets it run at the display's refresh rate. Without them the
# field is painted by QPainter's raster engine at thirty frames a second.
find_package(Qt6 6.4 QUIET COMPONENTS OpenGL OpenGLWidgets)
if(TARGET Qt6::OpenGLWidgets)
    message(STATUS "Qt6 OpenGLWidgets: the agent activity field renders on the GPU")
else()
    message(STATUS "Qt6 OpenGLWidgets not found: the agent activity field is "
                   "painted in software")
endif()

function(forkmesh_link_opengl target)
    if(TARGET Qt6::OpenGLWidgets)
        target_link_libraries(${target} PRIVATE Qt6::OpenGL Qt6::OpenGLWidgets)
        target_compile_definitions(${target} PRIVATE FORKMESH_HAVE_OPENGL)
    endif()
endfunction()

# HTTP/3 (QUIC) for the requests QtNetwork cannot make: QNetworkAccessManager
# stops at HTTP/2, so src/Http3Get.cpp drives OpenSSL's QUIC client (3.2+)
# and nghttp3's HTTP/3 framing from the Qt event loop. Optional: without them
# the hosts' log feeds reach the main log over SSH only.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(NGHTTP3 QUIET IMPORTED_TARGET libnghttp3)
endif()
if(TARGET PkgConfig::NGHTTP3 AND TARGET OpenSSL::SSL
   AND OPENSSL_VERSION VERSION_GREATER_EQUAL "3.2")
    set(FORKMESH_HAVE_HTTP3 ON)
    message(STATUS "HTTP/3: OpenSSL ${OPENSSL_VERSION} QUIC + nghttp3 "
                   "${NGHTTP3_VERSION}; host logs stream over QUIC")
else()
    set(FORKMESH_HAVE_HTTP3 OFF)
    message(STATUS "HTTP/3 unavailable (needs OpenSSL >= 3.2 and nghttp3): "
                   "host logs stream over SSH")
endif()

function(forkmesh_link_power_source target)
    if(APPLE)
        target_link_libraries(${target} PRIVATE "-framework IOKit" "-framework CoreFoundation")
    endif()
endfunction()

function(forkmesh_link_http3 target)
    if(FORKMESH_HAVE_HTTP3)
        target_link_libraries(${target} PRIVATE OpenSSL::SSL PkgConfig::NGHTTP3)
        target_compile_definitions(${target} PRIVATE FORKMESH_HAVE_HTTP3)
    endif()
endfunction()

# The local mesh room carries a second transport over Bluetooth RFCOMM, which
# is what keeps it working with no Wi-Fi and no internet. Qt Bluetooth is
# another optional module (Debian: qt6-connectivity-dev), so without it the
# mesh is network-only and every other part of the room behaves the same.
find_package(Qt6 6.4 QUIET COMPONENTS Bluetooth)
if(TARGET Qt6::Bluetooth)
    message(STATUS "Qt6 Bluetooth: the local mesh also runs with no network")
else()
    message(STATUS "Qt6 Bluetooth not found: the local mesh runs over the "
                   "local network only")
endif()

function(forkmesh_link_bluetooth target)
    if(TARGET Qt6::Bluetooth)
        # Concurrent: the adapter probe blocks for a DBus timeout when BlueZ
        # is not answering, so LocalMeshNode runs it on a worker thread.
        target_link_libraries(${target} PRIVATE Qt6::Bluetooth Qt6::Concurrent)
        target_compile_definitions(${target} PRIVATE FORKMESH_HAS_BLUETOOTH)
    endif()
endfunction()

# These feature translation units all begin by parsing MainWindowInternal.h,
# which currently expands to roughly 900 KB of shared ForkMesh/Qt declarations.
# Keep the list explicit so the app can precompile that common prefix without
# forcing the much smaller backend sources to load the MainWindow UI layer.
set(FORKMESH_MAIN_WINDOW_SOURCES
    src/MainWindow.cpp
    src/MainWindowInternal1.cpp
    src/MainWindowInternal2.cpp
    src/MainWindowInternal3.cpp
    src/MainWindowInternal4.cpp
    src/MainWindowOnboarding.cpp
    src/MainWindowShared.cpp
    src/MainWindowSetup.cpp
    src/MainWindowChat.cpp
    src/MainWindowDiagnostics.cpp
    src/MainWindowLogs.cpp
    src/MainWindowNavigation.cpp
    src/MainWindowNextShell.cpp
    src/MainWindowWelcome.cpp
    src/MainWindowCommandPalette.cpp
    src/MainWindowRailShortcuts.cpp
    src/MainWindowNetworkRepos.cpp
    src/MainWindowHosts.cpp
    src/MainWindowNodes.cpp
    src/MainWindowNetworkDiagnostics.cpp
    src/MainWindowUsers.cpp
    src/MainWindowVultrMirrors.cpp
    src/MainWindowHostInstall.cpp
    src/MainWindowTasks.cpp
    src/RepositoryCalendar.cpp
    src/MainWindowCalendar.cpp
    src/MainWindowNotes.cpp
    src/MainWindowFiles.cpp
    src/MainWindowStats.cpp
    src/MainWindowIssues.cpp
    src/MainWindowSourceControl.cpp
    src/MainWindowDiscussions.cpp
    src/MainWindowPulls.cpp
    src/MainWindowMergeQueue.cpp
    src/MainWindowAgents.cpp
    src/MainWindowAgentSync.cpp
    src/MainWindowAgentUsage.cpp
    src/MainWindowAgentList.cpp
    src/MainWindowAgentFolders.cpp
    src/MainWindowAgentDetail.cpp
    src/MainWindowAgentLaunch.cpp
    src/MainWindowAgentCliSession.cpp
    src/MainWindowAgentEvents.cpp
    src/MainWindowAgentTranscript.cpp
    src/MainWindowAgentLanding.cpp
    src/MainWindowAgentCloud.cpp
    src/MainWindowAgentActions.cpp
    src/MainWindowAgentModels.cpp
    src/MainWindowAgentFeed.cpp
    src/MainWindowRepoDetail.cpp
    src/MainWindowProjects.cpp
    src/MainWindowProjectPicker.cpp
    src/MainWindowAgentAccess.cpp
    src/MainWindowGlobalAgentRules.cpp
    src/MainWindowAgentIntake.cpp
    src/MainWindowCommandPermissions.cpp
    src/MainWindowBranches.cpp
    src/MainWindowReleases.cpp
    src/MainWindowRemoteReleases.cpp
    src/MainWindowShortcuts.cpp
    src/MainWindowViews.cpp
    src/MainWindowWorkspace.cpp
    src/MainWindowTeammateHome.cpp
    src/MainWindowTeammateRail.cpp
    src/MainWindowPackageRail.cpp
    src/MainWindowTeammates.cpp
    src/MainWindowSearch.cpp
    src/MainWindowSettings.cpp
    src/MainWindowOrganizations.cpp
    src/MainWindowMarket.cpp
    src/MainWindowPackageExtensions.cpp
    src/MainWindowLedger.cpp
    src/MainWindowTimers.cpp
    src/MainWindowMcp.cpp
    src/MainWindowLocalControl.cpp
    src/MainWindowLocalControlHosts.cpp
    src/MainWindowLocalControlAgentOps.cpp
    src/MainWindowPingForward.cpp
    src/MainWindowAgentParity.cpp
    src/MainWindowPowerAlert.cpp
    src/MainWindowPingFixes.cpp
    src/MainWindowSecurityAlarm.cpp
    src/MainWindowLocalControlMergeQueue.cpp
    src/MainWindowLocalControlFleet.cpp
    src/MainWindowLocalControlFiles.cpp
    src/MainWindowLocalControlDiagnostics.cpp
    src/MainWindowLocalControlTeammates.cpp
    src/MainWindowLocalControlInsights.cpp
    src/MainWindowLocalControlDiscord.cpp
    src/MainWindowLocalControlWallet.cpp
    src/MainWindowLocalControlSecrets.cpp
    src/MainWindowLocalControlStats.cpp
    src/MainWindowLocalControlRepos.cpp
    src/MainWindowLocalControlUpdate.cpp
    src/MainWindowLocalControlTerminals.cpp
    src/MainWindowDesktopCommands.cpp
    src/MainWindowLocalControlFamilies.cpp
    src/MainWindowData.cpp
    src/MainWindowCleanup.cpp
    src/MainWindowMessages.cpp
    src/MainWindowRepos.cpp
    src/MainWindowRepoSettings.cpp
    src/MainWindowRepoSync.cpp
    src/MainWindowMirrorPush.cpp
    src/MainWindowRepoFetch.cpp
    src/MainWindowAccounts.cpp
    src/MainWindowActions.cpp
    src/MainWindowCoves.cpp
    src/MainWindowControlNode.cpp
    src/MainWindowRewardPool.cpp
    src/MainWindowMembership.cpp
    src/MainWindowAds.cpp
    src/MainWindowGenie.cpp
)

# The two big resource bundles (~86 MB of generated byte arrays) compile in
# their own object library. Listed among the app sources they were force-fed
# the MainWindow precompiled header, so every header change recompiled both
# (~65 s of CPU inside the heavy-compile slots) although nothing in them had
# changed. Here they rebuild only when an icon or a package's content does.
file(GLOB FORKMESH_THEME_NEXT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/theme/next/*.qss"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/theme/next/*.svg")
file(GLOB FORKMESH_THEME_FONT_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts/Geist*.ttf")
list(SORT FORKMESH_THEME_NEXT_FILES)
list(SORT FORKMESH_THEME_FONT_FILES)
set(FORKMESH_THEME_QRC "${CMAKE_CURRENT_BINARY_DIR}/generated/theme.qrc")
set(FORKMESH_THEME_QRC_TEXT "<RCC>\n    <qresource prefix=\"/theme/next\">\n")
foreach(FORKMESH_THEME_FILE IN LISTS FORKMESH_THEME_NEXT_FILES)
    get_filename_component(FORKMESH_THEME_ALIAS "${FORKMESH_THEME_FILE}" NAME)
    string(APPEND FORKMESH_THEME_QRC_TEXT
        "        <file alias=\"${FORKMESH_THEME_ALIAS}\">${FORKMESH_THEME_FILE}</file>\n")
endforeach()
string(APPEND FORKMESH_THEME_QRC_TEXT
    "    </qresource>\n    <qresource prefix=\"/theme/fonts\">\n")
foreach(FORKMESH_THEME_FILE IN LISTS FORKMESH_THEME_FONT_FILES)
    get_filename_component(FORKMESH_THEME_ALIAS "${FORKMESH_THEME_FILE}" NAME)
    string(APPEND FORKMESH_THEME_QRC_TEXT
        "        <file alias=\"${FORKMESH_THEME_ALIAS}\">${FORKMESH_THEME_FILE}</file>\n")
endforeach()
string(APPEND FORKMESH_THEME_QRC_TEXT "    </qresource>\n</RCC>\n")
set(FORKMESH_THEME_QRC_OLD "")
if(EXISTS "${FORKMESH_THEME_QRC}")
    file(READ "${FORKMESH_THEME_QRC}" FORKMESH_THEME_QRC_OLD)
endif()
if(NOT FORKMESH_THEME_QRC_OLD STREQUAL FORKMESH_THEME_QRC_TEXT)
    file(WRITE "${FORKMESH_THEME_QRC}" "${FORKMESH_THEME_QRC_TEXT}")
endif()

add_library(forkmesh-resources OBJECT
    resources/icons.qrc
    ${FORKMESH_THEME_QRC}
    ${FORKMESH_MARKETPLACE_GENERATED_DIR}/marketplace.qrc
)
target_link_libraries(forkmesh-resources PRIVATE Qt6::Core)
set_target_properties(forkmesh-resources PROPERTIES
    AUTOMOC OFF
    JOB_POOL_COMPILE forkmesh_heavy)

option(FORKMESH_BROWSER_PANE "Embed Qt WebEngine in the workspace Browser tab" OFF)
add_library(forkmesh-browser-pane STATIC src/WorkspaceBrowserPane.cpp)
target_link_libraries(forkmesh-browser-pane PRIVATE Qt6::Widgets)
set_target_properties(forkmesh-browser-pane PROPERTIES AUTOMOC OFF)
if(FORKMESH_BROWSER_PANE)
    find_package(Qt6 6.4 QUIET COMPONENTS WebEngineWidgets)
    if(Qt6WebEngineWidgets_FOUND)
        target_link_libraries(forkmesh-browser-pane PRIVATE Qt6::WebEngineWidgets)
        target_compile_definitions(forkmesh-browser-pane PRIVATE FORKMESH_HAVE_WEBENGINE)
    else()
        message(WARNING "FORKMESH_BROWSER_PANE is on but Qt WebEngine Widgets was not found; the Browser tab opens pages in the system browser")
    endif()
endif()

set(FORKMESH_APP_SOURCES
    src/AgentFolders.h
    src/AgentFolders.cpp
    src/Http3Get.h
    src/Http3Get.cpp
    src/AgentGuardScript.h
    src/AgentSonifier.h
    src/AgentSonifier.cpp
    src/AgentAnnouncer.h
    src/AgentAnnouncer.cpp
    src/AgentAnnouncerVoice.h
    src/AgentAnnouncerVoice.cpp
    src/AgentActivityFeed.h
    src/AgentActivityFeed.cpp
    src/BackupHostFeed.h
    src/BackupHostFeed.cpp
    src/PetHostFeed.h
    src/PetHostFeed.cpp
    src/CleanupHostFeed.h
    src/CleanupHostFeed.cpp
    src/UiSoundCues.h
    src/UiSoundCues.cpp
    src/UiSounds.h
    src/UiSounds.cpp
    src/UserActivityLog.h
    src/UserActivityLog.cpp
    src/UserActivityChart.h
    src/UserActivityChart.cpp
    src/AgentGuardrails.h
    src/AgentGuardrails.cpp
    src/AgentResources.h
    src/AgentResources.cpp
    src/AgentResourceGovernor.h
    src/AgentResourceGovernor.cpp
    src/CrashHandler.h
    src/CrashHandler.cpp
    src/ProcessReaper.h
    src/ProcessReaper.cpp
    src/CrashLogScan.h
    src/CrashLogScan.cpp
    src/DiagnosticsReport.h
    src/DiagnosticsReport.cpp
    src/SourceCheckout.h
    src/PlatformLogFilter.h
    src/PlatformLogFilter.cpp
    src/QtRuntimeDiagnostics.h
    src/QtRuntimeDiagnostics.cpp
    src/LogSource.h
    src/LogSource.cpp
    src/D1DatabaseAdmin.h
    src/D1DatabaseAdmin.cpp
    src/D1QuerySource.h
    src/D1QuerySource.cpp
    src/CoreNodeLogs.h
    src/CoreNodeLogs.cpp
    src/FleetVisualizer.h
    src/FleetVisualizer.cpp
    src/QueryInspector.h
    src/QueryInspector.cpp
    src/DesktopProcesses.h
    src/DesktopProcesses.cpp
    src/DesktopProcessView.h
    src/DesktopProcessView.cpp
    src/PlannedRestarts.h
    src/PlannedRestarts.cpp
    src/IpCountry.h
    src/IpCountry.cpp
    src/IpWhois.h
    src/IpWhois.cpp
    src/DeployLogs.h
    src/DeployLogs.cpp
    src/DatabaseManagerWidget.h
    src/DatabaseManagerWidget.cpp
    src/SingleInstance.h
    src/SingleInstance.cpp
    src/StartupSplash.h
    src/StartupSplash.cpp
    src/DesktopDisplayReadiness.h
    src/DesktopDisplayReadiness.cpp
    src/Theme.h
    src/DesignTokens.h
    src/DesignTokens.cpp
    src/UiKit.h
    src/UiKit.cpp
    src/NextShellHooks.h
    src/MainWindowNextAccess.h
    src/WindowBackdrop.h
    src/WindowFade.h
    src/AccountCapability.h
    src/ForkMeshIdentity.h
    src/ForkMeshIdentity.cpp
    src/ChatBackend.h
    src/ChatRecipients.h
    src/ClientErrorReports.h
    src/ClientErrorReports.cpp
    src/GitRefusal.h
    src/GitRefusal.cpp
    src/PingSyncState.h
    src/PingSyncState.cpp
    src/DesktopPingForward.h
    src/DesktopPingForward.cpp
    src/DesktopPowerAlert.h
    src/DesktopPowerAlert.cpp
    src/PowerSourceReader.h
    src/PowerSourceReader.cpp
    src/PingFixes.h
    src/PingFixes.cpp
    src/MainnodeRoom.h
    src/RoomCrypto.h
    src/RoomCrypto.cpp
    src/RoomSeal.h
    src/RoomSeal.cpp
    src/CoveCrypto.h
    src/CoveCrypto.cpp
    src/CoveFileSupport.h
    src/CoveFileSupport.cpp
    src/CoveFileEditor.h
    src/CoveFileEditor.cpp
    src/MirrorCrypto.h
    src/MirrorCrypto.cpp
    src/PrivateMirrorStore.h
    src/PrivateMirrorStore.cpp
    src/PrivateMirrorRuntime.h
    src/PrivateMirrorRuntime.cpp
    src/PublicMirrorRuntime.h
    src/PublicMirrorRuntime.cpp
    src/UpstreamCheckoutSync.h
    src/UpstreamCheckoutSync.cpp
    src/CodeHealth.h
    src/CodeHealth.cpp
    src/DiffIntraLine.h
    src/DiffIntraLine.cpp
    src/ComponentHealthBoard.h
    src/ComponentHealthBoard.cpp
    src/MainWindowComponentHealth.cpp
    src/HomeBoard.h
    src/HomeBoard.cpp
    src/HomeLayout.h
    src/HomeLayout.cpp
    src/MainWindowHome.cpp
    src/BinarySize.h
    src/BinarySize.cpp
    src/BinarySizeMap.h
    src/BinarySizeMap.cpp
    src/ProcessMemory.h
    src/ProcessMemory.cpp
    src/MemoryUsageChart.h
    src/MemoryUsageChart.cpp
    src/MainWindowMemory.cpp
    src/MainWindowGovernor.cpp
    src/CodeMapModel.h
    src/CodeMapModel.cpp
    src/CodeMapView.h
    src/CodeMapView.cpp
    src/MainWindowCodeMap.cpp
    src/WebBranchOps.h
    src/WebBranchOps.cpp
    src/TaskWorkItems.h
    src/TaskWorkItems.cpp
    src/TaskMarkdown.h
    src/TaskMarkdown.cpp
    src/ListViewMode.h
    src/ListViewMode.cpp
    src/SystemOpenFolder.h
    src/SystemOpenFolder.cpp
    src/AgentTicker.h
    src/AgentTicker.cpp
    src/MainWindowTaskWorkItems.cpp
    src/MainWindowAgentTaskLink.cpp
    src/MirrorPushOutcome.h
    src/MirrorSyncVerdict.h
    src/MirrorNodeColumns.h
    src/NodeRole.h
    src/NodeDirectoryRows.h
    src/RelayRepositoryScope.h
    src/CoveStore.h
    src/CoveStore.cpp
    src/ChatHistoryLimits.h
    src/ChatMessageRetention.h
    src/ChatBlocks.h
    src/ChatFormat.h
    src/ChatLinkText.h
    src/AppViewMode.h
    src/ChannelGroups.h
    src/RailFolders.h
    src/RailGroups.h
    src/RailShortcuts.h
    src/ChatVisitorPresence.h
    src/NoteListEntry.h
    src/UserStatus.h
    src/UserStatus.cpp
    src/ServerNode.h
    src/ServerNode.cpp
    src/OfficeChannelMirror.h
    src/OfficeChannelMirror.cpp
    src/BleMeshLink.h
    src/BleMeshLink.cpp
    src/LocalMeshNode.h
    src/LocalMeshNode.cpp
    src/LocalControl.h
    src/LocalControl.cpp
    src/LocalControlStream.h
    src/LocalControlStream.cpp
    src/LocalControlHosts.h
    src/LocalControlHosts.cpp
    src/LocalControlAttachments.h
    src/LocalControlAttachments.cpp
    src/OrgAgentJobAttachments.h
    src/OrgAgentJobAttachments.cpp
    src/LocalControlPromptImages.h
    src/LocalControlPromptImages.cpp
    src/LocalControlAgentOps.h
    src/LocalControlAgentOps.cpp
    src/LocalControlMergeQueue.h
    src/LocalControlMergeQueue.cpp
    src/LocalControlFleet.h
    src/LocalControlFleet.cpp
    src/LocalControlFiles.h
    src/LocalControlFiles.cpp
    src/LocalControlDiagnostics.h
    src/LocalControlDiagnostics.cpp
    src/LocalControlTeammates.h
    src/LocalControlTeammates.cpp
    src/LocalControlInsights.h
    src/LocalControlInsights.cpp
    src/LocalControlDiscord.h
    src/LocalControlDiscord.cpp
    src/LocalControlWallet.h
    src/LocalControlWallet.cpp
    src/LocalControlSecrets.h
    src/LocalControlSecrets.cpp
    src/LocalControlStats.h
    src/LocalControlStats.cpp
    src/LocalControlRepos.h
    src/LocalControlRepos.cpp
    src/LocalControlUpdate.h
    src/LocalControlUpdate.cpp
    src/LocalControlTerminals.h
    src/LocalControlTerminals.cpp
    src/DesktopCommands.h
    src/DesktopCommands.cpp
    src/MeshFirewall.h
    src/MeshFirewall.cpp
    src/NodeEventSocket.h
    src/NodeEventSocket.cpp
    src/AgentLoginPty.h
    src/AgentLoginPty.cpp
    src/ClaudeAccountRouting.h
    src/ClaudeAccountRouting.cpp
    src/BackgroundActivity.h
    src/GuiPump.h
    src/ActionTelemetry.h
    src/ActionTelemetry.cpp
    src/ActionPushRange.h
    src/ActionPushRange.cpp
    src/NetworkBackoff.h
    src/NetworkReplyError.h
    src/ClientVersionGate.h
    src/BackoffNetworkAccessManager.h
    src/BackoffNetworkAccessManager.cpp
    src/ControlNode.h
    src/ControlNode.cpp
    src/MirrorActionsConfiguration.h
    src/MirrorActionsConfiguration.cpp
    src/MirrorActionsSummary.h
    src/MirrorActionsSummary.cpp
    src/RewardPoolSigner.h
    src/RewardPoolSigner.cpp
    src/TipSigner.h
    src/TipSigner.cpp
    src/ContributorPayoutReceipt.h
    src/MainWindowTips.cpp
    src/ForkMeshPackageRegistry.h
    src/ForkMeshPackageRegistry.cpp
    src/NativePackageExtension.h
    src/NativePackageExtension.cpp
    src/PackageRailTile.h
    src/PackageRailTile.cpp
    src/RepoStatsStore.h
    src/RepoStatsStore.cpp
    src/StrictGitReader.h
    src/StrictGitReader.cpp
    src/RepoSecurity.h
    src/RepoSecurity.cpp
    src/IssueStore.h
    src/IssueStore.cpp
    src/IssueModelMention.h
    src/IssueModelMention.cpp
    src/IssueBurnup.h
    src/IssueBurnup.cpp
    src/ProjectStore.h
    src/ProjectStore.cpp
    src/ProjectGantt.h
    src/DiscussionStore.h
    src/DiscussionStore.cpp
    src/FederatedThreadView.h
    src/FederatedThreadView.cpp
    src/PullStore.h
    src/PullStore.cpp
    src/GitIndexQueue.h
    src/GitIndexQueue.cpp
    src/PullReviewModel.h
    src/PullReviewModel.cpp
    src/PullAiReview.h
    src/PullAiReview.cpp
    src/MergeQueue.h
    src/MergeQueue.cpp
    src/PullBadgeWidget.h
    src/PullBadgeWidget.cpp
    src/PacmanProgress.h
    src/PromptSpellCheckHighlighter.h
    src/PromptSpellCheckHighlighter.cpp
    src/MarkdownRender.h
    src/MarkdownDocument.h
    src/ReferenceLinks.h
    src/ActionFailureSummary.h
    src/ActionFile.h
    src/ActionFile.cpp
    src/ActionStore.h
    src/ActionStore.cpp
    src/ActionRunner.h
    src/ActionRunner.cpp
    src/AgentStore.h
    src/AgentStore.cpp
    src/AgentCloudSessions.h
    src/AgentCloudSessions.cpp
    src/AgentCloudStream.h
    src/AgentCloudStream.cpp
    src/RunnerLogin.h
    src/RunnerLogin.cpp
    src/RunnerSetupHandoff.h
    src/RunnerSetupHandoff.cpp
    src/RunnerListView.h
    src/RunnerListView.cpp
    src/AgentCliLocator.h
    src/AgentCredentialEnvironment.h
    src/AgentJail.h
    src/AgentScratch.h
    src/AgentWorktree.h
    src/AgentWorkspaceNameGate.h
    src/VirtualMachineRuntime.h
    src/VirtualMachineRuntime.cpp
    src/AgentPromptImages.h
    src/AgentPromptImages.cpp
    src/SavedSearchStore.h
    src/SavedSearchStore.cpp
    src/SecurityAlarmState.h
    src/SecurityAlarmState.cpp
    src/AdBannerModel.h
    src/AdBannerModel.cpp
    src/AdBannerWidget.h
    src/AdBannerWidget.cpp
    src/SecretDiffBanner.h
    src/SecretDiffBanner.cpp
    src/GenieVitals.h
    src/GenieVitals.cpp
    src/GenieLandmarks.h
    src/GenieLandmarks.cpp
    src/GenieSpiderPainter.h
    src/GenieSpiderPainter.cpp
    src/GenieSpiderWidget.h
    src/GenieSpiderWidget.cpp
    src/UserAvatarStore.h
    src/UserAvatarStore.cpp
    src/PromptDraftStore.h
    src/PromptDraftStore.cpp
    src/UsageLimitCalendar.h
    src/UsageLimitCalendar.cpp
    src/AgentRunner.h
    src/AgentRunner.cpp
    src/TeammateStore.h
    src/TeammateStore.cpp
    src/TeammateMood.h
    src/TeammateMood.cpp
    src/TeammateWorkspace.h
    src/TeammateWorkspace.cpp
    src/QrCode.h
    src/QrCode.cpp
    src/ReactionCatalog.h
    src/ReactionEmoji.h
    src/ReactionEmoji.cpp
    src/MessageRow.h
    src/MessageRow.cpp
    src/MarkdownEditor.cpp
    src/KebabHeaderView.h
    src/KebabHeaderView.cpp
    src/SettingsSectionsView.h
    src/SettingsSectionsView.cpp
    src/PingSettings.h
    src/PingSettingsPanel.h
    src/PingSettingsPanel.cpp
    src/FleetAutoscaleModel.h
    src/FleetAutoscalePanel.h
    src/FleetAutoscalePanel.cpp
    src/AddCoreNodeModel.h
    src/AddCoreNodeModel.cpp
    src/AddCoreNodeDialog.h
    src/AddCoreNodeDialog.cpp
    src/AwsCredentials.h
    src/AwsCredentials.cpp
    src/AwsSettingsPanel.h
    src/AwsSettingsPanel.cpp
    src/SystemdTimers.h
    src/SystemdTimers.cpp
    src/SystemdTimersPanel.h
    src/SystemdTimersPanel.cpp
    src/RemoveCoreNodeModel.h
    src/RemoveCoreNodeModel.cpp
    src/RemoveCoreNodeDialog.h
    src/RemoveCoreNodeDialog.cpp
    src/TerminalWidget.h
    src/TerminalWidget.cpp
    src/ClaudeIdeBridge.h
    src/ClaudeIdeBridge.cpp
    src/WorldSpeechBridge.h
    src/WorldSpeechBridge.cpp
    src/AgentCapabilityBroker.h
    src/AgentCapabilityBroker.cpp
    src/AgentCapabilityStore.h
    src/AgentCapabilityStore.cpp
    src/AgentPermissionController.h
    src/AgentPermissionController.cpp
    src/AgentJsonlFramer.h
    src/AgentJsonlFramer.cpp
    src/AgentRuntimeSession.h
    src/AgentRuntimeSession.cpp
    src/AgentRuntimeRegistry.h
    src/AgentRuntimeRegistry.cpp
    src/ClaudeStreamSession.h
    src/ClaudeStreamSession.cpp
    src/CodexAppServerSession.h
    src/CodexAppServerSession.cpp
    src/CodexTranscriptStyle.h
    src/Tooltips.h
    src/ClaudeTranscriptView.h
    src/ClaudeTranscriptView.cpp
    src/ClaudeTranscriptTools.h
    src/ClaudeTranscriptTools.cpp
    src/ClaudeSessionScan.h
    src/ClaudeSessionScan.cpp
    src/MachineAgentScan.h
    src/MachineAgentScan.cpp
    src/AgentLogText.h
    src/AgentLogText.cpp
    src/ClaudePeerMessaging.h
    src/ClaudePeerMessaging.cpp
    src/TerminalHandoff.h
    src/TerminalHandoff.cpp
    src/DirectorySizeScan.h
    src/DirectorySizeScan.cpp
    src/AppPaths.h
    src/AppPaths.cpp
    src/ProjectRegistry.h
    src/ProjectRegistry.cpp
    src/ProjectInvite.h
    src/ProjectInvite.cpp
    src/ProjectAgentAccess.h
    src/ProjectAgentAccess.cpp
    src/AgentAccessFailure.h
    src/AgentAccessFailure.cpp
    src/AgentNetworkGate.h
    src/AgentNetworkGate.cpp
    src/CommandVariables.h
    src/CommandVariables.cpp
    src/AgentRunRules.h
    src/AgentRunRules.cpp
    src/DestructiveCommandReview.h
    src/DestructiveCommandReview.cpp
    src/AgentAccessEditor.h
    src/AgentAccessEditor.cpp
    src/AgentIntake.h
    src/AgentIntake.cpp
    src/ReleaseDraft.h
    src/ReleaseDraft.cpp
    src/AgentWindows.h
    src/AgentWindows.cpp
    src/ScrollJumpButtons.h
    src/ScrollJumpButtons.cpp
    src/SplitterHairline.h
    src/PromptFlightOverlay.h
    src/PromptFlightOverlay.cpp
    src/StallWatchdog.h
    src/StallWatchdog.cpp
    src/SystemPressure.h
    src/SystemPressure.cpp
    src/SystemStats.h
    src/SystemStats.cpp
    src/SuggestedApps.h
    src/SuggestedApps.cpp
    src/NodeDiagnostics.h
    src/NodeDiagnostics.cpp
    src/LocalBackupStore.h
    src/LocalBackupStore.cpp
    src/LogTimelineChart.h
    src/LogTimelineChart.cpp
    src/SkillUsageChart.h
    src/SkillUsageChart.cpp
    src/AgentTokenPricing.h
    src/AgentToolCatalog.h
    src/AgentToolCatalog.cpp
    src/AgentToolEfficiencyLedger.h
    src/AgentToolEfficiencyLedger.cpp
    src/BackgroundFileWriter.h
    src/BackgroundFileWriter.cpp
    src/SettingsWrite.h
    src/AgentToolImprovementLog.h
    src/AgentToolImprovementLog.cpp
    src/AgentToolEfficiencyBoard.h
    src/AgentToolEfficiencyBoard.cpp
    src/AgentToolUsageTable.h
    src/AgentToolUsageTable.cpp
    src/AgentRunEfficiencyLog.h
    src/AgentRunEfficiencyLog.cpp
    src/AgentRunTable.h
    src/AgentRunTable.cpp
    src/AgentModelStats.h
    src/AgentModelStats.cpp
    src/AgentModelTable.h
    src/AgentModelTable.cpp
    src/StatsDashboard.h
    src/StatsDashboard.cpp
    src/AgentToolPolicy.h
    src/AgentToolPolicy.cpp
    src/AgentGuardrailLedger.h
    src/AgentGuardrailLedger.cpp
    src/AgentGuardrailBoard.h
    src/AgentGuardrailBoard.cpp
    src/NetworkLogArchive.h
    src/NetworkLogArchive.cpp
    src/RestartTiming.h
    src/McpConnector.h
    src/McpConnector.cpp
    src/DesktopPackageExtension.h
    "${FORKMESH_MARKETPLACE_GENERATED_DIR}/DesktopPackageFactories.cpp"
    ${FORKMESH_MARKETPLACE_SOURCES}
    src/StartupTrace.h
    src/MainWindowInternal.h
    src/MainWindowAgentsInternal.h
    src/MainWindowNetworkInternal.h
    src/MainWindowReposInternal.h
    src/MainWindow.h
    src/CodeSymbols.h
    src/GlobalSearchMatch.h
    src/GlobalSearchMatch.cpp
    src/SearchHitDelegate.h
    src/SearchHitDelegate.cpp
    ${FORKMESH_MAIN_WINDOW_SOURCES}
)

qt_add_executable(forkmesh
    src/main.cpp
    ${FORKMESH_APP_SOURCES}
    src/forkmesh_autogen.cpp
)
add_dependencies(forkmesh forkmesh-version-header)
# Heavy app sources: throttle concurrent compiles (see the forkmesh_heavy pool).
set_target_properties(forkmesh PROPERTIES JOB_POOL_COMPILE forkmesh_heavy)
target_link_libraries(forkmesh PRIVATE forkmesh-resources forkmesh-browser-pane)

# Every MainWindow feature source includes MainWindowInternal.h. Parse that
# shared UI/Qt prefix once, then reuse it across the 25 large translation units.
# SKIP_PRECOMPILE_HEADERS keeps the PCH narrowly scoped: forcing this UI header
# into the independent backend sources costs more than it saves and increases
# their dependency surface.
target_precompile_headers(forkmesh PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/MainWindowInternal.h>")
# Clang otherwise embeds the PCH creation time in the artifact, preventing
# ccache from recognizing an identical clean rebuild. GCC PCH files are already
# deterministic for this purpose. It is a cc1 option: the clang driver (Apple's
# included) rejects a bare -fno-pch-timestamp, so it goes through -Xclang, and
# SHELL: keeps the pair together through CMake's option de-duplication.
if(FORKMESH_CCACHE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(forkmesh PRIVATE "SHELL:-Xclang -fno-pch-timestamp")
endif()
foreach(_forkmesh_source IN LISTS FORKMESH_APP_SOURCES)
    if(NOT _forkmesh_source IN_LIST FORKMESH_MAIN_WINDOW_SOURCES)
        set_source_files_properties(${_forkmesh_source} PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON)
    endif()
endforeach()
set_source_files_properties(src/main.cpp src/forkmesh_autogen.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON)

# The app compiles its Qt meta-object code through the single, checked-in
# src/forkmesh_autogen.cpp above rather than AUTOMOC's machine-generated
# forkmesh_autogen/mocs_compilation.cpp (which lands under a hashed build
# directory and stands out from the hand-written sources). Turn AUTOMOC off for
# just this target and moc its Q_OBJECT headers explicitly; forkmesh_autogen.cpp
# #includes the results. The test targets keep AUTOMOC so their
# FORKMESH_WINDOW_TESTS-only Q_INVOKABLE hooks are still moc'd correctly.
set_target_properties(forkmesh PROPERTIES AUTOMOC OFF)
qt_wrap_cpp(FORKMESH_MOC_SOURCES
    src/ActionRunner.h
    src/AddCoreNodeDialog.h
    src/AgentLoginPty.h
    src/AgentRunner.h
    src/AgentRuntimeSession.h
    src/BackoffNetworkAccessManager.h
    src/ChatBackend.h
    src/ClaudeIdeBridge.h
    src/WorldSpeechBridge.h
    src/ClaudeStreamSession.h
    src/ClaudeTranscriptView.h
    src/GenieSpiderWidget.h
    src/KebabHeaderView.h
    src/LocalMeshNode.h
    src/MainWindow.h
    src/MarkdownEditor.h
    src/MessageRow.h
    src/NodeEventSocket.h
    src/OfficeChannelMirror.h
    src/RemoveCoreNodeDialog.h
    src/ScrollJumpButtons.h
    src/ServerNode.h
    src/SettingsSectionsView.h
    src/StallWatchdog.h
    src/TerminalWidget.h
    TARGET forkmesh
)
# Package MOCs are compiled directly instead of being named in the checked-in
# core unity file. The descriptor is therefore the only place a native package
# declares its Q_OBJECT headers.
qt_wrap_cpp(FORKMESH_MARKETPLACE_MOC_SOURCES
    ${FORKMESH_MARKETPLACE_MOC_HEADERS}
    TARGET forkmesh
)
# CodexAppServerSession is compiled as a regular moc unit rather than included
# by the checked-in forkmesh_autogen.cpp, keeping this transport self-contained.
qt_wrap_cpp(FORKMESH_CODEX_MOC_SOURCES
    src/CodexAppServerSession.h
    TARGET forkmesh
)
target_sources(forkmesh PRIVATE ${FORKMESH_CODEX_MOC_SOURCES})
# The generated units are #included by forkmesh_autogen.cpp, so keep them out of
# the object list (HEADER_FILE_ONLY) and make the unity file depend on them so
# they are regenerated whenever a header changes.
set_source_files_properties(${FORKMESH_MOC_SOURCES} PROPERTIES HEADER_FILE_ONLY ON)
set_source_files_properties(src/forkmesh_autogen.cpp PROPERTIES
    OBJECT_DEPENDS "${FORKMESH_MOC_SOURCES}")
set_source_files_properties(${FORKMESH_MARKETPLACE_MOC_SOURCES} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON)
target_sources(forkmesh PRIVATE
    ${FORKMESH_MOC_SOURCES}
    ${FORKMESH_MARKETPLACE_MOC_SOURCES})
# forkmesh_autogen.cpp #includes the moc output by bare name, and each moc unit
# in turn #includes its header, so both the moc output dir(s) and the src tree
# must be on the include path now that AUTOMOC no longer manages them.
set(FORKMESH_MOC_DIRS "")
foreach(_moc ${FORKMESH_MOC_SOURCES})
    get_filename_component(_moc_dir "${_moc}" DIRECTORY)
    list(APPEND FORKMESH_MOC_DIRS "${_moc_dir}")
endforeach()
list(REMOVE_DUPLICATES FORKMESH_MOC_DIRS)
# Nothing regenerates -- or removes -- a moc unit whose input header is gone, and
# a leftover actively shadows the live one: forkmesh_autogen.cpp #includes each
# unit by bare name, so whichever moc dir comes first on the include path wins.
# Moving the capture package's headers out of desktop/src and into marketplace/
# left build/src/moc_MarkupCanvas.cpp behind, still #including
# "../../src/MarkupCanvas.h"; build/src precedes the new
# build/__/marketplace/... dir, so the stale copy won and every configured build
# tree failed on a header path that no longer exists. Sweep the output dirs and
# drop each moc_*.cpp the current header lists no longer generate. Only the
# qt_wrap_cpp dirs are scanned -- the test targets keep AUTOMOC, which cleans up
# after itself under <target>_autogen/.
set(_forkmesh_live_moc "")
set(_forkmesh_moc_scan_dirs "")
foreach(_moc IN LISTS FORKMESH_MOC_SOURCES FORKMESH_CODEX_MOC_SOURCES
                           FORKMESH_MARKETPLACE_MOC_SOURCES)
    get_filename_component(_moc_abs "${_moc}" ABSOLUTE)
    get_filename_component(_moc_dir "${_moc_abs}" DIRECTORY)
    list(APPEND _forkmesh_live_moc "${_moc_abs}")
    list(APPEND _forkmesh_moc_scan_dirs "${_moc_dir}")
endforeach()
list(REMOVE_DUPLICATES _forkmesh_moc_scan_dirs)
foreach(_moc_dir IN LISTS _forkmesh_moc_scan_dirs)
    file(GLOB _forkmesh_moc_present "${_moc_dir}/moc_*.cpp")
    foreach(_moc IN LISTS _forkmesh_moc_present)
        if(NOT _moc IN_LIST _forkmesh_live_moc)
            message(STATUS "Removing stale moc output: ${_moc}")
            file(REMOVE "${_moc}")
        endif()
    endforeach()
endforeach()
# qt_wrap_cpp() above marks each INPUT header it moc's with the source
# property SKIP_AUTOMOC ON (see Qt6CoreMacros.cmake) -- note FORKMESH_MOC_SOURCES
# itself now holds the *generated* moc_*.cpp outputs, not these headers. Source
# file properties are directory-scoped, not target-scoped, so that also
# suppresses AUTOMOC for every one of these headers when an AUTOMOC-enabled
# test target below lists it as a plain source (every one of them is also
# part of FORKMESH_APP_SOURCES) -- their Q_OBJECT vtables/staticMetaObjects
# never get moc'd there, leaving forkmesh-tests/forkmesh-window-tests/
# forkmesh-perf-tests with undefined references at link time (caught building
# forkmesh-perf-tests for issue #357 from a clean configure --
# forkmesh-window-tests has the exact same latent bug). The forkmesh target
# doesn't need SKIP_AUTOMOC (its whole-target AUTOMOC is already OFF), so it's
# safe to reset it here for the still-AUTOMOC-enabled test targets.
set_source_files_properties(
    src/ActionRunner.h
    src/AddCoreNodeDialog.h
    src/AgentLoginPty.h
    src/AgentRunner.h
    src/AgentRuntimeSession.h
    src/BackoffNetworkAccessManager.h
    src/ChatBackend.h
    src/ClaudeIdeBridge.h
    src/WorldSpeechBridge.h
    src/ClaudeStreamSession.h
    src/CodexAppServerSession.h
    src/ClaudeTranscriptView.h
    src/GenieSpiderWidget.h
    src/KebabHeaderView.h
    src/LocalMeshNode.h
    src/MainWindow.h
    src/MarkdownEditor.h
    ${FORKMESH_MARKETPLACE_MOC_HEADERS}
    src/MessageRow.h
    src/NodeEventSocket.h
    src/OfficeChannelMirror.h
    src/RemoveCoreNodeDialog.h
    src/ScrollJumpButtons.h
    src/ServerNode.h
    src/SettingsSectionsView.h
    src/StallWatchdog.h
    src/TerminalWidget.h
    PROPERTIES SKIP_AUTOMOC OFF SKIP_AUTOGEN OFF
)
target_include_directories(forkmesh PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FORKMESH_MARKETPLACE_INCLUDE_DIRS}
    ${FORKMESH_MOC_DIRS})

target_link_libraries(forkmesh PRIVATE Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Concurrent OpenSSL::Crypto)
forkmesh_link_webp(forkmesh)
forkmesh_link_multimedia(forkmesh)
forkmesh_link_webengine(forkmesh)
forkmesh_link_opengl(forkmesh)
forkmesh_link_http3(forkmesh)
forkmesh_link_power_source(forkmesh)
forkmesh_link_bluetooth(forkmesh)
# TerminalWidget runs commands under a PTY via forkpty(); on Linux that lives in
# libutil (macOS provides it in libSystem, so no extra link there).
if(UNIX)
    target_sources(forkmesh PRIVATE src/ForkMeshTerm.h src/ForkMeshTerm.cpp)
    target_compile_definitions(forkmesh PRIVATE FORKMESH_HAVE_TERM)
    add_executable(forkmesh-term src/forkmesh_term_main.cpp src/ForkMeshTerm.h src/ForkMeshTerm.cpp)
    target_compile_definitions(forkmesh-term PRIVATE FORKMESH_VERSION="${FORKMESH_VERSION}")
    if(NOT APPLE)
        target_link_libraries(forkmesh-term PRIVATE util)
    endif()
endif()
if(UNIX AND NOT APPLE)
    target_link_libraries(forkmesh PRIVATE util)
    # XDG Desktop Portal screenshot path (see ScreenCaptureOverlay).
    target_link_libraries(forkmesh PRIVATE Qt6::DBus)
    target_compile_definitions(forkmesh PRIVATE FORKMESH_HAVE_PORTAL)
    # Export symbols so the StallWatchdog's backtrace_symbols() resolves function
    # names (not just addresses) when it samples a frozen UI thread.
    target_link_options(forkmesh PRIVATE -rdynamic)
    # The signal handler walks frame pointers using only bounded memory reads.
    # Preserve those links in optimized builds so sampling never needs the
    # allocator-backed unwinder that can corrupt an interrupted malloc/free.
    target_compile_options(forkmesh PRIVATE -fno-omit-frame-pointer)
endif()

# The file/folder tree icons under icons/ are loaded at runtime through
# MainWindow::iconsDir() (MainWindowRepoDetail.cpp), which falls back to a
# loose "icons" directory beside the executable when it isn't running from a
# source checkout. None of the packagers (AppImage, NSIS, .dmg) or the bare
# published binary ship that directory alongside the executable, so those
# icons never appeared on anything but a from-source dev build (issue #417).
# Bake the whole set into the binary as a Qt resource instead, so it's always
# present regardless of how the app was installed.
file(GLOB FORKMESH_TREE_ICON_FILES "${CMAKE_CURRENT_SOURCE_DIR}/icons/*.svg")
qt_add_resources(forkmesh "treeicons"
    PREFIX "/icons/tree"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/icons"
    BIG_RESOURCES
    FILES ${FORKMESH_TREE_ICON_FILES}
)

# A GUI-subsystem executable on Windows: otherwise every launch opens a console
# window beside the client (tools/package/windows.sh also rewrites the header
# for release commits that predate this property).
if(WIN32)
    set_target_properties(forkmesh PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE)
    set_target_properties(forkmesh PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "ForkMesh"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.forkmesh.client"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${FORKMESH_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${FORKMESH_VERSION}"
        OUTPUT_NAME "ForkMesh"
    )
endif()

if(APPLE)
    set(FORKMESH_RESOURCE_INSTALL_ROOT
        "ForkMesh.app/Contents/Resources/forkmesh")
    install(TARGETS forkmesh BUNDLE DESTINATION ".")
    install(TARGETS forkmesh-term RUNTIME DESTINATION "ForkMesh.app/Contents/MacOS")
else()
    set(FORKMESH_RESOURCE_INSTALL_ROOT
        "${CMAKE_INSTALL_DATADIR}/forkmesh")
    install(TARGETS forkmesh RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    if(UNIX)
        install(TARGETS forkmesh-term RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
endif()
# The installed Cloudflare bootstrap bundle does not include this CMake file,
# but it must stamp the same exact product version as the desktop release that
# launched it.  Generate a tiny public metadata file instead of passing release
# identity (or any secret) through the child process.
set(FORKMESH_VERSION_METADATA
    "${CMAKE_CURRENT_BINARY_DIR}/forkmesh-version.txt")
file(GENERATE OUTPUT "${FORKMESH_VERSION_METADATA}"
    CONTENT "${FORKMESH_VERSION}\n")
install(FILES "${FORKMESH_VERSION_METADATA}"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../marketplace"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}")
install(PROGRAMS
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/cloudflare_bootstrap.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/cloudflare_tunnel_bootstrap.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/cloudflared_install.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/direct_https_bootstrap.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/headless_mirror_refresh.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/hosted_repository_sync.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/mirror_gateway.py"
    # The Vultr mirror installer uploads the operator policy hook and the
    # secret scanner it loads, so an installed client ships both.
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/node_operator_policy.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/secret_gate.py"
    # Settings -> MCP points external agents at this script, so an installed
    # client has to ship it too, not just a source checkout.
    "${CMAKE_CURRENT_SOURCE_DIR}/../tools/forkmesh_mcp_server.py"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}/tools")
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/../server/packaging/systemd/forkmesh-mirror-node.service"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}/server/packaging/systemd")
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/wrangler.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/pyproject.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/uv.lock"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/tailwind.dashboard.config.js"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/tailwind.input.css"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}/app")
install(PROGRAMS
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/pywrangler.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/migrate.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/deploy.sh"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}/app")
install(DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/public"
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/migrations"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}/app"
    PATTERN "__pycache__" EXCLUDE
    PATTERN ".pytest_cache" EXCLUDE
    PATTERN ".wrangler" EXCLUDE
    PATTERN "node_modules" EXCLUDE
    PATTERN "dist" EXCLUDE)
install(DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/../app/tools/"
    DESTINATION "${FORKMESH_RESOURCE_INSTALL_ROOT}/app/tools"
    FILES_MATCHING PATTERN "*.py"
    PATTERN "__pycache__" EXCLUDE)
foreach(FORKMESH_STATIC_WORKER IN ITEMS www world)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../${FORKMESH_STATIC_WORKER}/wrangler.toml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../${FORKMESH_STATIC_WORKER}/worker.js"
        DESTINATION
            "${FORKMESH_RESOURCE_INSTALL_ROOT}/${FORKMESH_STATIC_WORKER}")
    install(DIRECTORY
        "${CMAKE_CURRENT_SOURCE_DIR}/../${FORKMESH_STATIC_WORKER}/public"
        DESTINATION
            "${FORKMESH_RESOURCE_INSTALL_ROOT}/${FORKMESH_STATIC_WORKER}")
endforeach()

option(FORKMESH_BUILD_TESTS "Build the offscreen desktop smoke tests" ON)
if(FORKMESH_BUILD_TESTS)
    qt_add_executable(forkmesh-cove-files-tests
        tests/test_cove_files.cpp
        src/CoveFileSupport.h
        src/CoveFileSupport.cpp
        src/CoveFileEditor.h
        src/CoveFileEditor.cpp
    )
    target_link_libraries(forkmesh-cove-files-tests PRIVATE Qt6::Widgets)
    if(APPLE)
        set_target_properties(forkmesh-cove-files-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-project-invite-tests
        tests/test_project_invite.cpp
        src/ProjectInvite.h
        src/ProjectInvite.cpp
    )
    target_link_libraries(forkmesh-project-invite-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-project-invite-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Stats page grid: packing, edits, persistence and painting, offscreen.
    qt_add_executable(forkmesh-stats-dashboard-tests
        tests/test_stats_dashboard.cpp
        src/StatsDashboard.h
        src/StatsDashboard.cpp
    )
    target_link_libraries(forkmesh-stats-dashboard-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-stats-dashboard-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-stats-dashboard-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-ui-gallery
        tests/ui_gallery.cpp
        src/Theme.h
        src/DesignTokens.h
        src/DesignTokens.cpp
        src/UiKit.h
        src/UiKit.cpp
        resources/icons.qrc
        ${FORKMESH_THEME_QRC}
    )
    target_link_libraries(forkmesh-ui-gallery PRIVATE Qt6::Widgets Qt6::Svg)
    target_include_directories(forkmesh-ui-gallery PRIVATE src)
    set_target_properties(forkmesh-ui-gallery PROPERTIES EXCLUDE_FROM_ALL TRUE)
    if(APPLE)
        set_target_properties(forkmesh-ui-gallery PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-fleet-visualizer-tests
        tests/test_fleet_visualizer.cpp
        src/FleetVisualizer.h
        src/FleetVisualizer.cpp
        src/QueryInspector.h
        src/QueryInspector.cpp
        src/DesktopProcesses.h
        src/DesktopProcesses.cpp
        src/DesktopProcessView.h
        src/DesktopProcessView.cpp
        src/CoreNodeLogs.h
        src/CoreNodeLogs.cpp
        src/IpCountry.h
        src/IpCountry.cpp
        src/IpWhois.h
        src/IpWhois.cpp
    )
    target_link_libraries(forkmesh-fleet-visualizer-tests PRIVATE Qt6::Widgets
        Qt6::Network)
    target_include_directories(forkmesh-fleet-visualizer-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-fleet-visualizer-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Settings as one scrolling page: jump from the list, follow the section
    # being read, search that narrows the page and marks matches, and the
    # borrowed profile panel growing instead of nesting a scroll bar.
    qt_add_executable(forkmesh-settings-sections-tests
        tests/test_settings_sections.cpp
        src/SettingsSectionsView.h
        src/SettingsSectionsView.cpp
    )
    target_link_libraries(forkmesh-settings-sections-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-settings-sections-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-settings-sections-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Ping settings (#347): the kind catalog, quiet-hours arithmetic, the
    # relay sync snapshot, and the Settings > Pings panel built from them.
    qt_add_executable(forkmesh-ping-settings-tests
        tests/test_ping_settings.cpp
        src/PingSettings.h
        src/PingSettingsPanel.h
        src/PingSettingsPanel.cpp
    )
    target_link_libraries(forkmesh-ping-settings-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-ping-settings-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-ping-settings-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-tests
        tests/test_crypto.cpp
        resources/icons.qrc
        src/PlatformLogFilter.h
        src/PlatformLogFilter.cpp
        src/BackgroundActivity.h
        src/ActionTelemetry.h
        src/ActionTelemetry.cpp
        src/ClientVersionGate.h
        src/BackoffNetworkAccessManager.h
        src/BackoffNetworkAccessManager.cpp
        src/ActionFailureSummary.h
        src/ActionFile.h
        src/ActionFile.cpp
        src/ActionPushRange.h
        src/ActionPushRange.cpp
        src/AccountCapability.h
        src/ForkMeshIdentity.h
        src/ForkMeshIdentity.cpp
        src/StrictGitReader.h
        src/StrictGitReader.cpp
        src/RoomCrypto.h
        src/RoomCrypto.cpp
        src/RoomSeal.h
        src/RoomSeal.cpp
        src/CoveCrypto.h
        src/CoveCrypto.cpp
        src/CoveStore.h
        src/CoveStore.cpp
        src/MirrorCrypto.h
        src/MirrorCrypto.cpp
        src/PrivateMirrorStore.h
        src/PrivateMirrorStore.cpp
        src/IssueStore.h
        src/IssueStore.cpp
        src/IssueBurnup.h
        src/IssueBurnup.cpp
        src/ProjectStore.h
        src/ProjectStore.cpp
        src/DiscussionStore.h
        src/DiscussionStore.cpp
        src/PullStore.h
        src/PullStore.cpp
        src/GitIndexQueue.h
        src/GitIndexQueue.cpp
        src/PullReviewModel.h
        src/PullReviewModel.cpp
        src/PullAiReview.h
        src/PullAiReview.cpp
        src/CodeSymbols.h
        src/MergeQueue.h
        src/MergeQueue.cpp
        src/GlobalSearchMatch.h
        src/GlobalSearchMatch.cpp
        src/AgentStore.h
        src/AgentStore.cpp
        # AgentStore resumes sessions against the runtime contract.
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
        src/AgentWorktree.h
        src/UsageLimitCalendar.h
        src/UsageLimitCalendar.cpp
        src/ActionStore.h
        src/ActionStore.cpp
        src/RepoSecurity.h
        src/RepoSecurity.cpp
        src/LocalBackupStore.h
        src/LocalBackupStore.cpp
        src/McpConnector.h
        src/McpConnector.cpp
        src/DirectorySizeScan.h
        src/DirectorySizeScan.cpp
        src/AppPaths.h
        src/AppPaths.cpp
        src/MarkdownRender.h
        src/ReferenceLinks.h
        src/ChatHistoryLimits.h
        src/ChatMessageRetention.h
        src/ChatBlocks.h
        src/ChatFormat.h
        src/ChatLinkText.h
        src/AppViewMode.h
        src/ChannelGroups.h
        src/RailFolders.h
        src/RailGroups.h
        src/RailShortcuts.h
        src/ChatVisitorPresence.h
        src/NoteListEntry.h
        src/SystemStats.h
        src/SystemStats.cpp
        src/LogSource.h
        src/LogSource.cpp
        src/NodeDiagnostics.h
        src/NodeDiagnostics.cpp
        src/ClientErrorReports.h
        src/ClientErrorReports.cpp
        src/GitRefusal.h
        src/GitRefusal.cpp
        src/PlannedRestarts.h
        src/PlannedRestarts.cpp
        src/PingSyncState.h
        src/PingSyncState.cpp
    )
    # Qt6::Concurrent: DirectorySizeScan scans top-level trees in parallel.
    target_link_libraries(forkmesh-tests PRIVATE Qt6::Core Qt6::Network Qt6::Concurrent OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Focused local control-node contracts: Cloudflare tokens remain out of
    # argv/output, public fields are validated, wallet input is public-address
    # only, and the manifest signer verifies the exact payload it signs.
    qt_add_executable(forkmesh-control-tests
        tests/test_control_node.cpp
        src/CatalogAdminDelete.h
        src/ControlNode.h
        src/ControlNode.cpp
        src/MirrorSyncVerdict.h
        src/NodeRole.h
        src/FleetAutoscaleModel.h
        src/RelayRepositoryScope.h
        src/SyncStatusModel.h
        src/D1DatabaseAdmin.h
        src/D1DatabaseAdmin.cpp
        src/D1QuerySource.h
        src/D1QuerySource.cpp
        src/MirrorActionsConfiguration.h
        src/MirrorActionsConfiguration.cpp
        src/AppPaths.h
        src/AppPaths.cpp
        src/MirrorActionsSummary.h
        src/MirrorActionsSummary.cpp
        src/ActionStore.h
        src/ActionStore.cpp
    )
    target_include_directories(forkmesh-control-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # ActionStore deletes run trees on a worker, so every target that compiles
    # it needs Concurrent, not just the ones that exercise that path.
    target_link_libraries(forkmesh-control-tests PRIVATE Qt6::Core Qt6::Concurrent)
    target_compile_definitions(forkmesh-control-tests PRIVATE
        FORKMESH_SYNC_STATUS_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../conformance/sync-status/v1/fixture.json")
    if(APPLE)
        set_target_properties(forkmesh-control-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Scoped trust lists (forkmesh-trust-list-v1): the desktop resolver must
    # reproduce conformance/trust-lists/v1/vectors.json exactly -- rejection
    # codes, ordering, and telemetry -- whatever order its inputs arrive in.
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    qt_add_executable(forkmesh-trust-list-tests
        tests/test_trust_lists.cpp
        src/TrustLists.h
        src/TrustLists.cpp
    )
    target_include_directories(forkmesh-trust-list-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-trust-list-tests PRIVATE
        Qt6::Core Qt6::Test OpenSSL::Crypto)
    target_compile_definitions(forkmesh-trust-list-tests PRIVATE
        FORKMESH_TRUST_LIST_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../conformance/trust-lists/v1/vectors.json")
    if(APPLE)
        set_target_properties(forkmesh-trust-list-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Desktop/public-World chat convergence: saved settings migrate only the
    # retired shared-room route, blackholed upgrades have a deadline, and every
    # failure/network transition yields one bounded reconnect.
    qt_add_executable(forkmesh-chat-transport-tests
        tests/test_chat_transport.cpp
        src/MainnodeRoom.h
        src/ChatBackend.h
        # NodeDiagnostics classifies log lines through forkmesh::splitLogSource,
        # so this target does not link without it.
        src/LogSource.h
        src/LogSource.cpp
        src/RoomCrypto.h
        src/RoomCrypto.cpp
        src/RoomSeal.h
        src/RoomSeal.cpp
        src/SystemStats.h
        src/SystemStats.cpp
        src/LogSource.h
        src/LogSource.cpp
        src/NodeDiagnostics.h
        src/NodeDiagnostics.cpp
        src/UserStatus.h
        src/UserStatus.cpp
        src/ServerNode.h
        src/ServerNode.cpp
        src/OfficeChannelMirror.h
        src/OfficeChannelMirror.cpp
        src/BleMeshLink.h
        src/BleMeshLink.cpp
        src/LocalMeshNode.h
        src/LocalMeshNode.cpp
        src/MeshFirewall.h
        src/MeshFirewall.cpp
        src/ReactionCatalog.h
        ${FORKMESH_PACKAGE_DISCORD_SOURCES}
    )
    target_include_directories(forkmesh-chat-transport-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${FORKMESH_MARKETPLACE_INCLUDE_DIRS})
    target_compile_definitions(forkmesh-chat-transport-tests PRIVATE
        FORKMESH_SERVER_NODE_TESTS FORKMESH_DISCORD_TESTS
        FORKMESH_LOCAL_MESH_TESTS)
    target_link_libraries(forkmesh-chat-transport-tests PRIVATE
        Qt6::Core Qt6::Network OpenSSL::Crypto)
    forkmesh_link_bluetooth(forkmesh-chat-transport-tests)
    if(APPLE)
        set_target_properties(forkmesh-chat-transport-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # ForkMesh Actions trust boundary: repository-state-bound approvals plus
    # real Linux namespace/resource isolation. The integration cases execute
    # hostile workflows and prove host secrets, source writes, egress, runaway
    # files/output/processes, and deadlines fail closed.
    qt_add_executable(forkmesh-action-security-tests
        tests/test_action_security.cpp
        src/CrashHandler.h
        src/CrashHandler.cpp
        src/ActionFile.h
        src/ActionFile.cpp
        src/ActionStore.h
        src/ActionStore.cpp
        src/ActionRunner.h
        src/ActionRunner.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/SystemStats.h
        src/SystemStats.cpp
    )
    target_include_directories(forkmesh-action-security-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(forkmesh-action-security-tests PRIVATE
        FORKMESH_ACTION_STORE_TESTS)
    target_link_libraries(forkmesh-action-security-tests PRIVATE
        Qt6::Core Qt6::Concurrent)
    if(APPLE)
        set_target_properties(forkmesh-action-security-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Private control-node runtime: device-bound encrypted identity vault,
    # opaque whole-repository replicas, per-sync key rotation, recipient
    # revocation, owner-only temporary materialization, and legacy plaintext
    # cleanup/WebSocket fail-closed behavior.
    qt_add_executable(forkmesh-private-mirror-tests
        tests/test_private_mirror_runtime.cpp
        src/CoveCrypto.h
        src/CoveCrypto.cpp
        src/MirrorCrypto.h
        src/MirrorCrypto.cpp
        src/PrivateMirrorStore.h
        src/PrivateMirrorStore.cpp
        src/PrivateMirrorRuntime.h
        src/PrivateMirrorRuntime.cpp
    )
    target_include_directories(forkmesh-private-mirror-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-private-mirror-tests PRIVATE
        Qt6::Core Qt6::Network OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-private-mirror-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Public mirrors are durable only as official-age encrypted tar archives.
    # The focused runtime suite uses deterministic stand-in age executables to
    # exercise vault binding, atomic sealing, safe temporary materialization,
    # gateway request validation, tamper rejection, and legacy cleanup without
    # requiring a system age installation in CI.
    qt_add_executable(forkmesh-public-mirror-tests
        tests/test_public_mirror_runtime.cpp
        src/CoveCrypto.h
        src/CoveCrypto.cpp
        src/PublicMirrorRuntime.h
        src/PublicMirrorRuntime.cpp
    )
    target_include_directories(forkmesh-public-mirror-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-public-mirror-tests PRIVATE
        Qt6::Core OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-public-mirror-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Headless fleet nodes seal their public mirror from the installer's agent
    # checkout; this suite proves that checkout keeps tracking the upstream
    # relay clone (fast-forwards, forced pulls-metadata tracking, safe pruning,
    # divergence preservation) against real local git repositories.
    qt_add_executable(forkmesh-upstream-sync-tests
        tests/test_upstream_checkout_sync.cpp
        src/UpstreamCheckoutSync.h
        src/UpstreamCheckoutSync.cpp
    )
    target_include_directories(forkmesh-upstream-sync-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-upstream-sync-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-upstream-sync-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Per-repository index-writing git queue, index.lock liveness and the
    # merge timeout (docs/desktop/git-locking.md), against real repositories.
    qt_add_executable(forkmesh-git-index-queue-tests
        tests/test_git_index_queue.cpp
        src/GitIndexQueue.h
        src/GitIndexQueue.cpp
    )
    target_include_directories(forkmesh-git-index-queue-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-git-index-queue-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-git-index-queue-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Ad-hoc agent runs take their Agents-list title and `<id>-<slug>`
    # branch/worktree name from the first prompt (AgentWorktree.h).
    qt_add_executable(forkmesh-agent-prompt-title-tests
        tests/test_agent_prompt_title.cpp
        src/AgentWorktree.h
    )
    target_include_directories(forkmesh-agent-prompt-title-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-prompt-title-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-prompt-title-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-diff-intra-line-tests
        tests/test_diff_intra_line.cpp
        src/DiffIntraLine.h
        src/DiffIntraLine.cpp
    )
    target_include_directories(forkmesh-diff-intra-line-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-diff-intra-line-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-diff-intra-line-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # The Component health page reads this codebase to report where the UI
    # thread is at risk. Its line numbers are only trustworthy if comment and
    # literal blanking preserves every line, so that is asserted directly.
    qt_add_executable(forkmesh-code-health-tests
        tests/test_code_health.cpp
        src/CodeHealth.h
        src/CodeHealth.cpp
    )
    target_include_directories(forkmesh-code-health-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-code-health-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-code-health-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-binary-size-tests
        tests/test_binary_size.cpp
        src/BinarySize.h
        src/BinarySize.cpp
    )
    target_include_directories(forkmesh-binary-size-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-binary-size-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-binary-size-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # The health board is a plain QWidget with std::function callbacks, so it
    # can be driven directly: layout by column count, filtering, and hit
    # testing that must follow the filtered order rather than the original one.
    qt_add_executable(forkmesh-component-health-board-tests
        tests/test_component_health_board.cpp
        src/ComponentHealthBoard.h
        src/ComponentHealthBoard.cpp
    )
    target_include_directories(forkmesh-component-health-board-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-component-health-board-tests PRIVATE
        Qt6::Widgets)
    if(APPLE)
        set_target_properties(forkmesh-component-health-board-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-home-board-tests
        tests/test_home_board.cpp
        src/HomeBoard.h
        src/HomeBoard.cpp
        src/HomeLayout.h
        src/HomeLayout.cpp
    )
    target_include_directories(forkmesh-home-board-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-home-board-tests PRIVATE Qt6::Widgets)
    if(APPLE)
        set_target_properties(forkmesh-home-board-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # The code map's model is pure Qt Core: tree from `git ls-tree`, squarified
    # layout, 3D elevations, the exact back-to-front order the software 3D view
    # paints in, and the per-line tokenizer.
    qt_add_executable(forkmesh-code-map-model-tests
        tests/test_code_map_model.cpp
        src/CodeMapModel.h
        src/CodeMapModel.cpp
    )
    target_include_directories(forkmesh-code-map-model-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-code-map-model-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-code-map-model-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # The view is a plain QWidget with std::function callbacks, so it is driven
    # directly: painting in both modes, click-to-zoom, content requests and the
    # 3D hit test.
    qt_add_executable(forkmesh-code-map-view-tests
        tests/test_code_map_view.cpp
        src/CodeMapModel.h
        src/CodeMapModel.cpp
        src/CodeMapView.h
        src/CodeMapView.cpp
    )
    target_include_directories(forkmesh-code-map-view-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-code-map-view-tests PRIVATE Qt6::Widgets)
    if(APPLE)
        set_target_properties(forkmesh-code-map-view-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-window-fade-tests
        tests/test_window_fade.cpp
        src/WindowFade.h
    )
    target_include_directories(forkmesh-window-fade-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-window-fade-tests PRIVATE Qt6::Widgets)
    if(APPLE)
        set_target_properties(forkmesh-window-fade-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Branch management requested on the website (adhoc #2098) is applied by
    # the node that owns the checkout. This suite drives those operations
    # against real local git repositories: exact-tip deletes, fast-forward and
    # true merges of the default branch into a stale one, creation, and every
    # refusal (default branch, checked-out branch, moved tip, conflict).
    qt_add_executable(forkmesh-pull-store-retire-tests
        tests/test_pull_store_retire.cpp
        src/PullStore.h
        src/PullStore.cpp
        src/AppPaths.h
        src/AppPaths.cpp
        src/ForkMeshIdentity.h
        src/ForkMeshIdentity.cpp
        src/GitIndexQueue.h
        src/GitIndexQueue.cpp
        src/GitRefusal.h
        src/GitRefusal.cpp
        src/StrictGitReader.h
        src/StrictGitReader.cpp
        src/CoveCrypto.h
        src/CoveCrypto.cpp
    )
    target_include_directories(forkmesh-pull-store-retire-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-pull-store-retire-tests PRIVATE
        Qt6::Core Qt6::Network OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-pull-store-retire-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-web-branch-ops-tests
        tests/test_web_branch_ops.cpp
        src/WebBranchOps.h
        src/WebBranchOps.cpp
    )
    target_include_directories(forkmesh-web-branch-ops-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-web-branch-ops-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-web-branch-ops-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-add-core-node-tests
        tests/test_add_core_node.cpp
        src/AddCoreNodeModel.h
        src/AddCoreNodeModel.cpp
        src/AddCoreNodeDialog.h
        src/AddCoreNodeDialog.cpp
        src/FleetAutoscaleModel.h
    )
    target_include_directories(forkmesh-add-core-node-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-add-core-node-tests PRIVATE Qt6::Widgets)
    if(APPLE)
        set_target_properties(forkmesh-add-core-node-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-aws-credentials-tests
        tests/test_aws_credentials.cpp
        src/AwsCredentials.h
        src/AwsCredentials.cpp
        src/AwsSettingsPanel.h
        src/AwsSettingsPanel.cpp
    )
    target_include_directories(forkmesh-aws-credentials-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-aws-credentials-tests PRIVATE
        Qt6::Widgets Qt6::Network OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-aws-credentials-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-systemd-timers-tests
        tests/test_systemd_timers.cpp
        src/SystemdTimers.h
        src/SystemdTimers.cpp
        src/SystemdTimersPanel.h
        src/SystemdTimersPanel.cpp
    )
    target_include_directories(forkmesh-systemd-timers-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-systemd-timers-tests PRIVATE
        Qt6::Widgets Qt6::Concurrent)
    if(APPLE)
        set_target_properties(forkmesh-systemd-timers-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-remove-core-node-tests
        tests/test_remove_core_node.cpp
        src/AddCoreNodeModel.h
        src/AddCoreNodeModel.cpp
        src/RemoveCoreNodeModel.h
        src/RemoveCoreNodeModel.cpp
        src/RemoveCoreNodeDialog.h
        src/RemoveCoreNodeDialog.cpp
        src/FleetAutoscaleModel.h
    )
    target_include_directories(forkmesh-remove-core-node-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-remove-core-node-tests PRIVATE Qt6::Widgets)
    if(APPLE)
        set_target_properties(forkmesh-remove-core-node-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-task-work-items-tests
        tests/test_task_work_items.cpp
        src/TaskWorkItems.h
        src/TaskWorkItems.cpp
    )
    target_include_directories(forkmesh-task-work-items-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-task-work-items-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-task-work-items-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-task-markdown-tests
        tests/test_task_markdown.cpp
        src/TaskMarkdown.h
        src/TaskMarkdown.cpp
    )
    target_include_directories(forkmesh-task-markdown-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-task-markdown-tests PRIVATE Qt6::Gui)
    if(APPLE)
        set_target_properties(forkmesh-task-markdown-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-list-view-mode-tests
        tests/test_list_view_mode.cpp
        src/ListViewMode.h
        src/ListViewMode.cpp
        src/SettingsWrite.h
    )
    target_include_directories(forkmesh-list-view-mode-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-list-view-mode-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-list-view-mode-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-system-open-folder-tests
        tests/test_system_open_folder.cpp
        src/SystemOpenFolder.h
        src/SystemOpenFolder.cpp
    )
    target_include_directories(forkmesh-system-open-folder-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-system-open-folder-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-system-open-folder-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-agent-ticker-tests
        tests/test_agent_ticker.cpp
        src/AgentTicker.h
        src/AgentTicker.cpp
    )
    target_include_directories(forkmesh-agent-ticker-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-ticker-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-ticker-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-settings-write-tests
        tests/test_settings_write.cpp
        src/SettingsWrite.h
    )
    target_include_directories(forkmesh-settings-write-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-settings-write-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-settings-write-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-note-request-proof-tests
        tests/test_note_request_proof.cpp
        src/NoteRequestProof.h
    )
    target_include_directories(forkmesh-note-request-proof-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(forkmesh-note-request-proof-tests PRIVATE
        FORKMESH_APP_ENTRY="${CMAKE_CURRENT_SOURCE_DIR}/../app/src/entry.py")
    target_link_libraries(forkmesh-note-request-proof-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-note-request-proof-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # The unattended SSH mirror fan-out pushes every stable ref at once without
    # forcing, so a branch the gateway advanced first is rejected while the
    # rest land and git still exits non-zero. This suite proves that partial
    # rejection reads as a healthy push (and that an unreachable gateway or a
    # hook denial still reads as a failure), against real local git repos.
    qt_add_executable(forkmesh-ssh-mirror-push-tests
        tests/test_ssh_mirror_push.cpp
        src/MirrorGatewayHealth.h
        src/MirrorPushOutcome.h
        src/NetworkBackoff.h
    )
    target_include_directories(forkmesh-ssh-mirror-push-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-ssh-mirror-push-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-ssh-mirror-push-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # The first-instance owner's community reward-pool key remains inside a
    # passphrase-encrypted, owner-only desktop vault. These tests exercise key
    # import, strict transaction inspection, local signing, canonical owner
    # authorization, RPC validation, and redaction without a Worker or network.
    qt_add_executable(forkmesh-reward-pool-tests EXCLUDE_FROM_ALL
        tests/test_reward_pool_signer.cpp
        src/RewardPoolSigner.h
        src/RewardPoolSigner.cpp
    )
    target_include_directories(forkmesh-reward-pool-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-reward-pool-tests PRIVATE
        Qt6::Core OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-reward-pool-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Member-to-member tips. The user's own Solana key stays in a
    # passphrase-encrypted, owner-only desktop vault; these tests pin the vault
    # round trip, SOL amount parsing, the exact single-transfer transaction
    # layout, and the guards that refuse to sign anything else.
    qt_add_executable(forkmesh-tip-tests EXCLUDE_FROM_ALL
        tests/test_tip_signer.cpp
        src/TipSigner.h
        src/TipSigner.cpp
        src/RewardPoolSigner.h
        src/RewardPoolSigner.cpp
    )
    target_include_directories(forkmesh-tip-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-tip-tests PRIVATE
        Qt6::Core OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-tip-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # A focused suite for the Claude Code IDE bridge: drives its WebSocket /
    # JSON-RPC server the way the CLI does (no Widgets needed).
    qt_add_executable(forkmesh-ide-tests
        tests/test_ide_bridge.cpp
        src/AgentGuardScript.h
        src/AgentGuardrails.h
        src/AgentGuardrails.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/ClaudeIdeBridge.h
        src/ClaudeIdeBridge.cpp
        src/VirtualMachineRuntime.h
        src/VirtualMachineRuntime.cpp
    )
    target_link_libraries(forkmesh-ide-tests PRIVATE Qt6::Core Qt6::Network)
    if(APPLE)
        set_target_properties(forkmesh-ide-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Origin-bound, loopback-only browser -> local speech-to-text control. The
    # focused suite drives its real HTTP/CORS capability protocol and confirms
    # that there is no audio transport endpoint.
    qt_add_executable(forkmesh-world-speech-tests
        tests/test_world_speech_bridge.cpp
        src/WorldSpeechBridge.h
        src/WorldSpeechBridge.cpp
    )
    target_include_directories(forkmesh-world-speech-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-world-speech-tests PRIVATE
        Qt6::Core Qt6::Network)
    if(APPLE)
        set_target_properties(forkmesh-world-speech-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Provider-neutral runtime contract, registry, and compatibility tests.
    qt_add_executable(forkmesh-agent-runtime-contract-tests
        tests/test_agent_runtime_contract.cpp
        src/AgentCliLocator.h
        src/AgentCapabilityBroker.h
        src/AgentCapabilityBroker.cpp
        src/AgentCapabilityStore.h
        src/AgentCapabilityStore.cpp
        src/AgentPermissionController.h
        src/AgentPermissionController.cpp
        src/AgentGuardScript.h
        src/AgentGuardrails.h
        src/AgentGuardrails.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/AgentJsonlFramer.h
        src/AgentJsonlFramer.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
        src/AgentRuntimeRegistry.h
        src/AgentRuntimeRegistry.cpp
        src/ClaudeStreamSession.h
        src/ClaudeStreamSession.cpp
        src/AgentToolCatalog.h
        src/AgentToolCatalog.cpp
        src/AgentToolPolicy.h
        src/AgentToolPolicy.cpp
        src/CodexAppServerSession.h
        src/CodexAppServerSession.cpp
        src/VirtualMachineRuntime.h
        src/VirtualMachineRuntime.cpp
    )
    target_link_libraries(forkmesh-agent-runtime-contract-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-runtime-contract-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Tool catalog coverage and the owner-device tool efficiency ledger.
    qt_add_executable(forkmesh-agent-tool-efficiency-tests
        tests/test_agent_tool_efficiency.cpp
        src/AgentTokenPricing.h
        src/AgentToolCatalog.h
        src/AgentToolCatalog.cpp
        src/AgentToolPolicy.h
        src/AgentToolPolicy.cpp
        src/AgentToolEfficiencyLedger.h
        src/AgentToolEfficiencyLedger.cpp
        src/BackgroundFileWriter.h
        src/BackgroundFileWriter.cpp
        src/AgentToolImprovementLog.h
        src/AgentToolImprovementLog.cpp
        src/AgentRunEfficiencyLog.h
        src/AgentRunEfficiencyLog.cpp
    )
    target_link_libraries(forkmesh-agent-tool-efficiency-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-tool-efficiency-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-agent-model-stats-tests
        tests/test_agent_model_stats.cpp
        src/AgentModelStats.h
        src/AgentModelStats.cpp
        src/AgentTokenPricing.h
        src/AgentStore.h
        src/AgentStore.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
    )
    target_include_directories(forkmesh-agent-model-stats-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(forkmesh-agent-model-stats-tests PRIVATE
        FORKMESH_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(forkmesh-agent-model-stats-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-model-stats-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Focused Codex app-server JSONL transport and normalization tests.
    qt_add_executable(forkmesh-codex-tests
        tests/test_codex_app_server.cpp
        src/AgentGuardScript.h
        src/AgentGuardrails.h
        src/AgentGuardrails.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/AgentResumeIdentity.h
        src/AgentJsonlFramer.h
        src/AgentJsonlFramer.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
        src/CodexAppServerSession.h
        src/CodexAppServerSession.cpp
        src/AgentToolCatalog.h
        src/AgentToolCatalog.cpp
        src/AgentToolPolicy.h
        src/AgentToolPolicy.cpp
        src/CodexTranscriptStyle.h
        src/VirtualMachineRuntime.h
        src/VirtualMachineRuntime.cpp
    )
    target_link_libraries(forkmesh-codex-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-codex-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # The Claude Code stream transport's start/exit contract: a launch that never
    # starts still has to end, and a turn that cannot be written has to be
    # reported as undelivered (adhoc #1618).
    qt_add_executable(forkmesh-agent-transport-tests
        tests/test_agent_transport.cpp
        src/AgentGuardScript.h
        src/AgentGuardrails.h
        src/AgentGuardrails.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/AgentGuardrailLedger.h
        src/AgentGuardrailLedger.cpp
        src/BackgroundFileWriter.h
        src/BackgroundFileWriter.cpp
        src/AgentJsonlFramer.h
        src/AgentJsonlFramer.cpp
        src/AgentJail.h
        src/AgentStore.h
        src/AgentStore.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
        src/ClaudeStreamSession.h
        src/ClaudeStreamSession.cpp
        src/AgentToolCatalog.h
        src/AgentToolCatalog.cpp
        src/AgentToolPolicy.h
        src/AgentToolPolicy.cpp
        src/VirtualMachineRuntime.h
        src/VirtualMachineRuntime.cpp
    )
    target_compile_definitions(forkmesh-agent-transport-tests PRIVATE
        FORKMESH_AGENT_TRANSPORT_TESTS=1)
    target_link_libraries(forkmesh-agent-transport-tests PRIVATE Qt6::Core)

    # Deleting an agent has to stay recoverable for the retention window, and
    # the bin has to be swept once that window passes (issue #578).
    qt_add_executable(forkmesh-agent-recycle-tests
        tests/test_agent_recycle_bin.cpp
        src/AgentStore.h
        src/AgentStore.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
    )
    target_include_directories(forkmesh-agent-recycle-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-recycle-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-recycle-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Cloud agent sessions: the relay's agent-sessions payload as Agents list
    # rows, and the canonical the account key signs to read and steer them.
    qt_add_executable(forkmesh-agent-cloud-tests
        tests/test_agent_cloud_sessions.cpp
        src/AgentCloudSessions.h
        src/AgentCloudSessions.cpp
        src/AgentStore.h
        src/AgentStore.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
    )
    target_include_directories(forkmesh-agent-cloud-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-cloud-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-cloud-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-agent-cloud-stream-tests
        tests/test_agent_cloud_stream.cpp
        src/AgentCloudStream.h
        src/AgentCloudStream.cpp
    )
    target_include_directories(forkmesh-agent-cloud-stream-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-cloud-stream-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-cloud-stream-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-agent-workspace-name-tests
        tests/test_agent_workspace_name_gate.cpp
        src/AgentWorkspaceNameGate.h
    )
    target_include_directories(forkmesh-agent-workspace-name-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-workspace-name-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-agent-workspace-name-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-runner-login-tests
        tests/test_runner_login.cpp
        src/NodeDirectoryRows.h
        src/CoreNodeLogs.h
        src/RunnerLogin.h
        src/RunnerLogin.cpp
        src/RunnerListView.h
        src/RunnerListView.cpp
        src/TerminalWidget.h
        src/TerminalWidget.cpp
    )
    target_include_directories(forkmesh-runner-login-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-runner-login-tests PRIVATE
        Qt6::Widgets Qt6::Test)
    if(UNIX AND NOT APPLE)
        target_link_libraries(forkmesh-runner-login-tests PRIVATE util)
    endif()
    if(APPLE)
        set_target_properties(forkmesh-runner-login-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-runner-setup-handoff-tests
        tests/test_runner_setup_handoff.cpp
        src/RunnerSetupHandoff.h
        src/RunnerSetupHandoff.cpp
    )
    target_include_directories(forkmesh-runner-setup-handoff-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-runner-setup-handoff-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-runner-setup-handoff-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()
    if(APPLE)
        set_target_properties(forkmesh-agent-transport-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Local control channel: the phone on the same network drives this
    # desktop (handshake, pairing, requests, file transfer over loopback).
    qt_add_executable(forkmesh-local-control-tests
        tests/test_local_control.cpp
        src/LocalControl.h
        src/LocalControl.cpp
        src/LocalControlStream.h
        src/LocalControlStream.cpp
        src/LocalControlHosts.h
        src/LocalControlHosts.cpp
    )
    target_include_directories(forkmesh-local-control-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-tests PRIVATE
        Qt6::Core Qt6::Network OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-local-control-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-local-control-handoff-tests
        tests/test_local_control_handoff.cpp
        src/LocalControlHandoff.h
    )
    target_include_directories(forkmesh-local-control-handoff-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-handoff-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-handoff-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-agent-intake-tests
        tests/test_agent_intake.cpp
        src/AgentIntake.h
        src/AgentIntake.cpp
        src/ReleaseDraft.h
        src/ReleaseDraft.cpp
        src/LocalControlHandoff.h
        src/AgentStore.h
        src/AgentStore.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
    )
    target_include_directories(forkmesh-agent-intake-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-intake-tests PRIVATE Qt6::Core Qt6::Network)
    if(APPLE)
        set_target_properties(forkmesh-agent-intake-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-agent-windows-tests
        tests/test_agent_windows.cpp
        src/AgentWindows.h
        src/AgentWindows.cpp
        src/AgentIntake.h
        src/AgentIntake.cpp
        src/ReleaseDraft.h
        src/ReleaseDraft.cpp
    )
    target_include_directories(forkmesh-agent-windows-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-agent-windows-tests PRIVATE Qt6::Widgets Qt6::Network)
    forkmesh_link_webengine(forkmesh-agent-windows-tests)
    if(APPLE)
        set_target_properties(forkmesh-agent-windows-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-claude-transcript-tools-tests
        tests/test_claude_transcript_tools.cpp
        src/ClaudeTranscriptTools.h
        src/ClaudeTranscriptTools.cpp
    )
    target_include_directories(forkmesh-claude-transcript-tools-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-claude-transcript-tools-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-claude-transcript-tools-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-local-control-attachments-tests
        tests/test_local_control_attachments.cpp
        src/LocalControlAttachments.h
        src/LocalControlAttachments.cpp
    )
    target_include_directories(forkmesh-local-control-attachments-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-attachments-tests PRIVATE
        Qt6::Core Qt6::Gui)
    if(APPLE)
        set_target_properties(forkmesh-local-control-attachments-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-org-agent-job-attachments-tests
        tests/test_org_agent_job_attachments.cpp
        src/OrgAgentJobAttachments.h
        src/OrgAgentJobAttachments.cpp
    )
    target_include_directories(forkmesh-org-agent-job-attachments-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-org-agent-job-attachments-tests PRIVATE
        Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-org-agent-job-attachments-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-local-control-prompt-images-tests
        tests/test_local_control_prompt_images.cpp
        src/LocalControlPromptImages.h
        src/LocalControlPromptImages.cpp
        src/OrgAgentJobAttachments.h
        src/OrgAgentJobAttachments.cpp
    )
    target_include_directories(forkmesh-local-control-prompt-images-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-prompt-images-tests PRIVATE
        Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-prompt-images-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Boot-scoped Lima/QEMU command construction and host/guest boundary tests.
    qt_add_executable(forkmesh-vm-runtime-tests
        tests/test_virtual_machine_runtime.cpp
        src/VirtualMachineRuntime.h
        src/VirtualMachineRuntime.cpp
    )
    target_link_libraries(forkmesh-vm-runtime-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-vm-runtime-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # Nightly end-to-end mesh-loop test (issue #352): drives the real client
    # control/update channel, direct HTTP repository boundary, and signed-issue
    # code through publish -> browse -> clone -> issue -> agent PR -> merge.
    # A deterministic stub agent stands in for the model so the run never needs
    # an API quota or the network.
    qt_add_executable(forkmesh-e2e-stub-agent
        tests/e2e_stub_agent.cpp
    )
    target_link_libraries(forkmesh-e2e-stub-agent PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-e2e-stub-agent PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-e2e
        tests/test_mesh_e2e.cpp
        src/BackgroundActivity.h
        src/ActionTelemetry.h
        src/ActionTelemetry.cpp
        src/LogSource.h
        src/LogSource.cpp
        src/ClientVersionGate.h
        src/BackoffNetworkAccessManager.h
        src/BackoffNetworkAccessManager.cpp
        src/ActionFile.h
        src/ActionFile.cpp
        src/ForkMeshIdentity.h
        src/ForkMeshIdentity.cpp
        src/StrictGitReader.h
        src/StrictGitReader.cpp
        src/RoomCrypto.h
        src/RoomCrypto.cpp
        src/RoomSeal.h
        src/RoomSeal.cpp
        src/CoveCrypto.h
        src/CoveCrypto.cpp
        src/CoveStore.h
        src/CoveStore.cpp
        src/MirrorCrypto.h
        src/MirrorCrypto.cpp
        src/IssueStore.h
        src/IssueStore.cpp
        src/IssueBurnup.h
        src/IssueBurnup.cpp
        src/DiscussionStore.h
        src/DiscussionStore.cpp
        src/PullStore.h
        src/PullStore.cpp
        src/GitRefusal.h
        src/GitRefusal.cpp
        src/GitIndexQueue.h
        src/GitIndexQueue.cpp
        src/PullReviewModel.h
        src/PullReviewModel.cpp
        src/PullAiReview.h
        src/PullAiReview.cpp
        src/AgentStore.h
        src/AgentStore.cpp
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
        src/ActionStore.h
        src/ActionStore.cpp
        src/RepoSecurity.h
        src/RepoSecurity.cpp
        src/ReferenceLinks.h
    )
    target_link_libraries(forkmesh-e2e PRIVATE
        Qt6::Core Qt6::Network Qt6::Concurrent OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-e2e PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # A second suite that drives the real MainWindow (needs the full app sources).
    qt_add_executable(forkmesh-window-tests
        tests/test_window_resize.cpp
        tests/workspace_snapshots.cpp
        ${FORKMESH_APP_SOURCES}
    )
    target_link_libraries(forkmesh-window-tests PRIVATE forkmesh-resources forkmesh-browser-pane Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Concurrent OpenSSL::Crypto)
    target_include_directories(forkmesh-window-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${FORKMESH_MARKETPLACE_INCLUDE_DIRS})
    forkmesh_link_webp(forkmesh-window-tests)
    forkmesh_link_multimedia(forkmesh-window-tests)
    forkmesh_link_webengine(forkmesh-window-tests)
forkmesh_link_opengl(forkmesh-window-tests)
    forkmesh_link_http3(forkmesh-window-tests)
    forkmesh_link_power_source(forkmesh-window-tests)
    if(UNIX AND NOT APPLE)
        target_link_libraries(forkmesh-window-tests PRIVATE util Qt6::DBus)
        target_compile_definitions(forkmesh-window-tests PRIVATE FORKMESH_HAVE_PORTAL)
    endif()

    qt_add_executable(forkmesh-sonifier-tests
        tests/test_agent_sonifier.cpp
        src/AgentSonifier.h
        src/AgentSonifier.cpp
        src/UiSoundCues.h
        src/UiSoundCues.cpp
    )
    target_link_libraries(forkmesh-sonifier-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-sonifier-tests PRIVATE src)

    # The interface sounds' event filter: which cue each kind of control's
    # clicks and keys stand for, driven with synthetic events offscreen. It
    # plays nothing, so no audio device is involved.
    qt_add_executable(forkmesh-ui-sounds-tests
        tests/test_ui_sounds.cpp
        src/UiSounds.h
        src/UiSounds.cpp
        src/UiSoundCues.h
        src/UiSoundCues.cpp
        src/AgentSonifier.h
        src/AgentSonifier.cpp
    )
    target_link_libraries(forkmesh-ui-sounds-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-ui-sounds-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-ui-sounds-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-user-activity-tests
        tests/test_user_activity.cpp
        src/UserActivityLog.h
        src/UserActivityLog.cpp
        src/UserActivityChart.h
        src/UserActivityChart.cpp
        src/SourceCheckout.h
        src/AppPaths.h
        src/AppPaths.cpp
        src/ProjectRegistry.h
        src/ProjectRegistry.cpp
    )
    target_link_libraries(forkmesh-user-activity-tests PRIVATE
        Qt6::Widgets Qt6::Test)
    target_include_directories(forkmesh-user-activity-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-user-activity-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-announcer-tests
        tests/test_agent_announcer.cpp
        src/AgentAnnouncer.h
        src/AgentAnnouncer.cpp
        src/AgentSonifier.h
        src/AgentSonifier.cpp
    )
    target_link_libraries(forkmesh-announcer-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-announcer-tests PRIVATE src)

    qt_add_executable(forkmesh-agent-folders-tests
        tests/test_agent_folders.cpp
        src/AgentFolders.h
        src/AgentFolders.cpp
    )
    target_link_libraries(forkmesh-agent-folders-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-agent-folders-tests PRIVATE src)

    qt_add_executable(forkmesh-repo-stats-tests
        tests/test_repo_stats.cpp
        src/RepoStatsStore.h
        src/RepoStatsStore.cpp
    )
    target_link_libraries(forkmesh-repo-stats-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-repo-stats-tests PRIVATE src)

    qt_add_executable(forkmesh-suggested-apps-tests
        tests/test_suggested_apps.cpp
        src/SuggestedApps.h
        src/SuggestedApps.cpp
    )
    target_link_libraries(forkmesh-suggested-apps-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-suggested-apps-tests PRIVATE src)

    qt_add_executable(forkmesh-package-tests
        tests/test_package_registry.cpp
        ${FORKMESH_MARKETPLACE_GENERATED_DIR}/marketplace.qrc
        src/ForkMeshPackageRegistry.h
        src/ForkMeshPackageRegistry.cpp
    )
    target_link_libraries(forkmesh-package-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-package-tests PRIVATE src)

    qt_add_executable(forkmesh-native-package-tests
        tests/test_native_package_extension.cpp
        src/NativePackageExtension.h
        src/NativePackageExtension.cpp
        src/PackageRailTile.h
        src/PackageRailTile.cpp
    )
    target_link_libraries(forkmesh-native-package-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-native-package-tests PRIVATE src)

    qt_add_executable(forkmesh-teammate-tests
        tests/test_teammate_store.cpp
        src/TeammateStore.h
        src/TeammateStore.cpp
    )
    target_link_libraries(forkmesh-teammate-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-teammate-tests PRIVATE src)

    qt_add_executable(forkmesh-teammate-workspace-tests
        tests/test_teammate_workspace.cpp
        src/TeammateWorkspace.h
        src/TeammateWorkspace.cpp
        src/TeammateStore.h
        src/TeammateStore.cpp
    )
    target_link_libraries(forkmesh-teammate-workspace-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-teammate-workspace-tests PRIVATE src)

    qt_add_executable(forkmesh-teammate-mood-tests
        tests/test_teammate_mood.cpp
        src/TeammateMood.h
        src/TeammateMood.cpp
        src/TeammateStore.h
        src/TeammateStore.cpp
    )
    target_link_libraries(forkmesh-teammate-mood-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-teammate-mood-tests PRIVATE src)

    qt_add_executable(forkmesh-user-avatar-tests
        tests/test_user_avatar_store.cpp
        src/UserAvatarStore.h
        src/UserAvatarStore.cpp
    )
    target_link_libraries(forkmesh-user-avatar-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-user-avatar-tests PRIVATE src)

    # Away and custom status: what a userStatus payload may carry (one emoji,
    # one short line, a sane clear time), when it lapses, and what the Set a
    # status dialog's "Clear after" choices resolve to. Core-only.
    qt_add_executable(forkmesh-user-status-tests
        tests/test_user_status.cpp
        src/UserStatus.h
        src/UserStatus.cpp
    )
    target_link_libraries(forkmesh-user-status-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-user-status-tests PRIVATE src)

    # The footer prompt's unsent text survives a restart or crash.
    qt_add_executable(forkmesh-prompt-draft-tests
        tests/test_prompt_draft_store.cpp
        src/PromptDraftStore.h
        src/PromptDraftStore.cpp
    )
    target_link_libraries(forkmesh-prompt-draft-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-prompt-draft-tests PRIVATE src)

    # Which relay a repository record belongs to: a stamped record never
    # follows the active relay, and a legacy one follows the repository list
    # the relays themselves report. Core-only.
    qt_add_executable(forkmesh-relay-repository-scope-tests
        tests/test_relay_repository_scope.cpp
        src/RelayRepositoryScope.h
    )
    target_link_libraries(forkmesh-relay-repository-scope-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-relay-repository-scope-tests PRIVATE src)

    # Saved searches decide which search results count as new occurrences, and
    # so which ones file a ping. Core-only, like the other stores.
    qt_add_executable(forkmesh-saved-search-tests
        tests/test_saved_search_store.cpp
        src/SavedSearchStore.h
        src/SavedSearchStore.cpp
    )
    target_link_libraries(forkmesh-saved-search-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-saved-search-tests PRIVATE src)

    qt_add_executable(forkmesh-security-alarm-tests
        tests/test_security_alarm_state.cpp
        src/SavedSearchStore.h
        src/SavedSearchStore.cpp
        src/SecurityAlarmState.h
        src/SecurityAlarmState.cpp
    )
    target_link_libraries(forkmesh-security-alarm-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-security-alarm-tests PRIVATE src)

    # What the desktop will show from a ForkMesh Ads serve response: https
    # destinations only, the published text limits, an exact id, and only
    # ForkMesh's own relay ever asked. Core-only, so the rules that keep a
    # hostile response off the screen stay covered without the widget stack.
    qt_add_executable(forkmesh-ad-banner-model-tests
        tests/test_ad_banner_model.cpp
        src/AdBannerModel.h
        src/AdBannerModel.cpp
    )
    target_link_libraries(forkmesh-ad-banner-model-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-ad-banner-model-tests PRIVATE src)

    qt_add_executable(forkmesh-genie-vitals-tests
        tests/test_genie_vitals.cpp
        src/GenieVitals.h
        src/GenieVitals.cpp
    )
    target_link_libraries(forkmesh-genie-vitals-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-genie-vitals-tests PRIVATE src)

    qt_add_executable(forkmesh-genie-landmarks-tests
        tests/test_genie_landmarks.cpp
        src/GenieLandmarks.h
        src/GenieLandmarks.cpp
    )
    target_link_libraries(forkmesh-genie-landmarks-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-genie-landmarks-tests PRIVATE src)

    # "@opus why does this hang?" in an issue thread has to resolve to the right
    # model and be told apart from "@fable fix this", which starts an agent
    # instead of writing a reply. Core-only, so that call stays covered where
    # the widget stack is not built.
    qt_add_executable(forkmesh-issue-mention-tests
        tests/test_issue_model_mention.cpp
        src/IssueModelMention.h
        src/IssueModelMention.cpp
    )
    target_link_libraries(forkmesh-issue-mention-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-issue-mention-tests PRIVATE src)

    # The Ledger package's record model decides what the desktop renders from
    # a public /api/ledger response and which pending record gets a payment
    # button. Core-only, so the money-facing rules stay covered where the
    # window suite is flaky.
    qt_add_executable(forkmesh-ledger-tests
        tests/test_ledger_feature.cpp
        ${FORKMESH_PACKAGE_LEDGER_SOURCES}
    )
    target_link_libraries(forkmesh-ledger-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-ledger-tests PRIVATE
        ${FORKMESH_MARKETPLACE_INCLUDE_DIRS})
    if(APPLE)
        set_target_properties(forkmesh-ledger-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # A native package may own a focused test executable alongside its source.
    # Discovery emits only validated source paths and allowlisted Qt targets;
    # core CMake never needs a package id or package-specific filename.
    set(FORKMESH_MARKETPLACE_TEST_TARGETS)
    foreach(_forkmesh_package_id IN LISTS FORKMESH_MARKETPLACE_TEST_IDS)
        string(REGEX REPLACE "[^A-Za-z0-9]" "_"
               _forkmesh_package_variable "${_forkmesh_package_id}")
        string(TOUPPER "${_forkmesh_package_variable}"
               _forkmesh_package_variable)
        set(_forkmesh_package_test_sources
            "FORKMESH_PACKAGE_${_forkmesh_package_variable}_TEST_SOURCES")
        set(_forkmesh_package_test_libraries
            "FORKMESH_PACKAGE_${_forkmesh_package_variable}_TEST_LIBRARIES")
        set(_forkmesh_package_test_target
            "forkmesh-${_forkmesh_package_id}-tests")
        qt_add_executable(${_forkmesh_package_test_target}
            ${${_forkmesh_package_test_sources}})
        target_link_libraries(${_forkmesh_package_test_target} PRIVATE
            ${${_forkmesh_package_test_libraries}})
        target_include_directories(${_forkmesh_package_test_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${FORKMESH_MARKETPLACE_INCLUDE_DIRS})
        if(APPLE)
            set_target_properties(${_forkmesh_package_test_target} PROPERTIES
                MACOSX_BUNDLE FALSE)
        endif()
        list(APPEND FORKMESH_MARKETPLACE_TEST_TARGETS
             ${_forkmesh_package_test_target})
    endforeach()

    # Log retention: rotation, the 30-day age-out and the chart digest. Core
    # only, so it stays runnable where the window suite is flaky.
    qt_add_executable(forkmesh-network-log-tests
        tests/test_network_log_archive.cpp
        src/NetworkLogArchive.h
        src/NetworkLogArchive.cpp
    )
    target_link_libraries(forkmesh-network-log-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-network-log-tests PRIVATE src)

    # The Hosts live view's log taps: which chip a journal line belongs to and
    # whether the remote script still opens every feed. Core only, so the
    # classifier stays covered where the window suite is flaky.
    # Manual check of the HTTP/3 client against a real server:
    # forkmesh-http3-probe https://host/path [Header: value]...
    if(FORKMESH_HAVE_HTTP3)
        qt_add_executable(forkmesh-http3-probe
            tests/http3_probe.cpp
            src/Http3Get.cpp
        )
        set_target_properties(forkmesh-http3-probe PROPERTIES
            EXCLUDE_FROM_ALL ON AUTOMOC OFF)
        target_include_directories(forkmesh-http3-probe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        target_link_libraries(forkmesh-http3-probe PRIVATE Qt6::Core Qt6::Network)
        forkmesh_link_http3(forkmesh-http3-probe)
    endif()

    qt_add_executable(forkmesh-host-log-tests
        tests/test_host_log_sources.cpp
        src/HostLogSources.h
    )
    target_link_libraries(forkmesh-host-log-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-host-log-tests PRIVATE src)

    qt_add_executable(forkmesh-core-node-log-tests
        tests/test_core_node_logs.cpp
        src/CoreNodeLogs.h
        src/CoreNodeLogs.cpp
        src/IpCountry.h
        src/IpCountry.cpp
        src/IpWhois.h
        src/IpWhois.cpp
    )
    target_link_libraries(forkmesh-core-node-log-tests PRIVATE Qt6::Core Qt6::Network)
    target_include_directories(forkmesh-core-node-log-tests PRIVATE src)

    qt_add_executable(forkmesh-deploy-log-tests
        tests/test_deploy_logs.cpp
        src/DeployLogs.h
        src/DeployLogs.cpp
        src/CoreNodeLogs.h
        src/CoreNodeLogs.cpp
    )
    target_link_libraries(forkmesh-deploy-log-tests PRIVATE Qt6::Core Qt6::Network)
    target_include_directories(forkmesh-deploy-log-tests PRIVATE src)

    # Restart estimates are deliberately a small Core-only unit: the median
    # history and milestone-profile countdown can stay covered without starting
    # a real rebuild or relaunching the desktop test process.
    qt_add_executable(forkmesh-restart-timing-tests
        tests/test_restart_timing.cpp
        src/RestartTiming.h
    )
    target_link_libraries(forkmesh-restart-timing-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-restart-timing-tests PRIVATE src)

    qt_add_executable(forkmesh-prompt-spellcheck-tests
        tests/test_prompt_spellcheck.cpp
        src/PromptSpellCheckHighlighter.h
        src/PromptSpellCheckHighlighter.cpp
    )
    target_link_libraries(forkmesh-prompt-spellcheck-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-prompt-spellcheck-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-prompt-spellcheck-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # A crash is written to the log by the dying process, so the next launch
    # is the only thing that can turn it into repair work. Core only: the
    # "crash, not a stop request" rule stays covered where the window suite is
    # flaky.
    qt_add_executable(forkmesh-crash-scan-tests
        tests/test_crash_log_scan.cpp
        src/CrashLogScan.h
        src/CrashLogScan.cpp
    )
    target_link_libraries(forkmesh-crash-scan-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-crash-scan-tests PRIVATE src)

    qt_add_executable(forkmesh-diagnostics-report-tests
        tests/test_diagnostics_report.cpp
        src/DiagnosticsReport.h
        src/DiagnosticsReport.cpp
    )
    target_link_libraries(forkmesh-diagnostics-report-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-diagnostics-report-tests PRIVATE src)

    qt_add_executable(forkmesh-claude-session-scan-tests
        tests/test_claude_session_scan.cpp
        src/ClaudeSessionScan.h
        src/ClaudeSessionScan.cpp
    )
    target_link_libraries(forkmesh-claude-session-scan-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-claude-session-scan-tests PRIVATE src)

    qt_add_executable(forkmesh-machine-agent-scan-tests
        tests/test_machine_agent_scan.cpp
        src/MachineAgentScan.h
        src/MachineAgentScan.cpp
        src/SystemStats.h
        src/SystemStats.cpp
    )
    target_link_libraries(forkmesh-machine-agent-scan-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-machine-agent-scan-tests PRIVATE src)

    qt_add_executable(forkmesh-agent-log-text-tests
        tests/test_agent_log_text.cpp
        src/AgentLogText.h
        src/AgentLogText.cpp
        src/ClaudePeerMessaging.h
        src/ClaudePeerMessaging.cpp
    )
    target_link_libraries(forkmesh-agent-log-text-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-agent-log-text-tests PRIVATE src)
    target_compile_definitions(forkmesh-agent-log-text-tests PRIVATE
        FORKMESH_AGENT_TRANSCRIPT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../conformance/agent-transcript/v1")

    if(UNIX)
        add_executable(forkmesh-term-tests
            tests/test_forkmesh_term.cpp
            src/ForkMeshTerm.h
            src/ForkMeshTerm.cpp
        )
        target_include_directories(forkmesh-term-tests PRIVATE src)
        target_compile_definitions(forkmesh-term-tests PRIVATE
            FORKMESH_TERM_BINARY="$<TARGET_FILE:forkmesh-term>")
        add_dependencies(forkmesh-term-tests forkmesh-term)
        qt_add_executable(forkmesh-terminal-handoff-tests
            tests/test_terminal_handoff.cpp
            src/TerminalHandoff.h
            src/TerminalHandoff.cpp
            src/ClaudePeerMessaging.h
            src/ClaudePeerMessaging.cpp
            src/LocalControl.h
            src/LocalControl.cpp
            src/LocalControlStream.h
            src/LocalControlStream.cpp
            src/LocalControlHosts.h
            src/LocalControlHosts.cpp
        )
        target_include_directories(forkmesh-terminal-handoff-tests PRIVATE src)
        target_link_libraries(forkmesh-terminal-handoff-tests PRIVATE
            Qt6::Core Qt6::Network OpenSSL::Crypto)
        target_compile_definitions(forkmesh-terminal-handoff-tests PRIVATE
            FORKMESH_TERM_BINARY="$<TARGET_FILE:forkmesh-term>")
        add_dependencies(forkmesh-terminal-handoff-tests forkmesh-term)
        if(NOT APPLE)
            target_link_libraries(forkmesh-term-tests PRIVATE util)
            target_link_libraries(forkmesh-terminal-handoff-tests PRIVATE util)
        else()
            set_target_properties(forkmesh-terminal-handoff-tests PROPERTIES MACOSX_BUNDLE FALSE)
        endif()
        set_target_properties(forkmesh-term-tests forkmesh-terminal-handoff-tests
            PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endif()

    qt_add_executable(forkmesh-claude-peer-messaging-tests
        tests/test_claude_peer_messaging.cpp
        src/ClaudePeerMessaging.h
        src/ClaudePeerMessaging.cpp
    )
    target_link_libraries(forkmesh-claude-peer-messaging-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-claude-peer-messaging-tests PRIVATE src)
    target_compile_definitions(forkmesh-claude-peer-messaging-tests PRIVATE
        FORKMESH_CLAUDE_PEER_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../conformance/claude-peer/v1/fixture.json")

    # The UI-stall watchdog and the periodic refresh timers read the kernel's
    # pressure stall information through these parsers; the /proc formats and
    # the "under pressure" line stay pinned where CI always runs.
    qt_add_executable(forkmesh-system-pressure-tests
        tests/test_system_pressure.cpp
        src/SystemPressure.h
        src/SystemPressure.cpp
        src/SystemStats.h
        src/SystemStats.cpp
    )
    target_link_libraries(forkmesh-system-pressure-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-system-pressure-tests PRIVATE src)

    qt_add_executable(forkmesh-agent-resources-tests
        tests/test_agent_resources.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/AgentResourceGovernor.h
        src/AgentResourceGovernor.cpp
        src/SystemPressure.h
        src/SystemPressure.cpp
        src/SystemStats.h
        src/SystemStats.cpp
    )
    target_link_libraries(forkmesh-agent-resources-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-agent-resources-tests PRIVATE src)

    # The projects layout: root resolution, the rename-only migration of legacy
    # locations, ssh_config rewriting, the storage map and the project registry,
    # all inside a temporary home folder.
    qt_add_executable(forkmesh-app-paths-tests
        tests/test_app_paths.cpp
        src/SourceCheckout.h
        src/AppPaths.h
        src/AppPaths.cpp
        src/ProjectRegistry.h
        src/ProjectRegistry.cpp
    )
    target_link_libraries(forkmesh-app-paths-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-app-paths-tests PRIVATE src)

    qt_add_executable(forkmesh-process-reaper-tests
        tests/test_process_reaper.cpp
        src/ProcessReaper.h
        src/ProcessReaper.cpp
    )
    target_link_libraries(forkmesh-process-reaper-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-process-reaper-tests PRIVATE src)

    qt_add_executable(forkmesh-safe-delete-tests
        tests/test_safe_delete.cpp
        src/SafeDelete.h
        src/AppPaths.h
    )
    target_link_libraries(forkmesh-safe-delete-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-safe-delete-tests PRIVATE src)

    qt_add_executable(forkmesh-agent-parity-tests
        tests/test_agent_parity.cpp
        src/AgentParity.h
    )
    target_link_libraries(forkmesh-agent-parity-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-agent-parity-tests PRIVATE src)

    qt_add_executable(forkmesh-agent-scratch-tests
        tests/test_agent_scratch.cpp
        src/AgentScratch.h
        src/AgentWorktree.h
        src/SafeDelete.h
        src/AppPaths.h
    )
    target_link_libraries(forkmesh-agent-scratch-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-agent-scratch-tests PRIVATE src)

    # Project settings > Agents: the policy in project.json, the folder lock it
    # puts on launches, the external-call classifier and the Claude Code hook
    # talking to the gate socket (inside a real Landlock lock on Linux).
    qt_add_executable(forkmesh-agent-access-tests
        tests/test_agent_access.cpp
        src/DestructiveCommandReview.h
        src/DestructiveCommandReview.cpp
        src/AgentGuardScript.h
        src/AgentGuardrails.h
        src/AgentGuardrails.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/AgentAccessFailure.h
        src/AgentAccessFailure.cpp
        src/AgentNetworkGate.h
        src/AgentNetworkGate.cpp
        src/CommandVariables.h
        src/CommandVariables.cpp
        src/AgentRunRules.h
        src/AgentRunRules.cpp
        src/ProjectAgentAccess.h
        src/ProjectAgentAccess.cpp
        src/VirtualMachineRuntime.h
        src/VirtualMachineRuntime.cpp
    )
    target_link_libraries(forkmesh-agent-access-tests PRIVATE Qt6::Core Qt6::Network)
    target_include_directories(forkmesh-agent-access-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-agent-access-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-agent-boundary-tests
        tests/test_agent_host_boundaries.cpp
        src/AgentCredentialEnvironment.h
        src/AgentGuardScript.h
        src/AgentGuardrails.h
        src/AgentGuardrails.cpp
        src/AgentResources.h
        src/AgentResources.cpp
        src/AgentGuardrailLedger.h
        src/AgentGuardrailLedger.cpp
        src/BackgroundFileWriter.h
        src/BackgroundFileWriter.cpp
        src/AgentJsonlFramer.h
        src/AgentJsonlFramer.cpp
        src/AgentJail.h
        src/AgentStore.h
        src/AgentStore.cpp
        src/AgentResumeIdentity.h
        src/AgentRuntimeSession.h
        src/AgentRuntimeSession.cpp
        src/ClaudeStreamSession.h
        src/ClaudeStreamSession.cpp
        src/CodexAppServerSession.h
        src/CodexAppServerSession.cpp
        src/CodexTranscriptStyle.h
        src/AgentToolCatalog.h
        src/AgentToolCatalog.cpp
        src/AgentToolPolicy.h
        src/AgentToolPolicy.cpp
        src/AgentAccessFailure.h
        src/AgentAccessFailure.cpp
        src/AgentNetworkGate.h
        src/AgentNetworkGate.cpp
        src/CommandVariables.h
        src/CommandVariables.cpp
        src/AgentRunRules.h
        src/AgentRunRules.cpp
        src/ProjectAgentAccess.h
        src/ProjectAgentAccess.cpp
        src/VirtualMachineRuntime.h
        src/VirtualMachineRuntime.cpp
    )
    target_compile_definitions(forkmesh-agent-boundary-tests PRIVATE
        FORKMESH_AGENT_ADVERSARIAL_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/../conformance/agent-adversarial/v1/fixtures.json")
    target_link_libraries(forkmesh-agent-boundary-tests PRIVATE Qt6::Core Qt6::Network)
    target_include_directories(forkmesh-agent-boundary-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-agent-boundary-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # The Memory page's smaps / status parsers and mapping classifier, plus the
    # chart's axis rounding; pinned on verbatim /proc text.
    qt_add_executable(forkmesh-process-memory-tests
        tests/test_process_memory.cpp
        src/ProcessMemory.h
        src/ProcessMemory.cpp
        src/MemoryUsageChart.h
        src/MemoryUsageChart.cpp
        src/SystemPressure.h
        src/SystemPressure.cpp
        src/SystemStats.h
        src/SystemStats.cpp
    )
    target_link_libraries(forkmesh-process-memory-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-process-memory-tests PRIVATE src)

    # Blocks its own GUI thread and checks the watchdog dates the stall from
    # when the thread stopped answering, with the machine state alongside.
    qt_add_executable(forkmesh-stall-watchdog-tests
        tests/test_stall_watchdog.cpp
        src/StallWatchdog.h
        src/StallWatchdog.cpp
        src/SystemPressure.h
        src/SystemPressure.cpp
        src/SystemStats.h
        src/SystemStats.cpp
    )
    target_link_libraries(forkmesh-stall-watchdog-tests PRIVATE Qt6::Core)
    target_include_directories(forkmesh-stall-watchdog-tests PRIVATE src)

    # Event receivers may destroy themselves while QApplication::notify is
    # still on the stack. Keep this lifecycle regression small and independent
    # of the broad MainWindow suite so CI always exercises the crash boundary.
    qt_add_executable(forkmesh-qt-runtime-diagnostics-tests
        tests/test_qt_runtime_diagnostics.cpp
        src/QtRuntimeDiagnostics.h
        src/QtRuntimeDiagnostics.cpp
    )
    target_link_libraries(forkmesh-qt-runtime-diagnostics-tests
        PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-qt-runtime-diagnostics-tests PRIVATE src)
    target_compile_definitions(forkmesh-qt-runtime-diagnostics-tests
        PRIVATE FORKMESH_QT_RUNTIME_DIAGNOSTICS_TESTS)

    target_compile_definitions(forkmesh-window-tests PRIVATE FORKMESH_WINDOW_TESTS)
    # Recompiles every heavy app source: same throttle as the app target.
    set_target_properties(forkmesh-window-tests PROPERTIES
        JOB_POOL_COMPILE forkmesh_heavy)
    if(APPLE)
        set_target_properties(forkmesh-window-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Keep the pre-QApplication display check and splash transition in a small
    # focused target so startup remains covered without recompiling every
    # MainWindow source.
    # Tooltips: the bounded preview text, the placement rule, and the
    # app-drawn tip window offscreen (docs/desktop/README.md, Traps).
    qt_add_executable(forkmesh-tooltip-tests
        tests/test_tooltips.cpp
        src/Tooltips.h
    )
    target_link_libraries(forkmesh-tooltip-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-tooltip-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-tooltip-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    qt_add_executable(forkmesh-startup-splash-tests
        tests/test_startup_splash.cpp
        src/StartupSplash.h
        src/StartupSplash.cpp
        src/DesktopDisplayReadiness.h
        src/DesktopDisplayReadiness.cpp
    )
    target_link_libraries(forkmesh-startup-splash-tests PRIVATE Qt6::Widgets)
    target_include_directories(forkmesh-startup-splash-tests PRIVATE src)
    if(APPLE)
        set_target_properties(forkmesh-startup-splash-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()

    # A third suite (issue #357): performance budgets for startup and
    # repo-detail tab switches. Kept in its own binary rather than folded into
    # forkmesh-window-tests, which is known-flaky in some sandboxed
    # environments — a perf gate must be individually skippable
    # (FORKMESH_SKIP_PERF_TESTS=1) without disabling window-tests' functional
    # coverage.
    qt_add_executable(forkmesh-perf-tests
        tests/test_perf_budgets.cpp
        ${FORKMESH_APP_SOURCES}
    )
    target_link_libraries(forkmesh-perf-tests PRIVATE forkmesh-resources forkmesh-browser-pane Qt6::Widgets Qt6::Network Qt6::Svg Qt6::Concurrent OpenSSL::Crypto)
    target_include_directories(forkmesh-perf-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${FORKMESH_MARKETPLACE_INCLUDE_DIRS})
    forkmesh_link_webp(forkmesh-perf-tests)
    forkmesh_link_multimedia(forkmesh-perf-tests)
    forkmesh_link_webengine(forkmesh-perf-tests)
forkmesh_link_opengl(forkmesh-perf-tests)
    forkmesh_link_http3(forkmesh-perf-tests)
    forkmesh_link_power_source(forkmesh-perf-tests)
    if(UNIX AND NOT APPLE)
        target_link_libraries(forkmesh-perf-tests PRIVATE util Qt6::DBus)
        target_compile_definitions(forkmesh-perf-tests PRIVATE FORKMESH_HAVE_PORTAL)
    endif()
    target_compile_definitions(forkmesh-perf-tests PRIVATE FORKMESH_WINDOW_TESTS)
    # Recompiles every heavy app source: same throttle as the app target.
    set_target_properties(forkmesh-perf-tests PROPERTIES
        JOB_POOL_COMPILE forkmesh_heavy)
    if(APPLE)
        set_target_properties(forkmesh-perf-tests PROPERTIES MACOSX_BUNDLE FALSE)
    endif()

    # Keep the test binaries out of the default `all` target: window-tests and
    # perf-tests each recompile every app source, so building them implicitly
    # tripled the cost of a plain `cmake --build build` (what run.sh and the
    # in-app quick-update do). They still build on demand — `--target check`
    # below depends on all of them, run.sh test and ci.yml build the
    # `critical-tests` aggregate, and the release/deploy builds configure with
    # FORKMESH_BUILD_TESTS=OFF anyway.
    set_target_properties(
        forkmesh-trust-list-tests
        forkmesh-cove-files-tests forkmesh-tests
        forkmesh-project-invite-tests
        forkmesh-stats-dashboard-tests
        forkmesh-fleet-visualizer-tests
        forkmesh-ide-tests forkmesh-world-speech-tests
        forkmesh-agent-runtime-contract-tests
        forkmesh-agent-tool-efficiency-tests
        forkmesh-agent-model-stats-tests
        forkmesh-codex-tests forkmesh-agent-transport-tests
        forkmesh-agent-recycle-tests
        forkmesh-agent-cloud-tests
        forkmesh-agent-cloud-stream-tests
        forkmesh-agent-workspace-name-tests
        forkmesh-local-control-tests
        forkmesh-local-control-attachments-tests
        forkmesh-local-control-handoff-tests
        forkmesh-agent-intake-tests
        forkmesh-org-agent-job-attachments-tests
        forkmesh-local-control-prompt-images-tests
        forkmesh-vm-runtime-tests
        forkmesh-control-tests forkmesh-chat-transport-tests
        forkmesh-action-security-tests
        forkmesh-private-mirror-tests
        forkmesh-public-mirror-tests
        forkmesh-upstream-sync-tests
        forkmesh-git-index-queue-tests
        forkmesh-agent-prompt-title-tests
        forkmesh-diff-intra-line-tests
        forkmesh-pull-store-retire-tests
        forkmesh-web-branch-ops-tests
        forkmesh-add-core-node-tests
        forkmesh-aws-credentials-tests
        forkmesh-systemd-timers-tests
        forkmesh-remove-core-node-tests
        forkmesh-e2e-stub-agent forkmesh-e2e
        forkmesh-window-tests forkmesh-startup-splash-tests
        forkmesh-tooltip-tests
        forkmesh-list-view-mode-tests
        forkmesh-system-open-folder-tests
        forkmesh-agent-ticker-tests
        forkmesh-settings-write-tests
        forkmesh-perf-tests forkmesh-repo-stats-tests
        forkmesh-agent-folders-tests
        forkmesh-sonifier-tests
        forkmesh-ui-sounds-tests
        forkmesh-user-activity-tests
        forkmesh-announcer-tests
        forkmesh-suggested-apps-tests
        forkmesh-package-tests
        forkmesh-native-package-tests
        forkmesh-teammate-tests
        forkmesh-teammate-workspace-tests
        forkmesh-teammate-mood-tests
        forkmesh-ledger-tests
        ${FORKMESH_MARKETPLACE_TEST_TARGETS}
        forkmesh-network-log-tests
        forkmesh-restart-timing-tests
        forkmesh-host-log-tests
        forkmesh-core-node-log-tests
        forkmesh-deploy-log-tests
        forkmesh-crash-scan-tests forkmesh-system-pressure-tests
        forkmesh-diagnostics-report-tests
        forkmesh-agent-resources-tests
        forkmesh-claude-session-scan-tests
        forkmesh-machine-agent-scan-tests
        forkmesh-agent-log-text-tests
        forkmesh-claude-peer-messaging-tests
        forkmesh-process-memory-tests
        forkmesh-app-paths-tests
        forkmesh-safe-delete-tests
        forkmesh-agent-scratch-tests
        forkmesh-agent-parity-tests
        forkmesh-process-reaper-tests
        forkmesh-stall-watchdog-tests
        forkmesh-qt-runtime-diagnostics-tests
        forkmesh-prompt-spellcheck-tests
        PROPERTIES EXCLUDE_FROM_ALL TRUE)

    # Register all suites with CTest so they run independently of the app:
    #   ctest --test-dir build            (runs the already-built binaries)
    #   cmake --build build --target check (builds the tests, then runs them)
    # The desktop smoke tests need an offscreen Qt platform.
    enable_testing()
    add_test(NAME forkmesh-cove-files-tests COMMAND forkmesh-cove-files-tests)
    add_test(NAME forkmesh-project-invite-tests
             COMMAND forkmesh-project-invite-tests)
    add_test(NAME forkmesh-stats-dashboard-tests
             COMMAND forkmesh-stats-dashboard-tests)
    add_test(NAME forkmesh-fleet-visualizer-tests
             COMMAND forkmesh-fleet-visualizer-tests)
    add_test(NAME forkmesh-tests COMMAND forkmesh-tests)
    add_test(NAME forkmesh-settings-sections-tests
             COMMAND forkmesh-settings-sections-tests)
    add_test(NAME forkmesh-ping-settings-tests
             COMMAND forkmesh-ping-settings-tests)
    add_test(NAME forkmesh-repo-stats-tests COMMAND forkmesh-repo-stats-tests)
    add_test(NAME forkmesh-agent-folders-tests
             COMMAND forkmesh-agent-folders-tests)
    add_test(NAME forkmesh-sonifier-tests COMMAND forkmesh-sonifier-tests)
    add_test(NAME forkmesh-ui-sounds-tests COMMAND forkmesh-ui-sounds-tests)
    add_test(NAME forkmesh-user-activity-tests
             COMMAND forkmesh-user-activity-tests)
    add_test(NAME forkmesh-announcer-tests COMMAND forkmesh-announcer-tests)
    add_test(NAME forkmesh-code-health-tests COMMAND forkmesh-code-health-tests)
    add_test(NAME forkmesh-binary-size-tests COMMAND forkmesh-binary-size-tests)
    add_test(NAME forkmesh-component-health-board-tests
        COMMAND forkmesh-component-health-board-tests)
    add_test(NAME forkmesh-home-board-tests
        COMMAND forkmesh-home-board-tests)
    add_test(NAME forkmesh-code-map-model-tests
        COMMAND forkmesh-code-map-model-tests)
    add_test(NAME forkmesh-code-map-view-tests
        COMMAND forkmesh-code-map-view-tests)
    add_test(NAME forkmesh-window-fade-tests
        COMMAND forkmesh-window-fade-tests)
    add_test(NAME forkmesh-tooltip-tests COMMAND forkmesh-tooltip-tests)
    add_test(NAME forkmesh-suggested-apps-tests
             COMMAND forkmesh-suggested-apps-tests)
    add_test(NAME forkmesh-package-tests COMMAND forkmesh-package-tests)
    add_test(NAME forkmesh-native-package-tests
             COMMAND forkmesh-native-package-tests)
    add_test(NAME forkmesh-teammate-tests COMMAND forkmesh-teammate-tests)
    add_test(NAME forkmesh-teammate-workspace-tests
             COMMAND forkmesh-teammate-workspace-tests)
    add_test(NAME forkmesh-teammate-mood-tests COMMAND forkmesh-teammate-mood-tests)
    add_test(NAME forkmesh-user-avatar-tests COMMAND forkmesh-user-avatar-tests)
    add_test(NAME forkmesh-user-status-tests COMMAND forkmesh-user-status-tests)
    add_test(NAME forkmesh-prompt-draft-tests COMMAND forkmesh-prompt-draft-tests)
    add_test(NAME forkmesh-saved-search-tests COMMAND forkmesh-saved-search-tests)
    add_test(NAME forkmesh-security-alarm-tests COMMAND forkmesh-security-alarm-tests)
    add_test(NAME forkmesh-relay-repository-scope-tests
             COMMAND forkmesh-relay-repository-scope-tests)
    add_test(NAME forkmesh-ad-banner-model-tests
             COMMAND forkmesh-ad-banner-model-tests)
    add_test(NAME forkmesh-genie-vitals-tests
             COMMAND forkmesh-genie-vitals-tests)
    add_test(NAME forkmesh-genie-landmarks-tests
             COMMAND forkmesh-genie-landmarks-tests)
    set_tests_properties(forkmesh-genie-landmarks-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-issue-mention-tests
             COMMAND forkmesh-issue-mention-tests)
    add_test(NAME forkmesh-ledger-tests COMMAND forkmesh-ledger-tests)
    foreach(_forkmesh_package_test_target IN LISTS
            FORKMESH_MARKETPLACE_TEST_TARGETS)
        add_test(NAME ${_forkmesh_package_test_target}
                 COMMAND ${_forkmesh_package_test_target})
    endforeach()
    add_test(NAME forkmesh-network-log-tests COMMAND forkmesh-network-log-tests)
    add_test(NAME forkmesh-restart-timing-tests
             COMMAND forkmesh-restart-timing-tests)
    add_test(NAME forkmesh-host-log-tests COMMAND forkmesh-host-log-tests)
    add_test(NAME forkmesh-core-node-log-tests
             COMMAND forkmesh-core-node-log-tests)
    add_test(NAME forkmesh-deploy-log-tests COMMAND forkmesh-deploy-log-tests)
    add_test(NAME forkmesh-prompt-spellcheck-tests
             COMMAND forkmesh-prompt-spellcheck-tests)
    add_test(NAME forkmesh-crash-scan-tests COMMAND forkmesh-crash-scan-tests)
    add_test(NAME forkmesh-diagnostics-report-tests COMMAND forkmesh-diagnostics-report-tests)
    add_test(NAME forkmesh-claude-session-scan-tests
             COMMAND forkmesh-claude-session-scan-tests)
    add_test(NAME forkmesh-machine-agent-scan-tests
             COMMAND forkmesh-machine-agent-scan-tests)
    add_test(NAME forkmesh-agent-log-text-tests
             COMMAND forkmesh-agent-log-text-tests)
    add_test(NAME forkmesh-claude-peer-messaging-tests
             COMMAND forkmesh-claude-peer-messaging-tests)
    if(UNIX)
        add_test(NAME forkmesh-term-tests COMMAND forkmesh-term-tests)
        add_test(NAME forkmesh-terminal-handoff-tests
                 COMMAND forkmesh-terminal-handoff-tests)
    endif()
    add_test(NAME forkmesh-system-pressure-tests
             COMMAND forkmesh-system-pressure-tests)
    add_test(NAME forkmesh-agent-resources-tests
             COMMAND forkmesh-agent-resources-tests)
    add_test(NAME forkmesh-process-memory-tests
             COMMAND forkmesh-process-memory-tests)
    add_test(NAME forkmesh-app-paths-tests
             COMMAND forkmesh-app-paths-tests)
    add_test(NAME forkmesh-safe-delete-tests
             COMMAND forkmesh-safe-delete-tests)
    add_test(NAME forkmesh-agent-scratch-tests
             COMMAND forkmesh-agent-scratch-tests)
    add_test(NAME forkmesh-agent-parity-tests
             COMMAND forkmesh-agent-parity-tests)
    add_test(NAME forkmesh-process-reaper-tests
             COMMAND forkmesh-process-reaper-tests)
    add_test(NAME forkmesh-agent-access-tests
             COMMAND forkmesh-agent-access-tests)
    add_test(NAME forkmesh-agent-boundary-tests
             COMMAND forkmesh-agent-boundary-tests)
    add_test(NAME forkmesh-stall-watchdog-tests
             COMMAND forkmesh-stall-watchdog-tests)
    add_test(NAME forkmesh-qt-runtime-diagnostics-tests
             COMMAND forkmesh-qt-runtime-diagnostics-tests)
    find_program(FORKMESH_PYTHON_EXECUTABLE NAMES python3 python)
    if(FORKMESH_PYTHON_EXECUTABLE)
        add_test(
            NAME forkmesh-agent-e2ee-worker-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_owner_envelope_contract.py"
                    "$<TARGET_FILE:forkmesh-tests>")
        add_test(
            NAME forkmesh-org-agent-queue-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_org_agent_queue_contract.py")
        add_test(
            NAME forkmesh-cloud-agent-claim-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cloud_agent_claim_contract.py")
        add_test(
            NAME forkmesh-cloud-agent-watch-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cloud_agent_watch_contract.py")
        add_test(
            NAME forkmesh-cloud-agent-stream-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_cloud_agent_stream_contract.py")
        add_test(
            NAME forkmesh-agent-branch-only-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_agent_branch_only_contract.py")
        add_test(
            NAME forkmesh-agent-task-opt-in-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_agent_task_opt_in_contract.py")
        add_test(
            NAME forkmesh-system-link-vultr-confirm-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_system_link_vultr_confirm.py")
        add_test(
            NAME forkmesh-org-task-mine-filter-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_org_task_mine_filter.py")
        add_test(
            NAME forkmesh-error-log-regressions-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_error_log_regressions.py")
        add_test(
            NAME forkmesh-org-admin-proof-contract
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_org_admin_proof_contract.py")
        add_test(
            NAME forkmesh-octicon-names
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_octicon_names.py")
        add_test(
            NAME forkmesh-issue-author-avatar
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_issue_author_avatar.py")
        add_test(
            NAME forkmesh-chat-sender-avatar
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_chat_sender_avatar.py")
        add_test(
            NAME forkmesh-issue-directory-avatars
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_issue_directory_avatars.py")
        add_test(
            NAME forkmesh-qstring-unicode-literals
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_qstring_unicode_literals.py")
        add_test(
            NAME forkmesh-pull-refresh-responsiveness
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_pull_refresh_responsiveness.py")
        add_test(
            NAME forkmesh-release-version-stamp
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_release_version_stamp.py")
        add_test(
            NAME forkmesh-compile-gate
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_compile_gate.py")
        add_test(
            NAME forkmesh-no-environ-identifier
            COMMAND "${FORKMESH_PYTHON_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_no_environ_identifier.py")
    endif()
    add_test(NAME forkmesh-ide-tests COMMAND forkmesh-ide-tests)
    add_test(NAME forkmesh-world-speech-tests
             COMMAND forkmesh-world-speech-tests)
    add_test(NAME forkmesh-agent-runtime-contract-tests
             COMMAND forkmesh-agent-runtime-contract-tests)
    add_test(NAME forkmesh-agent-tool-efficiency-tests
             COMMAND forkmesh-agent-tool-efficiency-tests)
    add_test(NAME forkmesh-agent-model-stats-tests
             COMMAND forkmesh-agent-model-stats-tests)
    add_test(NAME forkmesh-codex-tests COMMAND forkmesh-codex-tests)
    add_test(NAME forkmesh-agent-transport-tests
        COMMAND forkmesh-agent-transport-tests)
    add_test(NAME forkmesh-agent-recycle-tests
        COMMAND forkmesh-agent-recycle-tests)
    add_test(NAME forkmesh-agent-cloud-tests
        COMMAND forkmesh-agent-cloud-tests)
    add_test(NAME forkmesh-agent-cloud-stream-tests
        COMMAND forkmesh-agent-cloud-stream-tests)
    add_test(NAME forkmesh-agent-workspace-name-tests
        COMMAND forkmesh-agent-workspace-name-tests)
    add_test(NAME forkmesh-runner-login-tests
        COMMAND forkmesh-runner-login-tests)
    add_test(NAME forkmesh-runner-setup-handoff-tests
        COMMAND forkmesh-runner-setup-handoff-tests)
    add_test(NAME forkmesh-local-control-tests
        COMMAND forkmesh-local-control-tests)
    add_test(NAME forkmesh-local-control-attachments-tests
        COMMAND forkmesh-local-control-attachments-tests)
    set_tests_properties(forkmesh-local-control-attachments-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-local-control-handoff-tests
        COMMAND forkmesh-local-control-handoff-tests)
    add_test(NAME forkmesh-agent-intake-tests
        COMMAND forkmesh-agent-intake-tests)
    add_test(NAME forkmesh-agent-windows-tests
        COMMAND forkmesh-agent-windows-tests)
    set_tests_properties(forkmesh-agent-windows-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-claude-transcript-tools-tests
        COMMAND forkmesh-claude-transcript-tools-tests)
    add_test(NAME forkmesh-org-agent-job-attachments-tests
        COMMAND forkmesh-org-agent-job-attachments-tests)
    add_test(NAME forkmesh-local-control-prompt-images-tests
        COMMAND forkmesh-local-control-prompt-images-tests)
    add_test(NAME forkmesh-vm-runtime-tests COMMAND forkmesh-vm-runtime-tests)
    add_test(NAME forkmesh-control-tests COMMAND forkmesh-control-tests)
    add_test(NAME forkmesh-trust-list-tests COMMAND forkmesh-trust-list-tests)
    add_test(NAME forkmesh-chat-transport-tests
             COMMAND forkmesh-chat-transport-tests)
    add_test(NAME forkmesh-discord-tests
             COMMAND forkmesh-chat-transport-tests --discord-only)
    add_test(NAME forkmesh-action-security-tests
             COMMAND forkmesh-action-security-tests)
    add_test(NAME forkmesh-private-mirror-tests
             COMMAND forkmesh-private-mirror-tests)
    add_test(NAME forkmesh-public-mirror-tests
             COMMAND forkmesh-public-mirror-tests)
    add_test(NAME forkmesh-upstream-sync-tests
             COMMAND forkmesh-upstream-sync-tests)
    add_test(NAME forkmesh-git-index-queue-tests
             COMMAND forkmesh-git-index-queue-tests)
    add_test(NAME forkmesh-agent-prompt-title-tests
             COMMAND forkmesh-agent-prompt-title-tests)
    add_test(NAME forkmesh-diff-intra-line-tests
             COMMAND forkmesh-diff-intra-line-tests)
    add_test(NAME forkmesh-ssh-mirror-push-tests
             COMMAND forkmesh-ssh-mirror-push-tests)
    add_test(NAME forkmesh-pull-store-retire-tests
             COMMAND forkmesh-pull-store-retire-tests)
    add_test(NAME forkmesh-web-branch-ops-tests
             COMMAND forkmesh-web-branch-ops-tests)
    add_test(NAME forkmesh-add-core-node-tests
             COMMAND forkmesh-add-core-node-tests)
    set_tests_properties(forkmesh-add-core-node-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-aws-credentials-tests
             COMMAND forkmesh-aws-credentials-tests)
    set_tests_properties(forkmesh-aws-credentials-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-systemd-timers-tests
             COMMAND forkmesh-systemd-timers-tests)
    set_tests_properties(forkmesh-systemd-timers-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-remove-core-node-tests
             COMMAND forkmesh-remove-core-node-tests)
    set_tests_properties(forkmesh-remove-core-node-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-task-work-items-tests
             COMMAND forkmesh-task-work-items-tests)
    add_test(NAME forkmesh-task-markdown-tests
             COMMAND forkmesh-task-markdown-tests)
    set_tests_properties(forkmesh-task-markdown-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_test(NAME forkmesh-list-view-mode-tests
             COMMAND forkmesh-list-view-mode-tests)
    add_test(NAME forkmesh-system-open-folder-tests
             COMMAND forkmesh-system-open-folder-tests)
    add_test(NAME forkmesh-agent-ticker-tests
             COMMAND forkmesh-agent-ticker-tests)
    add_test(NAME forkmesh-settings-write-tests
             COMMAND forkmesh-settings-write-tests)
    add_test(NAME forkmesh-note-request-proof-tests
             COMMAND forkmesh-note-request-proof-tests)
    add_test(NAME forkmesh-tip-tests COMMAND forkmesh-tip-tests)
    add_test(NAME forkmesh-window-tests COMMAND forkmesh-window-tests)
    add_test(NAME forkmesh-startup-splash-tests
             COMMAND forkmesh-startup-splash-tests)
    add_test(NAME forkmesh-teammates-window-tests
             COMMAND forkmesh-window-tests --teammates-only)
    add_test(NAME forkmesh-task-agent-avatar-window-tests
             COMMAND forkmesh-window-tests --task-agent-avatar-only)
    add_test(NAME forkmesh-agent-task-link-window-tests
             COMMAND forkmesh-window-tests --agent-task-link-only)
    add_test(NAME forkmesh-high-memory-panel-window-tests
             COMMAND forkmesh-window-tests --high-memory-panel-only)
    add_test(NAME forkmesh-agent-list-view-window-tests
             COMMAND forkmesh-window-tests --agent-list-view-only)
    add_test(NAME forkmesh-agent-folders-window-tests
             COMMAND forkmesh-window-tests --agent-folders-only)
    add_test(NAME forkmesh-agent-delete-branch-count-window-tests
             COMMAND forkmesh-window-tests --agent-delete-branch-count-only)
    add_test(NAME forkmesh-task-list-view-window-tests
             COMMAND forkmesh-window-tests --task-list-view-only)
    add_test(NAME forkmesh-mirror-fleet-window-tests
             COMMAND forkmesh-window-tests --mirror-fleet-only)
    add_test(NAME forkmesh-diff-stream-window-tests
             COMMAND forkmesh-window-tests --diff-stream-only)
    add_test(NAME forkmesh-saved-searches-window-tests
             COMMAND forkmesh-window-tests --saved-searches-only)
    add_test(NAME forkmesh-inline-diff-comments-window-tests
             COMMAND forkmesh-window-tests --inline-diff-comments-only)
    add_test(NAME forkmesh-diff-health-window-tests
             COMMAND forkmesh-window-tests --diff-health-only)
    add_test(NAME forkmesh-package-window-tests
             COMMAND forkmesh-window-tests --package-only)
    add_test(NAME forkmesh-tip-notice-window-tests
             COMMAND forkmesh-window-tests --tip-notice-only)
    add_test(NAME forkmesh-off-thread-teardown-window-tests
             COMMAND forkmesh-window-tests --off-thread-teardown-only)
    add_test(NAME forkmesh-onboarding-window-tests
             COMMAND forkmesh-window-tests --onboarding-only)
    add_test(NAME forkmesh-agent-efficiency-window-tests
             COMMAND forkmesh-window-tests --agent-efficiency-only)
    add_test(NAME forkmesh-agent-landing-window-tests
             COMMAND forkmesh-window-tests --agent-landing-only)
    add_test(NAME forkmesh-stats-window-tests
             COMMAND forkmesh-window-tests --stats-only)
    add_test(NAME forkmesh-view-modes-window-tests
             COMMAND forkmesh-window-tests --view-modes-only)
    add_test(NAME forkmesh-ui-snapshots-window-tests
             COMMAND forkmesh-window-tests --ui-snapshots-only)
    add_test(NAME forkmesh-chat-unread-window-tests
             COMMAND forkmesh-window-tests --chat-unread-only)
    add_test(NAME forkmesh-project-settings-window-tests
             COMMAND forkmesh-window-tests --project-settings-only)
    add_test(NAME forkmesh-perf-tests COMMAND forkmesh-perf-tests)
    add_test(NAME forkmesh-e2e COMMAND forkmesh-e2e)
    set_tests_properties(forkmesh-cove-files-tests
        forkmesh-project-invite-tests
        forkmesh-stats-dashboard-tests
        forkmesh-fleet-visualizer-tests
        forkmesh-code-map-view-tests
        forkmesh-window-fade-tests
        forkmesh-tests forkmesh-window-tests
        forkmesh-qt-runtime-diagnostics-tests
        forkmesh-prompt-spellcheck-tests
        forkmesh-startup-splash-tests
        forkmesh-teammates-window-tests
        forkmesh-mirror-fleet-window-tests forkmesh-diff-stream-window-tests
        forkmesh-saved-searches-window-tests
        forkmesh-inline-diff-comments-window-tests
        forkmesh-diff-health-window-tests
        forkmesh-package-window-tests forkmesh-tip-notice-window-tests
        forkmesh-off-thread-teardown-window-tests
        forkmesh-onboarding-window-tests
        forkmesh-agent-efficiency-window-tests
        forkmesh-agent-landing-window-tests
        forkmesh-stats-window-tests
        forkmesh-view-modes-window-tests
        forkmesh-ui-snapshots-window-tests
        forkmesh-task-agent-avatar-window-tests
        forkmesh-agent-task-link-window-tests
        forkmesh-high-memory-panel-window-tests
        forkmesh-agent-list-view-window-tests
        forkmesh-task-list-view-window-tests
        forkmesh-project-settings-window-tests
        forkmesh-perf-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # The default developer/CI suite stays intentionally small: deployment
    # trust boundaries, private/public mirror encryption, and the agent
    # transport. The remaining targets are available for focused changes.
    set(FORKMESH_CRITICAL_TESTS
        forkmesh-cove-files-tests
        forkmesh-control-tests forkmesh-trust-list-tests
        forkmesh-private-mirror-tests
        forkmesh-public-mirror-tests forkmesh-agent-runtime-contract-tests
        forkmesh-agent-tool-efficiency-tests
        forkmesh-codex-tests
        forkmesh-vm-runtime-tests forkmesh-startup-splash-tests
        forkmesh-qt-runtime-diagnostics-tests
        forkmesh-mirror-fleet-window-tests
        forkmesh-diff-stream-window-tests
        forkmesh-off-thread-teardown-window-tests
        forkmesh-agent-efficiency-window-tests)
    set_tests_properties(${FORKMESH_CRITICAL_TESTS}
        PROPERTIES LABELS "critical" TIMEOUT 20)
    # The binaries those test names execute. Not the same list: the focused
    # mirror-fleet, diff-stream and teardown suites are flag-selected runs of
    # forkmesh-window-tests.
    # Every critical test target is EXCLUDE_FROM_ALL, so ctest reports a bare
    # "***Not Run" when one was never built -- promoting a suite to critical
    # without teaching ci.yml to build it is silent until the run fails. This
    # aggregate is what ci.yml builds, so the two can no longer drift apart.
    set(FORKMESH_CRITICAL_TEST_TARGETS
        forkmesh-cove-files-tests
        forkmesh-control-tests forkmesh-trust-list-tests
        forkmesh-private-mirror-tests
        forkmesh-public-mirror-tests forkmesh-agent-runtime-contract-tests
        forkmesh-agent-tool-efficiency-tests
        forkmesh-codex-tests
        forkmesh-vm-runtime-tests forkmesh-window-tests
        forkmesh-startup-splash-tests
        forkmesh-qt-runtime-diagnostics-tests)
    add_custom_target(critical-tests DEPENDS ${FORKMESH_CRITICAL_TEST_TARGETS})
    # The e2e loop drives real git subprocesses and needs the stub agent's path.
    set_tests_properties(forkmesh-e2e PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;FORKMESH_E2E_STUB_AGENT=$<TARGET_FILE:forkmesh-e2e-stub-agent>")
    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL)
    add_dependencies(check forkmesh-cove-files-tests
        forkmesh-project-invite-tests
        forkmesh-stats-dashboard-tests
        forkmesh-fleet-visualizer-tests
        forkmesh-trust-list-tests
        forkmesh-tests forkmesh-ide-tests
        forkmesh-world-speech-tests forkmesh-agent-runtime-contract-tests
        forkmesh-codex-tests
        forkmesh-agent-model-stats-tests
        forkmesh-agent-transport-tests
        forkmesh-agent-boundary-tests
        forkmesh-agent-recycle-tests
        forkmesh-agent-cloud-tests
        forkmesh-agent-cloud-stream-tests
        forkmesh-agent-workspace-name-tests
        forkmesh-local-control-attachments-tests
        forkmesh-local-control-handoff-tests
        forkmesh-agent-intake-tests
        forkmesh-org-agent-job-attachments-tests
        forkmesh-local-control-prompt-images-tests
        forkmesh-vm-runtime-tests
        forkmesh-control-tests forkmesh-chat-transport-tests
        forkmesh-action-security-tests
        forkmesh-private-mirror-tests
        forkmesh-public-mirror-tests
        forkmesh-upstream-sync-tests
        forkmesh-git-index-queue-tests
        forkmesh-agent-prompt-title-tests
        forkmesh-diff-intra-line-tests
        forkmesh-ssh-mirror-push-tests
        forkmesh-pull-store-retire-tests
        forkmesh-web-branch-ops-tests
        forkmesh-add-core-node-tests
        forkmesh-aws-credentials-tests
        forkmesh-systemd-timers-tests
        forkmesh-tip-tests
        forkmesh-window-tests forkmesh-startup-splash-tests
        forkmesh-tooltip-tests
        forkmesh-list-view-mode-tests
        forkmesh-system-open-folder-tests
        forkmesh-agent-ticker-tests
        forkmesh-settings-write-tests
        forkmesh-perf-tests forkmesh-repo-stats-tests
        forkmesh-agent-folders-tests
        forkmesh-suggested-apps-tests forkmesh-package-tests
        forkmesh-native-package-tests
        forkmesh-network-log-tests forkmesh-restart-timing-tests
        forkmesh-host-log-tests
        forkmesh-core-node-log-tests
        forkmesh-deploy-log-tests
        forkmesh-e2e forkmesh-teammate-tests
        forkmesh-teammate-workspace-tests
        forkmesh-teammate-mood-tests
        forkmesh-issue-mention-tests
        forkmesh-ledger-tests
        forkmesh-prompt-spellcheck-tests
        ${FORKMESH_MARKETPLACE_TEST_TARGETS}
        forkmesh-crash-scan-tests forkmesh-system-pressure-tests
        forkmesh-diagnostics-report-tests
        forkmesh-agent-resources-tests
        forkmesh-claude-session-scan-tests
        forkmesh-machine-agent-scan-tests
        forkmesh-agent-log-text-tests
        forkmesh-claude-peer-messaging-tests
        forkmesh-process-memory-tests
        forkmesh-app-paths-tests
        forkmesh-safe-delete-tests
        forkmesh-agent-scratch-tests
        forkmesh-agent-parity-tests
        forkmesh-process-reaper-tests
        forkmesh-stall-watchdog-tests
        forkmesh-qt-runtime-diagnostics-tests
        forkmesh-e2e-stub-agent)
    if(UNIX)
        add_dependencies(check forkmesh-term-tests forkmesh-terminal-handoff-tests)
    endif()

    # Generate provenance before any test target can compile a source that
    # includes ForkMeshVersion.h. Applying this uniformly also makes a
    # dirty-after-configure regression observable in focused test builds.
    foreach(_forkmesh_test_target
            forkmesh-cove-files-tests forkmesh-tests
            forkmesh-ide-tests forkmesh-world-speech-tests
            forkmesh-agent-runtime-contract-tests
            forkmesh-codex-tests forkmesh-vm-runtime-tests
            forkmesh-control-tests
            forkmesh-chat-transport-tests
            forkmesh-action-security-tests
            forkmesh-private-mirror-tests forkmesh-public-mirror-tests
            forkmesh-upstream-sync-tests
            forkmesh-agent-prompt-title-tests
            forkmesh-diff-intra-line-tests
            forkmesh-reward-pool-tests forkmesh-tip-tests
            forkmesh-e2e-stub-agent forkmesh-e2e
            forkmesh-window-tests forkmesh-perf-tests)
        add_dependencies(${_forkmesh_test_target} forkmesh-version-header)
    endforeach()

    qt_add_executable(forkmesh-desktop-ping-forward-tests
        tests/test_desktop_ping_forward.cpp
        src/DesktopPingForward.h
        src/DesktopPingForward.cpp
    )
    target_include_directories(forkmesh-desktop-ping-forward-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-desktop-ping-forward-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-desktop-ping-forward-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-desktop-ping-forward-tests
        COMMAND forkmesh-desktop-ping-forward-tests)

    qt_add_executable(forkmesh-desktop-power-alert-tests
        tests/test_desktop_power_alert.cpp
        src/DesktopPowerAlert.h
        src/DesktopPowerAlert.cpp
        src/DesktopPingForward.h
        src/DesktopPingForward.cpp
    )
    target_include_directories(forkmesh-desktop-power-alert-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-desktop-power-alert-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-desktop-power-alert-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-desktop-power-alert-tests
        COMMAND forkmesh-desktop-power-alert-tests)

    qt_add_executable(forkmesh-ping-fixes-tests
        tests/test_ping_fixes.cpp
        src/PingFixes.h
        src/PingFixes.cpp
    )
    target_include_directories(forkmesh-ping-fixes-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-ping-fixes-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-ping-fixes-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-ping-fixes-tests
        COMMAND forkmesh-ping-fixes-tests)

    qt_add_executable(forkmesh-local-control-agentops-tests
        tests/test_local_control_agentops.cpp
        src/LocalControlAgentOps.h
        src/LocalControlAgentOps.cpp
        src/LocalControlHosts.h
        src/LocalControlHosts.cpp
    )
    target_include_directories(forkmesh-local-control-agentops-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-agentops-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-agentops-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-agentops-tests
        COMMAND forkmesh-local-control-agentops-tests)
    qt_add_executable(forkmesh-local-control-mergequeue-tests
        tests/test_local_control_mergequeue.cpp
        src/LocalControlMergeQueue.h
        src/LocalControlMergeQueue.cpp
        src/LocalControlHosts.h
        src/LocalControlHosts.cpp
        src/MergeQueue.h
        src/MergeQueue.cpp
    )
    target_include_directories(forkmesh-local-control-mergequeue-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-mergequeue-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-mergequeue-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-mergequeue-tests
        COMMAND forkmesh-local-control-mergequeue-tests)
    qt_add_executable(forkmesh-local-control-terminals-tests
        tests/test_local_control_terminals.cpp
        src/LocalControlTerminals.h
        src/LocalControlTerminals.cpp
    )
    target_include_directories(forkmesh-local-control-terminals-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-terminals-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-terminals-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-terminals-tests
        COMMAND forkmesh-local-control-terminals-tests)
    qt_add_executable(forkmesh-claude-account-routing-tests
        tests/test_claude_account_routing.cpp
        src/ClaudeAccountRouting.h
        src/ClaudeAccountRouting.cpp
    )
    target_include_directories(forkmesh-claude-account-routing-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-claude-account-routing-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-claude-account-routing-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-claude-account-routing-tests
        COMMAND forkmesh-claude-account-routing-tests)
    qt_add_executable(forkmesh-local-control-remote-tests
        tests/test_local_control_remote.cpp
        src/LocalControlFleet.h
        src/LocalControlFleet.cpp
        src/LocalControlFiles.h
        src/LocalControlFiles.cpp
        src/LocalControlDiagnostics.h
        src/LocalControlDiagnostics.cpp
    )
    target_include_directories(forkmesh-local-control-remote-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-remote-tests PRIVATE
        Qt6::Core Qt6::Network)
    if(APPLE)
        set_target_properties(forkmesh-local-control-remote-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-remote-tests
        COMMAND forkmesh-local-control-remote-tests)
    set_tests_properties(forkmesh-local-control-remote-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(forkmesh-local-control-teammates-tests
        tests/test_local_control_teammates.cpp
        src/LocalControlTeammates.h
        src/LocalControlTeammates.cpp
        src/LocalControlInsights.h
        src/LocalControlInsights.cpp
        src/TeammateStore.h
        src/TeammateStore.cpp
    )
    target_include_directories(forkmesh-local-control-teammates-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-teammates-tests PRIVATE
        Qt6::Core Qt6::Network)
    if(APPLE)
        set_target_properties(forkmesh-local-control-teammates-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-teammates-tests
        COMMAND forkmesh-local-control-teammates-tests)
    set_tests_properties(forkmesh-local-control-teammates-tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    qt_add_executable(forkmesh-local-control-wallet-tests
        tests/test_local_control_wallet.cpp
        src/LocalControlWallet.h
        src/LocalControlWallet.cpp
        src/LocalControlHosts.h
        src/LocalControlHosts.cpp
    )
    target_include_directories(forkmesh-local-control-wallet-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-wallet-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-wallet-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-wallet-tests
        COMMAND forkmesh-local-control-wallet-tests)

    qt_add_executable(forkmesh-local-control-secrets-tests
        tests/test_local_control_secrets.cpp
        src/LocalControlSecrets.h
        src/LocalControlSecrets.cpp
    )
    target_include_directories(forkmesh-local-control-secrets-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-secrets-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-secrets-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-secrets-tests
        COMMAND forkmesh-local-control-secrets-tests)
    qt_add_executable(forkmesh-local-control-discord-tests
        tests/test_local_control_discord.cpp
        src/LocalControlDiscord.h
        src/LocalControlDiscord.cpp
        src/LocalControlHosts.h
        src/LocalControlHosts.cpp
    )
    target_include_directories(forkmesh-local-control-discord-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-discord-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-discord-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-discord-tests
        COMMAND forkmesh-local-control-discord-tests)

    qt_add_executable(forkmesh-desktop-commands-tests
        tests/test_desktop_commands.cpp
        src/DesktopCommands.h
        src/DesktopCommands.cpp
    )
    target_include_directories(forkmesh-desktop-commands-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-desktop-commands-tests PRIVATE
        Qt6::Core OpenSSL::Crypto)
    if(APPLE)
        set_target_properties(forkmesh-desktop-commands-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-desktop-commands-tests
        COMMAND forkmesh-desktop-commands-tests)

    qt_add_executable(forkmesh-local-control-stats-tests
        tests/test_local_control_stats.cpp
        src/LocalControlStats.h
        src/LocalControlStats.cpp
        src/LocalControlHosts.h
        src/LocalControlHosts.cpp
    )
    target_include_directories(forkmesh-local-control-stats-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-stats-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-stats-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-stats-tests
        COMMAND forkmesh-local-control-stats-tests)

    qt_add_executable(forkmesh-local-control-update-tests
        tests/test_local_control_update.cpp
        src/LocalControlUpdate.h
        src/LocalControlUpdate.cpp
    )
    target_include_directories(forkmesh-local-control-update-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-update-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-update-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-update-tests
        COMMAND forkmesh-local-control-update-tests)

    qt_add_executable(forkmesh-local-control-repos-tests
        tests/test_local_control_repos.cpp
        src/LocalControlRepos.h
        src/LocalControlRepos.cpp
        src/LocalControlHosts.h
        src/LocalControlHosts.cpp
    )
    target_include_directories(forkmesh-local-control-repos-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(forkmesh-local-control-repos-tests PRIVATE Qt6::Core)
    if(APPLE)
        set_target_properties(forkmesh-local-control-repos-tests PROPERTIES
            MACOSX_BUNDLE FALSE)
    endif()
    add_test(NAME forkmesh-local-control-repos-tests
        COMMAND forkmesh-local-control-repos-tests)
endif()
