# Email: October product update (EM1)

**Owner:** Marketing
**Written:** 2026-10-07
**Status:** prepared. Needs E1 deployed, D8, D9 and the operator's approval
**Plan:** [../plan.md](../plan.md), campaign EM1 and "Email channel"

## Brief

| Field | Value |
| --- | --- |
| Audience | Accounts that turned on Product updates and offers. On 2026-10-07 that is nobody: the opt-in is new and off by default |
| Problem | People signed up and never published a repository (0 non-operator public repositories, plan "Weekly values") |
| Message | A public repository from GitHub, GitLab or Codeberg becomes a cloneable second home from the browser, and branches clean up after merged pull requests |
| Channel | Admin console → Marketing email, type Product update |
| Call to action | Mirror a first repository (the dashboard's empty state, which A1 and A3 already improved) |
| Success measure | Clicks, then signups and `first_mirror_created` from `utm_source=email`, `utm_campaign=product_update`, `utm_content=2026-10` |
| Baseline | 0: first send on the channel |
| Start | The day the gates pass |
| Review | 7 days after the send |

With fewer than 50 recipients, report counts, not rates.

## Gates

1. E1 is merged and deployed, and the console's Marketing email section opens.
2. D8: `MARKETING_EMAIL_POSTAL` is set (the console shows "Set").
3. D9: Sentinel has reviewed the consent wording.
4. The two posts linked below answer 200.
5. The operator receives the test send, checks it in Gmail (inbox, not spam;
   the footer links work) and approves it.

## Tagged link

```
https://forkmesh.com/blog/runs-that-listen-branches-that-let-go/?utm_source=email&utm_medium=email&utm_campaign=product_update&utm_content=2026-10
```

## Draft

**Subject:** Your code, a second home: what's new in ForkMesh this month

**Message:**

> Hi from ForkMesh. Here is what changed this month, in under a minute.
>
> Mirror from the browser. Paste the URL of a public repository on GitHub,
> GitLab or Codeberg into the dashboard and ForkMesh keeps a copy anyone can
> read and clone, no desktop needed.
>
> Branches that let go. Merging a pull request on the hosted service now
> cleans up its branch, and a public repository can be merged and updated
> from the browser while your desktop is off.
>
> Ride the coaster. The World's town has a roller coaster. It is live, and
> you can ride it signed in or as a guest.
>
> If you have not published a repository yet, the first mirror takes about
> a minute: open your dashboard and choose "Remote clone URL".

**Button label:** Read the update

**Button link:** the tagged link above.

Before sending, check each line against the posts and against what is
deployed. Drop any line whose feature is merged but not live.
