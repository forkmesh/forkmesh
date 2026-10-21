# Native iOS builds on the office Mac

Every push that touches `native/ios/` (or `codemagic.yaml`) builds the phone
app on the office MacBook and emails a tap-to-install link for the team's
registered iPhones. No Codemagic minutes and no TestFlight review are used. The
same Mac also builds tagged releases, App Store submissions of `main`, and the
macOS desktop `.dmg`. Codemagic remains the fallback ([IOS_RELEASE.md](IOS_RELEASE.md)).

## Pieces

| Piece | Where | Does |
|---|---|---|
| `.forkmesh/mac-native-build.yml` | ForkMesh Actions, pinned to the operator desktop (`runs-on: forkmesh`, `checkout: false`) | Finds the Mac, takes its build lock, has the Mac fetch the commit, runs each Codemagic step as its own Actions step, brings the IPA and logs back as artifacts (`data/actions/artifacts/<repo>/<run>/mac-adhoc/<commit>/`) |
| `native/ios/tools/codemagic_steps.py` | Read from the Mac's checkout | Writes `codemagic.yaml`'s `ios-native-adhoc` scripts as numbered shell steps plus the workflow's `vars`, so the Mac runs exactly what Codemagic runs |
| `native/ios/tools/mac_adhoc_build.sh` | Mac | Loads the App Store Connect key from `~/.asc`, runs the steps in order with `CM_BUILD_DIR` set; a fourth argument names the step to stop after, and `<steps>/done` counts finished steps |
| `native/ios/tools/mac_adhoc_job.sh` | Mac | Starts one step's build detached from ssh, or reconnects to it, and streams its log until it exits |
| `native/ios/tools/xcodebuild_live.awk` | Mac | Shrinks an xcodebuild log to one line per build action plus every distinct warning, error and `** … **` line (under the 1 MiB an Actions step may print) |
| `native/ios/tools/run_with_deadline.sh` | Mac | Runs a command with a deadline, killing its whole process tree (`pgrep -P`) on timeout; used by "Test ForkMeshCore" |
| `tools/mac_adhoc_deploy.py` | Operator desktop | The shortcuts below, plus `--status`, `--stop` and `--unstick` |
| `tools/mac_adhoc_publish.py` | systemd `--user` timer on the operator desktop | Seeds the newest unsent IPA and an OTA `manifest.plist` into the cores' release-blob store and emails the install link |
| `deploy/operator/forkmesh-mac-adhoc-publish.{service,timer}` | `~/.config/systemd/user/` | Runs the publisher every 15 seconds |
| `tools/office-mac.sh` | Operator desktop | Logs in to the Mac as `build`, runs a command, or prints its address (`--find`) |

The publishing half runs outside Actions because it needs the fleet SSH key,
which never goes into an Actions variable. The action only holds a key that
can log in as `build` on the Mac.

## The Mac

- An Intel MacBook Pro running macOS Sequoia through OpenCore Legacy Patcher
  (SIP `0x803`, SecureBootModel off, as the root patches need).
  `/Library/LaunchDaemons/com.forkmesh.autopatch.plist` runs
  `/usr/local/libexec/fm-autopatch.sh` at boot: on the first boot of a new
  macOS build it re-applies the root patches once and reboots (log in
  `/Users/Shared/fm-autopatch.<build>.log`), which restores Wi-Fi after an
  update provided the KDK for that build is reachable or cached.
- One admin account, `build`. Xcode 26 with the iOS platform and the Metal
  toolchain.
- The system `xcodeproj` gem cannot read Xcode 26 projects, so xcodeproj 1.27
  lives in its own gem home `~/.gems-xcodeproj` (CFPropertyList pinned to
  3.0.6); `mac_adhoc_build.sh` puts it first.
- The App Store Connect API key is in `~/.asc/key_id`, `~/.asc/issuer`,
  `~/.asc/AuthKey.p8` (0600).
- `build` never logs in at the screen, so `mac_adhoc_build.sh` creates an
  empty-password `build.keychain`, makes it the default and unlocks it each
  run. It holds nothing: signing happens in the cloud.
- The Mac never sleeps (`pmset disablesleep 1`), has wake-on-LAN on, and runs
  SSH from boot (FileVault off). `ComputerName` "ForkMesh Mac",
  `LocalHostName` `forkmesh-mac`.
- Step scripts run `native/ios/tools/*.py` with the Mac's `/usr/bin/python3`
  (3.9). A tool that uses `X | None` annotations must start with
  `from __future__ import annotations`; check with
  `ast.parse(src, feature_version=(3, 9))`.

### Source tree

- The source lives in one fixed folder, `~/ci/tree`, because Xcode keys
  DerivedData by project path. It is a partial (`blob:none`), sparse checkout
  of `native/ios/`, `codemagic.yaml`, `conformance/`, and the files outside
  them that the ForkMeshCore tests read (`app/src/apns.py`,
  `app/src/app_runtime.py`, the vault, people and secure-drive fixtures under
  `marketplace/packages/*/tests/fixtures/`, and the two performance playbooks
  `docs/native/PERFORMANCE_CHECKS.md` and
  `docs/marketplace/video-mix-dj-studio-performance.md`).
- `test_the_mac_checkout_holds_every_file_the_core_tests_read` in
  `tools/test_mac_adhoc_deploy.py` finds those files itself: in every test file
  that builds the repository root from `#filePath` (or calls a helper defined
  in one), each string literal or `Type.constant` from `Sources/` passed as a
  call's first argument that names an existing repository path with a `/` in
  it. Each must be covered by `MAC_IOS_FETCH_SCRIPT` in
  `tools/forkmesh-release-host.py`, so a new test that reads a file outside
  `native/ios/` fails here instead of on the Mac.
- The 2026-10-18 04:20 App Store build of `f123cb5c0dd8` failed after 19
  minutes in "Test ForkMeshCore": `DiagnosticsBundleTests` reads
  `docs/native/PERFORMANCE_CHECKS.md`, which the sparse checkout did not hold
  (`NSCocoaErrorDomain Code=260`). The guard test then listed its paths by
  hand and missed it; `DjVitalsTests` (the DJ studio playbook) would have
  failed the next build the same way, and `SecureDriveInteropTests` skipped
  silently without its fixtures.
- Each run fetches its commit at depth 1 from `MAC_BUILD_GIT_URL` and checks it
  out with `git clean -ffdx`. Fetch and checkout retry together (six times,
  20 s apart) because a core may advertise a commit before it holds every blob,
  and the checkout fetches the blobs lazily: on 2026-10-14 it died with
  `fatal: early EOF` / `could not fetch ... from promisor remote` while the
  fetch alone was retried.
- When the fetch fails, the step asks the cores for `main` and prints one
  `ERROR:` line naming the cause (commit not published yet, broken on the
  Mac's side, or the cores unreadable from the desktop too). The "Action
  failed" ping shows it as its `Cause:`.
- Each run's steps and output go in `~/ci/<commit>-<time>/`; the two newest are
  kept.

### Lock and scheduling

- `~/ci/lock` serializes builds; a lock older than four hours is stale.
- Only the newest iOS ad hoc build runs. Each run writes its name to
  `~/ci/ios-latest`; a newer `ci/*` run kills the older build's process tree
  (`~/ci/lock/pid`) and takes the lock. Superseded runs report "Cancelled" and
  succeed.
- Locks held by anything that is not a `ci/*` run (`desktop-release`,
  `ios-release`, `ios-app-store`, `desktop-build`) are waited for, never
  killed, for up to two hours. A run removes the lock only while it owns it
  (`~/ci/lock/owner`).
- Several shortcuts can run at once; each runs in its own unit and they take
  turns through the lock.

## Finding the Mac

The address comes from DHCP. The workflow's first step tries the hint address
(`MAC_BUILD_HOST`, when configured), then scans the desktop's /24 for
port 22 and tries ssh on each open host. ssh checks the host key by the
`forkmesh-mac` alias (`HostKeyAlias`, `StrictHostKeyChecking=yes`), so a
machine with a different key is refused before the key is offered. mDNS is not
used: the desktop's firewall drops the Mac's unicast replies and the Actions
sandbox cannot reach Avahi.

`tools/office-mac.sh` and the release host (`find_build_mac`) share the last
found address in `~/projects/forkmesh/state/mac-build/address`. After the Mac
has been off or away, `tools/office-mac.sh --find` checks it is back.

## Steps and live output

- The run shows: Find the build Mac, Take the Mac's build lock, Fetch the
  commit on the Mac, one step per `ios-native-adhoc` script, then Bring the IPA
  and logs back. A step the build leaves out prints "Skipped". The last step
  runs any Codemagic step no workflow step names;
  `tools/test_mac_native_build_workflow.py` fails until the workflow lists it.
- State between steps (key, Mac address, run name) lives in the sandbox home
  under `mac/`; `mac/lib.sh` holds the ssh helpers. A step script reaches bash
  on stdin, so `mac` calls `ssh -n`; `mac_in` is the one that sends data.
- `on.push.paths` lists `native/ios/*` down to ten levels, because the desktop
  matches paths with Qt wildcards where `*` never crosses `/`. A manual run
  always builds.
- `ios-native-release` hands values between steps through `$CM_ENV`;
  `mac_adhoc_build.sh` provides a `cm_env` file whose `KEY=VALUE` lines are
  exported before every step, and sets `CM_COMMIT`.

### Surviving a desktop restart

- `mac_adhoc_job.sh <run> <step>` starts the step detached (`nohup` plus
  `setsid` through perl) in `~/ci/<run>/jobs/<n>/` (`log`, `pid`, `started`,
  `status`), then follows `log` until `status` appears. A later call prints
  "Reconnected", replays the log and keeps following. A job whose process is
  gone without a `status` fails the step; `started` guards against PID reuse.
- A re-queued Actions run keeps its run number, finds the lock under its own
  owner (`actions-run-<FORKMESH_RUN_ID>-<commit>`), and resumes that run's
  folder (`resumed=1`), reading `started_at` and `locked_at` back from
  `~/ci/<run>/times`.
- If ssh drops (exit 255) the step reconnects every 30 s, up to 20 times.
- Stopping an Actions run from the desktop stops only the desktop side; the Mac
  finishes its step and the lock stays held until the next iOS run cancels it.

## The shortcut

`.forkmesh/shortcuts/deploy-native-ios-ad-hoc-mac.sh` ships `main` as an ad hoc
build on the Mac. It runs `tools/mac_adhoc_deploy.py --execute` (without
`--execute` it only reports `main`, what the cores serve and where the Mac is).

- It waits up to 30 minutes for `MAC_BUILD_GIT_URL` (default
  `https://app.forkmesh.com/forkmesh/forkmesh`) to serve this checkout's `main`
  or newer, and builds the served commit.
- It finds the Mac like the release host (key and pinned host key in
  `~/projects/forkmesh/ssh/mac-build/`) and does not go through Actions.
- It behaves like an Actions ad hoc run on the Mac: writes `~/ci/ios-latest`,
  takes the lock (`LOCK_SCRIPT`), cancels older `ci/*` builds and waits for
  releases. A newer build cancels it; it then exits 0 with "cancelled".
- It fetches into `~/ci/tree`, writes the steps from the Mac's own
  `codemagic.yaml`, and runs them through `mac_adhoc_job.sh`.
- Stopping it (Stop, Ctrl-C, `--stop`) kills the Mac's build tree through
  `~/ci/lock/pid` and releases the lock in one ssh call (`STOP_SCRIPT`), run in
  its own session so it completes even after the tool is killed.
- The IPA and logs come back to
  `~/projects/forkmesh/data/mac-adhoc/shortcut/<time>/mac-adhoc/<commit>/` with
  a `build.json`; the tool then publishes in-process
  (`mac_adhoc_publish.py --execute --artifacts ~/projects/forkmesh/data/mac-adhoc`).
  That folder is not the Actions artifacts folder, so the timer never sends
  these builds twice.

### Terminal output, timing and the failure Ping

- `--execute` opens with a banner (mode, source, start time, the tool's commit
  and Python, the unit log), then prints each phase under a numbered rule
  (wait for the cores, find the Mac, take the lock, fetch, write the steps,
  build, bring the IPA back, publish) and closes it with `✓`/`✗` and its time.
- Finding the Mac prints its macOS and Xcode versions, free disk and uptime
  (`MAC_FACTS_SCRIPT`). The step list is printed numbered.
- `mac_adhoc_build.sh` prints a `--- build Mac:` line (versions, free disk,
  commit, first step) and, after each step, `<== <step> ok in N s` or
  `!! <step> failed after N s`. The tool reads those lines, so step times are
  the Mac's own and survive a reconnect replaying the log.
- Every run ends with a Timing table: each phase, the Mac's steps under the
  build phase, and the total.
- The Mac's output passes through `relay`, which keeps every distinct line with
  `error:`, `fatal:`, `ERROR:` or `!! ` (paths under `~/ci/tree/` shortened).
  A failure prints a `✗ FAILED in "<phase>"` box with those lines (compiler
  errors first; else the failing `mac.run`'s `fatal:`/`ERROR:` lines) and a
  `Fix:` hint from `FAILURE_HINTS` (Swift error, fetch from the cores, cores
  behind, Mac away, lock held, no IPA, CoreSimulator wedged).
- actool can fail the watch asset catalog with `Failed to launch
  AssetCatalogSimulatorAgent via CoreSimulator spawn` (the 2026-10-16 ad hoc
  builds of `ad97f78c7fd4` and `665987101809`, Mac up 6 days; the 19:40 build
  that day had passed). The cause was not the service: dyld could not load
  `UIKitCore` from the mounted watchOS 26.5 runtime
  (`F_ADDFILESIGS_RETURN) failed with errno=94`), and `dd` of that file from
  `/Library/Developer/CoreSimulator/Volumes/watchOS_23T570` stopped at 61 MiB
  with `Bad message`, the sealed volume's hash check. The backing
  `WatchOSSimulatorRuntime_Cryptex.dmg` under `/System/Library/AssetsV2` read
  cleanly, and `xcrun simctl runtime unmount <id>` followed by a
  CoreSimulatorService restart and `xcrun simctl runtime scan-and-mount`
  mounted it again with the file whole: the damage lived in the mount, not
  the image. Restarting the service alone, as the first retry did, left the
  bad mount in place. An unmounted runtime is not remounted by `simctl list`
  or `runtime verify` (verify then fails with -67068); actool reports `No
  available simulator runtimes for platform watchsimulator` until
  `scan-and-mount` runs.
- The archive step greps its log for that line, prints the
  `F_ADDFILESIGS_RETURN` file if there is one, shuts the simulators down,
  unmounts every runtime disk image (`simctl runtime list` ids), kills the
  service, runs `scan-and-mount`, waits up to a minute for as many
  `CoreSimulator/Volumes` mounts as before (about 7 s on the office Mac) and
  archives once more. A second failure stops the build with a `Fix:` to run
  again or reboot the Mac.
- Compiler crashes: see the section below.
- The same text goes to the admins as an operator Ping (`notify_failure`, key
  `mac-adhoc-failure`, `POST /api/accounts/admin-operator-alert` on the
  `MAC_BUILD_GIT_URL` host, signed with this desktop's node identity like
  `next_autodeploy.Notifier`): title `<mode> failed: <phase>`, then the cause,
  the fix, the commit, the Mac, the Mac run folder, the log and
  `--status`. Stop, Ctrl-C, a cancelled run and a dry run send nothing; a Ping
  that cannot be sent is logged and does not change the exit code.

### Compiler crashes

The 2026-10-18 03:24 ad hoc build of `7feff82345c7` failed after 19 minutes
because `swift-frontend` (Swift 6.3.3, Xcode 26.6) crashed while optimizing the
whole-module `ForkMesh` compile: LLVM's `lower-expect` pass on the value
witness of `AgentRunActionChip` (`AgentRunScreen.swift`) called a garbage
address. That file had not changed since the 02:23 build of `390f439cc869`,
which passed, and an unsigned archive of the same tree right after succeeded
in 13 minutes: the crash does not repeat. A crash prints no `error:` line, so the step's "Every error"
grep found nothing and the Ping showed only `!! Archive … failed`.

- The signed archive step looks for the crash banner (`submit a bug report …
  crash backtrace`, Swift's and Clang's), prints the stack's numbered lines
  (`Apple Swift version`, `Running pass … on function …`), keeps the log as
  `xcodebuild-archive-crash.log` and archives once more. The compilation cache
  replays every other module, so the retry recompiles only the crashed one.
- A failed archive (signed or unsigned) prints those lines as
  `ERROR: compiler crash: …`, which the tool keeps as the cause, and
  `FAILURE_HINTS` adds the fix: run again, and if the same commit crashes
  again, rewrite the function the stack names.

## The macOS desktop shortcut

`.forkmesh/shortcuts/build-desktop-macos-mac.sh` runs
`tools/mac_adhoc_deploy.py --desktop --execute`: the universal macOS `.dmg`
from the local checkout's `main`. It takes the lock as `desktop-build <commit>`,
feeds a `git archive` of `main` into `~/desktop-release/src-main`
(`MAC_DESKTOP_SOURCE_SCRIPT`) and runs `tools/package/macos_universal_on_mac.sh`
(`MAC_DESKTOP_BUILD_SCRIPT`). Every build is a full one; the Qt, OpenSSL,
libwebp and nghttp3 caches in `~/desktop-release` stay. Output is kept in
`~/projects/forkmesh/data/mac-adhoc/desktop/<time>/mac-build.log` and the
`.dmg` is copied next to it and checked against the build's `sha256` line.
Nothing is published. The phone's Desktops page can start this and the ad hoc
shortcut from the office MacBook row.

## The App Store shortcut

`.forkmesh/shortcuts/deploy-native-ios-app-store-mac.sh` runs
`tools/mac_adhoc_deploy.py --app-store --execute`: it does for `main` what the
release host does for a tag ([IOS_RELEASE.md](IOS_RELEASE.md), "App Store
rollout for a tagged release").

- Waits for the cores to serve `main`, finds the Mac, fetches into `~/ci/tree`.
- Writes the `ios-native-release` steps and appends `FORKMESH_IOS_APP_STORE=1`
  and `FORKMESH_IOS_WHATS_NEW` to the steps' `env.sh`; refuses a
  `codemagic.yaml` without a "Submit to the App Store" step.
- What's New is `--whats-new`, else `FORKMESH_IOS_WHATS_NEW`, else "Bug fixes
  and improvements." It must not mention membership or tips
  ([IOS_RELEASE.md](IOS_RELEASE.md), "App Review: nothing paid on iOS").
- Run folder `~/release/ios-main-<commit>-<time>/`; lock
  `ios-app-store <commit>-<time>`, which waits for any holder and is never
  cancelled by an ad hoc run.
- Succeeds only when the log holds the "Submitted ... for App Review:" line,
  then writes `~/projects/forkmesh/data/mac-adhoc/app-store/<time>-<commit>.json`.
  A stop after upload can leave an uploaded, unsubmitted build
  (`asc_appstore.py status`).

## Release builds

The release host (`tools/forkmesh-release-host.py`,
[`docs/release/README.md`](../release/README.md)) uses the same Mac, key and
address rules for `desktop-release vX.Y.Z` (the universal `.dmg` from a
`git archive` of the release commit in `~/desktop-release/src-<version>`) and
`ios-release vX.Y.Z` (the `ios-native-release` steps with App Store submission,
in `~/release/ios-vX.Y.Z/`). Both take `~/ci/lock` under those names.

## Builds outlive the desktop

`mac_adhoc_deploy.py --execute` starts the build in its own transient unit,
`forkmesh-mac-adhoc-<time>-<pid>.service` (`systemd-run --user --collect`,
`KillMode=mixed`, `IgnoreOnIsolate=yes`, `TimeoutStopSec=180`), so a desktop
restart or an agent scope stopping does not kill it. The unit gets only
`PATH`, `LANG`, `LC_*`, `MAC_BUILD_*`, `CODEMAGIC_*` and `FORKMESH_*` (minus
`FORKMESH_AGENT_*`), reads `main` from the checkout's shared `.git` (`--repo`),
and logs to `~/projects/forkmesh/logs/mac-adhoc/<unit>.log` with the exit code
in `<unit>.exit` (`FORKMESH_MAC_ADHOC_LOG`). The process the shortcut started
only follows that log.

| Event | Effect |
|---|---|
| Desktop Stop, Ctrl-C (SIGINT to the follower) | `systemctl --user stop --no-block <unit>`; the unit's SIGTERM runs `STOP_SCRIPT` |
| `--stop` | Stops every `forkmesh-mac-adhoc-*` unit and waits |
| `--unstick` | `--stop`, then kills every build process on the Mac and releases `~/ci/lock` |
| A newer ad hoc run | Cancels this one on the Mac; the unit exits 0 "cancelled" |
| Desktop quit or restart, terminal closed | Nothing: the follower exits 0 and the unit keeps building, publishing and emailing |

Without `systemd-run`, the tool builds in its own process, which a restart
still kills.

## A stuck build: status and unstick

- `swift test` starts `xctest` in its own process group, so a plain alarm
  misses it; `run_with_deadline.sh` kills the whole tree.
- Before a fresh build (not a resumed one) `mac_adhoc_build.sh` kills leftover
  `build`-user tools with parent 1 (`xctest`, `xcodebuild`, `swift-build`,
  `swift-test`, `swift-frontend`, `swift-driver`). `ps` runs with `-ww` so
  long paths are not cut.

| Command | Shortcut | Does |
|---|---|---|
| `tools/mac_adhoc_deploy.py --status` | Office Mac build status | Read only: local units with minutes since their log grew, the lock holder and whether its process lives, the newest job log on the Mac, and every build process with orphans marked |
| `tools/mac_adhoc_deploy.py --unstick` | Unstick the office Mac build | `--stop`, kill `~/ci/lock/pid`'s tree and every build process (SIGTERM, then SIGKILL after 5 s), release the lock, exit 1 if anything remains |

`BUILD_TOOLS` in `tools/mac_adhoc_deploy.py` defines a build process: the Xcode
and SwiftPM tools above, `swift-plugin-server`, `SWBBuildService`,
`XCBBuildService`, the `mac_adhoc_*`, `macos_universal_on_mac.sh` and
`run_with_deadline.sh` scripts, or any command line holding `~/ci/`,
`~/release/` or `~/desktop-release/`.

Up to 15 minutes without output at `CompileSwift [ForkMesh]` is normal (the
Release archive compiles the app module in one `swift-frontend`). An orphan, a
lock whose process is gone, or a log silent for 30 minutes is not: run
"Unstick", then start again.

## Compilation cache

`xcodebuild archive` deletes `ArchiveIntermediates` at its start, so a fixed
tree path alone does not make an archive incremental. With
`COMPILATION_CACHE_ENABLE_CACHING=YES`, each Swift and Clang compile is keyed
by its inputs and stored in
`~/Library/Developer/Xcode/DerivedData/CompilationCache.noindex`, which survives
archives. Unchanged modules are replayed; the app module (whole-module
optimization), linking, Metal shaders and dsymutil still run.

- The "Archive" step adds the setting when `FORKMESH_NATIVE_COMPILE_CACHE=1`.
- `mac_adhoc_build.sh` exports it as `1` unless set, so every Mac build uses
  it. Codemagic does not (its machines start empty).
- A cold archive takes about 20 minutes; the same commit again under two; a
  change to app code recompiles only the `ForkMesh` module.

## Damaged IPA guard

`mac_adhoc_publish.py` runs `zipfile.testzip()` on the IPA before seeding it
(`damaged_entry`). A damaged IPA is not published or emailed: the tool prints
the failing entry and exits 1. Build again to replace it.

## Actions variables

Under Settings -> Secrets & Coves on the operator desktop:

| Variable | Value |
|---|---|
| `FORKMESH_ACTIONS_ALLOW_NETWORK` | `1` (the Mac is on the LAN) |
| `MAC_BUILD_SSH_KEY_B64` | `base64 -w0 ~/projects/forkmesh/ssh/mac-build/id_ed25519` |
| `MAC_BUILD_KNOWN_HOSTS` | the `forkmesh-mac ssh-ed25519 …` line from `~/projects/forkmesh/ssh/mac-build/known_hosts` |
| `MAC_BUILD_HOST` | optional, the address to try first |
| `MAC_BUILD_GIT_URL` | optional, where the Mac fetches (default `https://app.forkmesh.com/forkmesh/forkmesh`) |

## Publisher

```sh
python3 tools/mac_adhoc_publish.py            # dry run: shows the build and the links
python3 tools/mac_adhoc_publish.py --execute  # seed and email every early iOS tester
python3 tools/mac_adhoc_publish.py --to a@example.com --execute  # these addresses instead
python3 tools/mac_adhoc_publish.py --ipa path/to/App.ipa --execute  # a specific IPA
install -m 0644 deploy/operator/forkmesh-mac-adhoc-publish.{service,timer} ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now forkmesh-mac-adhoc-publish.timer
```

- Recipients are the accounts flagged "Early iOS tester" in the admin console
  (`fmnext-app-ops early-ios-testers all`), one message each; they also get a
  ping linking the install page (`fmnext-app-ops ios-build-ping`). `--to` sends
  no ping. See [early-ios-testers.md](../app/early-ios-testers.md).
- The email states the build time from `build.json` (`buildSeconds`, and
  `waitSeconds` when a minute or more).
- After `sendmail` the publisher leaves a note in #email-log through a core's
  rqlite (`MAIL_LOG.md`).
- Sent builds are recorded in `state/mac-adhoc/published.json` by IPA sha256,
  per address, so a failed send resumes without repeats. Only the newest build
  is sent. The timer runs every 15 s with `AccuracySec=1s`; a build is taken
  only once its run's `meta.json` says `success`.
- Files are served from
  `https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/<hash>?filename=…`
  (`application/octet-stream`, attachment; serving HTML there would let any
  release seeder publish pages).
- The email links
  `https://forkmesh.com/ios-install.html?build=<CFBundleVersion>&v=<first 12 of the manifest sha256>#<manifest sha256>`
  (`www/public/ios-install.html`), because mail apps drop `itms-services:`
  links. The query makes each build's URL unique for caches. The page fetches
  the manifest from the same origin, shows title, version and build (only
  `0-9A-Za-z.-`, up to 32 characters), and only builds a link to this origin's
  blob route for a 64-hex hash.
- Ad hoc profiles cover the devices registered when minted; register a device
  ([IOS_RELEASE.md](IOS_RELEASE.md)) and build again.

## What's new changelog

Every build bundles the ForkMesh changelog, `changelog/changelog.json`, the
same file forkmesh.com/changelog shows ([../changelog.md](../changelog.md)).
The app shows it from the version row of the You hub
([NAVIGATION.md](NAVIGATION.md)).

- **iOS.** The step "Bundle the changelog" runs
  `native/ios/tools/changelog_stamp.py`, which exports the file from the
  build's own checkout (`tools/changelog.py export --platform ios`) into
  `ForkMesh/Resources/Changelog.json`. A missing or broken file ships no
  notes and never stops a build.
- **Android.** `-Pforkmesh.changelog=<file>` bundles the file as the
  `changelog.json` asset (`bundleChangelog`); otherwise the empty placeholder
  `app/src/changelog/changelog.json` ships. The release host's APK build
  passes `tools/changelog.py export --platform android`.

Until 2026-10-21 the shortcut (`mac_adhoc_deploy.write_changelog`) generated
each ad hoc build's notes from `git log` since the previous build in
`state/mac-adhoc/published.json` and sent them to the Mac as
`steps/changelog.json`; that history now lives in the shared file, and the
generator only drafts lines (`tools/changelog.py draft`).

## Swift rules the Mac archive enforces

Linux checks only parse the app target, so the archive is the first full
compile. Keep to these:

- The app targets iOS 17: use only APIs available there (for example the
  one-value `onGeometryChange`, and `getCurrentConfigurations` instead of the
  async `currentConfigurations()`).
- Keep SwiftUI modifier chains under roughly fifteen; split large `body`s into
  typed pieces (as `HomeShell` and `GenieSpiderLayer` do) to stay inside the
  type checker's budget. The 2026-10-17 ad hoc build failed on
  `ShellNewAgentButton.body`, a 25-modifier chain with a ternary hint and an
  `any Tip` ternary; it now builds in `pressable`, `described`, `tipped` and
  `body`, with the hint as a plain computed property.
- Pass `popoverTip` a concrete tip type, never `any Tip`: an existential picks
  the iOS 26 overload (`popoverTip(_:isPresented:attachmentAnchor:arrowEdge:action:)`)
  and the archive fails on availability. To choose between tips, branch in a
  `@ViewBuilder` with one `popoverTip` per tip (the 2026-10-17 ad hoc build of
  `6e0b9032db` failed on `ShellNewAgentButton`'s `any Tip` property).
- `FMDesktopRemote` and `FMInsights` both declare `DesktopHealth`; a file that
  imports both must qualify it.
- Main-actor APIs called from static helpers or from a plain struct's `init`
  need that helper or `init` marked `@MainActor` (the 2026-10-14 ad hoc build
  failed on `PackageUninstallRequest.init` reading `AccountPackagesStore`).
- `JSONValue` literals take only integer literals; wrap computed `Int`s as
  `JSONValue(x)`.
- `JSON.encode` takes a `JSONValue` or a `JSONObject`, and a bare dictionary
  literal fits both; write `JSON.encode([...] as JSONObject)` (the 2026-10-15
  ad hoc build failed on `MiniAppClient.cachedApps`).
- `WidgetRoute.url` returns `URL?`, which `Link(destination:)` does not take;
  widgets wrap a tappable row in `WidgetRouteLink(route) { … }`, which links
  only when the route has a URL (the 2026-10-15 ad hoc build failed on
  `SummaryLiveActivity`'s agent, shortcut and timer rows).
- A bare `catch` binds an immutable `error` that shadows a view's
  `@State var error`; assign the property as `self.error = …` inside the
  `catch` (the 2026-10-15 ad hoc build failed on `ProcessDetailScreen` in
  `DiagnosticsScreens.swift`).
- Linux never compiles the `#if canImport(Network)` or `canImport(Security)`
  code or the Apple-only targets in `native/ios/Nexus`, so check them on the
  Mac for iOS (`xcodebuild -scheme Nexus-Package -destination
  generic/platform=iOS build CODE_SIGNING_ALLOWED=NO` in the package;
  `NexusUI` is iOS-only and does not build for macOS). Call `withTimeout(_:operation:)` without a label, give a
  `Void` `withCheckedThrowingContinuation` an explicit
  `CheckedContinuation<Void, Error>`, and describe an `NWError` with
  `error.debugDescription` (its POSIX, DNS and TLS codes have none). Every
  `testTarget` path must exist, or SwiftPM rejects the package.
  `SecCertificateCopyValues` is macOS-only; read certificate dates with
  `SecCertificateCopyNotValidBeforeDate`/`AfterDate` behind
  `#available(iOS 18, …)`. The 2026-10-17 ad hoc build at `6f4d8e7d60`
  failed on `NetworkTransport.swift` for these reasons; the same check then
  found the missing `Tests/NexusStoreTests` and `WebInspector.swift`.
