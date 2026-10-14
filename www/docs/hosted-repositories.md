# Empty repositories created from the web

Create an empty repository from **Repositories → New** and push with plain
`git` to `https://app.forkmesh.com/<owner>/<name>`, using your account name and
the push credential shown. The credential is a keyed MAC over account,
repository, session and a seven-day expiry; it is not stored, dies with the
session, and every push re-checks permissions. After each push ForkMesh records
the attestation (the signed list of branch and tag commits) that reads route
against.

Packs are bounded by the node's `receiveMaxInputBytes`. Where core nodes host
repositories (`HOSTED_GIT_CORE_NODES`) the form also offers private
repositories, initial files and imports, up to 10 repositories of 200 MB per
account.

## Operator notes

Requires `HOSTED_GIT_WRITE_NODE` and a node with `writableRepositoryRoot` and
`"writableOwners": ["*"]`; otherwise the API answers `hosted_git_unavailable`.
See [direct-https-mirror-gateway.md](operations/direct-https-mirror-gateway.md).
Endpoints are `POST /api/hosted-repositories` and
`POST /api/hosted-repositories/push-credential`; records are marked
`source: "hosted-web"` until a signed publication from the owner's node takes
over.
