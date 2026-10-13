// The membership panel shared by the website, app dashboard, World, and the
// desktop app's linked pricing experience: tiers, live member counts and the
// Patreon checkout links that turn a visitor into a paying member.
// It only ever opens on request -- a "Membership" link, a "Choose membership"
// button, a [data-forkmesh-membership] trigger, a #membership link (the
// address the mobile apps and extensions send people to), the
// forkmesh:show-membership event, or ForkMeshMembershipPrompt.open(). Nothing here interrupts a session
// on its own and Core is never feature-gated. Aggregate counts come from the
// same privacy-safe public digest that powers the site header's community
// count.
(() => {
  "use strict";

  if (window.ForkMeshMembershipPrompt?.installed) return;

  const PATREON_URL = "https://www.patreon.com/16434219/join";
  const PATREON_CHECKOUT = Object.freeze({
    free: "https://www.patreon.com/checkout/mdkaifansari04?rid=29122373",
    supporter: "https://www.patreon.com/checkout/mdkaifansari04?rid=29122413",
    pro: "https://www.patreon.com/checkout/mdkaifansari04?rid=29122568",
    builder: "https://www.patreon.com/checkout/mdkaifansari04?rid=29122566",
    studio: "https://www.patreon.com/checkout/mdkaifansari04?rid=29260767",
  });
  const FOUNDING_GOAL = 100;
  const PREVIEW = new URLSearchParams(location.search).get("membership") === "preview";
  let previousFocus = null;

  const style = document.createElement("style");
  // Every colour resolves through a token so the light palette below is a
  // token swap rather than a second sheet. Written one rule per line: it
  // ships inside this bundle, so it stays compact.
  style.textContent = `
@font-face{font-family:"ForkMesh Favorit";src:url("/assets/fonts/ABCFavorit-Bold.woff2") format("woff2");font-weight:400 800;font-style:normal;font-display:swap}
.fm-member-prompt[hidden]{display:none!important}
.fm-member-prompt{--fm-page:#0b0b0c;--fm-card:#141416;--fm-line:rgba(255,255,255,.12);--fm-line-soft:rgba(255,255,255,.08);--fm-fg:#f4f4f5;--fm-muted:#a1a1a8;--fm-check:#d4d4d8;--fm-track:rgba(255,255,255,.1);--fm-scrim:rgba(4,4,5,.72);--fm-card-shadow:0 0 0 1px rgba(255,255,255,.07),0 4px 8px rgba(0,0,0,.35);--fm-hi:#f4f4f5;--fm-hi-fg:#111113;--fm-hi-muted:#52525b;--fm-hi-check:#27272a;--fm-hi-line:rgba(17,17,19,.1);--fm-btn:linear-gradient(180deg,#ffffff 0%,#e8e8ea 100%);--fm-btn-hover:linear-gradient(180deg,#ffffff 0%,#f1f1f2 100%);--fm-btn-fg:#111113;--fm-btn-border:#f4f4f5;--fm-btn-shadow:inset 0 -1px 0 rgba(0,0,0,.14),0 1px 2px rgba(0,0,0,.5);--fm-btn-hi:linear-gradient(180deg,#2c2c2c 0%,#222222 100%);--fm-btn-hi-hover:linear-gradient(180deg,#363636 0%,#2a2a2a 100%);--fm-btn-hi-fg:#ffffff;--fm-btn-hi-border:#161616;--fm-btn-hi-shadow:inset 0 1px 0 rgba(255,255,255,.17),0 1px 2px rgba(0,0,0,.16);--fm-display:"ForkMesh Favorit",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;position:fixed;inset:0;z-index:2147483000;display:grid;place-items:center;padding:6px;font-family:"ForkMesh Lato",Inter,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:var(--fm-fg)}
html.light .fm-member-prompt{--fm-page:#f4f4f4;--fm-card:#ffffff;--fm-line:#e1e2e3;--fm-line-soft:#ebebec;--fm-fg:#242424;--fm-muted:#6f6f6f;--fm-check:#242424;--fm-track:rgba(36,36,36,.1);--fm-scrim:rgba(31,35,40,.42);--fm-card-shadow:0 0 0 1px rgba(34,42,53,.06),0 1px 5px -4px rgba(36,36,36,.7),0 4px 8px rgba(36,36,36,.05);--fm-hi:#242424;--fm-hi-fg:#ffffff;--fm-hi-muted:rgba(255,255,255,.72);--fm-hi-check:#ffffff;--fm-hi-line:rgba(255,255,255,.14);--fm-btn:linear-gradient(180deg,#2c2c2c 0%,#222222 100%);--fm-btn-hover:linear-gradient(180deg,#363636 0%,#2a2a2a 100%);--fm-btn-fg:#ffffff;--fm-btn-border:#161616;--fm-btn-shadow:inset 0 1px 0 rgba(255,255,255,.17),0 1px 2px rgba(0,0,0,.16);--fm-btn-hi:linear-gradient(180deg,#ffffff 0%,#fcfcfc 100%);--fm-btn-hi-hover:linear-gradient(180deg,#ffffff 0%,#f3f3f4 100%);--fm-btn-hi-fg:#292929;--fm-btn-hi-border:#ffffff;--fm-btn-hi-shadow:inset 0 -1px 0 rgba(0,0,0,.06),0 1px 2px rgba(0,0,0,.3)}
.fm-member-backdrop{position:absolute;inset:0;border:0;background:var(--fm-scrim);-webkit-backdrop-filter:blur(14px);backdrop-filter:blur(14px);cursor:default}
.fm-member-dialog{position:relative;display:flex;flex-direction:column;width:min(1160px,100%);max-height:calc(100svh - 12px);overflow:hidden;border:1px solid var(--fm-line);border-radius:22px;background:var(--fm-page);box-shadow:0 40px 120px rgba(0,0,0,.45)}
.fm-member-dialog:focus{outline:none}
.fm-member-close{position:absolute;z-index:4;top:14px;right:14px;display:grid;place-items:center;width:32px;height:32px;border:0;border-radius:10px;background:transparent;color:var(--fm-muted);cursor:pointer;transition:background-color .15s,color .15s}
.fm-member-close svg{width:16px;height:16px}
.fm-member-close:hover,.fm-member-close:focus-visible{background:var(--fm-track);color:var(--fm-fg);outline:none}
.fm-member-scroll{flex:1;min-height:0;overflow:auto;overscroll-behavior:contain}
.fm-member-hero{padding:30px 64px 0 32px}
.fm-member-title{margin:0;font-family:var(--fm-display);font-size:clamp(28px,3.4vw,40px);font-weight:700;line-height:1.05;letter-spacing:-.03em}
.fm-member-intro{max-width:72ch;margin:10px 0 0;color:var(--fm-muted);font-size:15px;line-height:1.55}
.fm-member-goal{display:grid;grid-template-columns:minmax(0,1fr) auto;align-items:center;gap:10px 20px;margin:20px 0 0;padding:14px 16px;border-radius:14px;background:var(--fm-card);box-shadow:var(--fm-card-shadow)}
.fm-member-goal-copy{display:grid;gap:3px;min-width:0}
.fm-member-goal-copy>strong{font-size:14px;font-weight:700}
.fm-member-goal-copy span{color:var(--fm-muted);font-size:12.5px}
.fm-member-goal-copy span strong{color:var(--fm-fg);font-weight:700}
.fm-member-goal-count{color:var(--fm-muted);font-size:12.5px;text-align:right;font-variant-numeric:tabular-nums}
.fm-member-progress{grid-column:1 / -1;height:6px;overflow:hidden;border-radius:999px;background:var(--fm-track)}
.fm-member-progress-fill{width:0;height:100%;border-radius:inherit;background:var(--fm-fg);transition:width 700ms cubic-bezier(.2,.75,.25,1)}
.fm-member-body{padding:18px 32px 26px}
.fm-member-plans{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:12px}
.fm-member-plan{position:relative;display:flex;min-width:0;flex-direction:column;padding:20px 18px;border-radius:18px;background:var(--fm-card);box-shadow:var(--fm-card-shadow);color:var(--fm-fg)}
.fm-member-plan[data-plan="pro"]{--fm-muted:var(--fm-hi-muted);--fm-check:var(--fm-hi-check);--fm-line-soft:var(--fm-hi-line);background:var(--fm-hi);color:var(--fm-hi-fg)}
.fm-member-plan-heading{display:flex;align-items:center;gap:10px}
.fm-member-plan h2{margin:0;font-family:var(--fm-display);font-size:19px;font-weight:700;letter-spacing:-.01em}
.fm-member-price{display:flex;align-items:baseline;gap:6px;margin:18px 0 0}
.fm-member-price strong{font-family:var(--fm-display);font-size:40px;font-weight:700;line-height:1;letter-spacing:-.03em}
.fm-member-price span{color:var(--fm-muted);font-size:14px}
.fm-member-plan-note{min-height:4.5em;margin:10px 0 0;color:var(--fm-muted);font-size:13px;line-height:1.5}
.fm-member-plan-button{display:flex;align-items:center;justify-content:center;gap:7px;min-height:40px;margin-top:16px;padding:0 12px;border:1px solid var(--fm-btn-border);border-radius:12px;background:var(--fm-btn);box-shadow:var(--fm-btn-shadow);color:var(--fm-btn-fg);font-size:14px;font-weight:600;text-decoration:none;white-space:nowrap;transition:background .15s,transform .15s}
.fm-member-plan-button svg{width:12px;height:12px;opacity:.6}
.fm-member-plan-button:hover{background:var(--fm-btn-hover);color:var(--fm-btn-fg)}
.fm-member-plan-button:active{transform:translateY(1px)}
.fm-member-plan-button:focus-visible{outline:2px solid #58a6ff;outline-offset:2px}
.fm-member-plan[data-plan="pro"] .fm-member-plan-button{border-color:var(--fm-btn-hi-border);background:var(--fm-btn-hi);box-shadow:var(--fm-btn-hi-shadow);color:var(--fm-btn-hi-fg)}
.fm-member-plan[data-plan="pro"] .fm-member-plan-button:hover{background:var(--fm-btn-hi-hover);color:var(--fm-btn-hi-fg)}
.fm-member-plan-count{margin:8px 0 0;color:var(--fm-muted);font-size:12px;text-align:center}
.fm-member-perks-label{margin:18px 0 0;padding-top:16px;border-top:1px solid var(--fm-line-soft);font-size:13px;font-weight:600}
.fm-member-perks{margin:4px 0 0;padding:0;list-style:none;font-size:13px;line-height:1.45}
.fm-member-perks li{position:relative;margin:9px 0 0;padding-left:24px}
.fm-member-perks li:before{content:"";position:absolute;left:0;top:.12em;width:15px;height:15px;background:var(--fm-check);-webkit-mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%23000' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='m5 12.5 4.5 4.5L19 7.5'/%3E%3C/svg%3E") center/contain no-repeat;mask:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%23000' stroke-width='2.2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='m5 12.5 4.5 4.5L19 7.5'/%3E%3C/svg%3E") center/contain no-repeat}
.fm-member-free{display:flex;flex-wrap:wrap;align-items:center;gap:6px 12px;margin:12px 0 0;padding:13px 16px;border-radius:14px;background:var(--fm-card);box-shadow:var(--fm-card-shadow);color:var(--fm-muted);font-size:13px}
.fm-member-free strong{color:var(--fm-fg);font-size:14px;font-weight:700}
.fm-member-free a{display:inline-flex;align-items:center;gap:5px;margin-left:auto;color:var(--fm-fg);font-weight:600;white-space:nowrap;text-decoration:none}
.fm-member-free a:hover{text-decoration:underline}
.fm-member-footer{flex-shrink:0;display:flex;align-items:center;justify-content:space-between;gap:20px;padding:14px 32px;border-top:1px solid var(--fm-line-soft);background:var(--fm-page)}
.fm-member-footnote{max-width:74ch;margin:0;color:var(--fm-muted);font-size:12.5px;line-height:1.5}
.fm-member-footnote a{color:var(--fm-fg);font-weight:600;text-underline-offset:3px}
.fm-member-actions{display:flex;align-items:center;gap:18px;white-space:nowrap}
.fm-member-later{border:0;background:transparent;color:var(--fm-muted);font:inherit;font-size:13px;font-weight:600;cursor:pointer}
.fm-member-later:hover,.fm-member-later:focus-visible{color:var(--fm-fg);outline:none;text-decoration:underline}
.fm-member-compare{display:inline-flex;align-items:center;gap:5px;color:var(--fm-fg);font-size:13px;font-weight:600;text-decoration:none;text-underline-offset:3px}
.fm-member-compare:hover{text-decoration:underline}
.fm-member-compare svg,.fm-member-free a svg{width:11px;height:11px;opacity:.6}
@media(max-width:1000px){.fm-member-plans{grid-template-columns:repeat(2,minmax(0,1fr))}}
@media(max-width:900px){.fm-member-hero{padding:24px 56px 0 20px}.fm-member-body{padding:16px 20px 20px}.fm-member-footer{flex-direction:column;align-items:flex-start;gap:10px;padding:14px 20px}.fm-member-actions{width:100%;justify-content:space-between}}
@media(max-width:560px){.fm-member-plans{grid-template-columns:1fr}.fm-member-goal{grid-template-columns:1fr}.fm-member-goal-count{text-align:left}.fm-member-plan-note{min-height:0}}
@media(prefers-reduced-motion:reduce){.fm-member-progress-fill{transition:none}}
  `;
  document.head.appendChild(style);

  const EXTERNAL_ICON =
    '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M5 11 11 5M6 5h5v5"/></svg>';
  const CLOSE_ICON =
    '<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><path d="m4 4 8 8M12 4l-8 8"/></svg>';

  async function activity() {
    try {
      const response = await fetch("/api/chat/activity", {
        headers: { accept: "application/json" },
        cache: "no-store",
      });
      if (!response.ok) return null;
      const data = await response.json();
      return data?.ok ? data : null;
    } catch (_) {
      return null;
    }
  }

  function countLabel(value, singular, plural = `${singular}s`) {
    const count = Math.max(0, Number(value) || 0);
    if (count === 0) return "Be the first to join";
    return `${count.toLocaleString()} ${count === 1 ? singular : plural}`;
  }

  function markup() {
    const root = document.createElement("div");
    root.className = "fm-member-prompt";
    root.hidden = true;
    root.innerHTML = `
      <button class="fm-member-backdrop" type="button" data-member-close aria-label="Close membership invitation"></button>
      <section class="fm-member-dialog" role="dialog" aria-modal="true" aria-labelledby="fm-member-title" aria-describedby="fm-member-intro" tabindex="-1">
        <button class="fm-member-close" type="button" data-member-close aria-label="Not now">${CLOSE_ICON}</button>
        <div class="fm-member-scroll">
          <header class="fm-member-hero">
            <h1 class="fm-member-title" id="fm-member-title">Become a ForkMesh member.</h1>
            <p class="fm-member-intro" id="fm-member-intro">ForkMesh Core is free, and it stays free for everyone. Memberships are how the community pays for the public relays, signed releases, and security work that keep code reachable. If ForkMesh is useful to you, chip in at whatever level feels right.</p>
            <div class="fm-member-goal" aria-live="polite">
              <div class="fm-member-goal-copy">
                <strong>Founding 100 membership drive</strong>
                <span><strong data-community-total>—</strong> people in the community · <strong data-member-total>—</strong> founding members so far</span>
              </div>
              <span class="fm-member-goal-count" data-member-progress-label>Be one of the first 100 sustaining members</span>
              <div class="fm-member-progress" role="progressbar" aria-label="Founding member goal" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0"><div class="fm-member-progress-fill"></div></div>
            </div>
          </header>
          <div class="fm-member-body">
            <div class="fm-member-plans">
              <article class="fm-member-plan" data-plan="supporter">
                <div class="fm-member-plan-heading">
                  <h2>Supporter</h2>
                </div>
                <p class="fm-member-price"><strong>$5</strong><span>per month</span></p>
                <p class="fm-member-plan-note">For anyone who wants to help keep the public relays and releases running.</p>
                <a class="fm-member-plan-button" href="${PATREON_CHECKOUT.supporter}" target="_blank" rel="noopener" data-member-cta aria-label="Support as a Supporter, $5 / month, on Patreon">Support for $5 a month ${EXTERNAL_ICON}</a>
                <p class="fm-member-plan-count" data-supporter-count>Count loading…</p>
                <p class="fm-member-perks-label">Includes:</p>
                <ul class="fm-member-perks">
                  <li>Supporter badge on your ForkMesh profile</li>
                  <li>Unlock World outfit colors and cuts</li>
                  <li>Supporters wall / public credits listing</li>
                  <li>Help fund public relays, mirrors and releases</li>
                </ul>
              </article>
              <article class="fm-member-plan" data-plan="pro">
                <div class="fm-member-plan-heading">
                  <h2>Pro</h2>
                </div>
                <p class="fm-member-price"><strong>$10</strong><span>per month</span></p>
                <p class="fm-member-plan-note">Support the project and put the hosted agent workflows to work every day.</p>
                <a class="fm-member-plan-button" href="${PATREON_CHECKOUT.pro}" target="_blank" rel="noopener" data-member-cta aria-label="Support as a Pro member, $10 / month, on Patreon">Support for $10 a month ${EXTERNAL_ICON}</a>
                <p class="fm-member-plan-count" data-pro-count>Count loading…</p>
                <p class="fm-member-perks-label">Everything in Supporter, plus:</p>
                <ul class="fm-member-perks">
                  <li>Pro badge on your ForkMesh profile</li>
                  <li>Hosted issue-to-agent coding workflows</li>
                  <li>Automatic AI PR reviews and quick fixes</li>
                  <li>AI conflict resolution and review loops</li>
                  <li>Parallel agents with resumable sessions</li>
                  <li>Mobile editor, terminal, files, and dev tools</li>
                  <li>Everything in Free Core (Git hosting, signed collaboration, mirrors, encrypted rooms)</li>
                </ul>
              </article>
              <article class="fm-member-plan" data-plan="builder">
                <div class="fm-member-plan-heading">
                  <h2>Builder</h2>
                </div>
                <p class="fm-member-price"><strong>$50</strong><span>per month</span></p>
                <p class="fm-member-plan-note">Fund what gets built next, with early access and time with the team.</p>
                <a class="fm-member-plan-button" href="${PATREON_CHECKOUT.builder}" target="_blank" rel="noopener" data-member-cta aria-label="Support as a Builder, $50 / month, on Patreon">Support for $50 a month ${EXTERNAL_ICON}</a>
                <p class="fm-member-plan-count" data-builder-count>Count loading…</p>
                <p class="fm-member-perks-label">Everything in Pro, plus:</p>
                <ul class="fm-member-perks">
                  <li>Builder badge on your ForkMesh profile</li>
                  <li>Exclusive Builder World outfit and office prop set</li>
                  <li>Priority queue for custom office and World requests</li>
                  <li>Access to supporters office hours and group AMAs</li>
                  <li>Early access to selected experimental features</li>
                  <li>Private supporters community role</li>
                </ul>
              </article>
              <article class="fm-member-plan" data-plan="studio">
                <div class="fm-member-plan-heading">
                  <h2>Studio</h2>
                </div>
                <p class="fm-member-price"><strong>$100</strong><span>per month</span></p>
                <p class="fm-member-plan-note">Sponsor ForkMesh and get hands-on help setting it up for your team.</p>
                <a class="fm-member-plan-button" href="${PATREON_CHECKOUT.studio}" target="_blank" rel="noopener" data-member-cta aria-label="Support as a Studio sponsor, $100 / month, on Patreon">Support for $100 a month ${EXTERNAL_ICON}</a>
                <p class="fm-member-plan-count" data-studio-count>Count loading…</p>
                <p class="fm-member-perks-label">Everything in Builder, plus:</p>
                <ul class="fm-member-perks">
                  <li>Studio badge on your ForkMesh profile</li>
                  <li>Kickoff onboarding call</li>
                  <li>Personal ForkMesh World and office setup assistance</li>
                  <li>Guided subdomain and deployment pipeline setup</li>
                  <li>Hands-on help throughout the entire setup process</li>
                  <li>Weekly syncs during onboarding and active Studio engagement</li>
                  <li>Priority support channel (business-hours priority, not 24/7)</li>
                  <li>Direct roadmap input for your use case</li>
                </ul>
              </article>
            </div>
            <div class="fm-member-free"><strong>Free</strong><span>$0 / month · Follow public updates; ForkMesh Core remains free.</span><a href="${PATREON_CHECKOUT.free}" target="_blank" rel="noopener" data-member-cta>Join free ${EXTERNAL_ICON}</a></div>
          </div>
        </div>
        <footer class="fm-member-footer">
          <p class="fm-member-footnote">Financially transparent—<a href="/ledger">review our public ledger</a>. Patreon handles payments; every Core feature stays enabled whether or not you join.</p>
          <div class="fm-member-actions"><button class="fm-member-later" type="button" data-member-close>Not now</button><a class="fm-member-compare" href="${PATREON_URL}" target="_blank" rel="noopener">View on Patreon ${EXTERNAL_ICON}</a></div>
        </footer>
      </section>`;
    document.body.appendChild(root);
    root.querySelectorAll("[data-member-close]").forEach((button) => {
      button.addEventListener("click", () => close());
    });
    return root;
  }

  function root() {
    return document.querySelector(".fm-member-prompt") || markup();
  }

  function close() {
    const prompt = document.querySelector(".fm-member-prompt");
    if (!prompt || prompt.hidden) return;
    prompt.hidden = true;
    // Drop #membership so following the same link again reopens the panel.
    if (location.hash === "#membership") {
      history.replaceState(null, "", location.pathname + location.search);
    }
    document.removeEventListener("keydown", onKeydown);
    previousFocus?.focus?.();
    window.dispatchEvent(new CustomEvent("forkmesh:membership-prompt-closed"));
  }

  function onKeydown(event) {
    const prompt = document.querySelector(".fm-member-prompt");
    if (!prompt || prompt.hidden) return;
    if (event.key === "Escape") {
      event.preventDefault();
      close();
      return;
    }
    if (event.key !== "Tab") return;
    const focusable = [...prompt.querySelectorAll("button,a[href]")].filter(
      (element) => !element.hidden && element.getClientRects().length,
    );
    if (!focusable.length) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  }

  async function hydrate(prompt) {
    const data = await activity();
    if (!data || prompt.hidden) return;
    const membership = data.membership || {};
    const supporter = Math.max(0, Number(membership.supporterCount) || 0);
    const pro = Math.max(0, Number(membership.proCount) || 0);
    const builder = Math.max(0, Number(membership.builderCount) || 0);
    const studio = Math.max(0, Number(membership.studioCount) || 0);
    const total = Math.max(supporter + pro + builder + studio, Number(membership.totalCount) || 0);
    const community = Math.max(0, Number(data.userCount) || 0);
    prompt.querySelector("[data-member-total]").textContent = total.toLocaleString();
    prompt.querySelector("[data-community-total]").textContent = community.toLocaleString();
    prompt.querySelector("[data-supporter-count]").textContent = countLabel(supporter, "active Supporter");
    prompt.querySelector("[data-pro-count]").textContent = countLabel(pro, "active Pro member", "active Pro members");
    prompt.querySelector("[data-builder-count]").textContent = countLabel(builder, "active Builder");
    prompt.querySelector("[data-studio-count]").textContent = countLabel(studio, "active Studio");
    const progress = Math.min(FOUNDING_GOAL, total);
    const bar = prompt.querySelector(".fm-member-progress");
    bar.setAttribute("aria-valuenow", String(progress));
    prompt.querySelector(".fm-member-progress-fill").style.width = `${progress}%`;
    prompt.querySelector("[data-member-progress-label]").textContent =
      total >= FOUNDING_GOAL
        ? `${total.toLocaleString()} members and growing`
        : `${total.toLocaleString()} of ${FOUNDING_GOAL} founding memberships`;
  }

  function open() {
    const prompt = root();
    if (!prompt.hidden) return;
    previousFocus = document.activeElement;
    prompt.hidden = false;
    document.addEventListener("keydown", onKeydown);
    prompt.querySelector(".fm-member-dialog").focus();
    void hydrate(prompt);
    window.dispatchEvent(new CustomEvent("forkmesh:membership-prompt-opened"));
  }

  // Any surface can offer membership by marking a control with
  // data-forkmesh-membership; the delegated listener below covers controls
  // that are rendered after this bundle loads.
  function onTriggerClick(event) {
    const trigger = event.target?.closest?.("[data-forkmesh-membership]");
    if (!trigger) return;
    event.preventDefault();
    open();
  }

  const api = Object.freeze({ installed: true, open, close });
  window.ForkMeshMembershipPrompt = api;
  window.addEventListener("forkmesh:show-membership", open);
  document.addEventListener("click", onTriggerClick);
  window.addEventListener("hashchange", () => {
    if (location.hash === "#membership") open();
  });
  // ?membership=preview renders the panel straight away so the design can be
  // reviewed and screenshotted without hunting for a trigger.
  if (PREVIEW || location.hash === "#membership") {
    if (document.readyState === "loading") {
      document.addEventListener("DOMContentLoaded", open, { once: true });
    } else {
      open();
    }
  }
})();
