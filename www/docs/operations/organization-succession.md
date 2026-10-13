# Non-custodial organization succession

ForkMesh organization succession is a governance mechanism for one
organization roster. It is not inheritance, account recovery, custody, or an
asset-transfer service.

## Transfer boundary

A completed succession performs exactly two scoped changes:

1. the configured organization owner becomes an organization administrator;
2. the explicitly named successor becomes that organization's owner.

The database completion trigger updates only those two `org_members.role`
values for the configured organization. Repository records, organization
repository links, teams, memberships, private-repository shares and encryption
recipients are not rewritten.

The following never transfer:

- wallets, wallet addresses, funds, rewards, transactions, or signing ability;
- private keys, account credentials, agent credentials, or private agent data;
- user accounts, profile/personal data, devices, or device ownership;
- repository ownership, repository content, or public/private repository ACLs.

ForkMesh does not create a custodial account or wallet for an organization or
successor. A succession response always includes this technical boundary.

## Configuration and lifecycle

Only the organization's sole current owner can configure a policy with
`PUT /api/orgs/{org}/succession`. The successor must be an explicitly named,
active user who is already an administrator or member of that organization.
Unknown fields fail closed, and transfer-shaped fields are rejected.

Configuration is bounded:

- inactivity threshold: 30–730 days (default 180);
- grace period: 7–90 days (default 30);
- independent approvals: 2–20 (default 2).

The approval threshold cannot exceed the current eligible roster. The owner
and successor are excluded from approvers, so approvals are independent.

The current owner can use `POST .../check-in`. A check-in refreshes the
succession activity timestamp and cancels an open grace case. Generalized
enabled-device and owned-node `last_seen` maxima also count as owner activity;
device identifiers, labels, platforms, locations and payloads never enter the
succession tables or API.

An hourly bounded job issues one deduplicated warning as the inactivity
deadline approaches. It never opens a case or transfers a role. A currently
authorized organization member must explicitly call `POST .../activate` after
the threshold to open the grace period.

During grace:

- the current owner may call `POST .../cancel` or `POST .../check-in`;
- independent current members call `POST .../approve`;
- duplicate approvals are idempotent;
- removed members' prior approvals stop counting immediately;
- the owner and successor cannot approve.

After grace, a current organization member may call `POST .../finalize`. D1
rechecks the grace deadline, current owner, current successor, and threshold of
currently authorized approvers inside the same transaction that changes the two
roles and appends completion history. A changed roster or stale approval fails
closed.

`DELETE /api/orgs/{org}/succession` disables the policy and cancels an active
case. Deleting an organization also disables/cancels current succession state;
its audit history remains append-only.

## Authorization and audit

The succession API receives organization-session and organization-roster
helpers only. It has no platform-administrator authorization method. Platform
administrators who are not current organization members receive `403`, and the
succession tables are hidden from the generic administrator table editor.

Cases and approvals are immutable snapshots. Approval, case, and completion
triggers protect transitions at the database boundary. Every configuration,
warning, grace opening, approval, cancellation, check-in, denied completion,
and completion adds a metadata-only event. Update/delete triggers make event
and approval history append-only.

Authorized organization members can read current status and bounded history
with `GET /api/orgs/{org}/succession`. The endpoint is `no-store`; it returns
counts and policy dates, not device data or sensitive account activity.
