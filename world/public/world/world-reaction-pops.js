export const REACTION_POP_MS = 1900;
export const REACTION_POP_LIMIT = 14;
export const REACTION_SPARKS = 8;

export function reactionBurst(count = REACTION_SPARKS, seed = 1) {
  let value = Math.max(1, Math.floor(seed) % 2147483647);
  const random = () => {
    value = (value * 16807) % 2147483647;
    return value / 2147483647;
  };
  return Array.from({ length: count }, (_, index) => {
    const angle = (index / count) * Math.PI * 2 + random() * 0.6;
    const reach = 46 + random() * 52;
    return {
      dx: Math.round(Math.cos(angle) * reach),
      dy: Math.round(Math.sin(angle) * reach * 0.8 - 18),
      spin: Math.round((random() - 0.5) * 300),
      delay: Math.round(random() * 140),
      scale: Number((0.45 + random() * 0.35).toFixed(2)),
    };
  });
}

export function popPlacement(point, viewport, margin = 40) {
  const width = Math.max(1, Number(viewport?.width) || 1);
  const height = Math.max(1, Number(viewport?.height) || 1);
  if (!point || !Number.isFinite(point.x) || !Number.isFinite(point.y)) {
    return { x: Math.round(width / 2), y: Math.round(height * 0.55), anchored: false };
  }
  return {
    x: Math.round(Math.max(margin, Math.min(width - margin, point.x))),
    y: Math.round(Math.max(margin * 2, Math.min(height - margin * 3, point.y))),
    anchored: point.onScreen !== false,
  };
}

export function createReactionPops({ layer, documentRef = globalThis.document, scope = globalThis, reducedMotion = false } = {}) {
  const live = [];
  let seed = 7;

  function remove(node) {
    const index = live.indexOf(node);
    if (index >= 0) live.splice(index, 1);
    node.remove();
  }

  function pop(emoji, { point = null, label = "", big = false } = {}) {
    const host = typeof layer === "function" ? layer() : layer;
    if (!host || !emoji) return null;
    const viewport = { width: scope.innerWidth || host.clientWidth, height: scope.innerHeight || host.clientHeight };
    const place = popPlacement(point, viewport);
    const node = documentRef.createElement("div");
    node.className = "world-reaction-pop";
    node.dataset.size = big ? "big" : "normal";
    node.dataset.anchored = place.anchored ? "true" : "false";
    node.style.setProperty("--pop-x", `${place.x}px`);
    node.style.setProperty("--pop-y", `${place.y}px`);
    const face = documentRef.createElement("span");
    face.className = "world-reaction-pop-emoji";
    face.textContent = emoji;
    node.append(face);
    if (!reducedMotion) {
      seed += 7919;
      for (const spark of reactionBurst(REACTION_SPARKS, seed)) {
        const particle = documentRef.createElement("span");
        particle.className = "world-reaction-spark";
        particle.textContent = emoji;
        particle.style.setProperty("--dx", `${spark.dx}px`);
        particle.style.setProperty("--dy", `${spark.dy}px`);
        particle.style.setProperty("--spin", `${spark.spin}deg`);
        particle.style.setProperty("--spark-scale", String(spark.scale));
        particle.style.animationDelay = `${spark.delay}ms`;
        node.append(particle);
      }
      const ring = documentRef.createElement("span");
      ring.className = "world-reaction-ring";
      node.append(ring);
    }
    if (label) {
      const chip = documentRef.createElement("span");
      chip.className = "world-reaction-pop-label";
      chip.textContent = label;
      node.append(chip);
    }
    host.append(node);
    live.push(node);
    while (live.length > REACTION_POP_LIMIT) remove(live[0]);
    scope.setTimeout(() => remove(node), REACTION_POP_MS);
    return node;
  }

  return {
    pop,
    count: () => live.length,
    clear() {
      while (live.length) remove(live[0]);
    },
  };
}
