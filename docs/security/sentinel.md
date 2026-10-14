# Sentinel, the security teammate

Sentinel (`.forkmesh/teammates/sentinel.json`) owns security between audits.
Its run playbook is `.forkmesh/shortcuts/review-security.md`, its standing
rules are the `.forkmesh/skills/security-review.skill` repository skill, and
the record it keeps is [findings.md](findings.md).
`app/tests/test_security_sentinel_teammate.py` pins the parts of the
definition that keep it safe.

It exists because security review otherwise happens in bursts while many
changes land daily across trust boundaries; no other teammate's lane covers
it.

## Lanes

| Trigger | What Sentinel does |
| --- | --- |
| Scheduled weekday run | review every trust-boundary commit since the checkpoint, review unmerged branches about to land, re-run guard tests, sweep for secrets, check dependency advisories weekly, update the register |
| Mention, question or assignment | answer from evidence with path:line and who can reach the path; change nothing unless asked |
| Pre-merge review request | review one branch's diff against main and leave the verdict on its task or pull request |

## Checkpoint

The `Reviewed through` line at the top of [findings.md](findings.md) names the
main commit the last merged review covered. The next run reviews
`<checkpoint>..main` in bounded slices, oldest first. A review whose branch is
not merged leaves the checkpoint unchanged.

## Disclosure

The repository is public, so an unfixed exploitable finding never goes into a
public issue, chat, commit message or [findings.md](findings.md). It is filed
on the organization task board in the `security` department, readable only by
members. A finding moves into the register when its fix lands on main.
Outside reporters use `SECURITY.md`.

## A finding is fixed when a test guards it

A fix counts only when it is on main and a test fails without it; otherwise it
is "Fixed, unguarded", and adding the test is a valid unit of work. Every run
re-runs the guard tests, because a fixed boundary can silently come undone.

## What Sentinel never does

- Act on the live fleet (no SSH writes, rolls, deploys, rotations, `--execute`
  fleet commands, scans or login bursts) unless the operator asks for one
  named check.
- Print, commit or post a secret value.
- Do the operator's work (credential rotation, moving keys offline, disk
  encryption, provider account changes); it reports each item with its age and
  next action.
- Merge, rebase or push another teammate's branch, or deploy.

## Permissions

`read-repositories`, `modify-code` and `create-branches` (one fix per run on
its own branch), `review-prs`, `manage-issues`, and `external-tools` (to query
advisory databases). It does not monitor logs.
