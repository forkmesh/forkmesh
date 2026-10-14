# ForkMesh product promise: proof and measurement

> ForkMesh lets developers collaborate on, mirror, and preserve source-code repositories across independent hosts—without relying on any single hosting provider.

## Four-step proof

Use a disposable public repository; never delete the source.

### 1. Collaborate

Create a signed issue, discussion or pull request.

### 2. Add an independent mirror

Mirror it on another node until it is online, integrity `ok` and at the same commit.

### 3. Take the source mirror offline

Stop `forkmesh-mirror-node` on the source host.

### 4. Clone again

`git clone https://forkmesh.com/OWNER/REPOSITORY` must succeed at the same commit.

## Expected failures and recovery

**No eligible mirror**, a stale or integrity-mismatched mirror, or
**All serving hosts offline** make the clone fail cleanly; a
**Private repository** is outside this proof. Rollback is explicit: restart
`forkmesh-mirror-node` and wait for fresh signed health.

## Measurement and closure gate

Consented analytics count only `home-proof-opened`, `proof-clone-copied` and
`repository-clone-copied`. In every five-person cohort of maintainers,
contributors, and operators, each reader must restate the promise. A cohort passes
at 4/5 or better. Do not mark this gate complete with blank, synthetic, or inferred results.

## Linked automated evidence

`app/tests/test_installer_release.py`, `app/tests/test_mirror_serving.py`,
`app/tests/test_https_mirror_routing_integration.py`,
`app/tests/test_mirror_service_recovery.py`, `app/tests/test_product_promise.py`,
`app/tests/test_first_party_analytics.py`.
