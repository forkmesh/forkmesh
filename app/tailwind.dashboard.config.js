const theme = require("./public/dashboard/theme.json");

module.exports = {
  // The dashboard's JS modules build markup as HTML strings, so their utility
  // classes have to be scanned too or they compile to nothing.
  content: [
    "./public/dashboard/**/*.html",
    "./public/dashboard/js/**/*.js",
    "./public/dashboard.js",
    // Composed from the same dashboard partials and served this stylesheet,
    // but they live outside the dashboard/ tree.
    "./public/microjobs/**/*.html",
    "./public/referral/**/*.html",
    // Marketplace packages mount their dashboard views into the same pages
    // and are served this stylesheet, so their markup is scanned as well.
    "../marketplace/packages/*/content/web/**/*.{html,js}",
  ],
  theme: {
    extend: {
      fontFamily: {
        sans: ["-apple-system", "BlinkMacSystemFont", "Segoe UI", "Noto Sans", "Helvetica", "Arial", "sans-serif"],
        mono: ["ForkMesh Dashboard Mono", "ui-monospace", "SFMono-Regular", "monospace"],
      },
      colors: {
        ...Object.fromEntries(
          Object.keys(theme.colors).map((name) => [name, `rgb(var(--dashboard-${name}-rgb) / <alpha-value>)`]),
        ),
        input: "transparent",
      },
      fontSize: {
        control: ["var(--dashboard-control-font)", { lineHeight: "1.4" }],
      },
      height: {
        control: "var(--dashboard-control-height)",
        "control-sm": "var(--dashboard-control-height-sm)",
        "menu-row": "var(--dashboard-menu-row-height)",
      },
      minHeight: {
        control: "var(--dashboard-control-height)",
        "control-sm": "var(--dashboard-control-height-sm)",
        "menu-row": "var(--dashboard-menu-row-height)",
      },
      borderRadius: {
        control: "var(--dashboard-control-radius)",
      },
    },
  },
};
