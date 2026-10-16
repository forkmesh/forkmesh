# Press kit

`https://forkmesh.com/press`. Template `www/templates/pages/press.html`,
rendered to `www/public/press.html` with `python3 app/tools/site_templates.py`
([../www/templates.md](../www/templates.md)). Tests:
`app/tests/test_press_page_frontend.py`, `www/tests/test_desktop_and_press.py`.

## Sections

| Section | Content and source |
| --- | --- |
| Hero | Short boilerplate, logo download, press contact, link to the product promise |
| Logo | `assets/logo.svg`, `assets/logo.png` (895×1024) |
| Style guide | Name, voice, fonts (from `www/public/styles.css`) |
| Colors | Website background `#090909` and green `#4ade80` (`www/public/styles.css`); app background `#010409`, foreground `#f0f6fc`, primary blue `#4493f8`, create green `#238636`, status green `#3fb950` (dark values in `app/public/dashboard/theme.json`, with their roles) |
| Fast facts | Category, focus, public beta, MIT License (`LICENSE`), desktop platforms (`/desktop`), mobile (`/mobile`), web dashboard |
| How it works | Mirrors, clone URL, failover, Ed25519 identity, agents (`/features`); three self-hosted core nodes and a runner tier (`docs/PLAN.md`) |
| Press contact | `founders@forkmesh.com`, the address the site already uses for press; `security@forkmesh.com` for security reports |
| Screenshots | Images already on the site: `assets/desktop-mockup/desktop-app.webp`, `assets/mobile/repos.webp`, `assets/mobile/review.webp`, `assets/everywhere/web.webp`, `assets/social-card.jpg`, `assets/logo-96.png` |
| Boilerplate | Short, medium and long versions |

The BetaNYC "coming soon" panel was removed: it had no link or date to show.
Coverage goes back on the page as a "Coverage" panel once there is a real
link. The page names no people; the About page covers the founders.

## Rules

- Only facts the site or the repository already states. No metrics, user
  counts or quotes.
- A test fails when a `/assets/` link on the page names a missing file.
