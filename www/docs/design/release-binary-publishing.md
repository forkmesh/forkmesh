# Release binary publishing

Issue #304. Release binaries are never committed to git. A release is two
things kept apart on purpose:

- **Metadata**: small, signed text committed to the repository and synced
  across the mesh like issues and pull requests.
- **Payloads**: the binary bytes, kept in a content-addressed store (CAS) on
  the node that serves the repository, and fetched by their SHA-256.

A mirror can carry the bytes, but it cannot change them without breaking the
signed hash, and git history never grows by the size of a build.

## Two kinds of release

**Repository releases.** Any repository can publish releases from the web or
the desktop. The manifest's canonical body is built by
`release_manifest_content` in `app/src/releases.py`: repository, tag, the
commit the tag pointed to (`tag_commit`), the commit embedded in the artifact
(`build_commit`, when present), and one line per asset (name, blob SHA-256,
size, content type, OS, architecture, label), sorted. The creator signs
`release_signing_message`:

```text
forkmesh-release-v1\n<repo>\n<tag>\n<author>\n<ts>\n<sha256 of the content>
```

with their Ed25519 identity; `verify_release_manifest` in `app/src/events.py`
checks it. Release name, notes and the prerelease flag are not signed, so they
can be edited without invalidating the release. Download counts are not
signed either.

**ForkMesh's own desktop releases.** Channels under
`.forkmesh/releases/<channel>/` (`latest`, `candidate-<version>`) hold
`SHASUMS256.txt`, `release.json` and `release.json.sig`: a raw 64-byte Ed25519
signature over the exact `release.json` bytes, made with the release publisher
key (not a user identity). `tools/forkmesh-release-publish.sh` writes them;
`www/public/install.sh` and the desktop updater verify them. Operating
instructions are in
[`.forkmesh/releases/README.md`](../../../.forkmesh/releases/README.md) and the
developer overview in [`docs/release/README.md`](../../../docs/release/README.md).

## The content-addressed store

Blob path, relative to the store root (`cas_blob_relpath` in
`app/src/releases.py`):

```text
sha256/<first two hex digits>/<full sha256>/data
```

The served store is `<bare repository>/forkmesh-releases/` on each mirror
(`server/bootstrap.go`, `server/gateway.go`). A local publish without
`--cas-dir` writes to `.forkmesh/release-blobs/`, which `.gitignore` excludes.

## Download

```text
GET /api/repo/<owner>/<repo>/releases/blob/sha256/<sha256>?filename=<asset name>
```

The route is matched in `app/src/urls.py`. The response is immutable and can
be cached forever at the edge; the App forwards it to a node holding the blob.
`filename` only sets the download name and never changes which bytes are
returned. Installers ask the repository recorded in the manifest (`repo`),
because only that node is guaranteed to hold the blob.

## Verification order

1. Authenticate the metadata: the release signature against a trust anchor
   (the publisher key built into `install.sh`, a local PEM, or a digest from
   an authenticated controller).
2. Check that the manifest binds `SHASUMS256.txt` (`checksums_sha256`).
3. Download the blob by hash and check its SHA-256 before installing or
   running it.

Nothing downloaded is executed to decide whether it is trustworthy.

## Related

- `app/src/releases.py`: tag and asset-name validation, CAS layout, canonical
  manifest, `SHASUMS256.txt` generation, semver ordering for `latest`, and the
  re-upload decision for an asset name.
- `app/tests/test_releases.py`
