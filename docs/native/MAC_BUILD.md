# Native iOS ad hoc builds on the local Mac

Every push that touches `native/ios/` (or `codemagic.yaml`) builds the phone
app on the office MacBook and emails a tap-to-install link for the team's
registered iPhones. Codemagic minutes are not used, and there is no TestFlight
review. Set up on 2026-09-27.

## Pieces

| Piece | Where | Does |
|---|---|---|
| `.forkmesh/mac-native-build.yml` | ForkMesh Actions, pinned to this desktop (`runs-on: forkmesh`): other nodes cannot reach the Mac or hold its key | Finds the Mac, takes its build lock, has the Mac fetch the commit, runs each Codemagic step as its own Actions step, and brings the IPA and logs back as Actions artifacts (`data/actions/artifacts/<repo>/<run>/mac-adhoc/<commit>/`). `checkout: false`: this desktop never clones the repository for it |
| `native/ios/tools/codemagic_steps.py` | Runs in the action, read from the Mac's checkout | Writes `codemagic.yaml`'s `ios-native-adhoc` scripts as numbered shell steps plus the workflow's `vars`, so the Mac runs exactly what Codemagic runs. The `watchOS platform` step is left out while the workflow is phone-only (`FORKMESH_NATIVE_WATCH=0`) |
| `native/ios/tools/mac_adhoc_build.sh` | Runs on the Mac | Loads the App Store Connect key from `~/.asc`, then runs the steps in order with `CM_BUILD_DIR` pointing at the run's `out` directory. A fourth argument names the step to stop after; `<steps>/done` counts the steps finished, so the next call carries on from there. While a step runs it follows every log the step writes in `out`, through `xcodebuild_live.awk` |
| `native/ios/tools/mac_adhoc_job.sh` | Runs on the Mac | Starts one build step's `mac_adhoc_build.sh` detached from ssh, or reconnects to it, and streams its log until it exits ("Surviving a desktop restart") |
| `native/ios/tools/xcodebuild_live.awk` | Runs on the Mac | Shrinks an xcodebuild log to one line per build action (`CompileC utils.cpp [Cmlx]`) plus every distinct warning, error and `** … **` line. A full archive comes to about 1,800 lines and 110 KB, under the 1 MiB an Actions step may print |
| `tools/mac_adhoc_publish.py` | systemd `--user` timer on this desktop | Takes the newest landed IPA it has not sent, seeds the IPA and an OTA `manifest.plist` into the cores' release-blob store (the same code path as `tools/forkmesh-release-host.py`), and emails the `itms-services` link from a core's Postfix |
| `deploy/operator/forkmesh-mac-adhoc-publish.{service,timer}` | `~/.config/systemd/user/` | Runs the publisher every 15 seconds |

The publishing half runs outside the action because it needs the fleet SSH
key, which never goes into an Actions variable. The action only holds a key
that can log in as `build` on the Mac.

## The Mac

- MacBook Pro 14,2 (2017, i5-7267U), `192.168.1.201`, Wi-Fi only.
- macOS Sequoia 15.7.9 (24G830) through OpenCore Legacy Patcher 2.5.1
  (`/Library/Application Support/Dortania`). OpenCore is on the internal
  EFI partition and boots by default. SIP is `0x803` and SecureBootModel is
  off, as the root patches need.
- 15.7.9 was chosen over 15.8 because Dortania publishes both a KDK and a
  MetallibSupportPkg for 24G830, so the root patches could run offline on the
  first boot (the Broadcom Wi-Fi has no driver until they are applied).
- `/Library/LaunchDaemons/com.forkmesh.autopatch.plist` runs
  `/usr/local/libexec/fm-autopatch.sh` at boot. On the first boot of any new
  macOS build (15 or later) it runs `OpenCore-Patcher --patch_sys_vol` once and
  reboots, leaving `/Users/Shared/fm-autopatch.<build>.log`. A macOS update
  therefore restores Wi-Fi by itself, provided OpenCore can reach the KDK for
  that build or it is cached in `/Library/Developer/KDKs`.
- The first boot through OpenCore needed a person: SIP refuses
  `bless --setBoot` on the EFI partition, so it was picked once with Option at
  startup. OpenCore then registers itself as the default.
- One account, `build` (admin). The previous owner's account and apps were
  deleted on 2026-09-27.
- Xcode 26.6 (17F113) in `/Applications`, with the iOS platform and the Metal
  toolchain. Xcode came from developer.apple.com as a `.xip` because
  downloading it needs an Apple ID.
- macOS's own `xcodeproj` gem (1.22) cannot read Xcode 26 projects (object
  version 77), and a user install of 1.27 clashes with the system Ruby's
  gems (`rexml`). So xcodeproj 1.27.0 lives in its own gem home,
  `~/.gems-xcodeproj`, with CFPropertyList pinned to 3.0.6: newer versions pull
  in `nkf`, whose native extension does not build against the system Ruby.
  `mac_adhoc_build.sh` puts that gem home first.
- `~/.asc/key_id`, `~/.asc/issuer`, `~/.asc/AuthKey.p8` (0600): App Store
  Connect API key `NAU34ZR27B`. The steps register bundle ids, and the
  export mints ad hoc profiles with it.
- The source lives in one fixed folder, `~/ci/tree`. Xcode keys DerivedData
  by project path, so a new folder per run meant a cold 25-30 minute archive
  every time. `~/ci/tree` is a git repository with a partial (`blob:none`),
  sparse checkout: `native/ios/`, `codemagic.yaml`, `conformance/`, and the
  few files outside them that the ForkMeshCore tests read (`app/src/apns.py`,
  `app/src/app_runtime.py`, `marketplace/packages/vault/tests/fixtures/`;
  [`apns.py` missing on the Mac](#apnspy-missing-on-the-mac-2026-10-09)). Each run fetches its
  commit at depth 1 from `MAC_BUILD_GIT_URL` and checks it out with
  `git clean -ffdx`, which takes about 10 seconds. The fetch retries for
  two minutes, because a push reaches the desktop before the cores. The
  checkout retries the same way (see below). Each run's
  steps and output go in `~/ci/<commit>-<time>/`, and the two newest runs are
  kept.
- `build` never logs in at the screen, so it had no login keychain, and
  `security cms -D` (the "Show each bundle's profile" step) failed with "A
  default keychain could not be found". `mac_adhoc_build.sh` creates an
  empty-password `build.keychain`, makes it the default, and unlocks it on
  every run. It holds nothing: signing happens in the cloud through the
  API key.
  `~/ci/lock` stops two runs from overlapping, and a lock older than four
  hours is treated as stale.
- Only the newest iOS build runs. Each run writes its name to
  `~/ci/ios-latest` first. A newer run kills the older build's process tree
  (its PID is in `~/ci/lock/pid`) and takes the lock; an older run still
  waiting sees it is no longer in `ios-latest` and exits. Both report
  "Cancelled"/"skipping" and succeed rather than fail. A lock held by
  anything that is not an iOS run (`ci/<commit>-<time>`), such as the
  desktop `.dmg` build (`desktop-release v1.2.5`), is waited for, never
  killed. A run removes the lock only while it still owns it.

- The steps run `native/ios/tools/*.py` with the Mac's own `/usr/bin/python3`, which is 3.9.6. Annotations like `bytes | None` need Python 3.10 when evaluated, so a tool that uses them must start with `from __future__ import annotations` (2026-10-14: `asc_bundle_ids.py` and `asc_store_report.py` stopped an ad hoc build at "Register the bundle ids" with `TypeError: unsupported operand type(s) for |`). Check with `ast.parse(src, feature_version=(3, 9))` before pushing.

## Release builds (2026-09-28)

The release host (`tools/forkmesh-release-host.py`, `docs/release/README.md`)
uses the same Mac, key and address rules for two jobs per release:

- `desktop-release vX.Y.Z`: the universal macOS `.dmg`, built from a
  `git archive` of the release commit in `~/desktop-release/src-<version>`
  by `tools/package/macos_universal_on_mac.sh`. `~/desktop-release` keeps the
  Qt, OpenSSL, libwebp and nghttp3 caches between releases.
- `ios-release vX.Y.Z`: Codemagic's `ios-native-release` steps with the App
  Store submission on, in `~/ci/tree` and `~/release/ios-vX.Y.Z/`
  ([IOS_RELEASE.md](IOS_RELEASE.md)).

Both take `~/ci/lock` under those names, so an ad hoc run waits for them
instead of cancelling them, and a release waits up to two hours for an ad hoc
run. `ios-native-release` hands values between steps through `$CM_ENV` (the
build number and version), so `mac_adhoc_build.sh` now provides it: a
`cm_env` file in the steps folder whose `KEY=VALUE` lines are exported before
every step, as on Codemagic. It also sets `CM_COMMIT` from the checkout. The
release host scans the LAN from Python (`find_build_mac`) with the same
pinned-host-key check as the workflow's first step.

## The Mac's light on the phone

The iPhone's footer showed whether the Mac is idle, building or out of reach
(2.0.48 to 2.0.54), from the desktop's `diag.buildMac` local control method,
which reads `~/ci/lock` over ssh with the release host's key. Since 2.0.55
the footer shows the desktop's load instead and nothing on the phone asks
`diag.buildMac` ([NAVIGATION.md](NAVIGATION.md) §2.0.55).
Since 2026-10-06 a `laptopcomputer` glyph with a dot sits after the desktop
glyph again, asking `diag.buildMac` as before, and the Desktops page has an
Office MacBook section ([NAVIGATION.md](NAVIGATION.md) §2.0.61).

## Finding the Mac

The Mac's address comes from DHCP, so nothing depends on it:

- The workflow's first step tries the hint address, then scans the
  desktop's /24 for port 22 in batches of 24 and tries ssh on each open
  host. ssh checks the host key by the `forkmesh-mac` alias
  (`HostKeyAlias`, `StrictHostKeyChecking=yes`), not by IP, so a machine
  with a different key is refused before the key is offered. A scan takes
  about 20 seconds. The scan was a Python script in `native/ios/tools/` until
  2026-09-28; with no local checkout the step can only use what the workflow
  file itself contains.
- mDNS was tried first (the Mac answers as `forkmesh-mac.local`), but the
  desktop's UFW drops the Mac's unicast reply to a query from an ephemeral
  port, and the Actions sandbox cannot reach Avahi. Replies to a TCP connection
  the desktop opens pass the firewall.
- The Mac never sleeps (`pmset disablesleep 1`, `sleep 0`, lid closed
  included) and has wake-on-LAN on. SSH runs from boot, before anyone logs in
  (FileVault is off), so a Mac that is switched on and on the Wi-Fi is ready
  for a build.
- `ComputerName` "ForkMesh Mac", `LocalHostName` `forkmesh-mac`.

### Reaching the Mac by hand (2026-10-01)

`tools/office-mac.sh` logs in as `build` with no arguments, runs a command
when given one (`tools/office-mac.sh 'cat ci/lock/run'`), and prints only the
address with `--find`. It uses the same key, pinned host key and
`HostKeyAlias` as the builds. It tries the address the Mac was last found at
(`~/projects/forkmesh/state/mac-build/address`), then `MAC_BUILD_HOST`
(default `192.168.1.201`), then scans the /24 for port 22 and tries ssh on
each open host, which takes under ten seconds. The address it finds is
written back to that file.

The release host (`find_build_mac`) reads and writes the same file, so once
DHCP moves the Mac, one scan by either of them puts the new address first
for both. Before this, every release run that missed `192.168.1.201`
rescanned the LAN, and reaching the Mac from a shell meant copying the ssh
options out of this page. The Actions workflow still starts from its own
hint (`MAC_BUILD_HOST` under Secrets & Coves) and does not read the file.

After the Mac has been off or away, `tools/office-mac.sh --find` is the
check that it is back. If it reports the Mac is not on the LAN, the Mac is
off, asleep or not on the Wi-Fi; nothing on this side needs changing.

## Steps and live output (2026-09-28)

- The first version cloned the whole repository on this desktop (the
  runner's worktree and writable clone, 8,000 files), then shipped
  `git archive HEAD native/ios` to the Mac. Only the Mac needs the source, so
  the workflow now sets `checkout: false` (docs/desktop/README.md) and the Mac
  fetches the commit itself.
- The run shows every step: Find the build Mac, Take the Mac's build lock,
  Fetch the commit on the Mac, one Actions step per `ios-native-adhoc` script
  in `codemagic.yaml`, then Bring the IPA and logs back. Each build step calls
  `mac_adhoc_build.sh` with its own name, and its output (plus the live
  xcodebuild summary) streams into that step's log. Since 2026-09-30 that
  call goes through `mac_adhoc_job.sh` ("Surviving a desktop restart"). A step the build leaves
  out, such as `watchOS platform` while `FORKMESH_NATIVE_WATCH=0`, prints
  "Skipped". The last step runs any Codemagic step no workflow step names, so
  a step added to `codemagic.yaml` still runs;
  `tools/test_mac_native_build_workflow.py` fails until the workflow lists it.
- State between steps (key, Mac address, run name) lives in the run's
  sandbox home under `mac/`; `mac/lib.sh` holds the ssh helpers. A step
  script reaches bash on stdin, so `mac` calls `ssh -n`: a plain `ssh` would
  read the rest of the step as its input. `mac_in` is the one that sends data.
- A step that fails releases `~/ci/lock` if the run still owns it. A newer iOS
  run now removes an older run's lock even when no build process is running
  (between steps there is no `ci/lock/pid` to kill); the older run sees it
  lost the lock at its next step and reports "Cancelled".
- The Mac's checkout is a partial clone (`blob:none`), so `git checkout`
  fetches the files it needs from the cores at that moment. Right after a push
  a core can advertise the commit before it has every blob, and the lazy fetch
  answers 404 (`fatal: could not fetch <blob> from promisor remote`; runs 1860
  and 1863 on 2026-09-30). The checkout is now retried like the fetch: up to
  six times, 20 s apart.
- The "Skip unless native/ios changed" step is gone, along with
  `MAC_BUILD_FORCE`. It diffed the commit against its first parent, so a
  manual run on a merge that did not touch `native/ios` skipped the build and
  still reported success. `on.push.paths` now lists `native/ios/*` down to
  ten levels: the desktop matches it with Qt wildcards, where `*` never
  crosses a `/`, so the old `native/ios/**` only matched files directly in
  `native/ios/`. A manual run always builds.

## Surviving a desktop restart (2026-09-30)

Until 2026-09-30 each build step was a foreground `ssh` running
`mac_adhoc_build.sh`. A desktop restart or shutdown killed the step's `ssh`,
which took the build on the Mac down with it. The run was then re-queued from
the beginning under a new run name, killed whatever was left of the old build,
and fetched and archived from scratch. Now the Mac build carries on while the
desktop is down, and the run picks it up again when the desktop comes back.

- `native/ios/tools/mac_adhoc_job.sh <run> <step>` is what a build step runs
  on the Mac. The first call for a step starts `mac_adhoc_build.sh` detached
  from the ssh session (`nohup`, and `setsid` through perl, because macOS has
  no `setsid` command). The job goes in `~/ci/<run>/jobs/<n>/`: `log` holds
  its output, `pid` and `started` hold its process and start time, and
  `status` holds its exit code once it ends. `<n>` is the step's line in
  `index.tsv`, or `all` for the last step. The script then follows `log` until
  `status` appears and exits with that code. A later call for the same step
  starts nothing: it prints "Reconnected", replays the log from the top and
  keeps following. A job whose process is gone with no `status` (the Mac
  restarted) fails the step. `started` is compared as well as `pid`, so a
  reused PID does not count as the job still running. The job's PID goes in
  `~/ci/lock/pid`, so a newer iOS run still cancels it with `killtree`.
- Each run records who owns the lock. "Find the build Mac" sets `owner` to
  `actions-run-<FORKMESH_RUN_ID>-<commit>`, and "Take the Mac's build lock"
  writes it to `~/ci/lock/owner`. A re-queued run keeps its run number, so
  when it finds the lock under its own owner it takes over that run's name
  (`resumed=1`) instead of starting a new one. It also reads `started_at` and
  `locked_at` back from `~/ci/<run>/times`, so `build.json`'s build time
  still counts from the first attempt.
- On a resumed run, "Fetch the commit on the Mac" leaves `~/ci/tree` alone if
  the earlier attempt finished fetching (`~/ci/<run>/fetched`, written after
  the steps are uploaded). It copies the steps and the commit subject back to
  the sandbox. If the fetch was cut short, it kills that fetch through
  `~/ci/lock/pid` and fetches again. Every build step then reconnects to its
  job, or starts it if the build had not reached that step yet.
- If the connection to the Mac drops while the desktop stays up (ssh exits
  255, e.g. a Wi-Fi blip), the step waits 30 s and reconnects, up to 20
  times, before it gives up. The build keeps running on the Mac the whole
  time.
- Desktops too old to set `FORKMESH_RUN_ID` write an empty owner and never
  resume, so they behave as before, apart from the detached steps.
- Stopping a run from the desktop (or a newer run superseding it on the
  desktop) now only stops the desktop side. The Mac finishes the step it was
  on, and `~/ci/lock` stays held until the next iOS run cancels it, or for
  four hours at most. A release waiting on the lock waits for that too.
- `tools/test_mac_native_build_workflow.py` drops a step's connection
  mid-build, reconnects, and checks the replayed log and exit code. It also
  covers a job that died without a status.

## A commit the cores never got (2026-09-30)

Runs 1959 (`a11e86ffe279`, 02:37) and 1981 (`8684a082afdc`, 02:54) failed at
"Fetch the commit on the Mac". Each of the six fetches got
`error: RPC failed; HTTP 404 ... fatal: expected 'packfile'`, and the only
explanation printed was "The commit is not on ... yet". The cores answer a
`want` for a commit they do not hold with a JSON 404 (`{"error":"not_found"}`).
The served `main` had stopped at `7fe3c12aa495` (02:33), 26 commits behind the
checkout.

The Mac and the cores were fine. At 02:35 this desktop started dropping every
TCP connect to its own `127.0.0.1` (127.0.0.2 and ::1 still connected; see
"The rqlite tunnel" in `docs/deploy/auto-deploy.md`). The source publisher
pushes through a signing proxy on `127.0.0.1`, so every pass hung for 133 s
and pushed nothing (`docs/tools/source-publisher.md`, Traps).

When the fetch fails, the step now asks the cores for `main` from this
desktop (`git ls-remote`) and ends with one `ERROR:` line that names the
cause:

| Cores' `main` | Message |
|---|---|
| a different commit | `ERROR: <commit> is not published: <url> serves main at <sha>`, followed by where to look on the desktop (`state/publisher/status.json`, the publisher's journal) |
| this commit | `ERROR: <url> serves <commit> as main, so the fetch broke on the Mac's side` |
| unreadable | `ERROR: this machine cannot read <url> either: <git's last line>` |

The "Action failed" ping shows that line as its `Cause:`
(`ActionFailureSummary`, `docs/desktop/README.md`). Each
retry now prints "Could not fetch ... (attempt N of 6)" instead of claiming
the commit is missing. Once the publisher has pushed again, run the workflow
by hand: a fixed publisher does not re-queue the failed runs.

## The shortcut (2026-10-04)

`.forkmesh/shortcuts/deploy-native-ios-ad-hoc-mac.sh` is the Mac's
counterpart of the Codemagic shortcut `deploy-native-ios-ad-hoc.sh`: one
click ships `main` as an ad hoc build, but on the office MacBook. It runs
`tools/mac_adhoc_deploy.py --execute` (without `--execute` the tool only
reports `main`, what the cores serve and where the Mac is).

- It waits until `MAC_BUILD_GIT_URL` (default
  `https://app.forkmesh.com/forkmesh/forkmesh`) serves this checkout's `main`
  or a newer commit, for at most 30 minutes, and builds the served commit.
- It finds the Mac the same way the release host does (the hint address,
  then a LAN scan against the pinned host key in
  `~/projects/forkmesh/ssh/mac-build/`). It does not go through ForkMesh
  Actions, so it needs no Actions variables and works while Actions is busy.
- It behaves like an Actions ad hoc run on the Mac. It writes its run name
  (`ci/<commit>-<time>`) to `~/ci/ios-latest`, and then takes `~/ci/lock`
  (`LOCK_SCRIPT` in the tool). An older iOS build holding the lock (any
  `ci/*` run: an Actions push build, or an earlier shortcut run, even one
  whose desktop has since restarted) is cancelled: its process tree through
  `~/ci/lock/pid`, then the lock. A release (`desktop-release`,
  `ios-release`) is waited for, up to two hours, and a lock older than four
  hours is cleared. An older Actions run that is still waiting for the lock
  gives up. A newer push build or shortcut run cancels this one, and the
  shortcut then ends with "cancelled" and exit 0, because that build emails
  its own link. So running the shortcut again cancels the running build and
  starts over from the cores' `main`.
- Once it holds the lock, it fetches the commit into `~/ci/tree` (the release
  host's fetch script). It then writes the `ios-native-adhoc` steps from the
  Mac's own `codemagic.yaml` and `codemagic_steps.py`, skipping `watchOS
  platform` while `FORKMESH_NATIVE_WATCH=0`. Last, it runs every step through
  `mac_adhoc_job.sh`. The shortcut's output is that job's log, with the
  xcodebuild summary. If ssh drops (exit 255), it reconnects every 30 s, up
  to 20 times, while the build keeps running on the Mac.
- Since 2026-10-06 the build runs in its own systemd unit and the shortcut
  only follows it ([Builds outlive the desktop](#builds-outlive-the-desktop-2026-10-06)).
  Stopping that unit (Stop, Ctrl-C, `--stop`, `systemctl --user stop`) at any
  point after it holds the lock (fetch, steps or build) kills the Mac's build tree through
  `~/ci/lock/pid` and releases the lock, in one ssh call (`STOP_SCRIPT`).
  This is unlike the Actions workflow, which leaves the Mac building. The
  desktop's Stop sends SIGKILL 3 s after SIGTERM, which is often before ssh
  has reached the Mac, so that ssh runs in its own session with no pipes back
  to the tool and finishes after the tool is gone. Further signals are
  ignored once the stop starts. Until 2026-10-06 the stop only covered the
  build steps, took two ssh calls, and was cut off by the desktop's SIGKILL,
  leaving the lock held until the next push build cleared it.
- On the desktop, clicking the shortcut again while it runs stops it and runs
  it again ([../desktop/README.md](../desktop/README.md), Recent changes,
  2026-10-06); the new run then cancels whatever the stop left behind.
- The IPA and logs come back to
  `~/projects/forkmesh/data/mac-adhoc/shortcut/<time>/mac-adhoc/<commit>/`
  with a `build.json` in the Actions layout. The tool then runs the
  equivalent of `mac_adhoc_publish.py --execute --artifacts ~/projects/forkmesh/data/mac-adhoc`
  (in the same process since 2026-10-06, so it still works after the checkout it was started from is removed).
  That command seeds the IPA and manifest to the cores and emails the
  install link to the early iOS testers, or to `--to` addresses passed to
  the tool. The folder is not the Actions artifacts folder, so the 15-second
  publisher timer never sees these builds and cannot send one twice.
- It does not prune old `~/ci/<run>/` folders on the Mac, because it has no
  guarded delete there. The next Actions ad hoc run keeps only the two newest.

## The macOS desktop shortcut (2026-10-06)

`.forkmesh/shortcuts/build-desktop-macos-mac.sh` builds the universal macOS
desktop `.dmg` from this checkout's `main` on the office MacBook. It runs
`tools/mac_adhoc_deploy.py --desktop --execute` (without `--execute` it only
reports `main` and where the Mac is). The iPhone's Desktops page runs it, and
the ad hoc iOS shortcut, from the Office MacBook row
([NAVIGATION.md](NAVIGATION.md) §2.0.64).

- It reuses the release's Mac pieces: `find_mac`, then the release host's
  `mac_lock` as `desktop-build <commit>`, `MAC_DESKTOP_SOURCE_SCRIPT` fed by a
  `git archive` of `main` (into `~/desktop-release/src-main`), and
  `MAC_DESKTOP_BUILD_SCRIPT` (`tools/package/macos_universal_on_mac.sh`). It
  does not wait for the cores: the source comes from the local checkout.
- The lock name is not `ci/*`, so an ad hoc iOS run waits for it instead of
  cancelling it, and it waits for an ad hoc run or a release in turn (up to
  two hours). Like a release, the source step clears the previous
  `src-*`, `build-src-*` and release `.dmg` in `~/desktop-release`, so every
  build is a full one; the Qt, OpenSSL, libwebp and nghttp3 caches stay.
- The build's output streams to the shortcut as it comes (`stream`) and is
  kept in `~/projects/forkmesh/data/mac-adhoc/desktop/<time>/mac-build.log`.
  The `.dmg` the build names in its last `sha256` line is copied next to it
  and checked against that digest. Nothing is published or emailed.
- It runs in the same `forkmesh-mac-adhoc-*` systemd unit as the iOS build,
  so a desktop restart leaves it building and `--stop` stops it too. A stop
  kills the Mac's build tree and releases the lock (`STOP_SCRIPT`). A dropped
  ssh connection ends it with an error; it does not reconnect.
- Tested in `tools/test_mac_adhoc_deploy.py`. The dry run found the Mac on
  2026-10-06; the full build was not run from the agent that added it.

## The App Store shortcut (2026-10-08)

`.forkmesh/shortcuts/deploy-native-ios-app-store-mac.sh` ships `main` to the
App Store for review in one click, on the office MacBook. It runs
`tools/mac_adhoc_deploy.py --app-store --execute`; without `--execute` it only
reports `main`, what the cores serve, where the Mac is and the What's New text.
It does for `main` what the release host does for a tagged release
([IOS_RELEASE.md](IOS_RELEASE.md), "App Store rollout for a tagged release"),
without waiting for a tag.

- Like the ad hoc shortcut, it waits for the cores to serve this checkout's
  `main` (or newer) for up to 30 minutes, finds the Mac with `find_mac`, and
  fetches the served commit into `~/ci/tree` with the release host's fetch
  script.
- It writes Codemagic's `ios-native-release` steps from the Mac's own
  `codemagic.yaml`, and appends `FORKMESH_IOS_APP_STORE=1` and
  `FORKMESH_IOS_WHATS_NEW` to the steps' `env.sh`. It refuses a
  `codemagic.yaml` with no "Submit to the App Store" step. The steps register
  the bundle ids, pick the next build number and an App Store version
  (`asc_appstore.py version`), archive, export with cloud signing, upload with
  `altool`, add the build to TestFlight internal testing, and submit it
  (`asc_appstore.py submit`), which replaces any version already waiting for
  review.
- What's New is `--whats-new`, else `FORKMESH_IOS_WHATS_NEW` from the
  environment, else "Bug fixes and improvements.". A merge subject such as
  "Merge 1805-... into main" is not store text, and App Review reads What's
  New, so it must not mention membership, Patreon or tips
  ([IOS_RELEASE.md](IOS_RELEASE.md), "App Review: nothing paid on iOS").
  TestFlight's What to Test still comes from the commit message.
- The run folder is `~/release/ios-main-<commit>-<time>/` (the release host's
  steps script, which keeps the two newest `release/ios-*` folders). The lock
  is `ios-app-store <commit>-<time>`, taken with the release host's
  `mac_lock`: it waits up to two hours for whatever holds `~/ci/lock`, ad hoc
  runs included, and is never cancelled by one. An ad hoc push build or
  shortcut waits for it instead (`LOCK_SCRIPT` only cancels `ci/*` holders),
  so a push during a submission does not kill the upload.
- The build runs through `mac_adhoc_job.sh` in the same
  `forkmesh-mac-adhoc-*` systemd unit as the other Mac shortcuts, so a desktop
  restart leaves it building, a dropped ssh connection reconnects every 30 s,
  and Stop, Ctrl-C or `--stop` kills the Mac's build and releases the lock.
  A stop after the upload step can leave an uploaded build that was not
  submitted; `asc_appstore.py status` shows where it got to.
- It succeeds only when the build log holds `asc_appstore.py`'s "Submitted
  ... for App Review:" line. It then writes
  `~/projects/forkmesh/data/mac-adhoc/app-store/<time>-<commit>.json` with the
  version, build number, What's New and the Mac's run folder. Nothing is
  emailed; Codemagic's release e-mail to founders@ does not apply on the Mac.
- The release host keeps its own record per tag (`ios-<tag>.json`), so a
  later tagged release still submits; the newer submission replaces this one.
- Tested in `tools/test_mac_adhoc_deploy.py` with a fake Mac. The dry run
  found the Mac on 2026-10-08; the agent that added it did not run a real
  submission.

### First run: three compile errors on `main` (2026-10-08)

The shortcut's first run (`105c03060bd5`, 12:34) stopped at "Test ForkMeshCore":
`AgentRunQueueTests` put `Int` expressions such as `now - 125_000` into
`JSONObject` literals, and `JSONValue` takes only integer literals. They are
now `JSONValue(now - 125_000)`. The ad hoc archive of `ecfc7f7ff5ca` the
evening before had already failed on two app errors, which this run would have
hit next:

- `DeleteAgentIntent.prepareError` called the main-actor
  `AgentVoiceSource.controlError` from a nonisolated static method. Every
  caller is already `@MainActor`, so `prepareError` is `@MainActor` too.
- `HomeShell.body` grew past the type checker's budget once the status page
  and the aquarium added two `onChange`s and two overlays (29 chained
  modifiers). It is now `framed` (backgrounds, overlays, tasks, incoming
  links), `routeChanges` (navigation `onChange`s) and `appChanges` (status
  page, scene phase, sign-in), wrapped around `sheets(overlays(layout))`, as
  in [Genie spider type-check budget](#genie-spider-type-check-budget-2026-10-06).

### `apns.py` missing on the Mac (2026-10-09)

The App Store run of `370c9d0cf803` (02:56) ran 3401 core tests and failed
one: `StoreConfigTests.testTheServerSendsPushesToThisBundleId` reads
`app/src/apns.py` to check that the server pushes to the app's bundle id, and
the Mac's sparse checkout (`MAC_IOS_FETCH_SCRIPT` in
`tools/forkmesh-release-host.py`) held only `native/ios/`, `codemagic.yaml`
and `conformance/`. Codemagic clones the whole repository, so the test only
fails on the Mac, and ad hoc runs skip the core tests. Earlier App Store runs
stopped on other test failures before reaching it.

The sparse checkout now adds `app/src/apns.py`, `app/src/app_runtime.py`
(`RelayTests` checks `RELAY_FEATURES` against it and skipped without it) and
`marketplace/packages/vault/tests/fixtures/` (`VaultInteropTests`, which
skipped too). The release host's tagged releases use the same script.
`test_the_mac_checkout_holds_every_file_the_core_tests_read` in
`tools/test_mac_adhoc_deploy.py` lists the files; a new test that reads a file
outside `native/ios/` must add it there and to the script.

### Ad hoc and App Store at once (2026-10-09)

The desktop now runs several script shortcuts at once
([../desktop/README.md](../desktop/README.md), Recent changes, 2026-10-09), so
the ad hoc, App Store and macOS desktop shortcuts can all be started together.
Each runs in its own `forkmesh-mac-adhoc-<time>-<pid>` unit with its own log,
and they take turns on the Mac through `~/ci/lock`: whichever starts second
waits (up to two hours) and then builds, so both finish without one
cancelling the other. Only a second ad hoc run cancels an earlier ad hoc run,
as before. Stop on one shortcut stops only its own unit; `--stop` still stops
them all.

## A stuck build: status and unstick (2026-10-09)

The ad hoc run of `42b167068b09` (04:21) sat at `CompileSwift [ForkMesh]`
with no output for over ten minutes; the two runs before it took 15 and 22
minutes from start to IPA. The Mac was not hung: an `xctest` from the App
Store run of `096e3e503e66` (23:12 the night before) had been running for
five hours with parent 1, using CPU the whole time. That run's
"Test ForkMeshCore" hung in `LocalModelManagerTests.testDownloadFailureAndCancel`.
After an hour, `perl -e 'alarm 3600; exec ...'` killed `swift test`. But
`swift test` starts `xctest` in its own process group, so the alarm, the
step's failure and `killtree` on `~/ci/lock/pid` all missed it. Once it was
killed, the archive finished in two minutes, and build 189 was published and
emailed.

Three changes:

- `native/ios/tools/run_with_deadline.sh <seconds> <command>` replaces the
  perl alarm in Codemagic's "Test ForkMeshCore" step. At the deadline it
  collects the command's whole process tree (`pgrep -P`, which follows a child
  into its own group or session), sends SIGTERM, waits up to 10 s and sends
  SIGKILL. Otherwise it exits with the command's code.
- `mac_adhoc_build.sh` kills leftover build tools before a fresh build
  (step 1, not a resumed one). It looks for processes of the `build` user
  with parent 1 whose program is `xctest`, `xcodebuild`, `swift-build`,
  `swift-test`, `swift-frontend` or `swift-driver`, and logs each one as
  "Killing ... left over from an earlier build". A live build's tools always
  have a living parent. Its detached job (`bash -c`, parent 1 after the
  desktop disconnects) is not on the list. The macOS desktop build does not
  run `mac_adhoc_build.sh`, so the next iOS build clears what that build
  leaves behind.
- `ps` runs with `-ww`. Without it, `ps` cuts `args` at `COLUMNS` (pytest
  sets it), and a cut path hides the program name.

Two commands, each also a shortcut:

| Command | Shortcut | Does |
|---|---|---|
| `tools/mac_adhoc_deploy.py --status` | Office Mac build status | Read only. Lists the `forkmesh-mac-adhoc-*` units on this desktop, with minutes since each log last grew and its last line. Then shows who holds `~/ci/lock` and for how long, and whether its process is alive. Then the newest job log on the Mac (`ci/*/jobs/*/log`, `release/*/jobs/*/log`), with its age and last three lines. Then every build process on the Mac, with each orphan marked "orphaned: its build is gone". |
| `tools/mac_adhoc_deploy.py --unstick` | Unstick the office Mac build | Runs `--stop` (every unit on this desktop, each releasing its own lock). Then it kills `~/ci/lock/pid`'s tree and every build process on the Mac (SIGTERM, then SIGKILL 5 s later). Then it releases `~/ci/lock`, whoever holds it, and checks that nothing is left (exit 1 if anything is). |

`BUILD_TOOLS` in `tools/mac_adhoc_deploy.py` defines a build process for
both commands. It is a process whose program, or whose script for `bash`,
is one of the Xcode and SwiftPM tools above, `swift-plugin-server`,
`SWBBuildService`, `XCBBuildService`, `mac_adhoc_build.sh`,
`mac_adhoc_job.sh`, `macos_universal_on_mac.sh` or
`run_with_deadline.sh`. It is also any process whose command line holds
`~/ci/`, `~/release/` or `~/desktop-release/` (log followers, step scripts).

When a Mac build seems stuck:

1. Run "Office Mac build status". Up to 15 minutes without output at
   `CompileSwift [ForkMesh]` is normal, because the Release archive compiles
   the whole app module in one `swift-frontend`. An orphaned process, a lock
   whose process is gone, or a log that has not grown for 30 minutes is not
   normal.
2. Run "Unstick the office Mac build", then start the shortcut again.

`--unstick` also ends an Actions run or a tagged release that is using the
Mac. The Actions run reports "Cancelled" at its next step, and the release
host's next attempt takes the lock again. A stop after an App Store upload
can leave a build that was uploaded but not submitted
(`asc_appstore.py status`). Tested in `tools/test_mac_adhoc_deploy.py`
against fake tools and a fake `~/ci`, and run against the real Mac on
2026-10-09 (status, an unstick on the idle Mac, and the deadline and orphan
sweep under its bash 3.2).

## Builds outlive the desktop (2026-10-06)

The ad hoc run of `93d16a4a410b` (the Genie type-check fix) was killed five
minutes into its archive (`** BUILD INTERRUPTED **`, steps `Terminated: 15`).
At 02:13 the desktop restarted. Its agents run in per-desktop scopes
(`forkmesh-agent-claude-<issue>-<desktop pid>-<n>.scope`), and a shortcut is a
child of the desktop itself. So the restart sent the tool SIGTERM, and the
tool's stop handler killed the Mac build, exactly as if someone had pressed
Stop.

`mac_adhoc_deploy.py --execute` now starts the build in its own transient
unit, `forkmesh-mac-adhoc-<time>-<pid>.service` (`systemd-run --user
--collect`, `KillMode=mixed`, `IgnoreOnIsolate=yes`, `TimeoutStopSec=180`).
`IgnoreOnIsolate` keeps the build unit alive when GNOME stops the user's
`default.target` during a desktop restart or logout; explicit stop commands
still stop the unit. The unit gets only
`PATH`, `LANG`, `LC_*`, `MAC_BUILD_*`, `CODEMAGIC_*` and `FORKMESH_*` (minus
`FORKMESH_AGENT_*`), so it does not depend on an agent's scratch folder or
`TMPDIR`. It reads `main` from the checkout's shared `.git` (`--repo`, from
`git rev-parse --git-common-dir`), so removing an agent worktree does not
break it. Its output goes to `~/projects/forkmesh/logs/mac-adhoc/<unit>.log`,
and its exit code to `<unit>.exit`, which the env var `FORKMESH_MAC_ADHOC_LOG`
points at. The process the shortcut started only prints that log as it grows,
then exits with the unit's code.

What ends a build:

| Event | Effect |
|---|---|
| Desktop Stop, Ctrl-C (SIGINT to the follower) | `systemctl --user stop --no-block <unit>`; the unit's SIGTERM runs the existing stop (`STOP_SCRIPT`) |
| `tools/mac_adhoc_deploy.py --stop` | stops every running `forkmesh-mac-adhoc-*` unit and waits for each |
| `tools/mac_adhoc_deploy.py --unstick` | `--stop`, then kills every build process on the Mac and releases `~/ci/lock` whoever holds it ([A stuck build](#a-stuck-build-status-and-unstick-2026-10-09)) |
| A newer shortcut run or Actions push build | takes `~/ci/lock` and cancels this one on the Mac; this unit ends with "cancelled", exit 0 |
| Desktop quit or restart, agent scope stopped, terminal closed (SIGTERM, SIGHUP, SIGKILL, or a closed pipe to the follower) | nothing: the follower prints where the log is and exits 0, and the unit keeps building, publishing and emailing |

For this, the desktop's Stop now sends SIGINT before SIGTERM
([../desktop/README.md](../desktop/README.md), Recent changes, 2026-10-06).
Without `systemd-run`, or if it fails to start the unit, the tool says so and
builds in its own process as before, which a restart still kills.

## A damaged IPA was emailed (2026-10-06)

Build 169 (`f470035e859b`) was emailed, and iOS refused it: "Unable to
install: this app cannot be installed because its integrity could not be
verified". Its served copy was byte-identical to the Mac's, and its profiles
and signed entitlements matched build 166's. But the zip was damaged:
`unzip -t` reported `Payload/ForkMesh.app/ForkMesh bad CRC e4f19911 (should
be d7b3f286)`, and `codesign --verify` on the unpacked app reported "code or
signature have been modified". The signed app in the `.xcarchive` verified,
and every other IPA on the Mac (153, 154, 165, 166) tested clean. Step 10
(`Refuse a build that cannot receive push`) unpacked the same IPA under
`set -e` at 03:03:35 and passed, and the IPA's mtime is still 03:03:35. So
the bytes changed after Xcode wrote the file, without a write: a memory or
storage fault on the Mac, not the signing order.

`mac_adhoc_publish.py` now runs `zipfile.testzip()` on the IPA before it
seeds anything (`damaged_entry`). A damaged IPA is not published or emailed:
the tool prints the entry that fails and exits 1. This covers the publisher
timer and the shortcut. Build again to replace a damaged IPA.

## Compilation cache (2026-10-06)

Every ad hoc build took 20 to 35 minutes, and 28 of the 30 minutes of a
typical run went to the "Archive" step. The Mac is a dual-core 2017 i5 (4
threads, 8 GB). Each archive compiled everything again: the same 184 C++
files of MLX (`Cmlx`), swift-syntax, swift-crypto, every other package and
every ForkMeshCore module, even when a push changed one Swift file in the
app. `xcodebuild archive` deletes `ArchiveIntermediates` at its start, so
the fixed `~/ci/tree` path (which keeps DerivedData) does not make an archive
incremental. Only package resolution and the plugin executables were reused.

Xcode 26's compilation cache fixes this without giving up `xcodebuild
archive`. With `COMPILATION_CACHE_ENABLE_CACHING=YES` each Swift and Clang
compile is keyed by its exact inputs and stored in
`~/Library/Developer/Xcode/DerivedData/CompilationCache.noindex`, which lives
outside `ArchiveIntermediates` and survives each archive. A module whose
inputs did not change is replayed from the cache instead of compiled. The
app module itself (whole-module optimization) still compiles in full when any
of its files change, along with linking, Metal shaders and dsymutil.

- The "Archive" step in `codemagic.yaml` adds the setting when
  `FORKMESH_NATIVE_COMPILE_CACHE=1`, for manual and automatic signing alike.
- `native/ios/tools/mac_adhoc_build.sh` exports it as `1` unless it is
  already set, so every Mac build uses it: Actions runs, the shortcut and
  the release host's `ios-release` (the cache only changes where outputs
  come from, not what they are). Set `FORKMESH_NATIVE_COMPILE_CACHE=0` in
  the step environment to build without it.
- Codemagic does not set it: its machines start empty every time, so a
  cache would only cost writes.
- The first build after this change fills the cache and takes as long as
  before. Xcode trims the cache by its own size limit.

Measured on the Mac on 2026-10-06, archiving `29a8ed349652` in `~/ci/tree`
by hand with the setting on:

| Archive | Time | Compile jobs |
|---|---|---|
| The push build just before, without the cache | 35 min | all compiled |
| Cold, the cache empty | 21 min 39 s (1,299 s) | 654 cache misses, 1.5 GB cached |
| The same commit again | 1 min 37 s (97 s) | 295 cache hits, no misses |

A third run, with one line added to an app Swift file, was stopped
partway at 164 hits and no misses: a push that changes app code compiles
the `ForkMesh` module and replays everything else.

`tools/test_mac_native_build_workflow.py` runs the archive step against a
stub `xcodebuild` and checks the setting is passed only when asked for.

## Actions variables

Under Settings -> Secrets & Coves on this desktop:

| Variable | Value |
|---|---|
| `FORKMESH_ACTIONS_ALLOW_NETWORK` | `1`: the sandbox has no network otherwise, and the Mac is on the LAN |
| `MAC_BUILD_SSH_KEY_B64` | `base64 -w0 ~/projects/forkmesh/ssh/mac-build/id_ed25519` |
| `MAC_BUILD_KNOWN_HOSTS` | the line in `~/projects/forkmesh/ssh/mac-build/known_hosts` (`forkmesh-mac ssh-ed25519 …`) |
| `MAC_BUILD_HOST` | optional, the address to try first (default `192.168.1.201`) |
| `MAC_BUILD_GIT_URL` | optional, where the Mac fetches the commit (default `https://app.forkmesh.com/forkmesh/forkmesh`) |

## Publisher

```sh
python3 tools/mac_adhoc_publish.py            # dry run: shows the build and the links
python3 tools/mac_adhoc_publish.py --execute  # seed and email every early iOS tester flagged in the admin
python3 tools/mac_adhoc_publish.py --to a@example.com --execute  # these addresses instead
python3 tools/mac_adhoc_publish.py --ipa path/to/App.ipa --execute  # send a specific IPA
install -m 0644 deploy/operator/forkmesh-mac-adhoc-publish.{service,timer} ~/.config/systemd/user/
systemctl --user daemon-reload && systemctl --user enable --now forkmesh-mac-adhoc-publish.timer
```

- The email goes to every account flagged "Early iOS tester" in the admin
  console, one message each. The publisher asks a core for the list
  (`fmnext-app-ops early-ios-testers all`) and falls back to
  `jett@forkmesh.com` when nobody is flagged. Before 2026-09-29 it went to
  that one address only. See
  [early-ios-testers.md](../app/early-ios-testers.md).
- Since 2026-10-07 the same testers also get a ping, "iPhone build 1.4.0 (21)
  is ready", whose link is the install page (`fmnext-app-ops ios-build-ping`,
  early-ios-testers.md, "The ping"). The ping buzzes the phone, and the ping
  screen in the iOS app opens the link in Safari. `--to` sends no ping.
- The email says how long the build took: `build.json` carries
  `buildSeconds` (from taking the Mac's lock to the IPA landing back here) and
  `waitSeconds` (from the run's start until it had the lock, mostly waiting for
  another build). The wait is only shown when it is a minute or more. The
  run's last step prints the same line ("Built in 18 min 57 s on the Mac").
  A `build.json` from before 2026-09-28 has neither field, and its email has
  no build-time line.
- Each build gets a new build number: the "Pick the ad hoc build number" step
  (codemagic.yaml) takes one past the higher of App Store Connect's latest
  build and `~/ci/ios-adhoc-build` on the Mac, or the xcconfig's number if
  that is higher (IOS_RELEASE.md). The number shows in the IPA name, the
  email's subject and the app's version in Settings.
- After `sendmail` it leaves a note in #email-log with the subject, the start
  of the text and the message id, through the same core's rqlite
  (`MAIL_LOG.md`, "Mail sent outside the app"). Postfix's own delivery line
  carries no body, so until 2026-09-29 #email-log showed this email only as
  "Sent outside the app".
- It records what it sent in `state/mac-adhoc/published.json` (by IPA
  sha256), with each address as it goes out, so a failed send resumes
  without repeating anyone. Only the newest build is sent, so a backlog does not become a burst
  of emails.
- The timer ran every two minutes until 2026-09-28, and the email for build
  19 arrived two minutes after its run reported success, which read as the
  build not being emailed. It now runs every 15 seconds with
  `AccuracySec=1s` (a user timer's default accuracy is a minute). A build whose
  IPA path is already in `published.json` is skipped without hashing, so a
  tick that finds nothing new is cheap. A build is only taken once its run's
  `data/actions/runs/<repo>/<run>/meta.json` says `success`: the desktop
  writes that after the artifacts finish landing, so a tick never hashes a
  half-copied IPA. An artifact with no run record (a manual one) is taken as
  is.
- Files are served from
  `https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/<hash>?filename=…`.
  That route answers `application/octet-stream` with an attachment
  disposition. iOS reads an OTA manifest regardless of those headers.
- The email's button is an ordinary HTTPS link to
  `https://forkmesh.com/ios-install.html#<manifest sha256>`
  (`www/public/ios-install.html`). Mail apps drop or ignore `itms-services:`
  links, so the first build (2026-09-27) only installed after the link was
  pasted into Safari. Codemagic's email never had that problem because it
  links to an HTTPS page too. The page fetches the manifest from the same
  origin and shows its title and version as text. Its Install button starts
  the `itms-services` download, and it only ever builds a link to this origin's
  blob route for a 64-hex hash.
- Since 2026-10-08 the link is
  `https://forkmesh.com/ios-install.html?build=<CFBundleVersion>&v=<first 12 of the manifest sha256>#<manifest sha256>`.
  The fragment never reaches a cache, so every build used to share one URL
  and a tester could get an old copy of the page from Safari or the proxy. The
  query differs for every build, so each email opens a fresh page. The page
  shows the build number from `build` (only `0-9A-Za-z.-`, up to 32
  characters, shown as text) next to the version it reads from the manifest,
  because the manifest's `bundle-version` holds the short version only.
- The blob route stays a download route (`application/octet-stream`,
  attachment). Serving HTML from it would let anyone who can seed a release
  blob publish a page on forkmesh.com.
- Ad hoc profiles cover the devices registered when the profile is minted.
  After registering an iPhone (`python3 native/ios/tools/asc_bundle_ids.py
  register-device`, see IOS_RELEASE.md), the next build includes it.

## First build (2026-09-27)

The first local archive of `main` found three compile errors that had merged
while Codemagic was out of minutes (nothing else compiles the app target):

- `IdentityBackupState` had no public initializer, and `IdentityScreen`
  builds one. It now has one.
- `SurfacesCoordinator` called the async `WidgetCenter.currentConfigurations()`,
  which is iOS 18 only, while the app targets iOS 17. It now wraps
  `getCurrentConfigurations` in a continuation.
- `HomeShell` used `ReportBugModel` without importing `FMTools`.

## Agent list sort key path (2026-10-01)

The archive failed in `AgentListOrdering.swift` with "cannot infer key path
type from context": `ordered.map(\.element)` over the enumerated tuples stopped
type-checking once a generic `sorted(_:by:nowMs:item:)` overload sat beside the
plain one. The plain overload now forwards to the generic one, which sorts
indices with explicitly typed closures, so no tuple key path is left to infer.
Linux has no Swift toolchain, so the next Mac archive is the check.

## Genie spider type-check budget (2026-10-06)

The ad hoc archive of `2595cf6b5` failed in `GenieSpiderLayer.swift` with "the
compiler is unable to type-check this expression in reasonable time", twice:
hand play (issue 1424) had grown `body` to 22 chained modifiers and the
spider's `TimelineView` content to 30, with `??`, ternary literals and
`reduceMotion ? nil : .spring(...)` inside them. `body` is now `stage` (layout)
wrapped by `sceneWatch` and `sessionWatch`, the watched session values are
typed properties (`hearing`, `phase`, `unread`, ...), and the spider is
`avatar`, `touchable` and `accessible`, each with its ternaries bound to typed
`let`s first. Keep new modifiers in one of these pieces rather than lengthening
a chain past roughly fifteen.

## Two `DesktopHealth` types (2026-10-06)

The shortcut's archive of `bad6b2925fc6` failed in `DesktopScreens.swift` with
"'DesktopHealth' is ambiguous for type lookup in this context". The machines
page (NAVIGATION.md §2.0.61) added `import FMInsights` beside
`import FMDesktopRemote`, and both modules declare a public `DesktopHealth`:
`FMDesktopRemote`'s is the full diagnostics snapshot the gauges read, and
`FMInsights`' is the stats board's smaller tile. `DesktopHealthGauges` now names
`FMDesktopRemote.DesktopHealth`. A file that imports both modules has to
qualify the name.

## `onGeometryChange` with old and new values (2026-10-06)

Every ad hoc archive from `c08f931d1a1e` (16:16) to `e3173be7c6e5` (22:08)
failed in `ChatScreen.swift` with "'onGeometryChange(for:of:action:)' is only
available in iOS 18.0 or newer". `chatKeepsEndInView` (NAVIGATION.md §2.0.66)
used the action that receives the old and the new value, which is iOS 18
only; the app targets iOS 17. It now uses the one-value action, which iOS 17
has, and keeps the previous height in `@State`. Checked on the Mac with
`swiftc -typecheck -target arm64-apple-ios17.0`: the old modifier gives the
same error, the new one passes. Use only the one-value `onGeometryChange` in
the app.

## The `/` menu lost `frosted` (2026-10-09)

The shortcut's archive of `19fd4a08ee7a` (06:08) failed in
`PromptComposer.swift` with "cannot find 'frosted' in scope". The glass
prompt card (`3b3762c8c5`, NAVIGATION.md) moved `frosted` from
`PromptComposer` into the private `PromptCardSurface` modifier, but the `/`
command menu still called it. The menu now draws the same wash itself:
`sheetSurface` at 72% (dark) or 78% (light) over `.ultraThinMaterial`, as it
did before the move. Checked on the Mac by resuming that run's Archive step
with the fix in `~/ci/tree`.

## Settings lost `show` (2026-10-09)

The shortcut's archives of `cabbb8e3f599` (16:20) and `182accb73956` (16:56)
failed in `SettingsScreen.swift` with "cannot find 'show' in scope". Removing
toasts (`36530e8ed1`) deleted the screen's `show` snack helper but left two
callers in the Profile card: `UserStatusControls` and
`ChatRetentionSettingsRow` passed it as `onRefused`. A refusal from either now
sets `profileNotice`, an error `FmNoticeLine` at the bottom of the Profile
card (`settings-profile-notice`), the same inline pattern as the Account and
Quick setup notices. Checked on the Mac by resuming the `182accb73956` run's
Archive step with the fix in `~/ci/tree`.

## Desktop session card repository type (2026-10-11)

The office-Mac ad hoc archive of `eb05c64227c2` failed in
`AgentSessionScreens.swift`: `DesktopSessionOpsCard` expects the complete
`Repository`, but the desktop agent-data update supplied a `RepoRef` rebuilt
from the session model. The card now receives `model.repo`, preserving the
repository metadata required by its operations and matching its declared
input type.

## What's new changelog (2026-10-13)

Every ad hoc build bundles a plain-language changelog that the app shows from
the version row of the You hub ([NAVIGATION.md](NAVIGATION.md), §4.x).

- **Generator.** `tools/mobile_changelog.py` (tests:
  `tools/test_mobile_changelog.py`). The previous builds come from the
  publisher's `state/mac-adhoc/published.json` (build number and version from
  the IPA name, commit, send time; the run folder's time is the build's
  start). For each build it reads `git log --no-merges` from the previous
  build's commit, or, when that commit is gone (main was rewritten on
  2026-10-13), from the previous build's start time. It drops merges,
  generic "N files changed" subjects, docs, tests, chores and commits that
  touch only docs or tests, subjects full of code names, and commits only for
  the other phone. It groups by area (the subject's prefix, then its words,
  then the paths: Market, Calls, Projects, Chat, Agents, World, Settings,
  iPhone, Android, Desktop, Server, Other improvements), turns imperative
  verbs into past tense, caps a line at 140 characters, 8 lines a section and
  24 a build, and dedupes. No network or model calls.
- **Hand-written notes.** `native/changelog/notes.json`, per platform and
  build number. A note for build N applies to the first build numbered N or
  later that follows the last published build, so notes written for 219 still
  ship if 219 fails and 220 is the next build. `"replace": true` replaces the
  generated lines; otherwise the notes come first and the generated lines
  follow in the same sections.
- **Wiring.** The shortcut (`mac_adhoc_deploy.write_changelog`) and the
  Actions workflow write a draft (`steps/changelog.json`) next to the steps
  they send to the Mac; the Mac has no history (a depth-1 sparse checkout), so
  the desktop generates it. `mac_adhoc_build.sh` exports
  `FORKMESH_CHANGELOG_DRAFT`. The `ios-native-adhoc` step "Bundle the
  changelog", right after "Pick the ad hoc build number", runs
  `native/ios/tools/changelog_stamp.py` with the picked build number: it names
  the current entry with that number, applies the notes due and writes
  `ForkMesh/Resources/Changelog.json`, so the number in the changelog is the
  `CFBundleVersion` the app shows. Without a draft (Codemagic, an old
  launcher) it writes the build with no notes. The checked-in file is an empty
  placeholder. A failed draft never stops a build.
- **Android.** `-Pforkmesh.changelog=<file>` bundles that file as the
  `changelog.json` asset (`bundleChangelog` task); without it the empty
  placeholder `app/src/changelog/changelog.json` ships. The release host's APK
  build generates it from the previous `v*` tag with the release's versionCode.
