import { createDesertPlots } from "../desert-plots.js";
import {
  CONTINUOUS_CITY_CENTER_Z,
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  CONTINUOUS_CITY_SURFACE_Y,
} from "../layout.js";
import {
  applyGroundLogo,
  cityGrassMaterial,
  createMainlandDistanceLod,
  createTownLandscape,
  roundedHexagonGeometry,
} from "../town.js";

export async function build({
  THREE,
  interactive,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  // One flat, textured city surface sits under every district, path, and
  // building. The prior thick cylinder contributed a ring of radial faces to
  // the triangle inspector even though it added no visible walkable surface.
  const continuousCityLand = new THREE.Mesh(
    roundedHexagonGeometry(
      THREE,
      1,
      CONTINUOUS_CITY_CORNER_RADIUS / CONTINUOUS_CITY_RADIUS,
      16,
    ),
    applyGroundLogo(THREE, cityGrassMaterial(THREE)),
  );
  continuousCityLand.name = "forkmesh-continuous-city-land";
  continuousCityLand.rotation.x = -Math.PI / 2;
  continuousCityLand.scale.set(
    CONTINUOUS_CITY_RADIUS,
    CONTINUOUS_CITY_RADIUS,
    1,
  );
  continuousCityLand.position.set(
    0,
    CONTINUOUS_CITY_SURFACE_Y,
    CONTINUOUS_CITY_CENTER_Z,
  );
  continuousCityLand.receiveShadow = true;
  continuousCityLand.userData.ground = true;
  world.add(continuousCityLand);

  const townLandscape = createTownLandscape(THREE);
  world.add(townLandscape);
  const mainlandDistanceLod = createMainlandDistanceLod(THREE);
  world.add(mainlandDistanceLod);
  const desertPlots = createDesertPlots(THREE, interactive);
  world.add(desertPlots);
  registerWorldElement(
    "city-terrain", "City terrain", "Terrain",
    continuousCityLand,
  );
  await sceneSlice();
  registerWorldElement(
    "town-landscape", "Town landscaping, buildings & trees", "Terrain",
    townLandscape,
  );
  await sceneSlice();
  registerWorldElement(
    "mainland-distance-lod", "Mainland horizon proxy", "Terrain",
    mainlandDistanceLod,
  );
  await sceneSlice();
  registerWorldElement(
    "desert-plots", "Desert plots · start your world signs", "Terrain",
    desertPlots,
  );
  return {
    continuousCityLand,
    desertPlots,
    mainlandDistanceLod,
  };
}
