# World federation

Federated hexes let a person walk from one ForkMesh World onto a neighbouring
World's hexagons inside the same page, keep their own name, and use what
stands there. Every World runs the same World code; each hexagon has one
owner, and everything on it is read from, and acted on at, the owner.

This page is the protocol other instances implement to join. Parts marked
*(planned)* are not built yet.

## Who owns a hexagon

`GET /api/world/mesh` lists every World in the mesh with `id`, `name`,
`host`, `world` (the World page's origin), `api` (its API origin), `slot`,
`shape` (`flower` or `hex`) and, for a single hexagon, `scene`. A hexagon
belongs to the entry whose layout places it (`worldMeshSlotOffset`,
`worldMeshOwnerAt`). The page's own World is `self`.

## Identity: visa, then visitor token

1. **Visa (home).** A signed-in walker's page asks its own server for a visa
   for the neighbour: `POST /api/world/visa {destination: <neighbour world>}`
   (same origin, the walker's session). The visa is HMAC-signed by home,
   names the walker, home and destination, and lives one minute.
2. **Exchange (owner).** The page sends it once to the neighbour:
   `POST <neighbour api>/api/world/visa/session {visa}` (cross-origin, no
   cookies). The owner checks that the visa's home is a listed neighbour,
   asks that home to vouch for it (`POST <home api>/api/world/visa/verify`),
   and that the browser call comes from home's World page. It answers
   `{token, expiresAt, name, home}`.
3. **Visitor token.** `fmwv1.<payload>.<sig>`: signed by the owner with its
   session secret under its own label, `aud` = the owner's World origin,
   `scope` = `world`, `sub` = the walker's name, `home` = home's host,
   15 minutes. Only the owner that minted it accepts it. It grants visitor
   rights only: read World data, presence and chat as `name@home`, and
   (planned) visitor entry to rooms. It never reaches the walker's home
   account, and no cookie crosses origins. The page renews it with a new
   visa before it expires.

The token is sent as `Authorization: Bearer fmwv1...` on World API calls to
the owner, and as `?visitor=fmwv1...` on the owner's presence socket
(`wss://<api>/api/world/ws`), where it replaces the one-minute visa, so
reconnects keep the walker's name.

Limits of the design, accepted on purpose:

- The page sends a visa once, but the owner does not record used visas: a
  visa can be exchanged again during its minute. Each exchange is checked
  with home, so it only ever yields a token for the walker the visa names.
- A token cannot be revoked. A walker who signs out at home keeps visitor
  rights at the neighbour until the token's quarter of an hour runs out.
- On the socket the token travels in the URL (`?visitor=`), like `?visa=`
  and `?ticket=` before it.
- Every well-formed visa that reaches the exchange makes the owner call
  home's `visa/verify`, with no rate limit of its own.

A neighbour's page calling a federated endpoint gets `Vary: Origin` merged
into the response's existing `Vary` (`_with_headers` in
`app/src/app_runtime.py`), so caching keys such as `Accept-Encoding` are kept.

## CORS

The owner answers CORS only for a neighbour's World page origin (an entry's
`world` in its own directory, never itself) and only on the federated World
endpoints (`FEDERATED_PATHS` in `world/src/world_mesh.py`): the exchange,
the directory and neighbour scene, public World reads (organizations, events,
repositories, leaderboards, and the town reads a page standing in that World
needs: the member directory `/api/accounts/users`, `/api/world/inactive`,
`/api/world/visitors`, `/api/world/fediverse`, `/api/world/fediverse-mentions`
and `/api/network/overview`) and the office's floors, attendance, status and
visitor entry. The answer echoes that exact origin, never `*` with
credentials, never `access-control-allow-credentials`, and allows the
`authorization`, `content-type` and `accept` headers. The town reads in
`FEDERATED_READ_ONLY_PATHS` get CORS for `GET` (and a preflight for `GET`)
only (`federated_method_allowed`), because `/api/accounts/users` and
`/api/world/inactive` also take writes that must stay same-origin. Everything
else keeps the same-origin rules.

## Presence and chat

The existing mesh links carry presence: a watch socket while a neighbour is
near, and on its land a visit socket that moves the walker's presence to the
owner's relay (`safe.space = visiting` at home). With a visitor token the
visit socket is `?visitor=`; both sides see each other live.

## Rendering and routing

- **Client.** `WorldFederation` (`world/public/world/world-federation.js`)
  keeps one context per owner: its API origin and visitor token. Nothing is
  fetched until something on that owner's ground needs it; the token is
  shared by concurrent callers, renewed a minute before it expires, and
  replaced by a plain visa whenever there is no token (an owner that answers
  404, or one whose exchange failed; see Rollout). Calls carry `Authorization: Bearer`, `credentials: "omit"` and
  `mode: "cors"`, and only paths in `FEDERATED_PATHS` (the same list as the
  server's) are ever sent. Reads are cached per owner and path for `maxAge`.
- **Routing.** `fetchJSONAt(x, z, path)` sends a call for something standing
  at a point to that point's owner (`worldMeshOwnerAt`), or to the World the
  page stands in when the ground is its own; home's ground goes to home with
  the page's own session. While the page stands in a neighbour's World
  ([README](README.md#world-mesh), Standing in another World), the town's
  public reads go to that World without the visitor token. A building's runtime is given the `fetchJSON`
  of the owner of the hexagon it stands on. `visitorSession(world)` is the
  identity shown there (`name@home`).
- **Visits.** The visit socket asks the federation for its credential:
  `?visitor=<token>`, or `?visa=` for an owner without the exchange.
- **The Office.** When the page stands in a neighbour's World,
  `enterMeshWorld()` calls `setOfficeOwner(world)` (and `setOfficeOwner(null)`
  on the way back). `setOfficeOwner(world)`
  hands its runtime (lobby, floors, attendance board,
  meetings, task wall) that owner's `fetchJSON`/`postJSON` and the visitor
  identity. At the owner a visitor token gives the two common floors (lobby
  and rooftop) as `name@home`; the attendance board is readable and a
  visitor's punch is accepted without being recorded; meetings answer
  `members_only` and the page says so (meetings belong to that Office's own
  World; visitors look around). Team floors stay with the owner's members.
  The task wall is read-only for visitors: its calls (`/api/tasks`,
  `/api/world/office/marketing-tasks`, `/api/accounts/...`) are not in
  `FEDERATED_PATHS`, so a visitor session (`visitor: true`) locks the wall
  with "Members of this World only" without calling them, and leaving the
  Office does not post `stop-active` there. A runtime that finishes loading
  after the owner changed is dropped. The owner marker on the runtime is
  never sent: owner calls carry the bearer token.
- **Owner-only actions** (administration, an owner's private rooms) stay
  read-only for visitors, with a note, instead of failing.

## The page's federation client

`world/public/world/world-federation.js` keeps one context per owner: its API
origin and a visitor token minted there from a visa this World issued.
Tokens are fetched lazily, the first time something on that owner's ground
needs one, shared by concurrent callers, cached until a minute before they
expire, and sent as a bearer header with credentials omitted, so no cookie
crosses origins. A visa lives a minute, so the one just tried is reused for
the socket fallback rather than asking again. Reads are cached per owner and
path for `maxAge`, and nothing outside `FEDERATED_PATHS` is ever sent
across. `visitorSession` is the identity the page holds at that owner
(`name@home`), for the owner's UI.

## Interactive neighbour scenes

A neighbour's published scene (`/api/world/neighbour-scene`, v1) is drawn
on its hexagon with its interactions:

- **Portal house** (`house` with an https `url`): solid walls; walking into
  its door, or clicking it, opens the link in the same tab behind a short
  fade (none with reduced motion). The fade names the destination's real
  hostname after the neighbour's label (`Entering <label> · <host>…`), so a
  neighbour cannot pass another site off under a trusted-sounding name.
- **Terrarium** (`terrarium`): glass walls and a lid with an open doorway in
  its front; walkers go in only through it. Each bug is a badge with its
  favicon (through the home World's icon route); clicking it opens a small
  card with the bug's name and its https link.
- **Ground** (`ground.color`, `accent`, `pattern`: `plain` or `circuit`),
  **plaza**, **sign**, **lamp** and **tree** are drawn as before.

Walls are footprints in each building's own frame
(`scene/neighbour-footprints.js`), registered per owner while its scene is
drawn (the network runtime hands them back as each scene loads) and checked
on every walking step. A footprint is a box whose +z face is the building's
front, either solid (a house) or hollow with glass walls (a terrarium), with
a door on that face: a hollow box lets walkers through it, and walking into a
solid box's door is reported to the page, which opens a portal house's link.
Moves are checked along their path and slide along the wall they meet.

## Rollout

Each step works with either side ahead:

- A World without `/api/world/visa/session` answers 404, or refuses the
  cross-origin preflight so the browser reports a network error; either way
  the walker keeps the one-minute visa on the visit socket (today's
  behaviour). `WorldFederation.credential` falls back to the visa whenever it
  has no token, and after a failed exchange it waits 30 s
  (`EXCHANGE_RETRY_MS`) before trying the exchange again, so a signed-in
  walker does not pay a visa and an exchange on every call.
- A World without federated CORS cannot be read cross-origin; its hexagons
  keep the logo or the published scene.
- Old visit sockets with `?visa=` keep working beside `?visitor=`.
