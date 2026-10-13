# Installation support matrix

This page lists which ForkMesh builds you can install, how they are signed and
verified, and what happens to your data when you upgrade, roll back, or
uninstall. The machine-readable version is
[`install-support-matrix.json`](install-support-matrix.json). CI checks that
every **supported** artifact is published in the latest signed release and that
each evidence test exists.

## Support tiers

| Tier | Meaning |
| --- | --- |
| Supported | A prebuilt artifact is on the latest channel, bound into the Ed25519-signed release manifest, SHA-256 verified by the installer, and covered by automated tests. Regressions block a release. |
| Preview | Packaging exists, but the latest channel does not publish the artifact or clean-machine evidence is missing. Fixes are best-effort. |
| Source-only | No prebuilt artifact. Build from source with `FORKMESH_FROM_SOURCE=1`. |
| Planned | Not installable yet. Tracked under issue #500. |

## Profiles

| Profile | Operator | Tier | Artifact | Signing |
| --- | --- | --- | --- | --- |
| `linux-desktop-x86_64` | Desktop | Supported | `forkmesh-linux-x86_64`, `.AppImage` | Signed manifest + SHA-256 |
| `linux-cli-headless-x86_64` | CLI / headless | Supported | `forkmesh-linux-x86_64` | Signed manifest + SHA-256 |
| `linux-server-mirror-node-x86_64` | Server | Supported | `forkmesh-mirror-node-linux-x86_64` | Signed manifest + SHA-256; systemd unit records the digest |
| `windows-desktop-x86_64` | Desktop | Supported | `forkmesh-windows-x86_64-setup.exe` | Authenticode (self-issued certificate, SmartScreen warns) + signed manifest |
| `android-universal` | Small device | Supported | `forkmesh-android-universal.apk` | APK v2 release key + signed manifest |
| `macos-desktop-arm64` | Desktop | Preview | `forkmesh-macos-arm64.dmg` | Not notarized |
| `macos-desktop-x86_64` | Desktop | Preview | `forkmesh-macos-x86_64.dmg` | Not notarized |
| `linux-desktop-arm64` | Home server | Preview | `forkmesh-linux-arm64.AppImage` | Signed manifest once published |
| `linux-arm-small-device` | Small device | Source-only | — | Source from a signed-refs-pinned mirror |
| `ios` | Small device | Preview | App Store / TestFlight | Apple distribution signing |
| `oci-container` | Container | Planned | — | Planned: signed image digest + provenance |
| `cluster` | Cluster | Planned | — | Planned: builds on the OCI image |

On a supported Linux machine, the installer detects your OS and architecture
and installs the matching artifact. If no prebuilt artifact exists, it stops
with an error instead of compiling on your machine.

## Lifecycle

Only the release on the `latest` channel is supported. Earlier releases stay
installable from their `candidate-<version>` channel.

| Action | Command |
| --- | --- |
| Install or upgrade | `curl -fsSL https://forkmesh.com/install.sh \| bash` |
| Roll back | `curl -fsSL https://forkmesh.com/install.sh \| FORKMESH_RELEASE=candidate-<version> bash` |
| Uninstall, keeping data | `curl -fsSL https://forkmesh.com/uninstall.sh \| bash` |
| Uninstall and erase all data | `curl -fsSL https://forkmesh.com/uninstall.sh \| bash -s -- --purge --yes` |

Every install checks the release manifest's signature, confirms that it binds
`SHASUMS256.txt`, and verifies the artifact's SHA-256 before it replaces the
installed binary. If any check fails, the existing binary stays in place.

## Your data is preserved

Upgrade, rollback, and uninstall only replace or remove program files. Your
settings, node identity key, repositories, and chat history stay in place until
you explicitly erase them:

| Kept (user data) | Removed (program files) |
| --- | --- |
| `~/projects/forkmesh/{data,config,ssh,state,logs}` — identity key, repositories, chat, settings, SSH keys, diagnostics (`logs/`) | `~/.local/bin/forkmesh`; desktop launcher, icons, autostart entry, managed systemd unit and binary |
| `/var/lib/forkmesh` — mirror node identity and repositories | `~/projects/forkmesh/src` — installer checkout, and `~/projects/forkmesh/cache` |
| Legacy locations, used only by installs not yet moved to the projects layout: `~/.config/ForkMesh` (settings), `~/.local/share/ForkMesh` (identity key, repositories, chat), `~/.forkmesh` (state, diagnostics) | Legacy program files: `~/.local/share/forkmesh` (installer checkout), `~/.cache/ForkMesh` |

Since 2026-09-21 installs use the projects layout: everything lives under
`~/projects/forkmesh`, with the checkout in `src/` (`desktop/src/AppPaths.cpp`,
`tools/forkmesh_paths.py`). An older install keeps working from the legacy
paths; the desktop moves its data into the layout at its next start and
leaves links behind. The project folder itself is never removed, because it may hold files of your own.

Only two actions delete user data:

- `--purge`, given to `uninstall.sh` or `install.sh --uninstall`. It asks you
  to type `DELETE` unless you also pass `--yes`.
- A confirmed **Uninstall** or **Reinstall** from the desktop app's Hosts panel.

The identity key cannot be recovered after a purge. Export or back it up first
(see [Portable identities and recovery](portable-identities.md)).
