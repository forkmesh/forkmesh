(() => {
  if (window.top !== window || globalThis.__forkmeshZohoSetup) return;
  globalThis.__forkmeshZohoSetup = true;

  const TICK_MS = 900;
  const AUTO_GAP_MS = 3500;
  const AUTO_TRIES = 4;
  const SCOPES = "ZohoMail.messages.READ,ZohoMail.messages.CREATE,ZohoMail.folders.READ,ZohoMail.accounts.READ";
  const FIELDS = ["clientId", "clientSecret", "code"];
  const NAMES = { clientId: "Client ID", clientSecret: "Client Secret", code: "Code" };
  const PATTERN = {
    clientId: /^1000\.[A-Za-z0-9]{10,64}$/,
    clientSecret: /^[A-Za-z0-9]{20,100}$/,
    code: /^1000\.[A-Za-z0-9]{10,100}\.[A-Za-z0-9]{10,100}$/,
  };
  const TOKEN = /^[A-Za-z0-9.]{20,220}$/;
  const CLICKABLE = "button, a[href], [role=button], [role=tab], [role=link], [role=menuitem], [role=option], li[tabindex], input[type=submit], input[type=button]";
  const DIALOGS = "[role=dialog], [role=alertdialog], dialog[open], [aria-modal=true], .modal, .popup, .zmodal, .zdialog";
  const TEXT_INPUT = "input:not([type=radio]):not([type=checkbox]):not([type=hidden]):not([type=submit]):not([type=button]), textarea";
  const WORDS = {
    generateTab: ["generate code"],
    secretTab: ["client secret"],
    selfClient: ["self client", "self-client"],
    createNow: ["create now", "create"],
    addClient: ["add client", "+ add client", "get started", "add new client", "create client"],
    create: ["create", "generate", "create code", "generate code"],
    confirm: ["ok", "yes", "confirm", "enable", "create", "continue", "next", "submit"],
    close: ["close", "ok", "done", "×", "✕", "x"],
  };
  const RISKY = /delete|remove|revoke|regenerate|reset|disable/;

  let setup = null;
  let stopped = false;
  let paused = false;
  let minimized = false;
  let hostNode = null;
  let parts = null;
  let timer = 0;
  let observer = null;
  let scheduled = 0;
  let target = null;
  let lastValues = "";
  let lastView = "";
  let notice = "";
  let lastAuto = { key: "", at: 0 };
  let tries = {};
  const sentCodes = new Set();

  function send(message) {
    return new Promise((resolve) => {
      try {
        if (!chrome.runtime?.id) {
          stopped = true;
          resolve(null);
          return;
        }
        chrome.runtime.sendMessage(message, (response) => {
          resolve(chrome.runtime.lastError ? null : response || null);
        });
      } catch {
        stopped = true;
        resolve(null);
      }
    });
  }

  const clean = (value) => String(value || "").replace(/\s+/g, " ").trim();
  const norm = (value) => clean(String(value || "").replace(/\([^)]*\)/g, " ")).toLowerCase().replace(/[\s:*✓]+$/g, "").trim();
  const matches = (text, words) => words.some((word) => (word instanceof RegExp ? word.test(text) : text === word));
  const ours = (node) => Boolean(hostNode && node && (node === hostNode || hostNode.contains(node)));
  const before = (a, b) => Boolean(a.compareDocumentPosition(b) & Node.DOCUMENT_POSITION_FOLLOWING);
  const have = (...names) => names.every((name) => setup?.keys?.[name]);

  function visible(node) {
    if (!node?.isConnected || ours(node) || !node.getBoundingClientRect) return false;
    const box = node.getBoundingClientRect();
    if (box.width < 2 || box.height < 2) return false;
    const style = getComputedStyle(node);
    return style.visibility !== "hidden" && style.display !== "none" && Number(style.opacity || 1) > 0.05;
  }

  const textOf = (node) => norm(node.innerText || node.textContent || node.getAttribute?.("aria-label") || node.getAttribute?.("title") || node.value || "");
  const scopeOf = (scope) => (scope === document ? document.body : scope);
  const pageText = () => norm(document.body?.innerText || "").slice(0, 20000);

  function clickables(scope = document) {
    const root = scopeOf(scope);
    return root ? [...root.querySelectorAll(CLICKABLE)].filter(visible) : [];
  }

  const findClickable = (words, scope = document) => clickables(scope).find((node) => matches(textOf(node), words)) || null;

  function labels(words, scope = document) {
    const root = scopeOf(scope);
    if (!root) return [];
    const found = [];
    for (const node of root.querySelectorAll("*")) {
      if (node.childElementCount > 4 || ["SCRIPT", "STYLE", "INPUT", "TEXTAREA", "SELECT", "svg", "path"].includes(node.tagName)) continue;
      const text = node.textContent;
      if (!text || text.length > 90 || !matches(norm(text), words) || !visible(node)) continue;
      found.push(node);
    }
    return found.filter((node) => !found.some((other) => other !== node && node.contains(other)));
  }

  const dialogs = () => [...document.querySelectorAll(DIALOGS)].filter((node) => visible(node) && clean(node.innerText).length > 2);

  function tokens(scope) {
    const found = [];
    if (!scope) return found;
    const walker = document.createTreeWalker(scope, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      if (node.nodeType === Node.TEXT_NODE) {
        const value = clean(node.nodeValue);
        if (TOKEN.test(value) && visible(node.parentElement)) found.push({ node, value });
      } else if (["INPUT", "TEXTAREA"].includes(node.tagName) || node.hasAttribute?.("data-value")) {
        if (ours(node)) continue;
        const value = clean(node.value ?? node.getAttribute("data-value"));
        if (TOKEN.test(value)) found.push({ node, value });
      }
    }
    return found;
  }

  function valueNear(words, pattern, avoid = []) {
    for (const label of labels(words)) {
      let box = label;
      for (let depth = 0; depth < 5 && box.parentElement && box.parentElement !== document.body; depth += 1) {
        box = box.parentElement;
        const hit = tokens(box).find(({ node, value }) => pattern.test(value) && !avoid.includes(value)
          && !label.contains(node) && before(label, node));
        if (hit) return hit.value;
      }
    }
    return "";
  }

  function readCode() {
    const scopes = [...dialogs(), document.body].filter(Boolean);
    for (const scope of scopes) {
      const hit = tokens(scope).find(({ value }) => PATTERN.code.test(value));
      if (hit) return hit.value;
    }
    return "";
  }

  function readValues() {
    const values = {};
    const code = readCode();
    if (code) values.code = code;
    if (labels(["client secret"]).length && labels(["client id"]).length) {
      const clientId = valueNear(["client id"], PATTERN.clientId);
      if (clientId) values.clientId = clientId;
      const secret = valueNear(["client secret"], PATTERN.clientSecret, [clientId]);
      if (secret && !secret.includes(".")) values.clientSecret = secret;
    }
    return values;
  }

  function setValue(input, value) {
    const proto = input.tagName === "TEXTAREA" ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, "value").set.call(input, value);
    input.dispatchEvent(new Event("input", { bubbles: true }));
    input.dispatchEvent(new Event("change", { bubbles: true }));
    input.dispatchEvent(new Event("blur", { bubbles: true }));
  }

  function near(label, selector) {
    if (label.tagName === "LABEL" && label.control) return label.control;
    const owner = label.closest("label");
    if (owner?.control) return owner.control;
    const id = label.getAttribute("for");
    if (id && document.getElementById(id)) return document.getElementById(id);
    let box = label;
    for (let depth = 0; depth < 4 && box.parentElement; depth += 1) {
      box = box.parentElement;
      const found = [...box.querySelectorAll(selector)].filter((node) => !ours(node) && visible(node));
      if (found.length === 1) return found[0];
      if (found.length > 1) return found.find((node) => before(label, node)) || null;
    }
    return null;
  }

  function inputFor(words, placeholder) {
    for (const label of labels(words)) {
      const input = near(label, TEXT_INPUT);
      if (input && visible(input) && !input.readOnly && !input.disabled) return input;
    }
    return [...document.querySelectorAll(TEXT_INPUT)].find((node) => !ours(node) && visible(node) && !node.readOnly
      && placeholder.test(String(node.getAttribute("placeholder") || node.getAttribute("aria-label") || node.name || ""))) || null;
  }

  const scopeInput = () => inputFor([/^scopes?$/, /^enter scopes?$/], /^(enter )?scopes?$/i);
  const descriptionInput = () => inputFor([/^scope description$/, /^description$/], /description/i);

  function pickDuration() {
    for (const label of labels([/^time duration$/, /^duration$/, /^expir/])) {
      const select = near(label, "select");
      if (!select) continue;
      const option = [...select.options].find((item) => /^10\s*min/i.test(clean(item.textContent)))
        || [...select.options].reduce((best, item) => (parseInt(item.value, 10) > parseInt(best?.value || "0", 10) ? item : best), null);
      if (option && select.value !== option.value) {
        select.value = option.value;
        select.dispatchEvent(new Event("change", { bubbles: true }));
      }
      return;
    }
  }

  function createButton(scope, generateTab) {
    let box = scope;
    for (let depth = 0; depth < 6 && box.parentElement; depth += 1) {
      box = box.parentElement;
      const found = clickables(box).filter((node) => node !== generateTab && matches(textOf(node), WORDS.create));
      if (found.length) return found.at(-1);
    }
    return [...clickables()].reverse().find((node) => node !== generateTab && matches(textOf(node), WORDS.create)) || null;
  }

  function click(node) {
    if (!node) return false;
    node.scrollIntoView({ block: "center", behavior: "smooth" });
    node.click();
    schedule(500);
    return true;
  }

  async function sendValues(values, submit = false) {
    if (values.code) sentCodes.add(values.code);
    const response = await send({ type: "fm-zoho-setup-keys", keys: values, submit });
    if (response?.setup) apply(response.setup);
    else if (response && !response.ok) notice = response.message || "The extension could not keep that value.";
    render(true);
  }

  function captureValues() {
    const values = readValues();
    if (values.code && sentCodes.has(values.code) && !have("code")) delete values.code;
    const fresh = Object.fromEntries(Object.entries(values).filter(([name, value]) => !setup.keys?.[name]?.endsWith(value.slice(-4))));
    const signature = JSON.stringify(fresh);
    if (signature === lastValues || !Object.keys(fresh).length) return;
    lastValues = signature;
    void sendValues(fresh);
  }

  function selfClientEntry() {
    for (const label of labels(WORDS.selfClient)) {
      const link = label.closest(CLICKABLE) || label.closest("[class*=client], [class*=card], li, tr");
      if (link && visible(link) && !ours(link)) return link;
    }
    return null;
  }

  function confirmDialog() {
    for (const box of dialogs()) {
      const text = norm(box.innerText);
      if (RISKY.test(text) || text.length > 400 || readCode()) continue;
      if ([...box.querySelectorAll(TEXT_INPUT)].some(visible) || findClickable([...WORDS.generateTab, ...WORDS.secretTab], box)) continue;
      const button = findClickable(WORDS.confirm, box);
      if (button) return button;
    }
    return null;
  }

  function usedCodeDialog() {
    if (have("code")) return null;
    for (const box of dialogs()) {
      const shown = tokens(box).find(({ value }) => PATTERN.code.test(value));
      if (!shown || !sentCodes.has(shown.value)) continue;
      return findClickable(WORDS.close, box) || box.querySelector("[aria-label*=lose i], [title*=lose i], .close, .zclose");
    }
    return null;
  }

  function guide() {
    const view = { title: "", text: "", warn: "", actions: [], target: null, auto: null, manual: false };
    const org = setup.organization;
    if (setup.state === "done") {
      view.title = "Zoho Mail connected";
      view.text = `${org}'s CRM reads and sends from account ${setup.accountId || "in Zoho Mail"} now. The CRM tab is syncing the shared boxes; you can close this tab.`;
      view.actions.push({ label: "Back to the CRM", primary: true, run: () => void send({ type: "fm-zoho-setup-back" }) });
      return view;
    }
    if (setup.state === "checking") {
      view.title = "Connecting…";
      view.text = "ForkMesh is redeeming the code with Zoho and reading the mail account.";
      return view;
    }
    if (setup.state === "error") {
      view.title = "Not connected yet";
      view.warn = `${setup.message}${setup.detail ? ` (${setup.detail})` : ""}.`;
      view.actions.push({ label: "Try again", primary: true, run: () => void send({ type: "fm-zoho-setup-restart" }).then((response) => { tries = {}; if (response?.setup) apply(response.setup); render(true); }) });
      view.actions.push({ label: "Back to the CRM", run: () => void send({ type: "fm-zoho-setup-back" }) });
      return view;
    }
    if (setup.rejected && !notice) notice = "Zoho refused the first code, so ForkMesh is making a new one.";
    const used = usedCodeDialog();
    if (used) {
      view.title = "Closing the old code";
      view.target = used;
      view.auto = { key: "close-code", run: () => click(used) };
      return view;
    }
    const confirm = confirmDialog();
    if (confirm) {
      view.title = "Confirming";
      view.text = `Pressing ${clean(confirm.innerText || confirm.value) || "OK"} in Zoho's window.`;
      view.target = confirm;
      view.auto = { key: `confirm:${textOf(confirm)}`, run: () => click(confirm) };
      return view;
    }
    const generateTab = findClickable(WORDS.generateTab);
    const secretTab = findClickable(WORDS.secretTab);
    if (generateTab || secretTab || scopeInput()) {
      if (!have("clientId", "clientSecret")) {
        view.title = "Client ID and secret";
        const shown = labels(["client id"]).length && labels(["client secret"]).length;
        if (shown) {
          view.text = "ForkMesh could not read them on this page. Copy each into the boxes below.";
          view.manual = true;
          return view;
        }
        view.text = "Opening the Self Client's Client Secret tab to read them.";
        view.target = secretTab;
        if (secretTab) view.auto = { key: "secret-tab", run: () => click(secretTab) };
        view.manual = !secretTab;
        return view;
      }
      if (!have("code")) {
        const scope = scopeInput();
        view.title = "A code for the four Mail scopes";
        if (scope) {
          view.text = "Filling in the scopes and a description, then pressing Create.";
          const run = () => {
            setValue(scope, SCOPES);
            const description = descriptionInput();
            if (description && description !== scope && !clean(description.value)) setValue(description, `ForkMesh CRM for ${org}`.slice(0, 60));
            pickDuration();
            setTimeout(() => {
              const create = createButton(scope, generateTab);
              if (create) click(create);
              else {
                notice = "Filled in. Press Zoho's Create button.";
                render(true);
              }
            }, 350);
          };
          view.auto = { key: "generate", run };
          view.actions.push({ label: "Make the code", run });
          return view;
        }
        view.text = "Opening the Generate Code tab.";
        view.target = generateTab;
        if (generateTab) view.auto = { key: "generate-tab", run: () => click(generateTab) };
        view.manual = !generateTab;
        return view;
      }
      view.title = "Everything is here";
      view.text = "Connecting now.";
      view.actions.push({ label: "Connect", primary: true, run: () => void sendValues({}, true) });
      return view;
    }
    const createNow = findClickable(WORDS.createNow);
    const card = selfClientEntry();
    if (/client type|choose a client|server-based|non-browser/.test(pageText()) && card) {
      view.title = "Make a Self Client";
      view.text = "Picking Self Client: it gives ForkMesh a code without a redirect address.";
      const inside = findClickable(WORDS.createNow, card.closest("[class*=card], li, div") || card);
      view.target = inside || card;
      view.auto = inside ? { key: "create-now", run: () => click(inside) } : { key: "self-card", run: () => { click(card); setTimeout(() => { const next = findClickable(WORDS.createNow); if (next) click(next); }, 500); } };
      return view;
    }
    if (card) {
      view.title = "Open the Self Client";
      view.text = "Your Zoho account already has a Self Client; ForkMesh uses it.";
      view.target = card;
      view.auto = { key: "self-open", run: () => click(card) };
      return view;
    }
    const add = findClickable(WORDS.addClient) || (createNow && !generateTab ? createNow : null);
    if (add) {
      view.title = "Make a Self Client";
      view.text = "No Self Client yet: adding one.";
      view.target = add;
      view.auto = { key: "add-client", run: () => click(add) };
      return view;
    }
    view.title = "Waiting for the API Console";
    view.text = "Sign in to Zoho if it asks. ForkMesh goes on once the console shows your clients.";
    view.actions.push({ label: "Open the console home", run: () => location.assign(`${location.origin}/`) });
    return view;
  }

  function autoRun(view) {
    if (!view.auto || paused || setup?.state !== "guiding") return;
    const now = Date.now();
    if (lastAuto.key === view.auto.key && now - lastAuto.at < AUTO_GAP_MS) return;
    tries[view.auto.key] = (tries[view.auto.key] || 0) + 1;
    if (tries[view.auto.key] > AUTO_TRIES) {
      view.manual = true;
      if (!notice) notice = "ForkMesh pressed this a few times without the page moving on. Press the highlighted control yourself, or paste the values below.";
      return;
    }
    lastAuto = { key: view.auto.key, at: now };
    view.auto.run();
  }

  function h(tag, props = {}, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(props)) {
      if (key === "on") for (const [event, handler] of Object.entries(value)) node.addEventListener(event, handler);
      else if (key === "class") node.className = value;
      else if (key in node) node[key] = value;
      else node.setAttribute(key, value);
    }
    for (const child of children.flat()) {
      if (child !== null && child !== undefined && child !== false) node.append(child);
    }
    return node;
  }

  const STYLE = `
    :host { all: initial; }
    .card { position: fixed; right: 16px; bottom: 16px; z-index: 2147483647; width: 340px; max-height: calc(100vh - 32px); overflow: auto;
      box-sizing: border-box; background: #fff; color: #0f172a; border: 1px solid #cbd5e1; border-radius: 12px;
      box-shadow: 0 8px 28px rgba(0,0,0,.22); font: 13px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
    .card.small .body { display: none; }
    .head { display: flex; align-items: center; gap: 8px; padding: 10px 12px; border-bottom: 1px solid #e2e8f0; }
    .head b { flex: 1; font-size: 13px; }
    .icon { border: 0; background: transparent; cursor: pointer; font: inherit; color: #475569; padding: 2px 6px; border-radius: 6px; }
    .icon:hover { background: #f1f5f9; }
    .body { padding: 10px 12px 12px; }
    ol { margin: 0 0 10px; padding: 0; list-style: none; }
    li { display: flex; gap: 8px; padding: 2px 0; color: #475569; }
    li.done { color: #15803d; }
    li.now { color: #0f172a; font-weight: 600; }
    li span:first-child { width: 14px; text-align: center; }
    h3 { margin: 0 0 4px; font-size: 14px; }
    p { margin: 0 0 8px; }
    .warn { background: #fff7ed; border: 1px solid #fed7aa; border-radius: 8px; padding: 6px 8px; }
    .notice { background: #eff6ff; border-radius: 8px; padding: 6px 8px; }
    .actions { display: flex; flex-wrap: wrap; gap: 6px; margin: 8px 0 4px; }
    button.act { border: 1px solid #cbd5e1; background: #fff; color: #0f172a; border-radius: 8px; padding: 5px 10px; font: inherit; font-weight: 600; cursor: pointer; }
    button.act.primary { background: #2563eb; border-color: #2563eb; color: #fff; }
    button.act:hover { filter: brightness(.95); }
    details { margin-top: 8px; border-top: 1px solid #e2e8f0; padding-top: 8px; }
    summary { cursor: pointer; color: #475569; }
    label { display: grid; gap: 2px; margin-top: 6px; color: #475569; font-size: 12px; }
    input { box-sizing: border-box; width: 100%; padding: 6px 8px; border: 1px solid #cbd5e1; border-radius: 6px; font: 12px ui-monospace, monospace; color: #0f172a; }
    .got { color: #15803d; font-size: 12px; }
    .ring { position: fixed; z-index: 2147483646; pointer-events: none; border: 3px solid #2563eb; border-radius: 10px;
      box-shadow: 0 0 0 4px rgba(37,99,235,.25); transition: all .15s ease; }
  `;

  function mount() {
    if (hostNode?.isConnected) return;
    hostNode = document.createElement("forkmesh-zoho-setup");
    const shadow = hostNode.attachShadow({ mode: "closed" });
    shadow.append(h("style", { textContent: STYLE }));
    const ring = h("div", { class: "ring", hidden: true });
    const title = h("b", { textContent: "Reconnect Zoho Mail" });
    const steps = h("ol");
    const now = h("div");
    const inputs = {};
    const pause = h("button", { class: "icon", type: "button", title: "Pause", textContent: "❚❚", on: { click: () => { paused = !paused; render(true); } } });
    const manual = h("details", {}, h("summary", { textContent: "Paste the values yourself" }),
      FIELDS.map((name) => {
        inputs[name] = h("input", {
          type: name === "clientSecret" ? "password" : "text", autocomplete: "off", spellcheck: false, placeholder: NAMES[name],
          on: { paste: () => setTimeout(() => routePaste(inputs[name], name), 0), change: () => routePaste(inputs[name], name) },
        });
        return h("label", {}, NAMES[name], inputs[name]);
      }),
      h("div", { class: "actions" }, h("button", { class: "act", type: "button", textContent: "Connect", on: { click: () => void sendValues({}, true) } })));
    const body = h("div", { class: "body" }, steps, now, manual);
    const card = h("div", { class: "card" },
      h("div", { class: "head" }, title, pause,
        h("button", { class: "icon", type: "button", title: "Minimize", textContent: "–", on: { click: () => { minimized = !minimized; render(true); } } }),
        h("button", { class: "icon", type: "button", title: "Stop the Zoho setup", textContent: "✕", on: { click: () => void stop() } })),
      body);
    shadow.append(ring, card);
    parts = { card, title, steps, now, manual, inputs, ring, pause };
    document.documentElement.append(hostNode);
  }

  function unmount() {
    hostNode?.remove();
    hostNode = null;
    parts = null;
    target = null;
  }

  function routePaste(input, name) {
    const value = clean(input.value).replace(/\s/g, "");
    if (!value) return;
    const field = PATTERN.code.test(value) ? "code" : PATTERN.clientId.test(value) ? "clientId" : PATTERN.clientSecret.test(value) ? "clientSecret" : "";
    if (!field) {
      notice = `That does not look like a ${NAMES[name]}.`;
      render(true);
      return;
    }
    input.value = "";
    if (field !== name) notice = `That looked like the ${NAMES[field]}, so it went there.`;
    void sendValues({ [field]: value });
  }

  async function stop() {
    await send({ type: "fm-zoho-setup-stop" });
    setup = null;
    unmount();
  }

  function placeRing() {
    if (!parts) return;
    const node = target?.isConnected && visible(target) ? target : null;
    if (!node || minimized) {
      parts.ring.hidden = true;
      return;
    }
    const box = node.getBoundingClientRect();
    Object.assign(parts.ring.style, { left: `${box.left - 6}px`, top: `${box.top - 6}px`, width: `${box.width + 12}px`, height: `${box.height + 12}px` });
    parts.ring.hidden = false;
  }

  function stepList() {
    const items = [
      ["Self Client in the API Console", have("clientId") || setup.state === "done"],
      ["Client ID and secret", have("clientId", "clientSecret") || setup.state === "done"],
      ["Code for reading and sending mail", have("code") || ["checking", "done"].includes(setup.state)],
      [`Saved in ${setup.organization}'s CRM`, setup.state === "done"],
    ];
    const current = items.findIndex(([, done]) => !done);
    return items.map(([label, done], index) => {
      const tail = index === 1 && done && setup.keys.clientId ? ` (${setup.keys.clientId})` : "";
      return h("li", { class: done ? "done" : index === current ? "now" : "" },
        h("span", { textContent: done ? "✓" : index === current ? "›" : "·" }), h("span", { textContent: label + tail }));
    });
  }

  function render(force = false) {
    if (!setup?.active || !parts) return;
    const view = guide();
    autoRun(view);
    target = view.target || (notice && target?.isConnected ? target : null);
    const signature = JSON.stringify([view.title, view.text, view.warn, view.actions.map((item) => item.label), view.manual,
      notice, minimized, paused, setup.state, setup.keys, setup.error]);
    placeRing();
    if (!force && signature === lastView) {
      parts.handlers = view.actions;
      return;
    }
    lastView = signature;
    parts.card.classList.toggle("small", minimized);
    parts.title.textContent = minimized ? `Zoho: ${view.title}` : "Reconnect Zoho Mail";
    parts.pause.textContent = paused ? "▶" : "❚❚";
    parts.pause.title = paused ? "Go on by itself" : "Pause";
    parts.steps.replaceChildren(...stepList());
    parts.handlers = view.actions;
    parts.now.replaceChildren(...[
      h("h3", { textContent: paused ? `Paused: ${view.title}` : view.title }),
      view.text ? h("p", { textContent: view.text }) : null,
      view.warn ? h("p", { class: "warn", textContent: view.warn }) : null,
      notice ? h("p", { class: "notice", textContent: notice }) : null,
      view.actions.length ? h("div", { class: "actions" }, view.actions.map((item, index) => h("button", {
        class: `act${item.primary ? " primary" : ""}`, type: "button", textContent: item.label,
        on: { click: () => { notice = ""; parts.handlers[index]?.run(); } },
      }))) : null].filter(Boolean));
    if (view.manual) parts.manual.open = true;
    for (const name of FIELDS) {
      const label = parts.inputs[name].parentElement;
      label.querySelector(".got")?.remove();
      if (setup.keys?.[name]) label.append(h("span", { class: "got", textContent: `✓ have ${setup.keys[name]}` }));
    }
  }

  function apply(next) {
    const was = setup;
    setup = next;
    if (!setup?.active) {
      unmount();
      return;
    }
    if (was?.state !== setup.state || was?.id !== setup.id) notice = "";
    if (was?.id !== setup.id) tries = {};
    mount();
  }

  function tick() {
    scheduled = 0;
    if (stopped || !setup?.active || document.visibilityState !== "visible") return;
    if (setup.state === "guiding") captureValues();
    render();
  }

  function schedule(delay = 250) {
    if (scheduled || stopped) return;
    scheduled = setTimeout(tick, delay);
  }

  function watch() {
    if (timer) return;
    timer = setInterval(tick, TICK_MS);
    observer = new MutationObserver(() => schedule());
    observer.observe(document.documentElement, { childList: true, subtree: true, characterData: true });
    addEventListener("scroll", placeRing, { passive: true, capture: true });
    addEventListener("resize", placeRing, { passive: true });
    document.addEventListener("visibilitychange", () => schedule(0));
  }

  chrome.runtime.onMessage.addListener((message) => {
    if (message?.type !== "fm-zoho-setup-changed") return;
    apply(message.setup);
    if (setup?.active) {
      watch();
      render(true);
    }
  });

  void send({ type: "fm-zoho-setup-state" }).then((response) => {
    if (!response?.setup?.active) return;
    apply(response.setup);
    watch();
    render(true);
  });
})();
