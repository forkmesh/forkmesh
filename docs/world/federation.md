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

## CORS

The owner answers CORS only for a neighbour's World page origin (an entry's
`world` in its own directory, never itself) and only on the federated World
endpoints (`FEDERATED_PATHS` in `world/src/world_mesh.py`): the exchange,
the directory and neighbour scene, public World reads (organizations, events,
repositories, leaderboards) and the office's floors, attendance, status and
visitor entry. The answer echoes that exact origin, never `*` with
credentials, never `access-control-allow-credentials`, and allows the
`authorization`, `content-type` and `accept` headers. Everything else keeps
the same-origin rules.

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
  dropped to a plain visa only for an owner that answers 404 to the
  exchange. Calls carry `Authorization: Bearer`, `credentials: "omit"` and
  `mode: "cors"`, and only paths in `FEDERATED_PATHS` (the same list as the
  server's) are ever sent. Reads are cached per owner and path for `maxAge`.
- **Routing.** `fetchJSONAt(x, z, path)` sends a call for something standing
  at a point to that point's owner (`worldMeshOwnerAt`), or to this World
  when the ground is its own. A building's runtime is given the `fetchJSON`
  of the owner of the hexagon it stands on. `visitorSession(world)` is the
  identity shown there (`name@home`).
- **Visits.** The visit socket asks the federation for its credential:
  `?visitor=<token>`, or `?visa=` for an owner without the exchange.
- **Owner-only actions** (administration, an owner's private rooms) stay
  read-only for visitors, with a note, instead of failing. *(planned)*

## Rollout

Each step works with either side ahead:

- A World without `/api/world/visa/session` answers 404; the walker keeps
  the one-minute visa on the visit socket (today's behaviour).
- A World without federated CORS cannot be read cross-origin; its hexagons
  keep the logo or the published scene.
- Old visit sockets with `?visa=` keep working beside `?visitor=`.
