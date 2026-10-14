const fs = require("fs"); const vm = require("vm");
const [page, out] = process.argv.slice(2);
const window = {}; vm.runInNewContext(fs.readFileSync(page, "utf8"), { window });
const R = window.ForkMeshPeopleRings;
const now = 1791936000000;
const day = 86400000, hour = 3600000;
const users = [
  { name: "ada", activityBucket: "hour", totalActiveMs: 50 * hour, totalSteps: 120000, officeVisits: 25, issues: 10, pulls: 20, commits: 100, discussions: 3, emailVerified: true, solana: "So1anaAddr", nodes: ["n1", "n2", "n3"], createdAt: now - 400 * day, avatarThumb: "data:image/webp;base64,UklGRg==" },
  { name: "Brook", activityBucket: "24h", totalActiveMs: 2 * hour, totalSteps: 600, emailVerified: true, createdAt: now - 90 * day },
  { name: "cy", activityBucket: "5h", totalActiveMs: 13 * hour, totalSteps: 26000, officeVisits: 6, commits: 7, emailVerified: true, nodes: ["n1"], createdAt: now - 200 * day },
  { name: "dee", activityBucket: "5d", emailVerified: true, createdAt: now - 60 * day },
  { name: "eli", activityBucket: "", createdAt: now - 3 * day },
  { name: "fay", activityBucket: "stale", createdAt: now - 13 * day - 23 * hour },
  { name: "gus", activityBucket: "30d", createdAt: now - 100 * day },
  { name: "hal", activityBucket: "stale", issues: 1, createdAt: now - 300 * day },
  { name: "ivy", activityBucket: "120d", createdAt: now - 300 * day, emailVerified: true },
  { name: "jo", activityBucket: "", createdAt: 0 },
  { name: "kit", activityBucket: "60d", totalSteps: 1, createdAt: now - 30 * day },
  { name: "lux", activityBucket: "3d", createdAt: now - 2 * day },
  { name: "node-1", kind: "node", activityBucket: "hour" },
  { name: "", activityBucket: "hour" },
  { name: "max", activityBucket: "hour", totalActiveMs: 5 * hour, createdAt: now - 20 * day, emailVerified: true, solana: " ", userStatus: { emoji: "🔥", text: "shipping" } },
];
const cases = users.map((user) => {
  const member = R.parseMember(user);
  return { user, member: member ? { name: member.name, contributions: member.contributions, nodeCount: member.nodeCount, hasPayoutAddress: member.hasPayoutAddress, avatar: member.avatar ? member.avatar.slice(member.avatar.indexOf(",") + 1) : "" } : null, score: member ? R.score(member) : null, ring: member ? R.ring(member, now) : null };
});
const members = users.map(R.parseMember).filter(Boolean);
const order = R.sections(members, now).map((s) => ({ ring: s.ring, names: s.people.map((p) => p.member.name) }));
const layout = [360, 560].map((size) => ({ size, seat: size === 360 ? 30 : 40, label: size === 360 ? 72 : 104 })).map((l) => ({ ...l, radii: [0,1,2,3,4].map((i) => +R.radius(i, l.size).toFixed(6)), capacities: [0,1,2,3,4].map((i) => R.capacity(i, l.size, l.seat, l.label)) }));
fs.writeFileSync(out, JSON.stringify({ now, rings: R.RINGS.map((r) => r.id), cases, order, layout }, null, 2) + "\n");
