// Deterministic avatar portraits, shared by the 3D scene and the flat HUD.
//
// world.js paints the account launcher badge while the town is still loading,
// long before the renderer exists. Keeping the painter here lets the shell
// import a few kilobytes for that badge instead of anchoring the whole scene
// module — a megabyte of geometry it cannot use yet — into the first visit's
// static graph.

// The scene draws from the same hash for every other deterministic choice
// it makes, so it lives with the portrait rather than being duplicated.
export function hashNumber(value) {
  let hash = 2166136261;
  for (const char of String(value || "")) {
    hash ^= char.charCodeAt(0);
    hash = Math.imul(hash, 16777619);
  }
  return Math.abs(hash >>> 0);
}

// A stable, code-native portrait for accounts that have not uploaded a photo.
// The same public identity key always selects the same skin, eyes, brows,
// mouth, freckles and glasses, so people remain recognizable across devices
// without storing another image or sending image bytes over presence.
//
// The painter is separated from the texture so the flat HUD — the account
// launcher badge in the top-right corner, which every guest reaches without a
// photo — can print the identical 128px face onto a plain 2D canvas instead of
// sitting empty. Returns the drawn skin colour.
export function paintProceduralAvatarFace(context, identityKey, expression = null) {
  const seed = hashNumber(String(identityKey || "forkmesh-visitor"));
  const skins = ["#f6d8b6", "#e9bb8c", "#c98555", "#8e5738", "#5d392a"];
  const eyes = ["#30231c", "#31576d", "#3f633b", "#725138"];
  const skin = skins[seed % skins.length];
  const eye = eyes[(seed >>> 3) % eyes.length];
  const eyeSpacing = 19 + ((seed >>> 7) % 7);
  const eyeRadius = 4 + ((seed >>> 10) % 3);
  const browLift = (seed >>> 13) % 7;
  const smile = (seed >>> 16) % 3;
  const glasses = ((seed >>> 19) & 3) === 0;
  const freckles = ((seed >>> 22) & 3) === 0;
  const live = expression && typeof expression === "object" ? expression : null;
  const level = (name, low = 0, high = 1) => Math.max(low, Math.min(high, Number(live?.[name]) || 0));
  context.fillStyle = skin;
  context.fillRect(0, 0, 128, 128);
  context.lineCap = "round";
  context.lineJoin = "round";

  const leftRaise = level("browLeft", -1, 1) * 7;
  const rightRaise = level("browRight", -1, 1) * 7;
  context.strokeStyle = live && seed % skins.length >= 3 ? "#1b110c" : "#553a2d";
  context.lineWidth = 5;
  context.beginPath();
  context.moveTo(64 - eyeSpacing - 8, 42 - browLift - leftRaise);
  context.quadraticCurveTo(64 - eyeSpacing, 38 - browLift - leftRaise * 1.3, 64 - eyeSpacing + 8, 42 - browLift - leftRaise);
  context.moveTo(64 + eyeSpacing - 8, 42 - (6 - browLift) - rightRaise);
  context.quadraticCurveTo(64 + eyeSpacing, 38 - (6 - browLift) - rightRaise * 1.3, 64 + eyeSpacing + 8, 42 - (6 - browLift) - rightRaise);
  context.stroke();

  const lookX = level("gazeX", -1, 1) * eyeRadius * 0.9;
  const lookY = level("gazeY", -1, 1) * eyeRadius * 0.7;
  [[64 - eyeSpacing, level("lidLeft")], [64 + eyeSpacing, level("lidRight")]].forEach(([x, lid]) => {
    const open = 1 - lid;
    if (open < 0.18) {
      context.strokeStyle = "#3a2a22";
      context.lineWidth = 3;
      context.beginPath();
      context.moveTo(x - eyeRadius - 3, 58);
      context.quadraticCurveTo(x, 61, x + eyeRadius + 3, 58);
      context.stroke();
      return;
    }
    context.save();
    context.fillStyle = "#ffffff";
    context.beginPath();
    context.ellipse(x, 57 + (1 - open) * 2, eyeRadius + 3, (eyeRadius + 5) * open, 0, 0, Math.PI * 2);
    context.fill();
    context.clip();
    context.fillStyle = eye;
    context.beginPath();
    context.arc(x + lookX, 58 + lookY, eyeRadius, 0, Math.PI * 2);
    context.fill();
    context.fillStyle = "#ffffff";
    context.beginPath();
    context.arc(x - 1 + lookX, 56 + lookY, 1.5, 0, Math.PI * 2);
    context.fill();
    context.restore();
  });

  context.strokeStyle = "#9a6245";
  context.lineWidth = 4;
  context.beginPath();
  context.moveTo(64, 61);
  context.quadraticCurveTo(58 + (seed % 13), 72, 65, 76);
  context.stroke();

  const lips = Array.isArray(live?.lips) && live.lips.length >= 20 ? live.lips : null;
  if (lips) {
    const scaleX = eyeSpacing * 2;
    const scaleY = 31;
    const at = ([x, y]) => [64 + x * scaleX, 57 + y * scaleY];
    const outer = lips.slice(0, 12).map(at);
    const inner = lips.slice(12, 20).map(at);
    context.fillStyle = "#b0524a";
    context.beginPath();
    outer.forEach(([x, y], i) => (i ? context.lineTo(x, y) : context.moveTo(x, y)));
    context.closePath();
    context.fill();
    const gap = Math.abs(inner[6][1] - inner[2][1]);
    if (gap > 1.2) {
      context.fillStyle = "#3b1016";
      context.beginPath();
      inner.forEach(([x, y], i) => (i ? context.lineTo(x, y) : context.moveTo(x, y)));
      context.closePath();
      context.fill();
      context.save();
      context.clip();
      context.fillStyle = "#f3ece2";
      context.fillRect(inner[0][0], Math.min(inner[2][1], inner[3][1], inner[1][1]) - 1, inner[4][0] - inner[0][0], Math.max(2, gap * 0.28));
      if (level("tongue") > 0.3) {
        context.fillStyle = "#e0727a";
        context.beginPath();
        context.ellipse((inner[0][0] + inner[4][0]) / 2, inner[6][1], (inner[4][0] - inner[0][0]) * 0.3, gap * 0.45, 0, 0, Math.PI * 2);
        context.fill();
      }
      context.restore();
    }
    context.strokeStyle = "#5c3028";
    context.lineWidth = 1.5;
    context.beginPath();
    outer.forEach(([x, y], i) => (i ? context.lineTo(x, y) : context.moveTo(x, y)));
    context.closePath();
    context.stroke();
  } else if (live) {
    const open = level("mouth");
    const grin = level("smile");
    const round = level("round");
    const half = 21 * (1 + grin * 0.25 - round * 0.4);
    if (open > 0.08) {
      context.fillStyle = "#3b1016";
      context.beginPath();
      context.ellipse(64, 90 + open * 4, half, 3 + open * 14, 0, 0, Math.PI * 2);
      context.fill();
    }
    context.strokeStyle = "#5c3028";
    context.lineWidth = 5;
    context.beginPath();
    context.moveTo(64 - half, 88 - grin * 5);
    context.quadraticCurveTo(64, 96 + grin * 8 + open * 8, 64 + half, 88 - grin * 5);
    context.stroke();
  } else {
    context.strokeStyle = "#5c3028";
    context.lineWidth = 5;
    context.beginPath();
    context.moveTo(43, 88);
    context.quadraticCurveTo(64, 100 + smile * 4, 85, 88 - smile * 2);
    context.stroke();
  }

  if (glasses) {
    context.strokeStyle = "#253a39";
    context.lineWidth = 4;
    context.strokeRect(38 - eyeSpacing / 5, 47, 29, 23);
    context.strokeRect(61 + eyeSpacing / 5, 47, 29, 23);
    context.beginPath();
    context.moveTo(67, 56);
    context.lineTo(73, 56);
    context.stroke();
  }
  if (freckles) {
    context.fillStyle = "rgba(104,56,42,0.55)";
    [-24, -17, -10, 10, 17, 24].forEach((offset, index) => {
      context.beginPath();
      context.arc(64 + offset, 75 + (index % 2) * 3, 1.5, 0, Math.PI * 2);
      context.fill();
    });
  }
  return skin;
}

// The same portrait as a flat image the HUD can hand to an <img>. Guests never
// have an uploaded photo, so without this the round account launcher in the
// top-right corner rendered as an empty disc.
export function proceduralAvatarFaceDataURL(identityKey, size = 128) {
  try {
    const canvas = document.createElement("canvas");
    canvas.width = size;
    canvas.height = size;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    if (!context) return "";
    if (size !== 128) context.setTransform(size / 128, 0, 0, size / 128, 0, 0);
    paintProceduralAvatarFace(context, identityKey);
    return canvas.toDataURL("image/png");
  } catch (_) {
    // A tainted or unavailable canvas must never keep the HUD from rendering.
    return "";
  }
}
