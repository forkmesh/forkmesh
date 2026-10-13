# name: Retire stale code and performance debt
# description: Find one evidence-backed maintenance target, safely remove or repair it, and verify the codebase is simpler or measurably faster.
# agent: codex
# model: gpt-5.6-sol

Complete one focused codebase-maintenance pass. Start from repository evidence and leave a small, reviewable improvement rather than attempting a broad rewrite.

1. Inspect current repository status, recent history, open maintenance issues, build and dependency configuration, tests, diagnostics, and available performance budgets. Look for abandoned or partially integrated features, unreachable or duplicated code, obsolete compatibility paths, unused dependencies, recurring complexity, and measured performance regressions.
2. Choose the highest-value target with strong evidence. Explain its maintenance cost or establish a reproducible performance baseline before editing. Do not optimize based only on intuition or delete code solely because a text search finds no direct caller; account for dynamic loading, reflection, generated code, scripts, packaging, configuration, feature flags, persisted data, migrations, wire formats, plugins, and public APIs.
3. Prefer removing stale code when its obsolescence is proven. Repair incomplete code only when the intended behavior is clear, still required, and a focused fix is lower risk than removal. Preserve supported compatibility paths and never weaken security, reliability, accessibility, observability, audit history, migrations, or test coverage to reduce code size or improve a metric.
4. Keep the change narrowly scoped. Add or update regression tests and benchmarks as appropriate, then run the relevant build, test, lint, and performance checks. Compare performance against the baseline with the same workload and environment. If safe implementation is blocked by uncertain ownership or compatibility, document the evidence and create a specific follow-up issue instead of guessing.
5. Inspect the final diff for unrelated changes and leave a review-ready commit on the branch when a repository change is warranted. Never open a pull request and never merge your own work.

Finish with a concise maintenance report covering: the target and evidence, code or dependencies removed or repaired, technical debt retired, before-and-after performance results when applicable, verification commands and outcomes, compatibility considerations, and the next most valuable cleanup candidate.
