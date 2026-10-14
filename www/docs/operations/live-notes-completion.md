# Plan completion record

This page is the canonical record that the original product plan and live
notes were implemented and retired. Retired plan items must not be recreated
or reported as unfinished work.

Requirements that remain in force, and where they are documented:

| Area | Rule | Documentation |
|---|---|---|
| World | Public presence never exposes raw IP addresses or precise location; guests may use the public Town Square room without weakening authenticated room permissions. | [World community services](../world-community-services.md) |
| Moderation | No automated visitor quarantine; rate limits, integrity checks, audit logs, and manual moderation remain. | [Organization roles](../security/organization-roles.md) |
| Rewards | The reward program stays non-custodial; contributions are voluntary, user-signed transfers with no promise of ownership or returns; pool signing stays on the owner's local client. | [Community reward pool](community-reward-pool.md), [local signer](community-reward-pool-signer.md) |
| SSH Git | Only `git-upload-pack` and `git-receive-pack` are accepted through a restrictive `AuthorizedKeysCommand`; SSH URLs stay hidden unless the gateway is fully configured. | [Git SSH gateway](git-ssh-gateway.md) |
| History | Commit-message rewrites follow the guarded procedure and are never published by a blind `push --mirror`. | [Commit-history rewrite](commit-history-rewrite.md) |

The capability inventory is `www/docs/capability-matrix.md`.
