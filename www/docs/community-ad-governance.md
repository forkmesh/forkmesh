# Community-reviewed contextual placements

ForkMesh calls these placements “community-reviewed,” not “ethical company”
badges. Approval means disclosed public evidence met an instance’s current
criteria for a limited period. It is not a universal endorsement and it may be
appealed, suspended, or expire.

The machine-readable criteria are returned by
`GET /api/world/community-ads/criteria`. Proposals disclose the sponsor,
beneficial-ownership context, product, data use, tracking, labor,
accessibility, environmental and community-impact evidence. At least two
distinct public HTTPS sources are required.

Voting is one eligible verified account per proposal, never wallet-weighted.
Accounts must be active, email-verified, and at least seven days old. Voters
declare conflicts; a conflicted voter may only abstain and publish a
disclosure. Approval needs the proposal quorum and at least two-thirds of
non-abstaining votes. Records preserve evidence provenance, moderation,
appeals, and a review deadline no later than 180 days.

Each independently operated instance is disabled by default and explicitly
selects placement contexts and a public revenue purpose. Placements receive
only a coarse page context such as `town-square` or `repository`; ForkMesh does
not use profiles, browser/OS/country badges, search terms, repository privacy,
wallets, or behavioral history for targeting. Every response says
“Community-reviewed placement” and `tracking: none`.

Member-written text ads reviewed by an instance's administrators are a
separate system, [ForkMesh Ads](operations/ad-network.md). A community-reviewed
placement always takes priority over one where both could appear.

Advertising receipts use the dedicated `community_ad_revenue` ledger class.
They are not user funds, pending rewards, completed on-chain transfers, or the
community reward pool. A revenue record references an external accounting
event; the Worker does not custody or transfer the money.
