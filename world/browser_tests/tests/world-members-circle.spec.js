// The Members Circle stands for the whole public directory: every account the
// roster lists gets a figure in the rings, the rings grow to hold them, and the
// paved clearing widens if the roster ever outgrows its own pavement. These
// contracts count the figures the scene actually builds rather than trusting
// the roster the sign quotes.
const { test, expect } = require("@playwright/test");

const CAMPFIRE_Z = 130;

function roster(count) {
  return Array.from({ length: count }, (_, index) => ({
    name: `circle-member-${index}`,
    countryCode: ["US", "FR", "DE", "JP", "BR"][index % 5],
    flag: "US",
    browser: "Firefox",
    os: "Linux",
    emailVerified: true,
    visitCount: index,
    createdAt: 1_700_000_000_000 + index * 1000,
    totalActiveMs: index * 60_000,
    totalSteps: 0,
    nodes: [],
    status: "",
    activityBucket: "hour",
  }));
}

async function standAtTheFire(page) {
  await page.setViewportSize({ width: 1200, height: 800 });
  await page.goto("/world/?nointro=1");
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      Boolean(shell?.world?.renderer?.domElement) &&
      Number(shell.world.renderer.info?.render?.frame || 0) > 0
    );
  });
  // The clearing is a compact district, so a visitor has to be standing in it
  // before its contents are resident.
  await page.evaluate((z) => {
    document
      .querySelector("forkmesh-world")
      .world.setSpawn({ x: 0, z: z + 16, space: "town-square" });
  }, CAMPFIRE_Z);
  await page.waitForTimeout(1200);
}

// The World's own roster poll prunes figures the live directory does not list,
// so a planted roster is measured in the same evaluation that plants it.
async function plantRoster(page, count) {
  return page.evaluate(
    ({ members, campfireZ }) => {
      const shell = document.querySelector("forkmesh-world");
      const { world } = shell;
      world.setPaused(true);
      world.updateMemberLounge(members, members.length, members, 0);
      const dirt = world.scene.getObjectByName("campfire-member-circle-dirt");
      const places = [];
      world.scene.traverse((child) => {
        if (!String(child.name || "").startsWith("avatar:member:")) return;
        places.push({
          name: child.name,
          radius: Math.hypot(child.position.x, child.position.z - campfireZ),
          x: child.position.x,
          z: child.position.z,
          scale: child.scale.x,
        });
      });
      let closest = Infinity;
      let closestInRing = Infinity;
      for (let left = 0; left < places.length; left += 1) {
        for (let right = left + 1; right < places.length; right += 1) {
          const gap = Math.hypot(
            places[left].x - places[right].x,
            places[left].z - places[right].z,
          );
          closest = Math.min(closest, gap);
          // Shoulder-to-shoulder neighbours share a ring; the rings themselves
          // stand a figure's depth apart, which is a shorter distance.
          if (
            Math.abs(places[left].radius - places[right].radius) < 0.01
          ) {
            closestInRing = Math.min(closestInRing, gap);
          }
        }
      }
      return {
        fed: members.length,
        figures: places.length,
        unique: new Set(places.map((place) => place.name)).size,
        signedCount: dirt.parent.userData.memberFigureCount,
        outermost: Math.max(0, ...places.map((place) => place.radius)),
        innermost: Math.min(Infinity, ...places.map((place) => place.radius)),
        pavedRadius: dirt.scale.x,
        closestPair: closest,
        closestInRing,
        plateScale: world.scene.getObjectByName(
          `avatar:member:${members[0].name}`,
        ).userData.plateScale,
        chestTexture: (() => {
          const texture = world.scene.getObjectByName(
            `avatar:member:${members[0].name}`,
          ).userData.badge.material.map;
          return {
            anisotropy: texture.anisotropy,
            generateMipmaps: texture.generateMipmaps,
            minFilter: texture.minFilter,
          };
        })(),
        // Every figure is built at the player's own scale, so the smallest one
        // in the circle is still a full-size person.
        smallestFigure: Math.min(
          Infinity,
          ...places.map((place) => place.scale),
        ),
      };
    },
    { members: roster(count), campfireZ: CAMPFIRE_Z },
  );
}

test("member lifecycle states use every public engagement signal", async ({
  page,
}) => {
  await standAtTheFire(page);
  const result = await page.evaluate(async () => {
    const now = 1_800_000_000_000;
    const day = 24 * 60 * 60 * 1000;
    const wallet = "11111111111111111111111111111111";
    const { memberEngagementProfile } = await import(
      "/world/world-scene.js"
    );
    const { normalizeMemberDirectory } = await import("/world/world.js");
    const profile = (fields) =>
      memberEngagementProfile(
        { createdAt: now - 365 * day, emailVerified: true, ...fields },
        now,
      );
    const baseline = profile({
      activityBucket: "3d",
      totalActiveMs: 60 * 60 * 1000,
    });
    const complete = profile({
      activityBucket: "3d",
      totalActiveMs: 60 * 60 * 1000,
      totalSteps: 500,
      visitCount: 12,
      officeVisits: 6,
      issues: 3,
      pulls: 2,
      commits: 18,
      discussions: 4,
      nodes: ["node-a", "node-b"],
      emailVerified: true,
      solana: wallet,
    });
    const normalized = normalizeMemberDirectory({
      users: [
        {
          name: "all-signals",
          activityBucket: "60d",
          totalSteps: 1234,
          officeVisits: 7,
          issues: 3,
          pulls: 2,
          commits: 18,
          discussions: 4,
          emailVerified: true,
          solana: wallet,
        },
      ],
    })[0];
    return {
      states: {
        worldStep: memberEngagementProfile(
          {
            createdAt: now - 8 * day,
            activityBucket: "stale",
            totalSteps: 1,
          },
          now,
        ).id,
        power: profile({
          activityBucket: "hour",
          totalActiveMs: 50 * 60 * 60 * 1000,
          visitCount: 40,
          commits: 32,
          nodes: ["node-a", "node-b"],
          emailVerified: true,
          solana: wallet,
        }).id,
        engaged: baseline.id,
        new: memberEngagementProfile(
          { createdAt: now - 2 * day, activityBucket: "stale" },
          now,
        ).id,
        casual: profile({
          activityBucket: "30d",
          totalActiveMs: 2 * 60 * 60 * 1000,
        }).id,
        cooling: profile({
          activityBucket: "60d",
          totalActiveMs: 2 * 60 * 60 * 1000,
        }).id,
        dormant: profile({
          activityBucket: "120d",
          totalActiveMs: 2 * 60 * 60 * 1000,
        }).id,
        neverActivated: profile({
          activityBucket: "stale",
          emailVerified: true,
          solana: wallet,
        }).id,
        churned: profile({
          activityBucket: "stale",
          totalActiveMs: 2 * 60 * 60 * 1000,
        }).id,
        unverified: memberEngagementProfile({
          createdAt: now - 8 * day,
          activityBucket: "hour",
        }, now).id,
      },
      participation: {
        steps: profile({ activityBucket: "stale", totalSteps: 1 }).id,
        office: profile({ activityBucket: "stale", officeVisits: 1 }).id,
        contribution: profile({ activityBucket: "stale", pulls: 1 }).id,
      },
      baselineScore: baseline.score,
      completeScore: complete.score,
      completeSignals: {
        contributions: complete.contributions,
        officeVisits: complete.officeVisits,
        nodeCount: complete.nodeCount,
        emailVerified: complete.emailVerified,
        hasSolana: complete.hasSolana,
      },
      normalized: {
        activityBucket: normalized.activityBucket,
        totalSteps: normalized.totalSteps,
        officeVisits: normalized.officeVisits,
        issues: normalized.issues,
        pulls: normalized.pulls,
        commits: normalized.commits,
        discussions: normalized.discussions,
        emailVerified: normalized.emailVerified,
        solana: normalized.solana,
      },
    };
  });

  expect(result.states).toEqual({
    worldStep: "world-step",
    power: "power",
    engaged: "engaged",
    new: "new",
    casual: "casual",
    cooling: "cooling",
    dormant: "dormant",
    neverActivated: "never-activated",
    churned: "churned",
    unverified: "unverified",
  });
  expect(result.participation).toEqual({
    steps: "world-step",
    office: "casual",
    contribution: "casual",
  });
  expect(result.completeScore).toBeGreaterThan(result.baselineScore);
  expect(result.completeSignals).toEqual({
    contributions: 27,
    officeVisits: 6,
    nodeCount: 2,
    emailVerified: true,
    hasSolana: true,
  });
  expect(result.normalized).toEqual({
    activityBucket: "60d",
    totalSteps: 1234,
    officeVisits: 7,
    issues: 3,
    pulls: 2,
    commits: 18,
    discussions: 4,
    emailVerified: true,
    solana: "11111111111111111111111111111111",
  });
});

test("warm-to-cold bands order members and recolor their ground and benches", async ({
  page,
}) => {
  await standAtTheFire(page);
  const result = await page.evaluate((campfireZ) => {
    const now = Date.now();
    const day = 24 * 60 * 60 * 1000;
    const old = now - 365 * day;
    const members = [
      {
        name: "state-world-step",
        createdAt: old,
        activityBucket: "stale",
        totalSteps: 1,
        emailVerified: true,
      },
      {
        name: "state-churned",
        createdAt: old,
        activityBucket: "stale",
        totalActiveMs: 2 * 60 * 60 * 1000,
        emailVerified: true,
      },
      {
        name: "state-casual",
        createdAt: old,
        activityBucket: "30d",
        totalActiveMs: 2 * 60 * 60 * 1000,
        emailVerified: true,
      },
      {
        name: "state-power",
        createdAt: old,
        activityBucket: "hour",
        totalActiveMs: 50 * 60 * 60 * 1000,
        visitCount: 40,
        commits: 32,
        nodes: ["node-a"],
        emailVerified: true,
        solana: "11111111111111111111111111111111",
      },
      {
        name: "state-never-activated",
        createdAt: old,
        activityBucket: "stale",
        emailVerified: true,
      },
      {
        name: "state-engaged",
        createdAt: old,
        activityBucket: "3d",
        totalActiveMs: 60 * 60 * 1000,
        emailVerified: true,
      },
      {
        name: "state-dormant",
        createdAt: old,
        activityBucket: "120d",
        totalActiveMs: 2 * 60 * 60 * 1000,
        emailVerified: true,
      },
      {
        name: "state-new",
        createdAt: now - 2 * day,
        activityBucket: "stale",
      },
      {
        name: "state-cooling",
        createdAt: old,
        activityBucket: "60d",
        totalActiveMs: 2 * 60 * 60 * 1000,
        emailVerified: true,
      },
      {
        name: "state-unverified",
        createdAt: now - 8 * day,
        activityBucket: "hour",
      },
    ];
    const shell = document.querySelector("forkmesh-world");
    const { world } = shell;
    world.setPaused(true);
    world.updateMemberLounge(members, members.length, members, 0);
    const dirt = world.scene.getObjectByName("campfire-member-circle-dirt");
    const campfire = dirt.parent;
    const seats = world.scene.getObjectByName("campfire-member-bench-seats");
    const color = seats.material.color.clone();
    const legend = campfire.userData.engagementLegend;
    const figures = members
      .map((member) => {
        const figure = world.scene.getObjectByName(
          `avatar:member:${member.name}`,
        );
        return {
          name: member.name,
          state: figure.userData.memberEngagement.id,
          order: figure.userData.memberEngagement.order,
          radius: Math.hypot(
            figure.position.x,
            figure.position.z - campfireZ,
          ),
        };
      })
      .sort((left, right) => left.radius - right.radius);
    const benchColors = Object.fromEntries(
      members.map((member) => {
        const index = campfire.userData.seatByName.get(member.name);
        seats.getColorAt(index, color);
        return [member.name.replace("state-", ""), `#${color.getHexString()}`];
      }),
    );
    const expectedBenchColors = Object.fromEntries(
      legend.map((entry) => [entry.id, entry.bench]),
    );
    const bands = campfire.userData.engagementBandLayer.children
      .filter((child) => child.name.startsWith("campfire-member-engagement-band-"))
      .map((child) => ({
        state: child.userData.engagement,
        count: child.userData.memberCount,
        inner: child.userData.innerRadius,
        outer: child.userData.outerRadius,
        texture: child.material.map.uuid,
      }));
    return {
      figures,
      benchColors,
      expectedBenchColors,
      bands,
      counts: campfire.userData.engagementCounts,
      legend: legend.map(({ id, texture }) => ({ id, texture })),
      pavedRadius: dirt.scale.x,
    };
  }, CAMPFIRE_Z);

  const stateOrder = [
    "world-step",
    "power",
    "engaged",
    "new",
    "casual",
    "cooling",
    "dormant",
    "never-activated",
    "churned",
    "unverified",
  ];
  expect(result.figures.map((figure) => figure.state)).toEqual(stateOrder);
  expect(result.figures.map((figure) => figure.order)).toEqual(
    stateOrder.map((_, index) => index),
  );
  for (let index = 1; index < result.figures.length; index += 1) {
    expect(result.figures[index].radius).toBeGreaterThan(
      result.figures[index - 1].radius,
    );
  }
  const worldStepGap = result.figures[1].radius - result.figures[0].radius;
  const powerGap = result.figures[2].radius - result.figures[1].radius;
  const nextGap = result.figures[3].radius - result.figures[2].radius;
  expect(worldStepGap).toBeGreaterThan(nextGap + 0.4);
  expect(powerGap).toBeGreaterThan(nextGap + 0.4);
  expect(result.bands.map((band) => band.state)).toEqual(stateOrder);
  expect(result.bands.every((band) => band.count === 1)).toBe(true);
  expect(new Set(result.bands.map((band) => band.texture)).size).toBe(10);
  expect(new Set(result.legend.map((entry) => entry.texture)).size).toBe(10);
  expect(result.counts).toEqual(
    Object.fromEntries(stateOrder.map((state) => [state, 1])),
  );
  for (const state of stateOrder) {
    expect(result.benchColors[state]).toBe(
      result.expectedBenchColors[state].toLowerCase(),
    );
  }
  expect(result.pavedRadius).toBeGreaterThan(
    result.bands[result.bands.length - 1].outer,
  );
});

test("every directory member gets a place in the rings", async ({ page }) => {
  await standAtTheFire(page);
  const planted = await plantRoster(page, 118);
  // No ring-load cap: the whole roster stands in the circle.
  expect(planted.figures).toBe(planted.fed);
  expect(planted.unique).toBe(planted.fed);
  expect(planted.signedCount).toBe(planted.fed);
  // Clear of the fire, and inside the pavement the clearing was built with.
  expect(planted.innermost).toBeGreaterThanOrEqual(4.3);
  expect(planted.outermost).toBeLessThanOrEqual(12);
  expect(planted.pavedRadius).toBeCloseTo(12.8, 1);
  // Everyone stands at a person's full height, not shrunk to make the roster
  // fit the clearing.
  expect(planted.smallestFigure).toBe(1);
  // Shoulder room: a full-size avatar is 1.75 across the arms, so neighbours
  // in a ring clear each other, and the rings themselves stand a figure's
  // depth apart rather than stacked into one another.
  expect(planted.closestInRing).toBeGreaterThan(1.75);
  expect(planted.closestPair).toBeGreaterThan(1.49);
  // A crowd is read from across the clearing, so its plates are the light ones.
  expect(planted.plateScale).toBe(0.5);
  expect(planted.chestTexture).toEqual({
    anisotropy: 8,
    generateMipmaps: true,
    // THREE.LinearMipmapLinearFilter
    minFilter: 1008,
  });
});

test("directory avatar photos are on faces before member selection", async ({
  page,
}) => {
  await standAtTheFire(page);
  const avatarPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL/2QAAAABJRU5ErkJggg==";
  const result = await page.evaluate(async (png) => {
    const shell = document.querySelector("forkmesh-world");
    const { normalizeMemberDirectory } = await import("/world/world.js");
    const members = normalizeMemberDirectory({
      users: [{ name: "photo-first", avatarPng: png }],
    });
    shell.world.updateMemberLounge(members, 1, members, 0);
    const figure = shell.world.scene.getObjectByName(
      "avatar:member:photo-first",
    );
    return {
      source: members[0].avatar,
      worn: figure.userData.faceImageUrl,
      profileState: figure.userData.fediverseProfile?.state,
    };
  }, avatarPng);
  expect(result).toEqual({
    source: `data:image/png;base64,${avatarPng}`,
    worn: `data:image/png;base64,${avatarPng}`,
    // The richer profile stays click-lazy; loading the face does not.
    profileState: "idle",
  });
});

test("photo members sit in the innermost ring and every ring is labelled", async ({
  page,
}) => {
  await standAtTheFire(page);
  const avatarPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL/2QAAAABJRU5ErkJggg==";
  const result = await page.evaluate(
    async ({ members, png }) => {
      const shell = document.querySelector("forkmesh-world");
      const { world } = shell;
      world.setPaused(true);
      const { normalizeMemberDirectory } = await import("/world/world.js");
      const directory = normalizeMemberDirectory({
        users: [
          ...members,
          {
            ...members[0],
            name: "cold-photo",
            activityBucket: "120d",
            avatarPng: png,
          },
        ],
      });
      world.updateMemberLounge(directory, directory.length, directory, 0);
      const campfire = world.scene.getObjectByName(
        "campfire-member-circle-dirt",
      ).parent;
      const radius = (name) => {
        const seat =
          campfire.userData.seatOffsets[campfire.userData.seatByName.get(name)];
        return Math.hypot(seat.x, seat.z);
      };
      const labels = campfire.userData.engagementBandLayer.children
        .filter((child) =>
          child.name.startsWith("campfire-member-engagement-label-"),
        )
        .map((child) => ({
          id: child.userData.engagement,
          count: child.userData.memberCount,
          textured: Boolean(child.material.map),
        }));
      return {
        photo: radius("cold-photo"),
        others: members.map((member) => radius(member.name)),
        firstBand: campfire.userData.engagementBands[0]?.id,
        labels,
        dormantCount: campfire.userData.engagementCounts.dormant,
      };
    },
    { members: roster(3), png: avatarPng },
  );
  expect(result.firstBand).toBe("custom-avatar");
  for (const other of result.others) {
    expect(result.photo).toBeLessThan(other);
  }
  expect(result.labels[0]).toEqual({
    id: "custom-avatar",
    count: 1,
    textured: true,
  });
  expect(
    result.labels.slice(1).reduce((sum, label) => sum + label.count, 0),
  ).toBe(3);
  expect(result.dormantCount).toBe(1);
});

test("a live peer wears the directory photo without being clicked", async ({
  page,
}) => {
  await standAtTheFire(page);
  const avatarPng =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQIHWP4z8DwHwAFgAI/ScL/2QAAAABJRU5ErkJggg==";
  const result = await page.evaluate(
    async ({ png, campfireZ }) => {
      const shell = document.querySelector("forkmesh-world");
      const { normalizeMemberDirectory } = await import("/world/world.js");
      const members = normalizeMemberDirectory({
        users: [
          { name: "photo-walker", avatarPng: png },
          { name: "photo-lurker", avatarPng: png },
          { name: "photo-hider", avatarPng: png },
        ],
      });
      shell.world.setPaused(true);
      shell.world.updateMemberLounge(members, 3, members, 0);
      shell.world.setRemotePlayers([
        // A live peer whose presence frame carries the opt-in.
        {
          id: "peer-walker",
          name: "photo-walker",
          accountStatus: "Registered",
          faceImage: true,
          activity: "exploring",
          x: 0,
          z: campfireZ + 16,
        },
        // A persisted contributor row: no presence frame, so no flag at all.
        {
          id: "inactive:peer-lurker",
          name: "photo-lurker",
          accountStatus: "Registered",
          activity: "idle",
          x: 1,
          z: campfireZ + 16,
        },
        // An explicit opt-out keeps the generated face.
        {
          id: "peer-hider",
          name: "photo-hider",
          accountStatus: "Registered",
          faceImage: false,
          activity: "exploring",
          x: 2,
          z: campfireZ + 16,
        },
      ]);
      const worn = (id) =>
        shell.world.scene.getObjectByName(`avatar:${id}`)?.userData
          ?.faceImageUrl ?? null;
      return {
        source: members[0].avatar,
        walker: worn("peer-walker"),
        lurker: worn("inactive:peer-lurker"),
        hider: worn("peer-hider"),
      };
    },
    { png: avatarPng, campfireZ: CAMPFIRE_Z },
  );
  const photo = `data:image/png;base64,${avatarPng}`;
  expect(result).toEqual({
    source: photo,
    // Both faces are dressed the moment their figure is built; neither needed
    // a visitor to open its chest card first.
    walker: photo,
    lurker: photo,
    // Never dressed at all, so the generated portrait it was born with stays.
    hider: null,
  });
});

test("a roster larger than the pavement grows the rings and the dirt", async ({
  page,
}) => {
  await standAtTheFire(page);
  const planted = await plantRoster(page, 190);
  expect(planted.figures).toBe(planted.fed);
  expect(planted.unique).toBe(planted.fed);
  // The rings keep going past the clearing's own six...
  expect(planted.outermost).toBeGreaterThan(12);
  // ...and the dirt widens to meet the outermost of them, so the overflow
  // stands on the plaza instead of out in the grass.
  expect(planted.pavedRadius).toBeGreaterThan(planted.outermost);
  expect(planted.smallestFigure).toBe(1);
  expect(planted.closestInRing).toBeGreaterThan(1.75);
  expect(planted.closestPair).toBeGreaterThan(1.49);
});
