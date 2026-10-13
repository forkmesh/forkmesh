# Organization roles and the authorization matrix

Every organization request in the Worker is decided by one deny-by-default
table, `app/src/organization_rbac.py`. A role holds a capability only when
the matrix names it; an unknown role, an empty role, or a capability the
matrix does not list is refused for everyone, owners included. This page is
generated from that module and checked against it by
`app/tests/test_organization_rbac.py`, so the table below is the policy the
deployed Worker enforces.

## Roles

Strongest first.

| Role | Purpose | Repository permission on linked repositories |
| --- | --- | --- |
| `owner` | Runs the organization. The only role that can dissolve it or grant and revoke `owner`/`admin`. | `admin` |
| `admin` | Administers members, teams, linked repositories, integrations and bot tokens. Cannot promote anyone to `owner`/`admin` or demote them. | `admin` |
| `maintainer` | Ships code: may queue agents and manage bots it links, administers nothing. | `maintain`, a team grant can raise it to `admin` |
| `member` | Collaborates. | `read`, a team grant can raise it to `admin` |
| `auditor` | Read-only observer: sees the roster, agent work and internal surfaces, never writes, pushes or votes. | `read`, a team grant cannot raise it |
| `guest` | Outside collaborator: sees the public profile and only the repositories a team grants, read-only. Not an insider for world floors, office chat channels, task boards or succession votes. | none, a team grant raises it to `read` at most |

## Capabilities

`read` capabilities never change organization state; `write` capabilities
do, and no `write` row lists `auditor` or `guest`. The Engineering-floor team
check still applies on top of the agent rows, exactly as before.

| Capability | Kind | owner | admin | maintainer | member | auditor | guest |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `organization.agents.queue` | write | yes | yes | yes | yes | no | no |
| `organization.agents.read` | read | yes | yes | yes | yes | yes | no |
| `organization.bots.link` | write | yes | yes | yes | yes | no | no |
| `organization.bots.manage` | write | yes | yes | no | no | no | no |
| `organization.chat.channels` | write | yes | yes | yes | yes | no | no |
| `organization.dissolve` | write | yes | no | no | no | no | no |
| `organization.fediverse.manage` | write | yes | yes | no | no | no | no |
| `organization.issues.manage` | write | yes | yes | no | no | no | no |
| `organization.members.manage` | write | yes | yes | no | no | no | no |
| `organization.pull_rules.manage` | write | yes | yes | no | no | no | no |
| `organization.read` | read | yes | yes | yes | yes | yes | yes |
| `organization.repositories.import` | write | yes | yes | no | no | no | no |
| `organization.repositories.link` | write | yes | yes | no | no | no | no |
| `organization.roles.administer` | write | yes | no | no | no | no | no |
| `organization.roster.read` | read | yes | yes | yes | yes | yes | no |
| `organization.settings.write` | write | yes | yes | no | no | no | no |
| `organization.succession.approve` | write | yes | yes | yes | yes | no | no |
| `organization.tasks.administer` | write | yes | yes | no | no | no | no |
| `organization.teams.manage` | write | yes | yes | no | no | no | no |
| `organization.world.private` | read | yes | yes | no | no | no | no |
| `organization.world.restricted` | read | yes | yes | yes | yes | yes | no |

## Role administration

`organization.members.manage` says who may add, re-role or remove members;
`organization.roles.administer` restricts the administrative roles:

- an `owner` may assign or remove any role;
- an `admin` may add, re-role and remove `maintainer`, `member`, `auditor`
  and `guest` accounts, but can neither hand out `owner`/`admin` nor touch an
  account that currently holds one;
- every other role may assign nothing;
- the last `owner` can never be removed or demoted.

Every denied write is recorded in the sensitive-action audit log with the
reason (`insufficient_role`, `owner_role_required`, `last_owner`).

## Where the matrix is enforced

- `/api/orgs/<org>` settings, `/members`, `/teams`, `/teams/<team>/members`,
  `/repos`, the fediverse toggle, bot tokens, and repository imports into an
  organization.
- `_org_permission`, which the git push gate and every repository-permission
  check use: the role's baseline, raised by team grants up to the role's
  ceiling.
- World building access (`restricted` admits insiders, `private` admits
  administrators), office chat channel creation, task boards and the
  Engineering floor roster, which enumerate the insider roles in SQL.
- Succession votes: the D1 triggers enumerate the
  `organization.succession.approve` roles (migration
  `0183_organization_rbac_roles.sql`).

## Rollback

The role vocabulary is plain text in `org_members.role`, so a rollback
needs no data migration: a Worker build that predates a role treats it as no
membership, which is the deny-by-default behaviour. The down migration
restores the previous succession triggers.
