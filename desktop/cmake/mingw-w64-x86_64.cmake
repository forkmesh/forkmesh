# CMake toolchain for cross-compiling the ForkMesh desktop client to Windows
# x86_64 from Linux with MinGW-w64 (GCC, POSIX threads) and an official Qt
# "win64_mingw" kit. tools/package/windows-toolchain.sh provisions both
# rootlessly and exports the variables read below:
#
#   FORKMESH_WIN_MINGW_ROOT   sysroot holding usr/bin/x86_64-w64-mingw32-g++-posix
#                             (Debian's g++-mingw-w64-x86-64-posix, extracted
#                             with dpkg-deb -x; the compiler is relocatable)
#   FORKMESH_WIN_QT_ROOT      the Qt kit prefix (…/Qt/<ver>/mingw_64)
#   FORKMESH_WIN_OPENSSL_ROOT an OpenSSL >= 3.5 prefix cross-built for MinGW
#                             (…/openssl-<ver>, from install-windows-toolchain.sh)
#   QT_HOST_PATH              the host Qt prefix providing moc/rcc/uic (/usr)
#
# Usage: cmake -S desktop -B build-win -DCMAKE_TOOLCHAIN_FILE=desktop/cmake/mingw-w64-x86_64.cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(_fm_triplet x86_64-w64-mingw32)
set(_fm_mingw_root "$ENV{FORKMESH_WIN_MINGW_ROOT}")
if(_fm_mingw_root)
    set(_fm_bin "${_fm_mingw_root}/usr/bin")
    set(CMAKE_C_COMPILER "${_fm_bin}/${_fm_triplet}-gcc-posix")
    set(CMAKE_CXX_COMPILER "${_fm_bin}/${_fm_triplet}-g++-posix")
    set(CMAKE_RC_COMPILER "${_fm_bin}/${_fm_triplet}-windres")
    set(CMAKE_AR "${_fm_bin}/${_fm_triplet}-ar")
    set(CMAKE_RANLIB "${_fm_bin}/${_fm_triplet}-ranlib")
    set(CMAKE_STRIP "${_fm_bin}/${_fm_triplet}-strip")
    # A relocated Debian MinGW toolchain still resolves cc1plus and its
    # headers relative to the driver, but the target sysroot must be named.
    set(CMAKE_SYSROOT "${_fm_mingw_root}/usr/${_fm_triplet}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${_fm_mingw_root}/usr/${_fm_triplet}")
else()
    set(CMAKE_C_COMPILER ${_fm_triplet}-gcc-posix)
    set(CMAKE_CXX_COMPILER ${_fm_triplet}-g++-posix)
    set(CMAKE_RC_COMPILER ${_fm_triplet}-windres)
endif()

string(APPEND CMAKE_CXX_FLAGS_INIT " -include \"${CMAKE_CURRENT_LIST_DIR}/mingw-no-environ-macro.h\"")

set(_fm_qt_root "$ENV{FORKMESH_WIN_QT_ROOT}")
if(_fm_qt_root)
    list(APPEND CMAKE_FIND_ROOT_PATH "${_fm_qt_root}")
    list(APPEND CMAKE_PREFIX_PATH "${_fm_qt_root}")
endif()
set(_fm_openssl_root "$ENV{FORKMESH_WIN_OPENSSL_ROOT}")
if(_fm_openssl_root)
    set(OPENSSL_ROOT_DIR "${_fm_openssl_root}" CACHE PATH "OpenSSL for the Windows target")
    list(APPEND CMAKE_FIND_ROOT_PATH "${_fm_openssl_root}")
endif()
if(NOT "$ENV{QT_HOST_PATH}" STREQUAL "")
    set(QT_HOST_PATH "$ENV{QT_HOST_PATH}" CACHE PATH "Host Qt providing moc/rcc/uic")
endif()
# Qt looks for Qt6HostInfo under PATHS "${QT_HOST_PATH}" "${QT_HOST_PATH_CMAKE_DIR}"
# with NO_DEFAULT_PATH. The prefix alone only reaches <prefix>/lib/<arch>/cmake
# when CMAKE_LIBRARY_ARCHITECTURE matches, and here it names the Windows target,
# so Debian's /usr/lib/x86_64-linux-gnu/cmake is missed and Qt falls back to the
# kit's Windows moc/rcc/uic — which cannot run on this host.
if(NOT "$ENV{QT_HOST_PATH_CMAKE_DIR}" STREQUAL "")
    set(QT_HOST_PATH_CMAKE_DIR "$ENV{QT_HOST_PATH_CMAKE_DIR}" CACHE PATH
        "Directory holding the host Qt6HostInfo package")
endif()

# Host programs (python3, git, ccache) come from the build machine; libraries
# and headers only from the Windows sysroots above.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config would otherwise report the build machine's Linux libraries (for
# example libnghttp3, which turns on HTTP/3 once OpenSSL is 3.2+). Only the
# Windows prefixes above may answer.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
if(_fm_openssl_root)
    set(ENV{PKG_CONFIG_LIBDIR} "${_fm_openssl_root}/lib/pkgconfig")
else()
    set(ENV{PKG_CONFIG_LIBDIR} "/nonexistent")
endif()
