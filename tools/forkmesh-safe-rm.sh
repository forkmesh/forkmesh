# shellcheck shell=bash

fm_safe_rm_tree() {
  local parent="$1" target base name home_dir projects project sub rel first kind root
  local worktree="${FM_SAFE_RM_WORKTREE:-0}"
  shift
  [ -n "$parent" ] && [ "$parent" != "/" ] || { printf 'Refusing to delete: no safe expected parent folder was given\n' >&2; return 64; }
  if [ ! -d "$parent" ]; then
    for target in "$@"; do
      if [ -e "$target" ] || [ -L "$target" ]; then printf 'Refusing to delete: %s is not inside %s\n' "$target" "$parent" >&2; return 64; fi
    done
    return 0
  fi
  parent="$(cd -P -- "$parent" 2>/dev/null && pwd -P)" || { printf 'Refusing to delete: cannot resolve %s\n' "$parent" >&2; return 64; }
  [ "$parent" != "/" ] || { printf 'Refusing to delete: the expected parent folder is /\n' >&2; return 64; }
  home_dir="$(cd -P -- "${HOME:-/}" 2>/dev/null && pwd -P)" || home_dir="/"
  for target in "$@"; do
    [ -n "$target" ] || { printf 'Refusing to delete: the path is empty\n' >&2; return 64; }
    [ -e "$target" ] || [ -L "$target" ] || continue
    name="${target%/}"; name="${name##*/}"
    case "$name" in ""|"."|"..") printf 'Refusing to delete: %s\n' "$target" >&2; return 64 ;; esac
    base="$(cd -P -- "$(dirname -- "$target")" 2>/dev/null && pwd -P)" || { printf 'Refusing to delete: cannot resolve %s\n' "$target" >&2; return 64; }
    target="${base%/}/$name"
    case "$home_dir/" in "$target"/*) printf 'Refusing to delete: %s is HOME or contains it\n' "$target" >&2; return 64 ;; esac
    case "$target" in "$parent"/*) ;; *) printf 'Refusing to delete: %s is not inside %s\n' "$target" "$parent" >&2; return 64 ;; esac
    case "/$target/" in */.git/*) printf 'Refusing to delete: %s is git history (.git)\n' "$target" >&2; return 64 ;; esac
    if [ -d "$target/.git" ] && [ ! -L "$target/.git" ]; then
      printf 'Refusing to delete: %s is a git checkout\n' "$target" >&2; return 64
    fi
    if [ -e "$target/.git" ] && [ "$worktree" != "1" ]; then
      printf 'Refusing to delete: %s is a git worktree\n' "$target" >&2; return 64
    fi
    for projects in "$home_dir/projects" "${FORKMESH_PROJECTS_DIR:-}" "${FORKMESH_HOME:+${FORKMESH_HOME%/*}}"; do
      [ -n "$projects" ] || continue
      if [ -d "$projects" ]; then projects="$(cd -P -- "$projects" && pwd -P)" || continue; fi
      case "$projects/" in "$target"/*) printf 'Refusing to delete: %s contains the projects folder %s\n' "$target" "$projects" >&2; return 64 ;; esac
      case "$target" in "$projects"/*) ;; *) continue ;; esac
      rel="${target#"$projects"/}"
      project="${rel%%/*}"
      [ "$project" != "$rel" ] || { printf 'Refusing to delete: %s is a project folder\n' "$target" >&2; return 64; }
      rel="${rel#*/}"
      first="${rel%%/*}"
      case "$project" in forkmesh-backup) printf 'Refusing to delete: %s is inside the backup project\n' "$target" >&2; return 64 ;; esac
      case "$first" in
        src|ssh) kind=strict ;;
        data|config|state|logs) kind=owned ;;
        bin|builds|cache|release|tmp|toolchain|toolchain-*|worktrees) continue ;;
        *) kind=strict ;;
      esac
      sub="${rel#"$first"}"; sub="${sub#/}"
      [ -n "$sub" ] || { printf 'Refusing to delete: %s is a protected ForkMesh folder\n' "$target" >&2; return 64; }
      case "/$sub/" in
        */build/*|*/build-*|*/build_*|*/builds/*|*/cache/*|*/.cache/*|*/caches/*|*/tmp/*|*/.tmp/*|*/tmp-*|*/tmp.*|*/temp/*|*/worktrees/*|*/.worktrees/*|*/node_modules/*|*/__pycache__/*|*/.pytest_cache/*|*/dist/*|*/out/*|*/.forkmesh/?*/*) continue ;;
      esac
      if [ "$kind" = "owned" ] && [ "${sub#*/}" != "$sub" ]; then continue; fi
      printf 'Refusing to delete: %s is inside protected ForkMesh folder %s/%s/%s\n' "$target" "$projects" "$project" "$first" >&2
      return 64
    done
    for root in "$home_dir/.ssh" "$home_dir/.forkmesh" "$home_dir/.config/ForkMesh" "$home_dir/.local/share/ForkMesh" "$home_dir/.local/share/forkmesh" "${FORKMESH_DATA_DIR:-}" "${FORKMESH_CONFIG_DIR:-}"; do
      [ -n "$root" ] || continue
      case "$root/" in "$target"/*) printf 'Refusing to delete: %s is or contains %s\n' "$target" "$root" >&2; return 64 ;; esac
      case "$target" in "$root"/*/*) ;; "$root"/*) printf 'Refusing to delete: %s is a top-level folder of %s\n' "$target" "$root" >&2; return 64 ;; esac
    done
  done
  for target in "$@"; do
    [ -e "$target" ] || [ -L "$target" ] || continue
    rm -rf -- "$target" || return 1
  done
}
