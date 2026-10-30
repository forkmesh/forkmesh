# Opus for the phone call engine

FM Media ([`docs/server/fm-media.md`](../server/fm-media.md)) replaces
libwebrtc, and with it the copy of Opus libwebrtc carried. The phones keep
Opus as the voice codec, and the engine has to survive a third of its audio
packets lost ("Beyond FaceTime", idea 2), so we build Opus ourselves from a
pinned upstream commit with its deep-learning features on: deep redundancy
(DRED), deep packet loss concealment (deep PLC) and decoder enhancement (OSCE:
LACE, NoLACE and the BBWENet bandwidth extension), next to the classic in-band
FEC, DTX and PLC. It follows the `native/webrtc/` pattern: a pin, a
reproducible build script and the SHA-256 of every output. Everything lives in
`native/opus/`.

## The pin

`native/opus/PIN` (sourced by the build script):

| Key | Value |
| --- | --- |
| `OPUS_URL` | `https://gitlab.xiph.org/xiph/opus.git`, Xiph's own repository (GitHub's `xiph/opus` is a mirror) |
| `OPUS_TAG`, `OPUS_VERSION` | `v1.6.1`, the newest stable release on 2026-10-21 (no 1.7 tags or release candidates exist); DRED, deep PLC and OSCE arrived in 1.5 |
| `OPUS_TAG_OBJECT` | `a5d6c1b6f4e582df97390f9ac5c6e7c51cbffffe`, the annotated tag object (PGP-signed by Jean-Marc Valin) |
| `OPUS_COMMIT` | `22244de5a79bd1d6d623c32e72bf1954b56235be`, the commit the tag points at |
| `OPUS_MODEL_SHA256`, `OPUS_MODEL_URL` | `a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e`, `https://media.xiph.org/opus/models/opus_data-<sha256>.tar.gz` (134,674,421 bytes): the model weights the release commit names in `autogen.sh` |
| `OPUS_TARBALL_URL`, `OPUS_TARBALL_SHA256` | `opus-1.6.1.tar.gz` on downloads.xiph.org, `6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1` (matches Xiph's `SHA256SUMS.txt` and a fresh download) |
| `OPUS_FEATURES` | `deep_plc dred osce`, what the build turns on |
| `CMAKE_*` | CMake 4.4.4 (`v4.4.4`, tag `569e8959d3c6`, commit `2e0b3aec14fe`) from gitlab.kitware.com, built only where no `cmake` is installed |
| `IOS_DEPLOYMENT_TARGET` | `17.0`, as `native/ios/WebRTCPackage` |
| `ANDROID_NDK_VERSION`, `ANDROID_API`, `ANDROID_ABIS` | `28.2.13676358`, `26` (the app's `minSdk`), `arm64-v8a armeabi-v7a x86_64` |

The tag and commit hashes were read from the repository itself
(`git ls-remote --tags`, then `git rev-parse v1.6.1` and `v1.6.1^{commit}` in
a fetched copy). The build fetches only that tag (`--depth 1`, retried up to
five times because gitlab.xiph.org answered 502 and 504 during these builds,
and skipped when the tag is already present) and stops unless the tag object
and the commit both match, the checkout has no changes other than the model
files, and `HEAD` is the pinned commit.

### The model weights

A git checkout of Opus has the network code but not the weights: upstream's
`autogen.sh` runs `dnn/download_model.sh a5177ec6…` to fetch and check
`opus_data-<sha256>.tar.gz`. `build.sh` does the same itself: it downloads the
file once into `$OPUS_WORK`, stops unless its SHA-256 is `OPUS_MODEL_SHA256`,
stops unless the pinned commit's `autogen.sh` names that same hash, and
unpacks the generated C sources (`dnn/*_data.{c,h}`,
`dnn/dred_rdovae_constants.h`) into the checkout. The tarball's PyTorch
checkpoints (`dnn/models/*.pth`, about 110 MB) are not unpacked; they are what
Xiph trained, and we cannot regenerate them (we cannot rebuild the weights
from source, only check they are the ones the release names).

The release tarball (`OPUS_SOURCE=tarball`, checked against
`OPUS_TARBALL_SHA256`) ships the same weights already unpacked. Comparing the
two on 2026-10-21: every weight array in the tarball is byte-identical to the
model download; the tarball only leaves out the `*_weights_float` debug copies
that sit under `#ifndef DISABLE_DEBUG_FLOAT`, which the CMake build defines
anyway (it does unless `OPUS_DNN_FLOAT_DEBUG`); no other compiled source
differs. A host build from the tarball passes the same smoke tests but its
libraries are not byte-identical to the git build's, so the hashes on this
page are for the default git path, which stays the default because the commit
hash is the stronger pin.

To move the pin: take the newest `vX.Y.Z` tag from
`git ls-remote --tags https://gitlab.xiph.org/xiph/opus.git` (the `^{}` line is
the commit), the model hash from that commit's `autogen.sh`, and the tarball
line from `https://downloads.xiph.org/releases/opus/SHA256SUMS.txt`; update
`PIN`, run every target and the tests below, and record the new hashes and
sizes here.

## Building

`native/opus/build.sh ios|android|host`, run from a checkout (it needs
`tools/forkmesh-safe-rm.sh` at `../../tools/` from the script). Outputs go to
`$OPUS_OUT/<target>` (default `native/opus/build/out`), the source, the model
download and the CMake build trees to `$OPUS_WORK` (default
`native/opus/build/work`). `build/` is ignored by git; nothing built is
committed. `OPUS_BUILD_FEATURES` overrides `OPUS_FEATURES` (`none`, or any of
`deep_plc dred osce`), which is how the size table below was measured.

| Target | Runs on | Needs |
| --- | --- | --- |
| `host` | Linux | a C compiler, git, curl, CMake 3.16+ |
| `android` | Linux | the NDK at `~/Android/Sdk/ndk/28.2.13676358` (or `$OPUS_ANDROID_NDK`); the script checks `source.properties` names the pinned version |
| `ios` | macOS with Xcode | `xcodebuild`, `zip`, `/usr/bin/python3` |

The script uses `cmake` from `PATH`, or `$OPUS_CMAKE`. With neither (the office
Mac has no CMake, Homebrew or MacPorts, and Xcode does not ship one), it fetches
the pinned CMake tag, checks it the same way, and bootstraps it into
`$OPUS_WORK/cmake-4.4.4-install` once (40 minutes on the 4-core Intel Mac while
other builds ran). The generator is always Unix Makefiles, so no Ninja is
needed. With the models on, a host build took 2.5 minutes on the agent box and
the three iOS slices 3.5 minutes on the Mac; the 20 MB weight sources dominate
compile time.

Every build directory is recreated through `fm_safe_rm_tree` with `$OPUS_WORK`
or `$OPUS_OUT` as the expected parent. Builds are reproducible: `LC_ALL=C`,
`TZ=UTC`, `ZERO_AR_DATE=1` (Apple `ar`/`ranlib`), and `-ffile-prefix-map` for
the work folder and the NDK (the NDK compiles with `-g`, so its paths would
otherwise land in the static libraries' debug info). Two Apple quirks needed
`native/opus/ios/normalize.py`:

- cctools `ranlib`/`libtool` leave the padding after the archive symbol
  table's last string uninitialized, so two runs over identical objects gave
  different archives (two bytes in 5 MB). `normalize.py archive` zeroes that
  padding in each thin library before `lipo`.
- `xcodebuild -create-xcframework` writes `AvailableLibraries` in either
  order. `normalize.py plist` sorts it by `LibraryIdentifier`.

Every target was then built twice in different work folders with identical
`SHA256SUMS`.

## Options and why

| Option | Value | Why |
| --- | --- | --- |
| `OPUS_DRED` | `ON` | up to a second of speech history in every packet, decoded by a neural vocoder, so a burst of lost packets is rebuilt from the next packet that arrives |
| `OPUS_DEEP_PLC` | `ON` | neural concealment when nothing (no LBRR, no DRED) covers a lost frame; DRED and OSCE turn it on in CMake anyway, the flag makes it explicit |
| `OPUS_OSCE` | `ON` | LACE and NoLACE clean up SILK's output at low bitrates; BBWENet extends wideband SILK to fullband |
| `ENABLE_OSCE_BWE` (C define) | with OSCE | upstream's autotools `--enable-osce` defines it, its CMake build does not, so without it `OPUS_SET_OSCE_BWE` returns unimplemented and BBWENet is dead weight; the script adds it whenever `osce` is on |
| `OPUS_FIXED_POINT` | `OFF` (float) | the DNN features require float (upstream refuses fixed point with them); every target has a hardware FPU, and the phones capture float PCM |
| `OPUS_ENABLE_FLOAT_API` | `ON` | `opus_encode_float` / `opus_decode_float` take the capture and playback buffers as they are |
| `OPUS_CUSTOM_MODES` | `OFF` | only 48 kHz standard frames are used |
| `OPUS_HARDENING` | `ON` (upstream default) | cheap run-time checks, meant for production |
| `OPUS_ASSERTIONS`, `OPUS_DNN_FLOAT_DEBUG` | `OFF` | debug only |
| `OPUS_BUILD_TESTING`, `OPUS_BUILD_PROGRAMS` | `OFF` | not shipped; our own smoke tests below |
| `CMAKE_POSITION_INDEPENDENT_CODE` | `ON` | the static libraries link into the JNI library and the Go test client |
| `OPUS_PRESUME_NEON` | `ON` for `armeabi-v7a` only | Opus 1.6.1's DNN dispatch declares `DNN_COMPUTE_LINEAR_IMPL` for 32-bit ARM with run-time NEON detection but only defines it when dot-product support is also possible, so the armv7 `libopus.so` failed to link (`undefined symbol: DNN_COMPUTE_LINEAR_IMPL`) and the static library carried the same hole. The NDK already compiles armv7 with NEON (`__ARM_NEON` is predefined), so presuming it costs nothing |
| other intrinsics | upstream defaults | NEON with run-time detection on arm64; SSE, SSE2, SSE4.1 and AVX2 with run-time detection on x86_64. Upstream's CMake never builds the arm64 dot-product DNN kernels (`dnn/arm/nnet_dotprod.c`; autotools does), so the networks run on plain NEON |

### Using the features at run time

Compiled in is not switched on. What the engine has to set:

- **Encoder:** `OPUS_SET_DRED_DURATION(n)` in 10 ms units (100 = 1 s of
  history) and `OPUS_SET_PACKET_LOSS_PERC(p)`: the DRED share of the bitrate
  grows with the expected loss, so `p` should follow the measured loss. DRED
  takes its bits out of the target bitrate. In the host test (synthetic voice,
  30% loss expected, `OPUS_SET_INBAND_FEC(1)` too) the history each packet
  carried depended on the bitrate:

  | Target bitrate | Actual | DRED history per packet | Lost frames rebuilt from DRED | LSD: classic PLC / deep PLC / DRED |
  | --- | --- | --- | --- | --- |
  | 24 kbps | 21 kbps | none | 0 of 48 | 7.77 / 8.56 / 8.56 dB |
  | 32 kbps | 30 kbps | 95 ms | 40 of 48 | 7.83 / 8.43 / 5.24 dB |
  | 48 kbps | 39 kbps | 415 ms | 48 of 48 | 7.72 / 8.39 / 4.41 dB |
  | 64 kbps | 48 kbps | 375 ms | 48 of 48 | 7.70 / 8.39 / 4.13 dB |

  So under 32 kbps DRED carries nothing; the engine should keep voice at
  48 kbps or more when it expects loss, or raise the bitrate when loss rises.
- **Decoder:** `OPUS_SET_COMPLEXITY` on the decoder picks the extras: 5 or
  more turns on deep PLC, 6 LACE, 7 or more NoLACE, and with
  `OPUS_SET_OSCE_BWE(1)` complexity 4 or more adds BBWENet. The default, 0,
  is the classic decoder.
- **DRED recovery:** the jitter buffer keeps an `OpusDREDDecoder` and an
  `OpusDRED`. When a packet arrives after `n` lost frames, it calls
  `opus_dred_parse(dred_dec, dred, packet, len, 48000, 48000, &end, 0)`,
  which returns how many samples of history the packet holds, then for each
  lost frame `k` (oldest first) `opus_decoder_dred_decode_float(dec, dred,
  (n - k) * 960, pcm, 960)` while the offset is within that history (plain
  PLC beyond it, or LBRR via `decode_fec=1` for the last lost frame), and
  finally decodes the packet itself. `src/opus_demo.c` upstream does the same.

## Size each feature adds

Stripped `libopus.so` bytes (what an APK carries per ABI) and, for iOS, the
`__TEXT` + `__DATA` bytes of the device arm64 `libopus.a` (what links into the
app; the archive file itself is larger). "+DRED" and "+OSCE" are on top of deep
PLC, which both need; the weights are int8 and dominate.

| Build | host x86_64 `.so` | arm64-v8a `.so` | armeabi-v7a `.so` | x86_64 `.so` | iOS arm64 sections |
| --- | --- | --- | --- | --- | --- |
| none | 604,504 | 431,744 | 324,236 | 516,528 | 409,736 |
| deep PLC | 2,161,008 (+1.56 MB) | 1,920,864 (+1.49 MB) | 1,791,588 (+1.47 MB) | 2,063,760 (+1.55 MB) | 1,898,027 (+1.49 MB) |
| deep PLC + DRED | 2,763,136 (+0.60 MB) | 2,522,912 (+0.60 MB) | 2,376,336 (+0.58 MB) | 2,665,688 (+0.60 MB) | 2,487,255 (+0.59 MB) |
| deep PLC + OSCE | 4,467,064 (+2.31 MB) | 4,234,672 (+2.31 MB) | 4,082,376 (+2.29 MB) | 4,383,536 (+2.32 MB) | 4,203,473 (+2.31 MB) |
| all three (shipped) | 5,073,288 (+4.47 MB) | 4,836,720 (+4.40 MB) | 4,667,140 (+4.34 MB) | 4,985,416 (+4.47 MB) | 4,792,701 (+4.38 MB) |

The weights hardly compress: gzip -9 takes the shipped arm64 `.so` to
4,079,192 bytes (268,349 without the models). An APK with all three ABIs
grows by about 13 MB uncompressed; an App Store install by about 4.4 MB.

## Output layout

```
build/out/host/
  include/opus/{opus,opus_defines,opus_types,opus_multistream,opus_projection}.h
  lib/libopus.a  lib/libopus.so -> libopus.so.0 -> libopus.so.0.11.1
  lib/pkgconfig/opus.pc   SHA256SUMS   PIN
build/out/android/
  include/opus/*.h
  lib/{arm64-v8a,armeabi-v7a,x86_64}/{libopus.a,libopus.so}
  SHA256SUMS   PIN
build/out/ios/
  Opus.xcframework/
    ios-arm64/libopus.a  ios-arm64/Headers/Opus/{*.h,fm_opus.h,module.modulemap}
    ios-arm64_x86_64-simulator/libopus.a  .../Headers/Opus/...
  Opus.xcframework.zip   SHA256SUMS   PIN
```

`SHA256SUMS` lists every output file (not `opus.pc`, which holds the absolute
prefix) and the script prints it at the end. The Android `.so` files have the
plain SONAME `libopus.so`, link only `libc`, `libm` and `libdl`, export the 13
DRED entry points, carry the API 26 note, and the 64-bit ones are 16 KB page
aligned (NDK r28's default), as Google Play requires. Every file in the
xcframework is stamped 2000-01-01 and the zip is written in sorted order
without extra attributes, so the zip's hash is stable and can be a SwiftPM
`binaryTarget(url:checksum:)` checksum.

The headers sit in `Headers/Opus/` with their module map, not in `Headers/`,
so the `module.modulemap` cannot collide with another library xcframework's
when Xcode copies headers into the build folder; clang finds it because the
folder is named after the module. C code includes `<Opus/opus.h>`.

`fm_opus.h` is ours: Swift cannot call C variadic functions, so
`opus_encoder_ctl` and `opus_decoder_ctl` are out of its reach.
`fm_opus_encoder_set/get/reset` and `fm_opus_decoder_set/get/reset` are
`static inline` wrappers taking the `*_REQUEST` constants (which Swift does
import), e.g. `fm_opus_encoder_set(enc, OPUS_SET_DRED_DURATION_REQUEST, 100)`.
The DRED functions themselves are not variadic and Swift calls them directly.

## Output hashes (opus 1.6.1, deep PLC + DRED + OSCE)

Built 2026-10-21: `host` and `android` on the agent box (Debian, system CMake
3.31.6, NDK r28c), `ios` on the office Mac (Xcode 26.6, iOS SDK 26.5, pinned
CMake 4.4.4). The headers are upstream's and hash the same everywhere.

| Output | SHA-256 |
| --- | --- |
| `host/lib/libopus.a` (x86_64) | `1be19980269e20d2cae2250e1196b0eb74604a610eb495ddefb19d7dfb02be7f` |
| `host/lib/libopus.so.0.11.1` | `2a8c5739d6297a135ed3d8af79bb77f59e8f4f5c917eb8d8eeb34f62bba4924a` |
| `android/lib/arm64-v8a/libopus.a` | `abe382dc847c3bef712290db249e15b4314ebafc41731e7c73ff94a26436c60e` |
| `android/lib/arm64-v8a/libopus.so` | `d7b86ff1a7f20f82435ac8355c369141edf9966884a039a65bf27b67901ba4ad` |
| `android/lib/armeabi-v7a/libopus.a` | `3a1c3a14be029b0d84a1fe7a51b7c62c26ac15c8db40b59f66c5286dfbbf5033` |
| `android/lib/armeabi-v7a/libopus.so` | `59163e6a0d885cc11882e24cf17b968cf712715ba60428536c2bb83ea4f2fa52` |
| `android/lib/x86_64/libopus.a` | `4ceb84ba09dd6336f41affe78a16f792904d4ded7d6d6f9de32e667aa31fd0bf` |
| `android/lib/x86_64/libopus.so` | `a65d7ed49cedbc4508c86589d078ba7a5792b1a673139c3b8f697b5d68fac728` |
| `ios/Opus.xcframework/ios-arm64/libopus.a` | `cd720c6669b011c54dbcd8f1c50b3c6b7dc7333f932195b4936767df6e70aa98` |
| `ios/Opus.xcframework/ios-arm64_x86_64-simulator/libopus.a` | `97892f05ecf5e8acf589afef9880233709226e24455049a9f6cd1bfee2d41488` |
| `ios/Opus.xcframework/Info.plist` | `e127cf9486723437858c4874aa92a84f3bf9b300251ff2cff3a2b92e8834be9a` |
| `ios/Opus.xcframework.zip` | `b2e4c408e99506ee2454cfaa37fc2d7dd01687d09150478e0fbde067195354bf` |

The device slice's objects carry `LC_BUILD_VERSION` platform `IOS`, minos
17.0; the simulator slice's carry `IOSSIMULATOR`. Hashes change with the
compiler (another Xcode or NDK gives other bytes), so record new ones when
either moves.

## Tests

- `app/tests/test_opus_build.py` checks the pin's shape, that the model hash
  and URL agree, and that the script turns the three features on and keeps
  fixed point off. When `native/opus/build.sh host` has run (or
  `$OPUS_OUT/host` exists) it compiles `native/opus/test/opus_smoke.c`
  against the static and the shared library, runs it, and checks the
  `SHA256SUMS` match the files. When the Android build and the NDK are there,
  it links the same program against each ABI's `libopus.a` and `libopus.so`
  (link only; that check would have caught the armv7 hole above). Without the
  builds those tests skip.
- `opus_smoke.c`, part one: one second of a 440 Hz sine at 48 kHz mono in
  20 ms frames at 32 kbps (VOIP, voice signal, in-band FEC on, 20% expected
  loss), decoded three ways. Required: the version string is the pinned one;
  FEC is on and at least half the packets carry LBRR; 20 to 48 kbps; decoded
  SNR at least 20 dB (aligned by cross-correlation over the first 640 samples
  of delay); frame 30 rebuilt from frame 31's FEC has 960 samples, within
  10 dB of the clean decode and better than PLC; PLC returns 960 samples; with
  DTX on, at least half of one second of silence encodes to packets of two
  bytes or less.
- `opus_smoke.c`, part two (DRED): 3.24 seconds of a synthetic voice (a
  gliding 85-195 Hz glottal tone with three formants and 4 Hz syllables) at
  48 kbps with `OPUS_SET_DRED_DURATION(100)`, 30% expected loss and FEC on.
  The receiver drops 48 of 162 packets (30%) in bursts of 1, 2, 4 and 5 frames
  (20 to 100 ms) and four decoders hear it: clean (complexity 10, nothing
  lost), classic PLC (complexity 0), deep PLC (complexity 10) and DRED
  (complexity 10, lost frames rebuilt from the next packet's DRED as above).
  It reports, over the lost frames only and against the original signal, the
  waveform SNR and the log-spectral distance (LSD, 20 ms Hann frames,
  0-8 kHz). Required: every packet but the first carries DRED, the history
  covers a 5-frame burst, every lost frame is rebuilt from DRED, DRED's LSD
  is at least 1 dB better than classic PLC's, and `OPUS_SET_OSCE_BWE` works.

  Measured on the host build (2026-10-21):

  | Receiver | SNR on lost frames | LSD on lost frames |
  | --- | --- | --- |
  | clean (nothing lost, ceiling) | 11.7 dB | 1.89 dB |
  | classic PLC | 0.2 dB | 7.72 dB |
  | deep PLC | 0.3 dB | 8.39 dB |
  | DRED (48 of 48 lost frames rebuilt) | -0.9 dB | 4.41 dB |

  Waveform SNR says little here: DRED and deep PLC resynthesize speech with a
  vocoder that does not keep the waveform's phase, so all three loss
  receivers sit near 0 dB. The spectral distance is the meaningful number,
  and DRED closes more than half the gap between concealment and no loss. Deep
  PLC does slightly worse than classic PLC on this synthetic tone; it was
  trained on real speech, and the bench (fm-media phase 1) has to measure all
  of this on recorded speech before the engine picks decoder complexities.
- `native/opus/test/ios/` is a Swift package whose executable imports `Opus`
  from `Opus.xcframework` (copied next to `Package.swift`, ignored by git). It
  sets FEC, DTX, DRED (100) and decoder complexity 10 through `fm_opus.h`,
  encodes and decodes 50 frames, parses DRED from every packet, rebuilds one
  frame from DRED, conceals one, and prints the version. It is built with
  `xcodebuild` for the iOS simulator and run there with `xcrun simctl spawn`.

What ran on 2026-10-21 on the office Mac, in `~/opus-build` (a copy of
`native/opus/` and `tools/forkmesh-safe-rm.sh`, outside `~/src` and
`~/webrtc`):

```
bash native/opus/build.sh ios
cd native/opus/test/ios && cp -R ../../build/out/ios/Opus.xcframework .
xcodebuild -scheme OpusSmoke -destination "generic/platform=iOS Simulator" -derivedDataPath ~/opus-build/spm-dd2 build
xcodebuild -scheme OpusSmoke -destination "generic/platform=iOS" -derivedDataPath ~/opus-build/spm-dd2 CODE_SIGNING_ALLOWED=NO build
xcrun simctl create opus-smoke com.apple.CoreSimulator.SimDeviceType.iPhone-17 com.apple.CoreSimulator.SimRuntime.iOS-26-5
xcrun simctl boot <id>; xcrun simctl spawn <id> ~/opus-build/spm-dd2/Build/Products/Debug-iphonesimulator/OpusSmoke
```

Both package builds succeeded and the simulator printed
`libopus 1.6.1 fec=1 lbrr=49 bytes=7029 decoded=48000 plc=960 dred_packets=49 dred_samples=19920 dred_recovered=960`;
the same source built with plain `swiftc -target x86_64-apple-ios17.0-simulator`
printed the same on an iOS 17.0 simulator. `swiftc` also linked the device
(`arm64-apple-ios17.0`) and arm64 simulator slices; the office Mac is an
Intel machine, so the arm64 simulator slice is linked but not run there.

## How the phone engines link it

- **iPhone.** A local Swift package beside `native/ios/WebRTCPackage`
  (`OpusPackage`, `binaryTarget(name: "Opus", path: "Opus.xcframework")`,
  the folder git-ignored like WebRTC's) that the app and the call engine target
  depend on; Swift code then does `import Opus`. Static library, so nothing is
  embedded or signed separately. A release build either copies
  `build/out/ios/Opus.xcframework` in, or a `binaryTarget(url:checksum:)`
  points at the published zip with the hash above.
- **Android.** Kotlin cannot call C directly, so the engine gets a small JNI
  library (built with the app's CMake through `externalNativeBuild`) that
  links `lib/<abi>/libopus.a` statically and exposes encode, decode, FEC and
  DRED decode and the few ctls it needs. One `.so` per ABI then ships in the
  APK. `libopus.so` is there for a JNI library that prefers to load Opus
  separately.
- **Go engine core and bench.** The core does not encode audio (codecs stay
  native, fm-media.md), but the Go test client the bench drives needs real
  Opus packets: it uses the host build through cgo with
  `PKG_CONFIG_PATH=native/opus/build/out/host/lib/pkgconfig`.
