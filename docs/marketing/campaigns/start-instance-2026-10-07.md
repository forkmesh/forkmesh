# Campaign: "Start your own ForkMesh" (start page, 2026-10-07)

**Written:** 2026-10-07, on the operator's request for a blog post about the
new start page and a marketing campaign linked to it
**Status:** prepared. Posting waits on the gates below and the operator's
go-ahead
**Plan:** [../plan.md](../plan.md), campaign SI1
**Campaign tag:** `start_instance`

## The post

An announcement of `https://app.forkmesh.com/start` (`forkmesh.com/start`
redirects there), the white-label "Start your own instance" page merged on
main in `9a10be9b9b` and served on 2026-10-07: the live page matched
`app/public/start.html` apart from its `?v=` asset stamps. It will live at
`https://forkmesh.com/blog/start-your-own-forkmesh/`. Every claim in it comes
from [../../INSTANCES.md](../../INSTANCES.md) or the page's own copy ("about
fifteen minutes" is the page's estimate, not a measurement).

The hero is a screenshot of the live page, not generated art. The headless
browser on the operator's box has no network (its app firewall), so the page
and its CSS, scripts, fonts and logo were fetched with curl and rendered from
disk at 1254x706 with a 4/3 device scale factor, giving 1672x941, then saved
as lossy WebP.

## Brief

| Field | Value |
| --- | --- |
| Audience | Developers and small teams who would rather host their own Git service; ForkMesh followers on Mastodon, X and Discord who have not tried a server of their own |
| Problem | Running your own ForkMesh used to mean the Cloudflare and Vultr launcher or a manual install; now it is one form, but nobody outside the repository knows the page exists |
| Message | Your name, your server, your domain: give forkmesh.com/start a VPS and an admin account and it sets up Git hosting, repositories, chat and agents while you watch every step |
| Channels | Blog; Mastodon and X from the ForkMesh account; Discord announcements (Hearth). Not Hacker News: readiness is blocked. Not Reddit for now: r/selfhosted is the obvious fit, but its self-promotion rules have not been read on the Reddit desk (D4) |
| Call to action | The post's links to `/start`, tagged (below) |
| Deliverables and owners | Post, hero and tests (this branch); commit and merge (operator review); Mastodon and X posts (operator posts the drafts below); Discord announcement (Hearth); post URLs written back into the post's `data-blog-social` block (Blogger); the campaign added in the ForkMesh organization's Marketing page from its template (operator or Marketing) |
| Success measure | Visits to `/start` with `utm_campaign=start_instance` in Analytics (Marketing → Performance, row `start_instance (blog / referral)`), and the post's own visits from `utm_campaign=start_instance` social links; replies, reposts and likes on the posts after 7 days |
| Baseline | 0: the tag is new |
| Start | The day every gate passes (the template's planned flight is 2026-10-08 to 2026-10-21 UTC) |
| End | 14 days after the start |
| Follow-up review | The next Marketing run after the end date |

No visit or launch target is set: there is no earlier reading of `/start`
traffic to base one on. Set them on the campaign after the first 7 days.
With fewer than 10 tagged visits, report the result as too small to mean
anything.

### What is and is not measured

- **Analytics** counts the tagged visits: `/start` and the blog post both
  load `/analytics.js`. Only visitors who allow analytics are counted, so
  compare this campaign with others, not with signups.
- **`/admin/acquisition` does not see `/start`.** It is a page shell that
  `app/wrangler.toml` excludes from `run_worker_first` (`!/start/`, and
  `/start` is on no include rule), and the Next host honours that list, so
  the page is served straight from assets and the attribution hook never
  runs ([../../attribution.md](../../attribution.md)). `start_instance` was
  still added to `ATTRIBUTION_ALLOWED_CAMPAIGNS` in `app/wrangler.toml` and
  `DEFAULT_ALLOWED_CAMPAIGNS` in `app/src/attribution.py`, as every brief's
  links must be, so a tagged visit that does reach the hook (a later signup
  on the same session) is not stored as `other`.
- **Launches are not attributed.** `instance_launches` keeps no session or
  campaign, and the page sends no Analytics event when a run starts or
  finishes. Counting launches per campaign needs one of those added.

## Tagged links

The post (lede and closing paragraph):

```
https://app.forkmesh.com/start?utm_source=blog&utm_medium=referral&utm_campaign=start_instance&utm_content=start-your-own-forkmesh
```

Mastodon and X posts link the post:

```
https://forkmesh.com/blog/start-your-own-forkmesh/?utm_source=mastodon&utm_medium=social&utm_campaign=start_instance
https://forkmesh.com/blog/start-your-own-forkmesh/?utm_source=x&utm_medium=social&utm_campaign=start_instance
```

`blog`, `mastodon`, `x`, `referral` and `social` are already on the
`ATTRIBUTION_ALLOWED_*` lists. Do not use the template's **Copy link** for
social posts: it tags `utm_source=forkmesh`, which is not on the source list.

## In the Marketing package

Marketing 1.22.0 ships the campaign as a template
(`START_INSTANCE_TEMPLATE` in `content/worker/marketing.py`): name, tag
`start_instance`, objective `launch`, status `planned`, the post as its
landing page and source, channels blog, Mastodon, X and Discord, and a
six-step checklist. The ForkMesh organization's Campaigns panel shows it as a
template card until a campaign from that post exists; **Add this campaign**
creates it. Posts and ads that name the campaign or carry the tag then join
it ([package README](../../../marketplace/packages/marketing/README.md#campaigns)).

## Gates before posting

1. The post is merged and deployed:
   `https://forkmesh.com/blog/start-your-own-forkmesh/` answers 200 and its
   image loads.
2. The tagged `/start` link answers 200 and shows the form.
3. Marketing 1.22.0 is deployed, and the campaign is added for the ForkMesh
   organization from its template.

## Drafts

Posted from the ForkMesh account, so the affiliation is the account itself.
If anyone shares them from a personal account, add "I work on ForkMesh."

**Mastodon** (under 500 characters):

> You can now run ForkMesh under your own name, on your own server.
>
> Give forkmesh.com/start a project name, a Debian or Ubuntu VPS and an admin
> account. It connects over SSH, installs Git hosting, sets up HTTPS (on
> sslip.io if you have no domain) and your first repository, and shows every
> step live.
>
> Your SSH password or key is used once and never stored.
>
> https://forkmesh.com/blog/start-your-own-forkmesh/?utm_source=mastodon&utm_medium=social&utm_campaign=start_instance
>
> #SelfHosted

**X** (under 280 characters with the link counted as 23):

> Run ForkMesh under your own name on your own server: give forkmesh.com/start
> a VPS and an admin account, and it sets up Git hosting, HTTPS and your first
> repository while you watch each step.
> https://forkmesh.com/blog/start-your-own-forkmesh/?utm_source=x&utm_medium=social&utm_campaign=start_instance

Attach the hero image to both, with its alt text from the post.

**Discord announcement** (for Hearth to adapt):

> 🛠️ You can now start your own ForkMesh: your project's name, your server,
> your domain. Fill in five short sections at https://forkmesh.com/start and
> watch it set up Git hosting, HTTPS and your first repository on your VPS.
> Write-up: https://forkmesh.com/blog/start-your-own-forkmesh/. If a step
> fails or the page confuses you, tell us in #bugs.

## Result

Pending.
