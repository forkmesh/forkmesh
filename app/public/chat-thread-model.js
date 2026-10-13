function copyRecord(value) {
  return value == null ? null : structuredClone(value);
}

function recordId(value) {
  return String(value || "").trim();
}

function replyOrder(left, right) {
  return (Number(left.ts) || 0) - (Number(right.ts) || 0) ||
    String(left.id).localeCompare(String(right.id));
}

export function createThreadStore() {
  const roots = new Map();
  const repliesById = new Map();
  const replyIdsByRoot = new Map();

  function replyRecords(rootId) {
    const ids = replyIdsByRoot.get(recordId(rootId)) || new Set();
    return [...ids]
      .map((id) => repliesById.get(id))
      .filter(Boolean)
      .sort(replyOrder);
  }

  function registerRoot(record) {
    const id = recordId(record?.id);
    if (!id) return null;
    const existing = roots.get(id);
    const next = {
      ...(existing || {}),
      ...copyRecord(record),
      id,
      deleted: Boolean(existing?.deleted || record?.deleted),
    };
    roots.set(id, next);
    return copyRecord(next);
  }

  function addReply(record) {
    const id = recordId(record?.id);
    const rootId = recordId(record?.rootId);
    if (!id || !rootId) return null;
    if (repliesById.has(id) || roots.has(id)) return copyRecord(repliesById.get(id));
    const next = { ...copyRecord(record), id, rootId };
    repliesById.set(id, next);
    const bucket = replyIdsByRoot.get(rootId) || new Set();
    bucket.add(id);
    replyIdsByRoot.set(rootId, bucket);
    return copyRecord(next);
  }

  function updateTarget(idValue, patch) {
    const id = recordId(idValue);
    const current = roots.get(id) || repliesById.get(id);
    if (!current || !patch || typeof patch !== "object") return null;
    const next = { ...current, ...copyRecord(patch), id: current.id };
    if (current.rootId) next.rootId = current.rootId;
    if (roots.has(id)) roots.set(id, next);
    else repliesById.set(id, next);
    return copyRecord(next);
  }

  function deleteTarget(idValue) {
    const id = recordId(idValue);
    const reply = repliesById.get(id);
    if (reply) {
      repliesById.delete(id);
      const bucket = replyIdsByRoot.get(reply.rootId);
      bucket?.delete(id);
      if (bucket && !bucket.size) replyIdsByRoot.delete(reply.rootId);
      return true;
    }
    const root = roots.get(id);
    if (!root) return false;
    if (replyRecords(id).length) {
      roots.set(id, {
        ...root,
        text: "",
        richText: null,
        attachment: null,
        deleted: true,
      });
    } else {
      roots.delete(id);
      replyIdsByRoot.delete(id);
    }
    return true;
  }

  function root(id) {
    return copyRecord(roots.get(recordId(id)) || null);
  }

  function target(idValue) {
    const id = recordId(idValue);
    return copyRecord(roots.get(id) || repliesById.get(id) || null);
  }

  function replies(rootId) {
    return replyRecords(rootId).map(copyRecord);
  }

  function summary(rootId) {
    const records = replyRecords(rootId);
    // Who replied, most recent first and one entry each, so a summary row can
    // show their faces and lead with whoever spoke last. senderId identifies a
    // person (two accounts can share a display name); a frame that carries no
    // senderId falls back to the name it did send.
    const seen = new Set();
    const repliers = [];
    for (let index = records.length - 1; index >= 0; index -= 1) {
      const record = records[index];
      const sender = String(record.sender || "").trim();
      if (!sender) continue;
      const identity = String(record.senderId || "").trim() || sender.toLowerCase();
      if (seen.has(identity)) continue;
      seen.add(identity);
      repliers.push({ sender, senderId: String(record.senderId || "") });
    }
    const latest = records.at(-1) || null;
    return {
      count: records.length,
      repliers,
      participants: repliers.map((replier) => replier.sender),
      latestSender: latest ? String(latest.sender || "").trim() : "",
      latestSenderId: latest ? String(latest.senderId || "") : "",
      latestReplyTs: latest ? Number(latest.ts) || 0 : 0,
    };
  }

  function clearChannel(channelValue) {
    const channel = String(channelValue || "");
    const removedRoots = new Set();
    for (const [id, record] of roots) {
      if (record.channel !== channel) continue;
      roots.delete(id);
      removedRoots.add(id);
    }
    for (const [id, reply] of repliesById) {
      if (reply.channel !== channel && !removedRoots.has(reply.rootId)) continue;
      repliesById.delete(id);
      replyIdsByRoot.get(reply.rootId)?.delete(id);
    }
    for (const [rootId, bucket] of replyIdsByRoot) {
      if (removedRoots.has(rootId) || !bucket.size) replyIdsByRoot.delete(rootId);
    }
  }

  return {
    registerRoot,
    addReply,
    updateTarget,
    deleteTarget,
    root,
    target,
    replies,
    summary,
    clearChannel,
  };
}
