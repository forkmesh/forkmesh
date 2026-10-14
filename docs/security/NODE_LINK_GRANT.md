# Node link grants: both parties consent

`POST /api/accounts/link-grant` (`_account_link_grant` in `app/src/entry.py`)
moves a node account to a new owner. A node changes owner only when both
sides prove themselves in one request:

1. **The node** signs a grant with its account key.
2. **The new owner** accepts with its own credentials: an authenticated
   session (cookie, `sessionToken` or `Authorization: Bearer`) or a signature
   from one of its signing keys (account `pubkey` or an enabled `owner_sign`
   device key).

The owner is always the accepting account; a body `user` is only a claim
checked against it. App tokens (`fmat_…`) are not accepted as a session.

## Grant forms

| Form | Canonical string (`\n`-joined) | Names the owner | Who may accept |
| --- | --- | --- | --- |
| v1 | `forkmesh-link-grant-v1`, node, ts | no | the session account; a body `user` must equal it |
| v2 | `forkmesh-link-grant-v2`, node, user, ts, nonce | yes | only `user`, by session or signed accept |

Signed accept (body fields `user`, `acceptTs`, `acceptSig`); it includes the
grant signature, so it cannot be moved to another grant:

```
forkmesh-link-accept-v1
<node>
<user>
<grant sig>
<acceptTs>
```

`nonce` is 16–64 characters of `[A-Za-z0-9_-]`.

## Freshness and replay

- A grant is valid from 1 minute before its `ts` to 5 minutes after
  (`LINK_GRANT_MAX_FUTURE_MS`, `LINK_GRANT_MAX_AGE_MS`); a signed accept uses
  the ±5-minute login window.
- Each redeemed grant is burned in `link_codes` (`grant:<node>:<ts>:<sig>`
  for v1, `grant2:<node>:<nonce>` for v2). A second redemption answers
  `409 grant_used`, unless the node is already owned by the acceptor
  (`alreadyLinked`, nothing written).

## Errors

| Error | Status | Meaning |
| --- | --- | --- |
| `bad_signature` | 401 | node signature does not verify (v2: includes a different `user`) |
| `grant_expired` | 401 | `ts` outside the window |
| `session_required` | 401 | no session and no signed accept |
| `accept_expired` | 401 | `acceptTs` outside the window |
| `bad_accept_signature` | 401 | accept does not verify with the account's keys |
| `grant_user_mismatch` | 403 | the grant or body names another account |
| `no_such_user`, `not_a_user` | 404, 403 | acceptor missing, inactive or a node account |
| `grant_used` | 409 | already redeemed |

## Audit

Every decision after a valid node signature writes `node.link_grant` to the
sensitive audit log (success with `{grant, via, from, to}`, or denied with
`{reason, grant, named, from}`). Requests with an invalid node signature are
not audited, so unauthenticated callers cannot fill the log.

## Clients

| Client | Flow |
| --- | --- |
| Desktop (`openLinkNodeInBrowser`) | signs a v1 grant and opens `/dashboard?link_node=…`; the browser session accepts |
| Web dashboard (`redeemLinkGrant`) | redeems with the signed-in session |
| `tools/next_node_owner.py` | signs a v2 grant on the node; the target accepts with its key or session |
| Go daemon, native apps | do not call link-grant |
