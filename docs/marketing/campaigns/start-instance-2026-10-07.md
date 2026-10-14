# Campaign brief: "Start your own ForkMesh"

**Campaign tag:** `start_instance`
**Plan:** [../plan.md](../plan.md)

Announces the white-label "Start your own instance" page at
`https://app.forkmesh.com/start` (`forkmesh.com/start` redirects there) with a
blog post at `https://forkmesh.com/blog/start-your-own-forkmesh/`. Every
claim comes from [../../INSTANCES.md](../../INSTANCES.md) or the page's own
copy.

## Brief

| Field | Value |
| --- | --- |
| Audience | Developers and small teams who would rather host their own Git service |
| Message | Give the start page a VPS and an admin account, and it sets up Git hosting, repositories, chat and agents while you watch every step |
| Channels | Blog, Mastodon, X and the Discord announcements channel; community sites only within their self-promotion rules |
| Call to action | The post's links to `/start`, tagged (below) |
| Success measure | Visits to `/start` with `utm_campaign=start_instance` in Analytics (Marketing → Performance), and the post's own visits |
| Baseline | 0: the tag is new |
| Duration | 14 days from the day every gate passes; reviewed by the next Marketing run after the end |

## What is and is not measured

- **Analytics** counts the tagged visits: `/start` and the blog post both
  load `/analytics.js`. Only visitors who allow analytics are counted, so
  compare campaigns with each other, not with signups.
- **`/admin/acquisition` does not see `/start`.** It is a page shell that
  `app/wrangler.toml` excludes from `run_worker_first` (`!/start/`), so it is
  served straight from assets and the attribution hook never runs
  ([../../attribution.md](../../attribution.md)). `start_instance` is still on
  `ATTRIBUTION_ALLOWED_CAMPAIGNS` in `app/wrangler.toml` and
  `DEFAULT_ALLOWED_CAMPAIGNS` in `app/src/attribution.py`, as every brief's
  links must be.
- **Launches are not attributed.** `instance_launches` keeps no session or
  campaign, and the page sends no Analytics event when a run starts.

## Tagged links

```
https://app.forkmesh.com/start?utm_source=blog&utm_medium=referral&utm_campaign=start_instance&utm_content=start-your-own-forkmesh
https://forkmesh.com/blog/start-your-own-forkmesh/?utm_source=mastodon&utm_medium=social&utm_campaign=start_instance
https://forkmesh.com/blog/start-your-own-forkmesh/?utm_source=x&utm_medium=social&utm_campaign=start_instance
```

## Drafts and gates

Social drafts link the post and stay within the page's claims. Posting waits
on these gates:

1. The post and `/start` are deployed and answer 200.
2. A person reviews and posts each draft from the ForkMesh account.

The Marketing package offers this campaign as a template (Marketing 1.22.0
and later).
