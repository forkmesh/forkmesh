# Clean-machine install evidence

The clean-machine harness tests whatever the signed `latest` manifest
publishes and stamps every step with the tag and `checksums_sha256` it
verified. Pin a release with `FORKMESH_EXPECTED_TAG` /
`FORKMESH_EXPECTED_CHECKSUMS_SHA256` (Actions variables `CLEAN_MACHINE_TAG` /
`CLEAN_MACHINE_CHECKSUMS`). Evidence classes follow the
[launch readiness packet](y-combinator-news-readiness.md). A step passes only
when a dated transcript line shows it.

Release measured on this page: tag `v1.5.0`, `checksums_sha256`
`61872afbc33560807e531bf9e650141d9424c5e2c9f417d72b24decd1167fb9f`
([checks](#v150-release-checks)). The earlier
[v1.2.7 checks](#v127-release-checks) are kept below for history.

## Status

| Acceptance step | Linux x86_64 | Windows x86_64 |
| --- | --- | --- |
| Manifest signature and `SHASUMS256.txt` binding | **Measured fact** | Same manifest |
| Blob hash of the downloaded asset | **Measured fact** | **Measured fact** |
| Authenticode signature | Not applicable | **Measured fact** (pinned signer) |
| SmartScreen dialog text | Not applicable | **Missing proof** |
| Installer run on a fresh machine | **Missing proof** | **Missing proof** |
| First launch and node identity creation | **Missing proof** | **Missing proof** |
| Provider CLI discovery | **Missing proof** | **Missing proof** |
| Continual cycle stops at the human decision card | **Missing proof** | **Missing proof** |
| **Stop work** pauses the loop | **Missing proof** | **Missing proof** |
| Uninstall (default keeps data) | **Missing proof** | **Missing proof** |
| `--purge` prompt text | **Missing proof** | No purge option |

### Why the steps do not run on a production host

`uninstall.sh` stops every process named `forkmesh` and `install.sh` stops
managed daemons, so `tools/clean_machine_transcript.sh run` refuses to start
when a ForkMesh process or ForkMesh data already exists. Fresh-machine steps
run only in a disposable VM.

## Harness

`tools/clean_machine_transcript.sh verify` clones `.forkmesh/releases/latest`,
takes the Ed25519 key from the live `install.sh`, verifies the manifest
signature, checks that `sha256(SHASUMS256.txt)` equals `checksums_sha256`, and
prints the asset hashes. The installer itself prints no verification lines on
success, so the harness records them before installing and compares the
installed binary with `blob_sha256` afterwards.

`run` executes, in order: machine header, release verification, provider CLIs,
`curl -fsSL https://forkmesh.com/install.sh | bash`, installed-binary hash,
node identity files, default `uninstall.sh` with a listing of kept data,
`--purge` answered `no`, `--purge` without a terminal, and `--purge --yes`.
Between install and uninstall it stops at operator checkpoints for first
launch, provider discovery, the decision card and **Stop work**; an empty
answer is logged as `NOT OBSERVED`. Use a throwaway account, because first
launch announces the node to the network.

The **Clean-machine VM evidence** Action (`.forkmesh/clean-machine-vm.yml`,
driver `tools/clean_machine_vm.py`) boots a disposable Debian VM under QEMU,
runs the harness inside it, and keeps per-step screenshots, a video, the
transcript, the serial log and `summary.json`. A host-side observer OCRs the
framebuffer at each step and matches phrases from
`tools/clean_machine_expectations.json`; the verdict is `observed`,
`not-observed` or `unchecked`, and only `observed` passes. GUI steps that need
a person stay `not-observed` in unattended runs.

Purge prompt, quoted from `www/public/uninstall.sh`:

```text
This removes ForkMesh and ALL of its data from this computer:
  • the forkmesh binary and installed source
  • settings, the node identity key (the account cannot be recovered)
  • every mirrored repository, and all chat history
  • the desktop launcher, icons, and login-autostart entry

Type DELETE to continue:
```

Without a terminal it prints `Refusing to erase ForkMesh data without
confirmation. Re-run with --yes (or set FORKMESH_ASSUME_YES=1): …`.

## Windows

The installer is Authenticode-signed with a self-issued certificate. Against
public roots verification fails ("self-signed certificate"); pinned to the
release certificate it passes, with a DigiCert RFC 3161 timestamp:

```text
$ openssl x509 -in signer.pem -noout -fingerprint -sha256
sha256 Fingerprint=06:E0:93:EA:D7:0B:BD:A1:0B:4C:B6:0A:AE:B2:2C:8C:52:AA:ED:07:6E:21:F4:05:6E:45:2D:38:D1:BE:45:73
$ osslsigncode verify -in setup.exe -CAfile signer.pem
Signature verification: ok
```

That fingerprint equals `DEFAULT_WINDOWS_SIGNER_SHA256` in
`tools/forkmesh-release-host.py`. SmartScreen therefore warns on first run.

The NSIS installer (`tools/package/forkmesh.nsi`) installs per user into
`%LOCALAPPDATA%\ForkMesh`; its uninstaller removes that directory, shortcuts,
the uninstall key and the `forkmesh:` URL handler, with no purge option. The
node identity key lives under Roaming `%APPDATA%\ForkMesh\ForkMesh` and
settings in the registry, so both should survive uninstall.

Fresh-VM procedure: `Get-FileHash` and `Get-AuthenticodeSignature` on the
setup executable, record the SmartScreen dialog text, install and launch,
list both data directories, run provider discovery, one continual cycle and
**Stop work**, uninstall, and list the directories again.

## macOS

The DMG is universal (`x86_64` and `arm64`), ad-hoc signed and not notarized,
so Gatekeeper blocks first launch until the user opens it with right-click →
**Open**. A fresh-Mac transcript must record the Gatekeeper dialog,
`codesign --verify --deep --strict`, first launch and removal.

## v1.2.7 release checks

Each blob is served from
`https://forkmesh.com/api/repo/forkmesh/forkmesh/releases/blob/sha256/<hash>`;
every hash and size below equals its manifest entry.

```text
57533adabf28889641ae8b383bce060b95476a2ff209fa8623f72da77670c5f2    111213600  ELF 64-bit LSB pie executable, x86-64          forkmesh-linux-x86_64
f5caed0e686e0ef66cb429c88f7702c5f355a177757182cca90cef45ec860c8c     13159125  ELF 64-bit LSB executable, x86-64, Go, static  forkmesh-mirror-node-linux-x86_64
57cda708a4098a666f9b7b95b56b2d361790bcf40438249ad782ac27b950d901     80290024  PE32 executable (GUI), Nullsoft Installer     forkmesh-windows-x86_64-setup.exe
6d6e3cb5fdfdc4c2978c4c5f37cc1b9fddfa22095d756d5e739c34efeb6ee7cf    173041477  zlib-compressed DMG holding an APFS volume    forkmesh-macos-arm64.dmg
```

`forkmesh-macos-x86_64.dmg` has the same `blob_sha256` and size as
`forkmesh-macos-arm64.dmg`. Every fresh-VM step in the status table is still
**Missing proof** for this release.

## v1.5.0 release checks

Measured 2026-10-31 from the same blob route. `release.json`,
`release.json.sig` and `SHASUMS256.txt` served at
`https://forkmesh.com/api/repo/forkmesh/forkmesh/raw?path=.forkmesh/releases/latest/…&ref=main`
(the `/desktop` page's manifest URL) are byte-identical to the repository
copies; the signature verifies with the key in `install.sh`, and
`sha256(SHASUMS256.txt)` equals `checksums_sha256`. Every hash and size below
equals its manifest entry.

```text
9ca90fe0dc5f26f97abaa3fd26b4d898a652fe3868ab32e53e5393fc99358ebf    142611552  ELF 64-bit LSB pie executable, x86-64          forkmesh-linux-x86_64
c4da0b5801fb0ca75deae158c61ac12b386ef7e9eb61ca87c5394a143f9c6378     14076218  ELF 64-bit LSB executable, x86-64, static     forkmesh-mirror-node-linux-x86_64
7889aad7fd55d490d603ffe75603b50a0da1099fed7bf218b14ec317e29efbd6     96940752  PE32 executable (GUI)                         forkmesh-windows-x86_64-setup.exe
8991bcc9e47c807b1ef5fd0d16ab2c643b050013ba3dc90736ed5c448d2f6040    214746483  zlib-compressed DMG                           forkmesh-macos-arm64.dmg
```

The setup executable's Authenticode signer is `CN=ForkMesh, O=ForkMesh`, SHA-256
fingerprint `06:E0:93:EA:D7:0B:BD:A1:0B:4C:B6:0A:AE:B2:2C:8C:52:AA:ED:07:6E:21:F4:05:6E:45:2D:38:D1:BE:45:73`
(the pinned `DEFAULT_WINDOWS_SIGNER_SHA256`), timestamped by DigiCert; Windows
reports the chain as ending in an untrusted root, as expected for the
self-issued certificate. The two macOS assets again share one digest.
Every fresh-VM step in the status table is still **Missing proof** for this
release.
