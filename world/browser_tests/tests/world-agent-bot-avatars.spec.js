// The per-session agent droids wear their model's portrait as a face plate.
// The roster below exercises every resolution path in agentBotAvatarLook —
// exact model names, versioned model ids, and the codex provider fallback —
// then asserts which portrait texture each spawned face actually loaded. The
// posed close-up written to /tmp/forkmesh-bot-avatars-closeup.png stays
// around so scene changes can be eyeballed after a run.
const { test, expect } = require("@playwright/test");

const ROSTER = [
  { id: "shot-opus", provider: "claude-code", model: "opus", status: "running", title: "Opus run", localAgentId: 1 },
  { id: "shot-sonnet", provider: "claude-code", model: "claude-sonnet-5", status: "queued", title: "Sonnet run", localAgentId: 2 },
  { id: "shot-haiku", provider: "claude-code", model: "haiku", status: "stopped", title: "Haiku run", localAgentId: 3 },
  { id: "shot-fable", provider: "claude-code", model: "claude-fable-5", status: "running", title: "Fable run", localAgentId: 4 },
  { id: "shot-sol", provider: "codex", model: "", status: "running", title: "Codex run", localAgentId: 5 },
];

test("session agent droids wear their model's portrait face", async ({ page }) => {
  await page.setViewportSize({ width: 1600, height: 900 });
  await page.goto("/world?nointro=1");
  await page.waitForFunction(() => {
    const shell = document.querySelector("forkmesh-world");
    return (
      Boolean(shell?.world?.renderer?.domElement) &&
      Number(shell.world.renderer.info?.render?.frame || 0) > 0
    );
  });
  await page.locator("forkmesh-world").evaluate((shell, roster) => {
    window.clearInterval(shell.orgAgentTimer);
    shell.orgAgentTimer = 0;
    shell.world.setAgentBotAccess(true);
    shell.world.updateMirrorAgentTasks(roster);
  }, ROSTER);
  // Every droid shares its portrait from the per-path texture cache; wait for
  // decoded pixels so the assertions below read settled materials.
  await page.waitForFunction((roster) => {
    const shell = document.querySelector("forkmesh-world");
    return roster.every((session) => {
      const face = shell.world.scene.getObjectByName(
        `agent-face:${session.id}`,
      );
      return Boolean(face?.material?.map?.image?.naturalWidth);
    });
  }, ROSTER);
  const faces = await page.locator("forkmesh-world").evaluate(
    (shell, roster) =>
      roster.map((session) => {
        const face = shell.world.scene.getObjectByName(
          `agent-face:${session.id}`,
        );
        return {
          id: session.id,
          texture: new URL(face.material.map.image.src).pathname,
        };
      }),
    ROSTER,
  );
  expect(faces).toEqual([
    { id: "shot-opus", texture: "/assets/bot-avatars/opus-face.webp" },
    { id: "shot-sonnet", texture: "/assets/bot-avatars/sonnet-face.webp" },
    { id: "shot-haiku", texture: "/assets/bot-avatars/haiku-face.webp" },
    { id: "shot-fable", texture: "/assets/bot-avatars/fable-face.webp" },
    { id: "shot-sol", texture: "/assets/bot-avatars/sol-face.webp" },
  ]);
  await page.locator("forkmesh-world").evaluate((shell, roster) => {
    const bots = roster.map((session) => {
      const bot =
        shell.world.scene.getObjectByName(
          `claude-agent-droid-${session.id}`,
        ) ||
        shell.world.scene.getObjectByName(`codex-agent-droid-${session.id}`);
      if (!bot) throw new Error(`${session.id} bot missing`);
      return bot;
    });
    shell.world.setPaused(true);
    const anchor = bots[0].position.clone();
    bots.forEach((bot, index) => {
      bot.position.set(anchor.x + (index - 2) * 1.7, anchor.y, anchor.z);
      bot.rotation.y = 0;
    });
    const camera = shell.world.camera;
    camera.fov = 45;
    camera.position.set(anchor.x, anchor.y + 1.6, anchor.z + 7.4);
    camera.lookAt(anchor.x, anchor.y + 1.05, anchor.z);
    camera.updateProjectionMatrix();
    shell.world.renderer.render(shell.world.scene, camera);
  }, ROSTER);
  await page.locator("canvas.world-canvas").screenshot({
    path: "/tmp/forkmesh-bot-avatars-closeup.png",
    animations: "disabled",
  });
});
