# ForkMesh surface capability matrix

ForkMesh aims for capability parity, not secret duplication. A capability is
present on a surface when the surface implements it directly or offers a safe
deep link to its canonical interface. The machine-readable contract is
[`capability-matrix.json`](capability-matrix.json).

The deliberate exceptions are owner-device boundaries:

- Cloudflare tokens, repository decryption keys, mirror process control, wallet
  signing keys, SSH private keys, agent-provider login credentials, and
  microphone audio stay on the local device.
- Web and World may show authorized state and open the local/canonical
  flow, but they do not receive those secrets.
- The Worker supplies authenticated APIs, policy enforcement, encrypted
  envelopes, and public-safe projections. An API entry does not imply custody or
  plaintext access.
- The Worker stores encrypted SSH public keys and enforces repository
  permissions, while raw SSH terminates only at the configured node-side
  forced-command gateway.

Every matrix row must name a canonical path and have a non-missing mode for the
Worker, dashboard/Web, World, and Qt. CI validates the matrix and the
World launch entry points. Feature-specific tests remain responsible for actual
authorization, privacy, and behavior.
