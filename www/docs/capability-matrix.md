# ForkMesh surface capability matrix

A capability is present on a surface when the surface implements it or offers a
safe deep link to its canonical interface. The contract is
[`capability-matrix.json`](capability-matrix.json): every row names a canonical
path and a non-missing mode for the Worker, dashboard/Web, World, and Qt, and
CI validates it.

Owner-device secrets never leave the device: Cloudflare tokens, repository
decryption keys, mirror process control, wallet signing keys, SSH private keys,
agent-provider credentials, and microphone audio. Web and World show authorized
state and link to the local flow. The Worker supplies authenticated APIs,
policy, encrypted envelopes, and public-safe projections; an API entry does
not imply custody or plaintext access. Raw SSH terminates only at the
node-side forced-command gateway.
