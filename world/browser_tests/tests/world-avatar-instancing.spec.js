// Avatar legs, shoes and shoulder antennas are the same untextured shapes in
// the same materials for everybody, so the whole crowd draws them as four
// instanced batches instead of eight meshes per person. These contracts pin
// both halves of that: the batches really carry the crowd and shrink back when
// people leave, and the lower body — which no longer has a mesh of its own to
// be hit — is still selectable through its invisible proxy.
const { test, expect } = require("@playwright/test");

const BATCH_NAMES = {
  leg: "forkmesh-avatar-leg-instances",
  shoe: "forkmesh-avatar-shoe-instances",
  antennaStalk: "forkmesh-avatar-antennaStalk-instances",
  antennaBulb: "forkmesh-avatar-antennaBulb-instances",
};

const CROWD = Array.from({ length: 12 }, (_, index) => ({
  id: `crowd-${index}`,
  name: `Crowd${index}`,
  x: -58 + (index % 4) * 3,
  z: -58 + Math.floor(index / 4) * 3,
  accountStatus: "Registered",
  activity: "exploring",
  countryCode: "US",
  flag: "US",
}));

async function bootWorld(page) {
  await page.setViewportSize({ width: 1200, height: 800 });
  await page.goto("/world/?nointro=1");
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      Boolean(shell?.world?.renderer?.domElement) &&
      Number(shell.world.renderer.info?.render?.frame || 0) > 0
    );
  });
}

test("a crowd draws its legs, shoes and antennas in four shared batches", async ({
  page,
}) => {
  await bootWorld(page);
  // The visitor roster keeps arriving from the World socket, so the crowd is
  // planted, measured and cleared inside one evaluation instead of across waits
  // a later roster could prune.
  const measured = await page.evaluate(
    ({ crowd, batchNames }) => {
      const shell = document.querySelector("forkmesh-world");
      const { world } = shell;
      const camera = world.camera;
      const draw = () => {
        camera.position.set(-44, 26, -18);
        camera.lookAt(-49, 2, -55);
        camera.updateMatrixWorld(true);
        world.renderer.info.reset();
        world.renderer.render(world.scene, camera);
        return world.renderer.info.render.calls;
      };
      world.setPaused(true);
      const alone = draw();
      world.setRemotePlayers(crowd);
      const crowded = draw();
      const peer = world.scene.getObjectByName(`avatar:${crowd[0].id}`);
      let peerDraws = 0;
      peer.traverse((child) => {
        if (child.isMesh && child.material?.visible !== false) peerDraws += 1;
      });
      const withCrowd = world.getDiagnostics().avatarPartInstances;
      world.setRemotePlayers([]);
      const departed = draw();
      return {
        crowd: crowd.length,
        callsPerPeer: (crowded - alone) / crowd.length,
        peerDraws,
        peerParts: {
          leg: peer.userData.instancedParts.leg.length,
          shoe: peer.userData.instancedParts.shoe.length,
          antennaStalk: peer.userData.instancedParts.antennaStalk.length,
          antennaBulb: peer.userData.instancedParts.antennaBulb.length,
        },
        instanced: Object.fromEntries(
          Object.keys(batchNames).map((kind) => [
            kind,
            world.scene.getObjectByName(batchNames[kind])?.isInstancedMesh ===
              true,
          ]),
        ),
        withCrowd,
        afterCrowd: world.getDiagnostics().avatarPartInstances,
        aloneCalls: alone,
        departedCalls: departed,
      };
    },
    { crowd: CROWD, batchNames: BATCH_NAMES },
  );

  // One batch per repeated shape for the whole World, and rigs — not meshes —
  // inside each person.
  expect(measured.instanced).toEqual({
    leg: true,
    shoe: true,
    antennaStalk: true,
    antennaBulb: true,
  });
  expect(measured.peerParts).toEqual({
    leg: 4,
    shoe: 2,
    antennaStalk: 1,
    antennaBulb: 1,
  });
  // Two thighs, two shins and two shoes for each of the crowd, on top of
  // whoever was already standing in the World.
  expect(measured.withCrowd.drawn.leg).toBeGreaterThanOrEqual(
    measured.crowd * 4,
  );
  expect(measured.withCrowd.drawn.leg).toBe(measured.withCrowd.drawn.shoe * 2);
  // Peers who left release their copies instead of leaving them drawn at a
  // stale transform.
  expect(measured.afterCrowd.drawn.leg).toBeLessThan(
    measured.withCrowd.drawn.leg,
  );
  expect(measured.departedCalls).toBeLessThanOrEqual(measured.aloneCalls);
  // Fifteen submissions a person before instancing. The nine that remain are
  // the pieces carrying that one member's own canvas — torso, head, arms, face,
  // chest card, name board, verification pin — which cannot share a batch.
  expect(measured.peerDraws).toBeLessThanOrEqual(10);
  expect(measured.callsPerPeer).toBeLessThanOrEqual(10);
});

test("an avatar pulled out of the game and put back keeps its instanced parts", async ({
  page,
}) => {
  await bootWorld(page);
  // The Elements panel detaches a root from the Scene instead of hiding it, so
  // the sweep drops that avatar from the crowd batch. Switching the element
  // back on has to hand its legs, shoes and antenna back.
  const toggled = await page.evaluate(() => {
    const shell = document.querySelector("forkmesh-world");
    const { world } = shell;
    const legs = () => world.getDiagnostics().avatarPartInstances.drawn.leg;
    const draw = () => world.renderer.render(world.scene, world.camera);
    world.setPaused(true);
    draw();
    const before = legs();
    world.setWorldElementEnabled("player-avatar", false);
    draw();
    const detached = legs();
    world.setWorldElementEnabled("player-avatar", true);
    draw();
    return { before, detached, restored: legs() };
  });
  expect(toggled.before).toBeGreaterThanOrEqual(4);
  expect(toggled.detached).toBe(toggled.before - 4);
  expect(toggled.restored).toBe(toggled.before);
});

test("instanced legs stay selectable through their invisible proxy", async ({
  page,
}) => {
  await bootWorld(page);
  const proxies = await page.evaluate(() => {
    const shell = document.querySelector("forkmesh-world");
    const found = [];
    shell.world.scene.traverse((child) => {
      if (child.name !== "avatar-leg-hit-proxy") return;
      found.push({
        isMesh: child.isMesh === true,
        visible: child.visible,
        raycastProxy: child.userData.raycastProxy === true,
      });
    });
    return found;
  });
  expect(proxies.length).toBeGreaterThan(0);
  // Never drawn, always hit-tested: the flag is what tells the World's pointer
  // filters that this one is hidden on purpose.
  for (const proxy of proxies) {
    expect(proxy).toEqual({ isMesh: true, visible: false, raycastProxy: true });
  }

  await page.evaluate(() => {
    document
      .querySelector("forkmesh-world")
      .world.setSpawn({ x: 0, z: 30, space: "town-square" });
  });
  // Wait for the follow camera to settle on the player before projecting: a
  // camera still travelling would aim the click at the ground behind them.
  await page.waitForFunction(() => {
    const { world } = document.querySelector("forkmesh-world");
    const spot = world.player?.position?.clone?.();
    if (!spot) return false;
    return world.camera.position.distanceTo(spot) < 40;
  });
  const aim = await page.evaluate(() => {
    const shell = document.querySelector("forkmesh-world");
    const { world } = shell;
    const spot = world.player.position.clone();
    // Mid-shin: well below the torso, the chest card and the name board, so
    // only the leg proxy covers this pixel.
    spot.y += 0.7;
    spot.project(world.camera);
    const rect = world.renderer.domElement.getBoundingClientRect();
    const point = {
      x: rect.left + ((spot.x + 1) / 2) * rect.width,
      y: rect.top + ((1 - spot.y) / 2) * rect.height,
    };
    // The Elements picker deliberately skips proxies, so what it reports under
    // this pixel is whatever is drawn there — the ground past the avatar.
    const drawn = world.pickWorldObject({
      clientX: point.x,
      clientY: point.y,
    });
    return { point, drawnElement: String(drawn?.elementId || "") };
  });
  expect(aim.drawnElement).not.toBe("player-avatar");
  expect(await page.locator("[data-world-detail]").textContent()).toBe("");

  // A press and release, without the trailing click event that the World's
  // outside-click dismissal also listens for.
  await page.mouse.move(aim.point.x, aim.point.y);
  await page.mouse.down();
  await page.mouse.up();
  await expect(page.locator("[data-world-detail]")).toContainText(
    "WORLD MEMBER",
  );
  await expect(page.locator("[data-world-detail]")).toContainText(
    "THIS IS YOU",
  );
});
