#!/usr/bin/env bash
#
# install.sh — register ForkMesh with the desktop (GNOME, KDE, any XDG desktop)
# so it shows the ForkMesh logo in the dock/overview and can be pinned.
#
# This is a USER-level install: it writes only under $HOME (no root, no sudo).
# The editable checkout and installed running copy are deliberately separate:
# the .desktop launcher points at ~/.local/bin/forkmesh, never at build output
# inside the working copy. Rebuild-only developer actions can still run the
# checkout binary directly; Update & restart owns the installed copy.
#
# Re-run it any time; it overwrites its own files and refreshes the icon cache.
#
# Usage:  ./install.sh                    # build if needed, then install
#         ./install.sh --uninstall        # remove installed executables/integration
#         ./install.sh --uninstall --purge # also erase ALL user data
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
BIN="$BUILD_DIR/forkmesh"
MIRROR_NODE_SOURCE="$SCRIPT_DIR/../server"
USER_BIN_DIR="$HOME/.local/bin"
CLIENT_BIN="$USER_BIN_DIR/forkmesh"
MIRROR_NODE_BIN="$USER_BIN_DIR/forkmesh-mirror-node"

# Run a command under a hard time limit when coreutils `timeout` is available, so
# a wedged desktop tool (a slow `gtk-update-icon-cache -f` regen, an ImageMagick
# delegate stuck waiting on a missing helper) can never freeze the installer.
# Falls back to running the command directly when `timeout` is absent.
TIMEOUT_BIN=""
if command -v timeout >/dev/null 2>&1; then TIMEOUT_BIN="timeout"; fi
bounded() { # bounded <seconds> <cmd> [args...]
    local secs="$1"; shift
    if [[ -n "$TIMEOUT_BIN" ]]; then
        "$TIMEOUT_BIN" "$secs" "$@"
    else
        "$@"
    fi
}

# XDG user dirs (honour overrides; fall back to the spec defaults).
DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
APPS_DIR="$DATA_HOME/applications"
ICON_BASE="$DATA_HOME/icons/hicolor"
DESKTOP_ID="forkmesh"                 # must match setDesktopFileName() in main.cpp
DESKTOP_FILE="$APPS_DIR/$DESKTOP_ID.desktop"
NAUTILUS_SCRIPT="$DATA_HOME/nautilus/scripts/Open with ForkMesh"
KDE_SERVICE_MENU="$DATA_HOME/kio/servicemenus/$DESKTOP_ID-open.desktop"
NEMO_ACTION="$DATA_HOME/nemo/actions/$DESKTOP_ID-open.nemo_action"

uninstall() {
    echo "Removing ForkMesh desktop integration..."
    rm -f "$DESKTOP_FILE" "$NAUTILUS_SCRIPT" "$KDE_SERVICE_MENU" "$NEMO_ACTION"
    find "$ICON_BASE" \( -name "$DESKTOP_ID.png" -o -name "$DESKTOP_ID.svg" \) -delete 2>/dev/null || true
    rm -f "$DATA_HOME/icons/$DESKTOP_ID.png"
    rm -f "${XDG_CONFIG_HOME:-$HOME/.config}/autostart/$DESKTOP_ID.desktop"
    rm -f "$CLIENT_BIN"
    rm -f "$MIRROR_NODE_BIN"
    refresh_caches

    # With --purge, also erase every byte of user data so a reinstall starts
    # clean (no leftover node name, identity key, mirrored repos, or chat). The
    # QSettings org+app are both "ForkMesh", so config/data/cache live under a
    # capitalised "ForkMesh" directory.
    if [[ "$PURGE" == "1" ]]; then
        if [[ -n "${CLAUDECODE:-}${CLAUDE_CODE_ENTRYPOINT:-}${FORKMESH_AGENT_PROVIDER:-}${CODEX_SANDBOX:-}${CODEX_SANDBOX_NETWORK_DISABLED:-}" ]]; then
            echo "Refusing: this is an AI agent shell, and --purge erases ForkMesh user data that cannot be recovered. Ask the user to run it in their own terminal." >&2
            exit 1
        fi
        echo "Erasing all ForkMesh user data (--purge)…"
        local config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
        local cache_home="${XDG_CACHE_HOME:-$HOME/.cache}"
        local d
        # The projects layout keeps the same things in ~/projects/forkmesh;
        # the project folder itself stays, since it also holds src/.
        local layout_home="${FORKMESH_HOME:-${FORKMESH_PROJECTS_DIR:-$HOME/projects}/forkmesh}"
        for d in "$config_home/ForkMesh" "$DATA_HOME/ForkMesh" \
                 "$cache_home/ForkMesh" "$HOME/.forkmesh" \
                 "$HOME/.local/share/forkmesh" \
                 "$layout_home/data" "$layout_home/config" "$layout_home/ssh" \
                 "$layout_home/state" "$layout_home/logs" "$layout_home/cache"; do
            [[ -e "$d" ]] || continue
            case "$d" in
                "$HOME"/?*) ;;
                *) echo "  kept $d (outside HOME)"; continue ;;
            esac
            if [[ -e "$d/.git" ]] || compgen -G "$d/*/.git" >/dev/null; then
                echo "  kept $d (it holds a git checkout)"
                continue
            fi
            rm -rf -- "$d" && echo "  removed $d"
        done
        echo "All user data removed."
    else
        echo "Done. (The build directory and your data were left untouched.)"
        echo "Re-run with --purge to also erase settings, repos, and chat history."
    fi
}

refresh_caches() {
    # Best-effort: these tools may be absent on minimal systems, and a stale
    # cache only delays the icon by a login cycle — never fail the install on it.
    # Each is bounded so a slow/hung cache regen can't freeze the installer (the
    # classic cause of the install appearing to hang at this step).
    echo "Refreshing icon cache…"
    command -v gtk-update-icon-cache >/dev/null 2>&1 \
        && bounded 60 gtk-update-icon-cache -f -t "$ICON_BASE" >/dev/null 2>&1 || true
    echo "Refreshing desktop database…"
    command -v update-desktop-database >/dev/null 2>&1 \
        && bounded 30 update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
    echo "Cache refresh done."
}

# Parse args: --uninstall [--purge] (order-independent).
PURGE=0
DO_UNINSTALL=0
for arg in "$@"; do
    case "$arg" in
        --uninstall|-u) DO_UNINSTALL=1 ;;
        --purge)        PURGE=1 ;;
    esac
done
if [[ "$DO_UNINSTALL" == "1" ]]; then
    uninstall
    exit 0
fi

# --- 1. Make sure a binary exists -------------------------------------------
if [[ ! -x "$BIN" ]]; then
    echo "No build found at $BIN — building..."
    if ! command -v cmake >/dev/null 2>&1; then
        echo "error: cmake is not installed; cannot build ForkMesh." >&2
        exit 1
    fi
    BUILD_GENERATOR=()
    command -v ninja >/dev/null 2>&1 && BUILD_GENERATOR=(-G Ninja)
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" "${BUILD_GENERATOR[@]}" -DCMAKE_BUILD_TYPE=Release
    # These translation units can use ~1.6 GiB each. A source install must not
    # turn every host core into a compiler and starve the desktop or installer:
    # budget 2 GiB a job from the memory left after a 6 GiB reserve, at most
    # twelve (the same figures as run.sh and the desktop's own rebuild).
    BUILD_JOBS="$(nproc 2>/dev/null || echo 2)"
    [[ "$BUILD_JOBS" -gt 12 ]] && BUILD_JOBS=12
    if [[ -r /proc/meminfo ]]; then
        BUILD_TOTAL_KB="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
        BUILD_AVAILABLE_KB="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
        BUILD_RESERVE_KB=$((6 * 1024 * 1024))
        if [[ "${BUILD_TOTAL_KB:-0}" -gt 0 && "$BUILD_RESERVE_KB" -gt $((BUILD_TOTAL_KB / 2)) ]]; then
            BUILD_RESERVE_KB=$((BUILD_TOTAL_KB / 2))
        fi
        if [[ "${BUILD_TOTAL_KB:-0}" -gt 0 ]]; then
            BUILD_TOTAL_JOBS=$(((BUILD_TOTAL_KB - BUILD_RESERVE_KB) / (2 * 1024 * 1024)))
            [[ "$BUILD_TOTAL_JOBS" -lt 1 ]] && BUILD_TOTAL_JOBS=1
            [[ "$BUILD_TOTAL_JOBS" -lt "$BUILD_JOBS" ]] && BUILD_JOBS="$BUILD_TOTAL_JOBS"
        fi
        BUILD_USABLE_KB=$((BUILD_AVAILABLE_KB - BUILD_RESERVE_KB))
        [[ "$BUILD_USABLE_KB" -lt 0 ]] && BUILD_USABLE_KB=0
        BUILD_MEMORY_JOBS=$((BUILD_USABLE_KB / (2 * 1024 * 1024)))
        [[ "$BUILD_MEMORY_JOBS" -lt 1 ]] && BUILD_MEMORY_JOBS=1
        [[ "$BUILD_MEMORY_JOBS" -lt "$BUILD_JOBS" ]] && BUILD_JOBS="$BUILD_MEMORY_JOBS"
    fi
    cmake --build "$BUILD_DIR" --target forkmesh -j"$BUILD_JOBS"
fi
[[ -x "$BIN" ]] || { echo "error: build did not produce $BIN" >&2; exit 1; }

# Install through a temporary sibling and atomic rename so an interrupted
# install never truncates the runnable copy. The source-tree build stays intact.
mkdir -p "$USER_BIN_DIR"
CLIENT_BIN_TMP="$(mktemp "$USER_BIN_DIR/.forkmesh.XXXXXX")"
trap 'rm -f "${CLIENT_BIN_TMP:-}"' EXIT
install -m 0755 "$BIN" "$CLIENT_BIN_TMP"
mv -f "$CLIENT_BIN_TMP" "$CLIENT_BIN"
CLIENT_BIN_TMP=""
trap - EXIT

# The Qt client manages a tiny Go mirror-node supervisor for every source-of-
# truth desktop. Install it beside the user's other local executables so the
# card and companion window can start it without root privileges. Release
# bundles may ship a prebuilt binary; source installs build the pinned module.
echo "Installing the Go mirror-node supervisor…"
mkdir -p "$USER_BIN_DIR"
if [[ -x "$MIRROR_NODE_SOURCE/forkmesh-mirror-node" ]]; then
    install -m 0755 "$MIRROR_NODE_SOURCE/forkmesh-mirror-node" "$MIRROR_NODE_BIN"
elif command -v go >/dev/null 2>&1; then
    MIRROR_NODE_TMP="$(mktemp "$USER_BIN_DIR/.forkmesh-mirror-node.XXXXXX")"
    trap 'rm -f "${MIRROR_NODE_TMP:-}"' EXIT
    (
        cd "$MIRROR_NODE_SOURCE"
        go build -trimpath -ldflags="-s -w" \
            -o "$MIRROR_NODE_TMP" ./cmd/forkmesh-mirror-node
    )
    chmod 0755 "$MIRROR_NODE_TMP"
    mv -f "$MIRROR_NODE_TMP" "$MIRROR_NODE_BIN"
    MIRROR_NODE_TMP=""
    trap - EXIT
else
    echo "error: Go is required to install forkmesh-mirror-node from source." >&2
    echo "Install Go, or place a prebuilt forkmesh-mirror-node in $MIRROR_NODE_SOURCE." >&2
    exit 1
fi
echo "  mirror node: $MIRROR_NODE_BIN"

MODULE_DIR="$HOME/.local/lib/forkmesh/packages"
if compgen -G "$BUILD_DIR/packages/*.json" > /dev/null; then
    echo "Installing desktop package modules…"
    mkdir -p "$MODULE_DIR"
    for module in "$BUILD_DIR"/packages/*.json "$BUILD_DIR"/packages/*.so; do
        [[ -f "$module" ]] || continue
        MODULE_TMP="$(mktemp "$MODULE_DIR/.module.XXXXXX")"
        install -m 0644 "$module" "$MODULE_TMP"
        mv -f "$MODULE_TMP" "$MODULE_DIR/$(basename "$module")"
    done
    for module in "$MODULE_DIR"/*.json "$MODULE_DIR"/*.so; do
        [[ -f "$module" ]] || continue
        [[ -e "$BUILD_DIR/packages/$(basename "$module")" ]] || rm -f "$module"
    done
    echo "  modules: $MODULE_DIR"
fi

# --- 2. Install the icon into the hicolor theme -----------------------------
# The icon name "forkmesh" is what the .desktop Icon= key and the Wayland app-id
# resolve against. Prefer ImageMagick to render exact-size buckets (sharp at
# every dock size); otherwise drop the source PNGs into their nearest correct
# bucket, which GNOME will scale.
SRC_1024="$SCRIPT_DIR/resources/icons/forkmesh.png"       # 1024x1024
SRC_256="$SCRIPT_DIR/resources/icons/forkmesh-256.png"    # 256x256
SRC_SVG="$SCRIPT_DIR/resources/icons/forkmesh.svg"        # scalable
[[ -f "$SRC_1024" ]] || { echo "error: missing $SRC_1024" >&2; exit 1; }

MAGICK=""
command -v magick      >/dev/null 2>&1 && MAGICK="magick"
[[ -z "$MAGICK" ]] && command -v convert >/dev/null 2>&1 && MAGICK="convert"

install_icon() { # <size>
    local size="$1"
    local dir="$ICON_BASE/${size}x${size}/apps"
    mkdir -p "$dir"
    if [[ -n "$MAGICK" ]]; then
        bounded 30 "$MAGICK" "$SRC_1024" -resize "${size}x${size}" "$dir/$DESKTOP_ID.png"
    elif [[ "$size" == "256" && -f "$SRC_256" ]]; then
        cp "$SRC_256" "$dir/$DESKTOP_ID.png"      # exact size, no scaling
    fi
}

if [[ -n "$MAGICK" ]]; then
    echo "Rendering icon sizes with $MAGICK…"
    for s in 16 32 48 64 128 256 512; do
        echo "  • ${s}x${s}"
        install_icon "$s" || echo "note: failed to render ${s}px icon (continuing)" >&2
    done
else
    echo "note: ImageMagick not found — installing the 256px icon only." >&2
    install_icon 256
    # 1024px source as a generic fallback so something always resolves.
    mkdir -p "$DATA_HOME/icons"
    cp "$SRC_1024" "$DATA_HOME/icons/$DESKTOP_ID.png"
fi

if [[ -f "$SRC_SVG" ]]; then
    mkdir -p "$ICON_BASE/scalable/apps"
    cp "$SRC_SVG" "$ICON_BASE/scalable/apps/$DESKTOP_ID.svg"
fi

# --- 3. Write the desktop entry ---------------------------------------------
echo "Writing desktop launcher → $DESKTOP_FILE"
mkdir -p "$APPS_DIR"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=ForkMesh
GenericName=Decentralized Git
Comment=Peer-to-peer Git hosting, chat, issues, and CI
Exec="$CLIENT_BIN" %u
Icon=$DESKTOP_ID
Terminal=false
Categories=Development;RevisionControl;
MimeType=x-scheme-handler/forkmesh;
Keywords=git;fork;mesh;p2p;repository;
StartupNotify=true
StartupWMClass=$DESKTOP_ID
EOF
chmod +x "$DESKTOP_FILE"

echo "Adding \"Open with ForkMesh\" to the file manager's right-click menu…"
if command -v nautilus >/dev/null 2>&1; then
    mkdir -p "$(dirname "$NAUTILUS_SCRIPT")"
    cat > "$NAUTILUS_SCRIPT" <<EOF
#!/bin/sh
target="\${1:-.}"
case "\$target" in /*) ;; *) target="\$PWD/\$target" ;; esac
[ -d "\$target" ] || target="\$(dirname -- "\$target")"
exec "$CLIENT_BIN" "\$target"
EOF
    chmod +x "$NAUTILUS_SCRIPT"
    echo "  nautilus : $NAUTILUS_SCRIPT"
fi
if command -v dolphin >/dev/null 2>&1; then
    mkdir -p "$(dirname "$KDE_SERVICE_MENU")"
    cat > "$KDE_SERVICE_MENU" <<EOF
[Desktop Entry]
Type=Service
MimeType=inode/directory;
X-KDE-ServiceTypes=KonqPopupMenu/Plugin
X-KDE-Priority=TopLevel
Actions=openWithForkMesh;

[Desktop Action openWithForkMesh]
Name=Open with ForkMesh
Icon=$DESKTOP_ID
Exec="$CLIENT_BIN" %u
EOF
    chmod +x "$KDE_SERVICE_MENU"
    echo "  dolphin  : $KDE_SERVICE_MENU"
fi
if command -v nemo >/dev/null 2>&1; then
    mkdir -p "$(dirname "$NEMO_ACTION")"
    cat > "$NEMO_ACTION" <<EOF
[Nemo Action]
Name=Open with ForkMesh
Comment=Open this folder in ForkMesh
Exec="$CLIENT_BIN" %F
Icon-Name=$DESKTOP_ID
Selection=s
Extensions=dir;
EOF
    echo "  nemo     : $NEMO_ACTION"
fi

# --- 4. Refresh caches so GNOME picks it up without a re-login --------------
refresh_caches

echo "ForkMesh installed for the desktop."
echo "  launcher : $DESKTOP_FILE"
echo "  runs     : $CLIENT_BIN"
echo
echo "Open the Activities/app grid, search \"ForkMesh\", launch it, then"
echo "right-click its dock icon -> \"Pin to Dash\" (GNOME) to keep it there."
