# Daily interface validation

The daily interface has one shared contract for the responsive web surface and
the web content embedded by desktop and mobile shells. Numeric limits live in
[`daily-interface-budgets.json`](daily-interface-budgets.json); changes that
make a limit weaker use the same expiring waiver process as other quality
budgets.

## Automated evidence

`npm run test:daily-interface` exercises the checked-in production dashboard in
Chromium at desktop and mobile viewports. It blocks CI when startup, repository
browse, in-memory search, diff construction, navigation handoff, or perceived
feedback exceed their limits. The journey also verifies:

- keyboard entry through Control/Command+K, roving command selection, Escape,
  focus restoration, repository-card activation, and skip navigation;
- semantic dialog, combobox, listbox, status, main, and navigation landmarks;
- visible focus, light/dark contrast, 200% zoom reflow, 44 CSS-pixel coarse
  pointer targets, and global reduced-motion behavior;
- readable unified diffs with non-color line signs, labelled line-comment
  controls, virtualized large-file layout, and announced copy feedback;
- empty, API-error, offline/retry, and recovery paths without losing the page;
- page-local performance events that carry only operation, duration, budget,
  source class (`local` or `remote`), and pass/fail. They contain no account,
  repository, route, search query, or content and are not persisted.

Run it locally after installing the pinned browser dependencies:

```bash
cd app/browser_tests
npm ci
npx playwright install --with-deps --only-shell chromium
npm run test:daily-interface
```

CI retains the Playwright report for 30 days even on failure. The static
`app/tests/test_daily_interface_contract.py` suite keeps the runtime constants,
workflow, documentation, and central quality-gate registry in sync.

## Representative-user release gate

Automation is necessary but does not substitute for representative users. Each
public-alpha candidate must link a dated, consented study report covering at
least two keyboard-only participants, two screen-reader participants, two
touch/mobile participants, and two pointer users. A participant may belong to
more than one cohort, but results stay disaggregated by access need.

Use a fresh supported build and a seeded repository. Do not collect repository
contents, assistive-technology speech logs, IP addresses, or participant
identity in telemetry. Record only consented aggregate duration, completion,
error count, and a 1–5 satisfaction score.

| Task | Target time | Maximum user errors | Required completion |
| --- | ---: | ---: | ---: |
| Browse to and open a file | 45 seconds | 1 | 90% |
| Copy a clone command | 45 seconds | 1 | 90% |
| Find and open an issue | 60 seconds | 1 | 90% |
| Review a diff and submit feedback | 120 seconds | 2 | 85% |
| Inspect mirrors and recover from offline | 90 seconds | 2 | 85% |

A release owner links the test run, cohort summary, supported platform matrix,
rollback/recovery drill, current user documentation, and privacy-reviewed
telemetry snapshot from the release checklist. Missing or placeholder evidence
means the adoption gate remains open.

## Rollback and recovery

The command palette and local measurement trail are additive and keep no server
state. Rollback is the previous static dashboard asset set. If catalog loading
fails, the page remains operable, exposes a semantic status, and offers an
explicit retry that bypasses cached data. Copy failures preserve selectable
commands and announce the manual fallback. Theme selection and cached catalog
data remain local and can be cleared by the existing sign-out/browser-state
cleanup path.
