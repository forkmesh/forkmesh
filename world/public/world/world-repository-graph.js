const COMMIT_RE = /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/;

function cleanText(value, fallback, limit) {
  const text = String(value ?? "")
    .replace(/[\u0000-\u001f\u007f]/g, "")
    .replace(/[<>"'`]/g, "")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, limit);
  return text || fallback;
}

function safeCount(value) {
  const number = Number(value);
  return Number.isSafeInteger(number) && number > 0
    ? Math.min(number, 10_000_000)
    : 0;
}

function safeAvatarURL(value) {
  try {
    const url = new URL(String(value || ""));
    if (
      url.protocol !== "https:" ||
      url.username ||
      url.password ||
      String(value || "").length > 500
    ) {
      return "";
    }
    return url.href;
  } catch (_) {
    return "";
  }
}

function safeRecord(record, kind) {
  const number = Number(record?.number);
  if (!Number.isSafeInteger(number) || number < 1 || number > 10_000_000) {
    return null;
  }
  const path = String(record?.path || "")
    .replaceAll("\\", "/")
    .replace(/^\/+/, "")
    .slice(0, 300);
  if (!path || path.split("/").includes("..")) return null;
  const state = ["open", "closed", "merged"].includes(record?.state)
    ? record.state
    : "";
  return {
    id: `${kind}:${number}`,
    kind,
    label: `${kind === "issue" ? "Issue" : "PR"} #${number}`,
    detail: state ? `${state} record at the selected commit` : "record at the selected commit",
    number,
    path,
    targetPaths: [path],
  };
}

/**
 * Build bounded, commit-matched graph entities from an authorized repository
 * snapshot. The function never invents individual issue/PR records: when the
 * commit-pinned record directory was unavailable it emits an explicitly
 * aggregate collection node using only the returned count.
 */
export function buildRepositoryGraphEntities(active = {}) {
  const owner = cleanText(active.owner, "", 40);
  const repo = cleanText(active.repo, "", 60);
  const commit = String(active.commit || "").trim().toLowerCase();
  if (!owner || !repo || !COMMIT_RE.test(commit)) return [];
  const entries = Array.isArray(active.entries) ? active.entries.slice(0, 160) : [];
  const entities = [];

  const stats = active.stats && typeof active.stats === "object" ? active.stats : {};
  const statsCommit = String(stats.commit || "").trim().toLowerCase();
  if (statsCommit === commit && Array.isArray(stats.contributors)) {
    const seenContributors = new Set();
    stats.contributors.slice(0, 24).forEach((candidate, index) => {
      const label = cleanText(
        candidate?.name || candidate?.login,
        `Public contributor ${index + 1}`,
        60,
      );
      const key = label.toLocaleLowerCase();
      if (seenContributors.has(key) || entities.length >= 8) return;
      seenContributors.add(key);
      const targetPaths = entries
        .filter(
          (entry) =>
            String(entry?.contributor || "").trim().toLocaleLowerCase() === key,
        )
        .map((entry) => String(entry.path || "").slice(0, 300))
        .filter(Boolean)
        .slice(0, 24);
      entities.push({
        id: `contributor:${index}:${key.slice(0, 32)}`,
        kind: "contributor",
        label,
        avatarUrl: safeAvatarURL(
          candidate?.avatarUrl || candidate?.avatar,
        ),
        detail: `${safeCount(candidate?.commits)} commit${
          safeCount(candidate?.commits) === 1 ? "" : "s"
        } in the commit-matched public statistics`,
        targetPaths,
      });
    });
  }

  const records =
    active.entityRecords && typeof active.entityRecords === "object"
      ? active.entityRecords
      : {};
  const recordsCommit = String(
    records.repositoryCommit || records.commit || "",
  )
    .trim()
    .toLowerCase();
  const recordsMatch = recordsCommit === commit;
  const pullMetadataCommit = String(records.pullMetadataCommit || "")
    .trim()
    .toLowerCase();
  const pullRecordsMatch =
    COMMIT_RE.test(pullMetadataCommit) &&
    records.pullsAvailable !== false;
  const counts = active.counts && typeof active.counts === "object" ? active.counts : {};
  const ownerPart = encodeURIComponent(owner);
  const repoPart = encodeURIComponent(repo);

  const issues = recordsMatch && Array.isArray(records.issues)
    ? records.issues.map((record) => safeRecord(record, "issue")).filter(Boolean)
    : [];
  issues.slice(0, 12).forEach((entity) => {
    entities.push({
      ...entity,
      href: `/${ownerPart}/${repoPart}/issues/${entity.number}`,
    });
  });
  if (!issues.length && safeCount(counts.issues)) {
    entities.push({
      id: "issue-collection",
      kind: "issue-collection",
      label: "Issues",
      detail: `${safeCount(counts.issues)} reported; individual commit-pinned records unavailable`,
      href: `/${ownerPart}/${repoPart}/issues`,
      targetPaths: [".forkmesh/issues"],
    });
  }

  const pulls = pullRecordsMatch && Array.isArray(records.pulls)
    ? records.pulls
        .filter((record) => record?.metadataAvailable !== false)
        .map((record) => safeRecord(record, "pull-request"))
        .filter(Boolean)
    : [];
  const pullCount =
    (records.pullCountExact === true ? safeCount(records.pullCount) : 0) ||
    safeCount(counts.pulls) ||
    safeCount(active.pullCount);
  if (pullCount) {
    entities.push({
      id: "pull-request-collection",
      kind: "pull-request-collection",
      label: "Pull requests",
      detail: pullRecordsMatch
        ? `${pullCount} metadata record${
            pullCount === 1 ? "" : "s"
          } pinned at ${pullMetadataCommit.slice(0, 12)}`
        : `${pullCount} reported; exact pull metadata is unavailable`,
      targetPaths: [".forkmesh/pulls"],
    });
  }
  pulls.slice(0, 12).forEach((entity) => {
    entities.push({
      ...entity,
      detail: `metadata pinned at ${pullMetadataCommit.slice(0, 12)}`,
    });
  });

  return entities.slice(0, 32);
}
