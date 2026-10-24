# ForkMesh feature definition of done

A feature is complete when its code and the following evidence can be reviewed
together. The pull-request template has one section per item. The GitHub mirror's CI
(`python3 tools/quality_gate.py pr-evidence`) rejects a pull request with a
missing or placeholder section; on ForkMesh the reviewer checks it.

1. **Threat review.** Trust boundaries, sensitive inputs, authority changes,
   failure behaviour. Authorization, identity, custody and repository privacy
   fail closed when uncertain.
2. **Abuse and privacy review.** Data collected, visibility, retention,
   deletion, consent, moderation and abuse cases. Raw IP addresses, secrets,
   private repository existence and private agent data never become public
   observability data.
3. **Benchmark and budget evidence.** The applicable performance budget, or a
   reproducible measurement, or a reason there is no hot path.
4. **Regression tests.** Success, denied, malformed-input and privacy or
   authorization boundary cases, with the commands and results.
5. **Operational evidence.** Deployment, configuration, rollback, monitoring
   and recovery.

## Running the gates

```bash
python3 tools/quality_gate.py validate
python3 tools/quality_gate.py run --profile core
python3 tools/quality_gate.py run --profile release
```

`core` is the fast local and CI gate. `release` adds the real-browser World
suite (pinned Playwright and Chromium). Native Qt, iOS and Android suites are
required where their toolchains exist; a missing toolchain is reported, never
counted as a pass. Desktop changes must also build the `forkmesh` app target,
since CI only builds the test targets. The responsive daily-interface journey is
described in [`daily-interface-validation.md`](daily-interface-validation.md).

## Changing a budget

Budgets live in `quality-gates.json`. Tightening needs no exception. Removing a
budget or raising a maximum requires a waiver:

1. Add a JSON record under `www/docs/engineering/quality-gate-waivers/` named
   after the budget ID, with every field `quality-gates.json` lists, a risk
   review, a UTC expiry date and the approving reviewer.
2. Run `python3 tools/quality_gate.py compare --base-ref <sha>`; it rejects
   silent removals and increases.
3. Restore or tighten the budget before the waiver expires.
