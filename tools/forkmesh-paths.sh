# shellcheck shell=bash
# Where ForkMesh keeps shared files, for shell scripts. Source this file.
#
# The shell twin of tools/forkmesh_paths.py and desktop/src/AppPaths.cpp for
# the home project's shared folders:
#
#   forkmesh_shared_dir <name-in-layout> <legacy-path>
#
# prints ~/projects/forkmesh/<name> when that exists, the legacy path while it
# is still the one in use, and the layout path for a fresh box whose
# ~/projects/forkmesh is ready to hold it. A home without ~/projects/forkmesh
# (a CI sandbox, a fresh container) keeps the legacy path, so workflow caches
# keyed on it keep matching.

forkmesh_home_dir() {
  printf '%s\n' "${FORKMESH_HOME:-${FORKMESH_PROJECTS_DIR:-$HOME/projects}/forkmesh}"
}

forkmesh_shared_dir() {
  local name="$1" legacy="$2" home_dir
  home_dir="$(forkmesh_home_dir)"
  if [ -e "$home_dir/$name" ]; then
    printf '%s\n' "$home_dir/$name"
  elif [ -e "$legacy" ] && [ ! -L "$legacy" ]; then
    printf '%s\n' "$legacy"
  elif [ -d "$home_dir" ] && [ ! -e "$home_dir/.git" ]; then
    printf '%s\n' "$home_dir/$name"
  else
    printf '%s\n' "$legacy"
  fi
}

# The ForkMesh home project's managed ssh folder (fleet key, host CA).
forkmesh_ssh_dir() {
  forkmesh_shared_dir ssh "${XDG_DATA_HOME:-$HOME/.local/share}/ForkMesh/ForkMesh/ssh"
}

forkmesh_toolchain_dir() {
  forkmesh_shared_dir toolchain "$HOME/.forkmesh/toolchain"
}

forkmesh_tool_logs_dir() {
  forkmesh_shared_dir logs/tools "$HOME/.forkmesh/logs"
}
