# Self-hostable service roles

ForkMesh's network services are split into eight roles. This page lists which
roles an independent operator can run today, how to start each one, what
replaces each optional dependency, and how to check a running deployment. It
matches the machine-readable manifest
[`conformance/service-roles/v1/manifest.json`](../../../conformance/service-roles/v1/manifest.json).
`tools/forkmesh_service_roles.py validate` fails if this page or the manifest
names a path, flag, source route, or heading that no longer exists.

Tracking issue: #502.

## Role status

| Role | Status | Independent profile | Contract checks |
| --- | --- | --- | --- |
| `web-ui` | partial | Mirror gateway dashboard | `/health`, `/api/version`, `/dashboard`, `/api/accounts/sessions` |
| `relay` | partial | Mirror gateway Smart HTTP + `forkmesh-mirror-node` | clone advertisement, signed endpoint manifest |
| `public-index` | partial | Mirror gateway local catalog | `/api/repositories` |
| `search` | hosted-only | — | `/api/repositories/search` (hosted) |
| `identity-resolver` | partial | Node key, SSH broker `local` mode, portable sessions | evidence tests only |
| `collaboration-relay` | partial | Daemon intake, gateway submissions, SSH pull spool | evidence tests only |
| `automation-broker` | partial | Desktop ActionRunner + signed checks | evidence tests only |
| `organization-service` | hosted-only | — | none yet |

- **independent**: runs from this repository with open-source dependencies
  only, needs no default endpoint, and lists no gaps. No role has this status
  yet.
- **partial**: an independent profile covers part of the role. The manifest's
  `gaps` list says what still needs the hosted deployment.
- **hosted-only**: runs only on the hosted Worker today.

Print the current list with:

```sh
python3 tools/forkmesh_service_roles.py gaps
```

## Starting the independent profiles

Every partial role runs on the same two open-source processes:

1. `tools/mirror_gateway.py --config gateway.json` serves Git, the dashboard,
   the local catalog, and the signed endpoint manifest on a loopback port.
   Run `--check` first. Its configuration schema is
   [`mirror-gateway-config.schema.json`](../mirror-gateway-config.schema.json).
2. `forkmesh-mirror-node --config mirror-node.json` synchronizes repositories,
   supervises the gateway, commits collaboration submissions, and reports
   `/healthz`, `/readyz`, and `/v1/status`. Run `--preflight --json` first. See
   [Mirror node service](mirror-node.md).

For SSH pushes, pull submissions, and forks with no controller, add the SSH
gateway and the authorization broker in `localAuthorization` mode `local`
([Controller-free operation](git-ssh-gateway.md#controller-free-operation)).
Put any TLS-terminating reverse proxy in front of the loopback listeners.

## Optional dependencies and substitutes

| Dependency | Used by | Substitute |
| --- | --- | --- |
| Cloudflare Tunnel (`cloudflared`) | `web-ui`, `relay` | `--disable-cloudflared` (or `disableCloudflared`) with your own HTTPS reverse proxy |
| Hosted login origin | `web-ui` | Omit `dashboard.canonicalOrigin`; the dashboard stays signed out except for existing portable sessions |
| Hosted node event socket | `relay` | The `post-receive` doorbell (`POST /v1/control/sync`), direct peer upstreams, and `syncInterval` for third-party upstreams |
| Hosted catalog publication | `public-index` | `disableCatalog`, or `--catalog-url` pointing at another operator's catalog |
| Hosted account registry | `identity-resolver` | SSH broker `localAuthorization` mode `local` with a grants file |
| Hosted collaboration inboxes | `collaboration-relay` | `forkmesh-pull-submit` over SSH, or direct signed submissions to the gateway |
| Hosted agent CLIs | `automation-broker` | Leave website agent jobs off; workflows and signed required checks run without them |

## Default endpoints

`forkmesh-mirror-node` has three flags whose defaults point at forkmesh.com.
An independent deployment overrides all three. The contract test fails if a
new forkmesh.com default appears without a matching manifest entry.

| Flag | Default | Role |
| --- | --- | --- |
| `--upstream` | `https://forkmesh.com/forkmesh/forkmesh` | `relay` |
| `--catalog-url` | `https://app.forkmesh.com/api/repositories` | `public-index` |
| `--accounts-url` | `https://app.forkmesh.com/api/accounts` | `identity-resolver` |

## Checking a running role

```sh
python3 tools/forkmesh_service_roles.py probe --role web-ui \
  --base-url https://mirror.example.org
python3 tools/forkmesh_service_roles.py probe --role relay \
  --base-url https://mirror.example.org --param owner=alice --param repository=demo
```

`probe` sends only read-only `GET` or `HEAD` requests. It prints a
`forkmesh-service-role-report-v1` JSON report with one result per check, the
role's status, `defaultPublicService` (true when the base URL is a
forkmesh.com host), and a `telemetry` block with request, transport error,
status class, and latency counts. The base URL is printed without credentials
or a query string.

| Verdict | Exit code | Meaning |
| --- | --- | --- |
| `pass` | 0 | Every check answered as specified. |
| `degraded` | 3 | Every check that did not pass returned the role's declared explicit degradation, such as `503` with a JSON `error`. |
| `fail` | 1 | A check answered outside both the contract and the declared degradation. |
| `unreachable` | 4 | Nothing answered. Treat this as service loss, never as an empty result. |

A usage or manifest error exits 2.

## Failure behavior and canonical data

Git objects, refs, and signed collaboration records under `.forkmesh/` stay in
repositories on configured nodes. Catalog rows, search results, and dashboard
pages are rebuilt from them. Losing a role never deletes canonical data:

- Without the hosted Worker, the gateway keeps serving clones and the last
  validated dashboard projection. The stable forkmesh.com URL cannot redirect
  to it.
- Submissions made while the Worker is down stay in the pull submission
  spool. Nothing is acknowledged until it is committed.
- In `fallback` mode the SSH broker uses local grants only when the Worker is
  unreachable, never after a denial, so cutting the uplink cannot bypass a
  revocation.
- Search answers `503 catalog_unavailable` when no catalog can be read.
- A required status check with no signed result keeps the merge gate closed.

Backups for every node-hosted role are covered by
[Node backup and recovery](node-backup-and-recovery.md).

## What is not proven yet

Issue #502 stays open until:

- `search` and `organization-service` have independent profiles;
- account registration, username resolution, stable-URL routing, live rooms,
  and the agent job queue run without the hosted Worker;
- workflows run in a headless service instead of only inside the desktop app;
- a clean-room deployment on fresh infrastructure serves browse, clone,
  collaboration, identity, and automation without forkmesh.com; and
- mixed-version and service-loss runs are recorded against real deployments.

`app/tests/test_service_roles_contract.py` starts a real mirror gateway with
no forkmesh.com configuration, runs the `web-ui`, `relay`, and `public-index`
contracts against it, and then stops it to check that the probe reports
`unreachable`. CI runs it as the `service-role-contract` quality gate.

## Changing the manifest

Change a role's status only in the same commit as the code and evidence that
justify it. A role can move to `independent` only when its gaps list is empty,
it has no proprietary required dependency, and it has a contract. The
validator enforces all three.
