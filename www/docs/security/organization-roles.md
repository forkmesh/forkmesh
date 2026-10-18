# Organization roles and the authorization matrix

Every organization request is decided by one deny-by-default table,
`app/src/organization_rbac.py`. A role holds a capability only when the matrix
names it; unknown roles, empty roles and unlisted capabilities are refused for
everyone, owners included. The tables below are generated from that module and
checked against it by `app/tests/test_organization_rbac.py`.

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

`read` capabilities never change organization state; no `write` row lists
`auditor` or `guest`.

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

- An `owner` may assign or remove any role.
- An `admin` may manage `maintainer`, `member`, `auditor` and `guest`
  accounts, but cannot grant `owner`/`admin` or change an account holding one.
- Other roles assign nothing.
- The last `owner` can never be removed or demoted.

Denied writes are recorded in the sensitive-action audit log with a reason
(`insufficient_role`, `owner_role_required`, `last_owner`).

## Enforcement points

- Organization settings, members, teams, linked repositories, integrations,
  bot tokens and repository imports.
- `_org_permission`, used by the Git push gate and every repository-permission
  check: the role's baseline, raised by team grants up to the role's ceiling.
- A linked repository's Settings tab and every endpoint behind it (About,
  features, federation switches, webhooks, provider mirror, Actions secrets,
  logo review, delete) need repository permission `admin`, so `owner` and
  `admin` hold it and `maintainer` does not unless a team grants `admin`; the
  account that stores the repository keeps it too
  (`docs/app/repo-settings-access.md` in the source tree).
- World building access, office chat channels, task boards and succession
  votes, which enumerate the insider roles in SQL.

Roles are stored as plain text in `org_members.role`; a build that does not
know a role treats it as no membership.
