# apt repository

Debian and Ubuntu users install and update the desktop app with apt:

```sh
sudo curl -fsSLo /usr/share/keyrings/forkmesh-archive-keyring.asc https://forkmesh.com/apt/forkmesh-archive-keyring.asc
sudo curl -fsSLo /etc/apt/sources.list.d/forkmesh.sources https://forkmesh.com/apt/forkmesh.sources
sudo apt update
sudo apt install forkmesh
```

After that, `sudo apt update && sudo apt upgrade` installs each new release.
The package runs on the same systems as the Linux binary: Ubuntu 22.04 and
newer, Debian 12 and newer, x86_64.

## Code map

| Path | Responsibility |
| --- | --- |
| `tools/package/deb.sh` | builds `forkmesh-linux-x86_64.deb` from the release binary and its CMake-installed resources |
| `tools/package/linux-baseline/build-forkmesh.sh` | runs `deb.sh` in the Debian 12 container after the AppImage |
| `tools/package/linux-baseline/smoke-test.sh` | `dpkg --dry-run -i` and the packaged binary's `--version` on Ubuntu 22.04, 24.04 and Debian 12 |
| `tools/package/apt/forkmesh.sources` | the apt source the package installs and the setup command downloads |
| `tools/package/apt/forkmesh-archive-keyring.asc` | the public apt signing key |
| `tools/forkmesh-apt-repo.py` | writes and signs `dists/`, verifies it; `init-key` creates the signing key |
| `tools/forkmesh-release-host.py` (`refresh_apt_repository`) | republishes the apt metadata each time `latest` is promoted |
| `.forkmesh/releases/apt/` | the committed, signed apt metadata |
| `app/src/releases.py` (`apt_redirect_target`), `app/src/app_runtime.py` | the `/apt/*` routes |

## Package

`deb.sh` installs `/usr/bin/forkmesh` (and `forkmesh-term` when the build
has it), the resource tree in `/usr/share/forkmesh` (the binary finds it at
`../share/forkmesh`, as in the AppImage), a launcher, the 512 px icon, the
public key in `/usr/share/keyrings/forkmesh-archive-keyring.asc` and the
source in `/etc/apt/sources.list.d/forkmesh.sources`. The source is a
conffile. A user who ran the setup commands already has byte-identical
copies of both files at those paths, so dpkg takes them over without a
prompt, and someone who installed the `.deb` by hand still gets updates.

`Depends` is worked out in the container: each `NEEDED` library of the binary
is mapped to its Debian 12 package with `dpkg -S`, and `libc6` gets the
newest `GLIBC_` symbol version the binary uses. The build fails if a library
maps to no package or to several. These package names are the same on
Ubuntu 22.04 and 24.04, which the smoke test checks.

The package version is the release version, with `+<build>` added for a
rebuilt platform build (`--deploy-build linux`, [README.md](README.md#build-numbers)).
Build 0 of 1.5.1 is `1.5.1`, build 2 is `1.5.1+2`, and dpkg orders
`1.5.1 < 1.5.1+2 < 1.5.2`. A rebuild with the same version would never reach
apt users, which is why the build number goes into the version.

The `.deb` is published into the candidate with the other Linux assets, but
it is not in `HOST_ASSETS` or the gate's required list: a missing `.deb`
never holds a release back. Its manifest entry is `os: linux,
arch: x86_64`, like the AppImage; the desktop updater takes the first
matching asset, and the manifest is sorted by name, so the bare binary still
comes first.

## Repository

The repository has one suite, `stable`, one component, `main`, and only the
newest package. Its metadata is committed under `.forkmesh/releases/apt/`:

```
dists/stable/Release
dists/stable/InRelease
dists/stable/Release.gpg
dists/stable/main/binary-amd64/Packages
forkmesh-archive-keyring.asc
forkmesh.sources
```

A package's `Filename` is `pool/sha256/<sha256>/forkmesh_<version>_<arch>.deb`.
The `.deb` bytes are never in git; they live in the release store like every
other asset.

The app answers `/apt/*` on `app.forkmesh.com` with a 302:

- `/apt/pool/sha256/<sha256>/<name>.deb` goes to
  `/api/repo/forkmesh/forkmesh/releases/blob/sha256/<sha256>`, so downloads
  are counted and served from the mirrors as usual;
- the six metadata files go to
  `/api/repo/forkmesh/forkmesh/raw?path=.forkmesh/releases/apt/<file>&ref=main`;
- anything else is a 404. Instance-branded nodes do not answer `/apt`.

`apt` is in `CORE_RESERVED_ROUTE_PREFIXES`, so no account or organization can
take a name the route would hide. `forkmesh.com/apt/*` reaches the same routes
through the www Worker, which 308s two-segment paths to the app; the source
file names `app.forkmesh.com` directly so apt takes one redirect less.

apt verifies `InRelease` against the key named in `Signed-By`, and each file
against the hashes in `Release`, so the redirects and mirrors are not trusted.
Each file is read from `main` separately, so in the minute after a promotion
one request can reach a mirror that already has the new commit and the next
one a mirror that does not; apt then reports a hash mismatch for `Packages`,
and the next `apt update` succeeds.

## Signing key

The key is an OpenPGP Ed25519 signing key, separate from the Ed25519 release
key that signs `release.json`: apt reads only OpenPGP signatures. It was
created on the release host on 2026-10-31 with

```sh
python3 tools/forkmesh-apt-repo.py init-key \
  --signing-key ~/projects/forkmesh/data/core/release/apt-signing-key.asc
```

which refuses to replace an existing key and writes the public half to
`tools/package/apt/forkmesh-archive-keyring.asc`. The secret key is
`data/core/release/apt-signing-key.asc` (0600), next to the Android keystore
and Authenticode certificate; `FORKMESH_APT_SIGNING_KEY` overrides the path.
It must be backed up with them. If it is lost, every installed system fails
`apt update` until its owner downloads a new keyring.

`forkmesh-apt-repo.py publish` refuses to sign with a key that is not the
committed public key, signs with SHA-512 (the Sequoia `sqv` verifier in
Debian 13 and newer apt rejects SHA-1), and verifies the result with `gpgv`
before it returns.

## Release host

After the gate promotes a candidate and the promoted manifest verifies,
`publish_and_promote` calls `refresh_apt_repository`. When `latest` carries
`forkmesh-linux-x86_64.deb`, it reads the `.deb` from the local release store
by hash, runs `forkmesh-apt-repo.py publish` into the promotion worktree and
adds `.forkmesh/releases/apt` to the same metadata commit as `latest`, so apt
and the installer switch together when that commit reaches `main`. The blob
is seeded to the mirrors with the other assets before the merge.

- No `.deb` in `latest`: nothing changes.
- No signing key: logged, and the repository keeps its previous release.
- The `.deb` missing from the store, or signing or verification failing:
  the step fails and nothing is promoted.

The repository starts with the first release built after this change. Until
then `dists/` does not exist and `apt update` reports a 404 for the source.

## Tested

On 2026-10-31 the v1.5.0 binary and resources were packaged with `deb.sh`
inside the Debian 12 builder, published with a throwaway key, and served by a
local copy of the `/apt` redirect routes. On Ubuntu 22.04, Ubuntu 24.04 and
Debian 12, `apt install forkmesh` installed it with the setup files already
in place (no conffile prompt) and `apt upgrade` moved it from `1.5.0+2` to
`1.5.0+3`. Debian 13's apt 3.0 (`sqv`) verified the same repository.
`app/tests/test_apt_repository.py` covers the package, the metadata and the
signatures.
