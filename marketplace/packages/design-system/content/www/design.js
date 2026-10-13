(() => {
  const ROOT = "/design/";
  const COLOR_VALUE = /^(#[0-9a-f]{3,8}|rgba?\(|hsla?\(|linear-gradient\(|transparent$)/i;
  const QT_SURFACES = {
    dark: { background: "#0d1117", color: "#e6edf3", border: "#30363d", label: "Desktop dark" },
    light: { background: "#f6f8fa", color: "#1f2328", border: "#d0d7de", label: "Desktop light" },
  };
  const WEIGHTS = { regular: 400, medium: 500, emphasis: 600 };
  const SETS = {
    web: "Web · Lucide",
    desktop: "Desktop",
    ios: "iPhone · Material",
    android: "Android · Material",
    sf: "iPhone · SF Symbols",
  };

  function el(tag, attrs, children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs || {})) {
      if (value === null || value === undefined || value === false) continue;
      if (key === "text") node.textContent = value;
      else if (key === "style" && typeof value === "object") {
        for (const [name, rule] of Object.entries(value)) {
          if (name.startsWith("--")) node.style.setProperty(name, rule);
          else node.style[name] = rule;
        }
      }
      else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
      else node.setAttribute(key, value === true ? "" : String(value));
    }
    for (const child of [].concat(children || [])) {
      if (child === null || child === undefined || child === false) continue;
      node.append(child instanceof Node ? child : document.createTextNode(String(child)));
    }
    return node;
  }

  let copiedNotice = null;

  function copyNotice() {
    return el("span", { class: "ds-copied", role: "status", "aria-live": "polite" });
  }

  function showCopied(button, message, tone) {
    const notice = button && button.querySelector(".ds-copied");
    if (copiedNotice && copiedNotice !== notice) {
      copiedNotice.textContent = "";
      copiedNotice.removeAttribute("data-tone");
    }
    if (!notice) return;
    notice.textContent = message;
    notice.dataset.tone = tone;
    copiedNotice = notice;
  }

  async function copy(text, button) {
    try {
      await navigator.clipboard.writeText(text);
      showCopied(button, "Copied", "good");
    } catch (error) {
      showCopied(button, "Couldn't copy: select the name and copy it", "bad");
    }
  }

  function heading(title, source, intro) {
    return [
      el("h2", { text: title }),
      source ? el("p", { class: "ds-source", text: `Read from ${source}` }) : null,
      intro ? el("p", { class: "ds-intro", text: intro }) : null,
    ].filter(Boolean);
  }

  function swatch(name, light, dark, detail) {
    const chips = el("div", { class: "ds-swatch-chips" });
    if (light !== undefined && light !== null) {
      chips.append(el("span", { style: { background: light } }, light === dark || dark === undefined || dark === null ? null : el("em", { text: "LIGHT" })));
    }
    if (dark !== undefined && dark !== null && dark !== light) {
      chips.append(el("span", { style: { background: dark } }, el("em", { text: "DARK" })));
    }
    const values = light === dark || dark === undefined || dark === null ? [light] : [light, dark];
    return el("button", {
      type: "button",
      class: "ds-swatch",
      title: `Copy ${name}`,
      onclick: (event) => copy(name, event.currentTarget),
      style: { padding: 0, textAlign: "left", font: "inherit", color: "inherit", cursor: "copy" },
    }, [
      chips,
      el("div", { class: "ds-swatch-meta" }, [
        el("strong", { text: name }),
        el("code", { text: values.join(" · ") }),
        detail ? el("div", { text: detail, style: { color: "var(--muted-2)", fontSize: "11px" } }) : null,
        copyNotice(),
      ]),
    ]);
  }

  function table(headers, rows) {
    return el("div", { class: "ds-scroll" }, el("table", { class: "ds-table" }, [
      el("thead", {}, el("tr", {}, headers.map((label) => el("th", { text: label })))),
      el("tbody", {}, rows.map((cells) => el("tr", {}, cells.map((cell) => el("td", {}, cell))))),
    ]));
  }

  function code(text) {
    return el("code", { text: String(text) });
  }

  function iconTile(icon) {
    let glyph;
    if (icon.kind === "lucide") {
      glyph = el("i", { "data-lucide": icon.name, "aria-hidden": "true" });
    } else if (icon.kind === "svg") {
      glyph = el("span", { class: "ds-mask", style: { "--ds-icon": `url("${ROOT}${icon.file}")` } });
    } else if (icon.kind === "png") {
      glyph = el("img", { src: ROOT + icon.file, alt: "", loading: "lazy" });
    } else if (icon.kind === "material") {
      glyph = el("span", { class: "ds-material", "aria-hidden": "true" }, String.fromCodePoint(parseInt(icon.codepoint, 16)));
    } else {
      glyph = el("span", { class: "ds-symbol", title: "SF Symbols draw only on Apple devices" }, "SF");
    }
    const uses = icon.uses ? ` · used ${icon.uses}×` : "";
    return el("button", {
      type: "button",
      class: "ds-icon",
      title: `${icon.name} (${SETS[icon.set]}${uses}). Click to copy the name.`,
      "data-name": icon.name,
      "data-set": icon.set,
      onclick: (event) => copy(icon.name, event.currentTarget),
    }, [
      el("span", { class: "ds-glyph" }, glyph),
      el("span", { class: "ds-icon-name", text: icon.name }),
      el("span", { class: "ds-icon-set", text: icon.family || SETS[icon.set] }),
      copyNotice(),
    ]);
  }

  function iconBrowser(icons, options) {
    const grid = el("div", { class: "ds-icon-grid" });
    const count = el("span", { class: "ds-source", style: { margin: 0 } });
    const search = el("input", {
      type: "search",
      placeholder: `Search ${icons.length} icons by name`,
      "aria-label": "Search icons",
    });
    let set = "all";
    const sets = [...new Set(icons.map((icon) => icon.set))];
    const picker = sets.length > 1 ? el("select", {
      "aria-label": "Icon set",
      style: { padding: "8px", borderRadius: "var(--radius)", background: "var(--card)", color: "var(--foreground)", border: "1px solid var(--border)" },
      onchange: (event) => { set = event.target.value; draw(); },
    }, [el("option", { value: "all", text: "Every set" })].concat(
      sets.map((value) => el("option", { value, text: SETS[value] })),
    )) : null;
    function draw() {
      const query = search.value.trim().toLowerCase();
      const shown = icons.filter((icon) =>
        (set === "all" || icon.set === set) && (!query || icon.name.toLowerCase().includes(query)));
      grid.replaceChildren(...shown.map(iconTile));
      count.textContent = `${shown.length} of ${icons.length}`;
      if (window.lucide && shown.some((icon) => icon.kind === "lucide")) {
        window.lucide.createIcons({ attrs: { "stroke-width": 1.75 } });
      }
    }
    search.addEventListener("input", draw);
    window.addEventListener("ds:lucide", draw);
    draw();
    return el("div", {}, [
      el("div", { class: "ds-filter" }, [search, picker, count]),
      options && options.note ? el("p", { class: "ds-intro", text: options.note }) : null,
      grid,
    ]);
  }

  function webIcons(data) {
    return data.web.icons.map((icon) => ({ ...icon, set: "web", kind: "lucide", family: "lucide" }));
  }

  function desktopIcons(data) {
    return data.desktop.icons.map((icon) => ({ ...icon, set: "desktop", kind: icon.format }));
  }

  function materialIcons(rows, set) {
    return rows.map((icon) => ({ ...icon, set, kind: "material", family: "material" }));
  }

  function sfIcons(data) {
    return data.ios.sfSymbols.map((icon) => ({ ...icon, set: "sf", kind: "sf", family: "SF Symbols" }));
  }

  function renderWeb(section, data) {
    const web = data.web;
    const colors = web.tokens.filter((token) => COLOR_VALUE.test(token.dark));
    const others = web.tokens.filter((token) => !COLOR_VALUE.test(token.dark));
    const demoButton = (cls) => {
      const classes = cls.split(" ");
      const label = classes.map((name) => `.${name}`).join("");
      if (classes[0] === "icon-button") {
        return el("button", { type: "button", class: cls, "aria-label": label, title: label }, el("i", { "data-lucide": "settings" }));
      }
      return el("button", { type: "button", class: cls }, classes[0] === "kit-btn" ? [el("i", { "data-lucide": "download" }), label] : label);
    };
    const buttonClasses = [...web.buttonClasses];
    if (buttonClasses.includes("kit-btn")) buttonClasses.splice(buttonClasses.indexOf("kit-btn") + 1, 0, "kit-btn is-primary");
    section.append(
      ...heading("Web", web.source, "forkmesh.com and app.forkmesh.com share these tokens. The page you are reading uses them, so the light and dark toggle in the header switches every specimen below."),
      el("h3", { text: `Colour tokens (${colors.length})` }),
      el("div", { class: "ds-swatches" }, colors.map((token) => swatch(token.name, token.light, token.dark))),
      el("h3", { text: "Type" }),
      table(["Token", "Specimen", "Stack"], others.filter((token) => ["--sans", "--mono", "--display"].includes(token.name)).map((token) => [
        code(token.name),
        el("span", { style: { fontFamily: `var(${token.name})`, fontSize: "20px" }, text: "Build it once. Ship it to the mesh. 0123456789" }),
        el("code", { text: token.dark, style: { fontSize: "11px", color: "var(--muted)" } }),
      ])),
      el("h3", { text: "Other tokens" }),
      table(["Token", "Dark", "Light"], others.filter((token) => !["--sans", "--mono", "--display"].includes(token.name)).map((token) => [
        code(token.name), code(token.dark), code(token.light),
      ])),
      el("h3", { text: `Buttons (${buttonClasses.length})` }),
      el("p", { class: "ds-intro", text: "Every button class in styles.css, drawn live with the site's own stylesheet." }),
      el("div", { class: "ds-panel is-site" }, el("div", { class: "ds-button-list" }, buttonClasses.map((cls) =>
        el("div", { class: "ds-button-row" }, [
          el("div", { class: "ds-button-name", text: `.${cls.replace(" ", ".")}` }),
          el("div", { class: "ds-button-states" }, [demoButton(cls), (() => {
            const disabled = demoButton(cls);
            disabled.disabled = true;
            return el("figure", {}, [disabled, el("figcaption", { text: "disabled" })]);
          })()]),
        ])))),
      el("h3", { text: `Icons (${web.icons.length})` }),
      iconBrowser(webIcons(data), { note: `Lucide ${web.script.match(/[\d.]+(?=\.min)/)?.[0] || ""}, as the dashboard and package pages name them with data-lucide.` }),
    );
  }

  function qtStyle(declarations) {
    const style = {};
    for (const [name, value] of Object.entries(declarations)) {
      if (/palette\(|url\(/.test(value)) continue;
      style[name] = value;
    }
    return style;
  }

  function stateRank(state) {
    return ["default", ":hover", ":pressed", ":checked", ":focus", ":disabled"].indexOf(state) >>> 0;
  }

  function qtVariantStates(button, buttons, theme) {
    const base = buttons.find((row) => !row.name && !row.attributes);
    const named = button.attributes && button.name ? buttons.find((row) => row.name === button.name && !row.attributes) : null;
    const layers = [base, named, button].filter((row, index, all) => row && all.indexOf(row) === index);
    const pick = (row, state) => (row.states[state] && row.states[state][theme]) || {};
    const states = Object.keys(button.states).sort((a, b) => stateRank(a) - stateRank(b) || a.localeCompare(b));
    return states.map((state) => {
      const merged = {};
      for (const row of layers) Object.assign(merged, pick(row, "default"));
      if (state !== "default") {
        for (const row of layers) {
          for (const [key, value] of Object.entries(row.states)) {
            if (key !== "default" && state.includes(key)) Object.assign(merged, value[theme] || {});
          }
        }
      }
      return { state, style: qtStyle(merged) };
    });
  }

  function qtLabel(button) {
    if (!button.name && !button.attributes) return "QPushButton";
    if (!button.name) return `QPushButton${button.attributes}`;
    return `#${button.name}${button.attributes}`;
  }

  function qtPanel(theme, buttons, query) {
    const surface = QT_SURFACES[theme];
    const rows = buttons.filter((button) => !query || qtLabel(button).toLowerCase().includes(query));
    return el("div", { class: "ds-panel", style: { background: surface.background, color: surface.color, borderColor: surface.border } }, [
      el("h4", { text: surface.label }),
      el("div", { class: "ds-button-list" }, rows.map((button) => el("div", { class: "ds-button-row" }, [
        el("div", { class: "ds-button-name" }, [
          qtLabel(button),
          button.uses ? el("small", { text: `used ${button.uses}×` }) : null,
        ]),
        el("div", { class: "ds-button-states" }, qtVariantStates(button, buttons, theme).map(({ state, style }) =>
          el("figure", {}, [
            el("button", {
              type: "button",
              class: "ds-qt-button",
              style: { color: surface.color, ...style },
            }, button.name ? button.name.replace(/([a-z])([A-Z])/g, "$1 $2").replace(/^./, (c) => c.toUpperCase()) : "Button"),
            el("figcaption", { text: state.replace(/^:/, "") }),
          ]))),
      ]))),
    ]);
  }

  function renderDesktop(section, data) {
    const desktop = data.desktop;
    const holder = el("div", { class: "ds-panels" });
    const search = el("input", { type: "search", placeholder: `Filter ${desktop.buttons.length} button styles by object name`, "aria-label": "Filter desktop buttons" });
    const draw = () => {
      const query = search.value.trim().toLowerCase();
      holder.replaceChildren(qtPanel("dark", desktop.buttons, query), qtPanel("light", desktop.buttons, query));
    };
    search.addEventListener("input", draw);
    draw();
    section.append(
      ...heading("Desktop", desktop.source, "The Qt desktop app draws with one application palette and two style sheets, dark and light. Buttons are QPushButtons styled by object name; set one with setObjectName and the matching rule below applies."),
      el("h3", { text: `Palette (${desktop.roles.length} roles)` }),
      el("div", { class: "ds-swatches" }, desktop.roles.map((role) => swatch(role.role, role.light, role.dark))),
      el("h3", { text: "Named colours" }),
      el("div", { class: "ds-swatches" }, desktop.constants.map((row) => swatch(row.name, row.value, row.value))
        .concat(desktop.senders.map((color, index) => swatch(`kSenderPalette[${index}]`, color, color, "Chat sender names")))),
      el("h3", { text: "Type" }),
      table(["Setting", "Value"], [
        [el("span", { text: "Interface font, first installed" }), el("span", { text: desktop.fonts.join(" → ") })],
        [el("span", { text: "Base size (QWidget)" }), code(desktop.baseFontSize ? `${desktop.baseFontSize}px` : "system")],
        [el("span", { text: "Button weight" }), code("600")],
      ]),
      el("h3", { text: `Buttons (${desktop.buttons.length})` }),
      el("p", { class: "ds-intro", text: "Each style sheet rule for QPushButton, drawn in both themes with every state the sheet defines. Counts are how many widgets the source names that way." }),
      el("div", { class: "ds-filter" }, search),
      holder,
      el("h3", { text: `Icons (${desktop.icons.length})` }),
      iconBrowser(desktopIcons(data), { note: "Every image icons.qrc compiles into the app: Octicons and ForkMesh drawings under :/icons/octicons, Codicons under :/icons/codicons, plus the app, membership and agent faces. Name an icon with setOcticon or themedOcticon." }),
    );
  }

  function scalarMap(rows) {
    return Object.fromEntries((rows || []).map((row) => [row.name, row.value]));
  }

  function pairColors(rows) {
    const map = new Map(rows.map((row) => [row.name, row.value]));
    const used = new Set();
    const out = [];
    for (const row of rows) {
      if (used.has(row.name)) continue;
      used.add(row.name);
      const cap = row.name.charAt(0).toUpperCase() + row.name.slice(1);
      const darkName = [`dark${cap}`, `${row.name}OnDark`].find((name) => map.has(name) && !used.has(name));
      const lightName = map.has(`${row.name}Light`) && !used.has(`${row.name}Light`) ? `${row.name}Light` : null;
      if (darkName) {
        used.add(darkName);
        out.push({ name: row.name, light: row.value, dark: map.get(darkName), detail: `dark: ${darkName}` });
      } else if (lightName) {
        used.add(lightName);
        out.push({ name: row.name, light: map.get(lightName), dark: row.value, detail: `light: ${lightName}` });
      } else {
        out.push({ name: row.name, light: row.value, dark: row.value });
      }
    }
    return out;
  }

  function phonePalette(spec, theme) {
    const colors = Object.fromEntries(spec.colors.map((row) => [row.name, row.value]));
    const dark = theme === "dark";
    return {
      background: dark ? colors.darkBgBase : colors.bgBase,
      raised: dark ? colors.darkBgRaised : colors.bgRaised,
      text: dark ? colors.darkTextPrimary : colors.textPrimary,
      border: dark ? colors.darkBorder : colors.border,
      accent: dark ? colors.accentOnDark : colors.accent,
      success: dark ? colors.successOnDark : colors.success,
      warning: dark ? colors.warningOnDark : colors.warning,
      danger: dark ? colors.dangerOnDark : colors.danger,
    };
  }

  function tint(color, amount) {
    const match = /^#([0-9a-f]{6})$/i.exec(color || "");
    if (!match) return color;
    const value = parseInt(match[1], 16);
    return `rgba(${value >> 16}, ${(value >> 8) & 255}, ${value & 255}, ${amount})`;
  }

  function glyph(spec, name) {
    const code = (spec.materialGlyphs || {})[name];
    return code ? String.fromCodePoint(parseInt(code, 16)) : "";
  }

  function phoneButtons(spec, theme, platform) {
    const p = phonePalette(spec, theme);
    const radius = scalarMap(spec.radius);
    const space = scalarMap(spec.space);
    const dark = theme === "dark";
    const recipes = {
      FmFilledButton: () => ({ label: "Filled", style: { background: p.text, color: p.background, minHeight: "40px", padding: `0 ${space.x5}px`, borderRadius: `${radius.full}px` } }),
      FmElevatedButton: () => ({ label: "Elevated", style: { background: p.text, color: p.background, minHeight: "40px", padding: `0 ${space.x5}px`, borderRadius: `${radius.full}px`, boxShadow: "0 1px 3px rgba(0,0,0,.25)" } }),
      FmOutlinedButton: () => ({ label: "Outlined", style: { background: "transparent", color: p.text, minHeight: "40px", padding: `0 ${space.x5}px`, borderRadius: `${radius.full}px`, border: `1px solid ${p.border}` } }),
      FmTextButton: () => ({ label: "Text", style: { background: "transparent", color: p.accent, minHeight: "44px", padding: `0 ${space.x3}px`, borderRadius: `${radius.full}px` } }),
      FmIconButton: () => ({ label: "", icon: "more_vert", style: { background: "transparent", color: p.text, width: "44px", height: "44px", borderRadius: "50%" } }),
      FmFloatingActionButton: () => ({ label: "", icon: "add", style: { background: p.accent, color: "#ffffff", width: "56px", height: "56px", borderRadius: "14px", boxShadow: "0 2px 3px rgba(0,0,0,.25)" } }),
      FmExtendedFloatingActionButton: () => ({ label: "New", icon: "add", style: { background: p.accent, color: "#ffffff", minHeight: "56px", padding: `0 ${space.x4}px`, borderRadius: "14px", boxShadow: "0 2px 3px rgba(0,0,0,.25)" } }),
    };
    const tones = [["Accent", p.accent], ["Success", p.success], ["Warning", p.warning], ["Danger", p.danger], ["Neutral", p.text]];
    const items = [];
    for (const button of spec.buttons) {
      if (button.name === "FmToneButton") {
        for (const [label, color] of tones) {
          items.push({ name: `${button.name} · ${label}`, source: button.source, label, style: { background: tint(color, dark ? 0.2 : 0.13), color, minHeight: "34px", padding: `0 ${space.x3}px`, borderRadius: `${radius.md}px`, fontSize: "13px" } });
        }
        continue;
      }
      const recipe = recipes[button.name];
      if (recipe) items.push({ name: button.name, source: button.source, ...recipe() });
    }
    return el("div", { class: "ds-panel", style: { background: p.background, color: p.text, borderColor: p.border } }, [
      el("h4", { text: `${platform} ${theme}` }),
      el("div", { class: "ds-phone-buttons" }, items.map((item) => el("figure", { style: { margin: 0, display: "grid", gap: "4px", justifyItems: "start" } }, [
        el("button", { type: "button", class: "ds-phone-button", style: item.style, title: item.source }, [
          item.icon ? el("span", { class: "ds-material", style: { fontFamily: "ForkMesh Material Icons", fontSize: "20px" } }, glyph(spec, item.icon)) : null,
          item.label || null,
        ]),
        el("figcaption", { style: { fontSize: "10px", opacity: 0.6, fontFamily: "var(--mono)" }, text: item.name }),
      ]))),
    ]);
  }

  function renderPhone(section, data, key, title) {
    const spec = data[key];
    const type = scalarMap(spec.type);
    const sizes = (spec.type || []).filter((row) => row.value > 2);
    const roles = spec.textRoles && spec.textRoles.length ? spec.textRoles : null;
    const children = [
      ...heading(title, spec.source, key === "ios"
        ? "SwiftUI tokens from the Theme folder. Android carries the same scale; only the accent differs, and on iPhone the person picks it."
        : "Jetpack Compose tokens from ForkMeshTheme.kt, the same scale as iPhone with a fixed purple accent."),
      el("h3", { text: `Colours (${spec.colors.length})` }),
      el("div", { class: "ds-swatches" }, pairColors(spec.colors).map((row) => swatch(row.name, row.light, row.dark, row.detail))),
    ];
    if (spec.accents && spec.accents.length) {
      children.push(
        el("h3", { text: "Accent choices" }),
        el("div", { class: "ds-swatches" }, spec.accents.map((row) => swatch(row.name, row.light, row.dark, `subtle ${row.subtle}`))),
      );
    }
    children.push(el("h3", { text: "Type" }));
    if (roles) {
      children.push(el("div", {}, roles.map((role) => {
        const size = type[role.size] || 15;
        const height = type[role.height] || parseFloat(role.height) || 1.3;
        return el("div", { class: "ds-type-row" }, [
          el("div", {}, [el("strong", { text: role.role, style: { display: "block", fontSize: "13px" } }), code(`${size}pt · ${WEIGHTS[role.weight] || role.weight} · ${height}× · ${role.tone}`)]),
          el("div", { style: { fontFamily: "-apple-system, BlinkMacSystemFont, 'SF Pro Text', sans-serif", fontSize: `${size}px`, fontWeight: WEIGHTS[role.weight] || 400, lineHeight: height, letterSpacing: `${role.tracking}px`, opacity: role.tone === "primary" ? 1 : role.tone === "secondary" ? 0.7 : 0.5 }, text: "Ship it to the mesh" }),
        ]);
      })));
    } else {
      children.push(el("div", {}, sizes.map((row) => el("div", { class: "ds-type-row" }, [
        code(`${row.name} · ${row.value}sp`),
        el("div", { style: { fontFamily: "Roboto, system-ui, sans-serif", fontSize: `${row.value}px` }, text: "Ship it to the mesh" }),
      ]))));
    }
    const unit = key === "ios" ? "pt" : "dp";
    children.push(
      el("h3", { text: "Spacing" }),
      table(["Token", "Value", ""], spec.space.map((row) => [code(`FmSpace.${row.name}`), code(`${row.value}${unit}`), el("div", { class: "ds-bar", style: { width: `${row.value * 3}px` } })])),
      el("h3", { text: "Corner radius" }),
      table(["Token", "Value", ""], spec.radius.concat((spec.shapes || []).map((row) => ({ ...row, name: `Shapes.${row.name}` }))).map((row) => [
        code(row.name.startsWith("Shapes.") ? row.name : `FmRadius.${row.name}`),
        code(`${row.value}${unit}`),
        el("div", { class: "ds-radius", style: { borderRadius: `${Math.min(row.value, 28)}px` } }),
      ])),
      el("h3", { text: "Motion" }),
      table(["Token", "Duration"], spec.motion.map((row) => [code(`FmMotion.${row.name}`), code(key === "ios" ? `${row.value * 1000} ms` : `${row.value} ms`)])),
      el("h3", { text: `Buttons (${spec.buttons.length})` }),
      el("p", { class: "ds-intro", text: "Drawn here from the token values above; the apps' own drawing is the reference. Hover a button for the file that defines it." }),
      el("div", { class: "ds-panels" }, [phoneButtons(spec, "light", title), phoneButtons(spec, "dark", title)]),
      table(["Component", "Defined in"], spec.buttons.map((button) => [code(button.name), code(button.source)])),
      el("h3", { text: `Material icons (${spec.materialIcons.length})` }),
      iconBrowser(materialIcons(spec.materialIcons, key), { note: "Glyphs from the Material Icons font bundled with the app, by the names the code passes to FmIcon." }),
    );
    if (key === "ios") {
      children.push(
        el("h3", { text: `SF Symbols (${spec.sfSymbols.length})` }),
        el("p", { class: "ds-intro", text: "Apple's symbols draw only on Apple devices, so they are listed by name. Open the SF Symbols app to see them." }),
        el("div", { class: "ds-chip-list" }, spec.sfSymbols.map((row) => el("code", { title: `used ${row.uses}×`, text: row.name }))),
      );
    }
    section.append(...children);
  }

  function renderIcons(section, data) {
    const all = [
      ...desktopIcons(data),
      ...webIcons(data),
      ...materialIcons(data.ios.materialIcons, "ios"),
      ...materialIcons(data.android.materialIcons, "android"),
      ...sfIcons(data),
    ];
    section.append(
      ...heading("All icons", null, `Every icon ForkMesh ships, ${all.length} in all. Search by name and pick a set; click any icon to copy its name.`),
      iconBrowser(all),
    );
  }

  function loadMaterialFont(data) {
    if (!data.materialFont) return;
    const style = document.createElement("style");
    style.textContent = `@font-face { font-family: "ForkMesh Material Icons"; src: url("${ROOT}${data.materialFont}") format("opentype"); font-display: block; }`;
    document.head.append(style);
  }

  function counts(data) {
    const total = data.desktop.icons.length + data.web.icons.length + data.ios.materialIcons.length +
      data.android.materialIcons.length + data.ios.sfSymbols.length;
    const buttons = data.desktop.buttons.length + data.web.buttonClasses.length + data.ios.buttons.length + data.android.buttons.length;
    return `${total} icons · ${buttons} button styles · 4 platforms`;
  }

  function markTabs() {
    const current = (location.hash || "#web").slice(1);
    document.querySelectorAll("[data-ds-tab]").forEach((tab) => {
      tab.setAttribute("aria-current", tab.dataset.dsTab === current ? "true" : "false");
    });
  }

  async function start() {
    const status = document.querySelector("[data-ds-status]");
    let data;
    try {
      const response = await fetch("/design/inventory.json?v=${version}", { credentials: "omit" });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      data = await response.json();
    } catch (error) {
      if (status) status.textContent = `The design system could not load (${error.message}). Reload to try again.`;
      return;
    }
    loadMaterialFont(data);
    const sections = Object.fromEntries([...document.querySelectorAll("[data-ds-section]")].map((node) => [node.dataset.dsSection, node]));
    renderWeb(sections.web, data);
    renderDesktop(sections.desktop, data);
    renderPhone(sections.ios, data, "ios", "iPhone");
    renderPhone(sections.android, data, "android", "Android");
    renderIcons(sections.icons, data);
    Object.values(sections).forEach((node) => { node.hidden = false; });
    const countLine = document.querySelector("[data-ds-counts]");
    if (countLine) countLine.textContent = counts(data);
    if (status) status.remove();
    if (window.lucide) window.lucide.createIcons({ attrs: { "stroke-width": 1.75 } });
    else window.addEventListener("load", () => window.dispatchEvent(new Event("ds:lucide")), { once: true });
    markTabs();
    if (location.hash) document.getElementById(location.hash.slice(1))?.scrollIntoView();
  }

  window.addEventListener("hashchange", markTabs);
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", start);
  else start();
})();
