# Campaign briefs

Part of [Marketing](../README.md). A campaign brief is a file in this folder
that pins what a campaign's copy may claim, its tagged links, the gates before
posting and the drafts. Marketing writes one per campaign and lists it under
Campaigns in [the plan](../plan.md); Herald and Blogger read the open ones.

| Brief | Campaign tag | Covers |
| --- | --- | --- |
| [start-instance-2026-10-07.md](start-instance-2026-10-07.md) | `start_instance` | "Start your own ForkMesh": the white-label start page and its blog post |
| [repo-mirrored-2026-10-09.md](repo-mirrored-2026-10-09.md) | `repo_mirrored` | "Is your repo mirrored?": copying a public repository from the browser |
| [plugin-race-and-affiliates-2026-10-16.md](plugin-race-and-affiliates-2026-10-16.md) | `plugin_race` | The Plugin Race and the affiliate program, one hashtag |

Briefs keep their file names: the Marketing package's campaign templates
(`marketplace/packages/marketing/content/worker/marketing.py`) and
`www/tests/test_mirror_page.py` cite them by path.

## Brief outline

Name the file `<slug>-<yyyy-mm-dd>.md`. Every brief has:

- **Header.** Campaign tag, landing pages, tracking hashtag if any, and a link
  to the plan.
- **Brief table.** Audience, problem, message, channels, offer or call to
  action, deliverables, owners, success measure with its baseline, start and
  end dates, and the follow-up review.
- **Claims and their evidence.** What the copy may say with the code or doc
  behind it, and what it must not say. Security and privacy claims follow the
  charter's [guardrails](../README.md#guardrails-and-why).
- **Tagged links.** UTM-tagged links whose campaign is on
  `ATTRIBUTION_ALLOWED_CAMPAIGNS` in `app/wrangler.toml` and
  `DEFAULT_ALLOWED_CAMPAIGNS` in `app/src/attribution.py`
  ([attribution](../../app/attribution.md)). Landing pages keep their own
  links untagged so the post stays the first touch.
- **What is and is not measured**, when a page or event escapes
  `/admin/acquisition`.
- **Gates before posting**, with a dated status for each; a person posts every
  draft.
- **Drafts.** A draft a test reads sits between `<!-- <id>:start -->` and
  `<!-- <id>:end -->` markers in a `text` block.
- **Follow-up and review**, then the **Result**, written when the campaign
  ends.
