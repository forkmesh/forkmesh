# Self-hosted instances in the ForkMesh directory and World

Part of [the App docs](README.md) and [../INSTANCES.md](../INSTANCES.md).
Instances started from `forkmesh.com/start` are written into the directory by
the launch itself. An instance installed directly with
`www/public/install-instance.sh` was never seen by forkmesh.com, so
`GET /api/start/instances` did not know it. This page describes how such an
instance joins the directory (the homepage list and a World hexagon), and
leaves it, on its operator's choice.

Code: `app/src/instance_registry.py` (protocol, both sides),
`app/src/instance_directory.py` (tables, list, health),
`app/src/instance_launch.py` (route), `app/src/instance_console.py`
(Platform → ForkMesh directory, the hourly announce), `app/src/app_runtime.py`
(`/.well-known/forkmesh-instance.json`, `/api/instance`). Tests:
`app/tests/test_instance_registry.py`.

## The choice (instance side)

The `instance_settings` row `directory` holds the admin's choice and the
outcome of the last announcement:

```json
{"listed": true, "world": true,
 "status": {"state": "listed", "at": 1800000000000, "slot": 3,
            "id": "<32 hex>", "worldHome": "https://world.forkmesh.com/world/",
            "message": "", "sent": {"listed": true, "world": true}}}
```

- **Install setting** `"directory": {"listed": true, "world": true}` (each
  optional, booleans only). `instance-bootstrap` writes the row once
  (`ON CONFLICT DO NOTHING`); `--update` ignores the setting, so it never
  overrides the admin.
- **Admin console**, Platform → ForkMesh directory: two switches (homepage
  listing, World hexagon), **Save and announce**, and **Announce now**. Both
  posts are CSRF-checked and audited (`admin.console_save_directory`,
  `admin.console_announce_directory`, with the choice and the result). The
  page shows the last result: listed (and on which hexagon), not listed, or
  the failure message forkmesh.com returned.
- Without the row, nothing is announced and the document below is not found.

## The protocol

1. **Document.** While the row exists, the instance serves
   `GET /.well-known/forkmesh-instance.json` (`no-store`):

   ```json
   {"forkmeshInstance": 1, "domain": "code.example.org",
    "url": "https://code.example.org/", "name": "Acme Code",
    "logo": "/instance-logo.png", "listed": true, "world": true,
    "worldUrl": "https://code.example.org/world/",
    "publicKey": "<base64url Ed25519 public key, 43 chars>"}
   ```

   `publicKey` is the instance's router identity
   (`MIRROR_ROUTER_PUBLIC_KEY`, made by the installer with its seed and kept
   across updates). `worldUrl` is set only when the World is chosen and the
   `world` plugin is on. `logo` is the brand logo path the installer wrote
   (`INSTANCE_LOGO`).
2. **Announcement.** The instance posts to
   `POST https://app.forkmesh.com/api/start/instances/register`:

   ```json
   {"domain": "code.example.org", "ts": 1800000000000,
    "nonce": "<32 hex>", "signature": "<base64url Ed25519, 86 chars>"}
   ```

   The signature covers the bytes
   `forkmesh-instance-directory-v1\n<domain>\n<ts>\n<nonce>` and is made with
   the router seed (`ed25519_sign`).
3. **Checks on forkmesh.com**, in this order, each refusing with a JSON
   `{"ok": false, "error", "message"}`:
   - only the directory serves the route (a branded instance answers 404);
   - body at most 2 KB, a valid domain that is not forkmesh.com or a
     subdomain, `ts` within ten minutes of forkmesh.com's clock, a 32-hex
     nonce and an 86-character signature (400);
   - rate limits counted in `instance_register_attempts` before any fetch:
     30 attempts per client (salted hash of the address, as for launches) and
     12 per domain per hour (429); failures count too;
   - `ts` must be newer than the last accepted one for the domain (409
     `replayed`);
   - the domain must resolve to public addresses only (400), and the host's
     egress guard checks the address again at connect time;
   - forkmesh.com fetches the document from `https://<domain>` itself
     (redirects not followed, 8 s, 16 KB max). A missing, non-200 or
     malformed document, or one naming another domain, is refused and
     changes nothing;
   - the signature must verify with the document's `publicKey` (401).

   Domain control is proven by HTTPS: only whoever serves the domain decides
   what the document says. The signature proves the request came from the
   holder of the key that document names, so a third party cannot make
   forkmesh.com act on someone else's behalf, and the per-domain `ts` makes
   each announcement single-use. A replay cannot change anything either: the
   result always follows the live document.
4. **Result.** With `listed` or `world` true, forkmesh.com fetches the logo
   from the same origin (200 KB max, type checked from the bytes with the
   launch's `logo_kind`; a bad logo is left out, the instance is still
   listed), writes the `instance_directory` row through
   `instance_directory.publish` (the id is kept across announcements, a free
   World slot is assigned when the World is chosen) and the
   `instance_registrations` row (key, `worldUrl`, last `ts`). The answer is
   `{"ok": true, "status": "listed", "id", "slot", "listed", "world",
   "hidden", "worldHome"}`. With both false it deletes both rows and answers
   `{"ok": true, "status": "removed"}`.
5. **Afterwards** the entry is like a launched one: `/api/start/instances`
   lists it, the `instance-health` cron probes `https://<domain>/readyz`
   every 15 minutes and hides it after two misses, and the admin console's
   Instances view shows "Joined from its own console" with the last
   announcement. Hide survives announcements (the upsert never touches
   `hidden`; the answer reports `hidden: true` and the instance shows "Hidden
   by a forkmesh.com admin."). Delete removes the registration as well, so a
   self-registered instance comes back on its next announcement.

Tables: migration `0273_instance_registrations.sql`
(`instance_registrations`, `instance_register_attempts`), also created by
`instance_directory.ensure`.

## Announcing again

The cron phase `instance-announce` (hourly at minute 32, catalogued in
`cron_activity.py`) runs `instance_registry.announce_if_due` on a branded
instance. It announces when the choice differs from the last one sent, when
the last announcement failed more than 50 minutes ago, six hours after the
last success (so a new name or logo reaches forkmesh.com), and after an
opt-out until forkmesh.com confirms the removal. Saving in the console
announces at once.

## Reaching forkmesh.com from an instance

The host's egress guard refuses every `*.forkmesh.com` name so that a staging
stack never calls production. An instance's app unit now sets
`FORKMESH_EGRESS_PRODUCTION_ALLOW=app.forkmesh.com`
(`egress.production_allowed`), and `app.env` carries
`INSTANCE_DIRECTORY_URL=https://app.forkmesh.com`, so the announcement is the
only production call an instance makes. Both are written by the installer
and refreshed by `--update`.

## World connectivity

What "each World is connected" means in the code:

- **One World per origin.** The World is a static app (`world/public/world`)
  plus the `/api/world/*` routes. On forkmesh.com it is served at
  `https://world.forkmesh.com/world/` (`WORLD_ORIGIN`). The installer sets
  `WORLD_ORIGIN` to the instance's own origin, so on an instance with the
  `world` plugin on, `external_site_delivery` serves the bundled World
  locally at `https://<domain>/world/`, with its own members, chat and Office
  in the instance's database. With the plugin off `/world` is not found.
  Worlds share no state: there is no relay between them.
- **The bundle.** The instance build copies the built World into `app/dist`
  (`build_site_assets.py instance-world`), and `_serve_world_asset` answers
  `/world`, `/world/` and `/world/*` from it. Before that step existed the
  instance had no World files at all, so `/world/` answered the 404 page even
  with the plugin on. Headers, caching, branding and the parts that need
  forkmesh.com are in [INSTANCES.md](../INSTANCES.md#the-world-on-an-instance).
- **Twelve instance slots.** The ForkMesh World's town is ringed by twelve
  instance hexagons (`INSTANCE_HEX_SLOTS`, `scene/layout.js`). Every World
  reads `/api/start/instances` on its own origin and builds a hexagon for each
  entry with `world: true` and a slot. Only forkmesh.com's directory has
  entries, so only the ForkMesh World shows them.
- **Portals.** A directory entry carries `worldUrl`
  (`https://<domain>/world/`) when the instance announced one, that is when
  it chose a World hexagon and its `world` plugin is on
  (`instance_directory.world_url` requires the same host). Its hexagon's
  label reads "portal · <domain>", and clicking the landmark travels, in the
  same tab, to `https://<domain>/world/?portal=<slot>`. Without `worldUrl`
  the landmark opens the instance's home page in a new tab, as before.
- **The way back.** `/api/instance` on an instance reports
  `directory: {listed, world, portal}`; `portal` is
  `{"home": "https://world.forkmesh.com/world/", "slot": <its hexagon>}` once
  forkmesh.com has listed it with a World (`worldHome` comes from
  forkmesh.com's own `WORLD_ORIGIN` in the announcement answer). The
  instance's World turns that into one "ForkMesh World" hexagon at the same
  slot (`homePortalHex`, `world/public/world/world-instance-portals.js`), and
  its landmark travels to `https://world.forkmesh.com/world/?portal=<slot>`.
- **Arrival.** `?portal=<n>` (0 to 11) makes the World place the visitor in
  front of hexagon `n`'s landmark (`instanceHexArrival`,
  `scene/instance-hexes.js`, through `setSpawn`) as soon as that hexagon is
  built, then drops the parameter from the address. Both Worlds use the same
  layout, so walking through a portal lands next to the portal that leads
  back. A slot that has no hexagon is ignored and the visitor arrives as
  usual.
- **Crossing the road.** Next to the portal boards, walking across the road
  between an instance hexagon and the ForkMesh hexagons travels between the
  two Worlds (`?from=`, `&slot=`, `&pos=`), and an instance's World is drawn
  as its own hexagon with the ForkMesh World next door instead of a copy of
  the ForkMesh town. See
  [INSTANCES.md](../INSTANCES.md#an-instance-world-is-its-own-hexagon).
- The legacy relay federation (`/api/world/instances`, approved `relays`
  rows drawn as small towers in the Fediverse district) is separate and
  unchanged.

## Testing it

Automated: `app/tests/test_instance_registry.py` (protocol, failure cases,
console, document, `/api/instance`), `app/tests/test_instance_directory.py`,
`app/tests/test_install_instance_script.py`,
`app/host/tests_unit/test_instance_bootstrap.py`,
`app/tests/host/test_host_egress.py`,
`world/tests/test_world_instance_portals.py` (URL rules, home portal, arrival
spots, wiring), `app/tests/test_instance_world_bundle.py` (bundle step, path
and cache rules, plugin ownership), `app/tests/host/test_instance_world_serving.py`
(`/world/`, modules, vendor files and the API through the instance router with
the plugin on, off and saved) and `world/tests/test_world_instance_brand.py`.

By hand, once forkmesh.com and the instance both run this code:

1. On the instance, Platform → Plugins: turn **World and Office** on and
   save. `https://<domain>/world/` opens the instance's World.
2. Platform → ForkMesh directory: turn on both switches, **Save and
   announce**. The page shows "Listed" and "World hexagon N". Check
   `curl -s https://<domain>/.well-known/forkmesh-instance.json` (choices,
   `worldUrl`, `publicKey`) and
   `curl -s https://app.forkmesh.com/api/start/instances` (the entry with
   `"world": true`, its `slot` and `worldUrl`).
3. `curl -s https://<domain>/api/instance | jq .directory` shows
   `portal.home` and `portal.slot`.
4. Open `https://world.forkmesh.com/world/?portal=<slot>`: you arrive in front
   of the instance's hexagon, labelled "portal · <domain>". Click its board:
   the browser goes to `https://<domain>/world/?portal=<slot>` and you arrive
   in front of the "ForkMesh World" hexagon there. Click that board to go
   back.
5. Turn both switches off and save: the page shows "Not listed" and the entry
   leaves `/api/start/instances`.

On forkmesh.com the admin console's Instances view shows the entry as
"Joined from its own console" with Hide and Delete.
