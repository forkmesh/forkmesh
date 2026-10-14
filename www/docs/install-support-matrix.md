# Installation support matrix

The machine-readable matrix is
[`install-support-matrix.json`](install-support-matrix.json); CI checks that
every supported artifact is in the latest signed release.

**Supported** artifacts are in the Ed25519-signed release manifest, SHA-256
verified and tested; **Preview** is packaged but not fully published;
**Source-only** builds with `FORKMESH_FROM_SOURCE=1`; **Planned** is not
installable.

## Profiles

| Profile | Tier | Artifact |
| --- | --- | --- |
| `linux-desktop-x86_64` | Supported | `forkmesh-linux-x86_64`, `.AppImage` |
| `linux-cli-headless-x86_64` | Supported | `forkmesh-linux-x86_64` |
| `linux-server-mirror-node-x86_64` | Supported | `forkmesh-mirror-node-linux-x86_64` |
| `windows-desktop-x86_64` | Supported | `forkmesh-windows-x86_64-setup.exe` |
| `android-universal` | Supported | `forkmesh-android-universal.apk` |
| `macos-desktop-arm64` | Preview | `forkmesh-macos-arm64.dmg` |
| `macos-desktop-x86_64` | Preview | `forkmesh-macos-x86_64.dmg` |
| `linux-desktop-arm64` | Preview | `forkmesh-linux-arm64.AppImage` |
| `linux-arm-small-device` | Source-only | none |
| `ios` | Preview | App Store / TestFlight |
| `oci-container` | Planned | none |
| `cluster` | Planned | none |

## Lifecycle

| Action | Command |
| --- | --- |
| Install or upgrade | `curl -fsSL https://forkmesh.com/install.sh \| bash` |
| Roll back | `curl -fsSL https://forkmesh.com/install.sh \| FORKMESH_RELEASE=candidate-<version> bash` |
| Uninstall, keeping data | `curl -fsSL https://forkmesh.com/uninstall.sh \| bash` |
| Uninstall and erase all data | `curl -fsSL https://forkmesh.com/uninstall.sh \| bash -s -- --purge --yes` |

Each install verifies the manifest signature, its binding of `SHASUMS256.txt`
and the artifact digest before replacing the binary. Upgrade, rollback and
uninstall touch only program files; user data in
`~/projects/forkmesh/{data,config,ssh,state,logs}` (and `/var/lib/forkmesh`
for a mirror node) is removed only by `--purge` or a confirmed desktop
uninstall. Back up the identity key first: see
[Portable identities and recovery](portable-identities.md).
