# Non-custodial organization succession

Organization succession is a governance mechanism for one organization
roster. It is not inheritance, account recovery, custody, or asset transfer.

## Transfer boundary

A completed succession performs exactly two scoped changes: the configured
owner becomes an organization administrator, and the named successor becomes
the owner. Only those two `org_members.role` values change; repository records,
links, teams, memberships, private-repository shares, and encryption recipients
are untouched.

Never transferred:

- wallets, wallet addresses, funds, rewards, or signing ability;
- private keys, account credentials, agent credentials, or private agent data;
- user accounts, personal data, devices, or device ownership;
- repository ownership, content, or ACLs.

Every succession response states this boundary.

## Configuration and lifecycle

Only the sole current owner configures a policy with
`PUT /api/orgs/{org}/succession`. The successor must be an active existing
administrator or member. Unknown or transfer-shaped fields are rejected.

| Setting | Range | Default |
|---|---|---|
| Inactivity threshold | 30–730 days | 180 |
| Grace period | 7–90 days | 30 |
| Independent approvals | 2–20 | 2 |

Approvals cannot exceed the eligible roster; owner and successor cannot approve.

1. `POST .../check-in` (or enabled-device and owned-node activity) refreshes the
   owner's activity and cancels an open grace case.
2. An hourly job sends one warning before the deadline; it never opens a case.
3. After the threshold, a current member calls `POST .../activate` to open grace.
4. During grace the owner may `cancel` or `check-in`; other current members
   `approve` (idempotent; removed members' approvals stop counting). Only
   members who already held an approving role when grace opened may approve:
   opening the case records that roster in `org_succession_case_approvers`
   (a trigger on the case insert, migration 0295), and both the approval
   insert and the completion check require the approver to be in it. An
   account added or promoted during grace gets
   `approver_joined_after_grace_opened`, so an administrator cannot reach the
   threshold with accounts made for the purpose.
5. After grace, a member calls `POST .../finalize`. One transaction rechecks
   deadline, owner, successor, and current approvers before changing the roles;
   any change fails closed.

`DELETE /api/orgs/{org}/succession` disables the policy and cancels any case.

## Authorization and audit

The API uses organization session and roster checks only; it has no
platform-administrator authorization path, and the tables are hidden from the
generic admin editor. Cases and approvals are immutable snapshots, and every
action appends a metadata-only event; triggers keep history append-only.
`GET /api/orgs/{org}/succession` (`no-store`) returns status, counts, and
dates, never device data.
