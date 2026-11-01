# ForkMesh Office

A 2D pixel-art office for each ForkMesh organization: walk around a tile
map, talk to people near you over WebRTC, sit at meeting tables, share a
screen, draw on whiteboards, chat, claim desks, see teammates' agent runs,
and play. It is served at `https://app.forkmesh.com/office`; the client is a
static bundle, and the server half runs in Python on the core nodes' app
host, like the [World](../world/README.md).

## Pages

| Page | Covers |
| --- | --- |
| [features.md](features.md) | Entering, controls, the map and rooms, profiles and desks, the Agent Room, rides, proximity audio, status lines, feature limits |
| [development.md](development.md) | Code map, architecture, the plug-in contract, protocol, security, the TURN relay, configuration, tests, performance, operations, traps |

## Related

- Core pages: [architecture](../architecture.md),
  [encryption](../encryption.md), [self-hosting](../self-hosting.md),
  [threat model](../protocol/threat-model.md).
- [app](../app/README.md),
  [cron and Durable Objects](../app/cron-and-durable-objects.md),
  [host](../host/README.md), [served stack](../operations/served-stack.md).

Back to the [documentation home](../README.md).
