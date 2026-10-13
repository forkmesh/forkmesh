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
        background: "rgb(var(--dashboard-background-rgb) / <alpha-value>)",
        foreground: "rgb(var(--dashboard-foreground-rgb) / <alpha-value>)",
        card: "rgb(var(--dashboard-card-rgb) / <alpha-value>)",
        "card-foreground": "rgb(var(--dashboard-card-foreground-rgb) / <alpha-value>)",
        popover: "rgb(var(--dashboard-popover-rgb) / <alpha-value>)",
        "popover-foreground": "rgb(var(--dashboard-popover-foreground-rgb) / <alpha-value>)",
        primary: "rgb(var(--dashboard-primary-rgb) / <alpha-value>)",
        "primary-foreground": "rgb(var(--dashboard-primary-foreground-rgb) / <alpha-value>)",
        secondary: "rgb(var(--dashboard-secondary-rgb) / <alpha-value>)",
        "secondary-foreground": "rgb(var(--dashboard-secondary-foreground-rgb) / <alpha-value>)",
        muted: "rgb(var(--dashboard-muted-rgb) / <alpha-value>)",
        "muted-foreground": "rgb(var(--dashboard-muted-foreground-rgb) / <alpha-value>)",
        accent: "rgb(var(--dashboard-accent-rgb) / <alpha-value>)",
        "accent-foreground": "rgb(var(--dashboard-accent-foreground-rgb) / <alpha-value>)",
        destructive: "rgb(var(--dashboard-destructive-rgb) / <alpha-value>)",
        border: "rgb(var(--dashboard-border-rgb) / <alpha-value>)",
        input: "transparent",
        ring: "rgb(var(--dashboard-ring-rgb) / <alpha-value>)",
      },
    },
  },
};
