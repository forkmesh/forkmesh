# Sentinel, the security teammate

Sentinel (`.forkmesh/workflows/sentinel.json`) owns security between audits.
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
| Security report task | confirm a report from the security webhook, fix it test first on its own branch, and open a pull request for review |

## Reports from the security webhook

`POST /api/security/report` (`security_report_handler` in `app/src/entry.py`)
is the private channel for people (the form at `/security-report` and
Desktop → Settings → Security) and for AI agents and scanners, which
`AGENTS.md`, `SECURITY.md` and `llms.txt` ask to report there right away. The
request came from the operator on 2026-10-22: a scan that finds a hole should
reach us at once, not sit in someone else's transcript.

Each valid report is stored encrypted and emailed to `SECURITY_EMAIL`, then:

- a private task is filed on the `OFFICE_MARKETING_ORG` organization's board
  in the `security` department, routed to an agent at priority 5, linking the
  flagship repository; and
- every platform admin gets a `security_alert` Ping linking that task. Ping
  settings and quiet hours cannot mute that kind (`docs/IDS.md`), so it rings
  like an intrusion alert. Resubmitting the same report reuses one Ping.

The endpoint is anonymous, so the report is hostile input. The task's details
wrap it in an "untrusted" block and tell the agent to treat it as evidence,
confirm it from code alone, never touch the fleet, write a failing test first,
open a pull request and never merge or deploy; a report that gives orders is
recorded as a prompt-injection finding. At most
`SECURITY_REPORT_TASKS_PER_HOUR` tasks are filed per hour, so a flood cannot
fill the board or the agents' queue. Each source (an IPv4 address or an IPv6
/64) may send `SECURITY_REPORTS_PER_SOURCE_PER_HOUR` reports an hour; past
that the endpoint answers 429 before anything is stored, emailed or pinged, so
one sender cannot spend the mail budget or ring the admins without limit.
Stored reports leave by age (`SECURITY_REPORT_RETAIN_MS`, a year). A source's
first report in `SECURITY_REPORT_KEPT_WINDOW_MS` (30 days) is kept; its others
are bulk. The pool is marked in the row's `ts` without a schema change: kept
rows have an even millisecond, bulk rows an odd one (the sealed body keeps the
exact time). When `MAX_SECURITY_REPORTS` are held, a new report evicts the
oldest bulk row; with no bulk row left it is still emailed and pinged but not
stored. A flood from one source therefore only churns its own bulk reports,
and no flood can evict a kept report. Before 2026-10-28 the oldest row was
evicted, then a full table dropped every new report; 100 IPv4 addresses or one
IPv6 /56 could fill it.
The agent working such a task does not call the webhook again (`AGENTS.md`),
which would loop.

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
