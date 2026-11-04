# Engineering

Rules that apply to every part of the codebase: what a finished feature must
show, the quality gates and budgets CI enforces, how destructive actions are
guarded, how the GitHub mirror's checks are watched, and how changes reach the
changelog. The rules agents must follow are summarised in `AGENTS.md`; these
pages explain them.

| Page | Covers |
| --- | --- |
| [Definition of done](definition-of-done.md) | The evidence every feature needs, the quality-gate commands, changing a budget |
| [Daily interface validation](daily-interface-validation.md) | The responsive dashboard journey test, its budgets and the release usability gate |
| [Destructive actions](destructive-actions.md) | The guarded delete helpers and the contract test that enforces them |
| [GitHub Actions](github-actions.md) | Watching the mirror's workflows, readable failures, rules for browser tests |
| [Changelog](changelog.md) | The one changelog file, adding a note, where the apps show it |
| [Quality-gate waivers](quality-gate-waivers/README.md) | Where waiver records for a weakened budget live |

Data files: `quality-gates.json` (gate profiles and budgets, read by
`tools/quality_gate.py`) and `daily-interface-budgets.json` (the daily
interface limits). Third-party dependencies are reviewed on
[Third-party dependencies](../security/dependency-security-audit.md).

Related core pages: [architecture](../architecture.md) and
[security](../security/README.md). Back to the
[documentation index](../README.md).
