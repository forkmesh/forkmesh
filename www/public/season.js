(() => {
  if (window.ForkMeshSeason) return;
  const KEY = "forkmesh.season";
  const root = document.documentElement;
  const HAT = "data:image/svg+xml," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 60 44"><path d="M16 36C22 26 26 16 31 9c3-5 10-7 17-4-5 1-10 3-11 8 1 9 5 17 9 23-10 3-20 3-30 0z" fill="#2e1a4a" stroke="#8f66d1" stroke-width=".8"/><path d="M17 31q14 5 28 0l1 5q-15 4-30 0z" fill="#fa7a18"/><rect x="27.5" y="31.5" width="6" height="4.6" rx=".6" fill="none" stroke="#ffd678"/><ellipse cx="31" cy="37.5" rx="27" ry="5.2" fill="#24123c" stroke="#8f66d1" stroke-width=".8"/></svg>');
  const SPIDER = '<svg viewBox="0 0 40 34" width="30" height="26" aria-hidden="true"><g stroke="#6b5a86" stroke-width="2" fill="none" stroke-linecap="round"><path d="M14 14 6 8 2 14M14 17 5 15 1 21M14 20 6 22 3 29M15 22 9 28 8 33M26 14 34 8 38 14M26 17 35 15 39 21M26 20 34 22 37 29M25 22 31 28 32 33"/></g><ellipse cx="20" cy="21" rx="7.5" ry="8.5" fill="#2a1f3a" stroke="#8f66d1"/><circle cx="20" cy="11" r="5" fill="#2a1f3a" stroke="#8f66d1"/><circle cx="18" cy="10.5" r="1.2" fill="#fa7a18"/><circle cx="22" cy="10.5" r="1.2" fill="#fa7a18"/></svg>';
  const STYLE = `
html[data-season=halloween] [data-profile-settings-button]{position:relative}
html[data-season=halloween] [data-profile-settings-button]::after{content:"";position:absolute;left:-6px;top:-13px;width:34px;height:25px;background:url("${HAT}") center/contain no-repeat;transform:rotate(-16deg);pointer-events:none}
.fm-season-web{position:fixed;pointer-events:none;z-index:1;color:rgb(201 209 217 / .22)}
html[data-dashboard-theme=light] .fm-season-web{color:rgb(31 35 40 / .16)}
.fm-season-web-corner{top:56px;right:0;width:240px;height:220px}
.fm-season-web-side{left:0;bottom:0;width:140px;height:130px}
.fm-season-spider{position:fixed;top:56px;right:90px;z-index:1;pointer-events:none;display:flex;flex-direction:column;align-items:center;transform-origin:50% 0;animation:fm-season-dangle 5s ease-in-out infinite alternate}
.fm-season-spider i{display:block;width:1px;height:150px;background:rgb(145 152 161 / .6)}
@keyframes fm-season-dangle{0%{transform:translateY(-40px) rotate(-2deg)}60%{transform:translateY(30px) rotate(2deg)}100%{transform:translateY(10px) rotate(-1deg)}}
@media (max-width:1499px){.fm-season-web-corner,.fm-season-spider{display:none}}
@media (max-width:1023px){.fm-season-web-side{display:none}}
@media (prefers-reduced-motion:reduce){.fm-season-spider{animation:none}}
@media print{.fm-season-web,.fm-season-spider{display:none}}
`;

  function read() {
    try {
      return JSON.parse(localStorage.getItem(KEY) || "null");
    } catch (_) {
      return null;
    }
  }

  function write(state) {
    try {
      localStorage.setItem(KEY, JSON.stringify(state));
    } catch (_) {}
  }

  function web(width, height, corner, spokes, rings) {
    const [cx, cy] = corner;
    const reach = Math.hypot(width, height);
    const sx = cx === 0 ? 1 : -1;
    const sy = cy === 0 ? 1 : -1;
    const at = (a, r) => [cx + sx * Math.abs(Math.cos(a)) * r, cy + sy * Math.abs(Math.sin(a)) * r];
    const angles = Array.from({ length: spokes }, (_, i) => (Math.PI / 2) * (i / (spokes - 1)));
    const f = (n) => n.toFixed(1);
    let d = "";
    angles.forEach((a) => {
      const [x, y] = at(a, reach);
      d += `M${cx} ${cy}L${f(x)} ${f(y)}`;
    });
    for (let k = 1; k <= rings; k += 1) {
      const r = (Math.min(width, height) * 1.05 * k) / rings;
      for (let i = 0; i < angles.length - 1; i += 1) {
        const [x1, y1] = at(angles[i], r);
        const [x2, y2] = at(angles[i + 1], r);
        const [mx, my] = at((angles[i] + angles[i + 1]) / 2, r * 0.86);
        d += `M${f(x1)} ${f(y1)}Q${f(mx)} ${f(my)} ${f(x2)} ${f(y2)}`;
      }
    }
    return `<svg viewBox="0 0 ${width} ${height}" aria-hidden="true"><path d="${d}" fill="none" stroke="currentColor" stroke-width="1"/></svg>`;
  }

  function decorate() {
    if (!document.body) return;
    const on = root.dataset.season === "halloween";
    document.querySelectorAll("[data-season-decor]").forEach((node) => {
      if (!on) node.remove();
    });
    if (!on || document.querySelector("[data-season-decor]")) return;
    const style = document.createElement("style");
    style.setAttribute("data-season-decor", "");
    style.textContent = STYLE;
    document.head.appendChild(style);
    const corner = document.createElement("div");
    corner.className = "fm-season-web fm-season-web-corner";
    corner.innerHTML = web(240, 220, [240, 0], 7, 6);
    const side = document.createElement("div");
    side.className = "fm-season-web fm-season-web-side";
    side.innerHTML = web(140, 130, [0, 130], 5, 4);
    const spider = document.createElement("div");
    spider.className = "fm-season-spider";
    spider.innerHTML = `<i></i>${SPIDER}`;
    [corner, side, spider].forEach((node) => {
      node.setAttribute("data-season-decor", "");
      node.setAttribute("aria-hidden", "true");
      document.body.appendChild(node);
    });
  }

  function apply(state) {
    const season = state && state.season && Number(state.until) > Date.now() ? String(state.season) : "";
    const before = root.dataset.season || "";
    if (season) root.dataset.season = season;
    else delete root.dataset.season;
    decorate();
    if (before !== season) window.dispatchEvent(new CustomEvent("forkmesh:season", { detail: { season } }));
  }

  function refresh() {
    return fetch("/api/season", { credentials: "omit", headers: { accept: "application/json" } })
      .then((response) => (response.ok ? response.json() : null))
      .then((data) => {
        if (!data || data.ok !== true) return;
        const state = { season: data.season || "", until: Number(data.until) || 0 };
        write(state);
        apply(state);
      })
      .catch(() => {});
  }

  window.ForkMeshSeason = {
    current: () => root.dataset.season || "",
    set(state) {
      write(state);
      apply(state);
    },
    refresh,
  };
  apply(read());
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", decorate, { once: true });
  void refresh();
})();
