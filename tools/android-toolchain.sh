#!/usr/bin/env bash
# Provide a rootless Android SDK and JDK toolchain under
# $HOME/.forkmesh/toolchain (or $FORKMESH_TOOLCHAIN_DIR) for the native Android
# app's Gradle build. Source this file from tools/forkmesh-release-host.py.
#
# Setup failures are reported through variables so callers using `set -e` can
# distinguish an offline runner from a broken toolchain:
#
#   ANDROID_TOOLCHAIN_READY   1 when a usable toolchain is set up, else 0
#   ANDROID_TOOLCHAIN_STATUS  ready | offline | failed. "offline" is a policy
#                             fact about the host (no egress, nothing to fix in
#                             the tree) that a caller can treat as a skip;
#                             "failed" is a real breakage worth a red run.
#   ANDROID_TOOLCHAIN_REASON  why it is not usable (empty when ready)
#   JAVA_HOME, ANDROID_HOME, ANDROID_SDK_ROOT, PATH  exported when ready
#
# Setup and Gradle dependency resolution require egress.
#
# The toolchain directory may outlive the run that fills it (CI keeps it on the
# persistent Actions cache), so every install below only puts a piece where the
# checks look once it is complete: a killed run must not leave a half-written
# SDK that the next run takes for a working one.

# shellcheck source=tools/forkmesh-paths.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/forkmesh-paths.sh"
# shellcheck source=tools/forkmesh-safe-rm.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/forkmesh-safe-rm.sh"
_fm_toolchain_dir="${FORKMESH_TOOLCHAIN_DIR:-$(forkmesh_toolchain_dir)}"

# Try DNS first, then raw TCP for hosts with egress but restricted resolution.
_fm_have_network() {
  if timeout 5 getent ahosts dl.google.com >/dev/null 2>&1; then
    return 0
  fi
  if timeout 5 bash -c 'exec 3<>/dev/tcp/1.1.1.1/443' >/dev/null 2>&1; then
    return 0
  fi
  return 1
}

# Locate a full JDK (javac, not just a JRE — Gradle needs the compiler). A
# system JDK counts: /usr is bind-mounted into the Actions sandbox, so one
# installed on the node is usable there without any download.
_fm_find_jdk() {
  local candidate javac
  for candidate in "${JAVA_HOME:-}" "$_fm_toolchain_dir/jdk"; do
    if [ -n "$candidate" ] && [ -x "$candidate/bin/javac" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  javac="$(command -v javac 2>/dev/null || true)"
  if [ -n "$javac" ]; then
    javac="$(readlink -f "$javac")"
    candidate="$(dirname "$(dirname "$javac")")"
    if [ -x "$candidate/bin/java" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi
  return 1
}

# The Actions sandbox has no configured apt at all -- not even a stale cache,
# but a missing /etc/apt/sources.list and no /var/lib/apt/lists, since the
# sandbox image ships no system package manager setup for the unprivileged
# build user to rely on. A bare `apt-get download` therefore always fails with
# "Unable to locate package", not just on a network hiccup. Point apt at a
# throwaway source list and state directory instead of touching /etc/apt,
# which may not even be writable here. `[trusted=yes]` skips signature
# verification since the sandbox also has no apt keyring to check against;
# that is no weaker than the unchecksummed wget download this script already
# does for the Android command-line tools below.
_fm_apt_download() {
  local build_dir="$1" apt_dir codename
  shift
  apt_dir="$build_dir/apt"
  mkdir -p "$apt_dir/lists/partial" "$apt_dir/archives/partial" || return 1
  codename="$(. /etc/os-release 2>/dev/null && printf '%s' "${VERSION_CODENAME:-}")"
  codename="${codename:-trixie}"
  {
    printf 'deb [trusted=yes] https://deb.debian.org/debian %s main\n' "$codename"
    printf 'deb [trusted=yes] https://deb.debian.org/debian-security %s-security main\n' "$codename"
  } > "$apt_dir/sources.list"
  local apt_opts=(
    -o "Dir::Etc::SourceList=$apt_dir/sources.list"
    -o "Dir::Etc::SourceParts=/dev/null"
    -o "Dir::State=$apt_dir"
    -o "Dir::State::status=/dev/null"
    -o "Dir::Cache=$apt_dir"
  )
  apt-get "${apt_opts[@]}" update -qq || return 1
  apt-get "${apt_opts[@]}" download "$@"
}

_fm_install_jdk() {
  local build_dir="$_fm_toolchain_dir/_jdk_build" jvm_dir deb target candidate
  echo "Setting up JDK (OpenJDK 21)..." >&2
  fm_safe_rm_tree "$_fm_toolchain_dir" "$build_dir"
  mkdir -p "$_fm_toolchain_dir" "$build_dir" || return 1
  (
    cd "$build_dir" || exit 1
    # Failures here are reported, not swallowed: an unreachable archive used to
    # surface only as the step's exit status.
    _fm_apt_download "$build_dir" openjdk-21-jre-headless openjdk-21-jdk-headless || exit 1
    mkdir -p root
    for deb in *.deb; do dpkg -x "$deb" root || exit 1; done
    # Absolute symlinks inside the extracted tree point at paths that only exist
    # once the package is really installed; re-point the dangling ones at the
    # extracted copy.
    find root -type l | while read -r link; do
      target="$(readlink "$link")"
      case "$target" in
        /*)
          if [ ! -e "$target" ]; then
            candidate="root$target"
            if [ -e "$candidate" ]; then
              rm -f "$link"
              cp -a "$candidate" "$link"
            fi
          fi
          ;;
      esac
    done
    jvm_dir="$(find root/usr/lib/jvm -maxdepth 1 -mindepth 1 -type d ! -name '.' | head -n1)"
    [ -n "$jvm_dir" ] || { echo "no JDK found in the extracted packages" >&2; exit 1; }
    mv "$jvm_dir" "$_fm_toolchain_dir/jdk"
  ) || { fm_safe_rm_tree "$_fm_toolchain_dir" "$build_dir"; return 1; }
  fm_safe_rm_tree "$_fm_toolchain_dir" "$build_dir"

  _fm_build_cacerts "$_fm_toolchain_dir/jdk" || return 1
  echo "✓ JDK installed" >&2
  return 0
}

# Generate cacerts from the system CA bundle: the bare dpkg-extracted JDK has no
# trust store (ca-certificates-java normally builds one via a postinst hook we
# can't run without root).
#
# The store is written unencrypted, which is how the JDK ships its own cacerts.
# keytool otherwise encrypts the certificates with the store password, and the
# JVM opens its *default* trust store with a null password -- so a password-
# encrypted store yields zero trust anchors and every HTTPS connection dies with
# "the trustAnchors parameter must be non-empty". Unencrypted, it just reads,
# and nothing has to set javax.net.ssl.trustStorePassword.
_fm_build_cacerts() {
  local jdk_dir="$1" tmpdir f i=0
  tmpdir="$(mktemp -d)" || return 1
  awk -v dir="$tmpdir" '/BEGIN CERTIFICATE/{n++; file=sprintf("%s/cert-%03d.pem", dir, n)} {print > file}' \
    /etc/ssl/certs/ca-certificates.crt
  rm -f "$jdk_dir/lib/security/cacerts"
  for f in "$tmpdir"/*.pem; do
    i=$((i+1))
    "$jdk_dir/bin/keytool" -importcert -noprompt -trustcacerts \
      -alias "cert$i" -file "$f" \
      -keystore "$jdk_dir/lib/security/cacerts" \
      -storepass changeit \
      -J-Dkeystore.pkcs12.certProtectionAlgorithm=NONE \
      -J-Dkeystore.pkcs12.macAlgorithm=NONE >/dev/null 2>&1 || true
  done
  fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmpdir"
  [ -s "$jdk_dir/lib/security/cacerts" ]
}

# True when the JDK's default trust store yields trust anchors with no password,
# which is what every HTTPS client in the build needs. Offline, ~0.5s. Probed
# with JAVA_TOOL_OPTIONS cleared so an inherited trustStorePassword can't make a
# store that only works by workaround look healthy.
_fm_trust_store_ok() {
  local jdk_dir="$1" probe_dir status
  probe_dir="$(mktemp -d)" || return 1
  cat > "$probe_dir/Probe.java" <<'PROBE'
public class Probe {
    public static void main(String[] args) throws Exception {
        javax.net.ssl.TrustManagerFactory factory =
            javax.net.ssl.TrustManagerFactory.getInstance(
                javax.net.ssl.TrustManagerFactory.getDefaultAlgorithm());
        factory.init((java.security.KeyStore) null);
        int anchors = 0;
        for (javax.net.ssl.TrustManager manager : factory.getTrustManagers()) {
            if (manager instanceof javax.net.ssl.X509TrustManager) {
                anchors = ((javax.net.ssl.X509TrustManager) manager).getAcceptedIssuers().length;
            }
        }
        System.exit(anchors > 0 ? 0 : 1);
    }
}
PROBE
  env -u JAVA_TOOL_OPTIONS "$jdk_dir/bin/java" "$probe_dir/Probe.java" >/dev/null 2>&1
  status=$?
  fm_safe_rm_tree "${TMPDIR:-/tmp}" "$probe_dir"
  return $status
}

_fm_install_cmdline_tools() {
  local sdk_root="$1" cli_zip attempt downloaded=false
  echo "Setting up Android SDK..." >&2
  mkdir -p "$sdk_root/cmdline-tools" || return 1

  # curl has been observed to hang indefinitely on some networks even when the
  # host is reachable; wget doesn't, so prefer it for this large download.
  cli_zip="$(mktemp -u /tmp/cmdline-tools.XXXXXX.zip)"
  for attempt in 1 2 3; do
    echo "  Downloading Android command-line tools (attempt $attempt)..." >&2
    if wget -q -O "$cli_zip" --tries=1 --timeout=90 \
      "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip" \
      && [ -s "$cli_zip" ]; then
      downloaded=true
      break
    fi
    rm -f "$cli_zip"
    sleep 2
  done
  if [ "$downloaded" = false ]; then
    echo "Failed to download the Android command-line tools from dl.google.com." >&2
    return 1
  fi

  # A run killed mid-unzip leaves this behind, and unzip would stop to ask
  # before overwriting it.
  fm_safe_rm_tree "$sdk_root/cmdline-tools" "$sdk_root/cmdline-tools/cmdline-tools" || return 1
  unzip -q "$cli_zip" -d "$sdk_root/cmdline-tools" || { rm -f "$cli_zip"; return 1; }
  fm_safe_rm_tree "$sdk_root/cmdline-tools" "$sdk_root/cmdline-tools/latest" || return 1
  mv "$sdk_root/cmdline-tools/cmdline-tools" "$sdk_root/cmdline-tools/latest" || return 1
  rm -f "$cli_zip"
  echo "✓ Android SDK command-line tools installed" >&2
  return 0
}

_fm_toolchain_setup() {
  local jdk_dir sdk_root missing="" sdkmanager

  jdk_dir="$(_fm_find_jdk || true)"
  sdk_root="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Android/Sdk}}"

  [ -n "$jdk_dir" ] || missing="$missing a JDK,"
  [ -x "$sdk_root/cmdline-tools/latest/bin/sdkmanager" ] ||
    missing="$missing the Android SDK command-line tools,"
  missing="${missing%,}"

  # Even a complete toolchain needs egress for Gradle resolution.
  if ! _fm_have_network; then
    ANDROID_TOOLCHAIN_STATUS="offline"
    if [ -n "$missing" ]; then
      ANDROID_TOOLCHAIN_REASON="this run has no network egress, and these would have to be downloaded first:${missing}"
    else
      ANDROID_TOOLCHAIN_REASON="this run has no network egress, which Gradle dependency resolution requires"
    fi
    return 1
  fi

  if [ -z "$jdk_dir" ]; then
    if ! _fm_install_jdk; then
      ANDROID_TOOLCHAIN_REASON="the OpenJDK 21 packages could not be downloaded and unpacked"
      return 1
    fi
    jdk_dir="$_fm_toolchain_dir/jdk"
  fi
  export JAVA_HOME="$jdk_dir" PATH="$jdk_dir/bin:$PATH"

  # Every HTTPS client in the build (sdkmanager, and critically Gradle's own
  # SDK/NDK auto-download) needs a trust store the JVM can read with no
  # password, or it dies with "the trustAnchors parameter must be non-empty".
  # A JDK left by an older revision of this script has a password-encrypted one,
  # so rebuild any store we own. For a JDK we don't own, fall back to naming the
  # password: that costs a "Picked up JAVA_TOOL_OPTIONS" line on every JVM start,
  # which beats failing outright.
  if ! _fm_trust_store_ok "$jdk_dir"; then
    if [ -w "$jdk_dir/lib/security" ]; then
      echo "Rebuilding the JDK trust store..." >&2
      if ! _fm_build_cacerts "$jdk_dir"; then
        ANDROID_TOOLCHAIN_REASON="the JDK trust store at $jdk_dir could not be rebuilt"
        return 1
      fi
    else
      export JAVA_TOOL_OPTIONS="${JAVA_TOOL_OPTIONS:-} -Djavax.net.ssl.trustStorePassword=changeit"
    fi
  fi

  if [ ! -x "$sdk_root/cmdline-tools/latest/bin/sdkmanager" ]; then
    if ! _fm_install_cmdline_tools "$sdk_root"; then
      ANDROID_TOOLCHAIN_REASON="the Android SDK command-line tools could not be installed"
      return 1
    fi
  fi
  export ANDROID_HOME="$sdk_root" ANDROID_SDK_ROOT="$sdk_root"
  # The Actions sandbox runs as an account whose passwd home is /nonexistent,
  # and the JVM resolves user.home from passwd, not $HOME. Without these,
  # Gradle's caches, wrapper distributions and daemon logs, plus the Android
  # SDK's ~/.android state, land in /nonexistent on the sandbox's scratch root
  # instead of the run's own home.
  export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HOME/.gradle}"
  export ANDROID_USER_HOME="${ANDROID_USER_HOME:-$HOME/.android}"
  sdkmanager="$sdk_root/cmdline-tools/latest/bin/sdkmanager"

  echo "Accepting Android SDK licenses..." >&2
  yes | "$sdkmanager" --licenses >/dev/null 2>&1 || true

  # Only platform-tools is installed up front; the Android Gradle Plugin
  # auto-downloads whatever compileSdk/build-tools/NDK version the build asks
  # for, now that licenses are accepted and cmdline-tools is on
  # ANDROID_SDK_ROOT.
  echo "Installing Android SDK platform-tools..." >&2
  "$sdkmanager" --install "platform-tools" 2>&1 | grep -v "^Warning:" || true
  return 0
}

ANDROID_TOOLCHAIN_READY=0
ANDROID_TOOLCHAIN_STATUS="failed"
ANDROID_TOOLCHAIN_REASON=""
if _fm_toolchain_setup; then
  ANDROID_TOOLCHAIN_READY=1
  ANDROID_TOOLCHAIN_STATUS="ready"
  ANDROID_TOOLCHAIN_REASON=""
elif [ -z "$ANDROID_TOOLCHAIN_REASON" ]; then
  ANDROID_TOOLCHAIN_REASON="the Android toolchain could not be set up"
fi

unset -f _fm_have_network _fm_find_jdk _fm_apt_download \
         _fm_install_jdk _fm_build_cacerts _fm_trust_store_ok \
         _fm_install_cmdline_tools _fm_toolchain_setup
unset _fm_toolchain_dir
