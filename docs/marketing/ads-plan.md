# Where ForkMesh should advertise first

Part of [the marketing docs](README.md). Written 2026-10-16. The Marketing
package's **Where to advertise** panel (Ads manager) re-ranks these channels
from live numbers as visits, conversions and logged ads come in; this page
is the reasoning behind its starting order and the plan for the first tests.

## Who we are buying attention from

ForkMesh is a privacy-first, self-hostable Git hosting and collaboration
product with a desktop app. Its buyers are developers, self-hosters and
open-source maintainers who:

- already search for alternatives to GitHub, GitLab and Gitea, or for ways to
  mirror and back up repositories;
- gather in a handful of communities (r/selfhosted, r/homelab, r/git,
  r/opensource, Hacker News);
- read developer newsletters and documentation sites, and block or distrust
  tracking ads.

Our analytics are consent-gated and first-party, so we cannot use
retargeting pixels, and we should not want to: that rules out most of the
value of Meta, LinkedIn and display networks.

## Ranking

| # | Channel | Why | First test |
| --- | --- | --- | --- |
| 1 | **Google Search ads** | The highest intent we can buy: people typing "self-hosted git", "github alternative", "gitea alternative", "git mirror", "backup github repositories" are already shopping. | Exact and phrase match on 10–20 of those terms, one ad group per intent, 20–50 USD a day for four weeks. Land on the homepage (where the hero experiment runs) or `/desktop`. |
| 2 | **Reddit Ads** | The audience is concentrated in a few subreddits, clicks are cheap, and the Reddit desk already tells us which subreddits react to us. | Promoted posts in r/selfhosted, r/homelab, r/git, r/opensource, r/programming, written like a post and disclosed, 15–30 USD a day for two weeks. |
| 3 | **Developer sponsorships** | Privacy-friendly developer ad networks (EthicalAds, Carbon) and newsletter placements (selfh.st, Console.dev, TLDR) reach developers where they read, with no tracking, which matches what we sell. | One newsletter issue or one week of EthicalAds, each with its own `utm_content`, then compare cost per install intent with search. |
| 4 | Microsoft Advertising | Same intent as Google at lower volume; it also serves DuckDuckGo, which our audience uses. | Import the Google campaign once it converts. |
| 5 | YouTube creator sponsorships, X Ads | Demos build trust, but clicks are few and intent is low. | Only after 1–3 show a cost per install intent we can afford. |
| 6 | LinkedIn, Meta, TikTok | Broad interest targeting, expensive clicks, low intent, and they rely on the pixels we do not run. | Not now. |

## What has to be true before spending

The **Ad readiness** checklist on the same page tracks these:

1. Analytics receives visits (only visitors who allow analytics are counted;
   plan on measured numbers being a fraction of clicks).
2. Conversion goals exist. The Analytics website playbook adds *Install
   intent*, *Signup intent* and *Account created*
   ([analytics conversions](../app/analytics.md#conversion-tracking)); add
   the same events as Marketing goals so campaign results show conversions.
3. A landing page converts without ads.
4. A campaign with a tag holds the ads, so every link carries one
   `utm_campaign`, built with the link builder (`utm_medium=cpc` for search,
   `paid_social` for Reddit and X, `sponsorship` for newsletters and
   creators).
5. A budget is set on the planned ad.
6. A landing-page experiment is running: the homepage hero headline test
   (`home-hero-headline`) is the first one.

## How to judge the tests

Compare channels on **cost per install intent** and **cost per account
created** from the Website page's campaign rows, not on clicks or CTR. Give
each test at least 100 measured visits before deciding; below that the
package marks numbers as a small sample. Log spend and results on each ad
weekly so the ranking learns from them: a channel that spends with no
conversions drops down the list, one that converts rises.
