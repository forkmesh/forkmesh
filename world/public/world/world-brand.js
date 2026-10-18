export const PLATFORM_NAME = "ForkMesh";
const BRAND_RE = /(?<![@\w./-])(ForkMesh|FORKMESH)(?![\w.-]|\s+(?:World|WORLD|Desktop|DESKTOP)\b)/g;
const MARK = Symbol.for("forkmesh.world.canvasBrand");
const NAME_RE = /^[A-Za-z0-9][A-Za-z0-9 ._&+-]{0,39}$/;
const METHODS = ["fillText", "strokeText", "measureText"];

export function instanceBrandName(doc = globalThis.document) {
  const name = String(doc?.querySelector?.("script[data-instance-brand]")?.dataset?.instanceName || "")
    .split(/\s+/)
    .filter(Boolean)
    .join(" ");
  return name !== PLATFORM_NAME && NAME_RE.test(name) ? name : "";
}

export function brandCanvasText(text, name) {
  if (!name || typeof text !== "string" || text.indexOf("ForkMesh") === -1 && text.indexOf("FORKMESH") === -1) {
    return text;
  }
  return text.replace(BRAND_RE, (match) => (match === "FORKMESH" ? name.toUpperCase() : name));
}

export function installCanvasBrand(name = instanceBrandName(), scope = globalThis) {
  if (!name) return false;
  let installed = false;
  for (const type of ["CanvasRenderingContext2D", "OffscreenCanvasRenderingContext2D"]) {
    const proto = scope?.[type]?.prototype;
    if (!proto || proto[MARK]) continue;
    for (const method of METHODS) {
      const original = proto[method];
      if (typeof original !== "function") continue;
      proto[method] = function brandedText(text, ...rest) {
        return original.call(this, brandCanvasText(text, name), ...rest);
      };
    }
    proto[MARK] = name;
    installed = true;
  }
  return installed;
}
