(() => {
  const WORLD_EMBED_URL = "https://world.forkmesh.com/world/";
  const WORLD_EMBED_FRAME_ORIGIN = new URL(WORLD_EMBED_URL).origin;
  const WORLD_EMBED_ORIGINS = new Set([
    "https://forkmesh.com",
    "https://www.forkmesh.com",
  ]);
  const WORLD_EMBED_MESSAGE_SOURCE = "forkmesh-site";
  // Start the world while the band is still a screen or so away, so it has
  // time to boot before its first pixel lands without competing with the
  // content above it during the page's own load.
  const WORLD_EMBED_PRELOAD_PX = 600;
  const WORLD_EMBED_PRELOAD_MARGIN = `${WORLD_EMBED_PRELOAD_PX}px 0px`;
  // A frame that never answers still has to become clickable. The world's own
  // watchdog gives up at ten seconds and paints a fallback with links on it.
  const WORLD_EMBED_READY_TIMEOUT_MS = 12000;
  const FOOTER_COLUMNS = [
    {
      title: "Explore",
      links: [
        { href: "/features", label: "Features" },
        { href: "/network", label: "Network", plugin: "network" },
        { href: "/desktop", label: "Desktop app" },
        { href: "/mobile", label: "Mobile app" },
        { href: "/market", label: "Market" },
        { href: "/ecosystem", label: "Ecosystem" },
      ],
    },
    {
      title: "Company",
      links: [
        { href: "/about", label: "About" },
        { href: "/press", label: "Press" },
        { href: "/careers", label: "Careers" },
        { href: "/changelog", label: "Changelog" },
        { href: "/terms", label: "Terms" },
        { href: "/privacy", label: "Privacy" },
      ],
    },
    {
      title: "Plans",
      links: [
        { href: "/#pricing", label: "Pricing" },
        { href: "/signup", label: "Create account" },
        { href: "/login", label: "Log In" },
        { href: "/mirror-payouts", label: "Mirror payouts" },
      ],
    },
    {
      title: "Learn",
      links: [
        { href: "/#faq", label: "FAQ" },
        { href: "/blog", label: "Blog" },
        { href: "/docs", label: "Documentation" },
        { href: "/security-report", label: "Security report" },
      ],
    },
    {
      title: "Community",
      links: [
        { href: "/status", label: "System status", statusEntry: true },
        { href: "/ledger", label: "Financial ledger", plugin: "ledger" },
        { href: "/chat", label: "Community chat", plugin: "chat" },
        { href: "/microjobs", label: "Microjobs" },
        { href: "/hackathon", label: "Hackathon" },
        { href: "/referral", label: "Referral program" },
        {
          href: "https://reddit.com/r/forkmesh",
          label: "Reddit",
          external: true,
        },
        {
          href: "https://x.com/forkmesh",
          label: "X (Twitter)",
          external: true,
        },
      ],
    },
  ];

  const SOCIAL_LINKS = [
    {
      href: "https://x.com/forkmesh",
      label: "X (Twitter)",
      external: true,
      icon:
        '<path d="M18.244 2.25h3.308l-7.227 8.26 8.502 11.24H16.17l-5.214-6.817L4.99 21.75H1.68l7.73-8.835L1.254 2.25H8.08l4.713 6.231zm-1.161 17.52h1.833L7.084 4.126H5.117z"></path>',
    },
    {
      href: "https://reddit.com/r/forkmesh",
      label: "Reddit",
      external: true,
      icon:
        '<path d="M12 0A12 12 0 0 0 0 12a12 12 0 0 0 12 12 12 12 0 0 0 12-12A12 12 0 0 0 12 0zm5.01 4.744c.688 0 1.25.561 1.25 1.249a1.25 1.25 0 0 1-2.498.056l-2.597-.547-.8 3.747c1.824.07 3.48.632 4.674 1.488.308-.309.73-.491 1.207-.491.968 0 1.754.786 1.754 1.754 0 .716-.435 1.333-1.01 1.614a3.111 3.111 0 0 1 .042.52c0 2.694-3.13 4.87-7.004 4.87-3.874 0-7.004-2.176-7.004-4.87 0-.183.015-.366.043-.534A1.748 1.748 0 0 1 4.028 12c0-.968.786-1.754 1.754-1.754.463 0 .898.196 1.207.49 1.207-.883 2.878-1.43 4.744-1.487l.885-4.182a.342.342 0 0 1 .14-.197.35.35 0 0 1 .238-.042l2.906.617a1.214 1.214 0 0 1 1.108-.701z"></path>',
    },
    {
      href: "/chat",
      label: "Chat",
      plugin: "chat",
      icon:
        '<path d="M4.804 21.644A6.707 6.707 0 0 0 6 21.75a6.721 6.721 0 0 0 3.583-1.029c.774.182 1.584.279 2.417.279 5.322 0 9.75-3.97 9.75-9 0-5.03-4.428-9-9.75-9s-9.75 3.97-9.75 9c0 2.409 1.025 4.587 2.674 6.192.232.226.277.428.254.543a3.73 3.73 0 0 1-.814 1.686.75.75 0 0 0 .44 1.223Z"></path>',
    },
  ];

  function attrsForLink(link) {
    return link.external ? ' target="_blank" rel="noopener noreferrer"' : "";
  }

  function pluginAttr(link) {
    return link.plugin ? ` data-plugin-entry="${link.plugin}"` : "";
  }

  function renderColumn(column) {
    return `
          <div>
            <h3 class="site-footer-column-title">${column.title}</h3>
            <ul class="site-footer-link-list">
              ${column.links
                .map(
                  (link) => `
              <li${link.statusEntry ? " data-status-entry" : ""}${pluginAttr(link)}><a class="site-footer-column-link" href="${link.href}"${attrsForLink(link)}>${link.label}</a></li>`,
                )
                .join("")}
            </ul>
          </div>`;
  }

  function renderSocialLink(link) {
    return `
              <a
                href="${link.href}"
                aria-label="${link.label}"
                class="site-footer-social-link"${pluginAttr(link)}
                ${attrsForLink(link)}
              >
                <svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
                  ${link.icon}
                </svg>
              </a>`;
  }

  function worldFrameMarkup() {
    return `
        <iframe
          src="${WORLD_EMBED_URL}"
          title="Interactive ForkMesh World"
          loading="lazy"
          referrerpolicy="same-origin"
          allow="fullscreen"
          class="site-footer-world-frame"
        ></iframe>`;
  }

  // The World is a whole 3D application; its module graph and textures dwarf
  // every other asset on a marketing page. The band therefore ships as an
  // empty stage with an honest loading state, and the frame is created only
  // once the visitor is nearly at it. The stage keeps three jobs after that:
  // it tells the world when it has scrolled off screen so the renderer can
  // stop spending GPU behind the page, it keeps the frame out of hit testing
  // until the visitor asks for it — a live 3D canvas swallows the wheel and
  // touch gestures that would otherwise scroll the page past it — and it
  // retires the loading state on the world's own ready signal.
  function renderWorldFrame() {
    if (!WORLD_EMBED_ORIGINS.has(window.location.origin)) return "";
    return `
        <div class="site-footer-world-stage" data-world-stage data-world-phase="idle">
          <p class="site-footer-world-status" data-world-status role="status">
            <span class="site-footer-world-spinner" aria-hidden="true"></span>
            <span data-world-status-text></span>
          </p>
          <button
            type="button"
            class="site-footer-world-shield"
            data-world-shield
            hidden
          >
            <span class="site-footer-world-shield-label" data-world-shield-label></span>
          </button>
        </div>`;
  }

  const WORLD_EMBED_PHASES = {
    // Out of range: the band is still far below the fold and owns no work.
    idle: { status: "", shield: "" },
    // A metered connection asks before a download this size.
    deferred: {
      status: "The 3D world is a large download.",
      shield: "Load ForkMesh World",
    },
    loading: { status: "Loading the Town Square…", shield: "" },
    ready: { status: "", shield: "Click to explore the world" },
    engaged: { status: "", shield: "" },
  };

  function connectionSavesData() {
    try {
      return navigator.connection?.saveData === true;
    } catch (_) {
      return false;
    }
  }

  function mountWorldStage(stage) {
    if (stage.dataset.worldMounted === "true") return;
    stage.dataset.worldMounted = "true";

    const statusText = stage.querySelector("[data-world-status-text]");
    const shield = stage.querySelector("[data-world-shield]");
    const shieldLabel = stage.querySelector("[data-world-shield-label]");
    let phase = "";
    let frame = null;
    let frameLoaded = false;
    let onScreen = true;
    let engaged = false;
    let readyTimer = 0;

    function setPhase(next) {
      if (next === phase) return;
      phase = next;
      const copy = WORLD_EMBED_PHASES[next] || WORLD_EMBED_PHASES.idle;
      stage.dataset.worldPhase = next;
      if (statusText) statusText.textContent = copy.status;
      if (shieldLabel) shieldLabel.textContent = copy.shield;
      if (shield) shield.hidden = !copy.shield;
    }

    setPhase("idle");

    // Posting to a frame that has not reached its own origin yet is a console
    // error, not a silent no-op, so nothing is sent before the load event and
    // the current state is replayed the moment it arrives.
    function post(message) {
      if (!frame || !frameLoaded) return;
      try {
        frame.contentWindow?.postMessage(
          { source: WORLD_EMBED_MESSAGE_SOURCE, ...message },
          WORLD_EMBED_FRAME_ORIGIN,
        );
      } catch (_) {}
    }

    function setEngaged(next) {
      if (next === engaged) return;
      engaged = next;
      if (phase === "ready" || phase === "engaged") {
        setPhase(next ? "engaged" : "ready");
      }
      post({ type: "world-embed-engaged", engaged: next });
      if (next) frame?.focus?.();
    }

    function markReady() {
      window.clearTimeout(readyTimer);
      readyTimer = 0;
      if (phase === "loading") setPhase(engaged ? "engaged" : "ready");
    }

    function mountFrame() {
      if (frame) return;
      const template = document.createElement("template");
      template.innerHTML = worldFrameMarkup().trim();
      frame = template.content.firstElementChild;
      frame.addEventListener("load", () => {
        frameLoaded = true;
        window.clearTimeout(readyTimer);
        post({ type: "world-embed-visibility", visible: onScreen });
        post({ type: "world-embed-engaged", engaged });
        readyTimer = window.setTimeout(markReady, WORLD_EMBED_READY_TIMEOUT_MS);
      });
      setPhase("loading");
      stage.prepend(frame);
    }

    window.addEventListener("message", (event) => {
      if (!frame || event.source !== frame.contentWindow) return;
      if (event.origin !== WORLD_EMBED_FRAME_ORIGIN) return;
      if (event.data?.source !== "forkmesh-world") return;
      if (event.data.type === "world-embed-ready") markReady();
    });

    shield?.addEventListener("click", () => {
      if (phase === "deferred") {
        mountFrame();
        return;
      }
      setEngaged(true);
    });

    // Engaging hands the wheel to the world's camera, so the pointer leaving
    // the band hands it back rather than trapping the page at its foot.
    stage.addEventListener("pointerleave", (event) => {
      if (event.pointerType === "mouse") setEngaged(false);
    });
    document.addEventListener(
      "pointerdown",
      (event) => {
        if (engaged && !stage.contains(event.target)) setEngaged(false);
      },
      true,
    );

    // A band mounted into a page that is already at it must not wait for an
    // observer callback to be delivered — on a cold renderer that lands a
    // few hundred milliseconds later, which is the whole visible load.
    function nearViewport() {
      const rect = stage.getBoundingClientRect();
      const limit =
        window.innerHeight || document.documentElement.clientHeight || 0;
      return (
        rect.top <= limit + WORLD_EMBED_PRELOAD_PX &&
        rect.bottom >= -WORLD_EMBED_PRELOAD_PX
      );
    }

    const observable = typeof IntersectionObserver === "function";
    if (connectionSavesData()) {
      setPhase("deferred");
    } else if (!observable || nearViewport()) {
      mountFrame();
    } else {
      const preload = new IntersectionObserver(
        (entries) => {
          if (!entries.some((entry) => entry.isIntersecting)) return;
          preload.disconnect();
          mountFrame();
        },
        { rootMargin: WORLD_EMBED_PRELOAD_MARGIN },
      );
      preload.observe(stage);
    }

    // Occupancy is reported however the frame arrived. Without an observer
    // the band simply never claims to have left the screen, which only ever
    // costs the frames the renderer would have drawn anyway.
    if (!observable) return;
    const occupancy = new IntersectionObserver((entries) => {
      const entry = entries[entries.length - 1];
      if (!entry) return;
      onScreen = entry.isIntersecting === true;
      post({ type: "world-embed-visibility", visible: onScreen });
      if (!onScreen) setEngaged(false);
    });
    occupancy.observe(stage);
  }

  function mountWorldStages(root) {
    root
      .querySelectorAll("[data-world-stage]")
      .forEach((stage) => mountWorldStage(stage));
  }

  // ForkMesh World band pinned to the very bottom of every page: a short intro
  // strip, then the live Town Square embedded edge-to-edge with no border
  // below it. The "full screen" link navigates the same window to /world/.
  // Pages without a footer (the auth pages) mount it standalone, which needs
  // the extra class because the palette variables live on .forkmesh-footer.
  function renderWorldBand(standalone) {
    const titleId = standalone
      ? "site-footer-world-title-standalone"
      : "site-footer-world-title";

    return `
      <section class="site-footer-world${standalone ? " site-footer-world-standalone" : ""}" data-plugin-entry="world" aria-labelledby="${titleId}">
        <div class="site-footer-world-intro">
          <div class="site-footer-world-heading">
            <h2 id="${titleId}" class="site-footer-world-title">Step into the developer city</h2>
            <p class="site-footer-world-copy">
              The multiplayer Town Square is one part of ForkMesh, not a
              replacement for the website. Explore it here, then keep browsing
              repositories, source code, documentation, pricing, and the
              operations dashboard through normal web pages.
            </p>
          </div>
          <a href="/world/" class="site-footer-world-open">Open World full screen</a>
        </div>
        ${renderWorldFrame()}
      </section>`;
  }

  function renderWordmark() {
    return `
      <div aria-hidden="true" class="site-footer-word-wrap">
        <div
          id="footer-glow"
          class="site-footer-glow"
          style="--footer-glow-opacity: 0; --footer-glow-x: 50%; --footer-glow-y: 50%;"
        >
          <span class="site-footer-word footer-glow-base">forkmesh</span>
          <span class="site-footer-word footer-glow-text">forkmesh</span>
        </div>
      </div>`;
  }

  function renderFooter(variant, includeWorld = true, includeWord = true) {
    const isLanding = variant === "landing";
    const footerId = isLanding ? ' id="signup"' : "";

    return `
    <footer${footerId} class="forkmesh-footer forkmesh-footer-${isLanding ? "landing" : "standard"}" data-rendered-footer="${isLanding ? "landing" : "standard"}">
      ${includeWord ? renderWordmark() : ""}
      <div class="site-footer-panel">
        <div class="site-footer-links-grid">
          <div class="site-footer-brand">
            <a href="/" aria-label="home" class="site-footer-home-link">
              <img src="/assets/logo.svg" alt="" class="site-footer-logo" />
              <span class="site-footer-wordmark">forkmesh</span>
            </a>
            <p class="site-footer-brand-copy">
              Local-first distributed source-code preservation and collaboration infrastructure.
            </p>
          </div>
          ${FOOTER_COLUMNS.map(renderColumn).join("")}
        </div>
        <div class="site-footer-bottom">
          <div class="site-footer-copyright">
            &copy; 2026 ForkMesh. Built for local-first Git collaboration.
          </div>
          <div class="site-footer-meta">
            <a href="/status" class="site-footer-status-pill" data-status-entry>
              <span class="site-footer-status-dot" aria-hidden="true"></span>
              System status
            </a>
            <div class="site-footer-socials">
              ${SOCIAL_LINKS.map(renderSocialLink).join("")}
            </div>
          </div>
        </div>
      </div>
      ${includeWorld ? renderWorldBand() : ""}
    </footer>`;
  }

  function initFooterGlow(root) {
    const glow = root.querySelector(".site-footer-glow");
    if (!glow) return;

    const move = (event) => {
      const rect = glow.getBoundingClientRect();
      glow.style.setProperty("--footer-glow-x", `${event.clientX - rect.left}px`);
      glow.style.setProperty("--footer-glow-y", `${event.clientY - rect.top}px`);
      glow.style.setProperty("--footer-glow-opacity", "1");
    };

    glow.addEventListener("pointerenter", move);
    glow.addEventListener("pointermove", move);
    glow.addEventListener("pointerleave", () => {
      glow.style.setProperty("--footer-glow-opacity", "0");
    });
  }

  function mountFooter(host) {
    const variant = host.dataset.forkmeshFooter || "standard";
    const includeWorld = host.dataset.forkmeshFooterWorld !== "hidden";
    const includeWord = host.dataset.forkmeshFooterWord !== "hidden";
    const template = document.createElement("template");
    template.innerHTML = renderFooter(variant, includeWorld, includeWord).trim();
    const footer = template.content.firstElementChild;
    host.replaceWith(footer);
    initFooterGlow(footer);
    mountWorldStages(footer);
    mountGenieReturn(footer);
  }

  function mountGenieReturn(footer) {
    const meta = footer.querySelector(".site-footer-meta");
    if (!meta) return;
    const attach = () => {
      const genie = window.ForkMeshGenie;
      if (typeof genie?.hidden !== "function" || document.documentElement.dataset.genie === "off") return;
      const button = document.createElement("button");
      button.type = "button";
      button.className = "site-footer-genie";
      button.textContent = "Show the Genie spider";
      button.addEventListener("click", () => genie.show());
      const sync = () => {
        button.hidden = !genie.hidden();
      };
      window.addEventListener("forkmesh:genie", sync);
      sync();
      meta.prepend(button);
    };
    if (window.ForkMeshGenie || document.readyState === "complete") attach();
    else document.addEventListener("DOMContentLoaded", attach, { once: true });
  }

  function mountFooters() {
    document
      .querySelectorAll("[data-forkmesh-footer]")
      .forEach((host) => mountFooter(host));
  }

  function mountWorldBand(host) {
    const template = document.createElement("template");
    template.innerHTML = renderWorldBand(true).trim();
    const band = template.content.firstElementChild;
    host.replaceWith(band);
    mountWorldStages(band);
  }

  // Standalone World band for pages that render no footer, such as the signup
  // page a referral link lands on.
  function mountWorldBands() {
    document
      .querySelectorAll("[data-forkmesh-world]")
      .forEach((host) => mountWorldBand(host));
  }

  // ==== ForkMesh Ads slot: start ====
  // One first-party, human-reviewed, text-only sponsored card directly above
  // the footer. The privacy rules are the product (see
  // /docs/operations/ad-network/): the serve call names the surface and
  // nothing else, so both requests below say credentials: "omit" — the one
  // value api-client.js's routed fetch leaves alone instead of upgrading to
  // "include" — send no session header, and withhold the referrer so the
  // ad server never learns which page the visitor is reading. Every ad string is
  // untrusted plain text and only ever reaches the page through textContent.
  // A missing ad, a failed request or a malformed payload renders nothing.
  const ADS_SERVE_PATH = "/api/ads/serve?placement=www";
  const ADS_CLICK_BODY = JSON.stringify({ placement: "www" });
  const ADS_POLICY_URL = "/docs/operations/ad-network/";
  const ADS_ADVERTISE_URL = "https://app.forkmesh.com/dashboard/ads";
  const ADS_HIDDEN_KEY = "forkmesh.ads.hiddenUntil";
  const ADS_HIDE_MS = 7 * 24 * 60 * 60 * 1000;
  const ADS_IDLE_TIMEOUT_MS = 1500;
  const ADS_ID_RE = /^[0-9a-f]{32}$/;
  // Pages that stay ad-free even though they mount (or may one day mount) the
  // footer. Legal and security pages are read under stress or as a record and
  // must carry nothing commercial; the 404 page is an error surface; and the
  // documentation — which includes the ad policy itself — stays neutral
  // reference material. Entries match the path itself and everything below
  // it, with any trailing slash or ".html" ignored. A page can also opt out
  // with data-forkmesh-no-ads on <html> or <body>.
  const ADS_PATH_DENYLIST = [
    "/privacy",
    "/terms",
    "/security-report",
    "/404",
    "/docs",
  ];
  const ADS_STYLE = `
.fm-ad-slot {
  --fm-ad-fg: #f5f5f5;
  --fm-ad-muted: #a3a3a3;
  --fm-ad-border: #313134;
  --fm-ad-surface: rgba(255, 255, 255, 0.03);
  --fm-ad-cta-hover: rgba(255, 255, 255, 0.08);
  box-sizing: border-box;
  width: 100%;
  max-width: 80rem;
  margin: 3rem auto 2.5rem;
  padding: 0 1.25rem;
  color: var(--fm-ad-fg);
  font-family: inherit;
  font-size: 0.875rem;
  line-height: 1.45;
  text-align: left;
}
html.light .fm-ad-slot {
  --fm-ad-fg: #1f2328;
  --fm-ad-muted: #656d76;
  --fm-ad-border: #d0d7de;
  --fm-ad-surface: rgba(31, 35, 40, 0.025);
  --fm-ad-cta-hover: rgba(31, 35, 40, 0.06);
}
.fm-ad-slot[hidden] { display: none !important; }
.fm-ad-slot *, .fm-ad-slot *::before, .fm-ad-slot *::after { box-sizing: border-box; }
.fm-ad-card {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: 0.75rem 1.5rem;
  padding: 0.875rem 1rem;
  border: 1px solid var(--fm-ad-border);
  border-radius: 0.75rem;
  background: var(--fm-ad-surface);
}
.fm-ad-copy { min-width: 0; }
.fm-ad-meta {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.25rem 0.5rem;
  margin: 0 0 0.25rem;
  color: var(--fm-ad-muted);
  font-size: 0.75rem;
}
.fm-ad-label {
  padding: 0.0625rem 0.375rem;
  border: 1px solid var(--fm-ad-border);
  border-radius: 999px;
  font-size: 0.6875rem;
  font-weight: 600;
  letter-spacing: 0.04em;
  text-transform: uppercase;
}
.fm-ad-sponsor { overflow-wrap: anywhere; }
.fm-ad-headline {
  margin: 0;
  color: var(--fm-ad-fg);
  font-size: 0.9375rem;
  font-weight: 600;
  line-height: 1.35;
  overflow-wrap: anywhere;
}
.fm-ad-body {
  margin: 0.125rem 0 0;
  color: var(--fm-ad-muted);
  overflow-wrap: anywhere;
}
.fm-ad-cta {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-height: 2.25rem;
  padding: 0.375rem 0.875rem;
  border: 1px solid var(--fm-ad-border);
  border-radius: 0.5rem;
  color: var(--fm-ad-fg);
  font-size: 0.8125rem;
  font-weight: 600;
  text-decoration: none;
  white-space: nowrap;
}
.fm-ad-cta:hover { background: var(--fm-ad-cta-hover); text-decoration: none; }
.fm-ad-tools {
  grid-column: 1 / -1;
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 0.25rem 1rem;
  padding-top: 0.5rem;
  border-top: 1px solid var(--fm-ad-border);
  color: var(--fm-ad-muted);
  font-size: 0.75rem;
}
.fm-ad-tools a, .fm-ad-hide {
  color: var(--fm-ad-muted);
  font: inherit;
  text-decoration: underline;
  text-decoration-color: transparent;
  text-underline-offset: 0.15em;
}
.fm-ad-tools a:hover, .fm-ad-hide:hover {
  color: var(--fm-ad-fg);
  text-decoration-color: currentColor;
}
.fm-ad-hide {
  margin: 0 0 0 auto;
  padding: 0;
  border: 0;
  background: none;
  cursor: pointer;
}
.fm-ad-cta:focus-visible, .fm-ad-tools a:focus-visible, .fm-ad-hide:focus-visible {
  outline: 2px solid var(--fm-ad-fg);
  outline-offset: 2px;
}
@media (max-width: 40rem) {
  .fm-ad-slot { margin-top: 2rem; }
  .fm-ad-card { grid-template-columns: minmax(0, 1fr); }
  .fm-ad-cta { justify-self: start; white-space: normal; }
}
`;
  let adSlotScheduled = false;

  function adsPathDenied(pathname) {
    const path =
      String(pathname || "/")
        .toLowerCase()
        .replace(/\.html$/, "")
        .replace(/\/+$/, "") || "/";
    return ADS_PATH_DENYLIST.some(
      (entry) => path === entry || path.startsWith(`${entry}/`),
    );
  }

  function adsOptedOut() {
    return (
      document.documentElement.hasAttribute("data-forkmesh-no-ads") ||
      document.body?.hasAttribute("data-forkmesh-no-ads") === true
    );
  }

  function adsHidden() {
    try {
      const until = Number(localStorage.getItem(ADS_HIDDEN_KEY) || 0);
      return Number.isFinite(until) && until > Date.now();
    } catch (_) {
      return false;
    }
  }

  function rememberAdsHidden() {
    try {
      localStorage.setItem(ADS_HIDDEN_KEY, String(Date.now() + ADS_HIDE_MS));
    } catch (_) {}
  }

  function adsApiUrl(path) {
    try {
      return window.ForkMeshAPI?.url ? window.ForkMeshAPI.url(path) : path;
    } catch (_) {
      return path;
    }
  }

  function adText(value, limit) {
    return String(value ?? "").trim().slice(0, limit);
  }

  // Only a complete, well-formed creative is shown: an https destination, a
  // headline and a sponsor. Anything else is treated as "no ad".
  function normalizeAd(ad) {
    if (!ad || typeof ad !== "object") return null;
    const id = String(ad.id || "");
    if (!ADS_ID_RE.test(id)) return null;
    let url;
    try {
      url = new URL(String(ad.url || ""));
    } catch (_) {
      return null;
    }
    if (url.protocol !== "https:" || url.username || url.password) return null;
    const headline = adText(ad.headline, 60);
    const sponsor = adText(ad.sponsor, 48);
    if (!headline || !sponsor) return null;
    return {
      id,
      url: url.href,
      headline,
      sponsor,
      body: adText(ad.body, 160),
      cta: adText(ad.cta, 24) || "Learn more",
    };
  }

  function adElement(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function reportAdClick(id) {
    try {
      window
        .fetch(adsApiUrl(`/api/ads/${id}/click`), {
          method: "POST",
          credentials: "omit",
          referrerPolicy: "no-referrer",
          keepalive: true,
          headers: { "content-type": "application/json" },
          body: ADS_CLICK_BODY,
        })
        .catch(() => {});
    } catch (_) {}
  }

  function renderAdSlot(slot, ad) {
    const card = adElement("div", "fm-ad-card");
    const copy = adElement("div", "fm-ad-copy");
    const meta = adElement("p", "fm-ad-meta");
    meta.append(
      adElement("span", "fm-ad-label", "Sponsored"),
      adElement("span", "fm-ad-sponsor", ad.sponsor),
    );
    copy.append(meta, adElement("p", "fm-ad-headline", ad.headline));
    if (ad.body) copy.append(adElement("p", "fm-ad-body", ad.body));

    const cta = adElement("a", "fm-ad-cta", ad.cta);
    cta.href = ad.url;
    cta.target = "_blank";
    cta.rel = "sponsored noopener noreferrer";
    cta.referrerPolicy = "no-referrer";
    let counted = false;
    const count = () => {
      if (counted) return;
      counted = true;
      reportAdClick(ad.id);
    };
    cta.addEventListener("click", count);
    cta.addEventListener("auxclick", (event) => {
      if (event.button === 1) count();
    });

    const tools = adElement("div", "fm-ad-tools");
    const why = adElement("a", "", "Why this ad?");
    why.href = ADS_POLICY_URL;
    const advertise = adElement("a", "", "Advertise here");
    advertise.href = ADS_ADVERTISE_URL;
    const hide = adElement("button", "fm-ad-hide", "Hide");
    hide.type = "button";
    hide.setAttribute("aria-label", "Hide sponsored messages for 7 days");
    hide.addEventListener("click", () => {
      rememberAdsHidden();
      slot.remove();
    });
    tools.append(why, advertise, hide);

    card.append(copy, cta, tools);
    slot.replaceChildren(card);
    slot.hidden = false;
  }

  async function loadAdSlot() {
    const footer = document.querySelector("footer.forkmesh-footer");
    if (!footer || !footer.parentNode) return;
    if (document.querySelector("[data-forkmesh-ad-slot]")) return;
    let ad = null;
    try {
      const response = await window.fetch(adsApiUrl(ADS_SERVE_PATH), {
        credentials: "omit",
        referrerPolicy: "no-referrer",
        headers: { accept: "application/json" },
      });
      if (!response.ok) return;
      const payload = await response.json();
      ad = payload?.ok === true ? normalizeAd(payload.ad) : null;
    } catch (_) {
      return;
    }
    if (!ad || adsHidden() || !footer.isConnected) return;

    if (!document.getElementById("forkmesh-ad-slot-style")) {
      const style = document.createElement("style");
      style.id = "forkmesh-ad-slot-style";
      style.textContent = ADS_STYLE;
      document.head.append(style);
    }
    // The slot joins the document only once there is an ad to put in it, and
    // starts hidden, so a page never reserves or shifts space for nothing.
    const slot = document.createElement("aside");
    slot.className = "fm-ad-slot";
    slot.hidden = true;
    slot.setAttribute("aria-label", "Sponsored");
    slot.setAttribute("data-forkmesh-ad-slot", "www");
    footer.before(slot);
    renderAdSlot(slot, ad);
  }

  // Called after the footers mounted. One slot and one request per page load,
  // however many footers mounted or how often mountFooters() runs again, and
  // only once the browser is idle so the ad never competes with the page.
  function scheduleAdSlot() {
    if (adSlotScheduled) return;
    if (!document.querySelector("footer.forkmesh-footer")) return;
    if (adsOptedOut() || adsPathDenied(window.location.pathname)) return;
    if (adsHidden()) return;
    adSlotScheduled = true;
    const run = () => {
      loadAdSlot().catch(() => {});
    };
    if (typeof window.requestIdleCallback === "function") {
      window.requestIdleCallback(run, { timeout: ADS_IDLE_TIMEOUT_MS });
    } else {
      window.setTimeout(run, ADS_IDLE_TIMEOUT_MS);
    }
  }
  // ==== ForkMesh Ads slot: end ====

  function mountAll() {
    mountFooters();
    mountWorldBands();
    scheduleAdSlot();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", mountAll);
  } else {
    mountAll();
  }

  window.ForkMeshFooter = {
    renderFooter,
    renderWorldBand,
    mountFooters,
    mountWorldBands,
    mountWorldStages,
  };
})();
