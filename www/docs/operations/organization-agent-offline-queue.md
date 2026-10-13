# Organization agent offline queue

Organization owners can save Claude Code and Codex work for the desktop linked
to an organization repository even while that desktop is offline. The queue is
durable in D1 and has no admission-count limit. List responses remain bounded;
that response bound is not a queue or task-creation cap.

## Authorization and execution boundary

Offline admission requires all of the following:

- the requester has the current organization `owner` role;
- the repository has an exact `org_repos` link;
- the requester's account owns the linked source node;
- the retained signed catalog record identifies that repository on a
  `desktop` runtime; and
- the record advertises the requested provider.

An explicitly selected owner target must be that linked desktop. Engineering
members retain the existing fresh, provider-capable mirror route.

Admission does not authorize execution. A reconnecting desktop must still
prove current node ownership when it claims the oldest job. It validates the
exact lease envelope and rechecks the provider binary and login locally before
dispatch. There is no model-based safety preflight: the Claude Haiku gate was
removed on 2026-09-13 (`a7dfa7936`), and leased jobs now dispatch directly
(`desktop/src/MainWindowAgents.cpp`, `server/agents.go`). Haiku remains
selectable as a model. Job and session rows remain encrypted at rest.

## Drain and recovery

The Worker leases one job per repository at a time in ascending row order and
verifies the conditional queued-to-leased update before returning it. The
desktop asks for the next job only after the Worker acknowledges the current
result. A lease abandoned for two minutes (`ORG_AGENT_JOB_LEASE_MS` in
`app/src/entry.py`) returns to `queued`; queued and running rows are never
removed by this flow.

The Qt client starts a drain after normal consolidated sync, including the
startup/reconnect fallback. It persists a running organization's job id, lease,
and repository envelope with the local agent session, restores that binding
after restart, and retries the terminal report. Resumed local sessions are
queued oldest first.

## Operator diagnostics

Queue rejections include a stable `error`, a safe user-facing `message`,
`retryable`, and—when relevant—`requiredAction`, `provider`, and `targetNode`.
Common actions are:

- `link_repository` or `publish_repository`;
- `link_owned_desktop` or `link_desktop_target`;
- `publish_provider_capability`;
- `select_linked_desktop` or `select_eligible_target`;
- `bring_agent_mirror_online`; and
- `relink_desktop`.

The Dashboard, chat composer, and World display the server message without
replacing it with a generic queue failure.

## Verification and rollback

Before deployment, run:

```bash
python3 tools/quality_gate.py validate
python3 tools/quality_gate.py run --profile core
python3 tools/quality_gate.py run --profile release
```

Focused QA should save several Claude Code and Codex tasks while the linked
desktop is offline, reconnect it, and confirm the jobs enter preflight in
creation order. Also exercise a non-owner, an unlinked repository, a foreign or
non-desktop target, and a missing provider capability; each UI must show the
exact server-authored remedy.

Rollback the Worker and web assets to the prior deployment revision if needed.
The `0114_remove_organization_task_limit.sql` migration only drops an admission
trigger and does not rewrite or delete task data, so rollback does not require
a reverse data migration. Do not recreate the old trigger: doing so would make
new durable work fail again once an organization retained 2,000 tasks.
