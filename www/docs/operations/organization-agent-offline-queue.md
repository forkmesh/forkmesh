# Organization agent offline queue

Organization owners can queue Claude Code and Codex work for the desktop linked
to an organization repository while that desktop is offline. The queue is
durable and has no admission-count limit; list responses are bounded.

## Admission

Offline admission requires that the requester holds the organization `owner`
role, the repository has an exact `org_repos` link, the requester owns the
linked source node, the signed catalog record places the repository on a
`desktop` runtime, and that record advertises the requested provider. Other
members use the online, provider-capable mirror route.

Admission does not authorize execution. The reconnecting desktop proves node
ownership when it claims a job, validates the lease envelope, and rechecks the
provider binary and login before dispatch. Job and session rows are encrypted
at rest.

## Drain and recovery

The server leases one job per repository at a time, oldest first, using a
conditional queued-to-leased update. The desktop requests the next job only
after its result is acknowledged. A lease abandoned for two minutes
(`ORG_AGENT_JOB_LEASE_MS`) returns to `queued`; queued and running rows are
never deleted by this flow. The desktop starts a drain after each sync,
persists the running job's lease with the local session, and retries the
terminal report after a restart.

## Diagnostics

Rejections carry a stable `error`, a user-facing `message`, `retryable`, and
where relevant `requiredAction`, `provider`, and `targetNode`. Actions include
`link_repository`, `publish_repository`, `link_owned_desktop`,
`link_desktop_target`, `publish_provider_capability`, `select_linked_desktop`,
`select_eligible_target`, `bring_agent_mirror_online`, and `relink_desktop`.
Clients show the server message verbatim.

## Verification

Run `python3 tools/quality_gate.py run --profile core` (and `--profile release`
before a release). Rolling back the server needs no data migration; do not
reintroduce a per-organization task-count trigger.
