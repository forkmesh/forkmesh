# Distribution: "Ride the ForkMesh coaster" (World, 2026-10-07)

**Written:** 2026-10-07, on the operator's request to post the coaster and
share it on the ForkMesh channels
**Status:** Discord announcement posted 2026-10-07; a repost with `@everyone`
is requested; Mastodon and X not recorded as posted

## The post

An announcement of the World roller coaster merged on main in `8bc2f549da`
(branch 1720) and served since revision `466bd43da3de`. It will live at
`https://forkmesh.com/blog/forkmesh-coaster/`. The hero is the operator's own
screenshot of a ride on the lift hill, cropped to 16:9 and scaled to
1672x941; the operator's avatar picture in the top-right corner was cropped
out. It was then restyled with Codex image generation (golden-hour sky, glowing
rails, speed streaks; the HUD, name tag and toolbar painted out) and the
ForkMesh logo composited top-left. Figures in the post come from `world/public/world/scene/coaster.js`:
a sampled lap of 74.8 s and a top speed of 214 km/h on the HUD (59.4 u/s ×
3.6), and the render cost from `docs/world/README.md` ("ForkMesh coaster").

## Brief

| Field | Value |
| --- | --- |
| Audience | People who follow ForkMesh on Mastodon, X or Discord, and Discord members who have never opened the World |
| Message | There is a roller coaster round the World's town; it is live and anyone can ride it, signed in or as a guest |
| Channels | Mastodon and X from the ForkMesh account; Discord announcements (Hearth). Not Reddit or Hacker News: a feature announcement is self-promotion there |
| Call to action | `https://world.forkmesh.com/`, through the post |
| Success measure | Replies, reposts and likes on the three posts after 7 days. The World does not record attribution, so no signup figure is claimed |

## Gate before posting

The post is merged and deployed: `https://forkmesh.com/blog/forkmesh-coaster/`
answers 200 and its image loads.

## Drafts

Posted from the ForkMesh account, so the affiliation is the account itself.
If anyone shares them from a personal account, add "I work on ForkMesh."

**Mastodon** (under 500 characters):

> The ForkMesh World has a roller coaster now.
>
> It runs right round the town: a 60-unit lift hill, a double loop, three
> airtime hills, a ramp jump into real free fall, a corkscrew, a helix and a
> tunnel. About 75 seconds a lap, and the speedometer passes 210 km/h.
>
> The drone race rings now weave over and under the track, too.
>
> Board at the station on the west side of town:
> https://forkmesh.com/blog/forkmesh-coaster/

**X** (under 280 characters with the link counted as 23):

> The ForkMesh World has a roller coaster: a double loop, a ramp jump into
> free fall, a corkscrew and a tunnel, right round the town. 75 seconds a
> lap, 210+ km/h at the bottom of the first drop.
> https://forkmesh.com/blog/forkmesh-coaster/

Attach the hero image to both, with its alt text from the post.

**Discord announcement** (for Hearth to adapt):

> @everyone 🎢 There's a roller coaster in the World now. Walk to the station on the
> west edge of town, click the train (or press E next to it) and hold on:
> double loop, a ramp jump, a corkscrew and a tunnel. Write-up:
> https://forkmesh.com/blog/forkmesh-coaster/ and the ride itself:
> https://world.forkmesh.com/. If anything looks off on the way round, tell
> us in #bugs.

## Result

| Channel | Posted | Where | Replies, reposts, likes or reactions at 7 days |
| --- | --- | --- | --- |
| Discord | 2026-10-07, by Hearth | `#announcements`, message `1557524161138786434` | Count on 2026-10-14 |
| Discord repost | Not posted. Requested by the operator on 2026-10-07, with `@everyone` | `#announcements`, message id to be added if posted | Count on 2026-10-14, added to the row above |
| Mastodon | Not recorded | | |
| X | Not recorded. Herald asked on 2026-10-09 for this row once the post goes out; it is filled in from the post's URL and date | | Count 7 days after posting |

The 7-day window for the Discord announcement runs 2026-10-07 to 2026-10-14;
the count is replies in the channel or its thread plus reactions on both
Discord messages, with any person counted once per message. The Mastodon and
X windows start on the day each is posted.

On 2026-10-07 the operator asked that every Discord announcement mention
`@everyone` and that this one be reposted with it. Hearth's `post` tool
now allows `@everyone` in an `--announce` post to `#announcements` and nowhere
else (`docs/community/hearth.md`). Hearth's own instructions still say never
to use `@everyone`, so the repost waits on the operator changing that line and
asking Hearth directly, or on the operator posting it by hand.
