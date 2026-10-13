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
export function paintProceduralAvatarFace(context, identityKey) {
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
  context.fillStyle = skin;
  context.fillRect(0, 0, 128, 128);
  context.lineCap = "round";
  context.lineJoin = "round";

  context.strokeStyle = "#553a2d";
  context.lineWidth = 5;
  context.beginPath();
  context.moveTo(64 - eyeSpacing - 8, 42 - browLift);
  context.quadraticCurveTo(64 - eyeSpacing, 38 - browLift, 64 - eyeSpacing + 8, 42 - browLift);
  context.moveTo(64 + eyeSpacing - 8, 42 - (6 - browLift));
  context.quadraticCurveTo(64 + eyeSpacing, 38 - (6 - browLift), 64 + eyeSpacing + 8, 42 - (6 - browLift));
  context.stroke();

  context.fillStyle = "#ffffff";
  [64 - eyeSpacing, 64 + eyeSpacing].forEach((x) => {
    context.beginPath();
    context.ellipse(x, 57, eyeRadius + 3, eyeRadius + 5, 0, 0, Math.PI * 2);
    context.fill();
    context.fillStyle = eye;
    context.beginPath();
    context.arc(x, 58, eyeRadius, 0, Math.PI * 2);
    context.fill();
    context.fillStyle = "#ffffff";
    context.beginPath();
    context.arc(x - 1, 56, 1.5, 0, Math.PI * 2);
    context.fill();
    context.fillStyle = "#ffffff";
  });

  context.strokeStyle = "#9a6245";
  context.lineWidth = 4;
  context.beginPath();
  context.moveTo(64, 61);
  context.quadraticCurveTo(58 + (seed % 13), 72, 65, 76);
  context.stroke();

  context.strokeStyle = "#5c3028";
  context.lineWidth = 5;
  context.beginPath();
  context.moveTo(43, 88);
  context.quadraticCurveTo(64, 100 + smile * 4, 85, 88 - smile * 2);
  context.stroke();

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
