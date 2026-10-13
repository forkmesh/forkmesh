export const AVATAR_LEG_SEGMENT = 0.625;
export const AVATAR_SHOE_HEIGHT = 0.26;
// Fallback for the head sphere when the worn emoji cannot be sampled. The
// live colour is read back out of the rendered glyph instead (see
// edgeEmojiColor) so the sphere is exactly the emoji's own rim colour.
export const AVATAR_EMOJI_SKIN_COLOR = "#ffcc4d";

// Reads back the drawn glyph, or "" when the canvas is tainted.
export function emojiPixels(context, canvas) {
  try {
    return context.getImageData(0, 0, canvas.width, canvas.height).data;
  } catch (_) {
    return null;
  }
}
export const ANTENNA_STALK_COLOR = "#1aa856";

// Unified-card border: one darker, node-light-style colour per coarse
// account-recency bucket from the server ("active within …").
export const ACTIVITY_LIGHT_COLORS = Object.freeze({
  hour: "#168a4d",
  "5h": "#205f43",
  "24h": "#607b2e",
  "3d": "#806628",
  "5d": "#a95525",
  "10d": "#9e302b",
  "30d": "#733f46",
  "60d": "#4d5f75",
  "120d": "#3f4f67",
  stale: "#555f5b",
});

export const AVATAR_SOLANA_ADDRESS_RE = /^[1-9A-HJ-NP-Za-km-z]{32,44}$/;
