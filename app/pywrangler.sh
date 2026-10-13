#!/usr/bin/env bash
# Shared pywrangler bootstrap for deploy.sh and migrate.sh.

PYWRANGLER_VENV="${PYWRANGLER_VENV:-.pywrangler}"
PYWRANGLER_BIN="$PYWRANGLER_VENV/bin/pywrangler"
PYWRANGLER_UVX="$PYWRANGLER_VENV/bin/uvx"
PYWRANGLER_UV="$PYWRANGLER_VENV/bin/uv"
WORKERS_PY_SPEC="${WORKERS_PY_SPEC:-workers-py<1.18.0}"
WRANGLER_NPM_SPEC="${WRANGLER_NPM_SPEC:-wrangler@4.136.2}"
# Wrangler 4.x hard-refuses to start on Node < 22 ("Wrangler requires at least
# Node.js v22.0.0"), and it makes that check only after pywrangler has already
# echoed the command it is about to run — so on a host whose /usr/bin/node is
# older, a deploy dies mid-flight behind a version banner instead of a real
# error. Distros keep shipping Node 20 long after Cloudflare drops it, so the
# newer runtime here lives under a version manager whose bin directory is only
# on PATH inside an interactive login shell. Find it ourselves so deploy.sh
# works the same from cron, hooks and non-login shells.
WRANGLER_MIN_NODE_MAJOR="${WRANGLER_MIN_NODE_MAJOR:-22}"

_pywrangler_version_key() {
    # Collapse a "v22.9.1"-style version into a sortable integer so we can pick
    # the newest installed runtime without depending on GNU `sort -V`.
    local version="${1#v}" major minor patch
    IFS=. read -r major minor patch <<< "$version"
    major="${major%%[!0-9]*}"
    minor="${minor%%[!0-9]*}"
    patch="${patch%%[!0-9]*}"
    printf '%d%03d%03d\n' "${major:-0}" "${minor:-0}" "${patch:-0}"
}

_pywrangler_node_version() {
    local version
    version="$("$1" --version 2>/dev/null)" || return 1
    case "$version" in
        v[0-9]*) printf '%s\n' "$version" ;;
        *) return 1 ;;
    esac
}

_pywrangler_node_is_new_enough() {
    local version
    version="$(_pywrangler_node_version "$1")" || return 1
    [ "$(_pywrangler_version_key "$version")" -ge \
      "$(_pywrangler_version_key "$WRANGLER_MIN_NODE_MAJOR.0.0")" ]
}

_pywrangler_find_node() {
    # Print the newest node >= $WRANGLER_MIN_NODE_MAJOR that a version manager
    # has installed but left off PATH. FORKMESH_NODE_BIN overrides the search.
    local candidate best="" best_key=0 key
    for candidate in \
        ${FORKMESH_NODE_BIN:+"$FORKMESH_NODE_BIN"} \
        "${NVM_DIR:-$HOME/.nvm}"/versions/node/*/bin/node \
        "$HOME"/.local/share/fnm/node-versions/*/installation/bin/node \
        "$HOME"/.volta/tools/image/node/*/bin/node \
        /usr/local/n/versions/node/*/bin/node
    do
        [ -x "$candidate" ] || continue
        _pywrangler_node_is_new_enough "$candidate" || continue
        key="$(_pywrangler_version_key "$(_pywrangler_node_version "$candidate")")"
        if [ "$key" -gt "$best_key" ]; then
            best_key="$key"
            best="$candidate"
        fi
    done
    [ -n "$best" ] || return 1
    printf '%s\n' "$best"
}

_pywrangler_prepend_path() {
    # Move $1 to the FRONT of PATH even if it already appears later on — an
    # older /usr/bin/node otherwise keeps winning the lookup.
    local dir="$1" part rebuilt=""
    local -a parts
    IFS=':' read -r -a parts <<< "$PATH"
    for part in ${parts[@]+"${parts[@]}"}; do
        [ "$part" = "$dir" ] && continue
        if [ -z "$rebuilt" ]; then rebuilt="$part"; else rebuilt="$rebuilt:$part"; fi
    done
    PATH="$dir${rebuilt:+:$rebuilt}"
    export PATH
}

_pywrangler_ensure_node() {
    [ "${_PYWRANGLER_NODE_READY:-0}" = "1" ] && return 0
    _PYWRANGLER_NODE_READY=1

    local current current_version=""
    current="$(command -v node 2>/dev/null || true)"
    if [ -n "$current" ]; then
        current_version="$(_pywrangler_node_version "$current" || true)"
        if _pywrangler_node_is_new_enough "$current"; then
            return 0
        fi
    fi

    local found
    found="$(_pywrangler_find_node)" || found=""
    if [ -n "$found" ]; then
        local dir
        dir="$(cd "$(dirname "$found")" && pwd)"
        _pywrangler_prepend_path "$dir"
        echo "note: PATH node was ${current_version:-missing}; using" \
             "$(_pywrangler_node_version "$found") from $dir for wrangler." >&2
        return 0
    fi

    # Not fatal: a FORKMESH_NO_NODE_PACKAGES pipeline never shells out to Node.
    # Everything else is about to fail, so say why while the output is still
    # readable rather than 200 lines into the deploy.
    echo "warning: no Node >= v${WRANGLER_MIN_NODE_MAJOR} found (PATH node is ${current_version:-missing})." >&2
    echo "         Wrangler 4.x refuses to run on older Node and will abort this deploy." >&2
    echo "         Install one (e.g. 'nvm install 22') or point FORKMESH_NODE_BIN at a node binary." >&2
    return 1
}

_pywrangler_ensure_venv_path() {
    case ":$PATH:" in
        *":$PYWRANGLER_VENV/bin:"*) ;;
        *) PATH="$PYWRANGLER_VENV/bin:$PATH" ;;
    esac
}

_pywrangler_install_npx_wrapper() {
    mkdir -p "$PYWRANGLER_VENV/bin"
    cat > "$PYWRANGLER_VENV/bin/npx" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ge 2 ] && [ "$1" = "--yes" ] && [ "$2" = "wrangler" ]; then
    shift 2
    if ! command -v npm >/dev/null 2>&1; then
        echo "error: npm is required because pywrangler delegates deploys to Wrangler." >&2
        exit 1
    fi
    exec npm exec --yes --package "${WRANGLER_NPM_SPEC:-wrangler@4.136.2}" -- wrangler "$@"
fi

self_dir="$(cd "$(dirname "$0")" && pwd)"
old_path="$PATH"
new_path=""
normalized_path=""
IFS=':' read -r -a path_parts <<< "$old_path"
for path_part in "${path_parts[@]}"; do
    # PATH can contain the wrapper directory as a relative entry (the default
    # PYWRANGLER_VENV is `.pywrangler`). Compare canonical directories so the
    # wrapper cannot rediscover and exec itself forever in a clean Actions
    # sandbox.
    case "$path_part" in
        "") normalized_path="$(pwd)" ;;
        /*) normalized_path="$path_part" ;;
        *) normalized_path="$(cd "$path_part" 2>/dev/null && pwd)" || normalized_path="$path_part" ;;
    esac
    [ "$normalized_path" = "$self_dir" ] && continue
    if [ -z "$new_path" ]; then
        new_path="$path_part"
    else
        new_path="$new_path:$path_part"
    fi
done
PATH="$new_path"
real_npx="$(command -v npx || true)"
PATH="$old_path"
if [ -z "$real_npx" ]; then
    echo "error: npx not found on PATH." >&2
    exit 1
fi
exec "$real_npx" "$@"
EOF
    chmod 0755 "$PYWRANGLER_VENV/bin/npx"
}

_pywrangler_executable_is_usable() {
    local executable="$1" first_line interpreter
    [ -x "$executable" ] || return 1
    IFS= read -r first_line < "$executable" || return 1
    case "$first_line" in
        '#!'*)
            interpreter="${first_line#\#!}"
            interpreter="${interpreter%% *}"
            [ -x "$interpreter" ] || return 1
            ;;
    esac
}

pywrangler() {
    _pywrangler_ensure_node || true
    _pywrangler_ensure_venv_path
    _pywrangler_install_npx_wrapper
    local pywrangler_path
    pywrangler_path="$(type -P pywrangler || true)"
    if [ -n "$pywrangler_path" ] && _pywrangler_executable_is_usable "$pywrangler_path"; then
        "$pywrangler_path" "$@"
        return
    elif [ -n "$pywrangler_path" ]; then
        echo "note: ignoring moved or broken pywrangler environment at $pywrangler_path." >&2
    fi

    # Secret inventory and bulk updates are native Wrangler operations. A clean
    # Actions sandbox should not have to download and create an entire
    # workers-py environment before it can preserve an existing Worker's secret
    # set. Keep an explicitly installed pywrangler above as the compatibility
    # choice, but otherwise take the pinned npm Wrangler path directly.
    if [ "${1:-}" = "secret" ]; then
        _pywrangler_run_wrangler "$@"
        return
    fi

    if [ "${FORKMESH_NO_NODE_PACKAGES:-0}" = "1" ]; then
        echo "error: pywrangler is not installed, and this Cloudflare pipeline is configured" >&2
        echo "       with FORKMESH_NO_NODE_PACKAGES=1." >&2
        echo "       Not auto-installing workers-py here: current pywrangler deploy" >&2
        echo "       proxies to 'npx wrangler', which would require Node/npm packages." >&2
        echo "       Install/provide a pywrangler executable in the runner image, or run" >&2
        echo "       without FORKMESH_NO_NODE_PACKAGES if a Node-backed pywrangler is OK." >&2
        return 1
    fi

    local tool_path
    tool_path="$(command -v uvx 2>/dev/null || true)"
    if [ -n "$tool_path" ] && _pywrangler_executable_is_usable "$tool_path"; then
        "$tool_path" --from "$WORKERS_PY_SPEC" pywrangler "$@" && return
    fi
    if _pywrangler_executable_is_usable "$PYWRANGLER_UVX"; then
        "$PYWRANGLER_UVX" --from "$WORKERS_PY_SPEC" pywrangler "$@" && return
    fi
    tool_path="$(command -v uv 2>/dev/null || true)"
    if [ -n "$tool_path" ] && _pywrangler_executable_is_usable "$tool_path"; then
        "$tool_path" tool run --from "$WORKERS_PY_SPEC" pywrangler "$@" && return
    fi
    if _pywrangler_executable_is_usable "$PYWRANGLER_UV"; then
        "$PYWRANGLER_UV" tool run --from "$WORKERS_PY_SPEC" pywrangler "$@" && return
    fi
    if ! _pywrangler_executable_is_usable "$PYWRANGLER_BIN"; then
        if ! install_pywrangler; then
            if _pywrangler_run_wrangler "$@"; then
                return
            fi
            return 1
        fi
    fi
    if _pywrangler_executable_is_usable "$PYWRANGLER_UVX"; then
        "$PYWRANGLER_UVX" --from "$WORKERS_PY_SPEC" pywrangler "$@" && return
    fi
    if _pywrangler_executable_is_usable "$PYWRANGLER_UV"; then
        "$PYWRANGLER_UV" tool run --from "$WORKERS_PY_SPEC" pywrangler "$@" && return
    fi
    if _pywrangler_executable_is_usable "$PYWRANGLER_BIN"; then
        "$PYWRANGLER_BIN" "$@"
        return
    fi
    if _pywrangler_run_wrangler "$@"; then
        return
    fi
    echo "error: workers-py installed, but $PYWRANGLER_BIN was not created." >&2
    return 1
}

_pywrangler_run_wrangler() {
    if command -v wrangler >/dev/null 2>&1; then
        wrangler "$@"
        return $?
    fi
    if command -v npx >/dev/null 2>&1; then
        npx --yes --package "${WRANGLER_NPM_SPEC:-wrangler@4.136.2}" -- wrangler "$@"
        return $?
    fi
    return 1
}

install_pywrangler() {
    if ! command -v python3 >/dev/null 2>&1; then
        echo "error: need python3 to install pywrangler automatically." >&2
        echo "       Install uv (recommended) or install pywrangler manually:" >&2
        echo "       https://docs.astral.sh/uv/getting-started/" >&2
        return 1
    fi

    echo "pywrangler not found; installing workers-py into $PYWRANGLER_VENV ..." >&2
    python3 -m venv --clear "$PYWRANGLER_VENV"
    if ! "$PYWRANGLER_VENV/bin/python" -m pip install --upgrade pip >/dev/null; then
        echo "error: failed to bootstrap pip in $PYWRANGLER_VENV." >&2
        return 1
    fi
    if ! "$PYWRANGLER_VENV/bin/python" -m pip install --upgrade "$WORKERS_PY_SPEC"; then
        echo "error: failed to install $WORKERS_PY_SPEC into $PYWRANGLER_VENV." >&2
        return 1
    fi
    # Newer workers-py binaries can require uv tooling at runtime. Install a
    # project-local uv copy so we can keep the deploy path self-contained when
    # the host only has python3.
    if [ ! -x "$PYWRANGLER_UVX" ] || [ ! -x "$PYWRANGLER_UV" ]; then
        if ! "$PYWRANGLER_VENV/bin/python" -m pip install --upgrade uv >/dev/null; then
            echo "note: unable to install uv in $PYWRANGLER_VENV; wrangler fallback remains available." >&2
        fi
    fi

    # Keep the venv bin on PATH so any worker-installed entrypoint that shells
    # out to uv/uvx can find the project-local copy.
    _pywrangler_ensure_venv_path
    _pywrangler_install_npx_wrapper
}

# Fix PATH at source time, not just inside pywrangler(): deploy.sh also shells
# out to `npm exec ... wrangler deploy` directly for the assets-only split
# Workers, and that path never goes through the pywrangler wrapper.
_pywrangler_ensure_node || true
