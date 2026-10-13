// Theme control for the marketing homepage. The homepage owns its hero
// navigation instead of mounting site-header.js, but its preference must stay
// in lockstep with every other public ForkMesh page and the dashboard.
(() => {
  const THEME_KEYS = ["forkmesh.dashboard.theme", "forkmesh.theme"];

  function storedTheme() {
    try {
      for (const key of THEME_KEYS) {
        const value = localStorage.getItem(key);
        if (value === "light" || value === "dark") return value;
      }
    } catch (_) {}
    return "";
  }

  // A saved choice (made on any public page or the dashboard) always wins.
  // A page can then pin its own default with data-theme-default on <html>:
  // the homepage pins "light" because its nav and hero are designed on
  // white. Pages without the attribute keep following the OS preference.
  function resolveTheme() {
    const saved = storedTheme();
    if (saved) return saved;
    const preset = document.documentElement.dataset.themeDefault;
    if (preset === "light" || preset === "dark") return preset;
    try {
      if (window.matchMedia("(prefers-color-scheme: light)").matches) return "light";
    } catch (_) {}
    return "dark";
  }

  function applyTheme(theme) {
    const light = theme === "light";
    const root = document.documentElement;
    root.classList.toggle("light", light);
    root.classList.toggle("dark", !light);
    root.style.colorScheme = light ? "light" : "dark";
    document.querySelector('meta[name="theme-color"]')?.setAttribute(
      "content", light ? "#ffffff" : "#090909",
    );

    document.querySelectorAll("[data-home-theme]").forEach((button) => {
      const sun = button.querySelector(".home-theme-sun");
      const moon = button.querySelector(".home-theme-moon");
      // toggleAttribute, not `.hidden`: these are <svg> elements, and the
      // `hidden` IDL property belongs to HTMLElement. Assigning it on an SVG
      // sets a plain expando and never touches the attribute, so the icon
      // that ships hidden in the markup stayed hidden forever and the other
      // one stayed visible - the toggle showed the same icon in both themes,
      // or both icons at once on a page whose stylesheet does not enforce
      // the attribute.
      sun?.toggleAttribute("hidden", light);
      moon?.toggleAttribute("hidden", !light);
      const label = light ? "Switch to dark mode" : "Switch to light mode";
      button.setAttribute("aria-label", label);
      button.title = label;
    });
  }

  function saveTheme(theme) {
    try {
      THEME_KEYS.forEach((key) => localStorage.setItem(key, theme));
    } catch (_) {}
    applyTheme(theme);
  }

  document.querySelectorAll("[data-home-theme]").forEach((button) => {
    button.addEventListener("click", () => {
      saveTheme(document.documentElement.classList.contains("light")
        ? "dark"
        : "light");
    });
  });

  applyTheme(resolveTheme());
  window.addEventListener("storage", (event) => {
    if (THEME_KEYS.includes(event.key)) applyTheme(resolveTheme());
  });
  try {
    window.matchMedia("(prefers-color-scheme: light)")
      .addEventListener("change", () => {
        if (!storedTheme()) applyTheme(resolveTheme());
      });
  } catch (_) {}
})();
