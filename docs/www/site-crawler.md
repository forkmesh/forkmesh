# Crawler, the website inspection special agent

Part of [the website docs](README.md). Crawler reads the live site the way a
visitor and a search engine do, sorts what it finds into errors (broken),
warnings (likely problems) and notices (improvements), traces each one to the
repository file that renders the page, files a tracker issue for each
confirmed error, and reports the rest as a ranked list. It never changes the
site or the code.

## Where the code is

| Part | File |
| --- | --- |
| Special agent | `.forkmesh/workflows/site-crawler.json` (id `site-crawler`, name Crawler, avatar `crawler`) |
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
  colo ([README.md](README.md), "Security"). The default is 6 requests a
  second across all workers. A 429 halves the pace and waits for
  `Retry-After` (capped at 30 s, up to three tries); a URL still limited is
  counted as not checked, never as broken.
- **User-Agent.** `ForkMeshSiteCheck/1.0 (+https://forkmesh.com)`. The
  request block rules answer 403 to a User-Agent containing "crawler" (and,
  in a test, to one ending in `/robots.txt`), so the tool cannot call itself
  a crawler.
- **Bot challenges.** `/signup` answers 403 with `cf-mitigated: challenge`
  to any non-browser client. Such a response, and an external 401, 403, 405,
  429 or 999, is counted as unverifiable rather than broken.
- **Time.** `--time-budget` (default 480 s) and `--max-pages` (600) stop the
  crawl; the report then says truncated and how many URLs were not checked.
  The playbook runs a full crawl in a 600 s foreground command.
- **Scripts.** It reads HTML as served and runs no JavaScript. An anchor or
  element a script adds is reported missing, which is why `broken-anchor` is a
  warning and the playbook verifies before filing.
