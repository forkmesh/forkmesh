# ForkMesh feature definition of done

A feature is complete only when its implementation and the evidence below are
reviewable together. This applies to application code, infrastructure, protocol
changes, and visible World behavior.

## Required evidence

1. **Threat review.** Identify trust boundaries, sensitive inputs, authority
   changes, failure behavior, and likely attacker goals. State why the feature
   fails closed where authorization, identity, custody, or repository privacy is
   uncertain.
2. **Abuse and privacy review.** Record the data collected, its visibility,
   retention and deletion behavior, consent controls, moderation/appeal path,
   and abuse cases. Raw IP addresses, wallet secrets, private repository
   existence, and private agent data must not become public observability data.
3. **Benchmark and budget evidence.** Run the applicable performance budget or
   provide a reproducible measurement. A feature with no relevant hot path may
   say so, but must explain why.
4. **Regression-test evidence.** Add tests for the success path, denied path,
   malformed input, and material privacy or authorization boundary. Record the
   exact commands and results.
5. **Operational evidence.** Document deployment, configuration, rollback,
   monitoring, recovery, and any external operation that was intentionally not
   performed.

The pull-request template exposes these five sections. CI rejects a pull request
when a section is missing or contains only the template placeholder.

## Running the gates

From the repository root:

```bash
python3 tools/quality_gate.py validate
python3 tools/quality_gate.py run --profile core
python3 tools/quality_gate.py run --profile release
```

The `core` profile is the fast local and CI gate. The `release` profile also
runs the real-browser World suite and therefore requires its pinned Playwright
dependencies and Chromium installation. Native Qt, iOS and Android suites remain
required when their toolchains are available; the completion report must name
any unavailable toolchain rather than silently treating it as passed.

For a desktop change, also build the `forkmesh` app target
(`./desktop/run.sh` or `cmake --build desktop/build --target forkmesh`). The
desktop workflow (`.forkmesh/desktop-ci.yml`) builds only the critical test
targets; `forkmesh-window-tests` among them compiles the `MainWindow*.cpp`
units, but no CI job compiles `desktop/src/main.cpp` or links the app. See [docs/desktop/build-and-test.md](../../../docs/desktop/build-and-test.md).

CI runs the core profile on pushes, pull requests, a weekly schedule, and manual
dispatch. The separate World browser workflow runs the browser portion on every
push and pull request, including the blocking responsive daily-interface timing
and accessibility journey documented in
[`daily-interface-validation.md`](daily-interface-validation.md).

## Changing a budget

Improving a maximum needs no exception. Removing a budget or raising a maximum
is a weakening and must be explicit:

1. Add a JSON record under `www/docs/engineering/quality-gate-waivers/` using the
   budget ID as the file name.
2. Include every field listed in `quality-gates.json`, a concrete risk review,
   a UTC expiry date, and the approving reviewer identity.
3. Pass the base commit to
   `python3 tools/quality_gate.py compare --base-ref <sha>`.
4. Replace the waiver with a restored or tighter budget before it expires.

The comparison command rejects silent removals or increases. Waivers are an
auditable temporary exception, not a way to make a failing test green.
