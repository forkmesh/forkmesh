# Releases

This directory holds **release metadata only**. Release binaries are **not**
committed to git (issue #304; see
[`www/docs/design/release-binary-publishing.md`](../../www/docs/design/release-binary-publishing.md);
the developer overview is [`docs/release/README.md`](../../docs/release/README.md)).
The actual binary bytes live in a per-node content-addressed store (the CAS),
served on demand from a hosting node over the relay — the same way the repo
itself is served. The checksum list is bound into an Ed25519-signed manifest,
and the downloaded bytes are then verified by SHA-256.

> Historical note: releases used to commit the prebuilt binary directly under
> `.forkmesh/releases/<channel>/forkmesh-<os>-<arch>`. The installer still understands that
> layout as a fallback, but new releases should use the metadata-only model below.

## Layout

```
.forkmesh/releases/<channel>/
  SHASUMS256.txt   # "<sha256>  <asset-name>" per asset (sha256sum -c compatible)
  release.json     # signed manifest body (repo, revisions, checksum-list hash, assets)
  release.json.sig # raw 64-byte Ed25519 signature over exact release.json bytes
```

- `<channel>` — `latest` by default. The installer reads `FORKMESH_RELEASE` to
  pick a different channel; the publisher reads `RELEASE_CHANNEL`.
- Bare assets follow `forkmesh-<os>-<arch>[.exe]`; native desktop packages add
  `.AppImage`, `.dmg`, or `-setup.exe`. `latest` (v1.2.4) offers
  `forkmesh-linux-x86_64`, its `.AppImage` and `.AppImage.zsync`,
  `forkmesh-mirror-node-linux-x86_64`, `forkmesh-windows-x86_64-setup.exe`,
  `forkmesh-macos-arm64.dmg` and `forkmesh-android-universal.apk`.
- `apt/` is not a channel: it holds the signed apt repository metadata for
  `forkmesh-linux-x86_64.deb` in `latest`, rewritten on each promotion
  (`docs/release/apt.md`).

The macOS `.dmg` is built by the Codemagic `macos-desktop` workflow
(`codemagic.yaml`), not by the Actions job or the release host, and is
published into the candidate by hand with `tools/forkmesh-release-publish.sh`.
Because `latest` now offers it, a candidate without it is held back by the
no-drop rule below.

The binary bytes for each asset live in the serving node's CAS, addressed by the
same sha256 recorded in `SHASUMS256.txt`:

```
<mirror>/forkmesh-releases/sha256/<aa>/<full-hash>/data     # gitignored, never committed
```

## Publishing

Don't add binaries here by hand. Cut a release by triggering the **Attach desktop
build to release** action (`.forkmesh/release.yml`) from the Actions tab, or run
the publisher directly:

```sh
tools/forkmesh-release-publish.sh \
  --channel latest --tag v1.2.3 --repo forkmesh/forkmesh \
  --signing-key /secure/path/release-ed25519-private.pem \
  --cas-dir <mirror>/forkmesh-releases \
  forkmesh-linux-x86_64
```

This hashes each binary, copies the bytes into the CAS (`--cas-dir`, which must
be the directory the serving node reads from — set `FORKMESH_RELEASE_CAS` to it),
and writes the checksum list, signed manifest, and detached signature. The
private key must be an Ed25519 PEM file supplied through `--signing-key` or
`FORKMESH_RELEASE_SIGNING_KEY`; publication fails when it is missing. Commit
**only that metadata** and publish it — never commit the private key.

Native runners add their assets to `.forkmesh/releases/candidate-<version>` for
the same immutable tag and build commit. When more than one platform builds a
release, run the workflow sequentially on each after syncing the preceding
candidate metadata commit. The publisher merges same-provenance assets by name.
The workflow then runs `tools/forkmesh-release-promote.py`, which copies the
signed candidate metadata to `latest` and regenerates its appcast only when:

- every **required** asset is in the candidate. The set comes from the
  `FORKMESH_RELEASE_REQUIRED_ASSETS` variable (comma- or space-separated asset
  names). Unset, it is what the fleet builds today: `forkmesh-linux-x86_64`,
  `forkmesh-linux-x86_64.AppImage`, and `forkmesh-mirror-node-linux-x86_64`.
  A value that names nothing fails closed. A failed runner therefore leaves the
  previous release live.
- the candidate does not **drop** any asset the current `latest` offers, such as
  the hand-signed `forkmesh-android-universal.apk`. Publish those into the
  candidate with `tools/forkmesh-release-publish.sh --channel candidate-<version>`
  for the same tag and `--build-commit`, then run
  `tools/forkmesh-release-promote.py --candidate candidate-<version>` from the
  repository root. To withdraw an asset on purpose, set
  `FORKMESH_RELEASE_ALLOW_DROPPED_ASSETS=1` (or pass `--allow-dropped-assets`).

A held-back candidate still lands its metadata. The script exits 3 in that case
and 2 on a configuration or manifest error. `--dry-run` reports the decision
without writing anything.

### Completing host-built assets on the release host

The Actions sandbox has neither the Android toolchain and upload keystore
nor a Windows cross toolchain and the Authenticode certificate, so
every candidate it publishes lacks `forkmesh-android-universal.apk` and
`forkmesh-windows-x86_64-setup.exe`; because `latest` offers both, the gate
holds such a candidate back. The release host finishes those releases
automatically with `tools/forkmesh-release-host.py`, run every five minutes by
the `forkmesh-release-host.timer` user unit (install
`tools/release-host/forkmesh-release-host.{service,timer}` into
`~/.config/systemd/user/` and `systemctl --user enable --now
forkmesh-release-host.timer`; `tools/forkmesh-release-android.py` is a
compatibility shim for the older unit name). For the newest candidate held
back only for host-built assets it:

1. builds and signs each missing asset at the release's build commit: the APK
   with versionCode `(MAJOR*10000 + MINOR*100 + PATCH) * 100 + BUILD` (v1.5.0
   build 0 is 1050000; releases up to v1.2.8 used the code without the build,
   v1.2.8 is 10208) so it installs over the previous build, signed with the v3 direct-distribution
   key `android-direct-distribution-v3.jks` in the same release folder (since
   v1.2.3; phones on a v2-signed APK reinstall once), and the Windows installer by
   cross-compiling the desktop client with MinGW-w64 and an official Qt
   win64_mingw kit (`tools/package/install-windows-toolchain.sh` provisions
   both without root), packaging it with NSIS, and Authenticode-signing it
   with osslsigncode using `windows-authenticode.pfx` in the release folder
   (`<data>/release/` from `tools/forkmesh_paths.py`, which is
   `~/projects/forkmesh/data/release/` on the projects layout; the same certificate, base64-encoded, is stored
   as `FORKMESH_WIN_CERT_PFX` / `FORKMESH_WIN_CERT_PASSWORD` in Settings ->
   Secrets & Coves for a native Windows Actions runner);
2. publishes them into the candidate, runs the gate, and verifies the promoted
   `latest` signature against the key in `www/public/install.sh`;
3. seeds every asset into each managed mirror's release store (hosts from
   `config/fanout/hosts` in the project folder, or `--hosts-file`), hash-verified, before the metadata is
   visible anywhere;
4. merges the metadata onto `main` and moves the release tag onto that commit.

Run it by hand with `--tag vX.Y.Z` (add `--dry-run` to see the plan). The
desktop notices the signed manifest within minutes, finishes the mirror
publication, and restarts itself onto the release through the usual restart
countdown.

The **Attach desktop build to release** action runs inside the Actions sandbox,
which mounts no part of the host filesystem — a key *path* there would name
nothing. Store the key material itself in the `FORKMESH_RELEASE_SIGNING_KEY_PEM`
variable (Settings → Actions → Variables / secrets); the job materializes it
`0600` inside the run, signs with it, and deletes it when the step ends. The
variables dialog is single-line, so a PEM pasted with literal `\n` escapes is
accepted as well as raw multi-line PEM (importing the variables JSON keeps the
newlines). The job verifies the key can produce a 64-byte Ed25519 signature
**before** starting the build, so a missing or wrong key fails in seconds
instead of after the whole client has compiled.

### Per-platform builds

A platform can ship a new build of the current version without a new tag.
`tools/forkmesh-release-host.py --deploy-build android|linux|windows|macos`
rebuilds that platform from `main`, publishes it into the version's candidate
and promotes it to `latest`; the `Deploy <platform> build` shortcuts run it.

- Build numbers start at 0 for every version (1 before v1.5.0). `builds.json`
  in this folder records the number each platform is on (`{"tag", "builds"}`),
  and `--reset-builds` sets all of them back to 0 for a new tag. The first
  build of a version is build 0, so its APK versionCode ends in `00`.
- A rebuilt asset carries `"build": N` and `"commit": "<sha>"` in its
  `release.json` entry (`--build` / `--asset-commit` on the publisher). The
  manifest's `build_commit` stays the release commit; the other assets keep
  their own entries. Assets without `build` are build 0 at `build_commit`.
- The next number is one above both `builds.json` and the highest `build` in
  the signed manifest, so a reset never hands out a number again and the
  Android versionCode only grows. Build 99 is the last of a version.
- The first build of a new version is the ordinary completion at the
  release's build commit; promotion still waits for every asset `latest`
  offers.
- A macOS rebuild needs the office Mac: Codemagic builds only the tag.
- A commit-pinned install (`FORKMESH_EXPECTED_BUILD_COMMIT`) checks the
  asset's own `commit` when it has one.

### Refreshing a same-version binary

Do not delete a checksum line or copy a local development executable into the
CAS. When fixes must ship under the current app version, first commit the exact
source to publish, then run the explicit refresh from a clean worktree:

```sh
FORKMESH_RELEASE_CAS=/absolute/path/to/the/served/forkmesh-releases \
  app/deploy.sh republish-release-binary
```

The command refuses a dirty tracked worktree or an unspecified served CAS. It
rebuilds the current platform in Release mode, pins the version from
`desktop/CMakeLists.txt`, verifies the executable's `--version`, replaces the
content-addressed bytes, and checks that `release.json`, `SHASUMS256.txt`, the
CAS hash, and the source commit all agree. It then commits the small release
metadata update. Push that commit and let mirror catalogs refresh before using
the desktop client's fleet binary-install action.

## Installing

The installer autodetects the platform, authenticates `release.json` against a
locally provisioned publisher public key (or an exact controller-supplied
manifest digest), verifies its binding to `SHASUMS256.txt`, then downloads the
matching content-addressed binary and verifies its SHA-256 before installing.
It never executes downloaded bytes to decide whether they are trustworthy:

```sh
FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE=/etc/forkmesh/release-publisher.pem \
  bash install.sh
```

Fleet controllers may instead set
`FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256` to a digest obtained through their
authenticated control channel. When neither is set, `install.sh` uses the
ForkMesh release publisher public key built into the script; the private key
is never shipped.

The desktop client's **Install from binary (all hosts)** path adds a stricter
provenance pin: it supplies the source commit embedded in the controller binary,
requires an identical `release.json/build_commit`, then checks both `--version`
and `--build-commit` on every installed host. Thus a checksum-valid artifact
published under the same semver but built from an older commit fails closed.
Ordinary one-line installs remain unpinned and keep following the selected
release channel.

Release builds configured from a Git checkout automatically embed the newest
non-release-metadata source commit. Packagers building from a source archive
must pass the manifest's exact commit with
`-DFORKMESH_BUILD_COMMIT_OVERRIDE=<40-or-64-hex-commit>`; without a known
revision the application still runs, but commit-pinned fleet deployment is
intentionally unavailable.

The `project(ForkMesh VERSION …)` bump must be committed before creating a
release tag. Release automation verifies that the tag version and committed
project version agree and never edits source after building the artifact; this
ensures a clean controller checkout can request the exact published revision.

Here “source commit” means the newest commit that changes anything outside
`.forkmesh/releases/`. The small manifest/checksum commit necessarily lands
after the artifact has been built, so excluding that metadata-only commit keeps
the controller and artifact on one stable provenance revision without weakening
the check for later source changes.

### What `tag_commit` means

This is the one definition of `tag_commit`; other code and docs refer here.

`tag_commit` is the release-cut commit: the commit `tag` peeled to when
`release.json` was signed. `tools/forkmesh-release-publish.sh` refuses to sign
unless `refs/tags/<tag>^{commit}` equals it at that moment.

It is **not** the commit the published tag peels to. After signing, the tag is
moved forward onto the publish commit that carries `.forkmesh/releases/`, because
updaters read the manifest at the tag. A commit cannot record its own hash, so in
every published release the peeled tag differs from `tag_commit` (for v1.2.4,
`tag_commit` is `3b50dc56…` and the tag peels to the publish commit `a066cb1c…`).
The publish commit is made on `main`, so `tag_commit..<tag>` can include
unrelated source commits besides the metadata.

The invariant a verifier checks is ancestry:

```bash
git merge-base --is-ancestor <tag_commit> <tag>
```

The tree at the tag is not the artifact's source. `build_commit` is the separate,
signed revision embedded in the binaries; it usually equals `tag_commit` and may
advance during an explicit same-version rebuild without moving the release tag.

Force a from-source developer build with `FORKMESH_FROM_SOURCE=1`. Normal
installs never fall back to compilation: if no authenticated prebuilt exists,
the installer fails quickly and points to the native downloads page. The legacy
fallback can be deliberately restored with `FORKMESH_NO_SOURCE_FALLBACK=0`.

## Download endpoint

```
GET /api/repo/<owner>/<repo>/releases/blob/sha256/<sha256>?filename=<url-encoded-asset-name>
```

Stable, content-addressed, and immutable — cached forever at the edge. The worker
forwards it to a node holding the blob, which streams the bytes back. Browser and
updater links should pass the signed manifest asset name in `filename` so the
response preserves its extension. Machine clients that choose their own output
path may omit the query parameter; it never changes the selected bytes.
