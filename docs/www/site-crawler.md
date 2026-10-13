# Crawler, the website inspection special agent

Part of [the website docs](README.md). Added 2026-10-07 (branch
`1735-special-agent-crawls-website`): the user asked for a special agent that
crawls forkmesh.com and reports a list of improvements and any issues.

Crawler reads the live site the way a visitor and a search engine do, sorts
what it finds into errors (broken), warnings (likely problems) and notices
(improvements), traces each one to the repository file that renders the page,
files a tracker issue for each confirmed error, and reports the rest as a
ranked list. It never changes the site or the code.

## Where the code is

| Part | File |
| --- | --- |
| Special agent | `.forkmesh/teammates/site-crawler.json` (id `site-crawler`, name Crawler, avatar `crawler`) |
| Playbook | `.forkmesh/shortcuts/crawl-the-website.md` |
| Skill | `.forkmesh/skills/website-inspection.skill` |
| Crawler | `tools/site_crawler.py` (standard library only) |
| Portrait | `desktop/resources/agent-ui/icons/teammate-crawler.png`, phone crops `teammate-face-crawler` (iOS) and `teammate_faces/crawler.jpg` (Android) |
| Tests | `app/tests/test_site_crawler_special_agent.py` (a fake site, no network) |

The agent has `read-repositories`, `manage-issues`, `external-tools` and
`run-unattended`, and no code, branch, merge or payout permission. It runs
weekly on Monday at 09:00 once its schedule is switched on; it ships paused.

## The crawler

```sh
python3 tools/site_crawler.py crawl                     # whole site, text report
python3 tools/site_crawler.py crawl --out report.json   # also save the JSON report
python3 tools/site_crawler.py crawl --include /docs/ --start https://forkmesh.com/docs/
python3 tools/site_crawler.py crawl --external          # also check links to other sites
python3 tools/site_crawler.py page https://forkmesh.com/desktop
python3 tools/site_crawler.py compare old.json new.json # new and resolved findings
```

It reads `robots.txt` (and obeys it), seeds the crawl with every URL in the
sitemap, follows same-site links (`forkmesh.com` and `www.forkmesh.com`),
and checks every other link and asset once: first-party hosts such as
`app.forkmesh.com` always, other sites only with `--external`. Assets get a
`HEAD`, falling back to a short `GET`. Link findings are grouped per target,
so a broken link in shared chrome is one finding with its page count.

| Check | Severity | Meaning |
| --- | --- | --- |
| `broken-link`, `broken-asset` | error | a first-party link, image, script, stylesheet or canonical answers 4xx/5xx or does not connect |
| `sitemap-url-error` | error | a sitemap URL fails |
| `missing-title` | error | no `<title>` |
| `broken-anchor` | warning | `#fragment` is not an id on the target page |
| `broken-external-link` | warning | another site's link fails (only with `--external`) |
| `missing-description`, `missing-h1`, `missing-lang`, `missing-viewport`, `image-missing-alt`, `empty-link-text` | warning | SEO and accessibility basics |
| `duplicate-title`, `duplicate-description` | warning | indexable pages that share one |
| `missing-security-header` | warning | HSTS, CSP, `X-Content-Type-Options`, `Referrer-Policy` or clickjacking protection missing on HTML pages |
| `mixed-content`, `soft-404`, `sitemap-url-redirects`, `sitemap-url-noindex`, `robots-missing`, `sitemap-missing`, `slow-page` (over 3 s) | warning | |
| `link-redirects` | notice | a same-site link goes through a redirect (usually a missing trailing slash) |
| `title-length` (15–70), `description-length` (50–170), `missing-social-tags`, `missing-canonical`, `canonical-elsewhere` | notice | search and social previews |
| `image-missing-size`, `heavy-page` (HTML over 250 KB), `slow-page` (over 1.5 s) | notice | layout shift and speed |
| `heading-skip`, `multiple-h1`, `generic-link-text`, `duplicate-id`, `insecure-link` | notice | structure and accessibility |
| `missing-from-sitemap`, `orphan-page` | notice | indexable page not in the sitemap; sitemap page no crawled page links to (full crawls only) |

Each finding has `url` (the page), `target`, `detail`, and `source`: the
repository file for the page when the URL maps to one
(`www/templates/pages/…`, `www/docs/…md`, `www/public/…`). Grouped findings
list every page in `pages` and their files in `sources`. `compare` matches
findings by check and target (or page), so a fix shows up as resolved.

## Limits and why

- **Pacing.** The zone rate limit is 120 requests per 10 s per address and
  colo ([README.md](README.md), "Security"), and the operator browses from
  the same address. The default is 6 requests a second across all workers.
  A 429 halves the pace and waits for `Retry-After` (capped at 30 s, up to
  three tries); a URL still limited is counted as not checked, never as
  broken. The first unpaced trial on 2026-10-07 tripped the limit within a
  minute and reported thousands of false 429s.
- **User-Agent.** `ForkMeshSiteCheck/1.0 (+https://forkmesh.com)`. The
  request block rules answer 403 to a User-Agent containing "crawler" (and,
  in a test, to one ending in `/robots.txt`), so the tool cannot call itself
  a crawler.
- **Bot challenges.** `/signup` answers 403 with `cf-mitigated: challenge`
  to any non-browser client. Such a response, and an external 401, 403, 405,
  429 or 999, is counted as unverifiable rather than broken.
- **Time.** `--time-budget` (default 480 s) and `--max-pages` (600) stop the
  crawl; the report then says truncated and how many URLs were not checked.
  A full crawl took 217 s on 2026-10-07; the playbook runs it in a 600 s
  foreground command.
- **Scripts.** It reads HTML as served and runs no JavaScript. An anchor or
  element a script adds is reported missing, which is why `broken-anchor` is a
  warning and the playbook verifies before filing.

## First run, 2026-10-07

262 HTML pages and 1,117 links and assets in 217 s: 175 errors, 8 warnings,
402 notices. The confirmed problems:

- Twenty JSON files linked from the docs page sidebar on 135 pages
  (`/docs/terminology.json`, `/docs/capability-matrix.json`, the
  `*.schema.json` files) 308 to `…json/` and then 404: the
  `/docs/:slug /docs/:slug/ 308` rule in `www/public/_redirects` catches them.
- The docs chrome links `/#roomchat` on 135 pages and the home page links
  `/#membership` on 4; the docs index links `/#install-banner`. None of those
  ids is on the served home page: `roomchat` and `membership` are gone, and
  the install banner is inside an HTML comment.
- `/assets/blog/intro-to-forkmesh.png` (404) is referenced by the home page
  and the hackathon pages.
- The App-rendered profile page `/@forkmesh` loads
  `dashboard-composer.css`, `dashboard-composer.js`,
  `dashboard-plugin-host.js` and `user-status.js` from forkmesh.com, where
  they 404, and has three icon links to `/dashboard/runners` with no text.
- `/docs/history/` links about 150 commit pages on `app.forkmesh.com` that
  404.
- `/docs/protocol/signatures-and-authentication/` links
  `/docs/protocol/#sec-1-3`, which does not exist.

The main improvements: 161 pages without Open Graph tags (144 of them docs
pages, plus about, careers, changelog, desktop and other site pages), 113
descriptions outside 50–170 characters (90 docs pages, 18 blog posts), 102 pages with images lacking width and height, 11
same-site links that go through a trailing-slash redirect (`/docs` from 20
pages), a 330 KB home page, heading skips on `/market` and one blog post, and
three sitemap pages no page links to: `/design`, `/blog/introducing-forkmesh/`
(the blog index shows that post inline instead) and
`/blog/beyond-the-room-key/` (its artwork is used by the encrypted repo rooms
post, but nothing links the post).

## Fixes after the first run

Fixed in the source on 2026-10-07, not yet deployed or re-crawled:

| First-run error | Fix |
| --- | --- |
| Docs JSON files 308 to `…json/`, then 404 | `/docs/:slug` rule removed from `www/public/_redirects`; `auto-trailing-slash` adds the slash to docs pages |
| `/#roomchat` in the docs chrome and the docs index footer | `https://app.forkmesh.com/chat` (`app/tools/docs_site.py`, `www/templates/pages/docs/index.html`) |
| `/#install-banner` on the docs index | `/desktop` (the banner is commented out of the home page) |
| `/@forkmesh` assets 404 | the four files proxied to the App in `www/worker.js` `APP_SUBRESOURCES` and both `run_worker_first` lists |
| `/docs/history/` commit links 404 | linked to `compare/<parent>...<sha>` on the App |
| `/docs/protocol/#sec-1-3` | `#signature-schemes` (section 1.3) |

Not changed: `/#membership` is intended (`app/public/membership-prompt.js`
opens the membership panel for that hash), and
`/assets/blog/intro-to-forkmesh.png` is no longer referenced by any template,
only by a stale local `www/dist` build. The icon-only `/dashboard/runners`
links on the profile page are still open.
