# Transports and anonymity postures

`tools/forkmesh_transports.py` decides how a device reaches repositories and
what it may reveal. A profile lists the device's adapters and one posture; the
tool plans a route for a URL, runs Git over the first permitted route that
answers (never falling back to a forbidden one), traces the Git process for
leaks in private postures, probes routes, and chooses which local addresses may
be advertised.

Formats: profile `forkmesh-transport-profile-v1`, plan
`forkmesh-transport-plan-v1`, health report `forkmesh-transport-health-v1`,
advertisement `forkmesh-transport-advertisement-v1`, telemetry
`forkmesh-transport-telemetry-v1`. Planner vectors:
`conformance/transports/v1/cases.json`.

```json
{"schema": "forkmesh-transport-profile-v1", "posture": "anonymous",
 "adapters": [{"id": "tor", "kind": "proxy", "network": "tor", "socks": "127.0.0.1:9050"}]}
```

## Adapters

| Kind | Carries | Name resolution |
| --- | --- | --- |
| `clearnet` | Direct internet connections | System resolver, `standard` posture only |
| `proxy` | SOCKS5; `network` is `tor`, `i2p` or `socks` | By the proxy (`socks5h`); proxy endpoint must be a literal `IP:port` |
| `overlay` | Addresses inside its `cidrs` (WireGuard, Yggdrasil and similar) | None; literal addresses or pinned `hosts` |
| `lan` | Loopback, private, link-local, unique-local | None, except multicast DNS for `.local` |

Adapter fields: `allowPlaintext`, `authenticatedNetwork` (overlay),
`isolateStreams` and `serviceAddress` (proxy), `budgets.latencyMs` (default
2000).

Profile errors, checked in order: `profile_schema`, `unknown_posture`,
`adapters_missing`, `adapter_id_invalid`, `adapter_id_duplicate`,
`unknown_adapter_kind`, `proxy_endpoint_not_literal`, `proxy_network_unknown`,
`overlay_cidr_invalid`, `overlay_cidr_global`, `pinned_host_invalid`,
`budget_invalid`, `posture_has_no_adapter`,
`anonymous_requires_anonymity_network`.

## Postures

| Posture | Adapters | Lookups | Direct fallback | Telemetry | Advertises |
| --- | --- | --- | --- | --- | --- |
| `standard` | all (`lan`, `overlay`, `clearnet`, `proxy`) | system | yes | counts | public, overlay, LAN |
| `private` | `proxy` | by proxy | never | counts | service address only |
| `anonymous` | `proxy` on `tor` or `i2p` | by proxy | never | off | service address only |
| `overlay-only` | `overlay` | none | never | counts | overlay |
| `lan-only` | `lan` | none | never | counts | LAN |

Outside `standard`, Git runs with a scrubbed environment, no system or global
config, no credential helpers or redirects, and redacted logs; `git://` and
`ssh://` go through the tool's relay. A repository config that could reroute
traffic (`insteadOf`, proxies, `core.sshCommand`, includes) is refused with
`repository_config_overrides_route`.

## Route refusals

Per adapter, first match: `adapter_not_permitted`,
`proxy_network_not_permitted`, `destination_requires_anonymity_network`,
`onion_requires_tor`, `i2p_requires_i2p`, `local_destination_via_proxy`,
`destination_outside_overlay`, `destination_outside_lan`, `name_needs_dns`,
`plaintext_via_exit`, `peer_unauthenticated`. Plan-level: `invalid_url`,
`scheme_unsupported` (only `https`, `http`, `ssh`, `git`), `no_permitted_route`.

## States

| State | Label | When |
| --- | --- | --- |
| `exposed` | Exposed | The trace saw a `dns_query` or `direct_connection` outside the route; wins over all others |
| `unreachable` | Unreachable | No permitted route answered, or the transfer broke |
| `degraded` | Degraded | Reachable with a reason: `failed_over`, `slow`, `intermittent`, `unauthenticated_peer` |
| `anonymous` | Anonymous | `anonymous` posture, healthy, no leak |
| `protected` | Protected | `private`, `overlay-only` or `lan-only`, healthy, no leak |
| `direct` | Direct | `standard` posture, healthy |

Clients display the report's `label`, `tone` and `explanation` verbatim. Leak
tracing uses `strace` on Linux; elsewhere `leakTrace` is `unavailable` and only
the in-process guard applies.

## Commands

```sh
tools/forkmesh_transports.py validate PROFILE
tools/forkmesh_transports.py plan --profile PROFILE URL
tools/forkmesh_transports.py probe --profile PROFILE URL [--report FILE]
tools/forkmesh_transports.py git --profile PROFILE URL [--repo DIR] [--trace auto|required|off] -- clone @URL app
tools/forkmesh_transports.py status REPORT
tools/forkmesh_transports.py advertise --profile PROFILE --address IP...
```

Use `fetch --atomic` so a broken route leaves refs unchanged. Git's own
failures do not trigger failover.

| Exit code | Meaning |
| --- | --- |
| `0` | Done. |
| `2` | Usage error, invalid profile, or required tracer missing. |
| `3` | Refused by policy before anything was sent. |
| `4` | Unreachable. |
| `5` | Exposed. |
| `6` | Git failed on a reachable route. |

Tests: `app/tests/test_transports.py` (quality gate `transport-conformance`).
Only Git traffic honours the profile so far.
