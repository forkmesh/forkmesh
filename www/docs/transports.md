# Transports and anonymity postures

Use this guide to choose how a device reaches repositories and how much it may
reveal on the way. A transport can be the open internet, Tor or I2P, an overlay
network such as a WireGuard mesh, or only the local network. You might need
this because your address must stay private, because a network blocks direct
access, or because a team shares code only inside a building.

`tools/forkmesh_transports.py` does five things:

1. **Plans** a route for a repository URL from a profile. The profile lists the
   adapters this device has and one posture that decides which adapters may
   be used.
2. **Runs Git** over the first permitted route that answers. If that route
   fails, it moves to the next permitted one and never to a route the posture
   forbids.
3. **Traces** the Git process in the private postures. Any DNS query or
   connection outside the route marks the run `exposed`.
4. **Probes** every permitted route and reports one state: reachable,
   degraded, unreachable, anonymous, or exposed.
5. **Chooses which of this device's addresses** a posture lets peers learn, and
   says whether it can accept inbound connections behind NAT.

The profile is `forkmesh-transport-profile-v1`. A plan is
`forkmesh-transport-plan-v1`, a health report is
`forkmesh-transport-health-v1`, an advertisement is
`forkmesh-transport-advertisement-v1`, and a telemetry line is
`forkmesh-transport-telemetry-v1`. Route-planning vectors live in
`conformance/transports/v1/cases.json`; another implementation must produce
the same routes and the same codes in the same order.

## Start here: clone over Tor without leaking your address

Run Tor (its SOCKS port is `127.0.0.1:9050` by default) and write a profile:

```json
{
  "schema": "forkmesh-transport-profile-v1",
  "posture": "anonymous",
  "adapters": [
    {"id": "tor", "kind": "proxy", "network": "tor", "socks": "127.0.0.1:9050"}
  ]
}
```

Then check it, see the route, and clone:

```sh
tools/forkmesh_transports.py validate tor.json
tools/forkmesh_transports.py plan --profile tor.json https://git.example.org/team/app.git
tools/forkmesh_transports.py git --profile tor.json https://git.example.org/team/app.git \
  --trace required --report health.json -- clone @URL app
tools/forkmesh_transports.py status health.json
```

`--trace required` stops before sending anything if the tracer is missing.
Leave it at `auto` on systems where tracing is not available (see
[Supported platforms](#supported-platforms)).

## Adapters

| Kind | What it carries | How names are resolved |
| --- | --- | --- |
| `clearnet` | Direct connections over the open internet. | The operating system resolver, and only in the `standard` posture. Other postures accept only literal addresses. |
| `proxy` | A SOCKS5 proxy. `network` is `tor`, `i2p`, or `socks` (a VPN provider, SSH `-D`, or any other SOCKS5 server). | By the proxy (`socks5h`). The device never looks up the destination. The proxy itself must be a literal `IP:port`, so reaching it needs no lookup either. |
| `overlay` | Addresses inside the overlay's `cidrs` (default `100.64.0.0/10` and `fd7a:115c:a1e0::/48`). Also `200::/7` for Yggdrasil, or your WireGuard, ZeroTier, or Nebula range. | No lookups. Use literal addresses, or pin names with `hosts`. |
| `lan` | Loopback, RFC 1918, link-local, and unique-local addresses. This is the same rule the desktop local mesh uses. Carrier-grade NAT space (`100.64.0.0/10`) is not the LAN. | No lookups, except multicast DNS for `.local` names. `hosts` pins other names. |

Other adapter fields:

- `allowPlaintext` lets `http://` and `git://` run without transport
  authentication on this adapter. The default is off, and the route is refused
  with `peer_unauthenticated`. HTTPS, SSH, onion, and I2P destinations
  authenticate the peer on their own.
- `authenticatedNetwork` on an overlay says the overlay authenticates every
  peer (WireGuard keys, for example), so plaintext inside it is acceptable.
- `isolateStreams` on a proxy sends fresh SOCKS credentials on every run.
  Tor's `IsolateSOCKSAuth` then puts each run on its own circuit. It is on by
  default for `tor` and always on in the `anonymous` posture.
- `serviceAddress` on a proxy is an onion or I2P address this device serves.
  It is the only address the private postures advertise.
- `budgets.latencyMs` (default 2000) is the median connection time above which
  a route is `degraded`.

Profile validation stops at the first failing check, in this order:
`profile_schema`, `unknown_posture`, `adapters_missing`, `adapter_id_invalid`,
`adapter_id_duplicate`, `unknown_adapter_kind`, `proxy_endpoint_not_literal`,
`proxy_network_unknown`, `overlay_cidr_invalid`, `overlay_cidr_global`,
`pinned_host_invalid`, `budget_invalid`, `posture_has_no_adapter`,
`anonymous_requires_anonymity_network`.

## Postures

| Posture | Adapters | Name lookups | Falls back to a direct connection | Telemetry | Logs | Advertises |
| --- | --- | --- | --- | --- | --- | --- |
| `standard` | all, preferring `lan`, then `overlay`, then `clearnet`, then `proxy` | system | yes | counts | full | public, overlay, and LAN addresses |
| `private` | `proxy` (any network), in profile order | by the proxy | never | counts | redacted | nothing but a service address |
| `anonymous` | `proxy` on `tor` or `i2p`, in profile order | by the proxy | never | off | redacted | nothing but a service address |
| `overlay-only` | `overlay` | none | never | counts | redacted | overlay addresses |
| `lan-only` | `lan` | none (multicast DNS for `.local`) | never | counts | redacted | LAN addresses |

In every posture except `standard`, Git runs with a scrubbed environment and
without system or global Git configuration. Proxy variables, `NO_PROXY`,
`GIT_SSH_COMMAND`, `GIT_PROXY_COMMAND`, `GIT_TRACE*`, and `GIT_CURL_VERBOSE`
cannot redirect traffic or print a destination. Credential helpers, prompts,
and HTTP redirects are also off. `git://` and `ssh://` go through this tool's
relay, which applies the same route rules as HTTP. OpenSSH runs with
`VerifyHostKeyDNS=no`, and in `private` and `anonymous` also with `-F none`,
so a `~/.ssh/config` `ProxyJump` or `HostName` cannot bypass the proxy. Load
keys through `ssh-agent`.

Before running, the tool reads the repository's own configuration. If
`url.*.insteadOf`, `http.*.proxy`, `core.gitProxy`, `core.sshCommand`,
`remote.*.proxy`, or an include is set, the run is refused with
`repository_config_overrides_route`, because it could send traffic around the
route. Nothing is sent.

## Route refusals

Each adapter is checked in this order, and the first refusal is recorded
against it:

| Code | Meaning |
| --- | --- |
| `adapter_not_permitted` | The posture does not use this kind of adapter. |
| `proxy_network_not_permitted` | `anonymous` accepts only `tor` and `i2p` proxies. |
| `destination_requires_anonymity_network` | An onion or I2P destination on a direct adapter. |
| `onion_requires_tor` | An onion destination on a proxy that is not Tor. |
| `i2p_requires_i2p` | An I2P destination on a proxy that is not I2P. |
| `local_destination_via_proxy` | A LAN, loopback, `.local`, or carrier-grade NAT address sent to a proxy, which would reveal your network layout to it. |
| `destination_outside_overlay` | The address, or pinned address, is outside every overlay range. |
| `destination_outside_lan` | The destination is not a local address, a `.local` name, or a pinned local name. |
| `name_needs_dns` | A direct adapter was given a name in a posture that forbids lookups. |
| `plaintext_via_exit` | `http://` or `git://` to a clearnet host in `anonymous`. The exit relay could read or change it. |
| `peer_unauthenticated` | Plaintext without `allowPlaintext` or an authenticated overlay. |

When no adapter is left the plan fails with `no_permitted_route`. A URL is
refused before any adapter is considered with `invalid_url` or
`scheme_unsupported` (only `https`, `http`, `ssh`, and `git` are carried).

## States

A health report has one `state`, with a `label`, a `tone`, and an
`explanation` that clients show without rewording. Precedence is from top to
bottom:

| State | Label | Tone | When |
| --- | --- | --- | --- |
| `exposed` | Exposed | danger | The trace saw a DNS query or a connection outside the route. The address or destination may have been disclosed. This wins over every other state, even if Git succeeded. |
| `unreachable` | Unreachable | danger | No permitted route answered, or the transfer broke. Nothing was sent another way. |
| `degraded` | Degraded | warning | Reachable, but with a `reason`: `failed_over` (a later route carried it), `slow` (median latency over budget), `intermittent` (some probes failed), or `unauthenticated_peer` (plaintext by opt-in). |
| `anonymous` | Anonymous | success | `anonymous` posture, healthy, and no leak observed. |
| `protected` | Protected | success | `private`, `overlay-only`, or `lan-only`, healthy, and no leak observed. |
| `direct` | Direct | neutral | `standard` posture, healthy. No anonymity is claimed. |

`blockedAttempts` counts lookups and connections the in-process guard stopped
before they left the device. They do not make a run `exposed`. The `leakTrace`
field says whether the Git process was `traced`, whether tracing was
`unavailable`, or whether it was `not_required` (the `standard` posture).

## How leaks are prevented and detected

- **DNS.** Proxy routes pass names to the proxy (`socks5h`, SOCKS5 address type
  3), and the `lan`, `overlay`, and pinned routes dial literal addresses. The
  tool's own sockets run inside an audit-hook guard. The guard blocks any
  lookup, and any connection except to the route's endpoint.
- **Direct fallback.** A private posture has no direct adapter to fall back
  to. When every permitted proxy is down, Git does not run, and the report is
  `unreachable`.
- **The Git process.** On Linux, Git runs under `strace`. Connections to
  resolver sockets (nscd, systemd-resolved, Avahi) or to ports 53, 853, or
  5353 count as `dns_query`. Any other connection outside the route counts as
  `direct_connection`. Either one makes the run `exposed`, with exit code 5.
- **Logs.** In every posture except `standard`, Git's output, errors, and the
  saved report go through redaction. The destination, proxy endpoint, SOCKS
  credentials, URLs, onion and I2P names, and IP addresses become
  `<redacted>`.
- **Telemetry.** `--telemetry FILE` appends one line with only posture, state,
  reason, and counts. `anonymous` never writes it, and the report records
  `"telemetry": "suppressed"`.
- **Peer addresses.** `advertise` lists only the addresses the posture allows
  peers to learn. It also reports `nat` as `public`, `behind_nat`,
  `carrier_nat`, or `no_clearnet_address`, and `inbound` as `direct`,
  `needs_relay_or_overlay`, `overlay`, `local_network`, `onion_service`, or
  `outbound_only`.

## Commands

```sh
tools/forkmesh_transports.py validate PROFILE [--json]
tools/forkmesh_transports.py plan --profile PROFILE URL [--json]
tools/forkmesh_transports.py probe --profile PROFILE URL [--attempts N] [--timeout S] \
  [--report FILE] [--telemetry FILE] [--json]
tools/forkmesh_transports.py git --profile PROFILE URL [--repo DIR] [--trace auto|required|off] \
  [--report FILE] [--telemetry FILE] [--json] -- GIT_ARGS...
tools/forkmesh_transports.py status REPORT [--json]
tools/forkmesh_transports.py advertise --profile PROFILE --address IP... [--json]
```

`GIT_ARGS` name the remote as `@URL`, for example
`-- -C app fetch --atomic @URL main`. Pass `--repo` for commands that run
inside a repository, so its configuration is checked first. Use `--atomic` on
fetches: when a route breaks mid-transfer, Git keeps every ref as it was, and
you can repeat the same command once the route recovers.

Git's own failures, such as a missing repository or a rejected push, do not
cause failover, because another route would fail the same way.

| Exit code | Meaning |
| --- | --- |
| `0` | Done. |
| `2` | Usage error, invalid profile, or `--trace required` without a tracer. |
| `3` | Refused by policy before anything was sent. |
| `4` | Unreachable: no permitted route answered, or the transfer broke. |
| `5` | Exposed: a prohibited DNS query or connection was observed. |
| `6` | Git failed on a reachable route. |

## Supported platforms

| Platform | Adapters and failover | `git://` and `ssh://` relay | Leak tracing of the Git process |
| --- | --- | --- | --- |
| Linux | covered by the CI workflow | covered by the CI workflow | covered by the CI workflow with `strace` |
| macOS | covered by the CI workflow | covered by the CI workflow | not available: `leakTrace` is `unavailable`; the in-process guard and Git configuration still apply |
| Windows | not yet tested | not yet tested | not available |

## Evidence

`app/tests/test_transports.py` is the `transport-conformance` quality gate. The
`Transport conformance` workflow runs it on Linux and macOS. It covers:

- common stream conformance for every adapter: 2 MiB round trip, throughput,
  latency, and unreachable detection;
- repository conformance for every adapter over smart HTTP and `git://`:
  `ls-remote`, clone, atomic fetch, and push;
- every planner vector and profile error;
- leak tests: hostile environment variables, dead proxies with no fallback, a
  simulated regression that drops the proxy (caught as `exposed`), repository
  configuration overrides, stream isolation, and log and telemetry disclosure;
- failover, a transfer dropped mid-fetch with refs unchanged and recovery
  afterwards, slow and intermittent routes, and address advertisement.

## Not built yet

- The desktop, mobile, and web clients do not yet show these states or edit
  profiles. They should read the health report and display `label`, `tone`,
  and `explanation` as written.
- Only Git traffic is carried. Relay chat, the Worker API, mirror sync, and the
  desktop local mesh do not yet read the profile, so a private posture here
  does not cover them.
- Tracing covers Linux only. macOS (`dtruss`, which needs SIP changes) and
  Windows (ETW) have no process-level leak evidence yet.
- A real Tor or I2P daemon, NAT traversal between two real networks, and
  bandwidth over real links are not part of CI. The tests use a local SOCKS5
  proxy that behaves like Tor's SOCKS port.
