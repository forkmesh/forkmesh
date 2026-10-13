// Live data for the public hackathon subpages (/hackathon/leaderboard,
// /hackathon/projects and /hackathon/rules).
//
// Everything the page needs is already in the markup as a truthful default:
// the tracks, the prize table, the steps and the FAQ are static content, and
// this script only replaces the parts that genuinely move -- the campaign
// name and dates, the community milestone meter, and the two public boards.
// That way the page is complete and readable before this file runs, and a
// failed fetch degrades to the static copy rather than to an empty shell.
(() => {
  "use strict";

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => Array.from(root.querySelectorAll(selector));

  function count(value) {
    return Number(value || 0).toLocaleString();
  }

  // Campaign dates are whole days in the visitor's own locale. The Worker
  // stores epoch milliseconds, so an absent value simply yields nothing
  // rather than 1 January 1970.
  function dateRange(startsAt, endsAt) {
    const format = (value) => {
      const ms = Number(value || 0);
      if (!ms) return "";
      try {
        return new Date(ms).toLocaleDateString(undefined, {
          month: "short",
          day: "numeric",
        });
      } catch (_) {
        return "";
      }
    };
    const start = format(startsAt);
    const end = format(endsAt);
    if (start && end) return `${start} – ${end}`;
    return start || end || "";
  }

  async function getJson(path) {
    const response = await fetch(path, {
      headers: { accept: "application/json" },
      credentials: "omit",
    });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return response.json();
  }

  // A signed-in visitor goes straight to their HQ; everyone else needs an
  // account first. Reading the same forkmesh.session marker the site header
  // and homepage use keeps this page's answer consistent with theirs.
  function signedIn() {
    try {
      const session = JSON.parse(localStorage.getItem("forkmesh.session") || "null");
      if (!session || !session.nodeName || session.kind === "node") return false;
      return session.kind === "user" || Boolean(session.email);
    } catch (_) {
      return false;
    }
  }

  // Three states, and the page must be honest in all of them: a campaign
  // that is open, one that has closed, and no published campaign at all --
  // which is what a visitor sees while the campaign is still a draft, and
  // must never be dressed up as open registration.
  function applyJoinTargets(campaign, resolved) {
    if (resolved && !campaign) {
      $$("[data-hk-join]").forEach((link) => {
        link.textContent = "Dates coming soon";
        link.setAttribute("aria-disabled", "true");
        link.href = "/hackathon#how";
      });
      const announce = $("[data-hk-announce]");
      if (announce) {
        announce.textContent = "Dates and prizes are being finalised. The tracks below are set.";
      }
      const dates = $("[data-hk-dates]");
      if (dates) dates.textContent = "Dates coming soon";
      return;
    }
    const open = campaign ? campaign.joinable !== false : true;
    $$("[data-hk-join]").forEach((link) => {
      if (!open) {
        link.textContent = campaign && campaign.status === "closed"
          ? "This hackathon has closed"
          : "Registration is closed";
        link.setAttribute("aria-disabled", "true");
        link.href = "/hackathon/leaderboard";
        return;
      }
      // Signing up carries the destination, so a new account lands in the
      // hackathon rather than on a generic dashboard.
      link.href = signedIn()
        ? "/hackathon/me"
        : "/signup?next=" + encodeURIComponent("/hackathon/me");
    });
  }

  function applyMilestone(campaign) {
    const card = $("[data-hk-meter]");
    if (!card) return;
    const milestone = campaign && campaign.nextMilestone;
    if (!milestone) {
      // Either every milestone is unlocked or none is configured. Showing an
      // empty meter would be worse than showing none.
      card.hidden = true;
      return;
    }
    const value = Number(milestone.value || 0);
    const threshold = Number(milestone.threshold || 0) || 1;
    const percent = Math.max(0, Math.min(100, Math.round((value * 100) / threshold)));
    $("[data-hk-meter-label]").textContent = milestone.label || "Next community unlock";
    $("[data-hk-meter-count]").textContent = `${count(value)} / ${count(threshold)}`;
    $("[data-hk-meter-fill]").style.width = `${percent}%`;
    $("[data-hk-meter-note]").textContent = milestone.description
      || "Every builder who joins moves this meter for everybody.";
    card.hidden = false;
  }

  function applyCampaign(campaign) {
    applyJoinTargets(campaign, true);
    if (!campaign) return;
    const name = $("[data-hk-campaign]");
    if (name && campaign.title) name.textContent = campaign.title;
    const theme = $("[data-hk-theme]");
    if (theme && campaign.theme) theme.textContent = campaign.theme;
    const dates = $("[data-hk-dates]");
    const range = dateRange(campaign.startsAt, campaign.endsAt);
    if (dates && range) dates.textContent = range;
    const announce = $("[data-hk-announce]");
    if (announce && campaign.participants) {
      announce.textContent =
        `${count(campaign.participants)} builders have joined. Every valid submission earns Pro.`;
    }
    applyMilestone(campaign);

    // The tracks in the markup are the published default. Replace them only
    // when the campaign actually configures its own, so an unreachable API
    // leaves three real tracks on the page instead of none.
    const trackRoot = $("[data-hk-tracks]");
    if (trackRoot && Array.isArray(campaign.tracks) && campaign.tracks.length) {
      trackRoot.replaceChildren(...campaign.tracks.map((track) => {
        const card = document.createElement("article");
        card.className = "card";
        const heading = document.createElement("h3");
        heading.textContent = track.name || track.id || "";
        const body = document.createElement("p");
        body.textContent = track.summary || "";
        card.append(heading, body);
        return card;
      }));
    }
  }

  function boardRow(rank, who, score) {
    const row = document.createElement("div");
    row.className = "board-row";
    const rankCell = document.createElement("span");
    rankCell.className = "rank";
    rankCell.textContent = `${rank}`;
    const whoCell = document.createElement("span");
    whoCell.className = "who";
    // Built as text, never innerHTML: these are other people's usernames.
    whoCell.textContent = who;
    const scoreCell = document.createElement("span");
    scoreCell.className = "score";
    scoreCell.textContent = score;
    row.append(rankCell, whoCell, scoreCell);
    return row;
  }

  function renderBoard(root, board, rows, limit) {
    if (!rows.length) {
      root.replaceChildren(Object.assign(document.createElement("p"), {
        className: "board-empty",
        textContent: board === "recruiters"
          ? "Nobody has brought a qualified member in yet. The first one tops this board."
          : "No points scored yet. The first builder to join takes the top spot.",
      }));
      return;
    }
    root.replaceChildren(...rows.slice(0, limit).map((row, index) => {
      if (board === "recruiters") {
        const pending = Number(row.joined || 0) - Number(row.qualified || 0);
        const score = pending > 0
          ? `${count(row.qualified)} qualified · ${count(pending)} pending`
          : `${count(row.qualified)} qualified`;
        return boardRow(index + 1, row.name || "", score);
      }
      if (board === "squads") {
        return boardRow(
          index + 1,
          `${row.name || ""}'s squad`,
          `${count(row.members)} builders · ${count(row.points)} pts`,
        );
      }
      return boardRow(index + 1, row.name || "", `${count(row.points)} pts`);
    }));
  }

  async function loadBoards() {
    const roots = $$("[data-hk-board]");
    if (!roots.length) return;
    await Promise.all(roots.map(async (root) => {
      const board = root.dataset.hkBoard;
      const limit = Number(root.dataset.hkLimit || 5);
      try {
        const data = await getJson(`/api/hackathon/leaderboards/${board}`);
        renderBoard(root, board, Array.isArray(data.rows) ? data.rows : [], limit);
      } catch (_) {
        root.replaceChildren(Object.assign(document.createElement("p"), {
          className: "board-empty",
          textContent: "This board is unavailable right now.",
        }));
      }
    }));
  }

  // The project gallery. Every field is somebody else's words, so the cards
  // are built as text nodes, and links are only ever the https URLs the
  // Worker already validated, marked ugc so a gallery listing passes no
  // ranking to whatever a submitter linked.
  function projectCard(project) {
    const card = document.createElement("article");
    card.className = "project";
    if (project.placement) {
      const badge = document.createElement("span");
      badge.className = "badge";
      badge.textContent = project.placement === "first" ? "1st place" : "2nd place";
      card.append(badge);
    }
    const heading = document.createElement("h3");
    const link = document.createElement("a");
    link.href = `/hackathon/projects/${encodeURIComponent(project.id || "")}`;
    link.textContent = project.title || "";
    heading.append(link);
    const meta = document.createElement("p");
    meta.className = "meta";
    const votes = Number.isFinite(Number(project.votes)) && project.votes !== undefined
      ? ` · ${count(project.votes)} votes` : "";
    meta.textContent = `${project.trackName || project.track || ""} · ${project.team || ""}${votes}`;
    card.append(heading, meta);
    if (project.pitch) {
      const pitch = document.createElement("p");
      pitch.className = "pitch";
      pitch.textContent = project.pitch;
      card.append(pitch);
    }
    const repo = document.createElement("a");
    repo.className = "repo";
    repo.href = `/${String(project.repo || "").split("/").map(encodeURIComponent).join("/")}`;
    repo.textContent = project.repo || "";
    card.append(repo);
    return card;
  }

  async function loadGallery() {
    const root = $("[data-hk-gallery]");
    if (!root) return;
    let data;
    try {
      data = await getJson("/api/hackathon/projects");
    } catch (_) {
      root.replaceChildren(Object.assign(document.createElement("p"), {
        className: "board-empty",
        textContent: "The gallery is unavailable right now.",
      }));
      return;
    }
    const projects = Array.isArray(data.projects) ? data.projects : [];
    if (!projects.length) {
      root.replaceChildren(Object.assign(document.createElement("p"), {
        className: "board-empty",
        textContent: "No projects yet. Valid submissions appear here as soon as they are made.",
      }));
      return;
    }
    const filters = $("[data-hk-gallery-filters]");
    const tracks = [...new Map(projects.map((p) => [p.track, p.trackName || p.track])).entries()];
    const show = (track) => {
      root.replaceChildren(...projects
        .filter((project) => !track || project.track === track)
        .map(projectCard));
      filters?.querySelectorAll("button").forEach((button) => {
        button.setAttribute("aria-pressed", String((button.dataset.track || "") === (track || "")));
      });
    };
    if (filters && tracks.length > 1) {
      const all = Object.assign(document.createElement("button"), {
        type: "button", textContent: "All tracks",
      });
      all.dataset.track = "";
      filters.replaceChildren(all, ...tracks.map(([id, name]) => {
        const button = Object.assign(document.createElement("button"), {
          type: "button", textContent: name,
        });
        button.dataset.track = id;
        return button;
      }));
      filters.addEventListener("click", (event) => {
        const button = event.target.closest("button");
        if (button) show(button.dataset.track || "");
      });
      filters.hidden = false;
    }
    show("");
  }

  // Pill navigation: scrolled state and the mobile panel. The hackathon
  // pages have flat navigation rather than the homepage's dropdowns, so this
  // is the whole of it.
  function initNav() {
    const nav = $("#site-nav");
    if (!nav) return;
    const toggle = $("#mobile-menu-toggle");
    const updateNav = () => nav.classList.toggle("is-scrolled", window.scrollY > 8);
    updateNav();
    window.addEventListener("scroll", updateNav, { passive: true });
    const setOpen = (open) => {
      nav.dataset.open = String(open);
      toggle?.setAttribute("aria-expanded", String(open));
      toggle?.setAttribute("aria-label", open ? "Close menu" : "Open menu");
    };
    toggle?.addEventListener("click", () => setOpen(nav.dataset.open !== "true"));
    nav.querySelectorAll("a").forEach((link) => {
      link.addEventListener("click", () => setOpen(false));
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && nav.dataset.open === "true") {
        setOpen(false);
        toggle?.focus();
      }
    });
  }

  async function start() {
    initNav();
    // Before the answer arrives, leave the markup's own wording alone: it is
    // accurate static copy, and rewriting it twice would flash.
    try {
      const data = await getJson("/api/hackathon");
      applyCampaign(data.campaign || null);
    } catch (_) {
      // The static copy stays -- it is accurate, just not live -- but the
      // call to action must not. If we cannot reach the campaign we cannot
      // claim registration is open, and pointing somebody at a join page we
      // could not confirm exists is the one failure worth avoiding here.
      applyJoinTargets(null, true);
    }
    await loadBoards();
    await loadGallery();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", start, { once: true });
  } else {
    void start();
  }
})();
