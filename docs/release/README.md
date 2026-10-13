# Releases

How a ForkMesh version is numbered, built, signed, published, promoted and
installed. It covers the desktop app on every platform, the Go mirror daemon,
the Android APK and the macOS `.dmg`, and the App Store submission of the
iOS app that every release triggers. The iOS build steps themselves are in
[`docs/native/IOS_RELEASE.md`](../native/IOS_RELEASE.md).

The metadata layout and the publisher's flags are documented next to the data
in [`.forkmesh/releases/README.md`](../../.forkmesh/releases/README.md); the
design (why binaries never enter git) is
[`www/docs/design/release-binary-publishing.md`](../../www/docs/design/release-binary-publishing.md).

## Code map

| Path | Responsibility |
| --- | --- |
| `desktop/CMakeLists.txt` (`project(ForkMesh VERSION …)`) | The release version. |
| `desktop/src/MainWindowReleases.cpp` | Releases panel: draft a release, stamp every versioned file on the target branch, tag. |
| `desktop/src/MainWindowRemoteReleases.cpp`, `desktop/src/ReleaseDraft.*` | The same cut without the dialog, for the MCP `create_release` tool and phone requests ([../tools/mcp-releases.md](../tools/mcp-releases.md)). |
| `desktop/tests/validate_release_version_stamp.py` | Lists every file the stamp must touch and fails when another tracked file declares the version. |
| `.forkmesh/release.yml` | Actions workflow "Attach desktop build to release" (on `release` and `workflow_dispatch`). |
| `tools/forkmesh-release-publish.sh` | Hashes binaries, copies them into the CAS, writes and signs the channel metadata. |
| `tools/forkmesh-release-promote.py` | The promotion gate: copies a candidate to `latest` and regenerates the appcast. |
| `tools/forkmesh-release-host.py`, `tools/release-host/*.{service,timer}` | Builds the assets the Actions sandbox cannot (Linux, APK, Windows installer, the macOS `.dmg` on the office Mac or Codemagic), promotes and seeds mirrors, rolls the mirror node out to the managed mirror hosts, submits the iOS app to the App Store, and checks that the homepage offers the release. |
| `tools/package/linux-baseline.sh`, `tools/package/linux-baseline/` | Debian 12 container build of the Linux binary and AppImage, glibc check, launch test on Ubuntu 22.04/24.04 and Debian 12 ([linux-baseline.md](linux-baseline.md)). |
| `tools/package/` | Per-platform packaging: `appimage.sh`, `macos.sh`, `windows.sh` + `forkmesh.nsi` + `nsis-uninstall-manifest.sh`, `package-release.sh`, `generate-appcast.sh`, toolchain scripts. |
| `tools/package/macos_universal_on_mac.sh` | Builds the universal (arm64 + x86_64) `.dmg` on a Mac: Qt 6.11.3 from aqt, OpenSSL 3.5.8 and static libwebp/nghttp3 per architecture, then `macos.sh`. The office Mac and Codemagic both run it. |
| `codemagic.yaml` (`macos-desktop`) | Runs `macos_universal_on_mac.sh` on a Codemagic Mac mini when the office Mac is offline; the release host starts it through the Codemagic API. |
| `codemagic.yaml` (`ios-native-release`), `native/ios/tools/asc_appstore.py` | The iOS release: archive, upload, TestFlight internal, and with `FORKMESH_IOS_APP_STORE=1` the App Store submission. |
| `.forkmesh/releases/<channel>/` | `SHASUMS256.txt`, `release.json`, `release.json.sig`, `appcast.xml` (latest). |
| `www/public/install.sh`, `www/public/uninstall.sh` | One-line installer and uninstaller. |
| `desktop/src/MainWindowSetup.cpp` (`tryPrebuiltAutoUpdate`) | The desktop's in-app update to a signed prebuilt. |
| `app/src/releases.py`, `app/src/urls.py` | Content-addressed download route used by installers and updaters. |

## Architecture

1. **Version.** The version lives in `desktop/CMakeLists.txt`. Drafting a
   release in the desktop's Releases panel stamps it into every versioned file
   on the target branch (`desktop/CMakeLists.txt` and
   `native/android/app/build.gradle.kts` with versionCode
   `MAJOR*10000 + MINOR*100 + PATCH`). Files it skips are named in the
   publish notice.
2. **Build.** `.forkmesh/release.yml` runs on the release event in the Actions
   sandbox and builds `forkmesh-mirror-node-linux-x86_64` (static Go). Since
   1.2.5 the Linux desktop binary and AppImage are host-built (step 4). It
   refuses a dirty tree, because the binary embeds its build commit.
3. **Publish to a candidate.** `tools/forkmesh-release-publish.sh --channel
   candidate-<version>` puts the bytes in the serving node's CAS
   (`<mirror>/forkmesh-releases/sha256/<aa>/<hash>/data`) and writes signed
   metadata. The signing key comes from the `FORKMESH_RELEASE_SIGNING_KEY_PEM`
   Actions variable and is written `0600` only for the step.
4. **Host-built assets.** `tools/forkmesh-release-host.py`, run every five
   minutes by `forkmesh-release-host.timer`, builds the Linux binary, AppImage
   and `.zsync` in the Debian 12 baseline container, builds and signs the APK and the
   Windows installer for the newest held-back candidate, builds the universal
   macOS `.dmg` on the office Mac (Codemagic when the Mac is offline, below),
   publishes them into it, runs the gate, verifies the promoted signature against the key in
   `www/public/install.sh`, seeds each managed mirror's release store, then
   merges the metadata to `main` and moves the tag onto that commit. The same
   run then rolls the promoted mirror node out to the managed mirror hosts
   (see "Mirror-node rollout" below), submits the iOS app to the App Store and
   checks the homepage (below). Each of the four steps runs even when an
   earlier one failed; the exit status is the worst of them.
5. **Promote.** `tools/forkmesh-release-promote.py` copies the candidate to
   `latest` only when it has every required asset
   (`FORKMESH_RELEASE_REQUIRED_ASSETS`; default Linux binary, AppImage and
   mirror node) and drops nothing `latest` already offers. Exit 3 means held
   back, 2 a configuration or manifest error.
6. **Install and update.** `install.sh` authenticates `release.json` (see
   Security), checks it binds `SHASUMS256.txt`, downloads the blob by hash from
   `/api/repo/<owner>/<repo>/releases/blob/sha256/<sha256>` and verifies it.
   The desktop's updater reads `.forkmesh/releases/latest/release.json` at the
   tag commit, then on the tracked upstream branch, and accepts it only if its
   `tag` is the release being installed.

### Platforms

- **Linux:** built by the release host in a pinned Debian 12 container with
  static Qt 6.8.2 and OpenSSL 3.5.8 (`tools/package/linux-baseline.sh`), so
  the binary and AppImage run on Ubuntu 22.04+ and Debian 12+; a release fails
  if any ELF needs `GLIBC_` above 2.35. Details:
  [linux-baseline.md](linux-baseline.md).
- **Windows:** cross-built on the release host with MinGW-w64 and a Qt
  `win64_mingw` kit; OpenSSL 3.5 is cross-built because the kit ships 3.0
  (316dc4c8e). NSIS installer, Authenticode-signed with osslsigncode using a
  self-issued certificate. Since 1.2.4 the uninstaller deletes only the files
  listed by `tools/package/nsis-uninstall-manifest.sh` (#727). Details:
  [`docs/desktop/windows.md`](../desktop/windows.md).
- **macOS:** one ad-hoc signed universal `.dmg`
  (`tools/package/macos_universal_on_mac.sh`), published as both
  `forkmesh-macos-arm64.dmg` and `forkmesh-macos-x86_64.dmg`. v1.2.5 was the
  first release carrying the Intel name, so the gate's no-drop rule now
  requires both on every release, and only a universal image supplies the
  Intel one. The release host builds it on the office Mac first: it finds the
  Mac as the ad hoc iOS builds do (hint address, then a LAN scan for its
  pinned host key, `docs/native/MAC_BUILD.md`), takes `~/ci/lock` as
  `desktop-release vX.Y.Z` (ad hoc iOS runs wait for it, never kill it),
  streams `git archive <build_commit>` into `~/desktop-release/src-<version>`
  (older sources and build trees are deleted first, so every release builds
  cold, about 35 minutes; the Qt and dependency caches stay), runs the script
  with `FORKMESH_BUILD_COMMIT`, copies the image back, checks its sha256
  against the one the script printed, and then runs the same checks as
  `--macos-dmg`: it must carry the build commit, and it is published under
  the Intel name only when every Mach-O file in it is universal.  (2026-10-12, v1.2.8) The check scans the decompressed image bytes, so a
  universal file stored in more than one extent inside the image shows one
  slice away from where its fat header points. Each thin header outside a
  fat slice is paired with a fat slice of the same CPU that has no header at
  its offset; only an unpaired thin header counts as single-arch. Before
  this, one fragmented file held v1.2.8's Intel image back although all 41
  Mach-O files were universal (`lipo` on the mounted image). The
  Linux, APK and Windows builds run meanwhile. Only when the Mac cannot be
  reached, before or during the build (ssh exit 255), does the host fall back
  to Codemagic; a build that fails on the Mac is retried after the one-hour
  backoff, because Codemagic would fail the same way and bill for it.
  The Codemagic `macos-desktop` workflow runs the same script (it used to
  build an arm64-only image with Homebrew Qt, which could not supply the
  Intel name). The host checks that the public mirror
  (`--public-repository`, default `https://forkmesh.com/forkmesh/forkmesh`,
  which is what Codemagic clones) serves the release tag at the manifest's
  `build_commit`, then `POST /builds` with `workflowId: macos-desktop` and
  `tag: vX.Y.Z`. A queued, running or finished build of the same tag and
  commit is reused rather than started again. It polls every minute for up to
  three hours (running the APK and Windows builds meanwhile), refuses a build
  that failed or ran another commit, downloads the
  `ForkMesh-<version>-macos-universal.dmg` artefact (a zipped artefact is
  unpacked), and checks the size and md5 Codemagic reports, a 10 MB floor and
  the UDIF `koly` trailer, then the same commit and universality checks.
  The token is sent only to `https://*.codemagic.io`; a redirected download
  goes on without it. Without a token the run stops with a configuration
  error (exit 2) naming the missing variable.
  Why build it rather than make macOS optional in the gate: the gate's
  no-drop rule is the only thing that keeps `latest` offering a current
  `.dmg`. Marking it optional would promote candidates without it, `latest`
  would stop listing a macOS download. A `.dmg`-installed app has no in-app
  updater (`maybeAutoUpdate` needs a source checkout, and Sparkle is not
  linked), so the `.dmg` that `latest`, the homepage and `/desktop` offer is
  the only way macOS users get a new version.
  `FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS=1` remains the manual escape hatch
  when neither the Mac nor Codemagic can build it, and `--macos-dmg PATH`
  publishes an image built by hand instead of building one.
- **iOS:** not a download; the App Store app `com.forkmesh.forkmesh`. For the
  newest candidate from v1.2.6 on (`IOS_FIRST_RELEASE`, so turning this on
  did not submit v1.2.5), the release host runs Codemagic's
  `ios-native-release` steps with `FORKMESH_IOS_APP_STORE=1`: archive, upload,
  TestFlight internal, and a submission for App Review that releases on
  approval (`native/ios/tools/asc_appstore.py`, version rules in
  [`docs/native/IOS_RELEASE.md`](../native/IOS_RELEASE.md)). What's New is the
  annotated tag's message. It runs on the office Mac when it answers: under
  `~/ci/lock` as `ios-release vX.Y.Z`, it fetches the build commit into
  `~/ci/tree` (the ad hoc builds' checkout, so DerivedData stays warm), writes
  the workflow's steps with `codemagic_steps.py` from the build commit, and
  runs them with `mac_adhoc_build.sh` in `~/release/ios-vX.Y.Z/` (the two
  newest kept). The run counts only if its log shows the submission. When the
  Mac cannot be reached it starts `ios-native-release` on Codemagic from the
  release tag with the same variables and waits up to two hours; a queued,
  running or finished build of that tag is reused. Success is recorded in
  `<work>/ios-vX.Y.Z.json`, so a release is submitted once; a failure waits
  for the one-hour backoff (`failed-ios-vX.Y.Z`). iOS keeps its own version
  (`native/ios/Config/Version.xcconfig`), so the desktop tag only decides
  when a build ships, not its number.
- **Homepage:** `www/public/homepage-downloads.js` reads
  `.forkmesh/releases/latest/release.json` (and a newer candidate) from
  `app.forkmesh.com` at `main` and verifies it in the browser, so the page
  needs no edit per release. After promotion the release host fetches that
  same URL and passes once it serves main's `latest` (same `tag` and
  `checksums_sha256`), recorded in `<work>/homepage-vX.Y.Z`. If
  app.forkmesh.com still serves an older manifest 30 minutes after the host
  first looked, every run fails with the URL and the tag it serves, until
  the metadata reaches the cores.
- **Android:** `forkmesh-android-universal.apk`, built by the release host and
  signed with the v3 direct-distribution key (`android-direct-distribution-v3.jks`,
  created 2026-09-22 after the v2 keystore was lost). Devices on a v2-signed
  APK must uninstall once.

### Mirror-node rollout

Why: the owner wants to cut a release and close the desktop. Building,
promoting, seeding and publishing main and tags were already headless; the
last step only the desktop did was installing the new
`forkmesh-mirror-node-linux-x86_64` on each managed mirror host
(`deployPendingReleaseRollout` and the published-binary branch of
`buildHostInstallCommand`, driven by the `releases/pendingMirrorRollouts`
intent and the three-minute finalization retry). The release host now does
it too, as `roll_out_release` in `tools/forkmesh-release-host.py`.

- **When.** Every run after the completion step (not with `--no-land`,
  `--ios-only` or `--skip-rollout`; `--rollout-only` runs only this step).
  It rolls out whatever `latest` on `main` names, so it also picks up a
  release whose assets were finished in an earlier run.
- **Hosts.** The managed mirrors in `config/fanout/hosts` (`--hosts-file`),
  reached as `root@` with the seeding key (`--ssh-key`,
  `ssh/vultr_mirror_ed25519`). Names starting `fmnext-` are left out, and a
  host whose unit is rendered by `deploy/core/render.py` answers
  `next-managed` and is left alone: the ForkMesh Next cores get their daemon
  from `tools/next_fleet.py`, which pushes an IDS expected-change manifest
  first, and an unannounced change to `/usr/local/bin` there is an IDS
  lockdown ([../IDS.md](../IDS.md)). The release seed hosts
  (`config/fanout/release-hosts`) are therefore seeded but never rolled out.
  Since the Vultr fleet was retired, `config/fanout/hosts` lists no host and
  the desktop's saved host list is empty, so today the step only records the
  release as settled.
- **Per host** (one SSH session, all hosts in parallel; the script is
  `ROLLOUT_REMOTE`): the same unit, account, config-path and `--check`
  checks as the desktop (exit 76 when the service is not controller
  managed); pick the blob for `uname -m`; if the installed binary already
  has that sha256 and `/v1/status` reports the version, wait for
  `/readyz` and answer `current` without touching anything. Otherwise check
  that `forkmesh-node` can write the config folder before anything changes
  (the 2026-09-13 trap: `/etc/forkmesh` re-owned to root made
  `--set-version` fail with exit 73 after the binary had been swapped);
  take the blob from the host's own seeded release store, or download it
  from `--rollout-blob-base`; verify its sha256; keep the old binary as
  `forkmesh-mirror-node.bak-update`; move the new one over it; run
  `--set-version` as `forkmesh-node`; restart; wait for `/healthz` and for
  `/v1/status` to report the version (30 s); then wait for `/readyz`
  (120 s). A failed `--set-version` or a daemon that does not come back
  puts the `.bak-update` binary and the previous config version back
  (exit 73 / 72). A host that runs the release but is not ready fails with
  exit 71 and is retried.
- **Record.** `<work>/rollout-vX.Y.Z.json` (`~/projects/forkmesh/release/`)
  keeps each host's last outcome (`current`, `deployed`, `next-managed` or
  `failed` with the exit and message). Settled hosts are never contacted
  again for that release; a host added to the file later, or whose address
  changed, is. A failed pass is retried after the one-hour backoff and given
  up after six failed passes (logged once). `--rollout-only --tag vX.Y.Z`
  retries at once, ignoring both.
- **Desktop.** It still runs its own rollout when its pending intent fires,
  so a box without the release host keeps working. Its per-host command now
  first checks the installed sha256 and the reported version, and when the
  host already runs the release it prints "already deployed and running"
  and exits 0 without downloading, swapping or restarting. When the release
  host got there first, every host answers that way, the deploy counts as
  successful and the desktop clears `releases/pendingMirrorRollouts`.

## Protocols and interfaces

- `release.json`: `schema`, `repo`, `tag`, `tag_commit`, `build_commit`,
  `channel`, `created_at`, `checksums_sha256`, `assets[]` (`name`,
  `blob_sha256`, `size`, `os`, `arch`). `release.json.sig` is a raw 64-byte
  Ed25519 signature over the exact file bytes.
- `tag_commit` is the release-cut commit the tag peeled to at signing; the tag
  then moves to the publish commit, so verifiers check
  `git merge-base --is-ancestor <tag_commit> <tag>`, not equality. The one
  definition is "What `tag_commit` means" in
  [`.forkmesh/releases/README.md`](../../.forkmesh/releases/README.md).
  `release_manifest_content` in `app/src/releases.py` leaves `build_commit` out
  of the signed header when it is empty, so legacy manifests that predate it
  keep their exact v1 canonical bytes.
- Asset names: `forkmesh-<os>-<arch>[.exe]`, plus `.AppImage`, `.dmg`,
  `-setup.exe`, `forkmesh-mirror-node-linux-x86_64`,
  `forkmesh-android-universal.apk`. The `forkmesh-mirror-node` prefix is how
  installers tell the daemon from the client.
- `appcast.xml`: Sparkle/WinSparkle appcast written by
  `tools/package/generate-appcast.sh` on promotion. Neither Sparkle nor
  WinSparkle is linked into the desktop, so nothing reads it: Windows and
  macOS installs update by installing the newer `setup.exe` / `.dmg` (see
  [`docs/desktop/windows.md`](../desktop/windows.md)).

## Configuration

Names only; values live in Actions variables or on the release host.

- Actions: `FORKMESH_RELEASE_SIGNING_KEY_PEM`, `FORKMESH_RELEASE_REQUIRED_ASSETS`,
  `FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS`, `FORKMESH_WIN_CERT_PFX`,
  `FORKMESH_WIN_CERT_PASSWORD`.
- Publisher: `FORKMESH_RELEASE_CAS`, `FORKMESH_RELEASE_SIGNING_KEY`,
  `RELEASE_CHANNEL`, `FORKMESH_TAG`, `FORKMESH_TAG_COMMIT`,
  `FORKMESH_BUILD_COMMIT`.
- Release host: signing material under `<data>/release/` as resolved by
  `tools/forkmesh_paths.py` (`~/projects/forkmesh/data/release/` on the
  projects layout): `android-direct-distribution-v3.jks`,
  `windows-authenticode.pfx` and their password files. Overrides:
  `FORKMESH_ANDROID_KEYSTORE`, `FORKMESH_ANDROID_KEYSTORE_PASSWORD_FILE`,
  `FORKMESH_WIN_CERT_PFX_FILE`. Mirror hosts to seed: `config/fanout/hosts`
  (`--hosts-file`). Codemagic: `CODEMAGIC_API_TOKEN` and optional
  `CODEMAGIC_APP_ID` (default `6aac6e1e7b35578e30ad5a76`), read from the
  service environment first, then from the desktop's Actions variables in the
  settings file (`[actions] variables`). The office Mac: its key and pinned
  host key in `<ssh>/mac-build/{id_ed25519,known_hosts}`
  (`~/projects/forkmesh/ssh/mac-build/`), and optional `MAC_BUILD_HOST`
  (tried after the address the Mac was last found at, default
  `192.168.1.201`; `docs/native/MAC_BUILD.md`) and `MAC_BUILD_GIT_URL`
  (where the Mac fetches the iOS source, default
  `https://app.forkmesh.com/forkmesh/forkmesh`), read the same way.
- Build: `-DFORKMESH_BUILD_COMMIT_OVERRIDE`, `-DFORKMESH_VERSION_OVERRIDE`.
- Installer: `FORKMESH_PUBLIC_HOSTNAME`, `FORKMESH_LINK_CODE`, `FORKMESH_DNS_WAIT_SECONDS` ([self-setup-node.md](self-setup-node.md)), `FORKMESH_RELEASE` (channel), `FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE`,
  `FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256`, `FORKMESH_FROM_SOURCE`.

## Build, run, test

```sh
python3 tools/forkmesh-release-host.py --tag vX.Y.Z --dry-run   # plan host assets and the iOS release
python3 tools/forkmesh-release-host.py --auto --rollout-only --dry-run   # which hosts still need the mirror node
python3 tools/forkmesh-release-host.py --tag vX.Y.Z --rollout-only      # roll the mirror node out now
python3 tools/forkmesh-release-host.py --tag vX.Y.Z --ios-only   # submit one release's iOS app now
python3 tools/forkmesh-release-host.py --auto --skip-ios         # desktop assets and homepage only
(cd native/ios && python3 tools/asc_appstore.py status)          # App Store versions and reviews
ASC_KEY_ID=… ASC_ISSUER_ID=… python3 native/ios/tools/asc_store_report.py  # versions, reviews, TestFlight groups, feedback
python3 tools/forkmesh-release-promote.py --candidate candidate-X.Y.Z --dry-run
python3 desktop/tests/validate_release_version_stamp.py
tools/package/linux-baseline.sh check <binary> <AppImage>        # glibc ceiling
python3 -m pytest app/tests/test_linux_release_baseline.py
python3 -m pytest app/tests/test_install_lifecycle.py app/tests/test_installer_source.py app/tests/test_releases.py
python3 -m pytest app/tests/test_release_host_completion.py tools/test_ios_app_store_release.py
python3 -m pytest tools/test_forkmesh_release_host_rollout.py tools/test_forkmesh_release_host_seed.py
```

## Operations

- Cut a release from the desktop's Releases panel (stamps, tags, triggers
  `.forkmesh/release.yml`), or without being at the desktop: an agent the
  desktop launched calls the MCP `create_release` tool, or the phone sends
  Create release ([../tools/mcp-releases.md](../tools/mcp-releases.md)). The
  release host completes it within minutes; the fleet's desktops then update
  themselves through the restart countdown.
- Watch the release host, including the mirror-node rollout (one line per
  host, then "the mirror node runs it on every managed host" or the failure):
  `journalctl --user -u forkmesh-release-host.service -f`, or
  `journalctl --user -u forkmesh-release-host.service --since today | grep rollout`.
  The per-host record is `~/projects/forkmesh/release/rollout-vX.Y.Z.json`.
- Check a release: fetch `.forkmesh/releases/latest/release.json{,.sig}` and
  verify with the public key in `www/public/install.sh`.
- Same-version rebuild: `FORKMESH_RELEASE_CAS=<served CAS> app/deploy.sh
  republish-release-binary` from a clean worktree.
- Fan-out to mirrors after a release: see
  [`docs/server/fleet-operations.md`](../server/fleet-operations.md).

## Security

- Two trust anchors for a prebuilt install: the release publisher's Ed25519
  public key (built into `install.sh`, or a local PEM via
  `FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE`, default
  `/etc/forkmesh/release-publisher.pem`), or a manifest digest from an
  authenticated controller. A checksum copied from the fetched manifest is
  never trusted by itself.
- The private release key, the APK keystore and the Authenticode certificate
  never enter git or docs.
- Fleet installs from the desktop also pin `build_commit` and check
  `--version` and `--build-commit` on each host.
- `install.sh --uninstall/--reinstall/--purge` and `uninstall.sh --purge`
  refuse to erase anything outside `HOME` and refuse a user-data wipe from an
  AI agent shell (detected by `CLAUDECODE`, `CLAUDE_CODE_ENTRYPOINT`,
  `FORKMESH_AGENT_PROVIDER`, `CODEX_SANDBOX`) (74f3694a0). This is a backstop;
  `AGENTS.md` forbids agents from running them at all.

## Traps

- **Manifest before tag.** The Actions job tags the bump commit and lands the
  metadata later, so `latest` at the tag commit can describe the previous
  release. The release host moves the tag onto the metadata commit; the
  updater also checks the upstream branch and rejects a manifest whose `tag`
  differs.
- **No-drop rule and the `.dmg`.** `latest` offers
  `forkmesh-macos-arm64.dmg` and `forkmesh-macos-x86_64.dmg`, so a candidate
  without both is held back (exit 3) until the release host has a universal
  image. An arm64-only image (an older Codemagic build, or a hand-built one)
  publishes the arm64 name and keeps the release held back for the Intel one.
  On the Codemagic fallback, if the public mirror has not received the tag
  yet, the host run fails and retries after the one-hour backoff (or run
  `--tag vX.Y.Z` by hand). Codemagic builds the tag from the public mirror, so
  a tag moved before the `.dmg` is built makes the commit check fail rather
  than ship a mismatched image. The office Mac gets its source from this
  checkout (`git archive`), so it does not wait for the mirror.
- **A killed host run leaves the Mac locked.** A remote step keeps running
  when the release host's ssh dies. The lock loop exits once its session is
  gone, and every step under the lock writes its PID to `~/ci/lock/pid`, so
  the next attempt of the same release (`desktop-release vX.Y.Z` or
  `ios-release vX.Y.Z`) ends the old one and takes the lock over. A lock held
  under another name is waited for up to two hours, and one older than four
  hours is removed. The release checks the lock every 5 seconds while ad hoc
  runs check every minute, so a stream of `native/ios` pushes cannot starve a
  release: it takes the lock as soon as the running build releases it.
- **Codemagic unavailable (2026-09-25).** When Codemagic refuses to start the
  macOS build (for example `HTTP 402` once the free minutes are used up), the
  release host no longer fails the whole run. It builds the other missing
  assets, publishes them into the candidate without promoting, lands that
  metadata on `main`, and leaves the tag where it is. The failure marker keeps
  retrying Codemagic after the one-hour backoff, and the first run that gets
  the `.dmg` promotes the candidate. So 1.2.5 carried Linux, Windows and
  Android in `candidate-1.2.5` while it waited for the Codemagic minutes to
  reset.
- **Dirty tree = dirty provenance.** Any untracked file in the build worktree
  marks the build commit dirty.
- **APK versionCode must grow**, or Android refuses the upgrade.
- **A stale `signature.pem`** in the Windows build dir broke osslsigncode's
  signer check (fixed in e02071772).
- **Installer tests** must sandbox `HOME` and `XDG_*_HOME` and drop
  `FORKMESH_HOME`/`FORKMESH_PROJECTS_DIR` (`app/tests/conftest.py`).

## Recent changes

- 2026-10-12: the release host rolls the promoted mirror node out to the
  managed mirror hosts, so the desktop can be closed after a cut. The
  desktop's per-host release command skips a host that already runs the
  release, so the two never redeploy each other's work. See
  "Mirror-node rollout".

- 2026-10-02: iOS release submission is off on the release host. Its
  hourly Codemagic `ios-native-release` retries for v1.2.7 all failed (a stray
  brace in `StatsBoardTests.swift`) and used about 120 Codemagic minutes a
  day, while the account is on the 500-minute free tier. Both user units now
  carry a `skip-ios.conf` drop-in that adds `--skip-ios`. The second unit,
  `forkmesh-release-android.timer`, was left over from before the Android
  step joined the release host: it ran `tools/forkmesh-release-android.py`,
  which only re-enters `forkmesh-release-host.py`, so every pass ran twice
  and kept submitting iOS after the first drop-in. That timer is now off.
  Turn timers on or off in the desktop under **Settings → Background timers**
  ([../desktop/diagnostics.md](../desktop/diagnostics.md#background-timers)).
- 2026-10-01: the first App Store run (v1.2.6) failed on the office Mac with
  exit 1 and no output. The iOS step script pruned old `release/ios-*` run
  folders before creating the new one, and with no folder yet the unmatched
  glob made macOS `ls` exit 1 under `pipefail` (its message went to
  `/dev/null`). It now creates the run folder first, so the glob always
  matches and the two newest folders, the current one included, are kept.
- 2026-09-29: `install.sh` sets up HTTPS for a node whose DNS the user points by hand (no Cloudflare token): it waits for the record, then lets Caddy get the certificate. `FORKMESH_DNS_WAIT_SECONDS` bounds the wait. See [self-setup-node.md](self-setup-node.md).
- 2026-09-28: a tagged release builds every platform and ships iOS. The
  macOS `.dmg` is universal and built on the office Mac, with Codemagic
  (running the same script) as the fallback; before this, Codemagic's
  arm64-only image could never supply the `forkmesh-macos-x86_64.dmg` that
  v1.2.5 put in `latest`, so v1.2.6 would have been held back for good. The
  release host submits the iOS app to the App Store (Mac first, Codemagic
  when the Mac is offline) and checks that app.forkmesh.com, which the
  homepage reads, serves the promoted manifest. Merged
  `release-local-macos-dmg` (`--macos-dmg`, used by hand for v1.2.5).

- 2026-09-27: `tag_commit` defined as the release-cut commit, an ancestor of
  the published tag (#703); v0.8.3, v0.8.10 and v1.2.4 pass the check. Stale
  "peeled target" comments in `install.sh` and `releases.py` were dropped
  rather than expanded, since source holds no prose.
- 2026-09-24: Linux assets built in a Debian 12 container (static Qt 6.8.2,
  OpenSSL 3.5.8), glibc ceiling 2.35, launch-tested on Ubuntu 22.04/24.04 and
  Debian 12; the Actions job publishes only the mirror node on Linux
  ([linux-baseline.md](linux-baseline.md)).
- 2026-09-24: v1.2.4, the Windows mirror Actions lock fix and a payload-only
  uninstaller (#727; f08182c96, aef0332af, a066cb1c7).
- 2026-09-22: v1.2.3; APKs signed with the v3 key (589a3d046); Windows
  OpenSSL 3.5 cross-build (316dc4c8e); macOS `.dmg` via Codemagic
  (7dc3cef5b, 62bac1a01); installer data-wipe guard (74f3694a0).
- 2026-09-21: installer defaults to `~/projects/forkmesh/src`; release tools
  resolve keys and hosts through the projects layout (058484c3e, 8c1ba825e).
- 2026-09-17: clean-machine VM evidence for the published release (#705).

## Related docs

- [self-setup-node.md](self-setup-node.md): a node set up from DNS alone, without the desktop
- [`.forkmesh/releases/README.md`](../../.forkmesh/releases/README.md)
- [`www/docs/design/release-binary-publishing.md`](../../www/docs/design/release-binary-publishing.md)
- [`www/docs/install-support-matrix.md`](../../www/docs/install-support-matrix.md)
- [`www/docs/launch/clean-machine-evidence.md`](../../www/docs/launch/clean-machine-evidence.md)
- [`docs/tools/README.md`](../tools/README.md), [`docs/tools/projects-layout.md`](../tools/projects-layout.md)
