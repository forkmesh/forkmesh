# Assignment deadline on issues

A repository can give assignees a time limit. When it does, each assignee in
an open issue's sidebar gets a small progress bar under their name: the time
since they were assigned against the limit, with the time left as text
("18 h left", "45 min left", "3 d left", "overdue by 2 h").

## Setting

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

## When the bar shows

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

## Colours

The track uses the neutral counter colour and the fill uses `primary`. Under
25% of the time left, or overdue, both the fill and the text use `attention`
(theme.json roles). Hovering shows the due time.

Without a deadline the assignee rows keep their usual markup, so the
server-rendered first view (`app/host/forkmesh_host/repo_ssr_issue.py`, which
does not draw the bar) still matches the dashboard; the bar appears when the
page script renders the issue.

The bar is computed in the browser from the viewer's clock when the issue is
drawn; it is not stored. Tests: `app/tests/test_issue_assignment_deadline.py`.
