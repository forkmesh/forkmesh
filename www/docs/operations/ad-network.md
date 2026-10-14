# ForkMesh Ads

ForkMesh Ads is a first-party, text-only ad network. Members write short ads,
a person reviews every one, and approved ads run on a product surface: the web
app, the website, the desktop app, the mobile apps, and World.

Three rules govern it:

1. **Placement is by surface, never by viewer.** The serve call takes only a
   surface name; no session, account, location, or history is read, and the
   network never learns who saw an ad.
2. **A person approves the exact words that run.** Automated screening only
   raises flags.
3. **The [prohibited list](#what-is-never-accepted) always applies**, and any
   signed-in member can report an ad.

Approval is not an endorsement and expires after 180 days. This network is
separate from [community-reviewed placements](../community-ad-governance.md)
and does not change the [business-model contract](../business-model.md):
personal data is never sold or used for advertising.

## Who can advertise

Any verified account can submit ads. There is no ad billing or bidding; an
active membership level (read from `role_grants`, see [Patreon](patreon.md))
funds live slots:

| Level | Ads running at once | Rotation weight per ad |
| --- | --- | --- |
| Free | 0 | — |
| Supporter | 1 | 1 |
| Pro | 2 | 2 |
| Builder | 5 | 5 |
| Studio | 10 | 10 |

Ads beyond the allowance wait; without a membership they pause. An account may
hold 25 ads and 5 pending review. The numbers live in the `advertising` block
of [`business-model.json`](/business-model.json) and are pinned by
`app/tests/test_ad_network.py`.

## What an ad is

A headline (60 characters), optional body (160), call-to-action label (24),
sponsor name (48), one public `https://` destination, one category, surfaces,
and an optional schedule. No images, scripts, embeds, or HTML. Clients render
plain text, label the slot **Sponsored**, and open the destination with
`rel="sponsored noopener noreferrer"`. Link shorteners and cross-site click
identifiers (`gclid`, `fbclid`, ...) are refused; `utm_*` tags are allowed.

Allowed categories cover developer tools, open source, hosting and
infrastructure, defensive security and privacy, education, events, hiring,
accessibility, nonprofits, climate, open hardware, independent software, and
community projects.

## What is never accepted

Gambling; tobacco, alcohol, and drugs; weapons; adult content; promised returns
or speculative tokens; predatory credit; unproven health claims; tracking, data
brokers, and stalkerware; malware or offensive hacking services; political
persuasion; hate or harassment; misleading or dark-pattern claims; illegal
goods; exploitative labor; anything aimed at children; destinations that
fingerprint, force downloads, or redirect through trackers. Reviewers cite these
rules by code. `GET /api/ads/policy` serves the machine-readable policy.

## Review

| Status | Meaning |
| --- | --- |
| `draft` | Saved, not submitted |
| `pending` | In the review queue |
| `approved` | May run, subject to membership, schedule, and slots |
| `rejected` | Declined with a reason and cited rules |
| `suspended` | Held by a reviewer; only a reviewer reinstates (back to `pending`) |
| `withdrawn` | Pulled by the owner |

- Reviewers are platform administrators (`users.is_admin = 1`) working at
  `/dashboard/ads`; the server enforces the role on every review route.
- Approval is pinned to the content hash of the words reviewed. An edit during
  review is refused (`creative_changed`); editing an approved ad's words sends
  it back to `pending`. Schedule changes and pauses need no review.
- Decisions are appended to `ad_network_reviews` and the audit log
  (`ad_network.approve|reject|suspend|reinstate`) without ad text. Self-review
  is allowed but recorded as such.
- Five open member reports on a running ad suspend it automatically.

## Serving and measurement

`GET /api/ads/serve?placement=web|www|desktop|mobile|world` returns one ad or
`null`. The pick is weighted by level and rotates on a five-minute bucket with
no randomness and no viewer input, so responses are cacheable
(`public, max-age=300`) and a suspension takes effect within five minutes.

The only measurement is an aggregate click counter per ad, surface, and UTC day
(`ad_network_daily_stats`, via `POST /api/ads/{id}/click`), with no viewer
columns, deleted after 400 days. Impressions are not measured, and nothing is
billed on clicks.

Clients send no credentials to the serve call, render nothing on failure, never
delay the host surface, let users hide a slot for seven days, keep legal and
documentation pages ad-free, and in World request a network ad only when no
community placement exists.

## Operating it

Storage is `ad_network_ads`, `ad_network_reviews`, `ad_network_reports`, and
`ad_network_daily_stats`. Ad text is public; the submitting username is sealed
and shown only to reviewers. Renaming an account moves its ads; deleting it
deletes ads, reports, and counters. The down migration drops the four tables
and touches no membership data. No third-party ad code runs anywhere, ads never
affect ranking or moderation, and self-hosted instances run their own
instance-local network.
