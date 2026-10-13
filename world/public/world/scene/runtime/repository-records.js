import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { clamp, removeGeneratedLayer } from "../object-utils.js";
import {
  compactSceneBytes,
  repositoryIssueAgentModelTexture,
  repositoryIssueAgentProviderTexture,
  repositoryIssueCardTexture,
  repositoryIssuePageTexture,
  repositoryPullCardTexture,
  repositoryPullReviewStatus,
  repositoryRecordCountTexture,
  repositoryRecordPageTexture,
  repositoryRecordPagerTexture,
  repositorySizeLabelSprite,
  repositorySizeMapSegments,
  repositoryWedgeGeometry,
} from "../repositories.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
}) {
  let repositoryIssueAgentPicker = null;

  const REPOSITORY_ISSUE_CARDS_VISIBLE = 25;
  const REPOSITORY_PULL_CARDS_VISIBLE = 25;
  const REPOSITORY_RECORDS_MAX = 250;


  function updateRepositorySizeMap(sizeTree = {}, selection = {}) {
    const district = ctx.landmarkObjects.get("repositories");
    const districtPortal = district?.userData?.portal;
    if (!districtPortal) return;
    const previousLayer = ctx.world.userData.repositorySizeLayer;
    const previousMount =
      ctx.world.userData.repositorySizeMount || previousLayer?.parent;
    removeGeneratedLayer(previousMount, previousLayer, ctx.interactive);
    ctx.world.userData.repositorySizeLayer = null;
    ctx.world.userData.repositorySizeMount = null;
    ctx.world.userData.repositorySizeLoadingTail = null;
    ctx.repositoryPortals.forEach((record) => {
      if (record.group.userData.repositoryFace) {
        record.group.userData.repositoryFace.visible = true;
      }
      if (record.group.userData.repositoryLabel) {
        record.group.userData.repositoryLabel.position.y = -1.42;
      }
      record.group.userData.repositorySizeLayer = null;
      record.group.userData.repositorySizeMeshes = [];
      record.group.userData.repositorySizeFocus = "";
    });
    if (district.userData.legacyFiles) {
      district.userData.legacyFiles.visible = ctx.repositoryPortals.size === 0;
    }
    if (districtPortal.userData.repositoryCore) {
      districtPortal.userData.repositoryCore.visible = true;
    }
    if (districtPortal.userData.repositoryEntityLayer) {
      districtPortal.userData.repositoryEntityLayer.visible =
        ctx.repositoryPortals.size === 0;
    }

    const owner = String(selection?.owner || "").slice(0, 40);
    const name = String(selection?.repo || selection?.name || "").slice(0, 60);
    const repositoryKey =
      `${owner.toLocaleLowerCase()}/${name.toLocaleLowerCase()}`;
    const mount = ctx.repositoryPortals.get(repositoryKey)?.group;
    const totalSize = Math.max(0, Number(sizeTree?.size) || 0);
    const children = Array.isArray(sizeTree?.children)
      ? sizeTree.children
      : [];
    if (!mount || !(totalSize > 0) || !children.length) return;

    const map = repositorySizeMapSegments(sizeTree, selection?.path || "");
    if (!map.segments.length) return;
    const layer = new THREE.Group();
    layer.name = "repository-3d-size-map";
    // Lift the sunburst clear of the portal base so its lowest wedges do not
    // intersect the ground plane.
    layer.position.y = 0.55;
    layer.position.z = 0.18;
    layer.userData.repositorySizeFocus = map.path;
    layer.userData.repositoryKey = repositoryKey;
    const innerRadius = 0.72;
    const outerRadius = 2.5;
    const ringWidth = (outerRadius - innerRadius) / map.ringCount;
    layer.userData.repositorySizeMap = {
      segments: map.segments,
      innerRadius,
      ringWidth,
      outerRadius,
      lift: 0.22,
      depthStep: 0.018,
    };
    const focusSize = Math.max(1, Number(map.node?.size) || totalSize);
    const sizeMeshes = [];
    let labelCount = 0;

    // A small comet-like tail lives in the gap between the hub (the current
    // folder) and the first directory ring. It is only visible while a folder
    // or file request is in flight, so the map itself stays readable.
    const loadingTail = new THREE.Group();
    loadingTail.name = "repository-size-map-loading-tail";
    [
      { offset: 0, arc: 0.22, opacity: 1 },
      { offset: -0.27, arc: 0.16, opacity: 0.7 },
      { offset: -0.49, arc: 0.1, opacity: 0.42 },
    ].forEach(({ offset, arc, opacity }) => {
      const trail = new THREE.Mesh(
        resources.primitive(THREE, "TorusGeometry", innerRadius + 0.12, 0.035, 7, 16, arc),
        resources.uniqueMaterial(THREE, {
          color: "#fff1ad",
          transparent: true,
          opacity,
          toneMapped: false,
        }, "MeshBasicMaterial"),
      );
      trail.rotation.z = offset;
      trail.position.z = 0.3;
      loadingTail.add(trail);
    });
    const spark = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.075, 12, 8),
      makeSharedMaterial(THREE, "#ffffff", {
        emissive: "#92f4ff",
        emissiveIntensity: 2.2,
      }),
    );
    spark.position.set(innerRadius + 0.12, 0, 0.31);
    loadingTail.add(spark);
    loadingTail.visible = Boolean(ctx.world.userData.repositorySizeLoading);
    layer.add(loadingTail);

    map.segments.forEach((segment, index) => {
      const radialGap = 0.025;
      const inner =
        innerRadius + (segment.depth - 1) * ringWidth + radialGap;
      const outer =
        innerRadius + segment.depth * ringWidth - radialGap;
      const extrusion =
        0.075 +
        Math.min(
          0.12,
          (Math.log1p(segment.size) / Math.log1p(focusSize)) * 0.1,
        );
      const geometry = repositoryWedgeGeometry(
        THREE,
        inner,
        outer,
        segment.from,
        segment.to,
        extrusion,
      );
      if (!geometry) return;
      const capMaterial = resources.uniqueMaterial(THREE, {
        color: segment.color,
        transparent: segment.type === "summary",
        opacity: segment.type === "summary" ? 0.58 : 0.98,
        side: THREE.DoubleSide,
        forceSinglePass: true,
        toneMapped: false,
      }, "MeshBasicMaterial");
      const sideColor = new THREE.Color(segment.color).multiplyScalar(0.55);
      const sideMaterial = makeMaterial(THREE, sideColor, {
        emissive: sideColor,
        emissiveIntensity: 0.14,
        metalness: 0.12,
        roughness: 0.44,
        transparent: segment.type === "summary",
        opacity: segment.type === "summary" ? 0.5 : 1,
        side: THREE.DoubleSide,
      });
      const mesh = new THREE.Mesh(geometry, [capMaterial, sideMaterial]);
      mesh.name = `repository-size-segment-${index}`;
      mesh.position.z = segment.depth * 0.018;
      mesh.userData.landmark = "repositories";
      mesh.userData.path = segment.path;
      mesh.userData.kind = segment.type;
      mesh.userData.repositorySizeNode = {
        owner: String(selection?.owner || "").slice(0, 40),
        name: String(selection?.repo || selection?.name || "").slice(0, 60),
        commit: String(selection?.commit || "").slice(0, 64),
        path: segment.path,
        label: segment.name,
        type: segment.type,
        size: segment.size,
        percent: Math.max(
          0,
          Math.min(100, (segment.size / focusSize) * 100),
        ),
      };
      layer.add(mesh);
      sizeMeshes.push(mesh);
      if (segment.type !== "summary") ctx.interactive.push(mesh);

      const span = segment.to - segment.from;
      if (
        labelCount < 18 &&
        segment.depth <= 3 &&
        span >= 0.34 &&
        segment.type !== "summary"
      ) {
        const middle = (segment.from + segment.to) * 0.5;
        const labelRadius = (inner + outer) * 0.5;
        const label = repositorySizeLabelSprite(
          THREE,
          segment.name.slice(0, 18),
          compactSceneBytes(segment.size),
          segment.color,
        );
        label.name = `repository-size-label-${index}`;
        label.scale.set(Math.min(1.55, 0.92 + span * 0.32), 0.42, 1);
        label.position.set(
          Math.cos(middle) * labelRadius,
          Math.sin(middle) * labelRadius,
          0.24 + segment.depth * 0.018,
        );
        layer.add(label);
        labelCount += 1;
      }
    });

    const hub = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.64, 0.64, 0.18, 48),
      makeSharedMaterial(THREE, "#0b2630", {
        emissive: "#1b6e87",
        emissiveIntensity: 0.58,
        metalness: 0.2,
        roughness: 0.36,
      }),
    );
    hub.name = "repository-size-map-hub";
    hub.rotation.x = Math.PI / 2;
    hub.position.z = 0.055;
    hub.userData.landmark = "repositories";
    const parentPath = map.path.split("/").slice(0, -1).join("/");
    hub.userData.repositorySizeNode = {
      owner: String(selection?.owner || "").slice(0, 40),
      name: String(selection?.repo || selection?.name || "").slice(0, 60),
      commit: String(selection?.commit || "").slice(0, 64),
      path: map.path,
      targetPath: parentPath,
      label: map.path.split("/").pop() || selection?.repo || "repository",
      type: "center",
      size: focusSize,
      percent: 100,
    };
    ctx.interactive.push(hub);
    layer.add(hub);

    const title = repositorySizeLabelSprite(
      THREE,
      String(
        map.path.split("/").pop() ||
          selection?.repo ||
          selection?.name ||
          "repository",
      ).slice(0, 18),
      compactSceneBytes(focusSize),
      "#9ef7c6",
    );
    title.name = "repository-size-map-title";
    title.scale.set(1.34, 0.45, 1);
    title.position.z = 0.26;
    layer.add(title);

    mount.add(layer);
    ctx.world.userData.repositorySizeLayer = layer;
    ctx.world.userData.repositorySizeMount = mount;
    ctx.world.userData.repositorySizeLoadingTail = loadingTail;
    mount.userData.repositorySizeLayer = layer;
    mount.userData.repositorySizeMeshes = sizeMeshes;
    mount.userData.repositorySizeFocus = map.path;
    if (mount.userData.repositoryFace) {
      mount.userData.repositoryFace.visible = false;
    }
    if (mount.userData.repositoryLabel) {
      mount.userData.repositoryLabel.position.y = -3.05;
    }
    if (district.userData.legacyFiles) {
      district.userData.legacyFiles.visible = false;
    }
    if (districtPortal.userData.repositoryEntityLayer) {
      districtPortal.userData.repositoryEntityLayer.visible = false;
    }
  }

  function setRepositorySizeLoading(loading = false) {
    ctx.world.userData.repositorySizeLoading = Boolean(loading);
    const tail = ctx.world.userData.repositorySizeLoadingTail;
    if (tail) tail.visible = Boolean(loading);
    const perimeter = ctx.world.userData.repositorySizeLayer?.getObjectByName(
      "repository-size-map-perimeter",
    );
    if (perimeter) perimeter.userData.loading = Boolean(loading);
  }

  function safeRecordNumber(value) {
    const number = Number(value);
    return Number.isSafeInteger(number) && number >= 1 && number <= 10_000_000
      ? number
      : 0;
  }

  function repositoryRecordStatusSummary(kind, items = []) {
    const counts = new Map();
    items.forEach((item) => {
      const status =
        kind === "pull"
          ? repositoryPullReviewStatus(item)
          : item?.state === "closed"
            ? "closed"
            : "open";
      counts.set(status, (counts.get(status) || 0) + 1);
    });
    const ranked = [...counts.entries()].sort(
      (left, right) =>
        right[1] - left[1] || left[0].localeCompare(right[0]),
    );
    const mostly = ranked[0]?.[0] || "unavailable";
    const detail = ranked
      .slice(0, 4)
      .map(([status, count]) => `${status.toUpperCase()} ${count}`)
      .join(" · ");
    return { mostly, detail };
  }

  function repositoryRecordPage(kind, count) {
    const perPage =
      kind === "issue"
        ? REPOSITORY_ISSUE_CARDS_VISIBLE
        : REPOSITORY_PULL_CARDS_VISIBLE;
    const pages = Math.max(1, Math.ceil(count / perPage));
    const requested = Number(
      ctx.world.userData.repositoryRecordPages?.[kind],
    );
    const page = clamp(
      Number.isSafeInteger(requested) ? requested : 0,
      0,
      pages - 1,
    );
    ctx.world.userData.repositoryRecordPages = {
      ...(ctx.world.userData.repositoryRecordPages || {}),
      [kind]: page,
    };
    return {
      page,
      pages,
      perPage,
      start: page * perPage,
      end: Math.min(count, (page + 1) * perPage),
    };
  }

  function changeRepositoryRecordPage(kind, direction) {
    const data = ctx.world.userData.repositoryRecordDeskData;
    if (!data || !["issue", "pull"].includes(kind)) return false;
    const items =
      kind === "issue"
        ? data.records?.issues
        : data.records?.pulls;
    const count = Array.isArray(items) ? items.length : 0;
    const current = repositoryRecordPage(kind, count);
    const next = clamp(
      current.page + Math.sign(Number(direction) || 0),
      0,
      current.pages - 1,
    );
    if (next === current.page) return false;
    ctx.world.userData.repositoryRecordPages = {
      ...(ctx.world.userData.repositoryRecordPages || {}),
      [kind]: next,
    };
    updateRepositoryRecordDesk(data.selection, data.records);
    return true;
  }

  // The open crate of issue pages and the pull-request review board that stand
  // beside the selected repository portal. Both are rebuilt from bounded,
  // commit-matched records the shell already verified; the scene never invents
  // an issue or pull number of its own.
  function updateRepositoryRecordDesk(selection = {}, records = {}) {
    const previousLayer = ctx.world.userData.repositoryRecordDeskLayer;
    const previousMount =
      ctx.world.userData.repositoryRecordDeskMount || previousLayer?.parent;
    removeGeneratedLayer(previousMount, previousLayer, ctx.interactive);
    ctx.world.userData.repositoryRecordDeskLayer = null;
    ctx.world.userData.repositoryRecordDeskMount = null;
    ctx.world.userData.repositoryRecordDeskData = null;

    // Repository issues and pull requests belong in their focused web
    // workbenches. Keeping these tall in-world columns made the repository
    // district read like two data towers and obscured the portal circle and
    // the new frame. This compatibility entry point deliberately only clears
    // a previously rendered desk while callers migrate through the shared
    // repository scene refresh path.
    void selection;
    void records;
    return;

    const owner = String(selection?.owner || "").slice(0, 40);
    const name = String(selection?.repo || selection?.name || "").slice(0, 60);
    const repositoryKey =
      `${owner.toLocaleLowerCase()}/${name.toLocaleLowerCase()}`;
    const mount = ctx.repositoryPortals.get(repositoryKey)?.group;
    if (ctx.world.userData.repositoryRecordPageKey !== repositoryKey) {
      ctx.world.userData.repositoryRecordPageKey = repositoryKey;
      ctx.world.userData.repositoryRecordPages = {
        issue: 0,
        pull: 0,
      };
      repositoryIssueAgentPicker = null;
    }
    const issues = (Array.isArray(records?.issues) ? records.issues : [])
      .map((record) => ({
        number: safeRecordNumber(record?.number),
        state: ["open", "closed"].includes(record?.state) ? record.state : "",
        title: String(record?.title || "").slice(0, 240),
        author: String(record?.author || "").slice(0, 100),
        createdAt: Math.max(0, Number(record?.createdAt) || 0),
        updatedAt: Math.max(0, Number(record?.updatedAt) || 0),
        labels: (Array.isArray(record?.labels) ? record.labels : [])
          .map((label) => String(label || "").slice(0, 40))
          .filter(Boolean)
          .slice(0, 6),
        assignees: (
          Array.isArray(record?.assignees) ? record.assignees : []
        )
          .map((assignee) => String(assignee || "").slice(0, 60))
          .filter(Boolean)
          .slice(0, 4),
        metadataAvailable: record?.metadataAvailable !== false,
      }))
      .filter((record) => record.number)
      .sort(
        (left, right) =>
          (right.updatedAt || right.createdAt || right.number) -
          (left.updatedAt || left.createdAt || left.number),
      )
      .slice(0, REPOSITORY_RECORDS_MAX);
    const pulls = (Array.isArray(records?.pulls) ? records.pulls : [])
      .map((record) => ({
        number: safeRecordNumber(record?.number),
        state: ["open", "closed", "merged"].includes(record?.state)
          ? record.state
          : "",
        title: String(record?.title || "").slice(0, 80),
        author: String(record?.author || "").slice(0, 100),
        base: String(record?.base || "").slice(0, 160),
        head: String(record?.head || "").slice(0, 160),
        createdAt: Math.max(0, Number(record?.createdAt) || 0),
        metadataAvailable: record?.metadataAvailable !== false,
        draft: record?.draft === true,
        creationBaseOid: String(record?.creationBaseOid || "").slice(0, 64),
        creationHeadOid: String(record?.creationHeadOid || "").slice(0, 64),
        mergeable:
          typeof record?.mergeable === "boolean"
            ? record.mergeable
            : undefined,
        hasConflicts:
          typeof record?.hasConflicts === "boolean"
            ? record.hasConflicts
            : typeof record?.conflict === "boolean"
              ? record.conflict
              : undefined,
        reviewStatus: String(record?.reviewStatus || "").slice(0, 24),
        mergeStatus: String(record?.mergeStatus || "").slice(0, 24),
        mergeability: String(record?.mergeability || "").slice(0, 24),
        checksStatus: String(
          record?.checksStatus ||
            record?.checkStatus ||
            record?.ciStatus ||
            "",
        ).slice(0, 24),
      }))
      .filter((record) => record.number)
      .sort(
        (left, right) =>
          (right.createdAt || right.number) -
          (left.createdAt || left.number),
      )
      .slice(0, REPOSITORY_RECORDS_MAX);
    if (!mount || (!issues.length && !pulls.length)) {
      return;
    }

    const expandedIssue = safeRecordNumber(records?.expandedIssue);
    const layer = new THREE.Group();
    layer.name = "repository-record-desk";
    const repositoryName = `${owner}/${name}`;
    // Ground level beside the portal plinth; the plinth base sits at -2.38 and
    // the follower gallery stands on -2.53.
    const groundY = -2.53;

    const addRecordBoard = (allItems, kind, x, accent) => {
      if (!allItems.length) return null;
      const desk = new THREE.Group();
      const pageInfo = repositoryRecordPage(kind, allItems.length);
      // Page zero contains the newest records. Every page keeps its records
      // in newest-first data order, then maps them into their absolute tower
      // slots so the newest entry is physically lowest and older pages climb.
      const items = allItems
        .slice(pageInfo.start, pageInfo.end);
      desk.name = `repository-${kind}-desk:${repositoryKey}`;
      desk.position.set(x, 0, 2.05);
      // A single reading column keeps every record and its controls aligned.
      // Tower height represents the complete bounded record collection, not
      // merely the current 25-record page. The lightweight slot separators
      // keep that truthful silhouette to one additional instanced draw call.
      const rows = Math.max(1, allItems.length);
      const rowPitch = 0.52;
      const boardHeight = 0.74 + rows * rowPitch;
      const boardBottomY = groundY + 2.62;
      const boardWidth = 4.05;
      const board = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", boardWidth, boardHeight, 0.12),
        makeMaterial(THREE, kind === "issue" ? "#10231b" : "#171429", {
          emissive: kind === "issue" ? "#163c2a" : "#241d45",
          emissiveIntensity: 0.32,
          roughness: 0.6,
        }),
      );
      board.position.set(0, boardBottomY + boardHeight / 2, 0);
      board.userData.landmark = "repositories";
      board.userData.repositoryRecordPageKind = kind;
      desk.add(board);
      ctx.interactive.push(board);
      const slotGeometry = resources.primitive(
        THREE,
        "BoxGeometry",
        boardWidth - 0.16,
        0.014,
        0.018,
      );
      const slotMaterial = makeMaterial(
        THREE,
        kind === "issue" ? "#426d59" : "#554d78",
        {
          emissive: kind === "issue" ? "#214b38" : "#302959",
          emissiveIntensity: 0.24,
          roughness: 0.8,
        },
      );
      const slots = new THREE.InstancedMesh(
        slotGeometry,
        slotMaterial,
        rows,
      );
      slots.name = `repository-${kind}-tower-slots:${repositoryKey}`;
      const slotTransform = new THREE.Object3D();
      for (let slot = 0; slot < rows; slot += 1) {
        slotTransform.position.set(
          0,
          boardBottomY + 0.22 + slot * rowPitch,
          0.071,
        );
        slotTransform.updateMatrix();
        slots.setMatrixAt(slot, slotTransform.matrix);
      }
      slots.instanceMatrix.needsUpdate = true;
      desk.add(slots);
      [-boardWidth / 2 + 0.28, boardWidth / 2 - 0.28].forEach((legX) => {
        const legHeight = boardBottomY - groundY;
        const leg = new THREE.Mesh(
          resources.primitive(THREE, "BoxGeometry", 0.14, legHeight, 0.14),
          makeSharedMaterial(THREE, "#0e0c1c", { roughness: 0.7 }),
        );
        leg.position.set(legX, groundY + legHeight / 2, 0);
        desk.add(leg);
      });
      items.forEach((record, index) => {
        const isIssue = kind === "issue";
        const towerSlot = pageInfo.start + index;
        const cardX = 0;
        const cardY =
          boardBottomY + 0.48 + towerSlot * rowPitch;
        const card = new THREE.Mesh(
          resources.primitive(THREE, "PlaneGeometry", 3.78, 0.46),
          resources.uniqueMaterial(THREE, {
            map: isIssue
              ? repositoryIssueCardTexture(THREE, record)
              : repositoryPullCardTexture(THREE, record),
            toneMapped: false,
          }, "MeshBasicMaterial"),
        );
        card.name = `repository-${kind}-card:${record.number}`;
        card.position.set(cardX, cardY, 0.08);
        card.userData.landmark = "repositories";
        card.userData.repositoryRecordPageKind = kind;
        const page = {
          owner,
          name,
          number: record.number,
          state: record.state,
        };
        if (isIssue) card.userData.repositoryIssuePage = page;
        else card.userData.repositoryPullPage = page;
        desk.add(card);
        ctx.interactive.push(card);
        if (isIssue && record.state === "open" && ctx.agentBotAccessAllowed) {
          const selectedProvider =
            repositoryIssueAgentPicker?.repositoryKey === repositoryKey &&
            repositoryIssueAgentPicker?.number === record.number
              ? repositoryIssueAgentPicker.provider
              : "";
          [
            { provider: "claude-code", x: 1.14 },
            { provider: "codex", x: 1.47 },
          ].forEach(({ provider, x }) => {
            const providerButton = new THREE.Mesh(
              resources.primitive(THREE, "PlaneGeometry", 0.28, 0.28),
              resources.uniqueMaterial(THREE, {
                map: repositoryIssueAgentProviderTexture(
                  THREE,
                  provider,
                  selectedProvider === provider,
                ),
                transparent: true,
                depthTest: false,
                depthWrite: false,
                toneMapped: false,
              }, "MeshBasicMaterial"),
            );
            providerButton.name =
              `repository-issue-agent-provider:${record.number}:${provider}`;
            providerButton.position.set(cardX + x, cardY + 0.1, 0.18);
            providerButton.renderOrder = 45;
            providerButton.userData.landmark = "repositories";
            providerButton.userData.repositoryIssueAgentProvider = {
              owner,
              name,
              number: record.number,
              title: record.title,
              provider,
            };
            desk.add(providerButton);
            ctx.interactive.push(providerButton);
          });
          if (selectedProvider) {
            const models =
              selectedProvider === "claude-code"
                ? ["haiku", "sonnet", "opus", "fable"]
                : ["sol", "luna", "terra"];
            const chipWidth = selectedProvider === "claude-code" ? 0.29 : 0.38;
            const chipGap = 0.02;
            const totalWidth =
              models.length * chipWidth + (models.length - 1) * chipGap;
            models.forEach((model, modelIndex) => {
              const modelButton = new THREE.Mesh(
                resources.primitive(THREE, "PlaneGeometry", chipWidth, 0.15),
                resources.uniqueMaterial(THREE, {
                  map: repositoryIssueAgentModelTexture(
                    THREE,
                    model,
                    selectedProvider,
                  ),
                  transparent: true,
                  depthTest: false,
                  depthWrite: false,
                  toneMapped: false,
                }, "MeshBasicMaterial"),
              );
              modelButton.name =
                `repository-issue-agent-model:${record.number}:${selectedProvider}:${model}`;
              modelButton.position.set(
                cardX +
                  0.84 +
                  modelIndex * (chipWidth + chipGap) -
                  totalWidth / 2 +
                  chipWidth / 2,
                cardY - 0.14,
                0.2,
              );
              modelButton.renderOrder = 46;
              modelButton.userData.landmark = "repositories";
              modelButton.userData.repositoryIssueAgentAssignment = {
                owner,
                name,
                number: record.number,
                title: record.title,
                provider: selectedProvider,
                model,
              };
              desk.add(modelButton);
              ctx.interactive.push(modelButton);
            });
          }
        }
      });
      const openCount = allItems.filter(
        (item) => String(item?.state || "").toLowerCase() === "open",
      ).length;
      const statusSummary = repositoryRecordStatusSummary(kind, allItems);
      const countHeader = new THREE.Mesh(
        resources.primitive(THREE, "PlaneGeometry", 4.25, 1.5),
        resources.uniqueMaterial(THREE, {
          map: repositoryRecordCountTexture(
            THREE,
            kind,
            openCount,
            kind === "issue"
              ? "OPEN RECORDS ONLY"
              : statusSummary.detail || "LIVE REVIEW STATUS",
            accent,
          ),
          transparent: true,
          toneMapped: false,
        }, "MeshBasicMaterial"),
      );
      countHeader.name = `repository-${kind}-open-count:${repositoryKey}`;
      countHeader.position.set(
        0,
        groundY + 1.75,
        0.2,
      );
      desk.add(countHeader);

      const addPagerButton = (direction, label, buttonX, enabled) => {
        const button = new THREE.Mesh(
          resources.primitive(THREE, "PlaneGeometry", 1.15, 0.48),
          resources.uniqueMaterial(THREE, {
            map: repositoryRecordPagerTexture(
              THREE,
              label,
              enabled ? "SCROLL" : "END",
              enabled ? accent : "#59655f",
            ),
            transparent: true,
            toneMapped: false,
          }, "MeshBasicMaterial"),
        );
        button.name =
          `repository-${kind}-page-${direction < 0 ? "prev" : "next"}`;
        button.position.set(
          buttonX,
          groundY + 0.7,
          0.2,
        );
        button.userData.landmark = "repositories";
        button.userData.repositoryRecordPageKind = kind;
        button.userData.repositoryRecordPage = {
          kind,
          direction,
          enabled,
        };
        desk.add(button);
        if (enabled) ctx.interactive.push(button);
      };
      addPagerButton(
        -1,
        "◀ PREV",
        -1.46,
        pageInfo.page > 0,
      );
      addPagerButton(
        1,
        "NEXT ▶",
        1.46,
        pageInfo.page + 1 < pageInfo.pages,
      );
      const pageReadout = new THREE.Mesh(
        resources.primitive(THREE, "PlaneGeometry", 1.72, 0.48),
        resources.uniqueMaterial(THREE, {
          map: repositoryRecordPageTexture(THREE, pageInfo, accent),
          transparent: true,
          toneMapped: false,
        }, "MeshBasicMaterial"),
      );
      pageReadout.name = `repository-${kind}-page-readout:${repositoryKey}`;
      pageReadout.position.set(0, groundY + 0.7, 0.2);
      desk.add(pageReadout);

      layer.add(desk);
      return { desk, board, boardHeight, items };
    };

    // Keep review and issue work visually and operationally independent:
    // pull requests page on the left, issues page on the right.
    const pullBoard = addRecordBoard(
      pulls,
      "pull",
      -7.75,
      "#d5b6ff",
    );
    const issueBoard = addRecordBoard(
      issues,
      "issue",
      7.75,
      "#9ef7c6",
    );

    if (
      issueBoard &&
      expandedIssue &&
      issueBoard.items.some(
        (issue) => issue.number === expandedIssue,
      )
    ) {
      const issue = issueBoard.items.find(
        (candidate) => candidate.number === expandedIssue,
      );
      const sheet = new THREE.Mesh(
        resources.primitive(THREE, "PlaneGeometry", 3.1, 4.08),
        resources.uniqueMaterial(THREE, {
          map: repositoryIssuePageTexture(
            THREE,
            issue,
            true,
            repositoryName,
          ),
          side: THREE.DoubleSide,
          forceSinglePass: true,
          toneMapped: false,
        }, "MeshBasicMaterial"),
      );
      sheet.name = `repository-issue-page-expanded:${issue.number}`;
      sheet.position.set(
        0,
        groundY + 0.8 + issueBoard.boardHeight / 2,
        0.34,
      );
      sheet.userData.landmark = "repositories";
      sheet.userData.repositoryIssuePage = {
        owner,
        name,
        number: issue.number,
        state: issue.state,
        expanded: true,
      };
      issueBoard.desk.add(sheet);
      ctx.interactive.push(sheet);
    }

    mount.add(layer);
    ctx.world.userData.repositoryRecordDeskLayer = layer;
    ctx.world.userData.repositoryRecordDeskMount = mount;
    ctx.world.userData.repositoryRecordDeskData = { selection, records };
  }

  function setRepositoryIssuePageExpanded(number = 0) {
    const data = ctx.world.userData.repositoryRecordDeskData;
    if (!data) return;
    updateRepositoryRecordDesk(data.selection, {
      ...data.records,
      expandedIssue: safeRecordNumber(number),
    });
  }

  function setRepositoryIssueAgentPicker(picker = null) {
    if (!ctx.agentBotAccessAllowed) return false;
    const data = ctx.world.userData.repositoryRecordDeskData;
    if (!data) return false;
    const number = safeRecordNumber(picker?.number);
    const provider = ["claude-code", "codex"].includes(picker?.provider)
      ? picker.provider
      : "";
    const repositoryKey = `${String(picker?.owner || "").toLowerCase()}/${String(
      picker?.name || "",
    ).toLowerCase()}`;
    repositoryIssueAgentPicker =
      number && provider
        ? {
            repositoryKey,
            number,
            provider,
          }
        : null;
    updateRepositoryRecordDesk(data.selection, data.records);
    return true;
  }

  return {
    updateRepositorySizeMap,
    setRepositorySizeLoading,
    safeRecordNumber,
    changeRepositoryRecordPage,
    updateRepositoryRecordDesk,
    setRepositoryIssuePageExpanded,
    setRepositoryIssueAgentPicker,
  };
}
