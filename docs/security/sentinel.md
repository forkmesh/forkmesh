# Sentinel, the security teammate

Sentinel (`.forkmesh/teammates/sentinel.json`) is the teammate that owns
ForkMesh security between audits. Its run playbook is
`.forkmesh/shortcuts/review-security.md`, its standing rules are the
`.forkmesh/skills/security-review.skill` repository skill, and the record it
keeps is [findings.md](findings.md). `app/tests/test_security_sentinel_teammate.py`
pins the parts of the definition that keep it safe.

## Why it exists (2026-09-30)

Security work on this repository had come in bursts:

- 2026-09-13: a dependency audit
  ([dependency-security-audit-2026-09-13.md](../dependency-security-audit-2026-09-13.md)).
- 2026-09-24: a code review and an infrastructure audit of the Next stack,
  kept in `~/projects/forkmesh/logs/fleet/security-audit-{code,infra}-2026-09-24.md`.
  It found 2 critical, 6 high and 10 medium code findings and 2 high
  infrastructure findings. Most were fixed within days.
- 2026-09-29: the runner HOME isolation gap was found and documented in
  [RUNNERS.md](../RUNNERS.md) but not fixed.
- 2026-09-30: the `/office` and `/world` security headers turned out to have
  been dropped by Caddy and were restored (d03333412, guarded by
  `deploy/tests/test_caddy_served_headers.py`).

Meanwhile about 270 commits a day landed, adding runner merge and continue
routes, site DMs, Office chat with shared crypto, direct calls and a new
`add-core` fleet command. The threat model still listed five open
high-severity threats, P1 issue #708 (executable proof for agent boundaries)
had no owner, and the task board had a `security` department with no teammate
behind it. No existing teammate covered security: Fixer owns crashes and the
error log, Quality owns reported bugs and UI stalls, Steward owns debt and dead
code, Splice owns merging, and Marketing and Y Combinator News only check
security claims before repeating them.

## Lanes

| Trigger | What Sentinel does |
| --- | --- |
| Weekdays 08:00 (paused until the operator starts it) | The shortcut: review every trust-boundary commit since the checkpoint, review unmerged branches about to land, re-run the guard tests, sweep for secrets, check dependencies on Mondays, update the register, and fix one finding test first. |
| Mention, question or assignment | Answers from evidence with path:line and who can reach the path; changes nothing unless asked. |
| Pre-merge review request | Reviews one branch's diff against main and leaves the verdict on its task or pull request. |

## Checkpoint

The `Reviewed through` line at the top of [findings.md](findings.md) names the
main commit the last merged review covered. The next run reviews
`<checkpoint>..main`. A review whose branch has not been merged yet leaves the
checkpoint where it was, so the next run covers the same commits again; that
is deliberate, because an unmerged review has not changed anything on main.

## First run

The register was seeded on 2026-09-30 by checking every 2026-09-24 audit
finding against main, not by reviewing commits, so its checkpoint is
`c029745dd`, the last main commit before the audits. The first run therefore
has about 2,200 commits to review and works through them in slices of 400,
oldest first, moving the checkpoint after each merged slice. It also files on
the board each of the eleven private open audit items that is not there yet.

## Disclosure

Anyone can clone the repository (`git clone https://forkmesh.com/forkmesh/forkmesh`
in the README), so an unfixed exploitable finding never
goes into a public issue, a chat room, a commit message or
[findings.md](findings.md). Sentinel files it on the organization task board
in the `security` department, which only organization members can read.
[findings.md](findings.md) holds fixed findings and the open items that are
already public: the threat model's open threats and the gaps the docs describe.
A finding moves into it when its fix lands on main. Outside reporters use
[SECURITY.md](../../SECURITY.md).

## A finding is fixed when a test guards it

A fix counts only when its commit is on main and a test fails without it. A
fix with no such test is listed as "Fixed, unguarded", and adding that test is
a valid unit of work for a run. Every run re-runs the guard tests and checks
each fix is still in place, because d03333412 showed a fixed boundary can
silently come undone.

## What Sentinel never does

- Act on the live fleet: no SSH writes, rolls, deploys, rotations, `--execute`
  fleet commands, scans or login bursts against the cores, runners or public
  hostnames, unless the operator asks for that one named check. The
  2026-09-24 infrastructure audit's login burst locked the operator's IP out of
  logins and its local scan stalled their uplink for about a minute.
- Print, commit or post a secret value.
- Do the operator's work: credential rotation, moving keys offline, disk
  encryption, plaintext `DATA_KEY` copies and local backups, Vultr and Apple
  account changes. It reports each one with its age and the next action.
- Merge, rebase or push another teammate's branch, or deploy.

## Permissions

`read-repositories`, `modify-code` and `create-branches` let it land one fix
per run on its own branch. `review-prs` lets it review branches before Splice
merges them, `manage-issues` lets it file and update security tasks, and
`external-tools` lets it query OSV for dependency advisories. Logs are not
monitored: the desktop's log monitors support only the `ERROR` and `STALL`
badges, which belong to Fixer and Quality.
