import {
  createInstancedMirrorSet,
  sharedGeometry,
} from "../../world-resource-cache.js";
import { makeLabelSprite } from "../labels.js";
import {
  REPOSITORY_ISLAND_CENTER_X,
  REPOSITORY_ISLAND_RING_RADIUS,
} from "../layout.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { clamp, removeGeneratedLayer } from "../object-utils.js";
import {
  REPOSITORY_CATALOG_MAX,
  REPOSITORY_EDGE_RADIUS,
  REPOSITORY_FOLLOWERS_VISIBLE,
  REPOSITORY_SPARKLE_MS,
  compactSceneBytes,
  makeRepositoryFollowerIcon,
  makeRepositorySizeMapSparkle,
  repositoryCommitActivityTexture,
  repositoryFollowButtonTexture,
  repositorySizeLabelSprite,
  repositorySizeMapSegments,
  repositoryStarPlaneTexture,
  repositoryWedgeGeometry,
} from "../repositories.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  reducedMotion,
  ctx,
  fns,
}) {
  let repositoryPortalMirror = null;

  function updateRepositoryCatalog(repositories = [], activeRepository = {}) {
    const district = ctx.landmarkObjects.get("repositories");
    const districtPortal = district?.userData?.portal;
    if (!districtPortal) return;
    const records = (Array.isArray(repositories) ? repositories : [])
      .filter((record) => record && (record.owner || record.name))
      // A retired development placeholder can still be advertised by an old
      // node snapshot. It has never hosted content, so do not turn the
      // non-live vm1/forkmesh stub into a permanent "mirror needed" portal.
      .filter(
        (record) =>
          !(
            String(record.owner || "").toLowerCase() === "vm1" &&
            String(record.name || "").toLowerCase() === "forkmesh" &&
            record.liveHost !== true
          ),
      )
      .slice(0, REPOSITORY_CATALOG_MAX)
      .map((record) => {
        const owner = String(record.owner || "external").slice(0, 40);
        const name = String(record.name || "repository").slice(0, 60);
        const rawBytes = Number(record.sizeBytes);
        const sizeBytes =
          Number.isSafeInteger(rawBytes) && rawBytes >= 0
            ? Math.min(rawBytes, 2 ** 50)
            : 0;
        const rawStarCount = Number(record.starCount);
        const rawFollowerCount = Number(record.fediverseFollowerCount);
        const rawSizeTree =
          record.sizeTree &&
          typeof record.sizeTree === "object" &&
          Number(record.sizeTree.size) > 0 &&
          Array.isArray(record.sizeTree.children)
            ? record.sizeTree
            : null;
        return {
          owner,
          name,
          key: `${owner.toLocaleLowerCase()}/${name.toLocaleLowerCase()}`,
          sizeBytes,
          liveHost: record.liveHost === true,
          isPrivate: record.isPrivate === true,
          termsFlagged: record.termsFlagged === true,
          termsCategory: String(record.termsCategory || "").slice(0, 20),
          source: String(record.source || "").slice(0, 40),
          provider: String(record.provider || "").slice(0, 20),
          providerLabel: String(record.providerLabel || "").slice(0, 30),
          externalUrl: String(record.externalUrl || "").slice(0, 500),
          importId: String(record.importId || "").slice(0, 64),
          mirrorState: String(record.mirrorState || "").slice(0, 24),
          starCount:
            Number.isSafeInteger(rawStarCount) && rawStarCount >= 0
              ? Math.min(rawStarCount, 10_000_000)
              : null,
          starred: record.starred === true,
          fediverseFollowerCount:
            Number.isSafeInteger(rawFollowerCount) && rawFollowerCount >= 0
              ? Math.min(rawFollowerCount, 10_000_000)
              : null,
          fediverseFollowerStatus: String(
            record.fediverseFollowerStatus || "idle",
          ).slice(0, 20),
          fediverseHandle: String(record.fediverseHandle || "").slice(0, 120),
          fediverseActorUrl: String(record.fediverseActorUrl || "").slice(
            0,
            500,
          ),
          fediverseFollowers: Array.isArray(record.fediverseFollowers)
            ? record.fediverseFollowers.slice(0, REPOSITORY_FOLLOWERS_VISIBLE)
            : [],
          sizeTree: rawSizeTree,
        };
      })
      .sort((left, right) => left.key.localeCompare(right.key));
    const activeKey = `${
      String(activeRepository?.owner || "").toLocaleLowerCase()
    }/${
      String(
        activeRepository?.repo || activeRepository?.name || "",
      ).toLocaleLowerCase()
    }`;
    ctx.world.userData.repositoryActiveKey = activeKey;
    const catalogSignature = JSON.stringify([
      activeKey,
      records.map((record) => [
        record.key,
        record.sizeBytes,
        record.liveHost,
        record.isPrivate,
        record.termsFlagged,
        record.termsCategory,
        record.source,
        record.provider,
        record.importId,
        record.starCount,
        record.starred,
        record.fediverseFollowerCount,
        record.fediverseFollowerStatus,
        record.fediverseHandle,
        record.fediverseActorUrl,
        record.sizeTree
          ? [
              String(record.sizeTree.commit || "").slice(0, 64),
              Number(record.sizeTree.size) || 0,
              record.sizeTree.children.length,
            ]
          : null,
        // Identity only: a follower's avatar/bio arriving does not need a
        // portal rebuild, but a different follower does.
        record.fediverseFollowers.map((follower) => follower?.handle || ""),
      ]),
    ]);
    const previousCatalog = ctx.world.userData.repositoryCatalogLayer;
    const previousPortalKeys = new Set(ctx.repositoryPortals.keys());
    if (
      catalogSignature === ctx.world.userData.repositoryCatalogSignature &&
      (records.length
        ? previousCatalog?.parent === ctx.world
        : !previousCatalog)
    ) {
      return;
    }
    fns.unregisterCompactDistrictRoot("repositories", previousCatalog);
    if (repositoryPortalMirror) {
      ctx.instancedMirrorSets.delete(repositoryPortalMirror);
      repositoryPortalMirror.dispose();
      repositoryPortalMirror = null;
    }
    removeGeneratedLayer(ctx.world, previousCatalog, ctx.interactive);
    ctx.world.userData.repositoryCatalogLayer = null;
    ctx.world.userData.repositorySizeLayer = null;
    ctx.world.userData.repositorySizeMount = null;
    ctx.world.userData.repositoryRecordDeskLayer = null;
    ctx.world.userData.repositoryRecordDeskMount = null;
    ctx.world.userData.repositoryRecordDeskData = null;
    ctx.world.userData.repositoryActivityLayer = null;
    ctx.world.userData.repositoryActivityMount = null;
    ctx.world.userData.repositoryPortalMeshes = [];
    ctx.world.userData.repositoryCatalogSignature = catalogSignature;
    ctx.repositoryPortals.clear();
    if (district.userData.legacyFiles) {
      district.userData.legacyFiles.visible = !records.length;
    }
    if (!records.length) return;

    const layer = new THREE.Group();
    layer.name = "repository-perimeter-portals";
    // The east district is the one repository home. Native and imported
    // records share its fully expanded ring instead of leaving a second,
    // partial repository orbit around the central node plaza.
    const coreRecords = [];
    const hostedRecords = records;
    [
      ["repository-perimeter-guide", REPOSITORY_EDGE_RADIUS, 0, coreRecords],
      [
        "hosted-repository-island-guide",
        REPOSITORY_ISLAND_RING_RADIUS,
        REPOSITORY_ISLAND_CENTER_X,
        hostedRecords,
      ],
    ].forEach(([name, radius, centerX, cohort]) => {
      if (!cohort.length) return;
      const guide = new THREE.Mesh(
        resources.primitive(THREE, "TorusGeometry", radius, 0.045, 4, 128),
        makeSharedMaterial(THREE, "#77d9ff", {
          emissive: "#1e637c",
          emissiveIntensity: 0.45,
          transparent: true,
          opacity: 0.38,
          roughness: 0.5,
        }),
      );
      guide.name = name;
      guide.rotation.x = Math.PI / 2;
      guide.position.set(centerX, 0.12, 0);
      layer.add(guide);
    });

    const maxBytes = Math.max(0, ...records.map((record) => record.sizeBytes));
    const diskGeometry = resources.primitive(THREE, "CircleGeometry", 1, 36);
    const outlineGeometry = resources.primitive(THREE, "TorusGeometry", 1.08, 0.065, 8, 40);
    const baseGeometry = resources.primitive(THREE, "BoxGeometry", 2.35, 0.22, 1.15);
    const materials = {
      live: makeMaterial(THREE, "#77d9ff", {
        emissive: "#207f9e",
        emissiveIntensity: 0.74,
        metalness: 0.25,
        roughness: 0.34,
      }),
      selected: makeMaterial(THREE, "#9ef7c6", {
        emissive: "#2ca76c",
        emissiveIntensity: 1.1,
        metalness: 0.25,
        roughness: 0.26,
      }),
      private: makeMaterial(THREE, "#d5b6ff", {
        emissive: "#7650a7",
        emissiveIntensity: 0.76,
        metalness: 0.24,
        roughness: 0.36,
      }),
      stub: makeMaterial(THREE, "#748a82", {
        emissive: "#294139",
        emissiveIntensity: 0.25,
        metalness: 0.08,
        roughness: 0.72,
      }),
      syncing: makeMaterial(THREE, "#f0c66f", {
        emissive: "#8d641e",
        emissiveIntensity: 0.62,
        metalness: 0.12,
        roughness: 0.52,
      }),
      outline: makeMaterial(THREE, "#b9edff", {
        emissive: "#3aa1c7",
        emissiveIntensity: 0.78,
        metalness: 0.2,
        roughness: 0.35,
      }),
      base: makeMaterial(THREE, "#102a32", {
        emissive: "#163f49",
        emissiveIntensity: 0.24,
        roughness: 0.72,
      }),
    };
    const usedMaterials = new Set();
    const portalMeshes = [];
    const orderedRecords = [...coreRecords, ...hostedRecords];
    orderedRecords.forEach((record, index) => {
      const islandRecord = true;
      const cohort = islandRecord ? hostedRecords : coreRecords;
      const cohortIndex = cohort.findIndex(
        (candidate) => candidate.key === record.key,
      );
      const ringRadius = islandRecord
        ? REPOSITORY_ISLAND_RING_RADIUS
        : REPOSITORY_EDGE_RADIUS;
      const centerX = islandRecord ? REPOSITORY_ISLAND_CENTER_X : 0;
      const angle =
        Math.PI / 2 +
        (cohortIndex / Math.max(1, cohort.length)) * Math.PI * 2;
      const portalSpacing =
        (Math.PI * 2 * ringRadius) / Math.max(1, cohort.length);
      const portalDensityScale = clamp(
        (portalSpacing - 0.06) / 3.15,
        0.58,
        1,
      );
      const isActive = record.key === activeKey;
      const material = isActive
        ? materials.selected
          : record.isPrivate
            ? materials.private
            : record.liveHost
              ? materials.live
              : record.mirrorState === "syncing"
                ? materials.syncing
              : materials.stub;
      const outlineMaterial = isActive
        ? materials.selected
        : materials.outline;
      usedMaterials.add(material);
      usedMaterials.add(outlineMaterial);
      usedMaterials.add(materials.base);
      const sizeRatio =
        maxBytes > 0 && record.sizeBytes > 0
          ? Math.log1p(record.sizeBytes) / Math.log1p(maxBytes)
          : 0.36;
      const nodeRadius =
        0.72 + sizeRatio * 0.2 + (isActive ? 0.08 : 0);
      const node = new THREE.Group();
      node.name = `repository-portal:${record.owner}/${record.name}`;
      node.position.set(
        centerX + Math.cos(angle) * ringRadius,
        2.55,
        Math.sin(angle) * ringRadius,
      );
      node.rotation.y = -angle - Math.PI / 2;
      // A portal rises when it is genuinely new to the ring: a fresh import, or
      // the whole district arriving at once because a character just walked
      // onto the repositories circle. The district reveal stages tightly so a
      // full catalog reads as one wave rather than a several-minute queue.
      const risesOnArrival =
        !previousPortalKeys.has(record.key) &&
        (ctx.repositoryRevealPending ||
          ["external-import", "hosted-import", "bulk-import"].includes(
            record.source,
          ));
      if (risesOnArrival) {
        const risingBefore = records
          .slice(0, index)
          .filter(
            (candidate) =>
              !previousPortalKeys.has(candidate.key) &&
              (ctx.repositoryRevealPending ||
                ["external-import", "hosted-import", "bulk-import"].includes(
                  candidate.source,
                )),
          ).length;
        const bornAt =
          performance.now() +
          risingBefore * (ctx.repositoryRevealPending ? 90 : 320);
        ctx.repositoryPortalBornAt.set(record.key, bornAt);
        ctx.repositorySparkleUntil.set(
          record.key,
          bornAt + REPOSITORY_SPARKLE_MS,
        );
        node.scale.setScalar(reducedMotion ? 1 : 0.015);
      }
      // The size tree has landed, so this portal has nothing left to build.
      if (record.sizeTree) ctx.repositorySparkleUntil.delete(record.key);
      const face = new THREE.Group();
      face.name = `repository-portal-face:${record.owner}/${record.name}`;
      face.scale.setScalar(portalDensityScale);
      const disk = new THREE.Mesh(diskGeometry, material);
      disk.position.z = 0.016;
      disk.scale.setScalar(nodeRadius);
      const outline = new THREE.Mesh(
        outlineGeometry,
        outlineMaterial,
      );
      outline.scale.setScalar(nodeRadius);
      const repositoryPortal = {
        owner: record.owner,
        name: record.name,
        sizeBytes: record.sizeBytes,
        liveHost: record.liveHost,
        isPrivate: record.isPrivate,
        termsFlagged: record.termsFlagged,
        termsCategory: record.termsCategory,
        source: record.source,
        provider: record.provider,
        providerLabel: record.providerLabel,
        externalUrl: record.externalUrl,
        importId: record.importId,
        mirrorState: record.mirrorState,
        starCount: record.starCount,
        starred: record.starred,
        angle,
        island: islandRecord,
      };
      for (const mesh of [disk, outline]) {
        mesh.userData.landmark = "repositories";
        mesh.userData.repositoryPortal = repositoryPortal;
        ctx.interactive.push(mesh);
        portalMeshes.push(mesh);
      }
      face.add(disk, outline);
      // Every repository — including a stub whose size tree has not reached
      // this mirror yet — carries the same concentric profile, but it stays
      // folded into the portal's own outline until somebody walks inside the
      // ring. From the town the east island is a circle of plain discs; the
      // rings only expand for the visitor standing among them, and a real
      // sizeTree replaces the centre with the file wedges below without
      // collapsing them again. updateRepositoryRingExpansion owns the animation.
      const expandedProfile = new THREE.Group();
      expandedProfile.name =
        `repository-always-expanded-profile:${record.owner}/${record.name}`;
      [1.18, 1.38, 1.58].forEach((radius, ringIndex) => {
        const ring = new THREE.Mesh(
          sharedGeometry(THREE, "TorusGeometry", radius, 0.035, 6, 36),
          outlineMaterial,
        );
        ring.name =
          `repository-expanded-ring-${ringIndex + 1}:${record.owner}/${record.name}`;
        ring.position.z = -0.008 - ringIndex * 0.006;
        ring.scale.setScalar(nodeRadius);
        expandedProfile.add(ring);
      });
      fns.applyRepositoryRingExpansion(expandedProfile);
      face.add(expandedProfile);
      node.userData.repositoryExpandedProfile = expandedProfile;
      // Still waiting on this repository's size tree. Shimmer while the map is
      // assembled — the wedges below replace the shimmer the moment it lands.
      const sparkleUntil = ctx.repositorySparkleUntil.get(record.key) || 0;
      if (!reducedMotion && !record.sizeTree && performance.now() < sparkleUntil) {
        const sparkle = makeRepositorySizeMapSparkle(
          THREE,
          nodeRadius,
          `${record.owner}/${record.name}`,
        );
        sparkle.userData.repositorySparkleUntil = sparkleUntil;
        face.add(sparkle);
        node.userData.repositorySparkle = sparkle;
      }
      if (record.termsFlagged) {
        const policyFlag = new THREE.Group();
        policyFlag.name =
          `repository-terms-flag:${record.owner}/${record.name}`;
        policyFlag.position.set(nodeRadius * 0.72, nodeRadius * 0.72, 0.16);
        const pole = new THREE.Mesh(
          resources.primitive(THREE, "BoxGeometry", 0.035, 0.44, 0.035),
          makeSharedMaterial(THREE, "#d7e1df", { metalness: 0.45 }),
        );
        pole.position.y = 0.16;
        const cloth = new THREE.Mesh(
          resources.primitive(THREE, "PlaneGeometry", 0.34, 0.2),
          makeSharedMaterial(THREE, "#ef4444", {
            emissive: "#7f1d1d",
            emissiveIntensity: 0.5,
          }),
        );
        cloth.position.set(0.18, 0.32, 0.02);
        policyFlag.add(pole, cloth);
        face.add(policyFlag);
      }
      if (!isActive && record.sizeTree) {
        const map = repositorySizeMapSegments(record.sizeTree, "");
        const miniature = new THREE.Group();
        miniature.name =
          `repository-mini-size-map:${record.owner}/${record.name}`;
        miniature.position.z = 0.09;
        const innerRadius = nodeRadius * 0.18;
        const outerRadius = nodeRadius * 0.91;
        const ringWidth =
          (outerRadius - innerRadius) / Math.max(1, map.ringCount);
        map.segments.slice(0, 24).forEach((segment) => {
          const geometry = repositoryWedgeGeometry(
            THREE,
            innerRadius + (segment.depth - 1) * ringWidth + 0.006,
            innerRadius + segment.depth * ringWidth - 0.006,
            segment.from,
            segment.to,
            0.045,
          );
          if (!geometry) return;
          const wedge = new THREE.Mesh(
            geometry,
            resources.uniqueMaterial(THREE, {
              color: segment.color,
              transparent: segment.type === "summary",
              opacity: segment.type === "summary" ? 0.62 : 0.98,
              side: THREE.DoubleSide,
              forceSinglePass: true,
              toneMapped: false,
            }, "MeshBasicMaterial"),
          );
          wedge.position.z = segment.depth * 0.006;
          miniature.add(wedge);
        });
        miniature.userData.repositorySizeMap = {
          segments: map.segments.slice(0, 24),
          innerRadius,
          ringWidth,
          outerRadius,
          lift: 0.06,
          depthStep: 0.006,
        };
        face.add(miniature);
        node.userData.repositoryMiniSizeMap = miniature;
      }
      if (isActive) {
        const selectedHalo = new THREE.Mesh(
          resources.primitive(THREE, "TorusGeometry", 1.38, 0.055, 8, 40),
          materials.selected,
        );
        selectedHalo.name = "repository-selected-halo";
        selectedHalo.scale.setScalar(nodeRadius);
        selectedHalo.userData.repositoryHaloScale = nodeRadius;
        face.add(selectedHalo);
        usedMaterials.add(materials.selected);
        const orbitMarker = new THREE.Mesh(
          resources.primitive(THREE, "SphereGeometry", 0.105, 12, 8),
          materials.selected,
        );
        orbitMarker.name = "repository-live-orbit-marker";
        orbitMarker.userData.repositoryOrbitRadius = nodeRadius * 1.38;
        orbitMarker.position.set(
          orbitMarker.userData.repositoryOrbitRadius,
          0,
          0.08,
        );
        face.add(orbitMarker);
        // Direct references for the per-frame pulse — animate() must not run
        // a recursive name lookup over every portal subtree each frame.
        node.userData.repositoryHalo = selectedHalo;
        node.userData.repositoryOrbitMarker = orbitMarker;
      }
      node.add(face);

      const label = repositorySizeLabelSprite(
        THREE,
        `${record.owner}/${record.name}`,
        record.isPrivate
          ? "AUTHORIZED PRIVATE"
          : record.liveHost
            ? record.sizeBytes
              ? `${compactSceneBytes(record.sizeBytes)} HOSTED`
              : "LIVE MIRROR"
            : record.source === "external-import"
              ? `${record.providerLabel || "EXTERNAL"} IMPORT`
            : record.mirrorState === "syncing"
              ? "MIRRORS SYNCING"
              : record.mirrorState === "offline"
                ? "MIRRORS OFFLINE"
                : "STUB · MIRROR NEEDED",
        isActive
          ? "#9ef7c6"
          : record.isPrivate
            ? "#d5b6ff"
            : record.mirrorState === "syncing"
              ? "#f0c66f"
              : record.mirrorState === "offline"
                ? "#91a39a"
                : "#77d9ff",
      );
      label.name = `repository-portal-label:${record.owner}/${record.name}`;
      label.scale.set(2.8, 0.76, 1);
      label.position.set(0, -1.42, 0.12);
      // The east island is a repository catalog rather than a selector that
      // hides every inactive name. Density scaling keeps the full labels
      // readable while every repository remains expanded.
      label.visible = true;
      node.add(label);

      if (isActive && !record.isPrivate) {
        const starData = {
          owner: record.owner,
          name: record.name,
          starCount: record.starCount,
          starred: record.starred,
        };
        // This is intentionally a fixed plane, not a 3D extrusion or a
        // camera-following sprite. Every repository therefore gets the same
        // readable, point-up star control when approached from the ring.
        const starButton = new THREE.Mesh(
          resources.primitive(THREE, "PlaneGeometry", 1.92, 1.92),
          resources.uniqueMaterial(THREE, {
            map: repositoryStarPlaneTexture(
              THREE,
              record.starCount,
              record.starred,
            ),
            transparent: true,
            side: THREE.DoubleSide,
            forceSinglePass: true,
            depthWrite: false,
            toneMapped: false,
          }, "MeshBasicMaterial"),
        );
        starButton.name = `repository-star-button:${record.owner}/${record.name}`;
        starButton.position.set(0, 4.05, 0.5);
        starButton.userData.landmark = "repositories";
        starButton.userData.repositoryStar = starData;
        node.add(starButton);
        ctx.interactive.push(starButton);
        const starCaption = repositorySizeLabelSprite(
          THREE,
          Number.isSafeInteger(record.starCount)
            ? `${record.starCount.toLocaleString("en-US")} STARS`
            : "STARS",
          record.starred ? "STARRED · CLICK TO REMOVE" : "CLICK TO STAR",
          record.starred ? "#f7c96b" : "#9ef7c6",
        );
        starCaption.name = `repository-star-caption:${record.owner}/${record.name}`;
        starCaption.scale.set(2.4, 0.68, 1);
        starCaption.position.set(0, 2.92, 0.5);
        node.add(starCaption);
      }
      const base = new THREE.Mesh(baseGeometry, materials.base);
      base.name = `repository-portal-base:${record.owner}/${record.name}`;
      base.position.set(0, -2.38, 0);
      base.scale.x = portalDensityScale;
      // The foundation is a direct link to the repository page. Keep its
      // action distinct from the face, which selects the portal in-world.
      base.userData.landmark = "repositories";
      base.userData.repositoryBase = repositoryPortal;
      ctx.interactive.push(base);
      node.add(base);
      // The angled commit-activity pedestal below carries the selected
      // repository name, so the wheel no longer needs a second name stone.
      // Who follows this repository over ActivityPub, shown as compact avatar
      // icons orbiting the selected file circle. These are real remote accounts
      // read from ap_followers; using icons keeps the repository graph visible
      // and avoids mistaking follower records for people physically present.
      if (isActive && !record.isPrivate) {
        const followers = Array.isArray(record.fediverseFollowers)
          ? record.fediverseFollowers.filter(
              (follower) =>
                follower &&
                String(follower.handle || follower.profileUrl || "").trim(),
            )
          : [];
        const gallery = new THREE.Group();
        gallery.name = `repository-fediverse-follower-icons:${record.owner}/${record.name}`;
        // Keep the social portraits on the wheel's local face plane. A deep
        // positive-Z offset made them parallax toward the camera and appear
        // detached from the circle at elevated viewing angles.
        gallery.position.set(0, 0.55, 0.18);
        const visibleFollowers = followers.slice(
          0,
          REPOSITORY_FOLLOWERS_VISIBLE,
        );
        visibleFollowers.forEach((follower, followerIndex) => {
          const icon = makeRepositoryFollowerIcon(THREE, follower);
          // A broad upper arc makes the large follower portraits an explicit
          // OUTER social ring while keeping the stone plaque clear.
          const orbitAngle =
            Math.PI * 1.08 -
            (followerIndex /
              Math.max(1, visibleFollowers.length - 1)) *
              Math.PI *
              1.16;
          const orbitRadius = 4.25;
          icon.position.set(
            Math.cos(orbitAngle) * orbitRadius,
            Math.sin(orbitAngle) * orbitRadius,
            0,
          );
          gallery.add(icon);
        });
        const followerTotal = Math.max(
          followers.length,
          Number(record.fediverseFollowerCount) || 0,
        );
        const captionTitle = followerTotal
          ? `FEDIVERSE ${followerTotal.toLocaleString("en-US")}`
          : "NOBODY FOLLOWS THIS REPOSITORY YET";
        const captionDetail =
          followerTotal > visibleFollowers.length
            ? `SHOWING ${visibleFollowers.length}`
            : "PUBLIC FOLLOWERS";
        const caption = makeLabelSprite(
          THREE,
          captionTitle,
          captionDetail,
          "#ff9eb7",
        );
        caption.name = "repository-fediverse-follower-caption";
        caption.position.set(0, -4.25, 0.04);
        caption.scale.set(3.2, 0.92, 1);
        caption.material.depthTest = false;
        caption.material.depthWrite = false;
        caption.renderOrder = 43;
        gallery.add(caption);
        // The size-map view hides the original portal face. Keep the social
        // ring on the persistent portal mount so real followers remain visible
        // around the live file sunburst.
        node.add(gallery);
        node.userData.fediverseFollowers = visibleFollowers;

        if (record.fediverseActorUrl || record.fediverseHandle) {
          const follow = new THREE.Mesh(
            resources.primitive(THREE, "PlaneGeometry", 1.5, 0.66),
            resources.uniqueMaterial(THREE, {
              map: repositoryFollowButtonTexture(
                THREE,
                record.fediverseHandle,
              ),
              transparent: true,
              depthTest: false,
              depthWrite: false,
              side: THREE.DoubleSide,
              forceSinglePass: true,
              toneMapped: false,
            }, "MeshBasicMaterial"),
          );
          follow.name = `repository-fediverse-follow:${record.owner}/${record.name}`;
          follow.position.set(1.8, 5.18, 0.2);
          follow.renderOrder = 42;
          follow.userData.landmark = "repositories";
          follow.userData.repositoryFediverseFollow = {
            owner: record.owner,
            name: record.name,
            handle: String(record.fediverseHandle || "").slice(0, 120),
            actorUrl: String(record.fediverseActorUrl || "").slice(0, 500),
          };
          node.add(follow);
          ctx.interactive.push(follow);
        }
      }
      node.userData.repositoryPortal = repositoryPortal;
      node.userData.repositoryFace = face;
      node.userData.repositoryLabel = label;
      layer.add(node);
      ctx.repositoryPortals.set(record.key, {
        group: node,
        owner: record.owner,
        name: record.name,
        angle,
        key: record.key,
        record,
      });
    });
    // The district has now risen once. Later catalog refreshes are ordinary
    // updates again, so only genuinely new imports get the arrival animation.
    ctx.repositoryRevealPending = false;
    ctx.repositorySparkleUntil.forEach((_, key) => {
      if (!ctx.repositoryPortals.has(key)) ctx.repositorySparkleUntil.delete(key);
    });

    layer.userData.repositoryCatalogLayer = true;
    repositoryPortalMirror = createInstancedMirrorSet(THREE, layer, {
      name: "repository-portal-instances",
    });
    layer.traverse((object) => {
      if (
        object.isMesh &&
        (object.geometry === diskGeometry ||
          object.geometry === outlineGeometry ||
          object.geometry === baseGeometry ||
          object.name.startsWith("repository-expanded-ring-"))
      ) {
        repositoryPortalMirror.add(object);
      }
    });
    ctx.instancedMirrorSets.add(repositoryPortalMirror);
    ctx.world.add(layer);
    fns.registerCompactDistrictRoot("repositories", layer);
    Object.values(materials).forEach((material) => {
      if (!usedMaterials.has(material)) material.dispose();
    });
    ctx.world.userData.repositoryCatalogLayer = layer;
    fns.registerWorldElement(
      "repository-portals",
      "Repository portal ring",
      "Districts",
      layer,
      () => ctx.world.userData.repositoryCatalogLayer === layer,
    );
    ctx.world.userData.repositoryPortalMeshes = portalMeshes;
    const activityData = ctx.world.userData.repositoryActivityData;
    const activitySelection = activityData?.selection || {};
    const activityKey = `${String(
      activitySelection.owner || "",
    ).toLocaleLowerCase()}/${String(
      activitySelection.repo || activitySelection.name || "",
    ).toLocaleLowerCase()}`;
    if (activityData && activityKey === activeKey) {
      updateRepositoryActivity(activityData.activity, activitySelection);
    }
  }

  function setRepositoryImportState(state = {}) {
    const district = ctx.landmarkObjects.get("repositories");
    const kiosk = district?.userData?.repositoryImportKiosk;
    if (!kiosk) return;
    kiosk.userData.importing = state.active === true;
    kiosk.userData.importStage = String(state.stage || "").slice(0, 40);
    kiosk.userData.importProvider = String(state.provider || "").slice(0, 20);
    const spark = kiosk.userData.importSpark;
    if (spark?.material?.color) {
      const color =
        state.status === "error"
          ? "#ff7f8f"
          : state.status === "complete"
            ? "#9ef7c6"
            : state.provider === "gitlab"
              ? "#fc8d45"
              : state.provider === "codeberg"
                ? "#77d9ff"
                : "#f0f6fc";
      spark.material.color.set(color);
      spark.material.emissive?.set?.(color);
    }
  }

  function updateRepositoryActivity(activity = {}, selection = {}) {
    ctx.world.userData.repositoryActivityData = { activity, selection };
    const previousLayer = ctx.world.userData.repositoryActivityLayer;
    const previousMount =
      ctx.world.userData.repositoryActivityMount || previousLayer?.parent;
    removeGeneratedLayer(previousMount, previousLayer, ctx.interactive);
    ctx.world.userData.repositoryActivityLayer = null;
    ctx.world.userData.repositoryActivityMount = null;

    const owner = String(selection?.owner || "").slice(0, 40);
    const name = String(selection?.repo || selection?.name || "").slice(0, 60);
    const repositoryKey =
      `${owner.toLocaleLowerCase()}/${name.toLocaleLowerCase()}`;
    const portalRecord = ctx.repositoryPortals.get(repositoryKey);
    const mount = portalRecord?.group;
    if (!mount) return;
    const runningAgents = ctx.agentBotAccessAllowed
      ? (ctx.repositoryAgentTasksByRepository.get(repositoryKey) || []).length
      : 0;
    const rawFollowerCount = portalRecord?.record?.fediverseFollowerCount;
    const visibleFollowerCount = Array.isArray(
      portalRecord?.record?.fediverseFollowers,
    )
      ? portalRecord.record.fediverseFollowers.length
      : 0;
    const fediverseFollowers =
      Number.isSafeInteger(rawFollowerCount) && rawFollowerCount >= 0
        ? rawFollowerCount
        : visibleFollowerCount;

    const layer = new THREE.Group();
    layer.name = "repository-commit-activity";
    layer.userData.repositoryKey = repositoryKey;
    layer.userData.commit = String(selection?.commit || "").slice(0, 64);

    // The selected file graph nearly reaches the ground. Its commit history is
    // now the named repository pedestal: a compact 45-degree lectern that can
    // be read while looking toward the wheel without lying flat on the ground.
    const backing = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 6.25, 2.25, 0.14),
      makeSharedMaterial(THREE, "#102a32", {
        emissive: "#123d49",
        emissiveIntensity: 0.36,
        metalness: 0.16,
        roughness: 0.68,
      }),
    );
    backing.name = "repository-commit-activity-pedestal";
    backing.rotation.x = -Math.PI / 4;
    backing.position.set(0, -1.58, 2.92);
    layer.add(backing);
    const chart = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 6.05, 2.02),
      resources.uniqueMaterial(THREE, {
        map: repositoryCommitActivityTexture(
          THREE,
          activity,
          `${owner}/${name}`,
          { runningAgents, fediverseFollowers },
        ),
        side: THREE.DoubleSide,
        forceSinglePass: true,
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    chart.name = "repository-commit-activity-chart";
    chart.rotation.x = -Math.PI / 4;
    chart.position.set(0, -1.55, 3.02);
    chart.userData.landmark = "repositories";
    chart.userData.repositoryCommitActivity = {
      owner,
      name,
      commit: String(selection?.commit || "").slice(0, 64),
      timezone: "UTC",
    };
    layer.add(chart);

    mount.add(layer);
    ctx.world.userData.repositoryActivityLayer = layer;
    ctx.world.userData.repositoryActivityMount = mount;
  }

  return {
    updateRepositoryCatalog,
    setRepositoryImportState,
    updateRepositoryActivity,
  };
}
