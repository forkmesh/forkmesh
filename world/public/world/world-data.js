export const LANDMARKS = [
  {
    id: "fountain",
    label: "SOL reward fountain",
    shortLabel: "Reward pool",
    eyebrow: "COMMUNITY REWARDS / 02",
    icon: "◎",
    color: "#f7c96b",
    position: [0, 0, 0],
    summary: "A visual model of transparent community incentives for healthy mirror operators.",
    metaphor: "Sunlight and coins flow from a shared fountain toward eligible nodes.",
    reality:
      "The production mainnet-beta flow reads public on-chain state, accepts direct wallet-to-pool contributions, applies auditable eligibility, and sends unsigned plans to the instance owner’s local Qt signer. Development instances may explicitly select a test network.",
    status: "Mainnet-beta · external signer",
    statusTone: "live",
    bullets: [
      "Choose an immediate all-eligible-node contribution or add funds to the randomized trickle pool.",
      "Community members’ wallet keys never reach ForkMesh; the pool key stays only in the instance operator’s encrypted local Qt signer.",
      "Pending walletless allocations expire after 24 hours without moving funds.",
      "Visual coins are not guaranteed rewards or investments.",
      "Public pool, pending allocation, and finalized on-chain states are shown separately.",
    ],
    primary: { label: "Join / fund reward program", action: "reward" },
    secondary: { label: "Payout eligibility", href: "/mirror-payouts" },
  },
  {
    id: "campfire",
    label: "Members Circle",
    shortLabel: "Members Center",
    eyebrow: "OPEN COMMUNITY / 04",
    icon: "◎",
    color: "#ff9a5a",
    position: [0, 0, 130],
    summary:
      "An open circle turns the public member directory into a warm-to-cold lifecycle view without enclosing the people who gather there.",
    metaphor:
      "Active members gather nearest the hearth; cooling and dormant accounts sit in textured rings farther out.",
    reality:
      "Coarse recency leads a bounded score using aggregate World time, steps, Office visits, public contributions, nodes, verified email, and public wallet readiness. Accounts still unverified after seven days sit in the outer band. No exact last-seen timestamp is published.",
    status: "Live member lifecycle",
    statusTone: "live",
    bullets: [
      "The information sign shows totals for Power, Engaged, New, Casual, Cooling, Dormant, Never activated, Churned, and Email unverified 7d+ members.",
      "Distinct ground textures and matching bench colors run from ember-warm inner rings to cool and verification-pending outer rings.",
      "Member growth updates one sign texture and reuses shared avatar geometry.",
      "Private profiles and hidden-name visitors are never named on the sign.",
      "There is no enclosing shell, roof, or entry collision around members.",
    ],
    primary: { label: "Visit the Members Center", action: "campfire" },
  },
  {
    id: "repositories",
    label: "Repository portals",
    shortLabel: "Repositories",
    eyebrow: "CODE DISTRICT / 03",
    icon: "{ }",
    color: "#77d9ff",
    // The repository landmark, import kiosk, concrete apron, and live portal
    // ring share one east-district origin.
    position: [130, 0, 0],
    summary: "Authorized repositories form distinct perimeter portals with size-weighted file rings.",
    metaphor: "Each repository opens as its own three-dimensional sunburst around the city perimeter.",
    reality:
      "Public catalog records come from the relay API. A repository marked external or stub is not represented as actively mirrored.",
    status: "Live catalog connected",
    statusTone: "live",
    bullets: [
      "The live catalog distinguishes available mirrors, external stubs, private repositories authorized to the viewer, and unavailable hosts.",
      "Each catalog entry has a separate perimeter portal; arc width reflects commit-pinned bytes and each concentric ring adds one directory level.",
      "Signed state, checksums, and host health distinguish available mirrors.",
      "A healthy response proves availability, not automatic trust; identity and content integrity are verified separately.",
      "Authorized private repositories appear only to their owners and explicitly authorized collaborators, and are never broadcast into public presence.",
      "Normal repository traffic stays on HTTPS; the game socket never carries Git data. Clone, browse, release, and private-replica ciphertext use the authenticated direct-HTTPS routing layer.",
    ],
    primary: { label: "Browse repositories", action: "repositories" },
    secondary: { label: "Open 2D repository view", href: "/dashboard/repos" },
  },
  {
    id: "office",
    label: "ForkMesh Office",
    shortLabel: "Office",
    eyebrow: "COLLABORATION / 10",
    icon: "⌁",
    color: "#9ef7c6",
    position: [0, 0, -207],
    summary:
      "A five-story team campus connected to the World by a walkable bridge.",
    metaphor:
      "An office tower with a staffed lobby, team floors, an elevator, and a rooftop observatory.",
    reality:
      "Building admission is open to every visitor. Elevator grants come from server-verified organization team membership; avatar position never grants private data access.",
    status: "Open lobby · team floors",
    statusTone: "live",
    bullets: [
      "Everyone, including guests, can cross the bridge and enter the lobby. Signing in is required only for authenticated meetings and restricted team floors.",
      "Every registered user can visit the lobby and rooftop patio; Marketing unlocks for verified members of that team.",
      "Restricted elevator buttons unlock only for verified members of that team.",
      "Messages, room keys, tokens, and attachments never enter world presence.",
      "Crossing the Office door reveals signs and boards only on the active floor while keeping nearby Town visible through the glass.",
    ],
    primary: { label: "Cross the office bridge", action: "office" },
    secondary: { label: "Open full chat", href: "/chat" },
  },
];

export const TOUR_STEPS = [
  {
    landmark: "fountain",
    title: "Rewards are transparent, not custodial",
    copy: "The fountain is a metaphor. Actual transfers require an external self-custodial address and an explicit on-chain state.",
  },
  {
    landmark: "repositories",
    title: "Repositories become places",
    copy: "The repository explorer is seeded from the authorized live catalog. Live, mirrored, stub, and unavailable are separate states.",
  },
  {
    landmark: "organizations",
    title: "Permissions shape the city",
    copy: "Organization floors, offices, and private repo spaces remain gated by the same authorization rules as the standard interface.",
  },
  {
    landmark: "security",
    title: "Trust stays inspectable",
    copy: "Every metaphor has a technical panel. Scan results, mirror health, and reward visuals disclose their scope and limitations.",
  },
  {
    landmark: "office",
    title: "Collaboration has a place",
    copy: "Cross the bridge into one continuous five-story workplace. Login and team authorization still come from the normal account system.",
  },
];

export const THEME_OPTIONS = [
  { id: "world", label: "Full daylight" },
  { id: "rain", label: "Daylight + rain" },
  { id: "snow", label: "Daylight + snow" },
  { id: "winter", label: "Daylight + winter" },
  { id: "cyberpunk", label: "Local cyberpunk" },
  { id: "low-light", label: "Local low light" },
];

// A Supporting member perk: a shared, other-visitors-see-it-too outfit color
// that replaces the default country-flag shirt. Guests and Registered
// accounts keep the default flag shirt; the server drops this field back to
// "" for anyone whose trusted accountStatus isn't "Supporting member".
export const OUTFIT_COLOR_OPTIONS = [
  { id: "aurora", label: "Aurora green", color: "#39c783" },
  { id: "ember", label: "Ember orange", color: "#f2793a" },
  { id: "violet", label: "Signal violet", color: "#8b6df2" },
  { id: "gold", label: "Founder's gold", color: "#e0b23e" },
  { id: "slate", label: "Slate blue", color: "#3a6ea5" },
];

// The tailored outfit cuts world-scene.js can draw. Every visitor's public
// name seeds one of these deterministically; Supporting members may pin a
// specific cut that every visitor sees instead. The ids must stay in lockstep
// with WORLD_OUTFIT_STYLE_VALUES in world/src/world.py.
export const OUTFIT_STYLE_OPTIONS = [
  { id: "sash", label: "Diagonal sash" },
  { id: "racer", label: "Racing stripes" },
  { id: "chevron", label: "Chevron stack" },
  { id: "argyle", label: "Argyle knit" },
  { id: "circuit", label: "Circuit board" },
  { id: "pixel", label: "Pixel mosaic" },
  { id: "waves", label: "Terminal waves" },
  { id: "starfield", label: "Night deploy" },
  { id: "hex", label: "Hex grid" },
  { id: "bolt", label: "Hotfix bolt" },
  { id: "tartan", label: "Merge tartan" },
  { id: "binary", label: "Binary rain" },
];

export const AVAILABILITY_OPTIONS = [
  { id: "online", label: "Online" },
  { id: "away", label: "Away" },
  { id: "inactive", label: "Inactive" },
  { id: "recent", label: "Last active recently" },
  { id: "offline-operator", label: "Offline mirror operator" },
  { id: "returning", label: "Returning contributor" },
];

export const ACTIVITY_OPTIONS = [
  { id: "automatic", label: "Automatic, generalized" },
  { id: "exploring-town-square", label: "Exploring the Town Square" },
  { id: "viewing-repository", label: "Viewing a repository" },
  { id: "reading-documentation", label: "Reading documentation" },
  { id: "visiting-organization", label: "Visiting an organization" },
  { id: "visiting-office", label: "Visiting the ForkMesh Office" },
  { id: "browsing-code-visualization", label: "Browsing a code visualization" },
];

export const WORLD_STATUS_NOTE_MAX = 20;

// The picker is intentionally broad while the adjacent free-form emoji field
// accepts any single valid Unicode emoji sequence, including flags, skin tones,
// keycaps, and joined family/profession sequences. Keeping the picker data local
// means opening it never sends a search term or profile hint to a third party.
export const WORLD_EMOJI_CATEGORIES = Object.freeze([
  {
    id: "faces",
    label: "Faces",
    emoji: [
      "😀", "😃", "😄", "😁", "😆", "😅", "😂", "🙂", "🙃", "😉",
      "😊", "🥰", "😍", "🤩", "😘", "😎", "🤓", "🧐", "🤔", "🫡",
      "🤗", "🤫", "🤭", "😴", "🥳", "😭", "😤", "😱", "😇", "🤠",
    ],
  },
  {
    id: "gestures",
    label: "People",
    emoji: [
      "👋", "🤚", "🖐️", "✋", "🖖", "🫶", "👌", "🤌", "🤏", "✌️",
      "🤞", "🫰", "🤟", "🤘", "🤙", "👈", "👉", "👆", "👇", "☝️",
      "👍", "👎", "✊", "👊", "🤝", "🙏", "💪", "🧠", "🧑‍💻", "🧑🏽‍🔬",
    ],
  },
  {
    id: "nature",
    label: "Nature",
    emoji: [
      "🐶", "🐱", "🐭", "🐹", "🐰", "🦊", "🐻", "🐼", "🐨", "🐯",
      "🦁", "🐮", "🐷", "🐸", "🐵", "🦄", "🐝", "🦋", "🐙", "🐢",
      "🌱", "🌿", "🍀", "🌵", "🌲", "🌳", "🌴", "🌻", "🌈", "🔥",
    ],
  },
  {
    id: "food",
    label: "Food",
    emoji: [
      "🍏", "🍎", "🍊", "🍋", "🍉", "🍇", "🍓", "🫐", "🍒", "🥝",
      "🍅", "🥑", "🌽", "🥕", "🥐", "🍞", "🧀", "🍕", "🌮", "🍜",
      "🍣", "🍪", "🍩", "🍫", "☕", "🫖", "🧃", "🥤", "🧋", "🍿",
    ],
  },
  {
    id: "activity",
    label: "Activity",
    emoji: [
      "⚽", "🏀", "🏈", "⚾", "🎾", "🏐", "🏓", "🏸", "🥅", "⛳",
      "🛹", "🛼", "🚲", "🏆", "🥇", "🎯", "🎮", "🎲", "🧩", "🎨",
      "🎭", "🎸", "🎹", "🎧", "🎤", "📷", "🎬", "🚀", "🧘", "🏕️",
    ],
  },
  {
    id: "travel",
    label: "Travel",
    emoji: [
      "🚗", "🚕", "🚌", "🚎", "🏎️", "🚓", "🚑", "🚒", "🚜", "🛵",
      "🚆", "🚇", "🚄", "✈️", "🛫", "🛸", "🚁", "⛵", "🚢", "⚓",
      "🗺️", "🧭", "🏔️", "🏝️", "🏙️", "🌋", "🌍", "🌎", "🌏", "🌌",
    ],
  },
  {
    id: "objects",
    label: "Objects",
    emoji: [
      "⌚", "📱", "💻", "⌨️", "🖥️", "🖱️", "💾", "💿", "📡", "🔋",
      "🔌", "💡", "🔦", "🧰", "🔧", "🔨", "⚙️", "🧲", "🧪", "🔬",
      "🔭", "📚", "📝", "📌", "📎", "🔐", "🔑", "🎁", "💎", "🪄",
    ],
  },
  {
    id: "symbols",
    label: "Symbols",
    emoji: [
      "❤️", "🧡", "💛", "💚", "💙", "💜", "🖤", "🤍", "🤎", "💔",
      "❣️", "💕", "💯", "💢", "💬", "💭", "💤", "✨", "⭐", "🌟",
      "⚡", "✅", "❌", "❓", "❗", "♻️", "⚠️", "🔔", "🔕", "♾️",
    ],
  },
  {
    id: "flags",
    label: "Flags",
    emoji: [
      "🏳️", "🏴", "🏁", "🚩", "🏳️‍🌈", "🏳️‍⚧️", "🇦🇺", "🇧🇷", "🇨🇦", "🇨🇳",
      "🇪🇺", "🇫🇷", "🇩🇪", "🇮🇳", "🇮🇪", "🇮🇹", "🇯🇵", "🇲🇽", "🇳🇿", "🇳🇬",
      "🇰🇷", "🇿🇦", "🇪🇸", "🇸🇪", "🇺🇦", "🇬🇧", "🇺🇸", "🇺🇳", "🇵🇷", "🇸🇬",
    ],
  },
]);

const WORLD_FLAG_EMOJI_RE = /^\p{Regional_Indicator}{2}$/u;
const WORLD_KEYCAP_EMOJI_RE = /^[#*0-9]\uFE0F?\u20E3$/u;
const WORLD_PICTOGRAPH_EMOJI_RE =
  /^(?:\p{Extended_Pictographic}(?:\uFE0E|\uFE0F)?(?:\p{Emoji_Modifier})?(?:[\u{E0020}-\u{E007E}]+\u{E007F})?)(?:\u200D\p{Extended_Pictographic}(?:\uFE0E|\uFE0F)?(?:\p{Emoji_Modifier})?)*$/u;
const WORLD_STATUS_NOTE_RE =
  /^[\p{L}\p{N}][\p{L}\p{M}\p{N}'’-]*$/u;

export function normalizeWorldEmoji(value) {
  const emoji = String(value || "").trim().normalize("NFC");
  if (!emoji || [...emoji].length > 24 || emoji.length > 48) return "";
  return (
    WORLD_FLAG_EMOJI_RE.test(emoji) ||
    WORLD_KEYCAP_EMOJI_RE.test(emoji) ||
    WORLD_PICTOGRAPH_EMOJI_RE.test(emoji)
  )
    ? emoji
    : "";
}

export function normalizeWorldStatusNote(value) {
  const note = String(value || "").trim().normalize("NFKC");
  if (
    !note ||
    [...note].length > WORLD_STATUS_NOTE_MAX ||
    !WORLD_STATUS_NOTE_RE.test(note)
  ) {
    return "";
  }
  return note;
}

export function normalizeWorldStatus(emojiValue, noteValue) {
  const emoji = normalizeWorldEmoji(emojiValue);
  return {
    emoji,
    note: emoji ? normalizeWorldStatusNote(noteValue) : "",
  };
}

export const WORLD_REGIONS = [
  { id: "east", label: "East Campus", phase: "Local daylight view" },
  { id: "central", label: "Central Campus", phase: "Local daylight view" },
  { id: "west", label: "West Campus", phase: "Local daylight view" },
];

export const FORKMESH_SONG = Object.freeze({
  name: "ForkMesh Forever (Indie Pop)",
  videoUrl: "https://www.youtube.com/watch?v=EsQMZuS6lbE",
  licenseUrl:
    "https://github.com/forkmesh/forkmesh/blob/main/www/docs/world-soundtrack-license.md",
});

export function landmarkById(id) {
  return LANDMARKS.find((landmark) => landmark.id === id) || LANDMARKS[0];
}

export function detectClient() {
  const ua = navigator.userAgent || "";
  const platform = navigator.userAgentData?.platform || navigator.platform || "";
  let browser = "Browser";
  if (/Edg\//.test(ua)) browser = "Edge";
  else if (/Firefox\/|FxiOS\//.test(ua)) browser = "Firefox";
  else if (/CriOS\//.test(ua)) browser = "Chrome";
  else if (/Chrome\//.test(ua)) browser = "Chrome";
  else if (/Safari\//.test(ua)) browser = "Safari";

  let os = "Device";
  if (/Android/i.test(ua)) os = "Android";
  else if (/iPhone|iPad|iPod/i.test(ua)) os = "iOS";
  else if (/CrOS/i.test(ua)) os = "ChromeOS";
  else if (/Mac/i.test(platform)) os = "macOS";
  else if (/Win/i.test(platform)) os = "Windows";
  else if (/Linux/i.test(platform) || /Linux/i.test(ua)) os = "Linux";

  return { browser, os, touch: navigator.maxTouchPoints > 0 };
}

// Derived per directory member on every lounge/badge refresh; the two-letter
// input space is tiny and the composed string is immutable, so memoize
// instead of re-running the spread + fromCodePoint on each call.
const FLAG_EMOJI_MEMO = new Map();
export function flagEmoji(code) {
  const normalized = String(code || "").trim().toUpperCase();
  if (!/^[A-Z]{2}$/.test(normalized)) return "◌";
  let flag = FLAG_EMOJI_MEMO.get(normalized);
  if (!flag) {
    flag = String.fromCodePoint(
      ...[...normalized].map((char) => 127397 + char.charCodeAt(0)),
    );
    FLAG_EMOJI_MEMO.set(normalized, flag);
  }
  return flag;
}

export function sanitizePresenceText(value, fallback, max = 28) {
  const clean = String(value || "")
    .replace(/[^\p{L}\p{N} ._@-]/gu, "")
    .trim()
    .slice(0, max);
  return clean || fallback;
}

// Shared by the seated pose and the presence frame so other visitors can render
// a bench sitter sitting rather than standing on the plank. The shell must keep
// it out of the landmark-proximity activity label, and reads it before the
// renderer loads, so it lives with the data both sides already import.
export const CAMPFIRE_SEATED_ACTIVITY = "sitting beside the campfire";
// Shared by the swing-set ride and the presence frame for the same reason: the
// shell keeps it out of the landmark-proximity activity label while a visitor
// is riding one of the town swings.
export const SWING_RIDING_ACTIVITY = "swinging on the town swing set";
