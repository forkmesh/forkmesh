# First-repository activation experiment

**Owner:** Marketing (marketing manager special agent)
**Written:** 2026-10-04
**Status:** merged on main on 2026-10-04 (`1e37a453a9`); starts when the
served stack is deployed from that commit or a later one, which had not been
checked by the end of 2026-10-04
**Plan:** [../marketing/plan.md](../marketing/plan.md), backlog item A1

## Why this stage

Activation is the funnel's tightest visible stage on 2026-10-04:

- Public NodeInfo (`https://forkmesh.com/nodeinfo/2.1`) reports 235 accounts
  at 18:05Z, the same as on 2026-09-30 and 5 fewer than on 2026-09-16.
- The public "Most public repositories" leaderboard
  (`https://app.forkmesh.com/api/leaderboards`, board `repos`) lists 4 owners,
  each with 1 public repository. Private repositories are not on that board,
  so this is a lower bound, not the activation rate.
- A new account that verifies its email lands on `/dashboard`. The home
  page's "Top repositories" rail is empty and said only "No repositories yet.
  Create one." There is no onboarding checklist.
- The new-repository form already mirrors a public URL from `github.com`,
  `gitlab.com` or `codeberg.org` in the browser on the served stack
  (`HOSTED_IMPORT_HOSTS` in `app/wrangler.toml`, `hostedImportAllowed` in
  `app/public/dashboard/js/05-repo-list-explorer.js`). The empty state did not
  say so, and a developer arriving with an existing project reads "Create one"
  as "start an empty repository".

## Brief

| Field | Value |
| --- | --- |
| Audience | New account holders on `/dashboard` with no repositories |
| Problem | They already keep their code on another host and do not see that ForkMesh can mirror it in one step |
| Message | "No repositories yet. Mirror one from GitHub, GitLab or Codeberg, or create one." |
| Channel | Dashboard home, left rail empty state (in product) |
| Call to action | The link to `/dashboard/repos/new` |
| Deliverables and owners | Copy change in `05-repo-list-explorer.js` and the rebuilt `app/public/dashboard.js` (Marketing, this branch); merge and deploy (operator); weekly readout (operator pastes the numbers, Marketing records them) |
| Tagged links | None. This is an in-product change measured by events, not a campaign link |
| Success measure | Weekly cohort ratio `first_mirror_created` / `signup_completed` on `/admin/acquisition` (same attribution session); secondary: "New accounts activated" on `/product-analysis` (first mirror within 7 days of signup, every account, once branch `1380-marketing-persistent-marketing` is deployed), and "accounts with at least one mirror" with its 30-day change |
| Baseline | Not readable by Marketing on 2026-10-04 (both pages need an admin browser session). The operator's first readout of the last complete week before deploy is the baseline |
| Start | The deploy date; planned for the week of 2026-10-05 |
| End | 28 days after the start |
| Follow-up review | The first Marketing run after the end date; planned 2026-11-02 |

## Decision rule

- Continue and keep the copy if the ratio rises by at least 5 percentage
  points over the baseline with at least 20 `signup_completed` events in the
  window.
- With fewer than 20 signups, report the result as too small to mean anything
  and extend the window by 28 days rather than claim a change.
- Revert if the ratio falls by 5 points or more on at least 20 signups.

## Amendment, 2026-10-06: the link opens "Remote clone URL"

- **Finding.** The A1 link went to `/dashboard/repos/new`, which opens on
  "New repository". The form's fourth tab is labelled "GitHub / GitLab /
  Codeberg" and imports provider metadata only (issue #678): it creates no
  mirror and fires no `first_mirror_created`. Someone following "Mirror one
  from GitHub, GitLab or Codeberg" was likely to pick that tab.
- **Change (A3).** The link is now `/dashboard/repos/new?source=remote`.
  `prepareNewRepoForm` presses the tab named by `source` when the form offers
  one and keeps the markup's default otherwise. On the served stack "Remote
  clone URL" is the browser import from the `HOSTED_IMPORT_HOSTS` hosts.
- **Measurement.** The public `repos` board's 4 owners on 2026-10-04 were the
  three core nodes and the operator's account, so outside activation started
  at zero visible public repositories. The account total stayed at 235, so 20
  signups in 28 days is out of reach. A1, A3 and next A2 are read as one
  bundle from A3's deploy date with the same decision rule, and the result
  names every change in the bundle.
- **Event gap.** The browser create and import path (`hosted_git.py`) wrote
  its catalog row without emitting `first_mirror_created`, so the primary
  measure on `/admin/acquisition` could not see the path A1 promotes. The
  same branch emits it there. Until that is deployed, read "New accounts
  activated" on `/product-analysis` (catalog-based) as the primary measure.

## Limitations

- Signups from the EU/EEA, or with DNT or GPC set, have no attribution
  session (`docs/attribution.md`), so their first repository is not joined to
  their signup on `/admin/acquisition`. The `/product-analysis` figure counts
  every account and is the cross-check.
- `first_mirror_created` fires on an owner's first published repository in
  the catalog publish path; a repository kept private and never published may
  not count.
- The form opened on "New repository"; a visitor who wanted to mirror still
  had to choose "Remote clone URL". Preselecting it (A3) was first held back
  so the two changes were not measured together, then shipped on 2026-10-06
  (amendment above).
- The public account total fell from 240 to 235 between 2026-09-16 and
  2026-10-04, so 20 signups in 28 days is not assured; the decision rule's
  extension is the likely outcome of the first window.
- Since 2026-10-06 the result is a bundle's (A1, A3, then A2, the
  verify-pending next step), not a single change's; it cannot say which
  change moved the ratio.
