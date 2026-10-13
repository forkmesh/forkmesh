import { wordTexture } from "../canvas.js";
import {
  LEADERBOARD_CONNECTION_MAX_X,
  LEADERBOARD_CONNECTION_MIN_X,
  LEADERBOARD_DISTRICT_GROUND_RADIUS,
  LEADERBOARD_ISLAND_CENTER_X,
  REPOSITORY_CONNECTION_MAX_X,
  REPOSITORY_CONNECTION_MIN_X,
  WORLD_PATH_CENTER_Y,
  WORLD_PATH_HEIGHT,
  WORLD_PATH_WIDTH,
} from "../layout.js";
import { makeSharedMaterial } from "../materials.js";
import { concreteBrickMaterial, createDistrictGroundCircle } from "../town.js";
import * as resources from "../resources.js";

export async function build({
  THREE,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  // Every repository imported from an external provider lives on its own
  // district, whether it is already hosted by a mirror or remains an external
  // stub. Broad earth connects it to town; the legacy "bridge" group is now a
  // paved promenade on that continuous causeway.
  const repositoryBridge = new THREE.Group();
  repositoryBridge.name = "hosted-repository-bridge";
  repositoryBridge.position.set(
    (REPOSITORY_CONNECTION_MIN_X + REPOSITORY_CONNECTION_MAX_X) / 2,
    0,
    0,
  );
  const repositoryConnectionLength =
    REPOSITORY_CONNECTION_MAX_X - REPOSITORY_CONNECTION_MIN_X;
  const repositoryPromenade = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      repositoryConnectionLength,
      WORLD_PATH_HEIGHT,
      WORLD_PATH_WIDTH,
    ),
    concreteBrickMaterial(THREE),
  );
  repositoryPromenade.name = "hosted-repository-promenade";
  repositoryPromenade.position.y = WORLD_PATH_CENTER_Y;
  repositoryPromenade.receiveShadow = true;
  repositoryPromenade.userData.ground = true;
  repositoryBridge.add(repositoryPromenade);
  world.add(repositoryBridge);
  registerWorldElement(
    "causeways", "Causeways & bridges", "Terrain", repositoryBridge,
  );
  await sceneSlice();

  // A purpose-built public rankings district balances the repository island
  // across town. The broad earth connection is always walkable, with an
  // illuminated promenade making the route legible at every sky theme.
  const leaderboardConnection = new THREE.Group();
  leaderboardConnection.name = "forkmesh-leaderboard-island-connection";
  leaderboardConnection.position.set(
    (LEADERBOARD_CONNECTION_MIN_X + LEADERBOARD_CONNECTION_MAX_X) / 2,
    0,
    0,
  );
  const leaderboardConnectionLength =
    LEADERBOARD_CONNECTION_MAX_X - LEADERBOARD_CONNECTION_MIN_X;
  const leaderboardPromenade = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      leaderboardConnectionLength,
      WORLD_PATH_HEIGHT,
      WORLD_PATH_WIDTH,
    ),
    concreteBrickMaterial(THREE),
  );
  leaderboardPromenade.name = "forkmesh-leaderboard-promenade";
  leaderboardPromenade.position.y = WORLD_PATH_CENTER_Y;
  leaderboardPromenade.receiveShadow = true;
  leaderboardPromenade.userData.ground = true;
  leaderboardConnection.add(leaderboardPromenade);
  world.add(leaderboardConnection);
  registerWorldElement(
    "causeways", "Causeways & bridges", "Terrain", leaderboardConnection,
  );
  await sceneSlice();

  const leaderboardDistrict = new THREE.Group();
  leaderboardDistrict.name = "forkmesh-leaderboard-district";
  leaderboardDistrict.position.set(LEADERBOARD_ISLAND_CENTER_X, 0, 0);
  const leaderboardInterior = new THREE.Group();
  leaderboardInterior.name = "forkmesh-leaderboard-interior";
  leaderboardInterior.userData.leaderboardInterior = true;
  leaderboardInterior.add(
    createDistrictGroundCircle(
      THREE,
      "leaderboards",
      LEADERBOARD_DISTRICT_GROUND_RADIUS,
    ),
  );
  leaderboardDistrict.add(leaderboardInterior);
  leaderboardDistrict.userData.leaderboardInterior = leaderboardInterior;
  const leaderboardBeacon = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 3.1, 4.2, 0.7, 24),
    makeSharedMaterial(THREE, "#173c35", {
      metalness: 0.35,
      roughness: 0.46,
    }),
  );
  leaderboardBeacon.position.y = 0.35;
  // Retained as a detached construction value for older layout snapshots;
  // the center is intentionally open and contains no "leaderboard area"
  // monument.
  leaderboardBeacon.visible = false;
  const leaderboardIslandTitle = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 7.4, 2.45),
    resources.uniqueMaterial(THREE, {
      map: wordTexture(
        THREE,
        "Leaderboard Island",
        "14 live public rankings",
        "#9ef7c6",
      ),
      transparent: true,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  leaderboardIslandTitle.name = "forkmesh-leaderboard-island-title";
  leaderboardIslandTitle.position.set(0, 3.1, 0);
  leaderboardIslandTitle.rotation.y = -Math.PI / 2;
  leaderboardIslandTitle.visible = false;
  world.add(leaderboardDistrict);
  registerWorldElement(
    "leaderboard-district", "Leaderboard district", "Districts",
    leaderboardDistrict,
  );
  return {
    leaderboardDistrict,
    leaderboardInterior,
  };
}
