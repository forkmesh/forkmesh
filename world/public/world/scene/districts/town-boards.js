import {
  worldBulletinTexture,
  worldDiscordBoardTexture,
  worldGeneralChatTexture,
} from "../boards.js";
import { canvasTexture } from "../canvas.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import * as resources from "../resources.js";

export async function build({
  THREE,
  interactive,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  const worldBulletin = new THREE.Group();
  worldBulletin.name = "forkmesh-world-bulletin";
  // Banner-sized boards have to be walked up to, so it now stands just past
  // the leaderboards instead of stranded near the rim of the terrain — still
  // clear of the arrival / join grid visitors spawn onto.
  worldBulletin.position.set(-24, 0, 33);
  // Plane textures face local +Z. Rotate the board so its readable face looks
  // back into the World from the outer edge of the circular terrain.
  worldBulletin.rotation.y = Math.atan2(
    -worldBulletin.position.x,
    -worldBulletin.position.z,
  );
  // Sized like the other standing banners in the square rather than the tower
  // it used to be: the page holds five entries, and the ▲ / ▼ controls page
  // through the rest.
  const BULLETIN_FACE_CENTER_Y = 3.9;
  const bulletinBase = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.9, 0.26, 1.5),
    makeSharedMaterial(THREE, "#123241", { roughness: 0.8 }),
  );
  bulletinBase.position.y = 0.13;
  worldBulletin.add(bulletinBase);
  for (const x of [-3.35, 3.35]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.18, 6.5, 0.18),
      makeSharedMaterial(THREE, "#1d5570", { metalness: 0.26, roughness: 0.5 }),
    );
    post.position.set(x, 3.25, 0);
    worldBulletin.add(post);
  }
  const bulletinFrame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.55, 5.1, 0.24),
    makeMaterial(THREE, "#163849", { metalness: 0.35, roughness: 0.44 }),
  );
  bulletinFrame.position.y = BULLETIN_FACE_CENTER_Y;
  bulletinFrame.userData.interactive = "world-bulletin";
  const bulletinFace = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 7.2, 4.8),
    resources.uniqueMaterial(THREE, {
      map: worldBulletinTexture(THREE),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  bulletinFace.name = "forkmesh-world-bulletin-face";
  bulletinFace.position.set(0, BULLETIN_FACE_CENTER_Y, 0.14);
  bulletinFace.userData.interactive = "world-bulletin";
  const makeBulletinControl = (label, direction, y) => {
    const control = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.52, 0.52),
      resources.uniqueMaterial(THREE, {
        map: canvasTexture(THREE, 256, 256, (context) => {
          context.fillStyle = "#12384b";
          context.fillRect(0, 0, 256, 256);
          context.strokeStyle = "#7ed9ff";
          context.lineWidth = 14;
          context.strokeRect(8, 8, 240, 240);
          context.fillStyle = "#e5f8ff";
          context.font = '800 160px "ForkMesh Mono", ui-monospace, monospace';
          context.textAlign = "center";
          context.textBaseline = "middle";
          context.fillText(label, 128, 136);
        }),
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    control.position.set(2.99, y, 0.2);
    control.userData.interactive = `world-bulletin-scroll-${direction}`;
    return control;
  };
  // Parked in the clear right-hand margin, level with the header and the
  // footer so neither control covers an entry.
  const bulletinScrollUp = makeBulletinControl("▲", "up", BULLETIN_FACE_CENTER_Y + 2.05);
  const bulletinScrollDown = makeBulletinControl("▼", "down", BULLETIN_FACE_CENTER_Y - 2.05);
  worldBulletin.add(bulletinFrame, bulletinFace, bulletinScrollUp, bulletinScrollDown);
  interactive.push(bulletinFrame, bulletinFace, bulletinScrollUp, bulletinScrollDown);
  world.add(worldBulletin);
  registerWorldElement(
    "world-bulletin", "Events bulletin board", "Boards & kiosks", worldBulletin,
  );
  await sceneSlice();

  const worldGeneralChatBoard = new THREE.Group();
  worldGeneralChatBoard.name = "forkmesh-world-general-chat-board";
  worldGeneralChatBoard.position.set(-15.5, 0, 39);
  worldGeneralChatBoard.rotation.y = Math.atan2(
    -worldGeneralChatBoard.position.x,
    -worldGeneralChatBoard.position.z,
  );
  const chatBoardBase = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.9, 0.26, 1.5),
    makeSharedMaterial(THREE, "#102b22", { roughness: 0.8 }),
  );
  chatBoardBase.position.y = 0.13;
  worldGeneralChatBoard.add(chatBoardBase);
  for (const x of [-3.35, 3.35]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.18, 6.5, 0.18),
      makeSharedMaterial(THREE, "#285d49", { metalness: 0.24, roughness: 0.52 }),
    );
    post.position.set(x, 3.25, 0);
    worldGeneralChatBoard.add(post);
  }
  const worldGeneralChatFrame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.55, 5.1, 0.24),
    makeSharedMaterial(THREE, "#123126", { metalness: 0.3, roughness: 0.48 }),
  );
  worldGeneralChatFrame.position.y = BULLETIN_FACE_CENTER_Y;
  worldGeneralChatFrame.userData.interactive = "world-general-chat-board";
  const worldGeneralChatFace = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 7.2, 4.8),
    resources.uniqueMaterial(THREE, {
      map: worldGeneralChatTexture(THREE),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  worldGeneralChatFace.name = "forkmesh-world-general-chat-board-face";
  worldGeneralChatFace.position.set(0, BULLETIN_FACE_CENTER_Y, 0.14);
  worldGeneralChatFace.userData.interactive = "world-general-chat-board";
  worldGeneralChatBoard.add(worldGeneralChatFrame, worldGeneralChatFace);
  interactive.push(worldGeneralChatFrame, worldGeneralChatFace);
  world.add(worldGeneralChatBoard);
  registerWorldElement(
    "world-chat-board", "World chat board", "Boards & kiosks",
    worldGeneralChatBoard,
  );
  await sceneSlice();

  const worldDiscordBoard = new THREE.Group();
  worldDiscordBoard.name = "forkmesh-world-discord-board";
  const discordBoardBase = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.9, 0.26, 1.5),
    makeSharedMaterial(THREE, "#161b22", { roughness: 0.8 }),
  );
  discordBoardBase.position.y = 0.13;
  worldDiscordBoard.add(discordBoardBase);
  for (const x of [-3.35, 3.35]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.18, 6.5, 0.18),
      makeSharedMaterial(THREE, "#5865f2", {
        metalness: 0.28,
        roughness: 0.48,
      }),
    );
    post.position.set(x, 3.25, 0);
    worldDiscordBoard.add(post);
  }
  const worldDiscordFrame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.55, 5.1, 0.24),
    makeSharedMaterial(THREE, "#202252", {
      metalness: 0.32,
      roughness: 0.46,
    }),
  );
  worldDiscordFrame.position.y = BULLETIN_FACE_CENTER_Y;
  worldDiscordFrame.userData.interactive = "world-discord-board";
  const worldDiscordFace = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 7.2, 4.8),
    resources.uniqueMaterial(THREE, {
      map: worldDiscordBoardTexture(THREE),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  worldDiscordFace.name = "forkmesh-world-discord-board-face";
  worldDiscordFace.position.set(0, BULLETIN_FACE_CENTER_Y, 0.14);
  worldDiscordFace.userData.interactive = "world-discord-board";
  worldDiscordBoard.add(worldDiscordFrame, worldDiscordFace);
  interactive.push(worldDiscordFrame, worldDiscordFace);
  world.add(worldDiscordBoard);
  registerWorldElement(
    "discord-board", "Discord board", "Boards & kiosks", worldDiscordBoard,
  );
  return {
    worldBulletin,
    bulletinFrame,
    bulletinFace,
    bulletinScrollUp,
    bulletinScrollDown,
    worldGeneralChatBoard,
    worldGeneralChatFace,
    worldDiscordBoard,
  };
}
