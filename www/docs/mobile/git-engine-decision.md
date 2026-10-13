# Mobile Git engine decision

Date: 2026-07-14.

## Current engine

The pure-Dart engine chosen below was retired with the Flutter app and its
`mobile/` tree, which have been removed from the repository. The owned Git
engine now lives in the native apps, under the same policy and specification:
Swift in `native/ios/ForkMeshCore/Sources/FMGit` and Kotlin in
`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/git`. The rest of
this page is the decision record as written, kept as history; the paths and
commands it cites no longer exist.

## Production-direction supersession

On 2026-07-16, the user selected a ForkMesh-owned pure-Dart Git engine as the only production direction.

The production engine may use Dart SDK and operating-system APIs through owned code, but it may not use libgit2, git2dart, libgit2dart, JGit, go-git, Dulwich, isomorphic-git, gix, any other Git implementation package, a Git executable, Termux, a shell, or a process fallback.

The archived libgit2 spike remains valid and useful evidence of required behavior, including progress, offline reopen, typed stale-push rejection, per-ref push confirmation, cancellation, and no-executable guarding.

It is rejected as a production dependency because of the user's dependency policy, not because its host experiment failed.

`git2dart` and its native binaries are rejected by that same policy regardless of package capability or maintenance status.

The authoritative production specification, compatibility matrix, and threat model are [`owned-git-engine-spec.md`](owned-git-engine-spec.md), [`owned-git-compatibility-matrix.md`](owned-git-compatibility-matrix.md), and [`owned-git-threat-model.md`](owned-git-threat-model.md).

The prior mobile-libgit2 completion instructions in `phase1-complete.md` are superseded for production-engine selection.

They remain historical instructions for validating the archived spike and must not be presented as a production path.

## Decision summary

The Phase 1 result is PARTIAL.
The disposable host spike validates the core repository and transport flow through libgit2 1.9.1 and Dart FFI without invoking a Git executable.
The spike does not validate an iOS or Android native package, simulator or device runtime, production credentials, TLS behavior, document-provider export, or mobile resource limits.
ForkMesh should continue with a libgit2-based engine abstraction, but production mobile authoring must remain capability-gated until the mobile exit criteria below pass.

## Question and constraints

The question was whether a Flutter node can own and author a real Git repository with an embedded engine and without Termux, a system Git executable, or a remote-only façade.

The experiment had these constraints.

- The spike had to stay isolated under `mobile/spikes/mobile_git` and could not change production Flutter, Qt, Worker, or tooling code.
- Every fixture Git operation had to use libgit2 APIs through a narrow C shim and Dart FFI.
- Clone had to set `git_clone_options.local` to `GIT_CLONE_NO_LOCAL` and expose `transfer_progress` events.
- Push had to inspect `git_push_options.callbacks.push_update_reference`, because an overall return value of zero is insufficient when a receiver rejects an individual ref.
- Cancellation had to return a negative value from a real transfer callback during a non-trivial clone and leave no successful completion event or usable HEAD.
- Package archives were unavailable from the shell network, so the maintained `git2dart` wrapper could be researched but not installed or measured.
- The installed engine was `/opt/homebrew/Cellar/libgit2/1.9.1`, which is a macOS host artifact and not mobile packaging evidence.
- Android SDK `/Users/mdkaifansari04/Library/Android/sdk` was absent, so no Android build was attempted.
- Xcode 26.6 was installed, but the managed sandbox could not connect to CoreSimulatorService or write several Xcode and SwiftPM cache locations.

The verdict rule required a real iOS simulator build and engine-backed run for VALIDATED.
Host success with mobile packaging or runtime still unproved therefore requires PARTIAL.

## Disposable implementation

The spike uses `native/mobile_git_bridge.c` as the only libgit2 boundary.
The shim owns libgit2 initialization, repositories, remotes, clone and fetch callbacks, branch checkout, staging, commits, pushes, refs, trees, blobs, history, error copying, and native memory.
The Dart layer owns fixture files, orchestration, SHA-256 checks, timing, size reporting, evidence serialization, and repository-handle lifetime tracking.
The CLI runs the complete scenario and emits named stages plus JSON evidence.
No spike engine source invokes Dart `Process` or C `system`, `popen`, `exec`, or `spawn` APIs for a repository operation.

The exercised flow was:

1. Create a working seed and bare origin with text, deterministic binary content, and `.forkmesh/repository.json`.
2. Clone through libgit2 with `GIT_CLONE_NO_LOCAL` and real transfer events.
3. Dispose the first engine after clone, load a fresh engine instance, rename the origin away, reopen the clone, and read its tree, blobs, and history offline.
4. Create and check out `feature/mobile-spike`, edit text, add a deterministic binary, stage, and commit.
5. Fetch and inspect `refs/remotes/origin/main`.
6. Push the feature ref and inspect the per-ref push callback.
7. Clone a verifier on the feature branch and compare the binary SHA-256.
8. Clone a second writer, advance `main`, and push it.
9. Retain and commit stale primary `main`, attempt a normal non-forced push, confirm `GIT_ENONFASTFORWARD`, and prove the origin OID is unchanged.
10. Fetch after rejection and confirm `origin/main` equals the writer OID.
11. Create a 12,582,912-byte deterministic cancellation fixture, return `-777` from `transfer_progress`, and prove there is no completion event or usable HEAD.

Rebase and merge after the stale rejection are intentionally deferred.

## TDD evidence

The package manifest and integration test were created before the engine API.
The first usable RED command failed specifically because the engine file and controller API did not exist.

```text
00:00 +0 -1: loading integration_test/git_flow_test.dart [E]
integration_test/git_flow_test.dart:3:8: Error: Error when reading 'lib/git_engine.dart': No such file or directory
integration_test/git_flow_test.dart:14:26: Error: Method not found: 'MobileGitSpikeController'.
```

The first Flutter runner attempt failed earlier at device discovery because Flutter treats `integration_test/` as a device-test location.
The host integration test therefore uses the Dart VM runner for the exact file, while `flutter test` is still attempted and reported separately below.

The final direct integration command passed one real integration test.

```text
00:00 +0: runs the complete embedded libgit2 flow without a git executable
00:02 +1: All tests passed!
```

The review-focused manifest assertion first failed because the fixture returned `schemaVersion` and omitted the exact Phase 0 fields.
After the fixture correction, the test requires exact decoded map equality rather than checking only the file path.

The fresh-engine lifecycle assertion and typed non-fast-forward assertion each produced a compile-time RED before their evidence fields existed.
The final test requires `freshEngineInstance == true`, `typedNonFastForward == true`, `overallCode == -11`, and an unchanged origin OID.

A temporary negative C probe containing `system("/usr/bin/git status")` initially escaped the old PATH-only runner with exit 0.
After the source preflight was added, the same probe produced `process-spawn-source-scan=FAILED` with exit 99.
The probe was then removed before the final guarded run.

## Exact commands

All Flutter and Dart commands used isolated writable state.

```sh
cd mobile/spikes/mobile_git

HOME=/private/tmp/formesh-flutter-home \
PUB_CACHE=/private/tmp/formesh-pub-cache \
FLUTTER_ALREADY_LOCKED=true \
FLUTTER_SUPPRESS_ANALYTICS=true \
/opt/homebrew/share/flutter/bin/cache/dart-sdk/bin/dart \
  /opt/homebrew/share/flutter/bin/cache/flutter_tools.snapshot \
  pub get --offline

./tool/build_host_bridge.sh

HOME=/private/tmp/formesh-flutter-home \
PUB_CACHE=/private/tmp/formesh-pub-cache \
FLUTTER_ALREADY_LOCKED=true \
FLUTTER_SUPPRESS_ANALYTICS=true \
/opt/homebrew/share/flutter/bin/cache/dart-sdk/bin/dart \
  /opt/homebrew/share/flutter/bin/cache/flutter_tools.snapshot \
  analyze

HOME=/private/tmp/formesh-flutter-home \
PUB_CACHE=/private/tmp/formesh-pub-cache \
DART_SUPPRESS_ANALYTICS=true \
/opt/homebrew/share/flutter/bin/cache/dart-sdk/bin/dart \
  test integration_test/git_flow_test.dart -r expanded

HOME=/private/tmp/formesh-flutter-home \
PUB_CACHE=/private/tmp/formesh-pub-cache \
FLUTTER_ALREADY_LOCKED=true \
FLUTTER_SUPPRESS_ANALYTICS=true \
/opt/homebrew/share/flutter/bin/cache/dart-sdk/bin/dart \
  /opt/homebrew/share/flutter/bin/cache/flutter_tools.snapshot \
  test test/host_git_flow_test.dart

./tool/run_host_no_git.sh \
  --workspace /private/tmp/formesh-mobile-git-final-20260714-191742 \
  --evidence /private/tmp/formesh-mobile-git-final-20260714-191742.json
```

The exact Flutter test command above exited 1 because this managed sandbox forbids the loopback server that Flutter's host tester starts.

```text
Failed to create server socket (OS Error: Operation not permitted, errno = 1), address = 127.0.0.1, port = 0
```

The same test body passed through the exact direct Dart VM command shown above.

The no-Git script first rejects Dart `Process` calls and C `system`, `popen`, `exec`, or `spawn` calls across the spike's Dart and C engine sources.
It then replaces `PATH` with `tool/no_git_bin`, whose only `git` entry writes a sentinel and exits 97.
The script fails if either the source scan finds a forbidden process API or the runtime sentinel appears.
The source scan covers a direct absolute `/usr/bin/git` call through those APIs, while the runtime sentinel covers name-based Git lookup.
This two-part guard does not interpose arbitrary code inside linked system libraries, so it is evidence about this spike's source and observed execution rather than a universal syscall proof.
The controller reports invocation count only when the guard environment is active and otherwise reports `-1` as unmeasured.

The platform attempts used these commands.

```sh
./tool/build_ios_simulator_probe.sh

cd ios_probe

HOME=/private/tmp/formesh-flutter-home \
PUB_CACHE=/private/tmp/formesh-pub-cache \
FLUTTER_ALREADY_LOCKED=true \
FLUTTER_SUPPRESS_ANALYTICS=true \
/opt/homebrew/share/flutter/bin/cache/dart-sdk/bin/dart \
  /opt/homebrew/share/flutter/bin/cache/flutter_tools.snapshot \
  build ios --simulator --debug --no-codesign

HOME=/private/tmp/formesh-flutter-home \
PUB_CACHE=/private/tmp/formesh-pub-cache \
FLUTTER_ALREADY_LOCKED=true \
FLUTTER_SUPPRESS_ANALYTICS=true \
/opt/homebrew/share/flutter/bin/cache/dart-sdk/bin/dart \
  /opt/homebrew/share/flutter/bin/cache/flutter_tools.snapshot \
  run -d ios --debug --no-pub
```

## Raw host results

This JSON is the decisive subset from the guarded run after review fixes.
All timing, size, and checksum values in this decision come from `/private/tmp/formesh-mobile-git-final-20260714-191742.json`.

The manifest read after the origin was disabled decoded to the exact Phase 0 shape.

```json
{
  "version": 1,
  "repoId": "fmrepo_AAECAwQFBgcICQoLDA0ODw",
  "createdBy": "mdkaif",
  "createdAt": 1767225000000,
  "defaultBranch": "main"
}
```

```json
{
  "backendVersion": "1.9.1",
  "backendIdentity": "libgit2/1.9.1;features=1791;https=securetransport;ssh=libssh2;sha1=builtin;sha256=none;bridge=mobile_git_bridge;linked=dynamic-loader;clone-local=GIT_CLONE_NO_LOCAL",
  "systemGitInvocations": 0,
  "noGitFallbackGuardActive": true,
  "gitSentinelDetected": false,
  "clone": {
    "completed": true,
    "transportMode": "GIT_CLONE_NO_LOCAL",
    "lastTransfer": "transfer received=7 indexed=7 total=7 bytes=131640"
  },
  "offline": {
    "originUnavailableDuringRead": true,
    "freshEngineInstance": true
  },
  "featurePush": {
    "accepted": true,
    "callbackObserved": true,
    "refName": "refs/heads/feature/mobile-spike",
    "overallCode": 0
  },
  "binary": {
    "sourceSha256": "9f06dfa425e15a28e11fccc2840ba96f87e31b855d999fd33232b636d4406c27",
    "verifierSha256": "9f06dfa425e15a28e11fccc2840ba96f87e31b855d999fd33232b636d4406c27",
    "byteCount": 196608
  },
  "stalePush": {
    "rejected": true,
    "typedNonFastForward": true,
    "reason": "non-fast-forward",
    "overallCode": -11,
    "errorClass": 4,
    "exactStatus": "cannot push because a reference that you are trying to update on the remote contains commits that are not present locally.",
    "remoteOidBefore": "3eda4a4982c10801de5cb4eb7bef24c000e8a29d",
    "remoteOidAfter": "3eda4a4982c10801de5cb4eb7bef24c000e8a29d"
  },
  "postRejectionOriginMainOid": "3eda4a4982c10801de5cb4eb7bef24c000e8a29d",
  "cancellation": {
    "requested": true,
    "cancelled": true,
    "completed": false,
    "resultCode": -777,
    "cancelCallbackReturn": -777,
    "cancelEvent": "cancel-request callback-return=-777 received=2",
    "exactError": "operation=git_clone code=-777 class=26 message=indexer progress callback returned -777",
    "fixtureByteCount": 12582912,
    "successfulHeadState": false
  },
  "timingsMilliseconds": {
    "prepareFixture": 41,
    "clone": 13,
    "offlineReopen": 1,
    "branchEditCommit": 18,
    "binaryRoundTrip": 20,
    "writerAdvance": 21,
    "stalePush": 5,
    "fetchAfterRejection": 2,
    "cancelClone": 1740
  },
  "repositorySizeBytes": 466865,
  "bridgeSizeBytes": 39480,
  "libgit2SizeBytes": 1095312,
  "hostCoreValidated": true
}
```

The measured stage timings in milliseconds were `prepareFixture=41`, `clone=13`, `offlineReopen=1`, `branchEditCommit=18`, `binaryRoundTrip=20`, `writerAdvance=21`, `stalePush=5`, `fetchAfterRejection=2`, and `cancelClone=1740`.
The primary repository occupied 466,865 bytes after the full scenario.
These are single-run host measurements and not mobile performance claims.

The guarded command ended with the following raw lines.

```text
process-spawn-source-scan=PROVED dart=Process c=system,popen,exec,spawn
host-verdict=VALIDATED
no-git-fallback=PROVED path=.../tool/no_git_bin sentinel=absent
```

## Push result interpretation

The shim always installs and inspects `push_update_reference`.
A successful feature push returned overall code 0 and a callback record for `refs/heads/feature/mobile-spike` with a null status.
The normal stale local push was rejected by libgit2 client preflight with typed code `GIT_ENONFASTFORWARD` or `-11`, so no receiver callback fired for that attempt.
The implementation accepts neither overall zero alone nor English text alone as proof.
A push is accepted only when overall code is zero, the per-ref callback was observed, and its status is empty.
The controlled stale fixture is classified as non-fast-forward only when the overall code is typed `GIT_ENONFASTFORWARD`, and the host verdict also requires an unchanged origin OID.
Generic non-empty per-ref callback status remains exposed for callers without being relabeled as non-fast-forward.
The exact callback or `git_error_last` detail is retained in either case.

## Binary size, architecture, and linkage

The measurements are host-only.

| Artifact | Measured size | Architecture | Meaning |
|---|---:|---|---|
| `libmobile_git_bridge.dylib` | 39,480 bytes | macOS arm64 | Dynamically linked spike shim |
| `libgit2.1.9.1.dylib` | 1,095,312 bytes | macOS arm64 | Homebrew shared engine used at runtime |

`otool -L` showed that the bridge links to `/opt/homebrew/opt/libgit2/lib/libgit2.1.9.dylib` and `libSystem`.
The local libgit2 dylib links to Apple CoreFoundation and Security, Homebrew libssh2, zlib, iconv, and libSystem.
The runtime feature identity reported SecureTransport, libssh2, built-in SHA-1, and no SHA-256 backend.
No iOS device slice, iOS simulator slice, Android `.so`, XCFramework, AAR, IPA, APK, or AAB size was produced.

## iOS result

The iOS result is unvalidated.
The isolated Flutter probe and its offline dependency resolution were created successfully inside the spike.
The probe imports the real Dart engine and would display either its native backend identity or the exact load error.

The native probe explicitly targets `arm64-apple-ios13.0-simulator`.
It exited 1 with this exact error because Homebrew supplied only a macOS libgit2 dylib.

```text
ld: building for 'iOS-simulator', but linking in dylib (/opt/homebrew/Cellar/libgit2/1.9.1/lib/libgit2.1.9.1.dylib) built for 'macOS'
clang: error: linker command failed with exit code 1 (use -v to see invocation)
```

Apple's [Xcode support matrix](https://developer.apple.com/support/xcode) lists Xcode 26.6 deployment targets, device support, and simulator support as iOS 15 or later.
The required iOS 13 target is therefore outside the supported Xcode 26.6 range and is a production blocker even if a correct iOS 13 libgit2 slice is later obtained.

The exact Flutter simulator build command exited 1 before application compilation.
The decisive raw failure included these lines.

```text
CoreSimulatorService connection became invalid. Simulator services will no longer be available.
xcodebuild: error: Could not resolve package dependencies:
error opening '/Users/mdkaifansari04/.cache/clang/ModuleCache/Swift-1IEYM950OGIQC.swiftmodule' for output: /Users/mdkaifansari04/.cache/clang/ModuleCache: Operation not permitted
cannot open file '/Users/mdkaifansari04/Library/Caches/org.swift.swiftpm/manifests/ManifestLoading/fluttergeneratedpluginswiftpackage.dia' for diagnostics emission (Operation not permitted)
```

The exact Flutter runtime command exited 1 with this result.

```text
No devices found yet. Checking for wireless devices...
No supported devices found with name or id matching 'ios'.
```

The accessible [`git2dart` iOS guide](https://github.com/DartGit-dev/git2dart/blob/main/doc/ios.md) advertises CocoaPods and XCFramework support.
The exact iOS slices, minimum deployment target, static-link details, symbol retention, and runtime behavior of the published binary archive remain unverified.

## Android result

The Android result is unvalidated.
The expected Android SDK directory was absent, and the instructions explicitly prohibited an Android build in that condition.
No emulator or device run occurred.

The maintained [`git2dart` Android guide](https://github.com/DartGit-dev/git2dart/blob/main/doc/android.md) advertises API 21 or later and `arm64-v8a` plus `x86_64` binaries.
That guide also describes extraction of a bundled Mozilla CA set for Android TLS.
These are researched wrapper claims, not measured ForkMesh evidence.
Support for `armeabi-v7a`, 16 KB page size, app-private storage behavior, scoped-storage export, TLS trust, and credentials remains unverified.

## Candidate comparison

The labels below keep evidence classes explicit.

- **Measured fact** means this workspace produced the result through the guarded host harness or a local artifact inspection.
- **Researched fact** means the cited upstream project, package registry, or platform owner documents the claim.
- **Inference** means the conclusion follows from those facts but was not directly measured here.
- **Unknown** means Phase 1 did not produce enough evidence for a production claim.

Dates written in upstream changelogs are treated only as changelog labels.
They are not represented here as Pub.dev publication timestamps.

### Narrow libgit2 C shim plus Dart FFI

- **Mobile packaging and floor:** Measured fact: the spike uses macOS arm64 libgit2 1.9.1 and a 39,480-byte macOS arm64 bridge.
- **Mobile packaging and floor:** Unknown: no Android ABI, AAR, iOS device slice, iOS simulator slice, XCFramework, minimum Android API, or supported iOS deployment floor was produced.
- **Core workflow:** Measured fact: create, bare origin, clone, offline reopen with a fresh engine instance, branch checkout, stage, commit, fetch, ref inspection, push, and typed stale rejection passed.
- **Credentials, progress, and cancellation:** Researched fact: libgit2 exposes credential, certificate, transfer, sideband, push-progress, and per-ref push callbacks through its [remote callback API](https://libgit2.org/docs/reference/main/remote/git_remote_callbacks.html).
- **Credentials, progress, and cancellation:** Measured fact: this shim preserves transfer events, per-ref push status, and an exact negative cancellation return, but it did not exercise HTTPS, SSH, certificates, or secret storage.
- **Repository features:** Researched fact: libgit2 supports shallow depth, bare repositories, normal worktrees, linked worktrees, and submodules.
- **Repository features:** Researched fact: no complete partial-clone or Git LFS client is exposed, and SHA-256 repositories require an experimental API-incompatible build.
- **License and dependencies:** Researched fact: libgit2 is GPLv2 with a linking exception, while the measured build also links platform security libraries, libssh2, zlib, and iconv.
- **Size and resources:** Measured fact: the decisive JSON records a 1,095,312-byte host engine dylib, but that number does not predict stripped IPA, AAB, or peak-memory cost.
- **Decision:** This is the best-measured behavioral baseline and gives the strongest callback control, but a reviewed mobile build, update pipeline, and dependency inventory remain blockers.

### git2dart plus git2dart_binaries

- **Maintenance metadata:** Researched fact: [`git2dart` 0.5.3](https://pub.dev/packages/git2dart) is an active MIT fork, and its [changelog](https://pub.dev/packages/git2dart/changelog) labels 0.5.3 as `2026-06-30`.
- **Maintenance metadata:** Researched fact: [`git2dart_binaries` 1.11.4](https://pub.dev/packages/git2dart_binaries) supplies native artifacts, and its [changelog](https://pub.dev/packages/git2dart_binaries/changelog) labels 1.11.4 as `2026-06-29` and identifies libgit2 1.9.4.
- **Mobile packaging and floor:** Researched fact: the [Android guide](https://github.com/DartGit-dev/git2dart/blob/main/doc/android.md) claims API 21 or later with `arm64-v8a` and `x86_64`, while the binaries changelog describes build coverage for `armeabi-v7a`, `arm64-v8a`, `x86`, and `x86_64`.
- **Mobile packaging and floor:** Researched fact: the [iOS guide](https://github.com/DartGit-dev/git2dart/blob/main/doc/ios.md) claims CocoaPods and XCFramework integration.
- **Mobile packaging and floor:** Unknown: the accessible evidence did not establish the exact archive checksums, XCFramework slices, iOS deployment target, static-link flags, exported symbols, 16 KB Android page compatibility, or runtime success.
- **Core workflow:** Researched fact: the wrapper API advertises initialize, open, clone, read, branch, checkout, index, commit, fetch, and push operations backed by libgit2.
- **Credentials, progress, and cancellation:** Researched fact: the wrapper exposes libgit2 remote credential and progress concepts, but Phase 1 did not verify secure token handoff, certificate rejection, SSH host-key policy, per-ref push rejection, or a negative transfer-callback cancellation surface.
- **Repository features:** Researched fact: the wrapper documents bare repositories, normal worktrees or checkout, linked worktrees, and submodules.
- **Repository features:** Inference: shallow clone may be available through underlying libgit2 only if the exact wrapper surface exposes depth, while partial clone, a complete LFS client, and normal-build SHA-256 support must be treated as unavailable.
- **License and dependencies:** Researched fact: the Dart wrapper is MIT, while distributed binaries can contain libgit2, TLS, SSH, compression, and CA-data components with separate notice and update obligations.
- **Size and resources:** Unknown: package download size is not a stripped app contribution, and no per-ABI size, IPA or AAB delta, RSS, disk-growth, battery, or cancellation-cleanup measurement was produced.
- **Decision:** This is the leading production integration candidate because it already presents a Dart API and claims both mobile platforms, but source pinning and direct archive, callback, credential, and device audit are mandatory.

### libgit2dart

- **Maintenance metadata:** Researched fact: [`libgit2dart` 1.2.2](https://pub.dev/packages/libgit2dart) is marked discontinued and lists only 64-bit Linux, macOS, and Windows.
- **Mobile packaging and floor:** Researched fact: it supplies no Android or iOS platform artifact, ABI list, or mobile OS floor.
- **Core workflow:** Researched fact: its desktop API documents repository initialization, clone, object access, branches, checkout, merge, worktrees, and submodules.
- **Credentials, progress, and cancellation:** Unknown: no supported mobile credential, certificate, progress, cancellation, or per-ref rejection contract was established.
- **Repository features:** Unknown: shallow and partial clone, LFS, mobile linked-worktree behavior, and SHA-256 support were not established for this discontinued package.
- **License and dependencies:** Researched fact: the wrapper is MIT and bundles libgit2 for supported desktops, so the libgit2 and transitive binary obligations still apply.
- **Size and resources:** Unknown: no mobile artifact or resource measurement exists.
- **Decision:** Reject because the hard platform requirement fails before behavioral comparison.

### git_on_dart

- **Maintenance metadata:** Researched fact: [`git_on_dart` 0.1.4](https://pub.dev/packages/git_on_dart) is an MIT pure-Dart package from an unverified uploader.
- **Mobile packaging and floor:** Inference: pure Dart removes a native Git ABI matrix, but the supported Dart, Flutter, Android, and iOS floors still require an app build and were not measured.
- **Core workflow:** Researched fact: the README claims initialize, clone, branch, stage, commit, fetch, push, bare repositories, HTTPS, and SSH.
- **Core workflow:** Researched fact: the repository's [`IMPLEMENTATION.md`](https://github.com/sojankreji/git-in-dart/blob/main/IMPLEMENTATION.md) says remote operations and pack writing are incomplete, which conflicts with using the README as production evidence.
- **Credentials, progress, and cancellation:** Researched fact: dependencies include `dartssh2`, and the README claims progress, but typed stale-ref handling, certificate policy, host-key policy, cancellation, and secret-store integration remain unknown.
- **Repository features:** Researched fact: the package limitations say LFS and submodules are not implemented and object identity is SHA-1 only.
- **Repository features:** Unknown: shallow clone, partial clone, linked worktrees, crash recovery, and full pack-protocol interoperability were not established.
- **License and dependencies:** Researched fact: the package is MIT and depends on archive, crypto, `dartssh2`, path, and path-provider packages, which moves protocol and security maintenance into the Dart dependency graph.
- **Size and resources:** Unknown: no AOT size, RSS, clone throughput, disk amplification, or large-pack evidence was produced.
- **Decision:** Reject for the first engine because the required remote workflow is contradicted by the implementation status.

### dart_git

- **Maintenance metadata:** Researched fact: [`dart_git` 0.0.2](https://pub.dev/packages/dart_git/versions) is an experimental Apache-2.0 pure-Dart package with a single old release line.
- **Mobile packaging and floor:** Inference: native ABIs do not apply to pure Dart, but current Flutter compatibility and Android or iOS deployment floors were not established.
- **Core workflow:** Unknown: no credible maintained evidence established the complete clone, fetch, push, credentials, progress, cancellation, and stale-ref workflow required here.
- **Repository features:** Unknown: shallow and partial clone, bare repositories, normal or linked worktrees, submodules, LFS, and SHA-256 repository support were not established.
- **License and dependencies:** Researched fact: the package is Apache-2.0 and brings multiple Dart protocol, archive, crypto, and filesystem dependencies.
- **Size and resources:** Unknown: no current mobile AOT size, memory, storage, or performance evidence exists.
- **Decision:** Reject because maintenance and required behavior are not credible enough for a security-sensitive repository engine.

### go-git plus gomobile

- **Maintenance metadata:** Researched fact: [`go-git` v5.19.1](https://github.com/go-git/go-git/releases/tag/v5.19.1) has a release dated 2026-05-18 and is Apache-2.0 licensed.
- **Mobile packaging and floor:** Researched fact: [`gomobile bind`](https://pkg.go.dev/golang.org/x/mobile/cmd/gomobile) can emit an Android AAR for arm, arm64, x86, and x86_64 with default and minimum API 16.
- **Mobile packaging and floor:** Researched fact: gomobile can emit an Apple XCFramework with arm64 device and arm64 or x86_64 simulator code and defaults its iOS version flag to 13.0.
- **Core workflow:** Researched fact: go-git implements initialize, open, clone, read, branch, checkout, add, commit, fetch, and push in process.
- **Credentials, progress, and cancellation:** Researched fact: it supports HTTPS credentials, token authentication, in-process SSH, human-readable progress writers, and transport-context cancellation.
- **Credentials, progress, and cancellation:** Inference: an adapter must add typed progress events, durable cancellation semantics above transport cancellation, secure key handling, and explicit ref-status mapping.
- **Repository features:** Researched fact: go-git supports shallow clone, bare and normal worktrees, and submodules, but not linked-worktree parity, partial clone, or an integrated LFS client.
- **Repository features:** Researched fact: its [compatibility document](https://github.com/go-git/go-git/blob/master/COMPATIBILITY.md) limits SHA-256 support to selected initialization and commit paths rather than complete fetch and push interoperability.
- **Repository features:** Researched fact: its local `file` transport can delegate to a Git executable, so that transport must be excluded and protected by the same no-executable sentinel.
- **License and dependencies:** Researched fact: the module is Apache-2.0 and its dependency inventory includes direct and indirect Go modules that require license, vulnerability, and SBOM review.
- **Size and resources:** Unknown: no stripped AAR, XCFramework, IPA, AAB, Go runtime RSS, or repository benchmark was produced.
- **Decision:** Keep as the strongest non-libgit2 fallback experiment if libgit2 mobile packaging or callback ownership fails.

### JGit

- **Maintenance metadata:** Researched fact: [`JGit` 7.7.0](https://projects.eclipse.org/projects/technology.jgit/releases/7.7.0) has a release dated 2026-06-10 and the project remains actively maintained.
- **Mobile packaging and floor:** Researched fact: JGit is Java rather than a native ABI bundle and version 7 requires Java 17.
- **Mobile packaging and floor:** Unknown: Android desugaring, required Java APIs, minimum SDK, DEX and R8 behavior, and runtime compatibility were not validated, and JGit has no iOS engine path.
- **Core workflow:** Researched fact: JGit supports initialize, open, clone, read, branch, checkout, add, commit, fetch, and push.
- **Credentials, progress, and cancellation:** Researched fact: it provides HTTP and SSH transports, credential providers, progress monitors, cancellation checks, and typed remote update status including non-fast-forward rejection.
- **Repository features:** Researched fact: JGit supports bare and normal repositories, checkout, submodules, and a separate LFS module.
- **Repository features:** Unknown: a supported shallow or partial-clone contract, linked-worktree parity, and SHA-256 repository interoperability were not established for this product.
- **License and dependencies:** Researched fact: JGit uses the Eclipse Distribution License or BSD-3-Clause and its core artifact depends on JavaEWAH, SLF4J, and Commons Codec, with extra modules for SSH and LFS.
- **Size and resources:** Unknown: no DEX or R8 contribution, method count, RSS, disk use, or Android performance measurement was produced.
- **Decision:** Reject as the shared engine because there is no iOS path, while retaining it only as a possible Android-specific fallback if a split-engine architecture is later justified.

### Dulwich plus embedded CPython

- **Maintenance metadata:** Researched fact: [`Dulwich` 1.2.10](https://pypi.org/project/dulwich/) is a maintained Python Git implementation offered under Apache-2.0 or GPL-2.0-or-later terms.
- **Mobile packaging and floor:** Researched fact: CPython's [Android embedding documentation](https://docs.python.org/3/using/android.html) describes app-distributed Python, but the chosen distribution determines ABIs and the Android API floor.
- **Mobile packaging and floor:** Researched fact: CPython's [iOS documentation](https://docs.python.org/3/using/ios.html) describes device and simulator XCFramework embedding with arm64 and x86_64 coverage and a default iOS 13 floor.
- **Core workflow:** Researched fact: the [`dulwich.porcelain` API](https://dulwich.io/api/dulwich.porcelain.html) exposes initialize, clone, status, add, commit, branch, fetch, and push.
- **Credentials, progress, and cancellation:** Researched fact: Dulwich supports HTTPS authentication and progress callbacks, while its default SSH path can invoke an external SSH executable unless an in-process backend such as Paramiko is selected.
- **Credentials, progress, and cancellation:** Unknown: a product-grade cancellation token, certificate and host-key policy, secure-store bridge, and typed stale-ref mapping were not measured.
- **Repository features:** Researched fact: Dulwich supports shallow clone, protocol-v2 object filters, bare and normal repositories, submodules, LFS primitives, and SHA-256 repository work.
- **Repository features:** Unknown: linked-worktree parity and end-to-end interoperability of each advanced feature were not established.
- **License and dependencies:** Researched fact: choosing Apache-2.0 avoids the GPL option for Dulwich, but distribution also includes CPython under the PSF license plus optional TLS, SSH, compression, and native-extension dependencies.
- **Size and resources:** Unknown: no Python runtime, standard-library, wheel, IPA, AAB, RSS, startup, or clone-throughput measurement was produced.
- **Decision:** Reject for the first engine because the CPython runtime and packaging surface add substantial unmeasured size, security-update, and lifecycle risk.

### isomorphic-git plus an embedded JavaScript runtime

- **Maintenance metadata:** Researched fact: [`isomorphic-git` 1.38.7](https://www.npmjs.com/package/isomorphic-git) is an MIT JavaScript implementation with an active npm release line and multiple runtime dependencies.
- **Mobile packaging and floor:** Researched fact: the project reports browser testing on Android and iOS, but it does not provide a Flutter AAR, XCFramework, Dart binding, or supported mobile OS floor.
- **Mobile packaging and floor:** Inference: ForkMesh would need to embed and maintain a JavaScript runtime and a filesystem and HTTP bridge on both platforms.
- **Core workflow:** Researched fact: it supports initialize, clone, read, branch, checkout, add, commit, fetch, and push with HTTP transports.
- **Credentials, progress, and cancellation:** Researched fact: it supports HTTPS authentication callbacks and [`onProgress`](https://isomorphic-git.org/docs/en/onProgress), but it does not provide SSH transport or a general cancellation contract.
- **Credentials, progress, and cancellation:** Researched fact: documented error codes distinguish non-fast-forward push rejection from generic failures.
- **Repository features:** Researched fact: it supports shallow clone and normal worktrees, but partial clone, submodules, LFS, and SHA-256 repositories are absent, and bare repository behavior is only an approximation through checkout choices.
- **License and dependencies:** Researched fact: the package is MIT and its JavaScript dependency tree and embedded runtime would require independent license, vulnerability, and sandbox review.
- **Size and resources:** Unknown: no bundled runtime size, AOT or JIT policy result, RSS, startup, storage, or throughput evidence was produced.
- **Decision:** Reject because the extra runtime and missing SSH and advanced repository features provide no clear advantage over the better-supported candidates.

### gix or gitoxide plus a Rust FFI layer

- **Maintenance metadata:** Researched fact: [`gix` and gitoxide](https://github.com/GitoxideLabs/gitoxide/releases) are active Rust projects offered under MIT or Apache-2.0.
- **Mobile packaging and floor:** Researched fact: Rust supports Android and Apple compilation targets, but gitoxide does not publish a ForkMesh-ready AAR, XCFramework, Dart binding, ABI matrix, or mobile OS floor.
- **Core workflow:** Researched fact: gix exposes repository open, object and ref access, clone, and fetch work, while its [`push` module](https://docs.rs/gix/latest/gix/push/index.html) does not yet provide the complete push workflow required here.
- **Credentials, progress, and cancellation:** Researched fact: gix has credential, progress, and interrupt facilities, but default SSH integration can rely on an external executable and mobile secure-transport behavior was not measured.
- **Repository features:** Researched fact: bare and normal repositories, shallow operations, submodule work, and SHA-256 work exist at differing maturity levels.
- **Repository features:** Unknown: partial clone, linked-worktree parity, LFS, and stable end-to-end mobile behavior were not established.
- **License and dependencies:** Researched fact: the dual permissive license is suitable in principle, while a production build still needs a Cargo lock, advisory audit, notices, and FFI ownership review.
- **Size and resources:** Unknown: no stripped mobile archive, IPA or AAB delta, RSS, storage, or benchmark was produced.
- **Decision:** Reject for Phase 2 because complete push support is a hard requirement, but revisit as the Rust implementation matures.

### Native Swift-only engine

- **Mobile packaging and floor:** Researched fact: a native Swift implementation provides an iOS path only and cannot serve as the common Android and iOS engine.
- **Core workflow:** Researched fact: no maintained complete Swift Git protocol and object engine was found, while projects such as [SwiftGitX](https://forums.swift.org/t/swiftgitx-integrate-git-to-your-apps/77827) are wrappers around libgit2 rather than independent engines.
- **Credentials, progress, cancellation, and repository features:** Inference: a Swift libgit2 wrapper inherits libgit2's capabilities and limitations but adds a second platform-specific wrapper surface.
- **License and dependencies:** Inference: the binary and notice inventory remains the libgit2 inventory plus the Swift wrapper's license and Swift runtime considerations.
- **Size and resources:** Unknown: no Android counterpart, mobile archive, IPA delta, RSS, or throughput evidence was produced.
- **Decision:** Reject as a shared engine because it duplicates wrapper work on iOS and leaves Android unsolved.

## libgit2 capability research

[Research fact] The spike measured local libgit2 1.9.1, while the current researched upstream release is [`libgit2` 1.9.4](https://github.com/libgit2/libgit2/releases/tag/v1.9.4).
Measured behavior must not be presented as 1.9.4 behavior.

[Research fact] libgit2 exposes credential, certificate, transfer-progress, push-progress, and push-reference callbacks through its [remote callback API](https://libgit2.org/docs/reference/main/remote/git_remote_callbacks.html) and [authentication guide](https://libgit2.org/docs/guides/authentication/).

[Research fact] libgit2 has APIs for [bare repositories](https://libgit2.org/docs/reference/main/repository/git_repository_init_ext.html), [worktrees](https://libgit2.org/docs/reference/main/worktree/index.html), and [submodules](https://libgit2.org/docs/reference/main/submodule/index.html).

[Research inference] The released clone and fetch option structures expose shallow depth but no credible promisor or object-filter API, so partial clone is not a supported production assumption.

[Research fact] Git LFS is a separate protocol and object-transfer concern, and libgit2 does not provide a complete LFS client.

[Research fact] SHA-256 remains experimental in libgit2 v1 and requires a special build that is API-incompatible with the normal SHA-1 build.
The measured Homebrew engine reported `sha256=none`.

[Research inference] The current `git2dart` public clone surface does not expose the exact negative `transfer_progress` cancellation control used by this shim.
Production must either verify an equivalent wrapper API or maintain a narrowly reviewed extension.

## Licenses and notices

[`git2dart`](https://github.com/DartGit-dev/git2dart/blob/main/LICENSE) is MIT licensed.
[`libgit2`](https://github.com/libgit2/libgit2/blob/main/COPYING) is GPLv2 with a linking exception.
The exception permits linking without applying the GPL to the whole application, but libgit2 itself and modifications to it retain their obligations.
[`OpenSSL`](https://www.openssl.org/source/license.html) uses Apache-2.0 for current releases.
[`libssh2`](https://github.com/libssh2/libssh2/blob/master/COPYING) uses BSD-3-Clause.
Mozilla's CA data is covered by [MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/).
The final mobile binary composition may differ by platform and build flags.
A third-party notice, source-offer, modification, export, and security-update audit is required before distribution.
This section is engineering inventory, not legal advice.

## Unsupported operations and risks

- HTTPS credentials, token refresh, secure-storage handoff, SSH keys, host-key verification, and certificate rejection were not exercised.
- The host fixture used local libgit2 transport, not a real HTTPS or SSH server.
- Partial clone has no credible libgit2 filter API.
- Git LFS requires a separate client and filter or pre-push semantics.
- SHA-256 repositories are not supported by the measured engine.
- Merge, rebase, conflict UI, submodule recursion, worktree interoperability, and bare hosting were not product-tested beyond bare-origin plumbing.
- App suspension, process death, disk exhaustion, pack corruption recovery, and cleanup after interrupted writes were not tested.
- Large-repository memory, cache limits, battery, thermal, and background execution behavior remain unknown.
- Case folding, Unicode normalization, symlinks, executable bits, file providers, and document-provider export need device tests.
- The wrapper's prebuilt native archives and transitive security update process require audit.
- The 39,480-byte bridge and 1,095,312-byte host dylib are not estimates of shipped mobile download size.

## Superseded production recommendation and historical fallback

The previous recommendation to use a maintained `git2dart` fork or a narrow libgit2 shim is superseded by the user-selected owned-engine policy.

Do not copy the host loader, Homebrew path, native bridge, or any libgit2 dependency into production.

The owned engine retains the behavioral requirements established by this experiment, including explicit certificate policy, secret redaction, progress, cancellation, repository recovery, per-ref push status, typed stale rejection, and no-process fallback.

Until owned-engine and mobile-runtime gates pass, advertise local Git authoring and hosting as unavailable.

The supported interim fallback is routing work to another trusted ForkMesh node that truthfully advertises the required capability.

Do not fall back to Termux, a bundled Git executable, a shell command, or another Git implementation package.

## Superseded libgit2 exit criteria

Phase 2 may use the engine abstraction for design work, but mobile Git capability must remain off until all of these criteria pass.

1. Pin and audit the exact wrapper and native archive sources, checksums, architectures, minimum OS versions, exported symbols, and dependency licenses.
2. Build arm64 iOS device and arm64 plus x86_64 simulator slices, package an XCFramework, and run this full engine-backed flow on a simulator and a physical device.
3. Build the selected Android ABIs with a current NDK, verify API floor and 16 KB page compatibility, and run the full flow on an emulator plus a physical device.
4. Exercise HTTPS clone, fetch, and push against a real server with secure token delivery, certificate validation, expired credentials, cancellation, and redacted errors.
5. Measure stripped artifact size, IPA and AAB contribution, peak memory, disk growth, clone time, and cancellation cleanup on representative repositories.
6. Validate app-private repository storage, backup exclusion, iOS document export, Android Storage Access Framework export, and import without exposing mutable internal repository state.
7. Define and test interrupted-operation recovery for clone, checkout, fetch, index writes, commits, and pushes.
8. Complete the third-party notice and source-compliance audit.
9. Keep partial clone, LFS, SHA-256 repositories, recursive submodules, rebase, and merge disabled or explicitly capability-gated until separately validated.

Historical spike verdict: PARTIAL - the host libgit2 core is validated, but mobile packaging, iOS runtime, Android runtime, and remote credential flows remain unvalidated.

Production verdict: owned-engine specification complete for review in G0, with no production Git capability enabled until the G1 through G7 gates pass.
