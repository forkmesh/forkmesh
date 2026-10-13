#!/usr/bin/env bash
# name: Launch native app (Android)
# description: Install the JDK and Android SDK if needed, then build, install and open the Kotlin + Compose ForkMesh app in native/android on a device or its own emulator.
set -euo pipefail

# The shared toolchain folder follows the projects layout. A shortcut run from
# a copy outside the checkout has no helper beside it and keeps the old path.
_fm_paths="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." 2>/dev/null && pwd)/tools/forkmesh-paths.sh"
if [ -r "$_fm_paths" ]; then
  # shellcheck source=tools/forkmesh-paths.sh
  . "$_fm_paths"
  TOOLCHAIN_DIR="$(forkmesh_toolchain_dir)"
  LOGS_DIR="$(forkmesh_tool_logs_dir)"
else
  TOOLCHAIN_DIR="$HOME/.forkmesh/toolchain"
  LOGS_DIR="$HOME/.forkmesh/logs"
fi
readonly TOOLCHAIN_DIR LOGS_DIR
readonly ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
readonly AVD_NAME="forkmesh-native-36"
# The emulator runs Android 36 although the app targets 37: the Android 37
# x86_64 image (r06, emulator 37.1.11) aborts SurfaceFlinger in RegionSampling
# ("hasReadColorBufferDma") every ~20 s on this host even with no app installed.
readonly AVD_SYSTEM_IMAGE="system-images;android-36;google_apis;x86_64"
readonly APP_ID="com.forkmesh.nativeapp"
readonly APP_ROOT="$(cd "$(dirname "$0")/../.." && pwd)/native/android"
readonly EMULATOR_LOG="$LOGS_DIR/native-android-emulator.log"

JAVA_HOME="$TOOLCHAIN_DIR/jdk"

die() {
  echo "$*" >&2
  exit 1
}

# Deletes a scratch directory this script created with mktemp, and nothing
# else: the resolved path must be a direct child of $parent whose name starts
# with $prefix. Anything unexpected is reported and left in place.
remove_scratch_dir() {
  local dir="$1" parent="$2" prefix="$3" resolved expected
  [ -d "$dir" ] || return 0
  resolved="$(readlink -f -- "$dir")"
  expected="$(readlink -f -- "$parent")/$prefix"
  if [ "${resolved#"$expected"}" != "$resolved" ] && [ "$(dirname -- "$resolved")" = "$(readlink -f -- "$parent")" ]; then
    rm -rf -- "$resolved"
  else
    echo "Leaving unexpected path in place: $resolved" >&2
  fi
}

# Builds an unencrypted trust store from the system CA bundle, the way the JDK
# ships cacerts, so the JVM's default (password-less) trust store sees every
# anchor. Only the files it writes are removed.
build_cacerts() {
  local jdk="$1" certs i=0 f
  certs="$(mktemp -d "$TOOLCHAIN_DIR/cacerts-build.XXXXXX")"
  awk -v dir="$certs" '/BEGIN CERTIFICATE/{n++; file=sprintf("%s/cert-%03d.pem", dir, n)} {print > file}' \
    /etc/ssl/certs/ca-certificates.crt
  rm -f -- "$jdk/lib/security/cacerts"
  for f in "$certs"/cert-*.pem; do
    i=$((i + 1))
    "$jdk/bin/keytool" -importcert -noprompt -trustcacerts -alias "cert$i" -file "$f" \
      -keystore "$jdk/lib/security/cacerts" -storepass changeit \
      -J-Dkeystore.pkcs12.certProtectionAlgorithm=NONE \
      -J-Dkeystore.pkcs12.macAlgorithm=NONE >/dev/null 2>&1 || true
    rm -f -- "$f"
  done
  rmdir -- "$certs"
}

# Rootless OpenJDK 21 from the distro's packages. It is assembled in a scratch
# directory and only swapped in once complete; a JDK already at $JAVA_HOME is
# moved aside, never deleted.
install_jdk() {
  echo "Setting up JDK (OpenJDK 21)..." >&2
  mkdir -p "$TOOLCHAIN_DIR"
  local build jvm_dir
  build="$(mktemp -d "$TOOLCHAIN_DIR/jdk-build.XXXXXX")"
  (
    cd "$build"
    apt-get download openjdk-21-jre-headless openjdk-21-jdk-headless >/dev/null 2>&1
    mkdir root
    for deb in ./*.deb; do dpkg -x "$deb" root; done
    find root -type l | while read -r link; do
      target="$(readlink "$link")"
      case "$target" in
        /*) if [ ! -e "$target" ] && [ -e "root$target" ]; then rm -f -- "$link"; cp -a "root$target" "$link"; fi ;;
      esac
    done
  )
  jvm_dir="$(find "$build/root/usr/lib/jvm" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | head -n1)"
  if [ -z "$jvm_dir" ] || [ ! -x "$jvm_dir/bin/javac" ]; then
    remove_scratch_dir "$build" "$TOOLCHAIN_DIR" "jdk-build."
    die "The downloaded JDK is incomplete; nothing was changed."
  fi
  build_cacerts "$jvm_dir"
  if [ -e "$JAVA_HOME" ]; then
    mv -- "$JAVA_HOME" "$TOOLCHAIN_DIR/jdk.previous.$(date +%Y%m%d%H%M%S)"
  fi
  mv -- "$jvm_dir" "$JAVA_HOME"
  remove_scratch_dir "$build" "$TOOLCHAIN_DIR" "jdk-build."
  echo "✓ JDK installed" >&2
}

[ -x "$JAVA_HOME/bin/javac" ] || install_jdk
export JAVA_HOME PATH="$JAVA_HOME/bin:$PATH"

SDKMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/sdkmanager"
AVDMANAGER="$ANDROID_SDK_ROOT/cmdline-tools/latest/bin/avdmanager"
if [ ! -x "$SDKMANAGER" ]; then
  echo "Setting up Android SDK command-line tools..." >&2
  mkdir -p "$ANDROID_SDK_ROOT/cmdline-tools"
  staging="$(mktemp -d "$ANDROID_SDK_ROOT/cmdline-tools/download.XXXXXX")"
  zip_file="$staging/commandlinetools.zip"
  for attempt in 1 2 3; do
    if wget -q -O "$zip_file" --tries=1 --timeout=90 \
      "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip" && [ -s "$zip_file" ]; then
      break
    fi
    rm -f -- "$zip_file"
    [ "$attempt" = 3 ] && die "Could not download the Android command-line tools."
    sleep 2
  done
  unzip -q "$zip_file" -d "$staging"
  rm -f -- "$zip_file"
  [ -x "$staging/cmdline-tools/bin/sdkmanager" ] || die "The command-line tools archive is incomplete; see $staging"
  if [ -e "$ANDROID_SDK_ROOT/cmdline-tools/latest" ]; then
    mv -- "$ANDROID_SDK_ROOT/cmdline-tools/latest" "$ANDROID_SDK_ROOT/cmdline-tools/latest.previous.$(date +%Y%m%d%H%M%S)"
  fi
  mv -- "$staging/cmdline-tools" "$ANDROID_SDK_ROOT/cmdline-tools/latest"
  rmdir -- "$staging"
fi
export ANDROID_HOME="$ANDROID_SDK_ROOT" ANDROID_SDK_ROOT

missing=()
[ -x "$ANDROID_SDK_ROOT/platform-tools/adb" ] || missing+=("platform-tools")
[ -f "$ANDROID_SDK_ROOT/platforms/android-37.0/android.jar" ] || missing+=("platforms;android-37.0")
[ -d "$ANDROID_SDK_ROOT/build-tools/37.0.0" ] || missing+=("build-tools;37.0.0")
[ -x "$ANDROID_SDK_ROOT/emulator/emulator" ] || missing+=("emulator")
[ -d "$ANDROID_SDK_ROOT/system-images/android-36/google_apis/x86_64" ] || missing+=("$AVD_SYSTEM_IMAGE")
if [ "${#missing[@]}" -gt 0 ]; then
  echo "Installing Android SDK components: ${missing[*]}" >&2
  yes | "$SDKMANAGER" --licenses >/dev/null 2>&1 || true
  "$SDKMANAGER" --install "${missing[@]}" 2>&1 | grep -v '^Warning:' || true
fi

readonly ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
readonly EMULATOR="$ANDROID_SDK_ROOT/emulator/emulator"

printf 'sdk.dir=%s\n' "$ANDROID_SDK_ROOT" > "$APP_ROOT/local.properties"

online_serials() {
  "$ADB" devices 2>/dev/null | awk 'NR > 1 && $2 == "device" {print $1}'
}

# The emulator already running this app's AVD, if any.
avd_serial() {
  local serial
  for serial in $(online_serials); do
    case "$serial" in
      emulator-*)
        if [ "$("$ADB" -s "$serial" emu avd name 2>/dev/null | head -n1 | tr -d '\r')" = "$AVD_NAME" ]; then
          echo "$serial"
          return
        fi
        ;;
    esac
  done
}

readonly AVD_CONFIG="$HOME/.android/avd/$AVD_NAME.avd/config.ini"

# Sets one key in this app's own AVD config, leaving every other line alone.
set_avd_option() {
  local key="$1" value="$2"
  [ -f "$AVD_CONFIG" ] || return 0
  if grep -q "^$key[ =]" "$AVD_CONFIG"; then
    sed -i "s|^$key[ ]*=.*|$key=$value|" "$AVD_CONFIG"
  else
    printf '%s=%s\n' "$key" "$value" >> "$AVD_CONFIG"
  fi
}

# The AVD's data partition in MB (0 when unknown). Values look like 800M, 6G
# or a byte count.
avd_data_partition_mb() {
  local raw number
  raw="$(sed -n 's/^disk.dataPartition.size[ ]*=[ ]*//p' "$AVD_CONFIG" 2>/dev/null | head -n1 | tr -d ' ')"
  number="${raw%[MmGg]}"
  case "$number" in ''|*[!0-9]*) echo 0; return ;; esac
  case "$raw" in
    *[Gg]) echo $((number * 1024)) ;;
    *[Mm]) echo "$number" ;;
    *) echo $((number / 1048576)) ;;
  esac
}

# A console port no emulator (or anything else) is listening on.
free_emulator_port() {
  local port
  for port in $(seq 5584 2 5680); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
      continue
    fi
    if (exec 3<>"/dev/tcp/127.0.0.1/$((port + 1))") 2>/dev/null; then
      continue
    fi
    echo "$port"
    return
  done
  die "No free emulator console port between 5584 and 5680."
}

# Target, in order: $ANDROID_SERIAL, this app's running emulator, a single
# attached physical device. Other emulators are never used or disturbed.
emulator_only=false
[ "${FORKMESH_NATIVE_ANDROID_TARGET:-}" = "emulator" ] && emulator_only=true
serial=""
[ "$emulator_only" = true ] || serial="${ANDROID_SERIAL:-}"
[ -n "$serial" ] || serial="$(avd_serial)"
if [ -z "$serial" ] && [ "$emulator_only" = false ]; then
  physical=()
  while read -r candidate; do
    case "$candidate" in emulator-*) ;; *) physical+=("$candidate") ;; esac
  done < <(online_serials)
  [ "${#physical[@]}" -eq 1 ] && serial="${physical[0]}"
fi

if [ -z "$serial" ]; then
  if ! "$EMULATOR" -list-avds 2>/dev/null | grep -qx "$AVD_NAME"; then
    echo "Creating Android emulator '$AVD_NAME'..." >&2
    echo "no" | "$AVDMANAGER" create avd -n "$AVD_NAME" -k "$AVD_SYSTEM_IMAGE" -d pixel_7 >/dev/null \
      || echo "no" | "$AVDMANAGER" create avd -n "$AVD_NAME" -k "$AVD_SYSTEM_IMAGE" >/dev/null
    # avdmanager's defaults (an 800 MB data partition) cannot hold the Android
    # 37 system data plus the app, and leave Compose short of memory.
    set_avd_option disk.dataPartition.size 6G
    set_avd_option hw.ramSize 3072M
  fi
  data_mb="$(avd_data_partition_mb)"
  if [ "$data_mb" -gt 0 ] && [ "$data_mb" -lt 2048 ]; then
    die "The '$AVD_NAME' emulator has only ${data_mb} MB of storage, too little to install the app.
Recreate it (this erases that emulator's data) and run this shortcut again:
  $AVDMANAGER delete avd -n $AVD_NAME"
  fi
  port="$(free_emulator_port)"
  serial="emulator-$port"
  mkdir -p "$(dirname "$EMULATOR_LOG")"
  echo "Starting the '$AVD_NAME' emulator as $serial..." >&2
  # Its own session, so the emulator outlives this script and whatever runs it.
  # FORKMESH_NATIVE_EMULATOR_ARGS adds flags, e.g. "-no-window" on a headless host.
  launcher=(nohup)
  command -v setsid >/dev/null 2>&1 && launcher=(setsid nohup)
  # shellcheck disable=SC2086
  "${launcher[@]}" "$EMULATOR" -avd "$AVD_NAME" -port "$port" -memory 3072 -no-snapshot-save \
    ${FORKMESH_NATIVE_EMULATOR_ARGS:-} </dev/null >"$EMULATOR_LOG" 2>&1 &
  timeout 300 "$ADB" -s "$serial" wait-for-device || die "The emulator did not appear; see $EMULATOR_LOG"
  booted=false
  for _ in $(seq 1 150); do
    if [ "$("$ADB" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; then
      booted=true
      break
    fi
    sleep 2
  done
  [ "$booted" = true ] || die "The emulator did not finish booting; see $EMULATOR_LOG"
fi

echo "Building and installing on $serial..." >&2
cd "$APP_ROOT"
ANDROID_SERIAL="$serial" ./gradlew --console=plain :app:installDebug

echo "Opening ForkMesh Native..." >&2
"$ADB" -s "$serial" shell am start -n "$APP_ID/.MainActivity" >/dev/null
echo "✓ ForkMesh Native is running on $serial" >&2
