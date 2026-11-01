# Issue assignment: the Assign button, the one-issue rule and deadlines

Part of [the App docs](README.md). How a contributor takes an issue and how
a repository keeps assignments moving: the **Assign** button, the limit of
one open issue without a pull request, and an optional time limit shown as a
progress bar. Both settings live in `.forkmesh/issues.json` on the default
branch:

```json
{"assignmentLimit": 1, "assignmentHours": 24}
```

## The Assign button

An open issue's comment box has an **Assign** button beside **Comment** (and
**Close issue** for managers), so a contributor takes an issue with one click
instead of typing `/assign` in a comment. It signs the same `claim` event as
the sidebar's **Assign yourself** link (issue #570,
`handleWebIssueAction("claim")`), so it needs no repository role and moves only
the viewer's own name. It shows while the issue is open and the viewer is not
an assignee; once they are, the sidebar's **Unassign yourself** link is how
they let go.

The button is a secondary button (`bg-secondary`), not `cta`: green stays the
page's one create button (theme.json). The server-rendered first view
(`member_comment_box` in `app/host/forkmesh_host/repo_ssr_issue.py`) draws it
on every open issue, as its sidebar always offers **Assign yourself**, because
the server does not know who is viewing; the dashboard drops it when the
viewer is already assigned.

A typed `/assign` comment is unchanged: it is an ordinary comment, and a
repository workflow that listens for it (`docs/server/instance-actions.md`,
"Assigning") still runs.

## One open issue at a time, until there is a pull request

A contributor who assigns themselves may hold one open issue in a repository
at a time. An issue stops counting once they have a pull request linked to
it that is open or merged, so opening the pull request for the first issue
frees them to take the next one. A closed, unmerged pull request does not
count.

- Linking is the sidebar's Development linking, the same test the
  assignment deadline bar uses ([below](#assignment-deadline)):
  a pull request authored by the assignee whose title or description names
  the issue (a closing keyword, or a plain `#N`), or a signed link in the
  issue's comments (`issueAssigneeHasPull`).
- Held issues are the repository's open issues whose assignees, with signed
  claims folded in, include the viewer (`issueAssignmentsWithoutPull`, over
  `loadRepoIssueRelationIndex` and `loadRepoDevelopmentPulls`). The issue
  being claimed is left out.
- When the viewer already holds the limit, the claim is not signed and the
  page says which issues: "You're already assigned to #8. Link a pull request
  to it (or unassign yourself there) before taking another issue."
- Repository managers (`issueMutationAuthorized`) are not limited: they can
  assign anyone through the Assignees picker anyway.
- If the issue list or the pull request list cannot be read, the claim goes
  ahead. The rule guides contributors; it is not a security boundary, since a
  claim only ever moves the claimer's own name.
- After a claim or release the cached issue lists take the new event
  (`syncWebIssueListClaim`), so taking a second issue within the minute the
  lists are cached is still counted.

### Setting the limit

`.forkmesh/issues.json` on the default branch, next to `assignmentHours`
([deadline](#setting-the-deadline)):

```json
{"assignmentLimit": 1}
```

`assignmentLimit` is how many open issues without a linked pull request one
person may hold through self-assignment: a whole number, a numeric string is
accepted, capped at 100. `0` turns the rule off. A missing key, or anything
that is not a whole number of zero or more, means `1`. Parsing is in
`repoCollabSettingsFromBlobs` (`issueAssignmentLimit`) in
`app/public/dashboard/js/06-repo-content.js`.

The check runs in the browser before the claim is signed. The relay, the Go
intake and the Actions job API accept a claim as before, so a repository
workflow that assigns through `forkmesh-actions assign` is not limited by it.

Tests: `app/tests/test_issue_assignment_limit.py`.

## Assignment deadline

A repository can give assignees a time
limit. When it does, each assignee in
an open issue's sidebar gets a small progress bar under their name: the time
since they were assigned against the limit, with the time left as text
("18 h left", "45 min left", "3 d left", "overdue by 2 h").

### Setting the deadline

`.forkmesh/issues.json` on the repository's default branch:

```json
{"assignmentHours": 24}
```

`assignmentHours` is a positive number of hours (fractions allowed, a numeric
string is accepted, capped at one year). Anything else, or no file, means no
deadline and no bar.

The key lives in `.forkmesh/issues.json`, not `.forkmesh/pulls.json`: it is
an issue setting, that file already holds the other issue setting
(`aiPromptTemplate`, [ai-disclosure.md](ai-disclosure.md)), and the dashboard
already reads it in the same batched blob read as `pulls.json` and
`AI_POLICY.md` (`loadRepoCollabSettings`, cached for a minute). Parsing is in
`repoCollabSettingsFromBlobs` (`issueAssignmentHours`) in
`app/public/dashboard/js/06-repo-content.js`.

### When the bar shows

- The issue is open, the repository sets `assignmentHours`, and the person is
  a current assignee.
- The assignment time is the signed issue event that last added that person:
  an `assignees` event naming them when they were not already assigned, or a
  `claim` (self-assignment). Removing someone and adding them again restarts
  their clock. An assignee with no such event (an older projection) gets no
  bar (`issueAssignmentTimes`).
- It is hidden once a pull request by that assignee is linked to the issue
  and is open or merged, using the same linking as the sidebar's Development
  section (`issueLinkedPullNumbers`: a closing keyword in the pull request or
  a signed link event). A closed, unmerged pull request does not hide it. The
  repository's pull list loads after the page, so the bar is redrawn when the
  Development section is (`renderRepoIssueDeadlines`).

### Colours

The track uses the neutral counter colour and the fill uses `primary`. Under
25% of the time left, or overdue, both the fill and the text use `attention`
(theme.json roles). Hovering shows the due time.

Without a deadline the assignee rows keep their usual markup, so the
server-rendered first view (`app/host/forkmesh_host/repo_ssr_issue.py`, which
does not draw the bar) still matches the dashboard; the bar appears when the
page script renders the issue.

The bar is computed in the browser from the viewer's clock when the issue is
drawn; it is not stored. Tests: `app/tests/test_issue_assignment_deadline.py`.
