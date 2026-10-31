# name: Retire stale code and performance debt
# description: Find one evidence-backed maintenance target, safely remove or repair it, and verify the codebase is simpler or measurably faster.

Complete one focused maintenance pass. Start from evidence in the repository
and leave a small, reviewable improvement rather than a broad rewrite.

1. Look at the repository's status, recent history, open maintenance issues,
   build and dependency files, tests, logs and any performance budgets. Look
   for abandoned or half-finished features, unreachable or duplicated code,
   obsolete compatibility paths, unused dependencies, recurring complexity
   and measured slowdowns.
2. Pick the most valuable target with strong evidence. Explain what it costs
   to keep, or record a reproducible performance baseline, before editing.
   Do not optimise on intuition, and do not delete code only because a text
   search finds no caller: account for dynamic loading, reflection, generated
   code, scripts, packaging, configuration, feature flags, stored data,
   migrations, wire formats, plugins and public APIs.
3. Prefer removing code whose obsolescence is proven. Repair half-finished
   code only when the intended behaviour is clear, still needed, and a
   focused fix is less risky than removal. Keep supported compatibility
   paths, and never weaken security, reliability, accessibility, logging,
   audit history, migrations or tests to make the code smaller or a number
   better.
4. Keep the change narrow. Add or update tests and benchmarks as needed, then
   run the relevant build, test, lint and performance checks, comparing
   against the baseline with the same workload. If ownership or compatibility
   is too uncertain to proceed safely, record the evidence in a specific
   follow-up issue instead of guessing.
5. Check the final diff for unrelated changes and leave a review-ready commit
   on the branch when a change is warranted. Do not open a pull request or
   merge your own work.

Finish with a short report: the target and its evidence, what was removed or
repaired, before-and-after performance where it applies, the checks you ran
and their results, compatibility notes, and the next most valuable cleanup.
