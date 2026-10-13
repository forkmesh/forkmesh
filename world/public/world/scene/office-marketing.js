import { canvasTexture, roundedRect } from "./canvas.js";

const OFFICE_MARKETING_TASK_LIMIT = 250;

function boundedOfficeMarketingTaskText(value, maxLength, fallback = "") {
  const normalized = String(value ?? "")
    .replace(/[\u0000-\u001f\u007f]+/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, maxLength);
  return normalized || fallback;
}

export function normalizeOfficeMarketingTasks(payload = {}) {
  const source =
    payload && typeof payload === "object" && !Array.isArray(payload)
      ? payload
      : {};
  const authorized = source.authorized === true;
  const requestedState = boundedOfficeMarketingTaskText(
    source.state,
    16,
    authorized ? "" : "locked",
  ).toLowerCase();
  const tasks = authorized
    ? (Array.isArray(source.tasks) ? source.tasks : [])
        .filter((task) => task && typeof task === "object" && !Array.isArray(task))
        .slice(0, OFFICE_MARKETING_TASK_LIMIT)
        .map((task) => ({
          id: boundedOfficeMarketingTaskText(task.id, 64),
          title: boundedOfficeMarketingTaskText(
            task.title,
            42,
            "Untitled marketing task",
          ),
          assignee: boundedOfficeMarketingTaskText(
            task.assignee,
            24,
            "Unassigned",
          ),
          status: boundedOfficeMarketingTaskText(
            task.status,
            16,
            "Open",
          ),
          elapsed: boundedOfficeMarketingTaskText(
            task.elapsed,
            18,
            "Not started",
          ),
          canStartStop: task.canStartStop === true,
          canComplete: task.canComplete === true,
          canDelete: task.canDelete === true,
        }))
    : [];
  const members = authorized
    ? (Array.isArray(source.members) ? source.members : [])
        .map((member) =>
          boundedOfficeMarketingTaskText(member, 24).toLowerCase(),
        )
        .filter(Boolean)
        .slice(0, 100)
    : [];
  const attendanceDays = authorized
    ? (Array.isArray(source.attendanceDays) ? source.attendanceDays : [])
        .filter((day) => day && typeof day === "object" && !Array.isArray(day))
        .slice(-7)
        .map((day) => ({
          date: boundedOfficeMarketingTaskText(day.date, 10),
          label: boundedOfficeMarketingTaskText(day.label, 16),
          hours: (Array.isArray(day.hours) ? day.hours : [])
            .filter(
              (entry) =>
                entry &&
                typeof entry === "object" &&
                !Array.isArray(entry),
            )
            .slice(0, 100)
            .map((entry) => ({
              member: boundedOfficeMarketingTaskText(
                entry.member,
                24,
              ).toLowerCase(),
              hours: Math.max(0, Math.min(24, Number(entry.hours) || 0)),
            })),
        }))
    : [];
  const proofs = authorized
    ? (Array.isArray(source.proofs) ? source.proofs : [])
        .filter(
          (proof) =>
            proof && typeof proof === "object" && !Array.isArray(proof),
        )
        .slice(0, 5000)
        .map((proof) => ({
          id: boundedOfficeMarketingTaskText(proof.id, 64),
          member: boundedOfficeMarketingTaskText(
            proof.member,
            24,
          ).toLowerCase(),
          host: boundedOfficeMarketingTaskText(proof.host, 120),
          label: boundedOfficeMarketingTaskText(
            proof.label,
            120,
            "Social post",
          ),
          createdAt: Math.max(0, Number(proof.createdAt) || 0),
        }))
        .filter((proof) => proof.id && proof.member && proof.host)
    : [];
  const initiatives = authorized
    ? (Array.isArray(source.initiatives) ? source.initiatives : [])
        .filter(
          (item) =>
            item && typeof item === "object" && !Array.isArray(item),
        )
        .slice(0, 250)
        .map((item) => ({
          id: boundedOfficeMarketingTaskText(item.id, 64),
          title: boundedOfficeMarketingTaskText(
            item.title,
            72,
            "Repository issue",
          ),
          repo: boundedOfficeMarketingTaskText(item.repo, 201),
          number: Math.max(0, Math.floor(Number(item.number) || 0)),
          href: boundedOfficeMarketingTaskText(item.href, 512),
          createdAt: Math.max(0, Number(item.createdAt) || 0),
        }))
        .filter(
          (item) =>
            item.id &&
            item.repo &&
            item.number > 0 &&
            /^\/[A-Za-z0-9._-]+\/[A-Za-z0-9._-]+\/issues\/\d+$/.test(
              item.href,
            ),
        )
    : [];
  const state =
    !authorized || requestedState === "locked"
      ? "locked"
      : requestedState === "loading"
        ? "loading"
        : tasks.length
          ? "ready"
          : "empty";
  return {
    authorized, state, tasks,
    members,
    attendanceDays,
    proofs,
    initiatives,
    actor: boundedOfficeMarketingTaskText(source.actor, 24).toLowerCase(),
    canManage: authorized && source.canManage === true,
  };
}

export function officeMarketingInitiativesTexture(THREE, payload = {}) {
  const snapshot = normalizeOfficeMarketingTasks(payload);
  return canvasTexture(THREE, 1024, 1024, (context) => {
    context.clearRect(0, 0, 1024, 1024);
    roundedRect(context, 5, 5, 1014, 1014, 18);
    context.fillStyle = "rgba(5,17,14,0.98)";
    context.fill();
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 9;
    context.stroke();
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 50px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText("MARKETING INITIATIVES", 48, 72);
    context.fillStyle = "#8fdcb2";
    context.font = '600 21px "ForkMesh Mono", monospace';
    context.fillText("PRIVATE MARKETING VIEW · ISSUE PIPELINE", 48, 116);
    context.strokeStyle = "#315b52";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(48, 145);
    context.lineTo(976, 145);
    context.stroke();
    if (!snapshot.initiatives.length) {
      context.fillStyle = "#789487";
      context.font = '500 29px "ForkMesh Mono", monospace';
      context.fillText("No repository issues promoted yet.", 48, 215);
      return;
    }
    snapshot.initiatives.slice(0, 8).forEach((item, index) => {
      const top = 164 + index * 98;
      roundedRect(context, 42, top, 940, 84, 10);
      context.fillStyle =
        index % 2 ? "rgba(15,46,36,0.72)" : "rgba(11,37,29,0.84)";
      context.fill();
      context.fillStyle = "#9ef7c6";
      context.font = '700 23px "ForkMesh Mono", monospace';
      context.fillText(`#${item.number}`, 60, top + 27);
      context.fillStyle = "#f1fff6";
      context.font = '650 27px "ForkMesh Favorit", system-ui, sans-serif';
      context.fillText(item.title.slice(0, 48), 145, top + 27);
      context.fillStyle = "#8fdcb2";
      context.font = '500 19px "ForkMesh Mono", monospace';
      context.fillText(`${item.repo} · CLICK TO OPEN`, 60, top + 61);
    });
  });
}

export function officeMarketingTasksTexture(THREE, payload = {}) {
  const snapshot = normalizeOfficeMarketingTasks(payload);
  return canvasTexture(THREE, 2048, 1024, (context) => {
    context.clearRect(0, 0, 2048, 1024);
    roundedRect(context, 5, 5, 2038, 1014, 18);
    context.fillStyle = "rgba(5,17,14,0.97)";
    context.fill();
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 10;
    context.stroke();

    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 54px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText("MARKETING TASK BOARD", 52, 64);
    context.fillStyle = "#9ef7c6";
    context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      `${snapshot.tasks.length} TASK${snapshot.tasks.length === 1 ? "" : "S"} · LIVE ORGANIZATION VIEW`,
      52,
      112,
    );
    context.strokeStyle = "rgba(158,247,198,0.28)";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(52, 142);
    context.lineTo(1996, 142);
    context.stroke();

    if (snapshot.state !== "ready") {
      const stateCopy = {
        locked: ["LOCKED", "Sign in with task access to view this board."],
        loading: ["LOADING", "Retrieving authorized marketing tasks…"],
        empty: ["NO OPEN TASKS", "The authorized marketing queue is empty."],
      }[snapshot.state] || ["UNAVAILABLE", "Task summaries are unavailable."];
      context.fillStyle =
        snapshot.state === "locked" ? "#f7c96b" : "#9ef7c6";
      context.font = '700 46px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(stateCopy[0], 52, 260);
      context.fillStyle = "#b9cfc4";
      context.font = '400 28px "ForkMesh Favorit", system-ui, sans-serif';
      context.fillText(stateCopy[1], 52, 316, 900);
      return;
    }

    let rowsTop = 174;
    if (snapshot.canManage) {
      context.fillStyle = "rgba(158,247,198,0.075)";
      roundedRect(context, 42, 164, 1954, 108, 10);
      context.fill();
      context.fillStyle = "#91a39a";
      context.font = '700 18px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("ASSIGN TO", 68, 190);
      context.fillStyle = "#f1fff6";
      context.font = '700 29px "ForkMesh Favorit", system-ui, sans-serif';
      context.fillText(
        `@${snapshot.members[0] || snapshot.actor || "marketing"}`,
        68,
        232,
        780,
      );
      context.fillStyle = "#9ef7c6";
      roundedRect(context, 1030, 181, 920, 72, 10);
      context.fill();
      context.fillStyle = "#071712";
      context.textAlign = "center";
      context.font = '800 29px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("+ ADD MARKETING TASK", 1490, 218);
      context.textAlign = "left";
      rowsTop = 298;
    }

    const visibleTasks = snapshot.tasks.slice(0, snapshot.canManage ? 6 : 7);
    visibleTasks.forEach((task, index) => {
      const top = rowsTop + index * 112;
      context.fillStyle = index % 2
        ? "rgba(255,255,255,0.025)"
        : "rgba(158,247,198,0.045)";
      roundedRect(context, 42, top, 1954, 96, 8);
      context.fill();
      context.fillStyle = "#d9ffea";
      context.font = '700 27px "ForkMesh Favorit", system-ui, sans-serif';
      context.fillText(task.title, 68, top + 30, 940);
      context.fillStyle = "#91a39a";
      context.font = '400 18px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `@${task.assignee} · ${task.elapsed} · ${task.status.toUpperCase()}`,
        68,
        top + 68,
        930,
      );
      const controls = [
        task.canStartStop
          ? {
              label: task.status === "active" ? "STOP" : "START",
              left: 1110,
              color: "#79d9ff",
            }
          : null,
        task.canComplete && task.status !== "done"
          ? { label: "DONE", left: 1405, color: "#9ef7c6" }
          : null,
        task.canDelete
          ? { label: "DELETE", left: 1700, color: "#ff9d86" }
          : null,
      ].filter(Boolean);
      controls.forEach((control) => {
        context.strokeStyle = control.color;
        context.lineWidth = 3;
        roundedRect(context, control.left, top + 18, 260, 60, 7);
        context.stroke();
        context.fillStyle = control.color;
        context.textAlign = "center";
        context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(control.label, control.left + 130, top + 49);
      });
      context.textAlign = "left";
    });
    if (snapshot.tasks.length > visibleTasks.length) {
      context.fillStyle = "#91a39a";
      context.font = '700 18px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `+ ${snapshot.tasks.length - visibleTasks.length} MORE TASKS · COMPLETE OR DELETE ITEMS TO REVEAL`,
        52,
        984,
      );
    }
  });
}

export function officeMarketingTasksBlankTexture(THREE) {
  return canvasTexture(THREE, 2048, 1024, (context) => {
    context.clearRect(0, 0, 2048, 1024);
    context.fillStyle = "rgba(5,17,14,0.97)";
    context.fillRect(0, 0, 2048, 1024);
  });
}

export function officeMarketingDeskNameTexture(THREE, member) {
  return canvasTexture(THREE, 768, 192, (context) => {
    context.fillStyle = "#06150f";
    context.fillRect(0, 0, 768, 192);
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 8;
    context.strokeRect(6, 6, 756, 180);
    context.fillStyle = "#dffff0";
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '800 58px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(member || "MARKETING").toUpperCase(), 384, 82, 690);
    context.fillStyle = "#7eb899";
    context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("MARKETING · RESERVED DESK", 384, 145, 690);
  });
}

export function officeMarketingDeskProofTexture(
  THREE,
  member,
  proofs,
  canAdd,
) {
  return canvasTexture(THREE, 768, 512, (context) => {
    context.fillStyle = "#06150f";
    context.fillRect(0, 0, 768, 512);
    context.strokeStyle = canAdd ? "#9ef7c6" : "#416957";
    context.lineWidth = 8;
    context.strokeRect(6, 6, 756, 500);
    context.fillStyle = "#dffff0";
    context.font = '800 42px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("POSTED WORK", 34, 60);
    context.fillStyle = "#7eb899";
    context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(`@${String(member || "").slice(0, 24)}`, 34, 98);
    const rows = (Array.isArray(proofs) ? proofs : []).slice(0, 4);
    if (!rows.length) {
      context.fillStyle = "#70887c";
      context.font = '600 29px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        canAdd ? "CLICK TO ADD HTTPS PROOF" : "NO POSTS RECORDED",
        34,
        186,
        700,
      );
      return;
    }
    rows.forEach((proof, index) => {
      const y = 158 + index * 76;
      context.fillStyle = "#effff6";
      context.font = '700 27px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        String(proof.label || proof.host || "Social post").slice(0, 38),
        34,
        y,
        700,
      );
      context.fillStyle = "#83cba8";
      context.font = '600 21px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(String(proof.host || "").slice(0, 54), 34, y + 30, 700);
    });
    if (canAdd) {
      context.fillStyle = "#9ef7c6";
      context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("+ CLICK DESK TO ADD ANOTHER", 34, 480);
    }
  });
}

export function officeMarketingAttendanceTexture(THREE, snapshot) {
  const members = snapshot.members || [];
  const days = snapshot.attendanceDays || [];
  return canvasTexture(THREE, 2048, 1024, (context) => {
    context.fillStyle = "#06150f";
    context.fillRect(0, 0, 2048, 1024);
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 14;
    context.strokeRect(8, 8, 2032, 1008);
    context.fillStyle = "#effff6";
    context.font = '800 66px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("MARKETING · OFFICE HOURS", 56, 86);
    context.fillStyle = "#80bda0";
    context.font = '700 26px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("LAST 7 UTC DAYS · SERVER ATTENDANCE", 56, 132);
    if (!members.length) {
      context.fillStyle = "#91a39a";
      context.font = '600 42px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO MARKETING MEMBERS ASSIGNED", 56, 250);
      return;
    }
    const labelWidth = 360;
    const dayWidth = (1940 - labelWidth) / Math.max(1, days.length);
    context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
    days.forEach((day, index) => {
      context.fillStyle = "#9ef7c6";
      context.textAlign = "center";
      context.fillText(
        day.label || day.date,
        46 + labelWidth + index * dayWidth + dayWidth / 2,
        190,
        dayWidth - 12,
      );
    });
    context.textAlign = "left";
    const rowHeight = Math.min(94, 760 / Math.max(1, members.length));
    members.forEach((member, row) => {
      const y = 245 + row * rowHeight;
      context.fillStyle =
        row % 2 ? "rgba(255,255,255,0.025)" : "rgba(158,247,198,0.05)";
      context.fillRect(40, y - rowHeight * 0.55, 1968, rowHeight * 0.84);
      context.fillStyle = "#e8fff2";
      context.font = `700 ${Math.max(17, Math.min(31, rowHeight * 0.34))}px "ForkMesh Mono", ui-monospace, monospace`;
      context.fillText(member.toUpperCase(), 58, y, labelWidth - 32);
      days.forEach((day, index) => {
        const entry = day.hours?.find((item) => item.member === member);
        context.fillStyle = Number(entry?.hours) > 0 ? "#9ef7c6" : "#668276";
        context.textAlign = "center";
        context.fillText(
          `${Number(entry?.hours || 0).toFixed(1)}h`,
          46 + labelWidth + index * dayWidth + dayWidth / 2,
          y,
        );
      });
      context.textAlign = "left";
    });
  });
}

export function officeAttendanceDurationLabel(value) {
  if (value === null || value === undefined || value === "") return "—";
  const milliseconds = Number(value);
  if (!Number.isFinite(milliseconds) || milliseconds < 0) return "—";
  const totalMinutes = Math.floor(milliseconds / 60_000);
  if (totalMinutes < 60) return `${totalMinutes}m`;
  const totalHours = Math.floor(totalMinutes / 60);
  const minutes = totalMinutes % 60;
  if (totalHours < 24) {
    return `${totalHours}h ${String(minutes).padStart(2, "0")}m`;
  }
  const days = Math.floor(totalHours / 24);
  return `${days}d ${String(totalHours % 24).padStart(2, "0")}h`;
}

export function officeAttendanceLeaderboardRows(
  snapshot = {},
  openElapsedMs = 0,
) {
  const liveElapsedMs = Math.max(0, Number(openElapsedMs) || 0);
  const supplied = Array.isArray(snapshot?.leaderboard)
    ? snapshot.leaderboard
    : [];
  const rows = new Map();
  const keep = (candidate = {}) => {
    const account = String(candidate?.account || "Contributor")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, 32) || "Contributor";
    const key = account.toLocaleLowerCase();
    const durationMs = Math.max(0, Number(candidate?.durationMs) || 0);
    const previous = rows.get(key);
    const present = previous?.present || candidate?.present === true;
    const candidateFloor = String(candidate?.floor || "")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, 32);
    rows.set(key, {
      account: previous?.account || account,
      durationMs: Math.max(previous?.durationMs || 0, durationMs),
      present,
      floor:
        candidate?.present === true && candidateFloor
          ? candidateFloor
          : String(previous?.floor || "")
          .replace(/\s+/g, " ")
          .trim()
          .slice(0, 32),
    });
  };
  if (supplied.length) {
    supplied.forEach((member) => {
      const present = member?.present === true;
      const longestDurationMs = Math.max(
        0,
        Number(member?.longestDurationMs) || 0,
      );
      const activeDurationMs =
        Math.max(0, Number(member?.activeDurationMs) || 0) +
        (present ? liveElapsedMs : 0);
      keep({
        account: member?.account,
        durationMs: Math.max(longestDurationMs, activeDurationMs),
        present,
        floor: member?.floor,
      });
    });
  } else {
    (Array.isArray(snapshot?.visits) ? snapshot.visits : []).forEach(
      (visit) => {
        const present = Boolean(visit?.inAt && !visit?.outAt);
        keep({
          account: visit?.account,
          durationMs:
            Math.max(0, Number(visit?.durationMs) || 0) +
            (present ? liveElapsedMs : 0),
          present,
          floor: visit?.floor,
        });
      },
    );
  }
  return [...rows.values()]
    .sort(
      (left, right) =>
        right.durationMs - left.durationMs ||
        left.account.localeCompare(right.account),
    )
    .slice(0, 20);
}
