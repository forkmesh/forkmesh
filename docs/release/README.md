# Releases

How a version is numbered, built, signed, published, promoted and installed:
the desktop app on every platform, the Go mirror daemon, the Android APK, the
macOS `.dmg`, and the App Store submission of the iOS app. The metadata layout
and publisher flags are documented beside the data in
`.forkmesh/releases/README.md`; binaries never enter git.

## Code map

| Path | Responsibility |
| --- | --- |
| `desktop/CMakeLists.txt` (`project(ForkMesh VERSION …)`) | the release version |
| `desktop/src/MainWindowReleases.cpp`, `MainWindowRemoteReleases.cpp`, `ReleaseDraft.*` | draft a release, stamp versioned files, tag (also from MCP and phones) |
| `desktop/tests/validate_release_version_stamp.py` | fails when a tracked file declares the version outside the stamp list |
| `.forkmesh/release.yml` | Actions workflow that builds the mirror daemon on the release event |
| `tools/forkmesh-release-publish.sh` | hashes binaries, copies them into the content-addressed store, writes and signs channel metadata |
| `tools/forkmesh-release-promote.py` | promotion gate from a candidate to `latest`; regenerates the appcast |
| `tools/forkmesh-release-host.py`, `tools/release-host/*` | builds what the Actions sandbox cannot, promotes, seeds mirrors, rolls out the daemon, submits iOS, checks the homepage |
| `tools/package/linux-baseline.sh`, `tools/package/linux-baseline/` | Linux build in a pinned Debian 12 container ([linux-baseline.md](linux-baseline.md)) |
| `tools/package/` | per-platform packaging (`appimage.sh`, `macos.sh`, `windows.sh`, NSIS, appcast) |
| `tools/package/macos_universal_on_mac.sh` | universal (arm64 + x86_64) `.dmg` build on a Mac |
| `codemagic.yaml` (`macos-desktop`, `ios-native-release`), `native/ios/tools/asc_appstore.py` | cloud Mac fallback for the `.dmg`; iOS archive, upload, TestFlight and App Store submission |
| `.forkmesh/releases/<channel>/` | `SHASUMS256.txt`, `release.json`, `release.json.sig`, `appcast.xml` |
| `.forkmesh/releases/builds.json` | the build number each platform is on for the current version |
| `.forkmesh/shortcuts/deploy-build-*.sh`, `deploy-new-version.sh` | one-tap per-platform builds and the new-version workflow ([Build numbers](#build-numbers)) |
| `www/public/install.sh`, `www/public/uninstall.sh` | one-line installer and uninstaller |
| `desktop/src/MainWindowSetup.cpp` (`tryPrebuiltAutoUpdate`) | in-app update to a signed prebuilt |
| `app/src/releases.py`, `app/src/urls.py` | content-addressed download route |

## Flow

1. **Version.** Drafting a release stamps the version into every versioned file
   on the target branch (including the Android versionCode
   `MAJOR*10000 + MINOR*100 + PATCH`).
2. **Build.** `.forkmesh/release.yml` builds the static Go mirror daemon in the
   Actions sandbox; it refuses a dirty tree because binaries embed their build
   commit.
3. **Publish a candidate.** `forkmesh-release-publish.sh --channel
   candidate-<version>` stores bytes under
   `<mirror>/forkmesh-releases/sha256/<aa>/<hash>/data` and writes signed
   metadata.
4. **Host-built assets.** `tools/forkmesh-release-host.py` (timer every five
   minutes) builds the Linux binary, AppImage and `.zsync`, the signed APK, the
   signed Windows installer and the universal macOS `.dmg`, publishes them into
   the candidate, runs the gate, verifies the promoted signature against the
   key in `install.sh`, seeds mirror release stores, merges the metadata to
   `main` and moves the tag onto that commit. It then rolls out the mirror
   daemon, submits the iOS app and checks the homepage; each step runs even
   if an earlier one failed.
5. **Promote.** The gate copies a candidate to `latest` only when it has every
   required asset (`FORKMESH_RELEASE_REQUIRED_ASSETS`) and drops nothing
   `latest` already offers. Exit 3 means held back, 2 a configuration error.
6. **Install and update.** `install.sh` authenticates `release.json`, checks it
   binds `SHASUMS256.txt`, downloads blobs by hash from
   `/api/repo/<owner>/<repo>/releases/blob/sha256/<sha256>` and verifies them.
   The desktop updater accepts a manifest only if its `tag` is the release
   being installed.

## Release trust rules

Every consumer of `release.json` checks `release.json.sig` against the one
Ed25519 release key pinned in `www/public/install.sh`. The desktop pins the
same key in `desktop/src/ReleaseTrust.cpp`;
`desktop/tests/validate_auto_update_release_trust.py` fails if the two differ.

- **Channel.** A manifest must name the channel being installed. A promoted
  `latest` is a byte copy of its signed candidate, so under `latest` the
  channel `candidate-<tag version>` is accepted too, for that tag only.
- **install.sh** (signature trust only) also refuses a manifest whose tag is
  older than `FORKMESH_MIN_RELEASE_VERSION`. The value is embedded in
  `install.sh`, an empty or malformed value refuses every manifest, and it must
  never be newer than the tag in `latest`
  (`test_installer_minimum_release_is_set_and_not_above_latest`). Raise it when
  a release must not be rolled back. A controller that pins a manifest SHA-256
  or an exact build commit has already chosen the exact release, so neither
  check applies there.
- **Desktop auto-update** (`maybeAutoUpdate`, `tryPrebuiltAutoUpdate`):
  - It orders tags by version number, not by tag date, and only looks at a tag
    newer than the running `FORKMESH_VERSION`.
  - The manifest must be signed, name `forkmesh/forkmesh`, match the tag and
    channel, and be strictly newer than the running version
    (`forkmesh::releasetrust::checkManifest`).
  - If no prebuilt fits, or its download fails, the source fallback
    fast-forwards only to the signed `tag_commit` and builds it once `HEAD`
    equals that commit. It never builds what `git pull` brings.
  - A release without a signed manifest, or one that fails a check, is logged
    and left alone. The manual Update button still pulls and builds by hand.
- **Release host** (`tools/forkmesh-release-host.py`): `manifests_at` drops
  any channel whose `release.json.sig` does not verify against the installer
  key. A candidate's `build_commit` must equal its `tag_commit` or descend from
  it.

Why: the auto-updater used to trust the newest-dated tag's unsigned
`release.json`. That included its `repo` field, which chose where the binary
was downloaded from. When no prebuilt fit, it built whatever `git pull`
returned. `install.sh` accepted any signed manifest from any channel or
version, so an old signed release could be replayed to roll a machine back.

## Platforms

- **Linux:** static Qt and OpenSSL in a Debian 12 container; a release fails if
  any ELF needs glibc above 2.35, so binaries run on Ubuntu 22.04+ and
  Debian 12+.
- **Windows:** cross-built with MinGW-w64; NSIS installer, Authenticode-signed
  with osslsigncode; the uninstaller deletes only files in its manifest.
- **macOS:** one ad-hoc signed universal `.dmg`, published under both arm64 and
  x86_64 names. Built on a dedicated Mac reached over SSH with a pinned host
  key, under a shared build lock, from `git archive` of the build commit; a
  cloud Mac build is the fallback only when the Mac cannot be reached. The
  image must carry the build commit and, for the Intel name, contain only
  universal Mach-O files. `FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS=1` and
  `--macos-dmg PATH` are manual escape hatches.
- **iOS:** an App Store app with its own version
  (`native/ios/Config/Version.xcconfig`); each desktop release submits the
  newest build once (recorded in `<work>/ios-vX.Y.Z.json`), with the annotated
  tag message as What's New.
- **Android:** `forkmesh-android-universal.apk`, signed with the
  direct-distribution keystore; versionCode must grow, so it is
  `(MAJOR*10000 + MINOR*100 + PATCH) * 100 + BUILD`.
- **Homepage:** `www/public/homepage-downloads.js` reads and verifies `latest`
  in the browser; the release host checks the served manifest after promotion.

## Build numbers

Each platform can ship a new build of the current version without a new tag,
so a fix reaches the homepage for one platform without a release.

- `--deploy-build PLATFORM` (`android`, `linux`, `windows`, `macos`) builds the
  next build number from `main`, publishes it into the newest candidate (or
  `--tag`), promotes it to `latest` when nothing else is missing, seeds the
  mirrors, merges the metadata to `main` and moves the tag. With `--dry-run`
  it only prints the plan. With `--first-build` it does nothing when the
  version already has a build for that platform.
- `--check-build PLATFORM` waits up to 30 minutes until app.forkmesh.com
  serves the promoted manifest. While the version is still held back for
  other platforms it reports that and succeeds.
- `--reset-builds` writes `builds.json` with every platform at 1 for the
  version; `--wait-candidate --tag vX.Y.Z` waits up to two hours for the
  release workflow's candidate manifest on `main`.
- These modes wait up to four hours for the timer's run to release the host
  lock instead of exiting.
- Build numbers live in `builds.json` and in each rebuilt asset's signed
  `build` and `commit` fields; the details and the reasons are in
  `.forkmesh/releases/README.md`, "Per-platform builds". The homepage shows
  `Build N` on a rebuilt download.

The shortcuts run these modes as named steps, so the desktop, phone and web
canvas show each operation:

| Shortcut | Steps |
| --- | --- |
| `Deploy Android build`, `Deploy Linux build`, `Deploy Windows build`, `Deploy macOS build` | Plan the build → Build, sign and publish → Check the homepage |
| `Deploy new version` | Cut the new version (`tools/forkmesh_release_request.py create`) → Wait for the release candidate → Reset build numbers to 1 → Deploy Android, Linux, Windows and macOS build (each runs the linked shortcut with `--first-build`) |

They are shortcuts rather than Actions workflows because they need the
release host's signing material, the office Mac and the primary checkout,
none of which the Actions sandbox can reach. Arguments given to the
new-version shortcut go to `forkmesh_release_request.py create` (for example
`--tag v1.3.0`).

## Mirror daemon rollout

`roll_out_release` in `tools/forkmesh-release-host.py` installs the promoted
`forkmesh-mirror-node-linux-x86_64` on managed mirror hosts listed in
`config/fanout/hosts`, all hosts in parallel. Core nodes are excluded (they
get the daemon through `tools/next_fleet.py`, which announces changes to the
IDS); a host whose unit is rendered by `deploy/core/render.py` answers
`next-managed` and is left alone. Per host: verify unit and config
ownership, skip if already current, verify the blob's sha256, keep the old
binary as `.bak-update`, swap, `--set-version`, restart, wait for health and
readiness, and restore the previous binary on failure. Outcomes are recorded
in `<work>/rollout-vX.Y.Z.json`; failed passes retry after a backoff. The
desktop's own rollout skips hosts that already run the release.

## Release store prune on nodes

Publishing, `seed_mirrors` and the desktop's push to managed hosts only ever
add blobs to `<gitDir>/forkmesh-releases`, and the desktop prunes only its own
store. On 2026-10-16 the n2 and n3 stores had reached 8.8 GB (about 5 GB a
month) with their disks at 85%, so the Go daemon now prunes its own stores
(`server/releaseprune.go`).

The sync loop runs it five minutes after start and then every
`releasePruneInterval` (default `6h`, 10m to 168h). Per release store (aliases
sharing one store are pruned once, with the union of their manifests):

- The referenced set is every `assets[].blob_sha256` in every
  `.forkmesh/releases/<channel>/release.json` at the repository's `HEAD`.
- If `HEAD` is unborn, a manifest cannot be read or parsed, an asset digest is
  malformed, or the set is empty, nothing is deleted.
- If the `HEAD` commit is older than `releasePruneMinAge`, nothing is deleted:
  a node whose sync has stalled would otherwise read an old channel list and
  delete blobs that only a newer `HEAD` references.
- A referenced blob missing from the store is counted (the log's "referenced
  not in the store") but does not stop the run. Until 2026-10-23 it did, and
  the store never shrank: n3 lacked 5 blobs that v1.2.2 and v1.2.6 name, and n4
  lacked all 87 referenced blobs. n2 and n3 reached 12 GB and 98% disk. A blob
  landing for a publish in progress is still safe, because nothing younger than
  `releasePruneMinAge` is a candidate.
- Only `sha256/<aa>/<64 lowercase hex>` directories whose name starts with
  `<aa>` are candidates, and only when the directory and every file in it are
  older than `releasePruneMinAge` (default `168h`, at least `24h`), because
  `seed_mirrors` lands blobs before their metadata.
- Deletes go through `removeTreeUnder(<store>/sha256/<aa>, <hash dir>)`; an
  emptied shard is removed with `os.Remove`. At most 200 blobs per store per
  run; each run logs one `release store prune` line per store with the count
  and bytes.

Releases dropped from every channel at `HEAD` lose their blobs after the
minimum age, so an old version stays downloadable by hash only while some
channel still names it.

Old candidates are retired by removing their `.forkmesh/releases/candidate-*`
folders on `main`; nothing removes them on its own. On 2026-10-25 the 16
candidates from 0.7.95 to 1.2.8 held 4.9 GB in every store, 4.3 GB of it named
by no channel but themselves. `candidate-0.7.95` through `candidate-1.2.7` were
removed, keeping `latest` and `candidate-1.2.8`, the one it was promoted from.
`tools/forkmesh-release-host.py` never revisits a candidate older than
`latest`, so removing older ones changes no rollout or completion.

## Protocols

- `release.json`: `schema`, `repo`, `tag`, `tag_commit`, `build_commit`,
  `channel`, `created_at`, `checksums_sha256`, `assets[]` (`name`,
  `blob_sha256`, `size`, `os`, `arch`, and on a per-platform rebuild `build`
  and `commit`). `release.json.sig` is a raw 64-byte
  Ed25519 signature over the exact file bytes.
- `tag_commit` is the commit the tag pointed to at signing; verifiers check
  `git merge-base --is-ancestor <tag_commit> <tag>`. An empty `build_commit` is
  left out of the signed header so legacy manifests keep their bytes.
- Asset names: `forkmesh-<os>-<arch>[.exe]`, `.AppImage`, `.dmg`,
  `-setup.exe`, `forkmesh-mirror-node-linux-x86_64`,
  `forkmesh-android-universal.apk`.
- `appcast.xml` is generated on promotion but not read by the desktop;
  Windows and macOS update by installing the newer installer.

## Configuration

Names only:

- Actions: `FORKMESH_RELEASE_SIGNING_KEY_PEM`, `FORKMESH_RELEASE_REQUIRED_ASSETS`,
  `FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS`, `FORKMESH_WIN_CERT_PFX`,
  `FORKMESH_WIN_CERT_PASSWORD`.
- Publisher: `FORKMESH_RELEASE_CAS`, `FORKMESH_RELEASE_SIGNING_KEY`,
  `RELEASE_CHANNEL`, `FORKMESH_TAG`, `FORKMESH_TAG_COMMIT`,
  `FORKMESH_BUILD_COMMIT`.
- Release host: signing material under `<data>/release/`
  (`FORKMESH_ANDROID_KEYSTORE`, `FORKMESH_ANDROID_KEYSTORE_PASSWORD_FILE`,
  `FORKMESH_WIN_CERT_PFX_FILE`), `CODEMAGIC_API_TOKEN`, `CODEMAGIC_APP_ID`,
  the build Mac's key and pinned host key under `<ssh>/mac-build/`,
  `MAC_BUILD_HOST`, `MAC_BUILD_GIT_URL`.
- Build: `-DFORKMESH_BUILD_COMMIT_OVERRIDE`, `-DFORKMESH_VERSION_OVERRIDE`.
- Installer: `FORKMESH_PUBLIC_HOSTNAME`, `FORKMESH_LINK_CODE`,
  `FORKMESH_DNS_WAIT_SECONDS` ([self-setup-node.md](self-setup-node.md)),
  `FORKMESH_RELEASE`, `FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE`,
  `FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256`, `FORKMESH_FROM_SOURCE`.

## Build, run, test

```sh
python3 tools/forkmesh-release-host.py --tag vX.Y.Z --dry-run
python3 tools/forkmesh-release-host.py --auto --rollout-only --dry-run
python3 tools/forkmesh-release-host.py --tag vX.Y.Z --ios-only
python3 tools/forkmesh-release-host.py --auto --skip-ios
python3 tools/forkmesh-release-host.py --deploy-build android --dry-run
python3 tools/forkmesh-release-host.py --reset-builds --tag vX.Y.Z --dry-run
python3 tools/forkmesh-release-promote.py --candidate candidate-X.Y.Z --dry-run
python3 desktop/tests/validate_release_version_stamp.py
tools/package/linux-baseline.sh check <binary> <AppImage>
python3 -m pytest app/tests/test_linux_release_baseline.py app/tests/test_install_lifecycle.py app/tests/test_installer_source.py app/tests/test_releases.py
python3 -m pytest app/tests/test_release_host_completion.py tools/test_ios_app_store_release.py
python3 -m pytest app/tests/test_release_host_builds.py app/tests/test_deploy_build_shortcuts.py
python3 -m pytest tools/test_forkmesh_release_host_rollout.py tools/test_forkmesh_release_host_seed.py
```

## Operations

- Cut a release from the desktop's Releases panel, the MCP `create_release`
  tool, or a phone request; the release host completes it within minutes.
- Watch it: `journalctl --user -u forkmesh-release-host.service -f`.
- Check a release: fetch `.forkmesh/releases/latest/release.json{,.sig}` and
  verify with the public key in `www/public/install.sh`.

## Security

- A prebuilt install trusts either the release publisher's Ed25519 key (built
  into `install.sh`, or a local PEM) or a manifest digest from an
  authenticated controller; a checksum copied from the fetched manifest is
  never trusted alone.
- Private signing keys, keystores and certificates never enter git or docs.
- Desktop fleet installs also pin `build_commit` and check `--version` and
  `--build-commit` on each host.
- `install.sh --uninstall/--reinstall/--purge` and `uninstall.sh --purge`
  refuse to erase anything outside `HOME` and refuse a user-data wipe from an
  agent shell. Agents must never run them (`AGENTS.md`).

## Traps

- **Manifest before tag:** the tag is moved onto the metadata commit; the
  updater rejects a manifest whose `tag` differs.
- **No-drop rule:** a candidate without both `.dmg` names is held back until a
  universal image exists; the host still publishes the other assets into the
  candidate and retries the macOS build after a backoff.
- **Mac build lock:** a killed host run leaves its remote step running; the
  next attempt of the same release takes the lock over, and stale locks expire.
- **Dirty tree:** any untracked file in the build worktree marks the build
  commit dirty.
- **Installer tests** must sandbox `HOME` and `XDG_*_HOME` and drop
  `FORKMESH_HOME`/`FORKMESH_PROJECTS_DIR`.
