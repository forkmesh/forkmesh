# Clean-machine install evidence

Tracks issue #705. The tooling is version-agnostic: it tests whatever the signed
`latest` manifest publishes and stamps every step with the tag and
`checksums_sha256` it verified, so the same Action re-proves each new release.
Pin a particular one with `FORKMESH_EXPECTED_TAG` /
`FORKMESH_EXPECTED_CHECKSUMS_SHA256` (Actions variables `CLEAN_MACHINE_TAG` /
`CLEAN_MACHINE_CHECKSUMS`).

The releases measured on this page are:

- tag `v0.8.10`, `checksums_sha256` `ac00a840f7da08d8c100bc8d3585784931b8237889ffc4a3d4e84d12e7c8a1a6`
  (the sections up to [v1.2.4 release checks](#v124-release-checks))
- tag `v1.2.4`, `checksums_sha256` `7cac783fc54efa290a77a8628adec41f34951c2dfc3d9465bb6387b93f0f935a`
  ([v1.2.4 release checks](#v124-release-checks))
- tag `v1.2.5`, `checksums_sha256` `60573442c9df313b1adccb0ad2aac92e022aced7d7342b946dc9c4bdf6180272`
  ([v1.2.5 release checks](#v125-release-checks))
- tag `v1.2.7`, `checksums_sha256` `6c9f75425309e01e9f42433c95f97a2ac4c10674a53a06acfde76315a7cb2ae5`
  ([v1.2.7 release checks](#v127-release-checks))

Later releases add a section of their own rather than overwriting this one.

This page uses the evidence classes of the
[launch readiness packet](y-combinator-news-readiness.md): **Measured fact**,
**Repository evidence**, **Inference**, and **Missing proof**. A step is a pass
only when a dated transcript line shows it.

## Status: v0.8.10

| Acceptance step | Linux x86_64 | Windows x86_64 |
| --- | --- | --- |
| Manifest signature and `SHASUMS256.txt` binding | **Measured fact** (2026-09-17 00:07Z, below) | Same manifest; see Linux |
| Blob hash of the downloaded asset | Measured 2026-09-16 (readiness packet §3); installed-binary hash is **Missing proof** | **Measured fact** (2026-09-17 00:05Z, below) |
| Authenticode signature | Not applicable | **Measured fact**: `osslsigncode verify` (below) |
| SmartScreen dialog text | Not applicable | **Missing proof** |
| Installer run on a fresh machine | **Missing proof** | **Missing proof** |
| First launch and node identity creation | **Missing proof** | **Missing proof** |
| Provider CLI discovery | **Missing proof** | **Missing proof** |
| Continual cycle stops at the human decision card | **Missing proof** | **Missing proof** |
| **Stop work** pauses the loop | **Missing proof** | **Missing proof** |
| Uninstall (default keeps data) | **Missing proof** | **Missing proof** |
| `--purge` prompt text | **Missing proof**; the source text is quoted below | No purge option exists (below) |

No fresh VM has run the installer yet. The steps marked missing need one.

### Why these steps were not run on the deploy box

The machine that produced the measurements below runs the production
source-of-truth desktop node. `uninstall.sh` stops every process named
`forkmesh` (`pkill -x forkmesh`, then `pkill -9`), and `install.sh` stops
managed daemons. Running either script there, even with a scratch `HOME`, would
take production down. The host also has no usable VM or container runtime.
`tools/clean_machine_transcript.sh run` therefore refuses to start when a
ForkMesh process or ForkMesh data already exists.

## Linux x86_64

### Measured: release verification (2026-09-17 00:07:37–00:07:41Z)

Command: `tools/clean_machine_transcript.sh verify`. It clones
`.forkmesh/releases/latest` from `https://forkmesh.com/forkmesh/forkmesh` and
takes the Ed25519 key from the live `install.sh`. The installer's SHA-256 was
`c1076e84c5b9c6cbd79d770bf8f886ee9212a36f1cb7a368024dee5e9c556b58`, identical to
`www/public/install.sh` in this repository.

```text
release checkout commit eaf79d882ce125f5d8b92b65c546839791cb1e66 2026-09-16T20:04:03-04:00
release public key (from install.sh): MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0=
$ openssl pkeyutl -verify -pubin -rawin -inkey release-publisher.pem -in .forkmesh/releases/latest/release.json -sigfile .forkmesh/releases/latest/release.json.sig
Signature Verified Successfully
exit=0
manifest tag:              v0.8.10 (expected v0.8.10)
manifest checksums_sha256: ac00a840f7da08d8c100bc8d3585784931b8237889ffc4a3d4e84d12e7c8a1a6
sha256(SHASUMS256.txt):    ac00a840f7da08d8c100bc8d3585784931b8237889ffc4a3d4e84d12e7c8a1a6
expected checksums_sha256: ac00a840f7da08d8c100bc8d3585784931b8237889ffc4a3d4e84d12e7c8a1a6
manifest forkmesh-linux-x86_64 blob_sha256 size: b9d129452537f40e693b6a8c405ad4a67ee967d881a037ddceeeb1522c58cd10 55191448
== release manifest verified 2026-09-17T00:07:41Z checksums_sha256=ac00a840f7da08d8c100bc8d3585784931b8237889ffc4a3d4e84d12e7c8a1a6 ==
```

### Repository evidence: the installer prints no verification lines on success

In `www/public/install.sh`, the manifest check warns only on failure
(“Release manifest signature is missing or invalid.”, “SHASUMS256.txt is not
bound to the authenticated release manifest.”). A successful install prints
“Checking … for a prebuilt release…” and “Installed prebuilt ForkMesh
linux/x86_64 binary to …”, but no signature or hash line. A transcript of
`curl … | bash` alone cannot show the signature and blob-hash lines the
acceptance asks for. The harness therefore records them itself: the
verification above before installing, and `sha256` of the installed binary
against the manifest `blob_sha256` afterwards.

### Missing proof: fresh-VM transcript

**Automated:** dispatch the **Clean-machine VM evidence** Action
(`.forkmesh/clean-machine-vm.yml`). It boots a disposable Debian 13 VM with
QEMU, runs `tools/clean_machine_transcript.sh run` inside it, and keeps the
following in the run's artifacts:

- one screenshot per step;
- `clean-machine.mp4`, a video of the VM display from boot to power-off;
- the dated transcript, the serial console log, and `summary.json`.

Screenshots and video are read from the VM's framebuffer by the host, not
produced inside the guest. Nothing is installed or stopped on the runner.
The driver is `tools/clean_machine_vm.py`. If the runner has no QEMU, the
driver unpacks it from the Debian archive and checks it against the signed
`InRelease` first. It also checks the base image against Debian's `SHA512SUMS`.
Set the Actions variable `CLEAN_MACHINE_MODE=verify` to boot the VM and run
only the release checks.

#### The screen observer

An unattended run cannot say a person read the screen, so a step counts as
observed only when a program did. At each step the guest asks the host over a
control serial port to look at the screen; the host screendumps the
framebuffer, OCRs it with tesseract, and matches the text against
`tools/clean_machine_expectations.json`, which lists the phrases each step must
show (and, for the install step, the `MISMATCH installed binary` text it must
not). The verdict travels back to the guest and the transcript records it:

```text
screen check 2026-09-17T18:23:13Z: release-verified — observed read on screen: Signature Verified Successfully; release manifest verified
```

Three verdicts are possible, and only the first is a pass:

| Verdict | Meaning |
| --- | --- |
| `observed` | every required phrase was read on the VM's screen, and no forbidden phrase was |
| `not-observed` | a required phrase was missing, a forbidden one appeared, OCR was unavailable, or the observer never answered |
| `unchecked` | the step has no entry in the expectations file |

The OCR text of every screenshot is kept beside it as `screenshots/NN-<step>.txt`,
and the full guest/host exchange as `observer-exchange.log`, so each verdict can
be re-derived from the artifacts. `summary.json` counts the three verdicts and
lists every observation. OCR reads prose reliably but garbles hashes, so the
expectations only ever name phrases a person would read; the manifest checksum
binding is proved by the transcript's own commands, not by OCR.

Steps that need someone to drive the GUI still do not pass. The unattended run
never opens the provider picker, and the guest has no signed-in provider CLI, so
provider discovery, the decision card, and **Stop work** stay `not-observed`
until a person runs those steps at the VM.

**By hand:** on a fresh Linux x86_64 VM with a desktop session, as the user who
will run ForkMesh:

```sh
git clone https://forkmesh.com/forkmesh/forkmesh && cd forkmesh
FORKMESH_TRANSCRIPT=$PWD/v0.8.10-clean-machine-linux.log tools/clean_machine_transcript.sh run
```

The script stamps every step with UTC time and `checksums_sha256` and runs
these steps in order:

1. Machine header.
2. Release verification.
3. Provider CLIs present.
4. `curl -fsSL https://forkmesh.com/install.sh | bash`.
5. Installed-binary hash.
6. Node identity files and the settings lines naming the node.
7. The default `uninstall.sh`, then a listing of the kept data directories.
8. `--purge` under a terminal, answered `no`.
9. `--purge` without a terminal.
10. `--purge --yes` to erase the scratch identity.

Between steps 5 and 7 it stops at operator checkpoints for first launch,
provider discovery, the decision card, and **Stop work**. The operator types
the on-screen text at each one. An empty answer is logged as `NOT OBSERVED`.
Commit the log and screenshots next to this page, then update the status table.

First launch creates a node identity and connects to the production mainnode.
The final `--purge --yes` erases the local key, but the node name the VM
announced stays in network history. Use a throwaway account.

The purge prompt text the transcript should show, quoted from
`www/public/uninstall.sh`:

```text
This removes ForkMesh and ALL of its data from this computer:
  • the forkmesh binary and installed source
  • settings, the node identity key (the account cannot be recovered)
  • every mirrored repository, and all chat history
  • the desktop launcher, icons, and login-autostart entry

Type DELETE to continue:
```

Without a terminal, it prints `Refusing to erase ForkMesh data without
confirmation. Re-run with --yes (or set FORKMESH_ASSUME_YES=1): …`.

## Windows x86_64

### Measured: blob hash (2026-09-17 00:05:05–00:05:13Z)

```text
$ curl -fsS -o setup.exe https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/f37477e4bb614d140dac4f42fd103234f2a286548b382bc8a73e6ae3ab4bef74
$ sha256sum setup.exe
f37477e4bb614d140dac4f42fd103234f2a286548b382bc8a73e6ae3ab4bef74  setup.exe
$ stat -c %s setup.exe
73636296
$ file setup.exe
setup.exe: PE32 executable for MS Windows 4.00 (GUI), Intel i386 (stripped to external PDB), Nullsoft Installer self-extracting archive, 7 sections
```

The hash and size equal the manifest entry for
`forkmesh-windows-x86_64-setup.exe`.

### Measured: Authenticode signature (2026-09-17 00:06Z, osslsigncode 2.10, OpenSSL 3.5.7)

The default trust store fails the check, as expected for a self-issued certificate (excerpt):

```text
$ osslsigncode verify -in setup.exe
Message digest algorithm  : SHA256
Current message digest    : 7B022BFAF598F994FC058BBC6AEBACA2A6F6278272038A4F2DB37DA98CF92EF3
Calculated message digest : 7B022BFAF598F994FC058BBC6AEBACA2A6F6278272038A4F2DB37DA98CF92EF3
Signer #0:
    Subject: O=ForkMesh,CN=ForkMesh
    Issuer : O=ForkMesh,CN=ForkMesh
    Serial : 5411072456DB8F3AED962E6335D3288CBD2DF54A
    notBefore : Sep 13 15:23:48 2026 GMT
    notAfter : Sep 10 15:23:48 2036 GMT
Authenticated attributes:
    Signing time: Sep 14 15:31:05 2026 GMT
    Microsoft Individual Code Signing purpose
    Text description: ForkMesh
Countersignatures:
    Timestamp time: Sep 14 15:31:05 2026 GMT
    Issuer: CN=DigiCert Trusted G4 TimeStamping RSA4096 SHA256 2025 CA1,O=DigiCert\, Inc.,C=US
Timestamp Server Signature CRL verification: ok
Timestamp Server Signature verification: ok
    Error: self-signed certificate
Signature verification: failed
Number of verified signatures: 1
Failed
exit=1
```

Pinned to the ForkMesh certificate taken from the signature, the check passes (excerpt):

```text
$ osslsigncode extract-signature -in setup.exe -out sig.p7 -pem
$ openssl x509 -in signer.pem -noout -fingerprint -sha256 -ext basicConstraints,keyUsage,extendedKeyUsage
sha256 Fingerprint=06:E0:93:EA:D7:0B:BD:A1:0B:4C:B6:0A:AE:B2:2C:8C:52:AA:ED:07:6E:21:F4:05:6E:45:2D:38:D1:BE:45:73
X509v3 Basic Constraints: critical
    CA:FALSE
X509v3 Key Usage: critical
    Digital Signature
X509v3 Extended Key Usage: critical
    Code Signing
$ osslsigncode verify -in setup.exe -CAfile signer.pem -TSA-CAfile /etc/ssl/certs/ca-certificates.crt
Signature verification: ok
Number of verified signatures: 1
Succeeded
```

**Measured fact:** the file digest matches its signature. The DigiCert RFC 3161
timestamp (2026-09-14 15:31:05Z) verifies against a public root. The signer
certificate's SHA-256 equals `DEFAULT_WINDOWS_SIGNER_SHA256` in
`tools/forkmesh-release-host.py`, the pin for the release Authenticode
certificate.

**Inference:** Windows does not chain the certificate to a trusted root, so
SmartScreen will warn when the installer first runs, as
`www/docs/install-support-matrix.md` already states. The exact dialog text is
**missing proof** and must be copied from the VM, not predicted.

### Repository evidence: what the Windows uninstaller removes

`tools/package/forkmesh.nsi` installs per user into `%LOCALAPPDATA%\ForkMesh`
(`RequestExecutionLevel user`). Its uninstaller runs `RMDir /r "$INSTDIR"` and
deletes the Start menu and desktop shortcuts, the uninstall registry key, and
the `forkmesh:` URL handler. It has no purge option and no confirmation beyond
the standard NSIS confirm page.

Qt places `AppLocalDataLocation` and `CacheLocation` under
`%LOCALAPPDATA%\ForkMesh\ForkMesh`, inside `$INSTDIR`. The node identity key
(`ForkMeshIdentity::defaultKeyDir`, `AppDataLocation`) lives under Roaming
`%APPDATA%\ForkMesh\ForkMesh`, and native-format settings live in the registry,
so both should survive. Local-data content, such as pull-review worktrees
(`MainWindowPulls.cpp`) and downloaded speech models, sits inside the
directory the uninstaller deletes. **Missing proof:** a Windows transcript must
list both directories before and after uninstall.

### Missing proof: fresh-VM procedure

On a fresh Windows x86_64 VM, record the UTC time and the `checksums_sha256`
value above with every step:

1. Download `forkmesh-windows-x86_64-setup.exe` from `/desktop`, then run
   `Get-FileHash -Algorithm SHA256 .\forkmesh-windows-x86_64-setup.exe`. The
   hash must be `f37477e4…4bef74`.
2. Run `Get-AuthenticodeSignature .\forkmesh-windows-x86_64-setup.exe | Format-List *`,
   and `signtool verify /pa /v` if the SDK is present.
3. Double-click the installer. Copy the SmartScreen dialog text verbatim, both
   before and after **More info**, and take a screenshot.
4. Finish setup and launch. Record the first-run text, then list
   `%APPDATA%\ForkMesh\ForkMesh` and `%LOCALAPPDATA%\ForkMesh`.
5. Run provider discovery, one continual cycle to the decision card, and
   **Stop work**, as in the Linux checkpoints.
6. Quit, uninstall from **Settings → Apps**, and list both directories again.

## v1.2.4 release checks

Measured 2026-09-24 from the deploy box. Every fresh-VM step in the v0.8.10
status table is still **Missing proof** for v1.2.4, and macOS joins them: no
clean Mac has mounted the `.dmg`.

### Measured: release verification (2026-09-24 09:14:06–09:14:10Z)

Command: `FORKMESH_EXPECTED_TAG=v1.2.4 FORKMESH_EXPECTED_CHECKSUMS_SHA256=7cac783f… tools/clean_machine_transcript.sh verify`
(excerpt; the clone was redirected to `mirror38.forkmesh.com`).

```text
release checkout commit c7f59659c5d3817009d700f6bda3b0668c908c2f 2026-09-24T05:10:14-04:00
$ sha256sum install.sh
9435b09eb391fded0f0b357ce52b83d7f3b7c256932b6551842b136905912630  install.sh
release public key (from install.sh): MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0=
Signature Verified Successfully
manifest tag:              v1.2.4 (pinned to v1.2.4)
manifest checksums_sha256: 7cac783fc54efa290a77a8628adec41f34951c2dfc3d9465bb6387b93f0f935a
sha256(SHASUMS256.txt):    7cac783fc54efa290a77a8628adec41f34951c2dfc3d9465bb6387b93f0f935a (pinned to 7cac783fc54efa290a77a8628adec41f34951c2dfc3d9465bb6387b93f0f935a)
manifest linux/x86_64 assets (blob_sha256 size name):
  945dd6c5db5f0c96e84c5ac05f78f24353043c5cbb2bc05c37cd0b9e89d41b2d 68038576 forkmesh-linux-x86_64
  3aa8c1fcab89e58ff7e362051dc344f0b2f9afe9bb12a1abb5bb5f43de048e76 104704504 forkmesh-linux-x86_64.AppImage
  4ceab772da018892196d5b6bd790e9292b2530984eae801e549a5a45e2963290 179173 forkmesh-linux-x86_64.AppImage.zsync
  4e83748b8d3167136474a2dbe16d77a8eda7f9d4cee3f046f83983f389201923 12040567 forkmesh-mirror-node-linux-x86_64
== release manifest verified 2026-09-24T09:14:10Z release=v1.2.4 checksums_sha256=7cac783fc54efa290a77a8628adec41f34951c2dfc3d9465bb6387b93f0f935a ==
```

### Measured: blob hashes (2026-09-24 09:14:17–09:14:48Z)

Each blob came from
`https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/<hash>`:

```text
945dd6c5db5f0c96e84c5ac05f78f24353043c5cbb2bc05c37cd0b9e89d41b2d  68038576   ELF 64-bit LSB pie executable, x86-64       forkmesh-linux-x86_64
4e83748b8d3167136474a2dbe16d77a8eda7f9d4cee3f046f83983f389201923  12040567   ELF 64-bit LSB executable, x86-64           forkmesh-mirror-node-linux-x86_64
5acc89301e80900fa6d60a4b4c49e3e45d818be4a81084ac9c411ca687c0621d  77477752   PE32 executable for MS Windows 4.00 (GUI)   forkmesh-windows-x86_64-setup.exe
2a7539ca6184e97b44b35897603be7b4a59f8b8c1be5b7a5f952f64ebb5786aa  115322125  UDIF disk image, `koly` trailer            forkmesh-macos-arm64.dmg
```

Every hash and size equals its manifest entry.

### Measured: Authenticode signature (2026-09-24 09:15Z)

```text
$ openssl x509 -in signer.pem -noout -fingerprint -sha256 -subject
sha256 Fingerprint=06:E0:93:EA:D7:0B:BD:A1:0B:4C:B6:0A:AE:B2:2C:8C:52:AA:ED:07:6E:21:F4:05:6E:45:2D:38:D1:BE:45:73
subject=CN=ForkMesh, O=ForkMesh
$ osslsigncode verify -in setup.exe -CAfile signer.pem
Current message digest    : 8EA8B99FEEAF452194FB6F6C62A82DE82B2C0916A8E1E5F7E539DA0D2CE624A3
Calculated message digest : 8EA8B99FEEAF452194FB6F6C62A82DE82B2C0916A8E1E5F7E539DA0D2CE624A3
	Timestamp time: Sep 24 06:40:39 2026 GMT
	Issuer: /C=US/O=DigiCert, Inc./CN=DigiCert Trusted G4 TimeStamping RSA4096 SHA256 2025 CA1
```

**Measured fact:** the v1.2.4 installer is signed by the same pinned
certificate as v0.8.10, with a verified DigiCert timestamp. In the fresh-VM
procedure, step 1's hash for v1.2.4 is `5acc8930…c0621d`.

### Missing proof: macOS

The `.dmg` is ad-hoc signed and not notarized (see `codemagic.yaml`,
`macos-desktop`), so Gatekeeper blocks the first launch until the user opens
it with right-click → **Open**. A fresh-Mac transcript must record the
Gatekeeper dialog text, `codesign --verify --deep --strict` on the mounted
app, first launch, and removal.

## v1.2.5 release checks

Measured 2026-09-30 from the deploy box. Every fresh-VM step in the v0.8.10
status table is still **Missing proof** for v1.2.5, and so is every macOS step.

### Measured: release verification (2026-09-30 23:18:55–23:19:06Z)

Command: `FORKMESH_EXPECTED_TAG=v1.2.5 FORKMESH_EXPECTED_CHECKSUMS_SHA256=60573442… tools/clean_machine_transcript.sh verify`
(excerpt; the clone was redirected to `n2.next.forkmesh.com`).

```text
release checkout commit b3c05d8b701e29a7bc7e96679c9cf257f7882b5b 2026-09-30T19:15:08-04:00
$ sha256sum install.sh
391ed91cf222c8453e0c1229b7ac2f4b7ece7edbf756ad44c215f9011538ca12  install.sh
release public key (from install.sh): MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0=
Signature Verified Successfully
manifest tag:              v1.2.5 (pinned to v1.2.5)
manifest checksums_sha256: 60573442c9df313b1adccb0ad2aac92e022aced7d7342b946dc9c4bdf6180272
sha256(SHASUMS256.txt):    60573442c9df313b1adccb0ad2aac92e022aced7d7342b946dc9c4bdf6180272 (pinned to 60573442c9df313b1adccb0ad2aac92e022aced7d7342b946dc9c4bdf6180272)
manifest linux/x86_64 assets (blob_sha256 size name):
  c00f3177b0e87528e0a54e6ea3baa373a8177ea686189fee09579ac5dd664aeb 99527024 forkmesh-linux-x86_64
  8584e3e8bb14fc3a86b9d1eca4ab50b794135fbb85e3a65758201b5e07c4c549 81558008 forkmesh-linux-x86_64.AppImage
  5e1461daa0879015065dc3a8b5aa3280cac7cc582cc2abc92975406f93d084f5 278999 forkmesh-linux-x86_64.AppImage.zsync
  6ea44ac3a3c2bea1631c2d6f5a4328e6596e45be2a7280b44fee13264e2748ba 12800319 forkmesh-mirror-node-linux-x86_64
== release manifest verified 2026-09-30T23:19:06Z release=v1.2.5 checksums_sha256=60573442c9df313b1adccb0ad2aac92e022aced7d7342b946dc9c4bdf6180272 ==
```

### Measured: blob hashes (2026-09-30 23:12:52–23:13:36Z)

Each blob came from
`https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/<hash>`:

```text
c00f3177b0e87528e0a54e6ea3baa373a8177ea686189fee09579ac5dd664aeb  99527024   ELF 64-bit LSB pie executable, x86-64                forkmesh-linux-x86_64
6ea44ac3a3c2bea1631c2d6f5a4328e6596e45be2a7280b44fee13264e2748ba  12800319   ELF 64-bit LSB executable, x86-64, Go, static        forkmesh-mirror-node-linux-x86_64
46df261a8e7ce2dbb92614c839410a6aadf3a0519877c7e0fdb6dae9561299c9  78321496   PE32 executable (GUI), Nullsoft Installer           forkmesh-windows-x86_64-setup.exe
96ecd3f1f76cc7dd47d1f3746dc6817dd4ac52c7e28aecb98672be9981396acb  165335198  zlib-compressed DMG holding an APFS volume          forkmesh-macos-arm64.dmg
```

Every hash and size equals its manifest entry. `forkmesh-macos-x86_64.dmg`
has the same `blob_sha256` and size as `forkmesh-macos-arm64.dmg`, so it was
not downloaded twice.

### Measured: the macOS DMG is universal (2026-09-30)

`7z` opened the image and extracted two Mach-O files from it:

```text
ForkMesh.app/Contents/MacOS/ForkMesh                          Mach-O universal binary with 2 architectures: [x86_64] [arm64]
ForkMesh.app/Contents/Frameworks/QtCore.framework/.../QtCore  Mach-O universal binary with 2 architectures: [x86_64] [arm64]
```

**Measured fact:** one universal build is published under both macOS names, so
both labels are correct. Other bundled libraries were not inspected.

### Measured: Authenticode signature (2026-09-30, osslsigncode 2.9)

```text
$ openssl x509 -in signer.pem -noout -fingerprint -sha256 -subject
sha256 Fingerprint=06:E0:93:EA:D7:0B:BD:A1:0B:4C:B6:0A:AE:B2:2C:8C:52:AA:ED:07:6E:21:F4:05:6E:45:2D:38:D1:BE:45:73
subject=CN=ForkMesh, O=ForkMesh
$ osslsigncode verify -in setup.exe -CAfile signer.pem
Current message digest    : 017BECDF5AEB48B190EC4EE573BCC1BCD3BFDF283CDE11C7B6FA5F3B6A3A3748
Calculated message digest : 017BECDF5AEB48B190EC4EE573BCC1BCD3BFDF283CDE11C7B6FA5F3B6A3A3748
	Timestamp time: Sep 26 02:02:25 2026 GMT
	Issuer: /C=US/O=DigiCert, Inc./CN=DigiCert Trusted G4 TimeStamping RSA4096 SHA256 2025 CA1
Signature verification: ok
$ osslsigncode verify -in setup.exe
Verify error: self-signed certificate
```

**Measured fact:** the v1.2.5 installer is signed by the same pinned
certificate as v0.8.10 and v1.2.4, with a verified DigiCert timestamp. In the
fresh-VM procedure, step 1's hash for v1.2.5 is `46df261a…1299c9`.

### Missing proof: macOS and every fresh-VM step

No Mac has mounted the DMG, so the Gatekeeper dialog text, `codesign --verify
--deep --strict` on the mounted app, first launch, and removal are unrecorded,
as are all the Linux and Windows fresh-VM steps in the status table.

## v1.2.7 release checks

Measured 2026-10-01. Every fresh-VM step in the status table is **Missing proof** for v1.2.7, and so is every macOS step.

### Measured: blob hashes (2026-10-01)

Each blob is available from
`https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/<hash>`:

```text
57533adabf28889641ae8b383bce060b95476a2ff209fa8623f72da77670c5f2    111213600  ELF 64-bit LSB pie executable, x86-64                forkmesh-linux-x86_64
f5caed0e686e0ef66cb429c88f7702c5f355a177757182cca90cef45ec860c8c     13159125  ELF 64-bit LSB executable, x86-64, Go, static        forkmesh-mirror-node-linux-x86_64
57cda708a4098a666f9b7b95b56b2d361790bcf40438249ad782ac27b950d901     80290024  PE32 executable (GUI), Nullsoft Installer           forkmesh-windows-x86_64-setup.exe
6d6e3cb5fdfdc4c2978c4c5f37cc1b9fddfa22095d756d5e739c34efeb6ee7cf    173041477  zlib-compressed DMG holding an APFS volume          forkmesh-macos-arm64.dmg
```

Every hash and size equals its manifest entry. `forkmesh-macos-x86_64.dmg`
has the same `blob_sha256` and size as `forkmesh-macos-arm64.dmg`.

### Missing proof: release verification, Authenticode signature, and every fresh-VM step

The v1.2.7 release was cut 2026-10-01 and manifest verification, Authenticode signature,
macOS universality check, and all fresh-VM steps are still unrun. The harness
configuration in `.forkmesh/clean-machine-vm.yml` will prove them on the next
clean-machine Action dispatch.
