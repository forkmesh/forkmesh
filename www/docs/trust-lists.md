# Trust lists

A trust list is a signed list of keys its owner vouches for or warns about, each
entry scoped to one role and one place (`repo:`, `org:` or `community:`). There
is no global list; following is local, private and optional, and nothing basic
(read, clone, sign in, push, recover) needs a list. Protocol:
[Scoped community trust lists](design/scoped-trust-lists.md).

Follows can only narrow a list. Included lists count inside the overlap of their
scopes, up to four deep. Disagreements show as conflict until a local override
(`trust`, `distrust`, `ignore`). Distrusted keys may sign an appeal. Lists
should name an offline recovery key; versions last at most 400 days. States are
Active, Recovered, Expired, Revoked, Conflicted and Missing.
`python3 tools/forkmesh_trust_lists.py drill` replays a key-compromise recovery.

Resolvers sharing one set of vectors: `app/src/trust_lists.py`,
`app/public/trust-lists.js`,
`native/ios/ForkMeshCore/Sources/FMAccount/TrustLists.swift`,
`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/TrustLists.kt`
and `desktop/src/TrustLists.cpp`.
