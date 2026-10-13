# World store elements

Each file in this folder is one **self-contained world element** — a plugin.
A visitor buys an element in the World store, and it is then built into *their*
world with the parameters they chose.

This folder is a staging area. It is expected to move to its own repository;
elements are then transferred back into this folder to be picked up, so nothing
here may import from the rest of the world bundle. An element may only use the
`THREE` instance and the helpers handed to `build()`.

## Contract

```js
export const manifest = {
  id: "aurora-beacon",          // stable, kebab-case, unique
  label: "Aurora beacon",
  category: "Store",            // world element category shown in Elements
  version: 1,
  summary: "One sentence shown in the store.",
  params: [                     // user-set parameters, validated both sides
    { key: "height", type: "number", label: "Height", default: 8,
      min: 4, max: 14, step: 0.5 },
    { key: "color", type: "select", label: "Colour", default: "aurora",
      options: ["aurora", "ember", "ice"] },
    { key: "label", type: "text", label: "Caption", default: "",
      maxLength: 40 },
    { key: "spin", type: "toggle", label: "Rotate", default: true },
  ],
  network: ["/api/world/deploy-status"],  // same-origin reads, allowlisted
};

export function build({ THREE, params, net, now }) {
  // ...assemble geometry...
  return {
    group,                       // THREE.Object3D added to the world
    tick(nowMs) {},              // optional, called each frame while enabled
    dispose() {},                // optional, called on uninstall
  };
}
```

`params` reaching `build()` are already normalised against the manifest, so a
missing or out-of-range value never reaches an element.

`net(path)` performs a same-origin `GET` and resolves to parsed JSON. It
refuses any path the manifest did not declare in `network`, so an element
cannot reach an endpoint the store did not disclose at purchase time. Elements
have no other I/O: no direct `fetch`, no storage, no cross-origin requests.

The price of an element is **not** taken from this folder. Prices live in
`world/src/world_element_store.py`, which is the authority for what
is charged and which parameters are accepted;
`world/tests/test_world_element_store.py` keeps the two in lockstep.
