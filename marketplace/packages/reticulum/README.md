# Reticulum

Connect the ForkMesh desktop to [Reticulum](https://reticulum.network/), the
encrypted mesh network that runs over the internet, LoRa radios, packet radio
and local networks. Install it from the Market to add **Reticulum** to the
desktop rail. It is bundled but off by default.

The page opens on a live view of the network:

- **Tiles**: connection state, how many destinations you have heard (and how
  many are new or one hop away), announces per minute, traffic in and out, and
  path lookups.
- **Network**: a map with this desktop in the middle. Each interface gets a
  slice of the circle, and every destination sits on the ring for its hop
  count. Colour and shape show what it is: people (LXMF messaging and calls),
  nodes and services (Nomad Network, file transfer), propagation nodes,
  transport nodes, or other. A ripple marks each new announce. Hover a point
  for its details and click it to select it. Beside the map is a three-minute
  traffic chart and the selected destination's identity, distance, next hop,
  announce time and forward secrecy.
- **Interfaces**: add, edit, switch off, reconnect and remove interfaces. Each
  row shows its status, uptime, bytes and packets in and out, and announces.
  The panel below has the selected interface's details and its own traffic
  chart. **Add public entry point** adds a community backbone in one click.
  **Import Reticulum config…** reads the interfaces from an existing
  `~/.reticulum/config`. Entry points that other nodes announce on the network
  are listed with their proof of work; connect to one with a click.
- **Destinations**: every destination heard, with a filter, kind picker,
  sorting and **Find path** for any destination hash.
- **Activity**: a timeline of announces, path answers, interfaces coming and
  going and rejected packets, plus packet counters.
- **Identity**: this desktop's Reticulum destination and identity hashes, the
  name in its announces, automatic announcing and answering path requests.

Three kinds of interface are supported:

| Interface | Connects to |
| --- | --- |
| TCP | A Reticulum node's `TCPServerInterface` or `BackboneInterface`, such as `rmap.world:4242`; KISS framing optional |
| Reticulum on this computer | The shared instance of `rnsd`, which brings its radios and paths with it |
| UDP | Other Reticulum nodes on the local network (`UDPInterface`) |

Nothing is announced until you choose to. Every connection asks the ForkMesh
firewall first, and **Disconnect** closes them all. Interfaces protected by a
network name and passphrase (IFAC) are not supported yet and are skipped on
import. Details are in
[docs/marketplace/reticulum.md](../../../docs/marketplace/reticulum.md).
