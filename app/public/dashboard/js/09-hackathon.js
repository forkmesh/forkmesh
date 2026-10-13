  // ---------------------------------------------------------------------------
  // Hackathon HQ (/hackathon/me). One participant's own standing: points,
  // rank, missions, squad and the share kit that brings the next builder in.
  //
  // Two rules this page exists to make visible, because getting them wrong is
  // what turns a growth loop into spam:
  //
  //   * A friend counts as *joined* when they verify their email, which is
  //     what moves the progress bars here. Anything of value waits for the
  //     referral ledger to credit them, and the two numbers are always shown
  //     apart so nobody reads a pending invite as an earned reward.
  //   * Sharing is never rewarded. Every share press is reported so the
  //     growth dashboard can measure a K-factor, and it is worth zero points
  //     by construction; the points come from what the person you invited
  //     then does.
  //
  // Nothing on this page can send a message. Each channel opens the user's
  // own client with text they can still edit, which is why ForkMesh asks for
  // no contact list and holds no recipient anywhere.
  function hackathonAccountName() {
    const session = state.session;
    if (!session?.nodeName || session.kind === "node") return "";
    if (session.kind !== "user" && !session.email) return "";
    return String(session.nodeName).toLowerCase();
  }

  function hackathonMeter(percent) {
    const value = Math.max(0, Math.min(100, Number(percent) || 0));
    return `<div class="mt-2 h-1.5 w-full overflow-hidden rounded-full bg-secondary">
      <div class="h-full rounded-full bg-accent" style="width:${value}%"></div>
    </div>`;
  }

  function hackathonStatTile(label, value, hint) {
    return `<div class="rounded-md border border-border bg-background p-3">
      <p class="text-xs text-muted-foreground">${escapeHtml(label)}</p>
      <p class="mt-1 text-2xl font-semibold text-foreground tabular-nums">${escapeHtml(value)}</p>
      <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(hint)}</p>
    </div>`;
  }

  function hackathonOnboardingHtml(data) {
    const onboarding = data.onboarding || {};
    if (!onboarding.steps || onboarding.complete) return "";
    const current = Number(onboarding.step) || 1;
    const steps = onboarding.steps.map((entry) => {
      const done = Number(entry.step) < current;
      const active = Number(entry.step) === current;
      const mark = done ? "check" : String(entry.step);
      return `<li class="rounded-md border ${active ? "border-foreground/30 bg-secondary" : "border-border bg-background"} p-3">
        <span class="inline-flex h-6 w-6 items-center justify-center rounded-full border border-border ${done ? "bg-accent text-accent-foreground" : "bg-secondary text-muted-foreground"} font-mono text-[11px]">${done ? "&check;" : escapeHtml(mark)}</span>
        <p class="mt-2 text-sm font-semibold text-foreground">${escapeHtml(entry.title || "")}</p>
      </li>`;
    }).join("");
    const needed = Number(onboarding.invitesNeeded) || 0;
    const nudge = current === 3 && needed > 0
      ? `<p class="mt-3 text-xs text-muted-foreground">${escapeHtml(`${onboarding.invitesJoined || 0} of 3 friends joined. ${needed === 1 ? "You are one friend away." : `${needed} to go.`}`)}</p>`
      : "";
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Getting started</h2>
      <ol class="mt-3 grid gap-3 sm:grid-cols-4">${steps}</ol>
      ${nudge}
    </div>`;
  }

  function hackathonShareHtml(data) {
    const share = data.share || {};
    const link = String(share.link || "");
    if (!link) return "";
    // Channels whose target is a URL arrive ready-built from the server.
    // copy/native/qr are client-owned because the clipboard, the share sheet
    // and the QR canvas all live here.
    const buttons = (share.channels || []).map((channel) => {
      const label = escapeHtml(channel.label || channel.id);
      const base = "inline-flex items-center gap-1.5 rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors";
      if (channel.kind === "url" && channel.url) {
        return `<a data-hackathon-share="${escapeHtml(channel.id)}" class="${base}" href="${escapeHtml(channel.url)}" target="_blank" rel="noopener noreferrer">${label}</a>`;
      }
      return `<button data-hackathon-share="${escapeHtml(channel.id)}" type="button" class="${base}">${label}</button>`;
    }).join("");
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Your invite link</h2>
      <div class="mt-3 flex flex-wrap items-center gap-2">
        <input
          data-hackathon-link
          type="text"
          readonly
          aria-label="Your hackathon invite link"
          value="${escapeHtml(link)}"
          class="min-w-0 flex-1 rounded-md border border-border bg-background px-3 py-2 font-mono text-sm text-foreground focus:outline-none focus:ring-1 focus:ring-foreground/20"
        />
        <button
          data-hackathon-copy
          type="button"
          class="shrink-0 inline-flex items-center gap-1.5 rounded-md bg-primary px-3 py-2 text-xs font-semibold text-primary-foreground hover:opacity-90 transition-colors"
        >
          <i data-lucide="copy" class="w-3.5 h-3.5"></i>
          Copy link
        </button>
      </div>
      <p class="mt-3 text-xs text-muted-foreground">Anyone opening it sees who invited them and where you stand, not a generic page.</p>
      <div class="mt-3 flex flex-wrap gap-2">${buttons}</div>
      <div data-hackathon-qr class="mt-4" hidden></div>
      <p class="mt-3 rounded-md border border-border bg-background p-3 text-xs text-muted-foreground">${escapeHtml(String(share.text || ""))}</p>
    </div>`;
  }

  // The story image. Drawn on a canvas here so Instagram and other
  // Stories -- which have no share URL -- still get something worth posting,
  // carrying the participant's real standing and their invite link as a QR
  // code. Colours are ForkMesh's own dark ground, fixed rather than themed,
  // because a story is posted into somebody else's app.
  function hackathonWrapText(context, text, maxWidth) {
    const words = String(text || "").split(/\s+/);
    const lines = [];
    let line = "";
    words.forEach((word) => {
      const next = line ? `${line} ${word}` : word;
      if (line && context.measureText(next).width > maxWidth) {
        lines.push(line);
        line = word;
      } else {
        line = next;
      }
    });
    if (line) lines.push(line);
    return lines;
  }

  async function renderHackathonStory(data) {
    const canvas = document.createElement("canvas");
    canvas.width = 1080;
    canvas.height = 1920;
    const context = canvas.getContext("2d");
    const you = data.you || {};
    const share = data.share || {};
    const sans = '-apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif';
    context.fillStyle = "#090909";
    context.fillRect(0, 0, 1080, 1920);
    context.strokeStyle = "#2a2a2e";
    context.lineWidth = 2;
    for (let x = 0; x <= 1080; x += 90) {
      context.beginPath(); context.moveTo(x, 0); context.lineTo(x, 1920); context.stroke();
    }
    for (let y = 0; y <= 1920; y += 90) {
      context.beginPath(); context.moveTo(0, y); context.lineTo(1080, y); context.stroke();
    }
    context.fillStyle = "#3fb950";
    context.font = `700 40px ${sans}`;
    context.fillText(String(data.campaign?.title || "ForkMesh Hackathon").toUpperCase(), 90, 220);
    context.fillStyle = "#f5f5f5";
    context.font = `700 104px ${sans}`;
    const headline = you.rank
      ? `I'm #${Number(you.rank).toLocaleString()} of ${Number(data.campaign?.participants || 0).toLocaleString()} builders.`
      : "I'm building in the ForkMesh Hackathon.";
    let y = 380;
    hackathonWrapText(context, headline, 900).slice(0, 4).forEach((line) => {
      context.fillText(line, 90, y);
      y += 124;
    });
    context.fillStyle = "#a3a3a3";
    context.font = `500 52px ${sans}`;
    y += 20;
    context.fillText(`${Number(you.points || 0).toLocaleString()} points. Think you can beat me?`, 90, y);
    const qrSize = 560;
    const qrTop = 1060;
    context.fillStyle = "#ffffff";
    context.fillRect(90 - 30, qrTop - 30, qrSize + 60, qrSize + 60);
    if (await loadHackathonQr()) {
      try {
        const qr = window.ForkMeshQR.toCanvas(String(share.link || ""), qrSize);
        context.drawImage(qr, 90, qrTop, qrSize, qrSize);
      } catch (_) {}
    }
    context.fillStyle = "#f5f5f5";
    context.font = `600 44px ${sans}`;
    context.fillText("Scan to join me", 720, qrTop + 80);
    context.fillStyle = "#a3a3a3";
    context.font = `400 34px ${sans}`;
    hackathonWrapText(context, "Build something that outlives its host.", 300).forEach((line, index) => {
      context.fillText(line, 720, qrTop + 150 + index * 46);
    });
    context.fillStyle = "#58a6ff";
    context.font = `500 34px ui-monospace, SFMono-Regular, Menlo, monospace`;
    context.fillText(String(share.link || "").replace(/^https?:\/\//, ""), 90, 1800);
    return new Promise((resolve) => canvas.toBlob(resolve, "image/png"));
  }

  async function shareHackathonStory(data) {
    const blob = await renderHackathonStory(data);
    if (!blob) return false;
    const file = new File([blob], "forkmesh-hackathon-story.png", { type: "image/png" });
    // Phones hand the image straight to the share sheet, which is where
    // Stories are posted from. Everywhere else it is saved as a file.
    if (navigator.canShare?.({ files: [file] })) {
      try {
        await navigator.share({ files: [file], title: "ForkMesh Hackathon" });
        return true;
      } catch (_) {
        return false;
      }
    }
    const url = URL.createObjectURL(blob);
    const anchor = Object.assign(document.createElement("a"), {
      href: url, download: file.name,
    });
    document.body.append(anchor);
    anchor.click();
    anchor.remove();
    window.setTimeout(() => URL.revokeObjectURL(url), 5000);
    return true;
  }

  function hackathonMissionsHtml(data) {
    const missions = data.missions || [];
    if (!missions.length) return "";
    const rows = missions.map((mission) => {
      const percent = mission.threshold
        ? Math.round((Number(mission.progress) || 0) * 100 / Number(mission.threshold))
        : 0;
      const reward = mission.reward?.text ? `<span class="shrink-0 rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold ${mission.complete ? "text-accent" : "text-muted-foreground"}">${escapeHtml(mission.reward.text)}</span>` : "";
      return `<div class="px-4 sm:px-5 py-3">
        <div class="flex items-center gap-3">
          <div class="min-w-0 flex-1">
            <p class="truncate text-sm font-semibold text-foreground">${mission.complete ? "&check; " : ""}${escapeHtml(mission.title || "")}</p>
            <p class="mt-0.5 text-xs text-muted-foreground">${escapeHtml(mission.description || "")}</p>
          </div>
          ${reward}
          <span class="shrink-0 font-mono text-xs text-muted-foreground tabular-nums">${escapeHtml(`${mission.progress || 0}/${mission.threshold || 0}`)}</span>
        </div>
        ${hackathonMeter(percent)}
      </div>`;
    }).join("");
    return `<div class="border-b border-border">
      <div class="px-4 sm:px-5 py-3 flex flex-wrap items-center justify-between gap-2">
        <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Missions</h2>
        <span class="text-xs text-muted-foreground">Fixed thresholds, no draws</span>
      </div>
      <div class="divide-y divide-border">${rows}</div>
    </div>`;
  }

  // What a finished mission or a judged result actually granted. Membership
  // months are a promotion on the account: they sit beside a paid plan rather
  // than replacing it, so the copy says so instead of implying a subscription.
  function hackathonRewardReason(key, data) {
    const parts = String(key || "").split(":");
    if (parts[0] === "mission") {
      const mission = (data.missions || []).find((m) => m.key === parts[1]);
      return mission ? `Mission: ${mission.title}` : "Mission";
    }
    if (parts[0] === "prize") {
      const labels = { first: "1st place", second: "2nd place",
        submission: "Valid submission", recruiter: "Top recruiter" };
      const track = parts[1] === "first" || parts[1] === "second"
        ? ` in ${parts[2] || ""}` : parts[1] === "recruiter" ? ` #${parts[2] || ""}` : "";
      return `${labels[parts[1]] || "Prize"}${track}`;
    }
    return "Hackathon reward";
  }

  function hackathonRewardsHtml(data) {
    const rewards = Array.isArray(data.rewards) ? data.rewards : [];
    if (!rewards.length) return "";
    const rows = rewards.map((reward) => {
      const months = Number(reward.months) || 0;
      const what = reward.kind === "pro_months"
        ? `${months} month${months === 1 ? "" : "s"} of Pro`
        : reward.kind === "supporter_months"
          ? `${months} month${months === 1 ? "" : "s"} of Supporter`
          : `Badge: ${String(reward.badge || "").replace(/^hackathon_/, "").replace(/_/g, " ")}`;
      const until = reward.expiresAt
        ? `<span class="shrink-0 text-xs text-muted-foreground">until ${escapeHtml(new Date(Number(reward.expiresAt)).toLocaleDateString())}</span>`
        : "";
      return `<li class="flex items-center gap-3 rounded-md border border-border bg-background px-3 py-2">
        <i data-lucide="${reward.kind === "badge" ? "award" : "gift"}" class="h-4 w-4 shrink-0 text-accent"></i>
        <div class="min-w-0 flex-1">
          <p class="text-sm font-semibold text-foreground">${escapeHtml(what)}</p>
          <p class="text-xs text-muted-foreground">${escapeHtml(hackathonRewardReason(reward.key, data))}</p>
        </div>
        ${until}
      </li>`;
    }).join("");
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Your rewards</h2>
      <ul class="mt-3 grid gap-2">${rows}</ul>
      <p class="mt-3 text-xs text-muted-foreground">Membership months are applied as a promotion. They stack with each other and never change or cancel a paid subscription.</p>
    </div>`;
  }

  // Challenge links. The number to beat is fixed when the link is sent, so the
  // page can say plainly what somebody has to do, and it never changes under
  // them. Beating a challenge is bragging rights: it pays no points, because
  // points paid for challenges could be minted by two friends in a loop.
  function hackathonDateLabel(ms) {
    const value = Number(ms) || 0;
    return value ? new Date(value).toLocaleString([], {
      month: "short", day: "numeric", hour: "numeric", minute: "2-digit",
    }) : "";
  }

  function hackathonChallengesHtml(data) {
    const challenges = data.challenges || {};
    const sent = Array.isArray(challenges.sent) ? challenges.sent : [];
    const accepted = Array.isArray(challenges.accepted) ? challenges.accepted : [];
    const points = Number(data.you?.points) || 0;
    const openSent = sent.filter((entry) => entry.open).length;
    const atLimit = openSent >= (Number(challenges.maxOpen) || 20);
    const sentRows = sent.map((entry) => `<li class="rounded-md border border-border bg-background p-3">
        <div class="flex flex-wrap items-center gap-2">
          <input type="text" readonly value="${escapeHtml(entry.link || "")}" aria-label="Challenge link" class="min-w-0 flex-1 rounded-md border border-border bg-background px-2 py-1.5 font-mono text-xs text-foreground" />
          <button type="button" data-hackathon-challenge-copy="${escapeHtml(entry.link || "")}" class="shrink-0 inline-flex items-center gap-1.5 rounded-md border border-border px-2.5 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary"${entry.open ? "" : " disabled"}><i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy</button>
        </div>
        <p class="mt-2 text-xs text-muted-foreground tabular-nums">${escapeHtml(`Beat ${formatCount(entry.target)} points · ${formatCount(entry.accepted)} accepted · ${formatCount(entry.beaten)} beaten · ${entry.open ? `open until ${hackathonDateLabel(entry.expiresAt)}` : "closed"}`)}</p>
      </li>`).join("");
    const acceptedRows = accepted.map((entry) => {
      const beaten = entry.you?.beaten;
      const status = beaten
        ? `You beat ${entry.challenger}`
        : entry.open
          ? `${formatCount(Math.max(0, entry.target + 1 - points))} more points to beat ${entry.challenger}`
          : `Closed before you passed ${entry.challenger}`;
      return `<li class="flex items-center gap-3 rounded-md border border-border bg-background px-3 py-2">
        <i data-lucide="${beaten ? "trophy" : "swords"}" class="h-4 w-4 shrink-0 ${beaten ? "text-accent" : "text-muted-foreground"}"></i>
        <div class="min-w-0 flex-1">
          <p class="text-sm font-semibold text-foreground">${escapeHtml(status)}</p>
          <p class="text-xs text-muted-foreground tabular-nums">${escapeHtml(`Target ${formatCount(entry.target)} points · you have ${formatCount(points)}`)}</p>
        </div>
      </li>`;
    }).join("");
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Challenges</h2>
        <button type="button" data-hackathon-challenge-new class="inline-flex items-center gap-1.5 rounded-md bg-primary px-3 py-1.5 text-xs font-semibold text-primary-foreground hover:opacity-90"${atLimit ? " disabled" : ""}><i data-lucide="swords" class="w-3.5 h-3.5"></i>${atLimit ? "Open challenge limit reached" : `Challenge friends to beat ${formatCount(points)}`}</button>
      </div>
      <p class="mt-2 text-xs text-muted-foreground">A challenge link dares anyone who opens it to pass your points as they are right now. One link works for a whole group chat. It is for bragging rights and pays no points.</p>
      ${sentRows ? `<h3 class="mt-4 text-xs font-semibold text-foreground">Sent</h3><ul class="mt-2 grid gap-2">${sentRows}</ul>` : ""}
      ${acceptedRows ? `<h3 class="mt-4 text-xs font-semibold text-foreground">Accepted</h3><ul class="mt-2 grid gap-2">${acceptedRows}</ul>` : ""}
    </div>`;
  }

  // Team and submission. A Team is the submission unit (up to five people,
  // one project per track); a Squad is everybody one participant brought in.
  // The two are separate panels with separate words on purpose.
  const HACKATHON_FLAG_LABELS = {
    repository_not_public: "The repository is not public on ForkMesh yet.",
    hosted_before_campaign: "The repository was first hosted before the campaign opened.",
    hosted_after_deadline: "The repository was first hosted after the deadline.",
  };
  const HACKATHON_STATUS_LABELS = {
    valid: "Valid",
    needs_review: "Waiting for a judge",
    invalid: "Not eligible",
    withdrawn: "Withdrawn",
  };

  function hackathonInput(name, label, value, attrs = "") {
    return `<label class="grid gap-1 text-xs text-muted-foreground">${escapeHtml(label)}
      <input name="${name}" value="${escapeHtml(value || "")}" ${attrs} class="rounded-md border border-border bg-background px-2.5 py-1.5 text-sm text-foreground focus:outline-none focus:ring-1 focus:ring-foreground/20" />
    </label>`;
  }

  function hackathonSubmissionForm(track, submission) {
    const s = submission || {};
    return `<form data-hackathon-submission-form data-track="${escapeHtml(track.id)}" class="mt-3 grid gap-2 sm:grid-cols-2">
      ${hackathonInput("repo", "Repository on ForkMesh (owner/name)", s.repo, 'required placeholder="alice/keepalive"')}
      ${hackathonInput("title", "Project title", s.title, 'required minlength="3" maxlength="80"')}
      <label class="grid gap-1 text-xs text-muted-foreground sm:col-span-2">Pitch (280 characters)
        <textarea name="pitch" maxlength="280" rows="2" class="rounded-md border border-border bg-background px-2.5 py-1.5 text-sm text-foreground focus:outline-none focus:ring-1 focus:ring-foreground/20">${escapeHtml(s.pitch || "")}</textarea>
      </label>
      ${hackathonInput("demoUrl", "Demo link (https, optional)", s.demoUrl, 'type="url" placeholder="https://"')}
      ${hackathonInput("videoUrl", "Video link (https, optional)", s.videoUrl, 'type="url" placeholder="https://"')}
      <div class="sm:col-span-2 flex flex-wrap items-center gap-2">
        <button type="submit" class="rounded-md bg-primary px-3 py-1.5 text-xs font-semibold text-primary-foreground hover:opacity-90">${submission ? "Save changes" : `Submit to ${escapeHtml(track.name)}`}</button>
        <span data-hackathon-form-status class="text-xs text-muted-foreground"></span>
      </div>
    </form>`;
  }

  function hackathonTeamHtml(data) {
    const team = data.team;
    const tracks = data.campaign?.tracks || [];
    if (!team) {
      if (!data.teamsOpen) return "";
      return `<div class="px-4 sm:px-5 py-4 border-b border-border">
        <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Your team</h2>
        <p class="mt-2 text-xs text-muted-foreground">A team is up to five people and submits one project per track. It is separate from your squad, which is everyone you brought in.</p>
        <div class="mt-3 grid gap-3 sm:grid-cols-2">
          <form data-hackathon-team-create class="flex items-end gap-2">
            ${hackathonInput("name", "Start a team", "", 'required minlength="2" maxlength="40" placeholder="Team name"')}
            <button type="submit" class="shrink-0 rounded-md bg-primary px-3 py-1.5 text-xs font-semibold text-primary-foreground hover:opacity-90">Create</button>
          </form>
          <form data-hackathon-team-join class="flex items-end gap-2">
            ${hackathonInput("code", "Join with an invite code", "", 'required pattern="[a-z0-9]{8}" placeholder="8 characters"')}
            <button type="submit" class="shrink-0 rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary">Join</button>
          </form>
        </div>
        <p data-hackathon-team-status class="mt-2 text-xs text-muted-foreground"></p>
      </div>`;
    }
    const submissions = Array.isArray(team.submissions) ? team.submissions : [];
    const perTrack = tracks.map((track) => {
      const submission = submissions.find((s) => s.track === track.id && s.status !== "withdrawn");
      if (!submission) {
        return data.submissionsOpen
          ? `<details class="rounded-md border border-border bg-background p-3"><summary class="cursor-pointer text-sm font-semibold text-foreground">Submit a project to ${escapeHtml(track.name)}</summary>${hackathonSubmissionForm(track, null)}</details>`
          : "";
      }
      const flags = (submission.reviewFlags || []).map((flag) =>
        `<li>${escapeHtml(HACKATHON_FLAG_LABELS[flag] || flag)}</li>`).join("");
      const placement = submission.placement
        ? `<span class="rounded-full bg-accent px-2 py-0.5 text-[10px] font-semibold text-accent-foreground">${submission.placement === "first" ? "1st place" : "2nd place"}</span>`
        : "";
      return `<div class="rounded-md border border-border bg-background p-3">
        <div class="flex flex-wrap items-center gap-2">
          <p class="min-w-0 flex-1 text-sm font-semibold text-foreground">${escapeHtml(submission.title)} <span class="font-normal text-muted-foreground">· ${escapeHtml(track.name)}</span></p>
          ${placement}
          <span class="rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold ${submission.status === "valid" ? "text-accent" : "text-muted-foreground"}">${escapeHtml(HACKATHON_STATUS_LABELS[submission.status] || submission.status)}</span>
        </div>
        <p class="mt-1 font-mono text-xs text-muted-foreground">${escapeHtml(submission.repo)}</p>
        ${flags ? `<ul class="mt-2 list-disc pl-4 text-xs text-muted-foreground">${flags}</ul>` : ""}
        ${data.submissionsOpen ? `<details class="mt-2"><summary class="cursor-pointer text-xs font-semibold text-foreground">Edit</summary>${hackathonSubmissionForm(track, submission)}</details>
          <button type="button" data-hackathon-withdraw="${escapeHtml(submission.id)}" class="mt-2 text-xs font-semibold text-muted-foreground hover:text-foreground underline">Withdraw</button>` : ""}
      </div>`;
    }).join("");
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Your team · ${escapeHtml(team.name)}</h2>
        ${data.teamsOpen ? '<button type="button" data-hackathon-team-leave class="text-xs font-semibold text-muted-foreground hover:text-foreground underline">Leave team</button>' : ""}
      </div>
      <p class="mt-2 text-xs text-muted-foreground">${escapeHtml(`${team.members.length} of 5: ${team.members.join(", ")}`)}${team.full ? "" : ` · Invite code <button type="button" data-hackathon-copy-code="${escapeHtml(team.code)}" class="font-mono font-semibold text-foreground underline">${escapeHtml(team.code)}</button>`}</p>
      <div class="mt-3 grid gap-2">${perTrack || '<p class="text-xs text-muted-foreground">Submissions are closed.</p>'}</div>
      <p data-hackathon-team-status class="mt-2 text-xs text-muted-foreground"></p>
    </div>`;
  }

  // Judging and Community Choice load on demand: most participants are not
  // judges, and the gallery is only worth fetching when voting is open.
  function hackathonJudgingHtml(data) {
    if (!data.judge) return "";
    const view = state.hackathonView.judging;
    let body = '<button type="button" data-hackathon-judging-load class="rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary">Open the judging queue</button>';
    if (view) {
      const criteria = view.criteria || [];
      body = (view.projects || []).map((project) => {
        const score = project.yourScore || {};
        const selects = criteria.map((c) => `<label class="grid gap-1 text-[11px] text-muted-foreground">${escapeHtml(c.label)}
          <select name="${escapeHtml(c.key)}" class="rounded-md border border-border bg-background px-1.5 py-1 text-sm text-foreground">${[1, 2, 3, 4, 5].map((n) => `<option value="${n}"${Number(score[c.key]) === n ? " selected" : ""}>${n}</option>`).join("")}</select>
        </label>`).join("");
        return `<form data-hackathon-score="${escapeHtml(project.id)}" class="rounded-md border border-border bg-background p-3">
          <p class="text-sm font-semibold text-foreground">${escapeHtml(project.title)} <span class="font-normal text-muted-foreground">· ${escapeHtml(project.trackName || project.track)} · ${escapeHtml(project.team)}</span></p>
          <p class="mt-1 font-mono text-xs text-muted-foreground">${escapeHtml(project.repo)} · ${escapeHtml(HACKATHON_STATUS_LABELS[project.status] || project.status)} · ${escapeHtml(`${project.judges} judge${project.judges === 1 ? "" : "s"}`)}</p>
          ${project.pitch ? `<p class="mt-1 text-xs text-foreground">${escapeHtml(project.pitch)}</p>` : ""}
          <div class="mt-2 grid grid-cols-2 gap-2 sm:grid-cols-5">${selects}
            <button type="submit" class="self-end rounded-md bg-primary px-3 py-1.5 text-xs font-semibold text-primary-foreground hover:opacity-90"${view.open ? "" : " disabled"}>${project.yourScore ? "Update" : "Score"}</button>
          </div>
        </form>`;
      }).join("") || '<p class="text-xs text-muted-foreground">No projects to judge yet.</p>';
      if (!view.open) body = `<p class="mb-2 text-xs text-muted-foreground">Scoring opens once submissions close.</p>${body}`;
    }
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Judging</h2>
      <p class="mt-2 text-xs text-muted-foreground">Score each project from 1 to 5 on four criteria. You can revise a score; projects your own team submitted are not yours to judge.</p>
      <div class="mt-3 grid gap-2">${body}</div>
    </div>`;
  }

  function hackathonVotingHtml(data) {
    if (!data.votingOpen) return "";
    const gallery = state.hackathonView.gallery;
    let body = '<button type="button" data-hackathon-gallery-load class="rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary">Show the projects</button>';
    if (gallery) {
      const ownTeam = data.team?.id || "";
      body = (gallery.projects || []).map((project) => `<div class="flex items-center gap-3 rounded-md border border-border bg-background px-3 py-2">
          <div class="min-w-0 flex-1">
            <p class="truncate text-sm font-semibold text-foreground">${escapeHtml(project.title)} <span class="font-normal text-muted-foreground">· ${escapeHtml(project.trackName || project.track)}</span></p>
            <p class="truncate text-xs text-muted-foreground">${escapeHtml(project.team)} · ${escapeHtml(`${formatCount(project.votes || 0)} votes`)}</p>
          </div>
          <button type="button" data-hackathon-vote="${escapeHtml(project.id)}" class="shrink-0 rounded-md border border-border px-2.5 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary"${ownTeam && data.team?.submissions?.some((s) => s.id === project.id) ? " disabled" : ""}>Vote</button>
        </div>`).join("") || '<p class="text-xs text-muted-foreground">No valid projects yet.</p>';
    }
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Community Choice</h2>
      <p class="mt-2 text-xs text-muted-foreground">One vote per track, and you can move it until judging closes. Verified participants only, and not for your own team's project.</p>
      <div class="mt-3 grid gap-2">${body}</div>
      <p data-hackathon-vote-status class="mt-2 text-xs text-muted-foreground"></p>
    </div>`;
  }

  // Community milestones: the whole campaign's shared unlocks. This panel is
  // where an unlock is announced in the product, because it moves the moment
  // the counter behind it does.
  function hackathonMilestonesHtml(data) {
    const milestones = Array.isArray(data.milestones) ? data.milestones : [];
    if (!milestones.length) return "";
    const rows = milestones.map((entry) => `<li class="rounded-md border border-border bg-background p-3">
        <div class="flex items-center justify-between gap-3">
          <p class="min-w-0 text-sm font-semibold text-foreground">${escapeHtml(entry.label || "")}</p>
          <span class="shrink-0 rounded-full ${entry.unlocked ? "bg-accent text-accent-foreground" : "border border-border text-muted-foreground"} px-2 py-0.5 text-[10px] font-semibold">${entry.unlocked ? "Unlocked" : `${formatCount(entry.value)} / ${formatCount(entry.threshold)}`}</span>
        </div>
        ${entry.unlocked ? "" : hackathonMeter(entry.percent)}
        ${entry.description ? `<p class="mt-2 text-xs text-muted-foreground">${escapeHtml(entry.description)}</p>` : ""}
      </li>`).join("");
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Community milestones</h2>
      <p class="mt-2 text-xs text-muted-foreground">Unlocks for everyone, reached by everyone who joins and builds.</p>
      <ul class="mt-3 grid gap-2">${rows}</ul>
    </div>`;
  }

  // The street team. Applying is a short pitch; an administrator decides. An
  // approved ambassador gets their own funnel and a printable poster carrying
  // their invite link as a QR code, for places they have permission to post.
  function hackathonAmbassadorHtml(data) {
    const ambassador = data.ambassador;
    const you = data.you || {};
    const applyForm = `<form data-hackathon-ambassador-apply class="mt-3 grid gap-2">
        <label class="grid gap-1 text-xs text-muted-foreground">Where would you bring builders from? (at least 20 characters)
          <textarea name="pitch" minlength="20" maxlength="400" rows="2" required class="rounded-md border border-border bg-background px-2.5 py-1.5 text-sm text-foreground focus:outline-none focus:ring-1 focus:ring-foreground/20">${escapeHtml(ambassador?.pitch || "")}</textarea>
        </label>
        <div class="flex items-center gap-2">
          <button type="submit" class="rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary">Apply</button>
          <span data-hackathon-ambassador-status class="text-xs text-muted-foreground"></span>
        </div>
      </form>`;
    let body;
    if (!ambassador) {
      if (!you.verified) return "";
      body = `<p class="mt-2 text-xs text-muted-foreground">Ambassadors run a local street team: a meetup, a university club, a community you already belong to. You get a poster with your QR code, your own numbers and the Ambassador badge.</p>${applyForm}`;
    } else if (ambassador.status === "pending") {
      body = '<p class="mt-2 text-sm text-foreground">Application received. An administrator will review it.</p>';
    } else if (ambassador.status === "rejected") {
      body = `<p class="mt-2 text-sm text-foreground">This application was not accepted. You can update it and apply again.</p>${applyForm}`;
    } else {
      const s = ambassador.stats || {};
      const pct = (value) => `${Math.round((Number(value) || 0) * 100)}%`;
      body = `<div class="mt-3 grid gap-3 sm:grid-cols-4">
          ${hackathonStatTile("Link visits", formatCount(s.clicks || 0), "Counted since the campaign opened")}
          ${hackathonStatTile("Joined", formatCount(s.joins || 0), `${pct(s.joinRate)} of visits`)}
          ${hackathonStatTile("Qualified", formatCount(s.qualified || 0), `${pct(s.qualifiedRate)} of joins`)}
          ${hackathonStatTile("Status", "Ambassador", "Badge awarded")}
        </div>
        <div class="mt-3 flex flex-wrap gap-2">
          <button type="button" data-hackathon-poster-toggle class="rounded-md bg-primary px-3 py-1.5 text-xs font-semibold text-primary-foreground hover:opacity-90">Show my poster</button>
          <button type="button" data-hackathon-poster-print class="rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary">Print poster</button>
        </div>
        <div data-hackathon-poster hidden class="mt-3 rounded-lg border border-border bg-white p-8 text-center text-black">
          <p class="text-xs font-semibold uppercase tracking-widest">${escapeHtml(data.campaign?.title || "ForkMesh Hackathon")}</p>
          <p class="mt-3 text-3xl font-bold leading-tight">Build something that outlives its host.</p>
          <p class="mt-2 text-sm">Scan to join with ${escapeHtml(you.name || "")}</p>
          <div data-hackathon-poster-qr class="mt-4 flex justify-center"></div>
          <p class="mt-3 font-mono text-xs">${escapeHtml(data.share?.link || "")}</p>
          <p class="mt-4 text-[10px]">Post only where you have permission.</p>
        </div>`;
    }
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Street team</h2>
      ${body}
    </div>`;
  }

  // Printing isolates the poster. The rule is added once, only when somebody
  // actually opens a poster, so no other dashboard page carries print styles.
  function ensureHackathonPrintStyle() {
    if (document.getElementById("hackathon-poster-print")) return;
    const style = document.createElement("style");
    style.id = "hackathon-poster-print";
    style.textContent = "@media print{body *{visibility:hidden!important}"
      + "[data-hackathon-poster],[data-hackathon-poster] *{visibility:visible!important}"
      + "[data-hackathon-poster]{position:fixed;inset:0;border:0!important}}";
    document.head.append(style);
  }

  async function showHackathonPoster() {
    const poster = $("[data-hackathon-poster]");
    const qr = $("[data-hackathon-poster-qr]");
    if (!poster || !qr) return false;
    ensureHackathonPrintStyle();
    poster.hidden = false;
    if (!qr.childElementCount && await loadHackathonQr()) {
      try {
        qr.append(window.ForkMeshQR.toCanvas(String(state.hackathonView.data?.share?.link || ""), 320));
      } catch (_) {}
    }
    return true;
  }

  function hackathonSquadHtml(data) {
    const squad = data.squad;
    if (!squad) {
      return `<div class="px-4 sm:px-5 py-4 border-b border-border">
        <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Your squad</h2>
        <p class="mt-2 text-sm text-muted-foreground">Your squad starts the moment somebody joins through your link. Everyone you bring in, and everyone they bring in, is in it.</p>
      </div>`;
    }
    const goal = squad.goal;
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <div class="flex flex-wrap items-center justify-between gap-2">
        <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">${escapeHtml(squad.yours ? "Your squad" : `${squad.owner}'s squad`)}</h2>
        <a class="text-xs text-accent underline" href="/hackathon/squad/${escapeHtml(squad.code || "")}">Open squad page</a>
      </div>
      <p class="mt-2 text-sm text-foreground">${escapeHtml(`${squad.members} builder${squad.members === 1 ? "" : "s"} · ${Number(squad.points || 0).toLocaleString()} squad points`)}</p>
      ${goal ? `<p class="mt-1 text-xs text-muted-foreground">${escapeHtml(`${squad.members} of ${goal.size}: ${goal.label}`)}</p>${hackathonMeter(squad.percent)}` : `<p class="mt-1 text-xs text-muted-foreground">Every squad goal reached.</p>`}
    </div>`;
  }

  function hackathonTracksHtml(data) {
    const tracks = data.campaign?.tracks || [];
    if (!tracks.length) return "";
    const chosen = String(data.you?.track || "");
    const cards = tracks.map((track) => `<button
      data-hackathon-track="${escapeHtml(track.id)}"
      type="button"
      class="rounded-md border ${chosen === track.id ? "border-foreground/40 bg-secondary" : "border-border bg-background"} p-3 text-left hover:bg-secondary transition-colors"
    >
      <p class="text-sm font-semibold text-foreground">${chosen === track.id ? "&check; " : ""}${escapeHtml(track.name || track.id)}</p>
      <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(track.summary || "")}</p>
    </button>`).join("");
    return `<div class="px-4 sm:px-5 py-4 border-b border-border">
      <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Your track</h2>
      <div class="mt-3 grid gap-3 sm:grid-cols-3">${cards}</div>
    </div>`;
  }

  function hackathonJoinHtml(view) {
    const joinable = view.data?.campaign?.joinable;
    const name = hackathonAccountName();
    if (!name) {
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      return `<div class="px-4 sm:px-5 py-8">
        <p class="text-sm text-muted-foreground">
          <a class="text-accent underline" href="/login?next=${next}">Log in</a>
          or <a class="text-accent underline" href="/signup">create an account</a>
          to take part. The <a class="text-accent underline" href="/hackathon">hackathon page</a> and its leaderboards are public.
        </p>
      </div>`;
    }
    return `<div class="px-4 sm:px-5 py-8">
      <p class="text-sm text-foreground">You have not joined yet.</p>
      <p class="mt-1 text-sm text-muted-foreground">Joining gives you an invite link, a squad and a place on the boards.</p>
      <button
        data-hackathon-join
        type="button"
        ${joinable ? "" : "disabled"}
        class="mt-4 inline-flex items-center gap-1.5 rounded-md bg-primary px-3 py-2 text-xs font-semibold text-primary-foreground hover:opacity-90 transition-colors disabled:opacity-50"
      >${joinable ? "Join the hackathon" : "Registration is closed"}</button>
    </div>`;
  }

  function renderHackathonPage() {
    const body = $("[data-hackathon-body]");
    const rank = $("[data-hackathon-rank]");
    if (!body) return;
    const view = state.hackathonView;
    if (view.loading) {
      body.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading your hackathon standing...")}</div>`;
      if (rank) rank.textContent = "Loading";
      return;
    }
    if (view.error) {
      // Signed out: keep the whole address, ?accept=<token> included, so a
      // challenge somebody followed here is still waiting after they log in.
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      const login = view.errorStatus === 401
        ? ` <a class="text-accent underline" href="/login?next=${next}">Log in</a> or <a class="text-accent underline" href="/signup?next=${next}">create an account</a>.`
        : "";
      body.innerHTML = `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${escapeHtml(view.error)}${login}</div>`;
      if (rank) rank.textContent = "Unavailable";
      return;
    }
    const data = view.data || {};
    const title = $("[data-hackathon-title]");
    const theme = $("[data-hackathon-theme]");
    if (title && data.campaign?.title) title.textContent = data.campaign.title;
    if (theme && data.campaign?.theme) theme.textContent = data.campaign.theme;
    if (!data.joined) {
      body.innerHTML = hackathonJoinHtml(view);
      if (rank) rank.textContent = "Not joined";
      window.lucide?.createIcons();
      return;
    }
    const you = data.you || {};
    if (rank) rank.textContent = you.rank ? `#${formatCount(you.rank)}` : "Unranked";
    const notice = view.notice
      ? `<div class="px-4 sm:px-5 py-3 border-b border-border text-sm text-foreground bg-secondary">${escapeHtml(view.notice)}</div>`
      : "";
    body.innerHTML = [
      notice,
      hackathonOnboardingHtml(data),
      `<div class="px-4 sm:px-5 py-4 border-b border-border">
        <h2 class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Your standing</h2>
        <div class="mt-3 grid gap-3 sm:grid-cols-4">
          ${hackathonStatTile("Points", formatCount(you.points || 0), "Earned from building and bringing people in")}
          ${hackathonStatTile("Rank", you.rank ? `#${formatCount(you.rank)}` : "Unranked", `of ${formatCount(data.campaign?.participants || 0)} builders`)}
          ${hackathonStatTile("Friends joined", formatCount(you.referralsJoined || 0), "Verified their account")}
          ${hackathonStatTile("Qualified", formatCount(you.referralsQualified || 0), "Credited by the referral ledger")}
        </div>
      </div>`,
      hackathonShareHtml(data),
      hackathonMissionsHtml(data),
      hackathonRewardsHtml(data),
      hackathonTeamHtml(data),
      hackathonJudgingHtml(data),
      hackathonVotingHtml(data),
      hackathonChallengesHtml(data),
      hackathonSquadHtml(data),
      hackathonMilestonesHtml(data),
      hackathonAmbassadorHtml(data),
      hackathonTracksHtml(data),
      `<div class="px-4 sm:px-5 py-3">
        <p class="text-xs text-muted-foreground">A friend counts as joined once they verify their email. Anything with real value waits until the referral ledger credits them, which takes seven days of membership and sustained use from a proved device. The public boards are on the <a class="text-accent underline" href="/hackathon/leaderboard">leaderboard page</a>.</p>
      </div>`,
    ].join("");
    window.lucide?.createIcons();
  }

  async function loadHackathon(options = {}) {
    state.hackathonView.loading = true;
    state.hackathonView.error = "";
    renderHackathonPage();
    try {
      state.hackathonView.data = await fetchJson("/api/hackathon/me", {
        fresh: options.fresh === true,
      });
    } catch (error) {
      state.hackathonView.data = null;
      state.hackathonView.errorStatus = Number(error?.status) || 0;
      state.hackathonView.error = error?.status === 401
        ? "Log in to see your hackathon standing."
        : error?.status === 404
          ? "No hackathon is running right now."
          : "Your hackathon standing is unavailable right now.";
    } finally {
      state.hackathonView.loading = false;
      renderHackathonPage();
    }
  }

  async function hackathonPost(path, body) {
    const headers = { accept: "application/json", "content-type": "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const response = await fetch(path, {
      method: "POST",
      headers,
      cache: "no-store",
      credentials: "same-origin",
      body: JSON.stringify(body || {}),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      throw error;
    }
    return payload;
  }

  // Report one share press. Fire-and-forget on purpose: a participant's share
  // must never wait on, or be blocked by, our own measurement.
  function reportHackathonShare(channel, surface = "hq") {
    if (!channel) return;
    void hackathonPost("/api/hackathon/share", { channel, surface }).catch(() => {});
  }

  async function renderHackathonQr(link) {
    const root = $("[data-hackathon-qr]");
    if (!root) return;
    if (!root.hidden) {
      root.hidden = true;
      root.innerHTML = "";
      return;
    }
    root.innerHTML = "";
    root.hidden = false;
    // The generator is only worth fetching because somebody actually pressed
    // QR code; loadQrGenerator pulls it from this origin on first use.
    if (!(await loadQrGenerator())) {
      root.innerHTML = '<p class="text-xs text-muted-foreground">The QR generator could not be loaded. Your link is above and works on its own.</p>';
      return;
    }
    try {
      const canvas = window.ForkMeshQR.toCanvas(link, 220);
      canvas.className = "rounded-md border border-border bg-white p-2";
      root.append(canvas);
    } catch (_) {
      root.innerHTML = '<p class="text-xs text-muted-foreground">That link is too long for a QR code.</p>';
    }
  }

  function initHackathonPage() {
    const panel = $("[data-hackathon-panel]");
    panel?.addEventListener("click", async (event) => {
      if (event.target.closest("[data-hackathon-refresh]")) {
        void loadHackathon({ fresh: true });
        return;
      }
      const join = event.target.closest("[data-hackathon-join]");
      if (join) {
        join.disabled = true;
        join.textContent = "Joining...";
        try {
          state.hackathonView.data = await hackathonPost("/api/hackathon/join", {});
          state.hackathonView.error = "";
        } catch (_) {
          state.hackathonView.error = "Could not join right now. Please try again.";
        }
        renderHackathonPage();
        return;
      }
      if (event.target.closest("[data-hackathon-poster-toggle]")) {
        const poster = $("[data-hackathon-poster]");
        if (poster && !poster.hidden) {
          poster.hidden = true;
        } else {
          await showHackathonPoster();
        }
        return;
      }
      if (event.target.closest("[data-hackathon-poster-print]")) {
        if (await showHackathonPoster()) window.print();
        return;
      }
      const leave = event.target.closest("[data-hackathon-team-leave]");
      if (leave) {
        if (!window.confirm("Leave this team?")) return;
        try {
          await hackathonPost("/api/hackathon/teams/leave", {});
          await loadHackathon({ fresh: true });
        } catch (error) {
          const status = $("[data-hackathon-team-status]");
          if (status) status.textContent = error?.message === "withdraw_submissions_first"
            ? "Withdraw your team's submissions before the last member leaves."
            : "Could not leave the team.";
        }
        return;
      }
      const codeCopy = event.target.closest("[data-hackathon-copy-code]");
      if (codeCopy) {
        if (await copyTextToClipboard(codeCopy.dataset.hackathonCopyCode || "")) {
          codeCopy.textContent = "copied";
        }
        return;
      }
      const withdraw = event.target.closest("[data-hackathon-withdraw]");
      if (withdraw) {
        if (!window.confirm("Withdraw this submission? You can resubmit before the deadline.")) return;
        try {
          await hackathonPost(`/api/hackathon/submissions/${encodeURIComponent(withdraw.dataset.hackathonWithdraw)}/withdraw`, {});
        } catch (_) {}
        await loadHackathon({ fresh: true });
        return;
      }
      if (event.target.closest("[data-hackathon-judging-load]")) {
        try {
          state.hackathonView.judging = await fetchJson("/api/hackathon/judging", { fresh: true });
        } catch (_) {
          state.hackathonView.judging = { projects: [], criteria: [], open: false };
        }
        renderHackathonPage();
        return;
      }
      if (event.target.closest("[data-hackathon-gallery-load]")) {
        try {
          state.hackathonView.gallery = await fetchJson("/api/hackathon/projects", { fresh: true });
        } catch (_) {
          state.hackathonView.gallery = { projects: [] };
        }
        renderHackathonPage();
        return;
      }
      const vote = event.target.closest("[data-hackathon-vote]");
      if (vote) {
        const status = $("[data-hackathon-vote-status]");
        try {
          const result = await hackathonPost(`/api/hackathon/projects/${encodeURIComponent(vote.dataset.hackathonVote)}/vote`, {});
          if (status) status.textContent = result.moved ? "Vote moved." : "Vote counted.";
          state.hackathonView.gallery = await fetchJson("/api/hackathon/projects", { fresh: true });
          renderHackathonPage();
        } catch (error) {
          if (status) status.textContent = error?.message === "verified_participants_only"
            ? "Verify your email to vote."
            : error?.message === "own_project" ? "You cannot vote for your own team's project."
              : "Could not record the vote.";
        }
        return;
      }
      const newChallenge = event.target.closest("[data-hackathon-challenge-new]");
      if (newChallenge) {
        newChallenge.disabled = true;
        try {
          const created = await hackathonPost("/api/hackathon/challenges", {});
          const newest = created.sent?.[0]?.link || "";
          if (newest && await copyTextToClipboard(newest)) {
            reportHackathonShare("copy", "challenge");
          }
          await loadHackathon({ fresh: true });
        } catch (error) {
          newChallenge.disabled = false;
          newChallenge.textContent = error?.status === 429
            ? "Open challenge limit reached"
            : "Could not create a challenge";
        }
        return;
      }
      const challengeCopy = event.target.closest("[data-hackathon-challenge-copy]");
      if (challengeCopy) {
        if (await copyTextToClipboard(challengeCopy.dataset.hackathonChallengeCopy || "")) {
          challengeCopy.textContent = "Copied";
          reportHackathonShare("copy", "challenge");
        }
        return;
      }
      const track = event.target.closest("[data-hackathon-track]");
      if (track) {
        const id = track.dataset.hackathonTrack;
        try {
          state.hackathonView.data = await hackathonPost("/api/hackathon/me", { track: id });
        } catch (_) {}
        renderHackathonPage();
        return;
      }
      const copy = event.target.closest("[data-hackathon-copy]");
      if (copy) {
        const input = $("[data-hackathon-link]");
        const value = input?.value || "";
        const copied = await copyTextToClipboard(value);
        copy.innerHTML = copied
          ? '<i data-lucide="check" class="w-3.5 h-3.5"></i>Copied'
          : '<i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy failed';
        window.lucide?.createIcons();
        if (copied) reportHackathonShare("copy");
        window.setTimeout(() => {
          if (!document.body.contains(copy)) return;
          copy.innerHTML = '<i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy link';
          window.lucide?.createIcons();
        }, 1600);
        return;
      }
      const share = event.target.closest("[data-hackathon-share]");
      if (!share) return;
      const channel = share.dataset.hackathonShare;
      const data = state.hackathonView.data || {};
      const link = String(data.share?.link || "");
      const text = String(data.share?.text || "");
      if (channel === "qr") {
        void renderHackathonQr(link);
        reportHackathonShare("qr");
        return;
      }
      if (channel === "story") {
        share.disabled = true;
        const shared = await shareHackathonStory(data);
        share.disabled = false;
        if (shared) reportHackathonShare("story");
        return;
      }
      if (channel === "copy" || channel === "discord") {
        // Discord has no share intent URL, so the honest affordance is a
        // ready-to-paste message rather than a link that opens the wrong app.
        if (await copyTextToClipboard(`${text}\n${link}`)) {
          reportHackathonShare(channel);
        }
        return;
      }
      if (channel === "mastodon") {
        // The instance is the visitor's own, so it has to be asked for; the
        // server validates the hostname before any link is built from it.
        const instance = window.prompt("Your Mastodon instance", "mastodon.social");
        if (!instance) return;
        const host = String(instance).trim().replace(/^https?:\/\//, "").replace(/\/.*$/, "");
        if (!/^[a-z0-9.-]+\.[a-z]{2,}$/i.test(host)) return;
        window.open(
          `https://${host}/share?text=${encodeURIComponent(`${text}\n\n${link}`)}`,
          "_blank", "noopener");
        reportHackathonShare("mastodon");
        return;
      }
      if (channel === "native") {
        if (navigator.share) {
          try {
            await navigator.share({ title: "ForkMesh Hackathon", text, url: link });
            reportHackathonShare("native");
          } catch (_) {}
        } else if (await copyTextToClipboard(`${text}\n${link}`)) {
          reportHackathonShare("copy");
        }
        return;
      }
      // Everything else is a real anchor the browser is already following.
      reportHackathonShare(channel);
    });
    panel?.addEventListener("submit", async (event) => {
      const form = event.target;
      if (!(form instanceof HTMLFormElement)) return;
      const values = Object.fromEntries(new FormData(form).entries());
      const teamStatus = $("[data-hackathon-team-status]");
      if (form.matches("[data-hackathon-team-create]") || form.matches("[data-hackathon-team-join]")) {
        event.preventDefault();
        const join = form.matches("[data-hackathon-team-join]");
        try {
          await hackathonPost(join ? "/api/hackathon/teams/join" : "/api/hackathon/teams",
            join ? { code: String(values.code || "").trim().toLowerCase() } : { name: values.name });
          await loadHackathon({ fresh: true });
        } catch (error) {
          const messages = {
            team_full: "That team already has five members.",
            team_not_found: "No team has that invite code.",
            invalid_team_code: "An invite code is eight letters and numbers.",
            invalid_team_name: "Team names are 2 to 40 characters.",
            already_on_a_team: "You are already on a team.",
          };
          if (teamStatus) teamStatus.textContent = messages[error?.message] || "Could not update your team.";
        }
        return;
      }
      if (form.matches("[data-hackathon-submission-form]")) {
        event.preventDefault();
        const status = form.querySelector("[data-hackathon-form-status]");
        if (status) status.textContent = "Saving...";
        try {
          const result = await hackathonPost("/api/hackathon/submissions", {
            track: form.dataset.track, repo: values.repo, title: values.title,
            pitch: values.pitch, demoUrl: values.demoUrl, videoUrl: values.videoUrl,
          });
          if (status) status.textContent = result.status === "valid" ? "Saved." : "Saved. A judge will review it.";
          await loadHackathon({ fresh: true });
        } catch (error) {
          const messages = {
            invalid_repository: "Name the repository as owner/name.",
            invalid_title: "Titles are 3 to 80 characters.",
            pitch_too_long: "The pitch is limited to 280 characters.",
            invalid_demo_url: "The demo link must start with https://.",
            invalid_video_url: "The video link must start with https://.",
            submissions_closed: "Submissions are closed.",
          };
          if (status) status.textContent = messages[error?.message] || "Could not save the submission.";
        }
        return;
      }
      if (form.matches("[data-hackathon-ambassador-apply]")) {
        event.preventDefault();
        const status = form.querySelector("[data-hackathon-ambassador-status]");
        try {
          await hackathonPost("/api/hackathon/ambassadors", { pitch: values.pitch });
          await loadHackathon({ fresh: true });
        } catch (error) {
          if (status) status.textContent = error?.message === "pitch_too_short"
            ? "Tell us a little more: at least 20 characters."
            : error?.message === "verify_email_first"
              ? "Verify your email before applying."
              : "Could not send the application.";
        }
        return;
      }
      if (form.matches("[data-hackathon-score]")) {
        event.preventDefault();
        try {
          await hackathonPost(`/api/hackathon/judging/${encodeURIComponent(form.dataset.hackathonScore)}`, values);
          state.hackathonView.judging = await fetchJson("/api/hackathon/judging", { fresh: true });
          renderHackathonPage();
        } catch (_) {
          form.querySelector("button[type=submit]")?.replaceChildren("Could not save");
        }
      }
    });
    void acceptPendingHackathonChallenge();
  }

  // A challenge link sends people here with ?accept=<token>. Accept it once
  // the page knows who is signed in, then drop the token from the address so
  // a refresh or a copied URL cannot accept it again on somebody else's
  // behalf.
  async function acceptPendingHackathonChallenge() {
    let token = "";
    try {
      token = new URLSearchParams(location.search).get("accept") || "";
    } catch (_) {}
    // Without a session the token stays in the address; the log-in link the
    // page renders carries it through, and this runs again on return.
    if (!/^[A-Za-z0-9_-]{16,40}$/.test(token) || !hackathonAccountName()) {
      await loadHackathon();
      return;
    }
    try {
      const url = new URL(location.href);
      url.searchParams.delete("accept");
      history.replaceState(null, "", url.pathname + url.search + url.hash);
    } catch (_) {}
    try {
      await hackathonPost(`/api/hackathon/challenges/${encodeURIComponent(token)}/accept`, {});
    } catch (error) {
      state.hackathonView.notice = error?.status === 410
        ? "That challenge has closed."
        : error?.status === 409
          ? "That challenge could not be accepted."
          : "";
    }
    await loadHackathon({ fresh: true });
  }
