import { normalizeWorldStatus } from "../world-data.js";
import { wordTexture } from "./canvas.js";
import { cloneSharedPlane } from "./materials.js";
import * as resources from "./resources.js";

export function makeLabelSprite(THREE, title, subtitle, color) {
  const material = new THREE.SpriteMaterial({
    map: wordTexture(THREE, title, subtitle, color),
    transparent: true,
    depthTest: false,
    depthWrite: false,
  });
  const sprite = new THREE.Sprite(material);
  sprite.scale.set(7.5, 2.5, 1);
  sprite.renderOrder = 10;
  return sprite;
}

export function makeOfficeWallPlacard(
  THREE,
  title,
  subtitle,
  color = "#9ef7c6",
  width = 22,
  height = 3.6,
) {
  const placard = cloneSharedPlane(
    THREE,
    width,
    height,
    resources.uniqueMaterial(THREE, {
      map: wordTexture(THREE, title, subtitle, color),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  placard.userData.officeWallMounted = true;
  return placard;
}

// Lettering cut into wood: a dark shadow above the glyph, a coloured bloom
// below it, and the glyph itself between them.
export function embossPlankText(context, text, x, y, font, glow) {
  context.font = font;
  context.fillStyle = "rgba(20,10,4,0.6)";
  context.fillText(text, x, y - 1.4);
  context.fillStyle = glow;
  context.globalAlpha = 0.5;
  context.fillText(text, x, y + 1.4);
  context.globalAlpha = 1;
  context.fillStyle = "#2a160a";
  context.fillText(text, x, y);
}

export function updatePlayerLabel(element, identity) {
  const status = normalizeWorldStatus(
    identity?.statusEmoji,
    identity?.statusNote,
  );
  const name = String(identity?.name || "visitor").slice(0, 32);
  const guestLabel =
    String(identity?.accountStatus || "Guest") === "Guest" ||
    /^World Guest\b|^Guest(?:\s|$)/i.test(name);
  element.hidden = guestLabel;
  element.setAttribute("aria-hidden", String(guestLabel));
  const nameCopy = document.createElement("span");
  nameCopy.className = "world-player-label-name";
  nameCopy.textContent = name;
  element.replaceChildren(nameCopy);
  if (status.emoji) {
    const statusCopy = document.createElement("span");
    statusCopy.className = "world-player-label-status";
    statusCopy.textContent = [status.emoji, status.note]
      .filter(Boolean)
      .join(" ");
    element.appendChild(statusCopy);
    element.setAttribute(
      "aria-label",
      `${name}, public status ${status.emoji}${
        status.note ? ` ${status.note}` : ""
      }`,
    );
  } else {
    element.setAttribute("aria-label", name);
  }
}

export function makePlayerLabel(player, labelLayer) {
  const element = document.createElement("div");
  element.className = "world-player-label";
  element.dataset.playerLabel = player.userData.id || "";
  updatePlayerLabel(element, player.userData);
  labelLayer.appendChild(element);
  return element;
}

export function updateScreenLabel(
  THREE,
  object,
  element,
  camera,
  width,
  height,
  yOffset = 0,
  position = new THREE.Vector3(),
) {
  object.getWorldPosition(position);
  position.y += yOffset;
  position.project(camera);
  const visible = position.z > -1 && position.z < 1 && Math.abs(position.x) < 1.25 && Math.abs(position.y) < 1.25;
  // Equality-guarded writes: labels run at ~30 Hz for every avatar, and
  // unconditional style writes dirty layout even when nothing moved.
  const visibility = visible ? "visible" : "hidden";
  if (element.style.visibility !== visibility) {
    element.style.opacity = visible ? "1" : "0";
    element.style.visibility = visibility;
  }
  if (!visible) return;
  const left = `${Math.round((position.x * 0.5 + 0.5) * width)}px`;
  const top = `${Math.round((-position.y * 0.5 + 0.5) * height)}px`;
  if (element.style.left !== left) element.style.left = left;
  if (element.style.top !== top) element.style.top = top;
}

export function updatePlayerScreenLabel(
  object,
  element,
  camera,
  width,
  height,
  yOffset,
  position,
  origin = null,
  maxDistanceSq = Infinity,
) {
  if (!element) return;
  if (!element.dataset.labelAnchored) {
    element.style.left = "0px";
    element.style.top = "0px";
    element.dataset.labelAnchored = "1";
  }
  object.getWorldPosition(position);
  const far =
    origin !== null && position.distanceToSquared(origin) > maxDistanceSq;
  const display = far ? "none" : "";
  if (element.style.display !== display) element.style.display = display;
  if (far) return;
  position.y += yOffset;
  position.project(camera);
  const visible = position.z > -1 && position.z < 1 && Math.abs(position.x) < 1.25 && Math.abs(position.y) < 1.25;
  const visibility = visible ? "visible" : "hidden";
  if (element.style.visibility !== visibility) {
    element.style.opacity = visible ? "1" : "0";
    element.style.visibility = visibility;
  }
  if (!visible) return;
  const x = Math.round((position.x * 0.5 + 0.5) * width);
  const y = Math.round((-position.y * 0.5 + 0.5) * height);
  if (element._labelX === x && element._labelY === y) return;
  element._labelX = x;
  element._labelY = y;
  element.style.transform = `translate3d(${x}px, ${y}px, 0) translate(-50%, -100%)`;
}
