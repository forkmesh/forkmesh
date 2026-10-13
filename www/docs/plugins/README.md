# Planned plugins

These pages list the features we plan to move out of the ForkMesh core and
into optional Marketplace packages. They come from the repo-wide plugin
decomposition audit in issue #677, which looked at 1,742 tracked code files
(686 of them tests). Nothing here is shipped yet. Each package family gets its
own issue once it is scheduled.

Here, **plugin** means a `.fmpkg` package with a manifest: it declares its
entrypoints and permissions, owns its own state, removes everything when
uninstalled, and has migrations that can be rolled back. It does not mean a
library loaded at runtime.

The audit comes in two modes:

| Mode | Page | What it answers |
| --- | --- | --- |
| Stingy | [Plugin shortlist](shortlist.md) | What we recommend building: 12 package families and 10 host socket families, plus the test a candidate must pass, when to stop, and the boundaries that stay in core. |
| Generous | [Plugin opportunity inventory](opportunities.md) | What could be built: 316 candidate packages across desktop, App, World, mobile, mirror, CLI/MCP/operations, web, content, and portable skills, plus the 15 package boundaries that already exist and 44 proposed host sockets. |

Use the shortlist to choose what to build next. Use the inventory to check
whether an idea has already been considered, and which host socket it would
need.

## Next step

Start with one family. The World element trio is the cheapest. Land its socket
with install, activate, uninstall, upgrade, rollback, and crash-restart tests
before building a general version.

The Marketplace package format and lifecycle are described in
[`marketplace/README.md`](../../../marketplace/README.md). The per-candidate
keep-or-extract review from issue #653 is
[`docs/marketplace-extraction-inventory.md`](../../../docs/marketplace-extraction-inventory.md).
