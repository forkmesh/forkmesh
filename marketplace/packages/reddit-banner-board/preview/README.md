# Reddit banner board preview

An orange-framed social-circle board showing `r/forkmesh`, the latest
normalized subreddit posts, refresh countdown, and posting-cadence state.
Selecting the board opens the community subreddit in a guarded new tab.

The package performs no network requests directly. ForkMesh World supplies the
public, edge-cached feed snapshot through the declared host capability.

`market.png` provides the required visual preview for the unified Market list.

The package ships with `install.default: false` (since 2026-09-23): it stays
listed in the World Marketplace, and World imports its code only after a
visitor installs it.
