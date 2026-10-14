# Community-reviewed contextual placements

Approval means disclosed public evidence met an instance's current criteria
for a limited period; it is not an endorsement and can be appealed, suspended,
or expire. Criteria are served by `GET /api/world/community-ads/criteria`.
Proposals disclose sponsor, ownership, product, data use, tracking, labor,
accessibility, and impact evidence, with at least two public HTTPS sources.

Voting is one eligible verified account per proposal (active, email-verified,
at least seven days old), never wallet-weighted. Conflicted voters may only
abstain. Approval needs quorum and two-thirds of non-abstaining votes; review
deadlines are at most 180 days.

Each instance is disabled by default and chooses placement contexts and a
public revenue purpose. Placements receive only a coarse page context such as
`town-square` or `repository`; targeting does not use profiles, device or
country data, search terms, wallets, or behavioral history. Responses carry
`tracking: none`. Member text ads are a separate system,
[ForkMesh Ads](operations/ad-network.md); a community placement takes
priority.

Receipts use the `community_ad_revenue` ledger class. They are not user funds,
rewards, or on-chain transfers; the Worker does not custody money.
