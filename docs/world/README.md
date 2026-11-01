# World

ForkMesh World is the three.js town at `world.forkmesh.com` where accounts,
repositories, mirrors and agents appear as places and avatars, with global
chat, the Office (encrypted meeting rooms) and community boards. The browser
code is static and served by the core nodes, whose App answers
`/api/world/*`; the desktop embeds the same page. Every self-hosted instance
can run its own World on one shared map. Entry point in the tree:
[world/README.md](../../world/README.md).

## Pages

| Page | Covers |
| --- | --- |
| [development.md](development.md) | Code map, build and serving on the cores, `/api/world/*` interfaces, configuration, tests, the performance gate, operations, security and traps |
| [rendering.md](rendering.md) | Scene modules, the resource authority, instancing and culling, the loading pipeline and diagnostics |
| [interface.md](interface.md) | The HTML chrome: inline notices, the HUD island with chat and agent prompts, HUD rails, fresh start, crash reports |
| [town.md](town.md) | The city, desert ring, roads and instance hexagons, the Office tower, board ring, Members Circle, mirror yard, floor notes and activities |
| [mesh.md](mesh.md) | One map for every World: neighbours, published scenes, watch and visit sockets, standing in another World, visas |
| [federation.md](federation.md) | The protocol an instance implements to join: hexagon ownership, visitor tokens, CORS, routing |
| [voice.md](voice.md) | Voice, camera-driven avatars, privacy bubbles, voice rooms and the one channel list |
| [genie-spider.md](genie-spider.md) | The Genie spider that follows you around the World and plays 100 acts |

## Related

- Core pages: [architecture](../architecture.md),
  [encryption](../encryption.md), [self-hosting](../self-hosting.md),
  [threat model](../protocol/threat-model.md).
- The 2D organization office: [Office](../office/README.md). Community
  services and boards: [World community services](../world-community-services.md).
- The App that serves `/api/world/*`: [app](../app/README.md),
  [sessions](../app/auth-sessions.md), [footprint](../app/worker-footprint.md);
  deploys: [fleet tooling](../operations/fleet-tooling.md),
  [auto-deploy](../operations/auto-deploy.md),
  [served stack](../operations/served-stack.md).
- World elements contract:
  [elements/README.md](../../world/public/world/elements/README.md).
  Security: [sealed chat](../security/sealed-chat.md),
  [local speech bridge](../security/world-local-speech-bridge.md).

Back to the [documentation home](../README.md).
