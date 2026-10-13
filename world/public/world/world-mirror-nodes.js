const COMMIT_PATTERN = /^[0-9a-f]{40,64}$/;

function text(value, fallback = "", limit = 80) {
  const cleaned = String(value ?? "")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim();
  return (cleaned || fallback).slice(0, limit);
}

function knownInteger(value, maximum = Number.MAX_SAFE_INTEGER) {
  if (value === null || value === undefined || value === "") return null;
  const number = Number(value);
  if (!Number.isSafeInteger(number) || number < 0) return null;
  return Math.min(number, maximum);
}

function knownNumber(value, minimum, maximum) {
  if (value === null || value === undefined || value === "") return null;
  const number = Number(value);
  if (!Number.isFinite(number) || number < minimum || number > maximum) {
    return null;
  }
  return number;
}

function omitUnknownValues(record) {
  return Object.fromEntries(
    Object.entries(record).filter(
      ([, value]) => value !== null && value !== undefined,
    ),
  );
}

function commitHash(value) {
  const commit = String(value || "").trim().toLowerCase();
  return COMMIT_PATTERN.test(commit) ? commit : "";
}

function timestamp(value) {
  const number = Number(value);
  if (Number.isSafeInteger(number) && number > 0) return number;
  const parsed = Date.parse(String(value || ""));
  return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
}

// Bounded client classes a node may report for the clone / website read it
// last served. Anything else (including a raw User-Agent a tampered record
// might carry) stays unknown rather than being echoed into the cabinet.
const SERVE_AGENT_CLASSES = new Set([
  "forkmesh-node",
  "git-client",
  "bot-tool",
  "browser",
  "client",
]);

function serveAgent(value) {
  const agent = text(value, "", 16).toLowerCase();
  return SERVE_AGENT_CLASSES.has(agent) ? agent : null;
}

function serveMetadata(source) {
  return omitUnknownValues({
    cloneServedAt: timestamp(source?.cloneServedAt),
    cloneServedAgent: serveAgent(source?.cloneServedAgent),
    websiteServedAt: timestamp(source?.websiteServedAt),
    websiteServedAgent: serveAgent(source?.websiteServedAgent),
  });
}

function commitMetadata(source) {
  const commit = source?.lastCommit || source?.commitDetails || {};
  const message = text(
    source?.lastCommitMessage ||
      source?.commitMessage ||
      source?.commitSubject ||
      commit?.message ||
      commit?.subject,
    "",
    220,
  );
  const authorName = text(
    source?.lastCommitAuthorName ||
      source?.commitAuthorName ||
      source?.authorName ||
      commit?.authorName ||
      commit?.author?.name ||
      source?.commitAuthor,
    "",
    100,
  );
  const committedAt = timestamp(
    source?.lastCommitAt ||
      source?.commitDate ||
      source?.committedAt ||
      source?.commitTimestamp ||
      commit?.committedAt ||
      commit?.date ||
      commit?.author?.date,
  );
  return omitUnknownValues({
    lastCommitMessage: message || null,
    lastCommitAuthorName: authorName || null,
    lastCommitAt: committedAt,
  });
}

function changedFiles(source) {
  return (Array.isArray(source?.changedFiles) ? source.changedFiles : [])
    .map((value) => text(value, "", 160).replace(/\\/g, "/"))
    .filter(
      (path, index, paths) =>
        path &&
        !path.startsWith("/") &&
        path !== ".." &&
        !path.startsWith("../") &&
        paths.indexOf(path) === index,
    )
    .slice(0, 8);
}

function repositoryRecency(record) {
  return Math.max(
    0,
    Number(record?.lastSync) || 0,
    Number(record?.updatedAt) || 0,
    Number(record?.lastCommitAt) || 0,
    Number(record?.lastSeen) || 0,
  );
}

function publicRepositoryRecord(mirror, payload) {
  const checkedAt =
    Number(mirror?.checkedAt) > 0 ? timestamp(mirror.checkedAt) : null;
  return omitUnknownValues({
    owner: text(payload?.requestedOwner || payload?.owner, "", 80),
    name: text(
      payload?.requestedRepo || payload?.repo || mirror?.repo,
      "repository",
      120,
    ),
    commit: commitHash(mirror?.commit),
    branch: text(mirror?.branch, "", 120),
    status: text(mirror?.status, "unknown", 24).toLowerCase(),
    integrity: text(mirror?.integrity, "unknown", 24).toLowerCase(),
    activity: text(mirror?.activity, "unknown", 32).toLowerCase(),
    activityUpdatedAt: timestamp(mirror?.activityUpdatedAt),
    cloneAvailable: mirror?.cloneAvailable === true,
    endpointHealthy: mirror?.endpointHealthy === true,
    endpointFresh: mirror?.endpointFresh === true,
    endpointIntegrity: text(
      mirror?.endpointIntegrity,
      "unknown",
      24,
    ).toLowerCase(),
    checkedAt,
    latencyMs:
      checkedAt === null ? null : knownInteger(mirror?.latencyMs, 60_000),
    endpoint: text(mirror?.endpoint, "", 240) || null,
    region: text(mirror?.region, "", 40).toLowerCase() || null,
    ownerUser: text(mirror?.ownerUser, "", 80) || null,
    abuseBlocked: mirror?.abuseBlocked === true,
    operations: Array.isArray(mirror?.operations)
      ? mirror.operations
          .map((value) => text(value, "", 40))
          .filter(Boolean)
          .slice(0, 32)
      : [],
    behind: mirror?.behind === true,
    lastSeen: timestamp(mirror?.lastSeen),
    lastSync: timestamp(mirror?.lastSync),
    updatedAt: timestamp(mirror?.updatedAt),
    syncAgeMs: knownInteger(mirror?.syncAgeMs),
    hostedSince: timestamp(mirror?.hostedSince),
    sizeBytes: knownInteger(mirror?.sizeBytes, 2 ** 50),
    issueCount: knownInteger(mirror?.issueCount, 1_000_000_000),
    commitCount: knownInteger(mirror?.commitCount, 1_000_000_000),
    branchCount: knownInteger(mirror?.branchCount, 1_000_000_000),
    pullCount: knownInteger(mirror?.pullCount, 1_000_000_000),
    discussionCount: knownInteger(mirror?.discussionCount, 1_000_000_000),
    worktreeCount: knownInteger(mirror?.worktreeCount, 1_000_000_000),
    artifactCount: knownInteger(mirror?.artifactCount, 1_000_000_000),
    clonesServed: knownInteger(mirror?.clonesServed, 1_000_000_000),
    websiteServed: knownInteger(mirror?.websiteServed, 1_000_000_000),
    platform: text(mirror?.platform, "", 24).toLowerCase(),
    version: text(mirror?.version, "", 32),
    id: text(mirror?.id, "", 120),
    machineName: text(mirror?.machineName, "", 63) || null,
    ...commitMetadata(mirror),
    changedFiles: changedFiles(mirror),
    ...serveMetadata(mirror),
  });
}

function nodeAggregateRecord(node) {
  return omitUnknownValues({
    repositoryCount: knownInteger(
      node?.repositoryCount ?? node?.repos,
      1_000_000,
    ),
    sizeBytes: knownInteger(node?.sizeBytes, 2 ** 50),
    issueCount: knownInteger(node?.issueCount, 1_000_000_000),
    commitCount: knownInteger(node?.commitCount, 1_000_000_000),
    branchCount: knownInteger(node?.branchCount, 1_000_000_000),
    pullCount: knownInteger(node?.pullCount, 1_000_000_000),
    discussionCount: knownInteger(node?.discussionCount, 1_000_000_000),
    worktreeCount: knownInteger(node?.worktreeCount, 1_000_000_000),
    artifactCount: knownInteger(node?.artifactCount, 1_000_000_000),
    clonesServed: knownInteger(node?.clonesServed, 1_000_000_000),
    websiteServed: knownInteger(node?.websiteServed, 1_000_000_000),
    commit: commitHash(node?.commit),
    branch: text(node?.branch, "", 120),
    lastSync: timestamp(node?.lastSync),
    updatedAt: timestamp(node?.updatedAt),
    platform: text(node?.platform, "", 24).toLowerCase(),
    version: text(node?.version, "", 32),
    nodeId: text(node?.nodeId || node?.id, "", 120),
    machineName: text(node?.machineName, "", 63) || null,
    ...commitMetadata(node),
    ...serveMetadata(node),
  });
}

function publicResourceRecord(record) {
  const memoryTotalBytes = knownInteger(
    record?.memTotalBytes ?? record?.memoryTotalBytes,
    2 ** 50,
  );
  const memoryUsedBytes = knownInteger(
    record?.memUsedBytes ?? record?.memoryUsedBytes,
    2 ** 50,
  );
  const diskTotalBytes = knownInteger(record?.diskTotalBytes, 2 ** 50);
  const diskUsedBytes = knownInteger(record?.diskUsedBytes, 2 ** 50);
  return omitUnknownValues({
    cpuPercent: knownNumber(record?.cpuPercent, 0, 100),
    memoryUsedBytes:
      memoryTotalBytes !== null &&
      memoryUsedBytes !== null &&
      memoryUsedBytes <= memoryTotalBytes
        ? memoryUsedBytes
        : null,
    memoryTotalBytes,
    diskUsedBytes:
      diskTotalBytes !== null &&
      diskUsedBytes !== null &&
      diskUsedBytes <= diskTotalBytes
        ? diskUsedBytes
        : null,
    diskTotalBytes,
  });
}

/**
 * Build the bounded, truthful records consumed by the 3D server cabinets.
 *
 * Cabinet membership comes only from records in the public signed mirror
 * payload. Offline, healing, and integrity-rejected records remain visible as
 * clearly non-serving cabinets; otherwise a temporary route failure makes a
 * real mirror appear to have never existed. The network overview contains
 * repository-owner aggregates as well as machines, so it may enrich a matching
 * cabinet but must never create one. Unknown and opted-out metrics remain null.
 */
export function buildLiveMirrorNodes(network, mirrorPayloads = []) {
  const details = Array.isArray(network?.leaderboards?.nodes)
    ? network.leaderboards.nodes
    : Array.isArray(network?.nodes)
      ? network.nodes
      : [];
  const detailByName = new Map();
  details.slice(0, 100).forEach((node) => {
    const name = text(node?.name || node?.label, "", 80);
    if (name) detailByName.set(name.toLowerCase(), node);
  });

  const payloadList = Array.isArray(mirrorPayloads)
    ? mirrorPayloads
    : [mirrorPayloads];
  const mirrorNodes = new Map();
  const repositoriesByNode = new Map();
  const mirrorByNode = new Map();
  payloadList.slice(0, 100).forEach((payload) => {
    if (!Array.isArray(payload?.mirrors)) return;
    payload.mirrors.slice(0, 100).forEach((mirror) => {
      // `node` is the canonical machine identity. `owner` can be a user or
      // repository account and is intentionally only a legacy fallback.
      const name = text(mirror?.node || mirror?.machineName, "", 80);
      if (!name) return;
      const key = name.toLowerCase();
      mirrorNodes.set(key, name);
      const previous = mirrorByNode.get(key);
      if (
        !previous ||
        Number(String(mirror?.status || "").toLowerCase() === "online") >
          Number(String(previous?.status || "").toLowerCase() === "online") ||
        (
          String(mirror?.status || "").toLowerCase() ===
            String(previous?.status || "").toLowerCase() &&
          repositoryRecency(mirror) > repositoryRecency(previous)
        )
      ) {
        mirrorByNode.set(key, mirror);
      }
      const items = repositoriesByNode.get(key) || [];
      items.push(publicRepositoryRecord(mirror, payload));
      repositoriesByNode.set(key, items);
    });
  });

  const nodes = [...mirrorNodes.entries()]
    .map(([key, displayName]) => {
      const detail = detailByName.get(key) || {};
      const repositories = (repositoriesByNode.get(key) || [])
        .sort((left, right) =>
          Number(right.status === "online") -
            Number(left.status === "online") ||
          repositoryRecency(right) - repositoryRecency(left) ||
          `${left.owner}/${left.name}`.localeCompare(
            `${right.owner}/${right.name}`,
          ),
        )
        .slice(0, 32);
      const primary = repositories[0] || {};
      const hasPrimary = repositories.length > 0;
      const now = Date.now();
      // The cabinet represents the physical node, not just this repository's
      // clone route. A recent generic signed endpoint challenge proves that
      // mirror2 is alive even while an exact refs pin is converging; the
      // repository record still keeps cloneAvailable=false and its integrity
      // verdict so the detail panel never implies that blocked bytes are
      // currently routable.
      const online = repositories.some(
        (repository) =>
          repository.status === "online" ||
          (
            repository.endpointHealthy === true &&
            Number(repository.checkedAt) > 0 &&
            now - Number(repository.checkedAt) <= 10 * 60 * 1000
          ),
      );
      const healthy = repositories.some(
        (repository) =>
          (
            repository.status === "online" ||
            (
              repository.endpointHealthy === true &&
              Number(repository.checkedAt) > 0 &&
              now - Number(repository.checkedAt) <= 10 * 60 * 1000
            )
          ) &&
          repository.integrity === "ok",
      );
      const resources = publicResourceRecord({
        ...detail,
        ...(mirrorByNode.get(key) || {}),
      });
      const aggregate = nodeAggregateRecord(detail);
      return omitUnknownValues({
        name: displayName,
        // The machine's advertised node name — the display label for the
        // cabinet. The account (`name`) stays the identity/grouping key.
        machineName: primary.machineName || aggregate.machineName || null,
        ownerUser: primary.ownerUser || null,
        online,
        healthy,
        status: online ? "online" : primary.status || "offline",
        integrity: primary.integrity || "unknown",
        activity: primary.activity || "unknown",
        activityUpdatedAt: primary.activityUpdatedAt || null,
        endpoint: primary.endpoint || null,
        checkedAt: primary.checkedAt || null,
        latencyMs: primary.latencyMs ?? null,
        region: primary.region || null,
        endpointHealthy: primary.endpointHealthy === true,
        endpointIntegrity: primary.endpointIntegrity || "unknown",
        endpointFresh: primary.endpointFresh === true,
        abuseBlocked: primary.abuseBlocked === true,
        operations: Array.isArray(primary.operations)
          ? primary.operations.slice(0, 32)
          : [],
        cloneAvailable: repositories.some(
          (repository) =>
            repository.status === "online" &&
            repository.cloneAvailable === true &&
            repository.integrity === "ok",
        ),
        behind: primary.behind === true,
        platform: text(primary.platform || detail?.platform, "", 24).toLowerCase(),
        version: text(primary.version || detail?.version, "", 32),
        nodeId: text(primary.id || detail?.nodeId, "", 120),
        commit: hasPrimary ? primary.commit : aggregate.commit,
        branch: hasPrimary ? primary.branch : aggregate.branch,
        lastCommitMessage: hasPrimary
          ? primary.lastCommitMessage
          : aggregate.lastCommitMessage,
        lastCommitAuthorName: hasPrimary
          ? primary.lastCommitAuthorName
          : aggregate.lastCommitAuthorName,
        lastCommitAt: hasPrimary
          ? primary.lastCommitAt
          : aggregate.lastCommitAt,
        lastSeen: primary.lastSeen || null,
        lastSync: hasPrimary ? primary.lastSync : aggregate.lastSync,
        updatedAt: hasPrimary ? primary.updatedAt : aggregate.updatedAt,
        hostedSince: primary.hostedSince || null,
        syncAgeMs: primary.syncAgeMs ?? null,
        repositoryCount: aggregate.repositoryCount ?? repositories.length,
        sizeBytes: hasPrimary ? primary.sizeBytes : aggregate.sizeBytes,
        issueCount: hasPrimary ? primary.issueCount : aggregate.issueCount,
        commitCount: hasPrimary ? primary.commitCount : aggregate.commitCount,
        branchCount: hasPrimary ? primary.branchCount : aggregate.branchCount,
        pullCount: hasPrimary ? primary.pullCount : aggregate.pullCount,
        discussionCount: hasPrimary
          ? primary.discussionCount
          : aggregate.discussionCount,
        worktreeCount: hasPrimary
          ? primary.worktreeCount
          : aggregate.worktreeCount,
        artifactCount: hasPrimary
          ? primary.artifactCount
          : aggregate.artifactCount,
        clonesServed: hasPrimary
          ? primary.clonesServed
          : aggregate.clonesServed,
        websiteServed: hasPrimary
          ? primary.websiteServed
          : aggregate.websiteServed,
        // Paired with the two counters above: the stamp has to describe the
        // same repository the count does, so it follows the same primary /
        // aggregate choice rather than mixing sources.
        cloneServedAt: hasPrimary
          ? (primary.cloneServedAt ?? null)
          : (aggregate.cloneServedAt ?? null),
        cloneServedAgent: hasPrimary
          ? (primary.cloneServedAgent ?? null)
          : (aggregate.cloneServedAgent ?? null),
        websiteServedAt: hasPrimary
          ? (primary.websiteServedAt ?? null)
          : (aggregate.websiteServedAt ?? null),
        websiteServedAgent: hasPrimary
          ? (primary.websiteServedAgent ?? null)
          : (aggregate.websiteServedAgent ?? null),
        repositories,
        ...resources,
      });
    });
  // The freshest mirror is placed first in the cabinet yard. This also keeps a
  // multi-repository node's displayed commit tied to its newest reported state,
  // rather than whichever repository happens to sort first by name.
  return nodes
    .sort((left, right) =>
      Number(right.online === true) - Number(left.online === true) ||
      repositoryRecency(right) - repositoryRecency(left) ||
      left.name.localeCompare(right.name),
    )
    .slice(0, 64);
}
