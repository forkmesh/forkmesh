import { AVATAR_SOLANA_ADDRESS_RE } from "./avatar-shared.js";
import { clamp } from "./object-utils.js";

// The circle is a lifecycle view, not a social partition. These states are
// mutually exclusive and ordered by their distance from the fire. New members
// stay near the warm rings as "kindling" while they build enough history to be
// judged on the normal recency ladder; never-activated and formerly-engaged
// accounts remain separate because they point to different product problems.
// Past a seven-day grace period, an unverified account that has never stepped
// into the World owns the outermost row so the verification backlog stays
// visible without displacing members who have already entered the gathering.
export const MEMBER_ENGAGEMENT_STATES = Object.freeze([
  Object.freeze({
    id: "world-step",
    label: "World step",
    bench: "#e8732f",
    legs: "#733010",
    ground: "#b84c22",
    accent: "#ffe08a",
    texture: "trail",
  }),
  Object.freeze({
    id: "power",
    label: "Power",
    bench: "#c94a24",
    legs: "#642313",
    ground: "#9e351f",
    accent: "#ffb347",
    texture: "embers",
  }),
  Object.freeze({
    id: "engaged",
    label: "Engaged",
    bench: "#347d53",
    legs: "#193f2a",
    ground: "#315f42",
    accent: "#9ef7c6",
    texture: "sprouts",
  }),
  Object.freeze({
    id: "new",
    label: "New / Kindling",
    bench: "#c99b3f",
    legs: "#664c1d",
    ground: "#8a6a2c",
    accent: "#ffe6a6",
    texture: "kindling",
  }),
  Object.freeze({
    id: "casual",
    label: "Casual",
    bench: "#9b7637",
    legs: "#4d3919",
    ground: "#70552e",
    accent: "#f4cc78",
    texture: "footprints",
  }),
  Object.freeze({
    id: "cooling",
    label: "Cooling",
    bench: "#995744",
    legs: "#4b2921",
    ground: "#704239",
    accent: "#e69a72",
    texture: "leaves",
  }),
  Object.freeze({
    id: "dormant",
    label: "Dormant",
    bench: "#4c6d82",
    legs: "#263743",
    ground: "#3c5364",
    accent: "#87bdd8",
    texture: "pebbles",
  }),
  Object.freeze({
    id: "never-activated",
    label: "Never activated",
    bench: "#655b7b",
    legs: "#322d3e",
    ground: "#4d465e",
    accent: "#b9a9d6",
    texture: "unlit",
  }),
  Object.freeze({
    id: "churned",
    label: "Churned",
    bench: "#789db0",
    legs: "#3b4f59",
    ground: "#526f7d",
    accent: "#d8f2ff",
    texture: "frost",
  }),
  Object.freeze({
    id: "unverified",
    label: "Email unverified 7d+",
    bench: "#70454c",
    legs: "#392126",
    ground: "#56383e",
    accent: "#ff9b9b",
    texture: "crosses",
  }),
]);

export const MEMBER_ENGAGEMENT_BY_ID = new Map(
  MEMBER_ENGAGEMENT_STATES.map((state, order) => [
    state.id,
    Object.freeze({ ...state, order }),
  ]),
);
export const MEMBER_CIRCLE_AVATAR_BAND = Object.freeze({
  id: "custom-avatar",
  label: "Custom avatar",
  bench: "#d9a441",
  legs: "#6b4d16",
  ground: "#8c6a2a",
  accent: "#fff2b8",
  texture: "portraits",
});

export function memberCircleBandSpec(id) {
  return id === MEMBER_CIRCLE_AVATAR_BAND.id
    ? MEMBER_CIRCLE_AVATAR_BAND
    : MEMBER_ENGAGEMENT_BY_ID.get(String(id || ""));
}

export function memberHasCustomAvatar(member) {
  return String(member?.avatar || "").trim().length > 0;
}

const MEMBER_ENGAGEMENT_NEW_MS = 14 * 24 * 60 * 60 * 1000;
const MEMBER_EMAIL_VERIFICATION_GRACE_MS = 7 * 24 * 60 * 60 * 1000;
const MEMBER_ACTIVITY_SCORE = Object.freeze({
  hour: 48,
  "5h": 46,
  "24h": 42,
  "3d": 36,
  "5d": 32,
  "10d": 25,
  "30d": 18,
  "60d": 11,
  "120d": 5,
  stale: 0,
});

function memberMetricPoints(value, tiers) {
  const total = Math.max(0, Number(value) || 0);
  for (const [minimum, points] of tiers) {
    if (total >= minimum) return points;
  }
  return 0;
}

function memberContributionTotal(member) {
  return ["issues", "pulls", "commits", "discussions"].reduce(
    (total, key) => total + Math.max(0, Number(member?.[key]) || 0),
    0,
  );
}

// Public, aggregate-only inputs deliberately drive this score: activity
// recency, lifetime time and steps, visits, public contribution tallies, node
// ownership, email verification, and an optional public payout address.
// Country/client fields and email-delivery results are intentionally excluded;
// neither is member engagement and using them would make the rings misleading.
// Taking even one step in the World earns the dedicated innermost ring. Other
// concrete participation (Office visits or public contributions) floors a
// verified member at Casual instead of leaving them in an inactive outer band
// solely because their latest activity bucket has gone stale.
export function memberEngagementProfile(member = {}, now = Date.now()) {
  const activityBucket = String(member?.activityBucket || "");
  const activeNow = member?.away === true || member?.activeNow === true;
  const totalActiveMs = Math.max(
    0,
    Number(member?.totalActiveMs ?? member?.activeMs) || 0,
  );
  const totalSteps = Math.max(0, Number(member?.totalSteps) || 0);
  const visitCount = Math.max(0, Number(member?.visitCount) || 0);
  const officeVisits = Math.max(0, Number(member?.officeVisits) || 0);
  const contributions = memberContributionTotal(member);
  const nodeCount = Math.min(
    6,
    Array.isArray(member?.nodes) ? member.nodes.length : 0,
  );
  const emailVerified = member?.emailVerified === true;
  const hasSolana = AVATAR_SOLANA_ADDRESS_RE.test(
    String(member?.solana || ""),
  );
  const createdAt = Math.max(0, Number(member?.createdAt) || 0);
  const ageMs = createdAt > 0 ? Math.max(0, Number(now) - createdAt) : Infinity;
  const recencyPoints = activeNow
    ? 52
    : MEMBER_ACTIVITY_SCORE[activityBucket] ?? 0;
  const depthPoints =
    memberMetricPoints(totalActiveMs, [
      [40 * 60 * 60 * 1000, 22],
      [12 * 60 * 60 * 1000, 17],
      [4 * 60 * 60 * 1000, 12],
      [60 * 60 * 1000, 7],
      [10 * 60 * 1000, 3],
    ]) +
    memberMetricPoints(totalSteps, [
      [100000, 13],
      [25000, 10],
      [5000, 7],
      [500, 4],
      [1, 1],
    ]) +
    memberMetricPoints(visitCount, [
      [30, 7],
      [10, 5],
      [3, 3],
      [1, 1],
    ]) +
    memberMetricPoints(officeVisits, [
      [20, 6],
      [5, 4],
      [1, 2],
    ]) +
    Math.min(14, Math.floor(Math.log2(contributions + 1) * 3.5));
  const readinessPoints =
    (emailVerified ? 4 : 0) +
    (hasSolana ? 4 : 0) +
    Math.min(7, nodeCount * 3);
  const score = clamp(
    Math.round(recencyPoints + depthPoints + readinessPoints),
    0,
    100,
  );
  const emailVerificationOverdue =
    createdAt > 0 &&
    !emailVerified &&
    ageMs >= MEMBER_EMAIL_VERIFICATION_GRACE_MS;
  const hasRecordedParticipation =
    totalSteps > 0 || officeVisits > 0 || contributions > 0;
  const hasTakenWorldStep = totalSteps > 0;
  const hasActivityHistory =
    activeNow ||
    (activityBucket && activityBucket !== "stale") ||
    totalActiveMs >= 10 * 60 * 1000 ||
    hasRecordedParticipation ||
    visitCount >= 2 ||
    nodeCount > 0;

  let stateId = "churned";
  if (hasTakenWorldStep) {
    stateId = "world-step";
  } else if (emailVerificationOverdue) {
    stateId = "unverified";
  } else if (ageMs < MEMBER_ENGAGEMENT_NEW_MS) {
    stateId = "new";
  } else if (!hasActivityHistory) {
    stateId = "never-activated";
  } else if (
    (activeNow || ["hour", "5h", "24h"].includes(activityBucket)) &&
    score >= 78
  ) {
    stateId = "power";
  } else if (
    activeNow ||
    ["hour", "5h", "24h", "3d", "5d"].includes(activityBucket)
  ) {
    stateId = "engaged";
  } else if (
    ["10d", "30d"].includes(activityBucket) ||
    hasRecordedParticipation
  ) {
    stateId = "casual";
  } else if (activityBucket === "60d") {
    stateId = "cooling";
  } else if (activityBucket === "120d") {
    stateId = "dormant";
  }

  const state = MEMBER_ENGAGEMENT_BY_ID.get(stateId);
  return {
    id: state.id,
    label: state.label,
    order: state.order,
    score,
    activeNow,
    activityBucket,
    totalActiveMs,
    totalSteps,
    visitCount,
    officeVisits,
    contributions,
    nodeCount,
    emailVerified,
    emailVerificationOverdue,
    hasSolana,
    hasTakenWorldStep,
    hasRecordedParticipation,
    hasActivityHistory,
  };
}

export function rankedMembersByEngagement(members = [], now = Date.now()) {
  return (Array.isArray(members) ? members : [])
    .map((member) => ({
      ...member,
      engagement: memberEngagementProfile(member, now),
    }))
    .sort(
      (left, right) =>
        left.engagement.order - right.engagement.order ||
        right.engagement.score - left.engagement.score ||
        right.engagement.totalActiveMs - left.engagement.totalActiveMs ||
        right.engagement.officeVisits - left.engagement.officeVisits ||
        right.engagement.totalSteps - left.engagement.totalSteps ||
        right.engagement.contributions - left.engagement.contributions ||
        String(left.name || "").localeCompare(String(right.name || "")),
    );
}
