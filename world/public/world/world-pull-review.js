const COMMIT_RE = /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/i;
const MAX_PULL_BODY_CHARS = 12_000;
const MAX_PULL_METADATA_CHARS = 64 * 1024;
const MAX_DIFF_CHARS = 2 * 1024 * 1024;
const MAX_DIFF_FILES = 200;
const MAX_DIFF_ROWS = 20_000;
const MAX_DIFF_LINE_CHARS = 4_000;
const MERGE_REQUEST_ID_RE = /^[A-Za-z0-9_-]{12,80}$/;

function boundedText(value, limit) {
  return String(value ?? "")
    .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "")
    .slice(0, limit);
}

function unquoteMetadataValue(value) {
  const text = String(value || "").trim();
  if (
    text.length >= 2 &&
    text.startsWith('"') &&
    text.endsWith('"')
  ) {
    try {
      const parsed = JSON.parse(text);
      return typeof parsed === "string" ? parsed : text;
    } catch (_) {}
  }
  if (
    text.length >= 2 &&
    text.startsWith("'") &&
    text.endsWith("'")
  ) {
    return text.slice(1, -1).replace(/''/g, "'");
  }
  return text;
}

export function immutableGitOid(value) {
  const commit = String(value || "").trim().toLowerCase();
  return COMMIT_RE.test(commit) ? commit : "";
}

export function safePullNumber(value) {
  const number = Number(value);
  return Number.isSafeInteger(number) &&
    number >= 1 &&
    number <= 10_000_000
    ? number
    : 0;
}

export function safeDiffPath(value) {
  const path = boundedText(value, 400)
    .replaceAll("\\", "/")
    .trim();
  if (
    !path ||
    path.startsWith("/") ||
    /^[A-Za-z]:\//.test(path) ||
    /[\r\n\t]/.test(path) ||
    path.startsWith('"') ||
    path.endsWith('"') ||
    path.includes("\u0000")
  ) {
    return "";
  }
  const segments = path.split("/");
  if (
    segments.some(
      (segment) =>
        !segment ||
        segment === "." ||
        segment === ".." ||
        segment.length > 180,
    )
  ) {
    return "";
  }
  return path;
}

export function parsePullFrontMatter(markdown, fallbackNumber = 0) {
  const source = boundedText(markdown, MAX_PULL_METADATA_CHARS);
  const lines = source.split(/\r?\n/);
  const values = {};
  let bodyStart = 0;
  if (lines[0] === "---") {
    let closing = -1;
    for (let index = 1; index < Math.min(lines.length, 160); index += 1) {
      if (lines[index] === "---") {
        closing = index;
        break;
      }
      const match = lines[index].match(/^([A-Za-z][A-Za-z0-9_-]{0,40}):\s*(.*)$/);
      if (!match) continue;
      values[match[1]] = unquoteMetadataValue(match[2]);
    }
    if (closing >= 0) bodyStart = closing + 1;
  }
  const number = safePullNumber(values.number) || safePullNumber(fallbackNumber);
  const status = ["open", "closed", "merged"].includes(
    String(values.status || "").toLowerCase(),
  )
    ? String(values.status).toLowerCase()
    : "unknown";
  const draft = ["1", "true", "yes"].includes(
    String(values.draft || "").toLowerCase(),
  );
  const mergeableValue = String(values.mergeable || "").toLowerCase();
  const mergeable = ["1", "true", "yes"].includes(mergeableValue)
    ? true
    : ["0", "false", "no"].includes(mergeableValue)
      ? false
      : undefined;
  const reviewStatus = boundedText(
    values.reviewStatus ||
      values.review_status ||
      values.mergeStatus ||
      values.merge_status ||
      "",
    24,
  ).toLowerCase();
  const checksStatus = boundedText(
    values.checksStatus ||
      values.checks_status ||
      values.ciStatus ||
      values.ci_status ||
      "",
    24,
  ).toLowerCase();
  return {
    number,
    schema: boundedText(values.schema || "", 64).trim(),
    title: boundedText(
      values.title || (number ? `Pull request #${number}` : "Pull request"),
      240,
    ).trim(),
    status,
    draft,
    mergeable,
    reviewStatus,
    checksStatus,
    author: boundedText(values.authorName || values.author || "Unknown", 100).trim(),
    base: boundedText(values.base || "main", 160).trim(),
    head: boundedText(values.head || "", 160).trim(),
    derive: boundedText(values.derive || "", 32).trim().toLowerCase(),
    createdAt: Number(values.ts || values.createdAt || 0) || 0,
    creationBaseOid: immutableGitOid(
      values.creationBaseOid || values.baseOid || "",
    ),
    creationHeadOid: immutableGitOid(
      values.creationHeadOid || values.headOid || "",
    ),
    body: boundedText(lines.slice(bodyStart).join("\n").trim(), MAX_PULL_BODY_CHARS),
  };
}

function diffHeaderPaths(line) {
  const match = String(line || "").match(/^diff --git a\/(.+?) b\/(.+)$/);
  if (!match) return null;
  const oldPath = safeDiffPath(match[1]);
  const newPath = safeDiffPath(match[2]);
  return oldPath && newPath ? { oldPath, newPath } : null;
}

function newDiffFile(paths, index) {
  return {
    id: `pull-file-${index + 1}`,
    oldPath: paths.oldPath,
    newPath: paths.newPath,
    path: paths.newPath || paths.oldPath,
    status: "modified",
    binary: false,
    additions: 0,
    deletions: 0,
    rows: [],
    truncated: false,
  };
}

export function parseUnifiedDiff(rawPatch, options = {}) {
  const maxCharacters = Math.min(
    Math.max(Number(options.maxCharacters) || MAX_DIFF_CHARS, 1_024),
    MAX_DIFF_CHARS,
  );
  const maxFiles = Math.min(
    Math.max(Number(options.maxFiles) || MAX_DIFF_FILES, 1),
    MAX_DIFF_FILES,
  );
  const maxRows = Math.min(
    Math.max(Number(options.maxRows) || MAX_DIFF_ROWS, 1),
    MAX_DIFF_ROWS,
  );
  const raw = String(rawPatch || "");
  const source = boundedText(raw, maxCharacters);
  const lines = source.split(/\r?\n/);
  const files = [];
  let current = null;
  let oldLine = 0;
  let newLine = 0;
  let rowCount = 0;
  let omittedFiles = 0;
  let truncated = raw.length > source.length;

  for (const lineValue of lines) {
    const line = boundedText(lineValue, MAX_DIFF_LINE_CHARS);
    if (lineValue.length > line.length) truncated = true;
    if (line.startsWith("diff --git ")) {
      const paths = diffHeaderPaths(line);
      if (!paths) {
        current = null;
        continue;
      }
      if (files.length >= maxFiles) {
        omittedFiles += 1;
        current = null;
        truncated = true;
        continue;
      }
      current = newDiffFile(paths, files.length);
      files.push(current);
      oldLine = 0;
      newLine = 0;
      continue;
    }
    if (!current) continue;
    if (line.startsWith("new file mode ")) {
      current.status = "added";
      continue;
    }
    if (line.startsWith("deleted file mode ")) {
      current.status = "deleted";
      continue;
    }
    if (line.startsWith("rename from ")) {
      const path = safeDiffPath(line.slice("rename from ".length));
      if (path) current.oldPath = path;
      current.status = "renamed";
      continue;
    }
    if (line.startsWith("rename to ")) {
      const path = safeDiffPath(line.slice("rename to ".length));
      if (path) {
        current.newPath = path;
        current.path = path;
      }
      continue;
    }
    if (
      line.startsWith("Binary files ") ||
      line.startsWith("GIT binary patch")
    ) {
      current.binary = true;
      continue;
    }
    if (
      line.startsWith("index ") ||
      line.startsWith("similarity index ") ||
      line.startsWith("old mode ") ||
      line.startsWith("new mode ") ||
      line.startsWith("--- ") ||
      line.startsWith("+++ ")
    ) {
      continue;
    }
    if (rowCount >= maxRows) {
      current.truncated = true;
      truncated = true;
      continue;
    }
    if (line.startsWith("@@")) {
      const match = line.match(
        /^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@/,
      );
      if (match) {
        oldLine = Number(match[1]);
        newLine = Number(match[2]);
      }
      current.rows.push({ type: "hunk", text: line });
      rowCount += 1;
      continue;
    }
    if (!line || !["+", "-", " ", "\\"].includes(line[0])) continue;
    if (line[0] === "+") {
      current.rows.push({
        type: "add",
        oldLine: null,
        newLine,
        text: line.slice(1),
      });
      current.additions += 1;
      newLine += 1;
    } else if (line[0] === "-") {
      current.rows.push({
        type: "delete",
        oldLine,
        newLine: null,
        text: line.slice(1),
      });
      current.deletions += 1;
      oldLine += 1;
    } else if (line[0] === "\\") {
      current.rows.push({
        type: "meta",
        oldLine: null,
        newLine: null,
        text: line,
      });
    } else {
      current.rows.push({
        type: "context",
        oldLine,
        newLine,
        text: line.slice(1),
      });
      oldLine += 1;
      newLine += 1;
    }
    rowCount += 1;
  }

  return {
    files,
    additions: files.reduce((total, file) => total + file.additions, 0),
    deletions: files.reduce((total, file) => total + file.deletions, 0),
    rowCount,
    truncated,
    omittedFiles,
  };
}

export function buildPullFileTree(files) {
  const root = { kind: "directory", name: "", path: "", children: [] };
  const directories = new Map([["", root]]);
  (Array.isArray(files) ? files : []).forEach((file, fileIndex) => {
    const path = safeDiffPath(file?.path || file?.newPath || file?.oldPath);
    if (!path) return;
    const segments = path.split("/");
    let parent = root;
    let prefix = "";
    segments.forEach((segment, segmentIndex) => {
      prefix = prefix ? `${prefix}/${segment}` : segment;
      const leaf = segmentIndex === segments.length - 1;
      if (leaf) {
        parent.children.push({
          kind: "file",
          name: segment,
          path,
          fileIndex,
          status: String(file?.status || "modified"),
          additions: Number(file?.additions) || 0,
          deletions: Number(file?.deletions) || 0,
        });
        return;
      }
      let directory = directories.get(prefix);
      if (!directory) {
        directory = {
          kind: "directory",
          name: segment,
          path: prefix,
          children: [],
        };
        directories.set(prefix, directory);
        parent.children.push(directory);
      }
      parent = directory;
    });
  });

  const sort = (node) => {
    node.children.sort((left, right) => {
      if (left.kind !== right.kind) return left.kind === "directory" ? -1 : 1;
      return left.name.localeCompare(right.name);
    });
    node.children.forEach((child) => {
      if (child.kind === "directory") sort(child);
    });
  };
  sort(root);
  return root.children;
}

export function pullViewedStateKey(owner, repo, metadataCommit, number) {
  const commit = immutableGitOid(metadataCommit);
  const pullNumber = safePullNumber(number);
  const cleanOwner = boundedText(owner, 40).trim().toLowerCase();
  const cleanRepo = boundedText(repo, 60).trim().toLowerCase();
  return cleanOwner && cleanRepo && commit && pullNumber
    ? `${cleanOwner}/${cleanRepo}@${commit}#${pullNumber}`
    : "";
}

// Return the exact immutable inputs that the protected merge endpoint is
// allowed to receive. A syntactically valid session is checked separately by
// the World shell because this pure helper deliberately has no storage access.
export function exactPullMergeContext(active, review) {
  const metadata = review?.metadata;
  const number = safePullNumber(review?.number);
  const expectedBaseOid = immutableGitOid(active?.commit);
  const expectedHeadOid = immutableGitOid(metadata?.creationHeadOid);
  const expectedPullsOid = immutableGitOid(review?.metadataCommit);
  const indexedBaseOid = immutableGitOid(
    active?.entityRecords?.repositoryCommit || active?.commit,
  );
  const indexedPullsOid = immutableGitOid(
    active?.entityRecords?.pullMetadataCommit,
  );
  if (
    active?.isPrivate === true ||
    review?.state !== "ready" ||
    !metadata ||
    metadata.schema !== "forkmesh-pull-v1" ||
    metadata.derive !== "branch" ||
    metadata.status !== "open" ||
    safePullNumber(metadata.number) !== number ||
    !expectedBaseOid ||
    expectedBaseOid !== immutableGitOid(metadata.creationBaseOid) ||
    indexedBaseOid !== expectedBaseOid ||
    !expectedHeadOid ||
    !expectedPullsOid ||
    indexedPullsOid !== expectedPullsOid ||
    new Set([
      expectedBaseOid.length,
      expectedHeadOid.length,
      expectedPullsOid.length,
    ]).size !== 1
  ) {
    return null;
  }
  return {
    number,
    expectedBaseOid,
    expectedHeadOid,
    expectedPullsOid,
  };
}

export function buildPullMergeRequest(context, requestId, options = {}) {
  const checked = exactPullMergeContext(
    {
      commit: context?.expectedBaseOid,
      entityRecords: {
        repositoryCommit: context?.expectedBaseOid,
        pullMetadataCommit: context?.expectedPullsOid,
      },
    },
    {
      state: "ready",
      number: context?.number,
      metadataCommit: context?.expectedPullsOid,
      metadata: {
        number: context?.number,
        schema: "forkmesh-pull-v1",
        derive: "branch",
        status: "open",
        creationBaseOid: context?.expectedBaseOid,
        creationHeadOid: context?.expectedHeadOid,
      },
    },
  );
  const stableRequestId = String(requestId || "").trim();
  if (!checked || !MERGE_REQUEST_ID_RE.test(stableRequestId)) return null;
  return {
    schemaVersion: 1,
    type: "forkmesh.pull-merge-v1",
    pullNumber: checked.number,
    requestId: stableRequestId,
    expectedBaseOid: checked.expectedBaseOid,
    expectedHeadOid: checked.expectedHeadOid,
    expectedPullsOid: checked.expectedPullsOid,
    // A repository owner may merge without the independent-review gate. The
    // field is absent otherwise, so an ordinary merge keeps the exact request
    // shape the relay and every mirror already accept.
    ...(options.overrideReview === true ? { overrideReview: true } : {}),
  };
}
