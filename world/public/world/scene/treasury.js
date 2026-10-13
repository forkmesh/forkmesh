import {
  ACTIVITY_LIGHT_COLORS,
  AVATAR_SOLANA_ADDRESS_RE,
} from "./avatar-shared.js";
import {
  canvasTexture,
  roundedRect,
  wordTexture,
} from "./canvas.js";
import { makeSharedMaterial } from "./materials.js";
import * as resources from "./resources.js";

// The reward pool names itself: instead of a pale stone foundation plus a
// ground plaque, the title wraps around the green rim of the basin.
export function rewardPoolRimTexture(THREE) {
  const width = 4096;
  const height = 256;
  // Three repeats around the rim: five crowded the 18-character title into its
  // own neighbours, so the wrap read as one smeared word from every angle.
  const repeats = 3;
  const title = "GLOBAL REWARD POOL";
  return canvasTexture(THREE, width, height, (context) => {
    context.fillStyle = "#1d5240";
    context.fillRect(0, 0, width, height);
    context.fillStyle = "#164236";
    context.fillRect(0, 0, width, 18);
    context.fillRect(0, height - 18, width, 18);
    context.textAlign = "center";
    context.textBaseline = "middle";
    const slot = width / repeats;
    // Measured, not assumed: shrink the title until a repeat plus its ◎
    // separator fits inside one slot, whatever font the browser resolves.
    const available = slot * 0.72;
    let titleSize = 108;
    const titleFont = (size) =>
      `800 ${size}px "ForkMesh Mono", ui-monospace, monospace`;
    context.font = titleFont(titleSize);
    const measured = context.measureText(title).width;
    if (measured > available) {
      titleSize = Math.max(
        48,
        titleSize - 2 * Math.ceil((titleSize - (titleSize * available) / measured) / 2),
      );
      context.font = titleFont(titleSize);
      while (titleSize > 48 && context.measureText(title).width > available) {
        titleSize -= 2;
        context.font = titleFont(titleSize);
      }
    }
    for (let index = 0; index < repeats; index += 1) {
      const centre = slot * (index + 0.5);
      context.fillStyle = "#f7c96b";
      context.font = titleFont(titleSize);
      context.fillText(title, centre, height / 2 + 4);
      context.fillStyle = "#9ef7c6";
      context.font = `700 ${Math.round(
        titleSize * 0.88,
      )}px "ForkMesh Mono", ui-monospace, monospace`;
      context.fillText("◎", centre + slot / 2, height / 2 + 4);
    }
  });
}

// The mirror tally that hangs over the reward pool's orb. The cabinets are
// already arranged in rings around this basin, so the headline number belongs
// above them rather than on yet another sign: how many mirror nodes the signed
// catalog currently lists, and how many of those are answering right now.
export function rewardPoolMirrorCountTexture(THREE, total, online) {
  const count = Math.max(0, Math.min(9999, Math.round(Number(total) || 0)));
  const live = Math.max(0, Math.min(count, Math.round(Number(online) || 0)));
  return canvasTexture(THREE, 512, 256, (context) => {
    context.clearRect(0, 0, 512, 256);
    context.textAlign = "center";
    context.textBaseline = "middle";
    const glow = context.createLinearGradient(0, 26, 0, 150);
    glow.addColorStop(0, "#fff4d2");
    glow.addColorStop(0.55, "#f7c96b");
    glow.addColorStop(1, "#ffae3f");
    context.shadowColor = "rgba(255, 174, 63, 0.9)";
    context.shadowBlur = 34;
    context.fillStyle = glow;
    context.font = '700 142px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText(live.toLocaleString("en-US"), 256, 92);
    context.shadowBlur = 18;
    context.fillStyle = "#9ef7c6";
    context.font = '700 36px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("ONLINE", 256, 168);
    context.shadowBlur = 12;
    context.fillStyle = "#ffd479";
    context.font = '500 30px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      `${count.toLocaleString("en-US")} TOTAL`,
      256,
      216,
    );
  });
}

// Paints a QR matrix (from /qr.js) into `size` square pixels at (x, y).
export function drawQrModules(context, text, x, y, size) {
  const encoder = globalThis.ForkMeshQR;
  if (!encoder?.generate || !text) return false;
  let matrix = null;
  try {
    matrix = encoder.generate(text);
  } catch {
    return false;
  }
  if (!matrix?.size) return false;
  const quiet = 4;
  const total = matrix.size + quiet * 2;
  const scale = size / total;
  context.fillStyle = "#ffffff";
  context.fillRect(x, y, size, size);
  context.fillStyle = "#07120e";
  for (let row = 0; row < matrix.size; row += 1) {
    for (let column = 0; column < matrix.size; column += 1) {
      if (!matrix.modules[row][column]) continue;
      context.fillRect(
        x + (column + quiet) * scale,
        y + (row + quiet) * scale,
        Math.ceil(scale),
        Math.ceil(scale),
      );
    }
  }
  return true;
}

// The chest wallet chip: the account's published Solana address as a QR,
// ringed with the transaction-recency colour and captioned with the public
// balance the app layer fetched.
function walletChipTexture(THREE, wallet) {
  const ring =
    ACTIVITY_LIGHT_COLORS[wallet.txBucket] || ACTIVITY_LIGHT_COLORS.stale;
  return canvasTexture(THREE, 256, 256, (context) => {
    context.clearRect(0, 0, 256, 256);
    roundedRect(context, 6, 6, 244, 244, 24);
    context.fillStyle = "rgba(7, 18, 14, 0.94)";
    context.fill();
    // The ring wears the same colour ladder as the chest activity light,
    // keyed to how recently the wallet last saw a transaction.
    roundedRect(context, 40, 12, 176, 176, 14);
    context.strokeStyle = ring;
    context.lineWidth = 10;
    context.stroke();
    const drawn = drawQrModules(context, wallet.address, 53, 25, 150);
    if (!drawn) {
      context.fillStyle = "rgba(158, 247, 198, 0.14)";
      context.fillRect(53, 25, 150, 150);
    }
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '700 30px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#f7c96b";
    const sol = Number(wallet.sol);
    context.fillText(
      Number.isFinite(sol)
        ? `◎ ${sol.toLocaleString("en-US", { maximumFractionDigits: 4 })}`
        : "◎ …",
      128,
      222,
    );
  });
}

export function syncAvatarWallet(THREE, avatar, identity) {
  const chip = avatar?.userData?.walletChip;
  if (!chip?.material) return;
  const address = AVATAR_SOLANA_ADDRESS_RE.test(String(identity?.solana || ""))
    ? String(identity.solana)
    : "";
  const wallet = {
    address,
    sol: Number.isFinite(Number(identity?.walletSol))
      ? Number(identity.walletSol)
      : null,
    txBucket: ACTIVITY_LIGHT_COLORS[identity?.walletTxBucket]
      ? String(identity.walletTxBucket)
      : "",
  };
  const key = `${wallet.address} ${wallet.sol} ${wallet.txBucket}`;
  if (avatar.userData.walletKey === key) return;
  avatar.userData.walletKey = key;
  chip.visible = Boolean(wallet.address);
  if (!wallet.address) return;
  const old = chip.material.map;
  chip.material.map = walletChipTexture(THREE, wallet);
  chip.material.needsUpdate = true;
  old?.dispose?.();
}

function rewardTreasuryState(state = {}) {
  const address = String(state?.address || "").trim();
  const valid = /^[1-9A-HJ-NP-Za-km-z]{32,44}$/.test(address);
  const rawBalance = Number(
    state?.balanceSol ?? state?.balance ?? state?.sol ?? Number.NaN,
  );
  return {
    address: valid ? address : "",
    network: String(state?.network || "").slice(0, 24),
    balance:
      valid && Number.isFinite(rawBalance)
        ? `${rawBalance.toLocaleString("en-US", {
            minimumFractionDigits: 0,
            maximumFractionDigits: 6,
          })} SOL`
        : "",
  };
}

function rewardTreasuryTexture(THREE, treasury) {
  return canvasTexture(THREE, 512, 640, (context) => {
    context.clearRect(0, 0, 512, 640);
    roundedRect(context, 8, 8, 496, 624, 26);
    context.fillStyle = "rgba(7, 20, 16, 0.95)";
    context.fill();
    context.strokeStyle = "#f7c96b";
    context.lineWidth = 6;
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = "#9ef7c6";
    context.font = '700 30px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      `TREASURY · ${(treasury.network || "mainnet-beta").toUpperCase()}`,
      256,
      56,
    );
    const drawn = drawQrModules(context, treasury.address, 106, 92, 300);
    if (!drawn) {
      context.fillStyle = "rgba(158, 247, 198, 0.14)";
      context.fillRect(106, 92, 300, 300);
      context.fillStyle = "#d9ffea";
      context.font = '700 26px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO VERIFIED", 256, 226);
      context.fillText("POOL ADDRESS", 256, 262);
    }
    context.fillStyle = "#f7c96b";
    context.font = '800 54px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText(treasury.balance || "BALANCE UNAVAILABLE", 256, 448);
    context.fillStyle = "#d9ffea";
    context.font = '400 22px "ForkMesh Mono", ui-monospace, monospace';
    const address = treasury.address || "—";
    context.fillText(address.slice(0, 22), 256, 512);
    context.fillText(address.slice(22) || " ", 256, 542);
    context.fillStyle = "#9ef7c6";
    context.font = '400 18px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("PUBLIC ON-CHAIN BALANCE · EXTERNAL SIGNER", 256, 592);
  });
}

// A double-sided board at the basin edge showing the treasury address as a
// scannable QR code with the live public balance beneath it.
export function createRewardTreasurySign(THREE) {
  const sign = new THREE.Group();
  sign.name = "reward-treasury-sign";
  const post = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.28, 1.5, 0.28),
    makeSharedMaterial(THREE, "#123a2c", { roughness: 0.82 }),
  );
  post.position.y = 0.75;
  sign.add(post);
  const board = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 2.1, 2.6, 0.16),
    makeSharedMaterial(THREE, "#0c1f19", { roughness: 0.56, metalness: 0.12 }),
  );
  board.position.y = 2.55;
  sign.add(board);
  const faces = [];
  for (const facing of [1, -1]) {
    const face = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 1.94, 2.42),
      resources.uniqueMaterial(THREE, {
        map: rewardTreasuryTexture(THREE, rewardTreasuryState()),
        transparent: true,
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    face.position.z = facing * 0.085;
    face.rotation.y = facing > 0 ? 0 : Math.PI;
    board.add(face);
    faces.push(face);
  }
  sign.userData.treasuryFaces = faces;
  const nodeButton = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 1.5, 0.92, 0.18),
    makeSharedMaterial(THREE, "#1f6f46", {
      metalness: 0.16,
      roughness: 0.46,
    }),
  );
  nodeButton.name = "reward-treasury-start-node-button";
  nodeButton.position.set(1.68, 1.66, 0);
  nodeButton.userData.interactive = "start-node-download";
  sign.add(nodeButton);
  for (const facing of [1, -1]) {
    const face = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 1.36, 0.78),
      resources.uniqueMaterial(THREE, {
        map: wordTexture(
          THREE,
          "START A NODE",
          "DOWNLOAD APP  ↗",
          "#9ef7c6",
        ),
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    face.position.set(0, 0, facing * 0.095);
    face.rotation.y = facing > 0 ? 0 : Math.PI;
    face.userData.interactive = "start-node-download";
    nodeButton.add(face);
  }
  const mirrorButton = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 1.82, 0.92, 0.2),
    makeSharedMaterial(THREE, "#7849d6", {
      emissive: "#4b22a3",
      emissiveIntensity: 0.62,
      metalness: 0.18,
      roughness: 0.36,
    }),
  );
  mirrorButton.name = "reward-treasury-launch-mirror-button";
  mirrorButton.position.set(1.86, 2.82, 0);
  mirrorButton.userData.interactive = "launch-vultr-mirror";
  sign.add(mirrorButton);
  for (const facing of [1, -1]) {
    const face = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 1.68, 0.78),
      resources.uniqueMaterial(THREE, {
        map: wordTexture(
          THREE,
          "LAUNCH MIRROR",
          "VULTR  ↗",
          "#d8c7ff",
        ),
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    face.position.set(0, 0, facing * 0.105);
    face.rotation.y = facing > 0 ? 0 : Math.PI;
    face.userData.interactive = "launch-vultr-mirror";
    mirrorButton.add(face);
  }
  sign.userData.treasurySignature = "";
  return sign;
}

export function applyRewardTreasury(THREE, sign, state) {
  const faces = sign?.userData?.treasuryFaces;
  if (!Array.isArray(faces) || !faces.length) return;
  const treasury = rewardTreasuryState(state);
  const signature = `${treasury.address}|${treasury.balance}|${treasury.network}`;
  if (sign.userData.treasurySignature === signature) return;
  sign.userData.treasurySignature = signature;
  faces.forEach((face) => {
    face.material.map?.dispose?.();
    face.material.map = rewardTreasuryTexture(THREE, treasury);
    face.material.needsUpdate = true;
  });
}
