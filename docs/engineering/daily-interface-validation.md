# Daily interface validation

One contract covers the responsive web dashboard and the web content embedded by
the desktop and mobile shells. Numeric limits live in
[`daily-interface-budgets.json`](daily-interface-budgets.json); weakening one
uses the expiring waiver process in
[`definition-of-done.md`](definition-of-done.md).

## Automated check

`npm run test:daily-interface` drives the production dashboard in Chromium at
desktop and mobile viewports. It fails CI when startup, browse, search, diff
construction, navigation or feedback latency exceed their budgets, and verifies
keyboard navigation and focus handling, semantic landmarks, contrast in both
themes, 200% zoom reflow, 44 CSS-pixel coarse-pointer targets, reduced motion,
accessible diffs, and empty, error, offline and retry states.

Performance events stay in the page and carry only operation, duration, budget,
source class (`local` or `remote`) and pass/fail; no account, repository, route,
query or content.

```bash
cd app/browser_tests
npm ci
npx playwright install --with-deps --only-shell chromium
npm run test:daily-interface
```

`app/tests/test_daily_interface_contract.py` keeps the runtime constants,
workflow and quality-gate registry in sync.

What the check relies on:

- Focus outlines come from the shared `:where(...):focus-visible` rules in
  `app/public/dashboard/shell.html`: a 3px ring on buttons and links, and a
  1px primary outline on text fields (`docs/app/dashboard-components.md`).
  Those rules have no specificity, so Tailwind's `focus:outline-none` replaces
  them with a transparent outline. The repository filter no longer carries
  that class, and the check asserts its outline is solid and visible.
- Text on solid `primary` must reach 4.5:1 in both themes
  (`docs/app/dashboard-colors.md`).
- On a coarse pointer every button is at least 44px tall; the header's menu
  button also gets `min-width: 44px`, because its padding and icon only reach
  34px.
- `fetchJson` retries a 5xx twice (400 ms, then 1.5 s) before it gives up, so
  the offline test fails the catalog three times before the error state shows.
- Clone commands use the site the page is served from, not the catalog's
  `cloneUrl`, so the test compares them with the page origin.

## Release usability gate

Each public release candidate needs a consented usability study covering
keyboard-only, screen-reader, touch and pointer users, reported per access need.
Only aggregate duration, completion, error count and satisfaction are recorded.

| Task | Target time | Maximum user errors | Required completion |
| --- | ---: | ---: | ---: |
| Browse to and open a file | 45 seconds | 1 | 90% |
| Copy a clone command | 45 seconds | 1 | 90% |
| Find and open an issue | 60 seconds | 1 | 90% |
| Review a diff and submit feedback | 120 seconds | 2 | 85% |
| Inspect mirrors and recover from offline | 90 seconds | 2 | 85% |

## Rollback

The command palette and measurement trail keep no server state; rollback is the
previous static asset set. If the catalog fails to load, the page stays usable
and offers a retry that bypasses cached data.
