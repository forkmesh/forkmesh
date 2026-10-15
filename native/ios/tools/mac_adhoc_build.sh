#!/usr/bin/env bash
set -euo pipefail

mkdir -p "$3"
steps="$(cd "$1" && pwd)"
tree="$(cd "$2" && pwd)"
export CM_BUILD_DIR="$(cd "$3" && pwd)"
through="${4:-}"
tools="$tree/native/ios/tools"
if [ ! -s "$steps/index.tsv" ]; then
  echo "ERROR: $steps/index.tsv is missing or empty." >&2
  exit 1
fi
if [ -n "$through" ] && ! cut -f2 "$steps/index.tsv" | grep -Fxq -- "$through"; then
  echo "ERROR: no step named \"$through\" in $steps/index.tsv." >&2
  exit 1
fi

asc="$HOME/.asc"
for file in key_id issuer AuthKey.p8; do
  if [ ! -s "$asc/$file" ]; then
    echo "ERROR: $asc/$file is missing on the build Mac (docs/native/MAC_BUILD.md)." >&2
    exit 1
  fi
done

. "$steps/env.sh"
export ASC_KEY_ID="$(cat "$asc/key_id")"
export ASC_ISSUER_ID="$(cat "$asc/issuer")"
export ASC_API_KEY_P8="$(cat "$asc/AuthKey.p8")"
export GEM_HOME="$HOME/.gems-xcodeproj" GEM_PATH="$HOME/.gems-xcodeproj"
export PATH="$GEM_HOME/bin:$PATH"
export FORKMESH_ADHOC_BUILD_FILE="$HOME/ci/ios-adhoc-build"
export FORKMESH_CHANGELOG_DRAFT="${FORKMESH_CHANGELOG_DRAFT:-$steps/changelog.json}"
export FORKMESH_NATIVE_COMPILE_CACHE="${FORKMESH_NATIVE_COMPILE_CACHE:-1}"

export CM_ENV="$steps/cm_env"
touch "$CM_ENV"
export CM_COMMIT="${CM_COMMIT:-$(git -C "$tree" rev-parse HEAD 2>/dev/null || true)}"
load_cm_env() {
  local line
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      [A-Za-z_]*=*) export "$line" ;;
    esac
  done < "$CM_ENV"
}

done_file="$steps/done"
completed="$(cat "$done_file" 2>/dev/null || echo 0)"
if [ "$completed" = "0" ]; then
  find "$CM_BUILD_DIR" -mindepth 1 -delete
fi

keychain="$HOME/Library/Keychains/build.keychain-db"
if [ ! -f "$keychain" ]; then
  security create-keychain -p "" build.keychain
  security set-keychain-settings build.keychain
fi
security list-keychains -d user -s build.keychain
security default-keychain -d user -s build.keychain
security unlock-keychain -p "" build.keychain

killtree() {
  local child
  for child in $(pgrep -P "$1"); do killtree "$child"; done
  kill "$1" 2>/dev/null || true
}

sweep_orphans() {
  local pid ppid elapsed command name
  ps -ww -U "$(id -u)" -o pid=,ppid=,etime=,args= | while read -r pid ppid elapsed command _; do
    [ "$ppid" = 1 ] || continue
    name="${command##*/}"
    case " xctest xcodebuild swift-build swift-test swift-frontend swift-driver " in
      *" $name "*)
        echo "Killing $name (pid $pid, running $elapsed) left over from an earlier build"
        kill -9 "$pid" 2>/dev/null || true
        ;;
    esac
  done
}

if [ "$completed" = "0" ]; then
  sweep_orphans
fi

follow_logs() {
  local seen=" " log
  for log in "$CM_BUILD_DIR"/*.log; do
    [ -f "$log" ] && seen="$seen$log "
  done
  while :; do
    for log in "$CM_BUILD_DIR"/*.log; do
      [ -f "$log" ] || continue
      case "$seen" in *" $log "*) continue ;; esac
      seen="$seen$log "
      echo "--- live: ${log##*/}"
      tail -n +1 -F "$log" 2>/dev/null | awk -f "$tools/xcodebuild_live.awk" &
    done
    sleep 2
  done
}

follow_logs 2>/dev/null &
follower=$!
trap 'sleep 2; killtree "$follower"' EXIT

if [ "$completed" = "0" ]; then
  sdk="$(xcrun --sdk iphoneos --show-sdk-version)"
  if ! xcrun simctl runtime list 2>/dev/null | grep -q "iOS $sdk"; then
    echo "==> iOS $sdk platform ($(date +%H:%M:%S))"
    xcodebuild -downloadPlatform iOS > "$CM_BUILD_DIR/ios-platform.log" 2>&1 || {
      tail -n 20 "$CM_BUILD_DIR/ios-platform.log" >&2
      exit 1
    }
  fi
fi

cd "$tree/native/ios"
echo "--- build Mac: macOS $(sw_vers -productVersion 2>/dev/null || echo '?'), $(xcodebuild -version 2>/dev/null | head -n 1 || echo 'no Xcode'), $(df -h "$HOME" 2>/dev/null | awk 'NR == 2 { print $4 }') free, commit ${CM_COMMIT:0:12}, step $((completed + 1)) of $(grep -c . "$steps/index.tsv")"
index=0
ran=0
build_started=$SECONDS
while IFS=$'\t' read -r file name; do
  [ -n "$file" ] || continue
  index=$((index + 1))
  [ "$index" -le "$completed" ] && continue
  echo "==> $name ($(date +%H:%M:%S))"
  load_cm_env
  step_started=$SECONDS
  if ! bash "$steps/$file" < /dev/null; then
    echo "!! $name failed after $((SECONDS - step_started)) s" >&2
    exit 1
  fi
  echo "<== $name ok in $((SECONDS - step_started)) s"
  echo "$index" > "$done_file"
  ran=1
  [ "$name" = "$through" ] && break
done < "$steps/index.tsv"
if [ "$ran" = 1 ] && [ "$(cat "$done_file" 2>/dev/null || echo 0)" = "$(grep -c . "$steps/index.tsv")" ]; then
  echo "==> done ($(date +%H:%M:%S), $((SECONDS - build_started)) s)"
fi
