# Brand assets: press kit and images

Part of [Marketing](README.md). This page covers the press kit page and the
brand images the site and blog reuse, where each one's source lives and how to
re-render it.

## Press kit page

`https://forkmesh.com/press`. Template `www/templates/pages/press.html`,
rendered to `www/public/press.html` with `python3 app/tools/site_templates.py`
([../www/templates.md](../www/templates.md)). Tests:
`app/tests/test_press_page_frontend.py`, `www/tests/test_desktop_and_press.py`.

| Section | Content and source |
| --- | --- |
| Hero | Short boilerplate, logo download, press contact, link to the product promise |
| Logo | `assets/logo.svg`, `assets/logo.png` (895×1024) |
| Style guide | Name, voice, fonts (from `www/public/styles.css`) |
| Colors | Website background `#090909` and green `#4ade80` (`www/public/styles.css`); app background `#010409`, foreground `#f0f6fc`, primary blue `#4493f8`, create green `#238636`, status green `#3fb950` (dark values in `app/public/dashboard/theme.json`, with their roles) |
| Fast facts | Category, focus, public beta, MIT License (`LICENSE`), desktop platforms (`/desktop`), mobile (`/mobile`), web dashboard |
| How it works | Mirrors, clone URL, failover, Ed25519 identity, agents (`/features`); three self-hosted core nodes and a runner tier (`docs/operations/served-stack.md`) |
| Press contact | `founders@forkmesh.com`, the address the site already uses for press; `security@forkmesh.com` for security reports |
| Screenshots | Images already on the site: `assets/desktop-mockup/desktop-app.webp`, `assets/mobile/repos.webp`, `assets/mobile/review.webp`, `assets/everywhere/web.webp`, `assets/social-card.jpg`, `assets/logo-96.png` |
| Boilerplate | Short, medium and long versions |

Rules:

- Only facts the site or the repository already states. No metrics, user
  counts or quotes.
- No coverage or "coming soon" panel until there is a real coverage link;
  then it goes on the page as a "Coverage" panel.
- The page names no people; the About page covers the founders.
- A test fails when a `/assets/` link on the page names a missing file.

## Agents-context image

`www/public/assets/agents-context.svg` (source) and `agents-context.png`
(render) are a 1920×1080 brand image with the line "ForkMesh gives your agents
context". It draws ForkMesh as a tree of light: the logo is the crown, the core
nodes are the trunk, and the branches reach the desktop app, web dashboard,
World, issues and pull requests, agents, runners, the iOS and Android apps and
the Marketplace. The roots are Git and the mirrors, since they keep a
repository alive. The forest, moon, leaves and fireflies add nature to a neon
perspective grid and aurora.

The blog post `forkmesh-gives-your-agents-context` (October 19, 2026) uses it
as its hero, converted to
`www/public/assets/blog/features/forkmesh-gives-your-agents-context.webp`.

The SVG loads its fonts from `assets/fonts/`, so render it from where it
lives. To refresh the PNG after editing the SVG:

```sh
chrome-headless-shell --no-sandbox --hide-scrollbars --window-size=1920,1080 \
  --virtual-time-budget=3000 --screenshot=www/public/assets/agents-context.png \
  "file://$PWD/www/public/assets/agents-context.svg"
```

Playwright installs `chrome-headless-shell` under `~/.cache/ms-playwright/`.
Escape `&` as `&amp;` in labels; otherwise the XML does not parse and Chrome
renders an error banner instead of the image.
