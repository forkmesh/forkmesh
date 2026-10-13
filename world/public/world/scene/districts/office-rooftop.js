import {
  OFFICE_ENGINEERING_STATION_HALF_DEPTH,
  OFFICE_ENGINEERING_STATION_HALF_WIDTH,
  OFFICE_ENGINEERING_STATION_XS,
  OFFICE_ENGINEERING_STATION_Z,
  OFFICE_HEX_APOTHEM,
  OFFICE_HEX_RADIUS,
  OFFICE_HEX_SIDES,
  OFFICE_ROOFTOP_TABLES,
  officeFloorY,
} from "../../world-office-tower.js";
import { sharedGeometry } from "../../world-resource-cache.js";
import { canvasTexture } from "../canvas.js";
import {
  ZIP_LINE_BEACH_ANCHOR_HEIGHT,
  ZIP_LINE_BEACH_OFFSET,
  ZIP_LINE_ROOF_ANCHOR_HEIGHT,
  ZIP_LINE_ROOF_LOCAL,
  ZIP_LINE_SAG,
} from "../gameplay-constants.js";
import { makeLabelSprite } from "../labels.js";
import { BEACH_CENTER_X, BEACH_CENTER_Z } from "../layout.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { setShadows } from "../object-utils.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
}) {
  function addOfficeFunFloorProps() {
    const metal = makeMaterial(THREE, "#315e56", {
      metalness: 0.52,
      roughness: 0.35,
    });
    const glow = makeMaterial(THREE, "#73dfff", {
      emissive: "#2d82a4",
      emissiveIntensity: 1.15,
      metalness: 0.22,
      roughness: 0.24,
    });
    const engineering = ctx.officeFloorGroups.get("engineering");
    for (const x of OFFICE_ENGINEERING_STATION_XS) {
      const station = new THREE.Group();
      const desk = new THREE.Mesh(
        sharedGeometry(
          THREE,
          "BoxGeometry",
          OFFICE_ENGINEERING_STATION_HALF_WIDTH * 2,
          0.42,
          OFFICE_ENGINEERING_STATION_HALF_DEPTH * 2,
        ),
        metal,
      );
      desk.position.y = 1.35;
      station.add(desk);
      const screen = new THREE.Mesh(
        sharedGeometry(THREE, "BoxGeometry", 5.8, 2.8, 0.18),
        glow,
      );
      screen.position.set(0, 3, -1.6);
      station.add(screen);
      station.position.set(x, 0, OFFICE_ENGINEERING_STATION_Z);
      engineering.add(station);
    }

    const rooftop = ctx.officeFloorGroups.get("rooftop");
    const roofGlass = makeMaterial(THREE, "#d8ffff", {
      transparent: true,
      opacity: 0.24,
      metalness: 0,
      roughness: 0.54,
      depthWrite: false,
    });
    const barrierHalf = OFFICE_HEX_RADIUS / 2;
    for (const side of OFFICE_HEX_SIDES) {
      const spans =
        side.id === "back"
          ? [
              [-barrierHalf, ctx.elevatorCutMinX],
              [ctx.elevatorCutMaxX, barrierHalf],
            ]
          : [[-barrierHalf, barrierHalf]];
      for (const [minU, maxU] of spans) {
        const barrier = new THREE.Mesh(
          resources.primitive(THREE, "BoxGeometry", maxU - minU, 3.2, 0.22),
          roofGlass,
        );
        const along = (minU + maxU) / 2;
        const reach = OFFICE_HEX_APOTHEM - 0.2;
        barrier.name = `forkmesh-office-rooftop-barrier-${side.id}`;
        barrier.position.set(
          side.normalX * reach + side.tangentX * along,
          1.75,
          side.normalZ * reach + side.tangentZ * along,
        );
        barrier.rotation.y = side.rotationY;
        rooftop.add(barrier);
      }
    }
    const patioWood = makeMaterial(THREE, "#8b6848", {
      roughness: 0.7,
    });
    const patioFrame = makeMaterial(THREE, "#213b36", {
      metalness: 0.45,
      roughness: 0.44,
    });
    const patioSeat = makeMaterial(THREE, "#3f8369", {
      roughness: 0.68,
    });
    for (const [tableIndex, { x, z }] of OFFICE_ROOFTOP_TABLES.entries()) {
      const patioTable = new THREE.Group();
      patioTable.name = `forkmesh-office-rooftop-table-${tableIndex + 1}`;
      patioTable.position.set(x, 0, z);
      const tabletop = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 3.1, 3.1, 0.34, 24),
        patioWood,
      );
      tabletop.name =
        `forkmesh-office-rooftop-tabletop-${tableIndex + 1}`;
      tabletop.position.y = 2.68;
      patioTable.add(tabletop);
      for (const [legIndex, [legX, legZ]] of [
        [-1.8, -1.8],
        [-1.8, 1.8],
        [1.8, -1.8],
        [1.8, 1.8],
      ].entries()) {
        const leg = new THREE.Mesh(
          sharedGeometry(THREE, "BoxGeometry", 0.28, 2.5, 0.28),
          patioFrame,
        );
        leg.name =
          `forkmesh-office-rooftop-table-${tableIndex + 1}-leg-${legIndex + 1}`;
        leg.position.set(legX, 1.25, legZ);
        patioTable.add(leg);
      }
      rooftop.add(patioTable);
      for (let index = 0; index < 4; index += 1) {
        const angle = index * Math.PI / 2;
        const chairId = `rooftop-chair-${tableIndex * 4 + index + 1}`;
        const chair = new THREE.Group();
        chair.name = `forkmesh-office-${chairId}`;
        const seatTopY = 1.42;
        const seat = new THREE.Mesh(
          sharedGeometry(THREE, "BoxGeometry", 2.1, 0.22, 1.8),
          patioSeat,
        );
        seat.name = `forkmesh-office-${chairId}-seat`;
        seat.position.y = seatTopY - 0.11;
        chair.add(seat);
        for (const [legIndex, [legX, legZ]] of [
          [-0.78, -0.62],
          [-0.78, 0.62],
          [0.78, -0.62],
          [0.78, 0.62],
        ].entries()) {
          const leg = new THREE.Mesh(
            sharedGeometry(THREE, "BoxGeometry", 0.2, 1.2, 0.2),
            patioFrame,
          );
          leg.name =
            `forkmesh-office-${chairId}-leg-${legIndex + 1}`;
          leg.position.set(legX, 0.6, legZ);
          chair.add(leg);
        }
        const back = new THREE.Mesh(
          sharedGeometry(THREE, "BoxGeometry", 2.1, 1.65, 0.2),
          patioSeat,
        );
        back.name = `forkmesh-office-${chairId}-back`;
        back.position.set(0, 2.08, -0.8);
        chair.add(back);
        chair.position.set(
          x + Math.cos(angle) * 5,
          0,
          z + Math.sin(angle) * 5,
        );
        chair.rotation.y = Math.atan2(
          -Math.cos(angle) * 5,
          -Math.sin(angle) * 5,
        );
        chair.userData.officeChairId = chairId;
        chair.userData.officeFloorId = "rooftop";
        chair.userData.officeSeatTopY = seatTopY;
        chair.traverse((child) => {
          if (!child.isMesh) return;
          child.userData.officeChairId = chairId;
          child.userData.officeFloorId = "rooftop";
          child.userData.interactive = "office-chair";
          ctx.interactive.push(child);
        });
        ctx.officeChairs.set(chairId, chair);
        rooftop.add(chair);
      }
    }

    // This is a launcher for the real repository source surface, not a mock
    // editor. The shell owns the fixed same-origin action and explains that
    // write-capable source work remains in ForkMesh desktop or an explicitly
    // installed Marketplace IDE plugin.
    const rooftopLaptop = new THREE.Group();
    rooftopLaptop.name = "forkmesh-office-rooftop-laptop";
    rooftopLaptop.position.set(
      OFFICE_ROOFTOP_TABLES[1].x,
      2.9,
      OFFICE_ROOFTOP_TABLES[1].z,
    );
    rooftopLaptop.userData.officeFloorId = "rooftop";
    const laptopBase = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 2.8, 0.12, 1.8),
      makeSharedMaterial(THREE, "#151c24", {
        metalness: 0.72,
        roughness: 0.24,
      }),
    );
    laptopBase.name = "forkmesh-office-rooftop-laptop-base";
    laptopBase.position.z = -0.15;
    rooftopLaptop.add(laptopBase);
    const laptopScreen = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 2.52, 1.48),
      resources.uniqueMaterial(THREE, {
        map: canvasTexture(THREE, 756, 444, (context) => {
          context.fillStyle = "#071714";
          context.fillRect(0, 0, 756, 444);
          context.strokeStyle = "#9ef7c6";
          context.lineWidth = 18;
          context.strokeRect(12, 12, 732, 420);
          context.fillStyle = "#eafff6";
          context.font =
            '800 48px "ForkMesh Mono", ui-monospace, monospace';
          context.textAlign = "center";
          context.fillText("FORKMESH SOURCE", 378, 190);
          context.fillStyle = "#8ecdb6";
          context.font =
            '600 29px "ForkMesh Mono", ui-monospace, monospace';
          context.fillText("CLICK TO OPEN REAL REPOSITORY", 378, 260);
        }),
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    laptopScreen.name = "forkmesh-office-rooftop-laptop-screen";
    laptopScreen.position.set(0, 0.86, 0.72);
    laptopScreen.rotation.x = -0.16;
    rooftopLaptop.add(laptopScreen);
    rooftopLaptop.traverse((child) => {
      if (!child.isMesh) return;
      child.userData.officeFloorId = "rooftop";
      child.userData.interactive = "office-rooftop-laptop";
      ctx.interactive.push(child);
    });
    rooftop.add(rooftopLaptop);
  }
  function zipLineCablePoint(t, target) {
    target.lerpVectors(ctx.zipLineStart, ctx.zipLineEnd, t);
    target.y -= ZIP_LINE_SAG * 4 * t * (1 - t);
    return target;
  }
  function addZipLineMast(name, parent, x, baseY, z, height) {
    const mast = new THREE.Group();
    mast.name = name;
    mast.position.set(x, baseY, z);
    const post = new THREE.Mesh(
      new THREE.BoxGeometry(0.62, height, 0.62),
      ctx.zipLineSteel,
    );
    post.position.y = height / 2;
    const arm = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.4, 0.4, 2.6),
      ctx.zipLineSteel,
    );
    arm.position.y = height - 0.5;
    arm.rotation.y = ctx.zipLineHeading;
    const cap = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.42, 12, 8),
      ctx.zipLineAccent,
    );
    cap.position.y = height + 0.2;
    mast.add(post, arm, cap);
    setShadows(mast);
    parent.add(mast);
    return mast;
  }

  return {
    addOfficeFunFloorProps,
    zipLineCablePoint,
    addZipLineMast,
  };
}

export function build({
  THREE,
  addZipLineMast,
  beachScene,
  ctx,
  interactive,
  officeFloorGroups,
  officeInterior,
  registerWorldElement,
  world,
  zipLine,
  zipLineCablePoint,
}) {
  // One cable from the Office roof to the beach. The masts and cable live in
  // World coordinates so Town sees them; the clickable harness station lives
  // on the rooftop floor because Office clicks only reach the active storey.
  const zipLineRoofFloorY = officeFloorY("rooftop") + 0.38;
  officeInterior.updateWorldMatrix(true, false);
  const zipLineStart = world.worldToLocal(
    officeInterior.localToWorld(
      new THREE.Vector3(
        ZIP_LINE_ROOF_LOCAL[0],
        zipLineRoofFloorY + ZIP_LINE_ROOF_ANCHOR_HEIGHT,
        ZIP_LINE_ROOF_LOCAL[1],
      ),
    ),
  );
  ctx.zipLineStart = zipLineStart;
  const zipLineBeachDirection = new THREE.Vector2(
    zipLineStart.x - BEACH_CENTER_X,
    zipLineStart.z - BEACH_CENTER_Z,
  ).normalize();
  const zipLineEnd = new THREE.Vector3(
    BEACH_CENTER_X + zipLineBeachDirection.x * ZIP_LINE_BEACH_OFFSET,
    0.035 + ZIP_LINE_BEACH_ANCHOR_HEIGHT,
    BEACH_CENTER_Z + zipLineBeachDirection.y * ZIP_LINE_BEACH_OFFSET,
  );
  ctx.zipLineEnd = zipLineEnd;
  const zipLineHeading = Math.atan2(
    -(zipLineEnd.x - zipLineStart.x),
    -(zipLineEnd.z - zipLineStart.z),
  );
  ctx.zipLineHeading = zipLineHeading;
  const zipLineCurve = new THREE.CatmullRomCurve3(
    Array.from({ length: 33 }, (_, index) =>
      zipLineCablePoint(index / 32, new THREE.Vector3()),
    ),
  );
  const zipLineLength = zipLineCurve.getLength();
  // Riders let go a few units before the beach mast rather than inside it.
  const zipLineStopProgress = 1 - 3 / zipLineLength;
  const zipLineSteel = makeMaterial(THREE, "#3a4550", {
    metalness: 0.7,
    roughness: 0.34,
  });
  ctx.zipLineSteel = zipLineSteel;
  const zipLineAccent = makeMaterial(THREE, "#77d9ff", {
    emissive: "#1f8fb8",
    emissiveIntensity: 0.9,
  });
  ctx.zipLineAccent = zipLineAccent;
  const zipLineCable = new THREE.Mesh(
    new THREE.TubeGeometry(zipLineCurve, 96, 0.08, 5, false),
    zipLineSteel,
  );
  zipLineCable.name = "forkmesh-roof-beach-zip-line-cable";
  zipLine.add(zipLineCable);
  addZipLineMast(
    "forkmesh-roof-beach-zip-line-roof-mast",
    zipLine,
    zipLineStart.x,
    zipLineStart.y - ZIP_LINE_ROOF_ANCHOR_HEIGHT,
    zipLineStart.z,
    ZIP_LINE_ROOF_ANCHOR_HEIGHT + 0.5,
  );
  const zipLineBeachMast = addZipLineMast(
    "forkmesh-roof-beach-zip-line-beach-mast",
    beachScene,
    zipLineEnd.x,
    0.035,
    zipLineEnd.z,
    ZIP_LINE_BEACH_ANCHOR_HEIGHT + 0.5,
  );
  const zipLineBeachLabel = makeLabelSprite(
    THREE, "ZIP LINE LANDING", "FROM THE OFFICE ROOF", "#77d9ff",
  );
  zipLineBeachLabel.name = "forkmesh-roof-beach-zip-line-beach-label";
  zipLineBeachLabel.position.y = ZIP_LINE_BEACH_ANCHOR_HEIGHT + 3.2;
  zipLineBeachLabel.scale.set(6.4, 2.15, 1);
  zipLineBeachMast.add(zipLineBeachLabel);

  // The trolley parks at the roof anchor; its strap only shows during a ride.
  const zipLineTrolley = new THREE.Group();
  zipLineTrolley.name = "forkmesh-roof-beach-zip-line-trolley";
  const zipLineTrolleyBody = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.7, 0.46, 1.1),
    zipLineAccent,
  );
  zipLineTrolleyBody.position.y = -0.2;
  zipLineTrolleyBody.rotation.y = zipLineHeading;
  const zipLineStrapGeometry = new THREE.CylinderGeometry(0.05, 0.05, 1, 6);
  zipLineStrapGeometry.translate(0, -0.5, 0);
  const zipLineStrap = new THREE.Mesh(zipLineStrapGeometry, zipLineSteel);
  zipLineStrap.visible = false;
  zipLineTrolley.add(zipLineTrolleyBody, zipLineStrap);
  zipLineTrolley.position.copy(zipLineStart);
  zipLine.add(zipLineTrolley);

  const zipLineStation = new THREE.Group();
  zipLineStation.name = "forkmesh-office-rooftop-zip-line-station";
  zipLineStation.position.set(
    ZIP_LINE_ROOF_LOCAL[0] - 3,
    0,
    ZIP_LINE_ROOF_LOCAL[1],
  );
  zipLineStation.userData.officeFloorId = "rooftop";
  const zipLinePad = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 2.6, 2.6, 0.12, 24),
    zipLineAccent,
  );
  zipLinePad.position.set(3, 0.44, 0);
  const zipLineConsole = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 1.2, 2.4, 1.2),
    zipLineSteel,
  );
  zipLineConsole.position.set(0, 1.58, 3.4);
  zipLineStation.add(zipLinePad, zipLineConsole);
  zipLineStation.traverse((child) => {
    if (!child.isMesh) return;
    child.userData.officeFloorId = "rooftop";
    child.userData.interactive = "office-roof-zipline";
    interactive.push(child);
  });
  const zipLineHint = makeLabelSprite(
    THREE, "ZIP LINE TO BEACH", "CLICK OR E TO RIDE", "#77d9ff",
  );
  zipLineHint.name = "forkmesh-office-rooftop-zip-line-hint";
  zipLineHint.position.set(0, 5.2, 3.4);
  zipLineHint.scale.set(6.4, 2.15, 1);
  zipLineStation.add(zipLineHint);
  officeFloorGroups.get("rooftop").add(zipLineStation);
  registerWorldElement(
    "zip-line",
    "Roof-to-beach zip line",
    "Vehicles & rides",
    [zipLine, zipLineBeachMast, zipLineStation],
  );
  return {
    zipLineLength,
    zipLineStopProgress,
    zipLineTrolley,
    zipLineStrap,
  };
}
