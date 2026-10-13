// ForkMesh homepage pill navigation — shared behaviour.
//
// Lifted verbatim from the inline <script> in index.html so a second page can
// mount the same header. Scrolled state, hover/click dropdowns and the mobile
// panel. Theme switching is not here: that is home-theme.js, and the signed-in
// swap of Sign in / Sign up is home-header-auth.js, exactly as on the homepage.
//
// The homepage still has its inline copy; keep the two in step until it is
// switched over to this file.

/* Pill navigation: scrolled state, dropdown menus and the mobile panel.
   Menus open on hover for a mouse and on click/Enter for touch and
   keyboard; Escape, an outside press, or following a link closes
   everything. */
(() => {
  const nav = document.getElementById("site-nav");
  if (!nav) return;
  const toggle = document.getElementById("mobile-menu-toggle");
  const updateNav = () => {
    nav.classList.toggle("is-scrolled", window.scrollY > 8);
  };
  updateNav();
  window.addEventListener("scroll", updateNav, { passive: true });

  const setMobile = (open) => {
    nav.dataset.open = String(open);
    toggle?.setAttribute("aria-expanded", String(open));
    toggle?.setAttribute("aria-label", open ? "Close menu" : "Open menu");
  };
  toggle?.addEventListener("click", () => {
    setMobile(nav.dataset.open !== "true");
  });

  const items = Array.from(nav.querySelectorAll(".home-nav-item.has-menu"));
  const setOpen = (item, open) => {
    item.classList.toggle("is-open", open);
    item
      .querySelector("[aria-expanded]")
      ?.setAttribute("aria-expanded", String(open));
  };
  const closeAll = (except) => {
    items.forEach((item) => {
      if (item !== except) setOpen(item, false);
    });
  };
  items.forEach((item) => {
    const button = item.querySelector("button[aria-expanded]");
    let leaveTimer = 0;
    button?.addEventListener("click", () => {
      const open = !item.classList.contains("is-open");
      closeAll(item);
      setOpen(item, open);
    });
    item.addEventListener("pointerenter", (event) => {
      if (event.pointerType !== "mouse") return;
      window.clearTimeout(leaveTimer);
      closeAll(item);
      setOpen(item, true);
    });
    item.addEventListener("pointerleave", (event) => {
      if (event.pointerType !== "mouse") return;
      leaveTimer = window.setTimeout(() => setOpen(item, false), 140);
    });
    item.addEventListener("focusout", (event) => {
      if (!item.contains(event.relatedTarget)) setOpen(item, false);
    });
    item.addEventListener("keydown", (event) => {
      if (event.key !== "Escape") return;
      setOpen(item, false);
      button?.focus();
    });
  });
  document.addEventListener("pointerdown", (event) => {
    if (!nav.contains(event.target)) {
      closeAll();
      if (nav.dataset.open === "true") setMobile(false);
    }
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && nav.dataset.open === "true") {
      setMobile(false);
      toggle?.focus();
    }
  });
  nav.querySelectorAll("a").forEach((link) => {
    link.addEventListener("click", () => {
      closeAll();
      setMobile(false);
    });
  });
})();
