// Registry of purchasable world elements.
//
// Modules load lazily: a visitor downloads the geometry for an element only
// once they own it and have it enabled. Adding an element to the store means
// dropping its file in this folder, adding one line here, and adding its
// price and parameters to world/src/world_element_store.py.

const MODULES = {
  "aurora-beacon": () => import("./aurora-beacon.js"),
  "orbit-sculpture": () => import("./orbit-sculpture.js"),
  "status-obelisk": () => import("./status-obelisk.js"),
  "status-board": () => import("./status-board.js"),
};

export function storeElementIds() {
  return Object.keys(MODULES);
}

export async function loadStoreElementModule(id) {
  const load = MODULES[id];
  if (!load) throw new Error(`unknown store element: ${id}`);
  const module = await load();
  if (typeof module?.build !== "function" || !module?.manifest) {
    throw new Error(`store element ${id} does not export manifest + build`);
  }
  if (module.manifest.id !== id) {
    throw new Error(`store element ${id} declares id ${module.manifest.id}`);
  }
  return module;
}

// Normalises one visitor's saved parameters against the element's own
// manifest. The worker validates the same values before storing them; this
// pass exists so a stale or hand-edited value can never reach build().
export function normalizeElementParams(manifest, saved) {
  const source = saved && typeof saved === "object" ? saved : {};
  const params = {};
  (manifest.params || []).forEach((spec) => {
    const value = source[spec.key];
    if (spec.type === "number") {
      const number = Number(value);
      params[spec.key] = Number.isFinite(number)
        ? Math.min(spec.max, Math.max(spec.min, number))
        : spec.default;
    } else if (spec.type === "select") {
      params[spec.key] = (spec.options || []).includes(value)
        ? value
        : spec.default;
    } else if (spec.type === "toggle") {
      params[spec.key] = typeof value === "boolean" ? value : spec.default;
    } else {
      params[spec.key] = String(value ?? spec.default ?? "").slice(
        0,
        spec.maxLength || 80,
      );
    }
  });
  return params;
}
