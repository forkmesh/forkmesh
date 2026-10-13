# Trust lists

A trust list is a signed list of people. Its owner vouches for them, or warns
you about them, in a specific role and place. Examples:

- the security researchers whose advisories a project's maintainers stand
  behind;
- the reviewers a maintainer relies on for one repository;
- the mirror operators an organization has worked with;
- the accounts a community's moderation group has flagged.

You choose which lists to follow. ForkMesh does not ship a global list and
follows nothing on your behalf. Trust is never universal: an entry always
names one role, such as `reviewer`, and one place, such as
`repo:forkmesh/app`, `org:forkmesh`, or `community:rust`. An organization
scope covers that organization's repositories. Nothing covers everything.

The protocol, verification order, and threat model are in
[Scoped community trust lists](design/scoped-trust-lists.md).

## What does not need a list

You never need to follow a list to read or clone a repository, sign in,
recover your identity, push, fork, or open an issue or pull request. Lists
only add information, such as review suggestions, badges, and moderation
queue hints. If you follow nothing, ForkMesh works exactly as it does
without trust lists.

## Following and narrowing

Following a list is local to your device. It is not published, and
ForkMesh's usage counts never include which lists you follow. When you
follow a list you can narrow it further: for example, only its `reviewer`
entries, and only for `repo:forkmesh/app`. Narrowing can only shrink what a
list says, never widen it.

Lists can include other lists, and an owner can hand part of a list to a
trusted maintainer until a date. Included entries only count inside the
overlap of every scope along the way. Composition stops after four lists and
ignores loops.

## When lists disagree

If one list you follow trusts a key as a security researcher for
`org:forkmesh` and another distrusts it, ForkMesh shows **conflict** and lists
both sources. It never picks a winner for you. To settle it on your device,
add a local override (`trust`, `distrust`, or `ignore`) for that key, role,
and place. Overrides stay on your device, and the conflict stays visible with
a note that you overrode it.

## Appeals

If a list distrusts you, you can sign an appeal with your own key, naming the
list version and entry. The appeal appears next to that entry for everyone
who follows the list. It shows as **resolved** once a newer version of the
list no longer distrusts you. The list owner decides what their list says,
and your appeal makes sure followers can see your side.

## List states

| State | What you see | What to do |
| --- | --- | --- |
| Active | The list's newest version applies | Nothing |
| Recovered | Something went wrong with newer versions (a revocation, two conflicting copies of one version, or a rewritten history), so ForkMesh uses the last good version | Read the reason; if a successor list is shown, decide whether to follow it |
| Expired | The newest version's expiry passed; older versions are never revived | Wait for the owner to publish a new version, or unfollow |
| Revoked | The owner (or their recovery key) withdrew every version | Unfollow, or follow the successor if you trust it |
| Conflicted | Conflicting copies left no usable version | Unfollow until the owner publishes a clean successor |
| Missing | You follow a list but have no verified copy yet | Check your connection or mirror |

## Publishing a list safely

- Give every list at least one **recovery key**, kept offline and separate
  from the signing key. Recovery keys named in a version can revoke any
  later version, which is how you recover from a stolen signing key.
- Keep lists short-lived. A version can last at most 400 days. Publish
  regular updates rather than long-lived lists.
- Keep scope tight. A list cannot contain entries outside its own roles and
  places, and a client rejects the whole version if it tries.

## Drill: compromised list

Run this drill before relying on a list:

1. **Compromise.** A stolen signing key publishes a new version that
   distrusts a real maintainer and trusts the attacker. Followers who have
   not yet seen a revocation accept it, because the signature is valid.
2. **Revoke.** With a recovery key named in an earlier version, publish a
   revocation from the malicious version onward, with reason `compromised`.
   Point it at a successor list signed by a new key.
3. **Recover.** As soon as a follower has the revocation, their client shows
   the list as **Recovered** at the last good version. The malicious entries
   disappear, and the successor is offered but not followed automatically.

The shared vectors replay this drill on every client. Operators can replay it
with:

```bash
python3 tools/forkmesh_trust_lists.py drill
```

It prints the before and after states, the number of assertions removed,
and how long local recovery took, and exits non-zero if recovery did not
happen. `tools/forkmesh_trust_lists.py resolve <bundle.json>` and
`verify <bundle.json>` inspect any bundle of documents offline.

## Evidence and telemetry

Every client resolves the same documents to the same result:

| Client | Resolver | Test |
| --- | --- | --- |
| Web | `app/public/trust-lists.js` | `app/tests/test_trust_lists.py` (Node) |
| Worker and tools | `app/src/trust_lists.py` | `app/tests/test_trust_lists.py` |
| iOS | `native/ios/ForkMeshCore/Sources/FMAccount/TrustLists.swift` | `native/ios/ForkMeshCore/Tests/FMAccountTests/ConformanceTests.swift` |
| Android | `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/TrustLists.kt` | `native/android/core/src/test/kotlin/com/forkmesh/nativeapp/security/TrustListsCryptoTest.kt` |
| Desktop | `desktop/src/TrustLists.cpp` | `forkmesh-trust-list-tests` |

Each resolution includes a `telemetry` object of counts only: lists by state,
revocations, rejected documents by reason, conflicts, overrides, and appeals
by status. It never includes keys, list names, or follows.

## Not yet available

Trust lists are resolved and verified on every client. The following are
still being built: publishing and fetching lists through relays and mirrors,
creating and signing lists in the apps, and settings screens for follows and
overrides.
