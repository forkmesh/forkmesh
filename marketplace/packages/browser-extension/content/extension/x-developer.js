(() => {
  if (window.top !== window || globalThis.__forkmeshXSetup) return;
  globalThis.__forkmeshXSetup = true;

  const TICK_MS = 900;
  const CONFIRM_MS = 10000;
  const FIELDS = ["apiKey", "apiSecret", "accessToken", "accessSecret"];
  const NAMES = { apiKey: "API key", apiSecret: "API key secret", accessToken: "Access token", accessSecret: "Access token secret" };
  const LABELS = {
    apiKey: ["api key", "consumer key", "api key consumer key"],
    apiSecret: ["api key secret", "api secret", "api secret key", "consumer secret", "api key secret consumer secret"],
    accessToken: ["access token"],
    accessSecret: ["access token secret"],
  };
  const READ = {
    apiKey: /^[A-Za-z0-9]{20,32}$/,
    apiSecret: /^[A-Za-z0-9]{40,60}$/,
    accessToken: /^\d{1,20}-[A-Za-z0-9]{10,64}$/,
    accessSecret: /^[A-Za-z0-9]{35,60}$/,
  };
  const PASTE = {
    apiKey: /^[A-Za-z0-9]{15,64}$/,
    apiSecret: /^[A-Za-z0-9]{30,80}$/,
    accessToken: /^\d{1,20}-[A-Za-z0-9]{10,64}$/,
    accessSecret: /^[A-Za-z0-9]{30,80}$/,
  };
  const TOKEN = /^[A-Za-z0-9-]{15,100}$/;
  const CLICKABLE = "button, a[href], [role=button], [role=tab], [role=link], [role=menuitem], input[type=submit]";
  const DIALOGS = "[role=dialog], [role=alertdialog], dialog[open], [aria-modal=true]";
  const WORDS = {
    signIn: ["sign in", "log in", "sign in with x", "log in with x", "sign in to x", "continue with x"],
    submit: ["submit", "continue", "next", "agree", "i agree", "accept", "let's do this", "save", "create"],
    apps: ["apps", "projects & apps", "projects and apps", "my apps"],
    createApp: [/^\+?\s*(create|add|new) (an )?app$/, /^\+?\s*create (a )?project$/, "create new app"],
    settingsTab: ["settings", "app settings", "user authentication settings"],
    keysTab: ["keys and tokens", "keys & tokens"],
    setUp: ["set up", "setup", "edit", "edit settings", "configure"],
    save: [/^save( changes)?$/, "update", "save settings"],
    generate: ["generate", "regenerate", "create", "generate token", "regenerate token"],
    apiGenerate: ["regenerate", "generate", "reveal", "regenerate keys", "create"],
    confirm: [/^yes,? regenerate/, "regenerate", "yes", "confirm", "generate", /^yes,? (change|save|update)/, "change permissions"],
    dismiss: [/^yes,? i (have )?saved (them|it)/, "i saved them", "i've saved them", "done", "got it", "close", "ok", "continue", "dismiss"],
    billing: ["billing", "credits", "buy credits", "add credits"],
  };
  const API_SECTION = ["api key and secret", "consumer keys", "consumer key and secret", "api keys", "oauth 1.0a keys"];
  const ACCESS_SECTION = ["access token and secret", "access tokens", "access token & secret"];
  const PERMISSION_LABEL = [/^read and write(?! and direct)/];
  const APP_TYPE_LABEL = [/^web app, automated app,? or bot/];
  const AUTH_SECTION = ["user authentication settings", "user authentication set up", "authentication settings"];

  let setup = null;
  let stopped = false;
  let hostNode = null;
  let shadow = null;
  let parts = null;
  let timer = 0;
  let observer = null;
  let scheduled = 0;
  let pending = null;
  let target = null;
  let lastKeys = "";
  let lastView = "";
  let notice = "";
  let minimized = false;
  let marking = false;

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

  function visible(node) {
    if (!node?.isConnected || ours(node) || !node.getBoundingClientRect) return false;
    const box = node.getBoundingClientRect();
    if (box.width < 2 || box.height < 2) return false;
    const style = getComputedStyle(node);
    return style.visibility !== "hidden" && style.display !== "none" && Number(style.opacity || 1) > 0.05;
  }

  const textOf = (node) => norm(node.innerText || node.textContent || node.getAttribute?.("aria-label") || node.value || "");
  const scopeOf = (scope) => (scope === document ? document.body : scope);

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

  const dialogs = () => [...document.querySelectorAll(DIALOGS)].filter(visible);
  const pageText = () => norm(document.body?.innerText || "").slice(0, 20000);

  function section(words, avoid = []) {
    for (const label of labels(words)) {
      let box = label;
      for (let depth = 0; depth < 6 && box?.parentElement && box.parentElement !== document.body; depth += 1) {
        const parent = box.parentElement;
        if (avoid.length && labels(avoid, parent).length) break;
        box = parent;
        if (clickables(box).length) return { label, box };
      }
      return { label, box };
    }
    return null;
  }

  function tokens(scope) {
    const found = [];
    const walker = document.createTreeWalker(scope, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_ELEMENT);
    for (let node = walker.nextNode(); node; node = walker.nextNode()) {
      if (node.nodeType === Node.TEXT_NODE) {
        const value = clean(node.nodeValue);
        if (TOKEN.test(value)) found.push({ node, value });
      } else if (["INPUT", "TEXTAREA"].includes(node.tagName) || node.hasAttribute?.("data-value")) {
        const value = clean(node.value ?? node.getAttribute("data-value"));
        if (TOKEN.test(value)) found.push({ node, value });
      }
    }
    return found;
  }

  function readKeys() {
    const keys = {};
    const scopes = dialogs();
    if (!scopes.length && document.body) scopes.push(document.body);
    for (const scope of scopes) {
      const all = FIELDS.flatMap((name) => labels(LABELS[name], scope).map((node) => ({ name, node })));
      for (const { name, node } of all) {
        if (keys[name]) continue;
        const next = all.filter((other) => other.node !== node && before(node, other.node));
        let box = node;
        for (let depth = 0; depth < 5 && box.parentElement && box !== scope; depth += 1) {
          box = box.parentElement;
          const hit = tokens(box).find(({ node: spot, value }) => READ[name].test(value)
            && !node.contains(spot) && before(node, spot)
            && !Object.values(keys).includes(value)
            && !next.some((other) => before(other.node, spot)));
          if (hit) {
            keys[name] = hit.value;
            break;
          }
        }
      }
    }
    return keys;
  }

  function setValue(input, value) {
    const proto = input.tagName === "TEXTAREA" ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, "value").set.call(input, value);
    input.dispatchEvent(new Event("input", { bubbles: true }));
    input.dispatchEvent(new Event("change", { bubbles: true }));
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
      const found = [...box.querySelectorAll(selector)].filter((node) => !ours(node));
      if (found.length === 1) return found[0];
      if (found.length > 1) return found.find((node) => before(label, node)) || null;
    }
    return null;
  }

  function choose(words) {
    for (const label of labels(words)) {
      const input = near(label, "input[type=radio], [role=radio]");
      const checked = input && (input.checked || input.getAttribute("aria-checked") === "true");
      if (!checked) (input || label).click();
      return true;
    }
    return false;
  }

  const TEXT_INPUT = "input:not([type=radio]):not([type=checkbox]):not([type=hidden]):not([type=submit]), textarea";

  function fill(words, value) {
    for (const label of labels(words)) {
      const input = near(label, TEXT_INPUT);
      if (!input || !visible(input) || input.readOnly || input.disabled) continue;
      if (!clean(input.value)) setValue(input, value);
      return true;
    }
    return false;
  }

  function click(node, confirm = null) {
    if (!node) return false;
    node.scrollIntoView({ block: "center", behavior: "smooth" });
    node.click();
    if (confirm) pending = { words: confirm, until: Date.now() + CONFIRM_MS };
    schedule(400);
    return true;
  }

  function confirmPending() {
    if (!pending) return;
    if (Date.now() > pending.until) {
      pending = null;
      return;
    }
    for (const box of dialogs()) {
      if (labels(FIELDS.flatMap((name) => LABELS[name]), box).length) continue;
      const button = findClickable(pending.words, box);
      if (button && !matches(textOf(button), ["cancel", "no", "back"])) {
        pending = null;
        button.click();
        schedule(500);
        return;
      }
    }
  }

  const origin = () => {
    try {
      return new URL(setup?.marketingUrl || "https://app.forkmesh.com").origin;
    } catch {
      return "https://app.forkmesh.com";
    }
  };

  function useCase() {
    return `We use the X API to publish posts from our own organization's account through ForkMesh Marketing, `
      + `the marketing dashboard of the "${setup?.organization || "our"}" organization. Owners and admins write posts there and send `
      + `each one to X themselves, confirming every post. We also read our own account's profile once to check which `
      + `account is connected. We do not read, analyze or store other people's posts or data, do not sell or share data, `
      + `and do not post on anyone else's behalf.`;
  }

  async function sendKeys(keys, submit = false) {
    const response = await send({ type: "fm-x-setup-keys", keys, submit });
    if (response?.setup) apply(response.setup);
    else if (response && !response.ok) notice = response.message || "The extension could not keep that key.";
    render(true);
  }

  async function mark(permission) {
    if (marking || Boolean(setup?.permission) === permission) return;
    marking = true;
    const response = await send({ type: "fm-x-setup-mark", permission });
    marking = false;
    if (response?.setup) apply(response.setup);
    render(true);
  }

  function captureKeys() {
    const keys = readKeys();
    const signature = JSON.stringify(keys);
    if (signature === lastKeys || !Object.keys(keys).length) return;
    lastKeys = signature;
    void sendKeys(keys);
  }

  const have = (...names) => names.every((name) => setup?.keys?.[name]);

  function isSignIn() {
    if (/\/(login|signin|i\/flow)/i.test(location.pathname)) return true;
    const button = findClickable(WORDS.signIn);
    return Boolean(button) && !findClickable(WORDS.apps) && !labels([...API_SECTION, ...ACCESS_SECTION, ...AUTH_SECTION]).length;
  }

  function isOnboarding() {
    const area = [...document.querySelectorAll("textarea")].find(visible);
    if (!area) return false;
    const text = pageText();
    return /developer agreement|use case|describe|how (will|do) you (plan to )?use/.test(text);
  }

  const isAuthForm = () => labels(PERMISSION_LABEL).length > 0 && /app permissions/.test(pageText());
  const isKeysPage = () => labels([...API_SECTION, ...ACCESS_SECTION, "bearer token", ...LABELS.apiKey, ...LABELS.accessToken]).length > 0;
  const apiSection = () => section(API_SECTION, [...ACCESS_SECTION, "bearer token"])
    || section([...LABELS.apiKey, ...LABELS.apiSecret], [...ACCESS_SECTION, ...LABELS.accessToken, "bearer token"]);
  const accessSection = () => section(ACCESS_SECTION, [...API_SECTION, "bearer token"])
    || section([...LABELS.accessToken, ...LABELS.accessSecret], [...API_SECTION, ...LABELS.apiKey, "bearer token"]);

  function appLinks() {
    const seen = new Set();
    return [...document.querySelectorAll("a[href]")].filter((link) => {
      if (!visible(link)) return false;
      let path = "";
      try {
        const url = new URL(link.href);
        path = url.pathname + url.hash;
      } catch {
        return false;
      }
      if (!/\/apps?\/[^/]+/.test(path) || seen.has(path)) return false;
      const label = clean(link.innerText);
      if (!label || label.length > 60 || matches(norm(label), [...WORDS.keysTab, ...WORDS.settingsTab, ...WORDS.apps])) return false;
      seen.add(path);
      return true;
    }).slice(0, 3);
  }

  function errorText() {
    const detail = setup.detail ? ` (${setup.detail})` : "";
    const credits = /credit|402|enroll|billing|payment/i.test(`${setup.error} ${setup.detail}`);
    if (credits) return { credits: true, text: `X has no API credits for this app yet${detail}. Open Billing → Credits in the console, add credits yourself, then press Check again.` };
    return {
      x_not_authorized: { text: `X refused the access token${detail}. It was most likely made before Read and write was saved, so ForkMesh dropped it: make the access token again on Keys and tokens.` },
      x_rate_limited: { text: "X is rate limiting this app. Wait a minute, then press Check again." },
      organization_admin_required: { text: `Only an owner or admin of ${setup.organization} can connect X.` },
      not_signed_in: { text: "Sign in to the ForkMesh extension (its toolbar icon), then press Check again." },
      x_credentials_required: { text: "One of the four keys is missing." },
      x_account_not_found: { text: "X accepted the keys but returned no account. Make the access token again." },
    }[setup.error] || { text: `ForkMesh could not save the keys: ${setup.message || setup.error}${detail}.` };
  }

  function guide() {
    const view = { title: "", text: "", actions: [], warn: "", target: null, manual: false };
    if (setup.state === "done") {
      view.title = `Connected as @${setup.username}`;
      view.text = "Marketing posts to X with these keys now. You can close this tab.";
      view.actions.push({ label: "Back to Marketing", primary: true, run: () => void send({ type: "fm-x-setup-back" }) });
      return view;
    }
    if (setup.state === "checking") {
      view.title = "Checking the keys with X…";
      view.text = "ForkMesh is asking X which account these keys belong to.";
      return view;
    }
    const keysDialog = dialogs().find((box) => labels(FIELDS.flatMap((name) => LABELS[name]), box).length);
    const clientDialog = !keysDialog && dialogs().find((box) => labels(["client id", "client secret", "oauth 2.0 client id"], box).length);
    if (setup.state === "error") {
      const error = errorText();
      view.title = "Not connected yet";
      view.warn = error.text;
      if (error.credits) {
        const billing = findClickable(WORDS.billing);
        if (billing) view.actions.push({ label: "Open Billing", run: () => click(billing) });
      }
      if (have(...FIELDS)) view.actions.push({ label: "Check again", primary: true, run: () => void sendKeys({}, true) });
      if (setup.error !== "x_not_authorized" || keysDialog) return view;
    }
    if (keysDialog) {
      const shown = FIELDS.filter((name) => labels(LABELS[name], keysDialog).length);
      const read = readKeys();
      const missing = shown.filter((name) => !read[name]);
      view.title = "X is showing keys";
      if (!missing.length) {
        view.text = `Got the ${shown.map((name) => NAMES[name]).join(" and ")}. X shows them only once; ForkMesh keeps them until it saves them for ${setup.organization}. Close X's window to go on.`;
        const close = findClickable(WORDS.dismiss, keysDialog);
        view.target = close;
        if (close) view.actions.push({ label: "Close X's window", primary: true, run: () => click(close) });
      } else {
        view.text = `ForkMesh could not read the ${missing.map((name) => NAMES[name]).join(" and ")} on this page. Copy each from X's window into the boxes below before you close it.`;
        view.manual = true;
      }
      return view;
    }
    if (clientDialog) {
      view.title = "OAuth 2.0 keys";
      view.text = "These are the Client ID and Client Secret. ForkMesh does not need them: close X's window.";
      const close = findClickable(WORDS.dismiss, clientDialog);
      view.target = close;
      if (close) view.actions.push({ label: "Close X's window", primary: true, run: () => click(close) });
      return view;
    }
    if (isSignIn()) {
      view.title = "Sign in to X";
      view.text = "Sign in with the X account Marketing should post as. The access token you make later belongs to whoever is signed in here.";
      const button = findClickable(WORDS.signIn);
      view.target = button;
      if (button) view.actions.push({ label: "Sign in", primary: true, run: () => click(button) });
      return view;
    }
    if (isOnboarding()) {
      view.title = "Tell X how you will use the API";
      view.text = "Fill in describes ForkMesh Marketing in X's box (edit it if you like). Tick X's agreement boxes yourself, then submit.";
      view.actions.push({
        label: "Fill in", primary: true,
        run: () => {
          for (const area of [...document.querySelectorAll("textarea")].filter(visible)) {
            if (!clean(area.value)) setValue(area, useCase());
          }
          const submit = findClickable(WORDS.submit);
          target = submit;
          notice = "Filled in. Read X's terms, tick the boxes, then submit.";
          render(true);
        },
      });
      return view;
    }
    if (isAuthForm()) {
      view.title = "Read and write permission";
      view.text = `Fill in picks Read and write and Web App, Automated App or Bot, and puts ${origin()} in Callback URI and Website URL where they are empty. Marketing only posts, so it needs no Direct message access.`;
      const save = findClickable(WORDS.save);
      view.target = save;
      const fillForm = () => {
        choose(PERMISSION_LABEL);
        choose(APP_TYPE_LABEL);
        fill([/^callback ur[il]/, /^redirect ur[il]/], `${origin()}/dashboard/marketing`);
        fill([/^website ur[il]/, "website"], origin());
      };
      view.actions.push({
        label: "Fill in and save", primary: true,
        run: () => {
          fillForm();
          setTimeout(() => {
            const button = findClickable(WORDS.save);
            if (!button) {
              notice = "Filled in. Press Save at the bottom of X's form.";
              render(true);
              return;
            }
            click(button, WORDS.confirm);
            void mark(true);
            notice = "Saved. If X shows an error on the form, fix it and save again.";
          }, 300);
        },
      });
      view.actions.push({ label: "Fill in only", run: () => { fillForm(); notice = "Filled in. Check the form, then press Save."; render(true); } });
      return view;
    }
    if (isKeysPage()) {
      const access = accessSection();
      const readOnly = access && /read[- ]only/.test(textOf(access.box));
      if (!have("apiKey", "apiSecret")) {
        const api = apiSection();
        const button = api && findClickable(WORDS.apiGenerate, api.box);
        view.title = "API key and secret";
        view.text = "Make a new API key and secret: ForkMesh reads them off X's window.";
        view.warn = "Regenerating replaces this app's current API key. Anything else using this app needs the new one.";
        view.target = button;
        if (button) view.actions.push({ label: "Make the API key", primary: true, run: () => click(button, WORDS.confirm) });
        view.manual = !button;
        return view;
      }
      if (!setup.permission) {
        view.title = "Read and write permission first";
        view.text = "Got the API key. Before the access token, give the app Read and write permission: a token made earlier stays read-only.";
        const tab = findClickable(WORDS.settingsTab);
        view.target = tab;
        if (tab) view.actions.push({ label: "Open app settings", primary: true, run: () => click(tab) });
        view.actions.push({ label: "It is already Read and write", run: () => void mark(true) });
        return view;
      }
      if (!have("accessToken", "accessSecret")) {
        const button = access && findClickable(WORDS.generate, access.box);
        view.title = "Access token and secret";
        view.text = "Make the access token for the signed-in account: ForkMesh reads it off X's window and then checks all four keys.";
        if (readOnly) view.warn = "X says the current token is read-only. Making it again gives it Read and write.";
        view.target = button;
        if (button) view.actions.push({ label: "Make the access token", primary: true, run: () => click(button, WORDS.confirm) });
        view.manual = !button;
        return view;
      }
      view.title = "All four keys";
      view.text = "Press Check and save to connect them to Marketing.";
      view.actions.push({ label: "Check and save", primary: true, run: () => void sendKeys({}, true) });
      return view;
    }
    const auth = section(AUTH_SECTION);
    if (auth) {
      const authText = textOf(auth.box);
      if (!setup.permission && /read and write/.test(authText)) void mark(true);
      if (!setup.permission && !/read and write/.test(authText)) {
        const button = findClickable(WORDS.setUp, auth.box);
        view.title = "Read and write permission";
        view.text = "Open the app's user authentication settings to let Marketing post.";
        view.target = button;
        if (button) view.actions.push({ label: "Set up user authentication", primary: true, run: () => click(button) });
        return view;
      }
      const tab = findClickable(WORDS.keysTab);
      view.title = "Keys and tokens";
      view.text = "The app can post. Next, open Keys and tokens.";
      view.target = tab;
      if (tab) view.actions.push({ label: "Open Keys and tokens", primary: true, run: () => click(tab) });
      return view;
    }
    const keysTab = findClickable(WORDS.keysTab);
    if (keysTab) {
      view.title = "Your app";
      view.text = setup.permission ? "Open Keys and tokens." : "Open the app's settings for Read and write first, or Keys and tokens for the API key.";
      const settingsTab = findClickable(WORDS.settingsTab);
      view.target = setup.permission || !settingsTab ? keysTab : settingsTab;
      if (settingsTab && !setup.permission) view.actions.push({ label: "Open app settings", primary: true, run: () => click(settingsTab) });
      view.actions.push({ label: "Open Keys and tokens", primary: setup.permission || !settingsTab, run: () => click(keysTab) });
      return view;
    }
    const apps = appLinks();
    if (apps.length) {
      view.title = "Open your app";
      view.text = apps.length === 1 ? "Open the app Marketing should post with." : "Pick the app Marketing should post with.";
      view.target = apps[0];
      for (const [index, link] of apps.entries()) {
        view.actions.push({ label: `Open ${clean(link.innerText).slice(0, 30)}`, primary: index === 0, run: () => click(link) });
      }
      return view;
    }
    const appName = labels([/^app name/, /^name your app/, /^name of (the|your) app/]).map((label) => near(label, TEXT_INPUT)).find((input) => input && visible(input));
    if (appName) {
      view.title = "Name the app";
      view.text = `Fill in names it "${setup.organization} Marketing"; pick Production if X asks for an environment, then create it.`;
      view.actions.push({
        label: "Fill in", primary: true,
        run: () => {
          if (!clean(appName.value)) setValue(appName, `${setup.organization} Marketing`.slice(0, 32));
          target = findClickable([...WORDS.submit, /^create( app)?$/]);
          notice = "Named. Press X's create button.";
          render(true);
        },
      });
      return view;
    }
    const create = findClickable(WORDS.createApp);
    if (create) {
      view.title = "Create an app";
      view.text = `No app yet. Create one, named for example "${setup.organization} Marketing", in the Production environment. ForkMesh reads the keys X shows when it is made.`;
      view.target = create;
      view.actions.push({ label: "Create an app", primary: true, run: () => click(create) });
      return view;
    }
    const nav = findClickable(WORDS.apps);
    view.title = "Find your app";
    view.text = "Open Apps in the console's menu, then the app Marketing should post with.";
    view.target = nav;
    if (nav) view.actions.push({ label: "Go to Apps", primary: true, run: () => click(nav) });
    view.manual = !nav;
    return view;
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
      box-sizing: border-box; background: #fff; color: #0f1419; border: 1px solid #cfd9de; border-radius: 12px;
      box-shadow: 0 8px 28px rgba(0,0,0,.22); font: 13px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
    .card.small .body { display: none; }
    .head { display: flex; align-items: center; gap: 8px; padding: 10px 12px; border-bottom: 1px solid #eff3f4; }
    .head b { flex: 1; font-size: 13px; }
    .icon { border: 0; background: transparent; cursor: pointer; font: inherit; color: #536471; padding: 2px 6px; border-radius: 6px; }
    .icon:hover { background: #eff3f4; }
    .body { padding: 10px 12px 12px; }
    ol { margin: 0 0 10px; padding: 0; list-style: none; }
    li { display: flex; gap: 8px; padding: 2px 0; color: #536471; }
    li.done { color: #00875a; }
    li.now { color: #0f1419; font-weight: 600; }
    li span:first-child { width: 14px; text-align: center; }
    h3 { margin: 0 0 4px; font-size: 14px; }
    p { margin: 0 0 8px; }
    .warn { background: #fff4e5; border: 1px solid #ffd8a8; border-radius: 8px; padding: 6px 8px; }
    .notice { background: #e8f5fd; border-radius: 8px; padding: 6px 8px; }
    .actions { display: flex; flex-wrap: wrap; gap: 6px; margin: 8px 0 4px; }
    button.act { border: 1px solid #cfd9de; background: #fff; color: #0f1419; border-radius: 999px; padding: 6px 12px; font: inherit; font-weight: 600; cursor: pointer; }
    button.act.primary { background: #1d9bf0; border-color: #1d9bf0; color: #fff; }
    button.act:hover { filter: brightness(.95); }
    details { margin-top: 8px; border-top: 1px solid #eff3f4; padding-top: 8px; }
    summary { cursor: pointer; color: #536471; }
    label { display: grid; gap: 2px; margin-top: 6px; color: #536471; font-size: 12px; }
    input { box-sizing: border-box; width: 100%; padding: 6px 8px; border: 1px solid #cfd9de; border-radius: 6px; font: 12px ui-monospace, monospace; color: #0f1419; }
    .got { color: #00875a; font-size: 12px; }
    .ring { position: fixed; z-index: 2147483646; pointer-events: none; border: 3px solid #1d9bf0; border-radius: 10px;
      box-shadow: 0 0 0 4px rgba(29,155,240,.25); transition: all .15s ease; }
  `;

  function mount() {
    if (hostNode?.isConnected) return;
    hostNode = document.createElement("forkmesh-x-setup");
    shadow = hostNode.attachShadow({ mode: "closed" });
    shadow.append(h("style", { textContent: STYLE }));
    const ring = h("div", { class: "ring", hidden: true });
    const title = h("b", { textContent: "Connect X to ForkMesh" });
    const steps = h("ol");
    const now = h("div");
    const inputs = {};
    const manual = h("details", {}, h("summary", { textContent: "Paste the keys yourself" }),
      FIELDS.map((name) => {
        inputs[name] = h("input", {
          type: name.endsWith("Secret") ? "password" : "text", autocomplete: "off", spellcheck: false,
          placeholder: NAMES[name],
          on: { paste: () => setTimeout(() => routePaste(inputs[name], name), 0), change: () => routePaste(inputs[name], name) },
        });
        return h("label", {}, NAMES[name], inputs[name]);
      }),
      h("div", { class: "actions" }, h("button", { class: "act", type: "button", textContent: "Check and save", on: { click: () => void sendKeys({}, true) } })));
    const body = h("div", { class: "body" }, steps, now, manual);
    const card = h("div", { class: "card" },
      h("div", { class: "head" }, title,
        h("button", { class: "icon", type: "button", title: "Minimize", textContent: "–", on: { click: () => { minimized = !minimized; render(true); } } }),
        h("button", { class: "icon", type: "button", title: "Stop the X setup", textContent: "✕", on: { click: () => void stop() } })),
      body);
    shadow.append(ring, card);
    parts = { card, title, steps, now, manual, inputs, ring };
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
    let field = name;
    if (PASTE.accessToken.test(value)) field = "accessToken";
    else if (/^[A-Za-z0-9]+$/.test(value)) field = { 25: "apiKey", 50: "apiSecret", 45: "accessSecret" }[value.length] || (name === "accessToken" ? "" : name);
    if (!field || !PASTE[field].test(value)) {
      notice = `That does not look like an ${NAMES[name]}.`;
      render(true);
      return;
    }
    input.value = "";
    if (field !== name) notice = `That looked like the ${NAMES[field]}, so it went there.`;
    void sendKeys({ [field]: value });
  }

  async function stop() {
    await send({ type: "fm-x-setup-stop" });
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
      ["Read and write permission", Boolean(setup.permission)],
      ["API key and secret", have("apiKey", "apiSecret")],
      ["Access token and secret", have("accessToken", "accessSecret")],
      [`Saved in ${setup.organization}'s Marketing`, setup.state === "done"],
    ];
    const current = items.findIndex(([, done]) => !done);
    return items.map(([label, done], index) => {
      const tail = index === 1 && done ? ` (${setup.keys.apiKey})` : index === 2 && done ? ` (${setup.keys.accessToken})` : "";
      return h("li", { class: done ? "done" : index === current ? "now" : "" },
        h("span", { textContent: done ? "✓" : index === current ? "›" : "·" }), h("span", { textContent: label + tail }));
    });
  }

  function render(force = false) {
    if (!setup?.active || !parts) return;
    const view = guide();
    target = view.target || (notice && target?.isConnected ? target : null);
    const signature = JSON.stringify([view.title, view.text, view.warn, view.actions.map((item) => item.label), view.manual,
      notice, minimized, setup.state, setup.permission, setup.keys, setup.error]);
    placeRing();
    if (!force && signature === lastView) {
      parts.handlers = view.actions;
      return;
    }
    lastView = signature;
    parts.card.classList.toggle("small", minimized);
    parts.title.textContent = minimized ? `X setup: ${view.title}` : "Connect X to ForkMesh";
    parts.steps.replaceChildren(...stepList());
    parts.handlers = view.actions;
    parts.now.replaceChildren(...[
      h("h3", { textContent: view.title }),
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
    mount();
  }

  function tick() {
    scheduled = 0;
    if (stopped || !setup?.active || document.visibilityState !== "visible") return;
    confirmPending();
    if (!["checking", "done"].includes(setup.state)) captureKeys();
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
    document.addEventListener("click", (event) => {
      if (!setup?.active || setup.permission || ours(event.target)) return;
      const button = event.target.closest?.(CLICKABLE);
      if (button && isAuthForm() && matches(textOf(button), WORDS.save)) {
        pending = { words: WORDS.confirm, until: Date.now() + CONFIRM_MS };
        void mark(true);
      }
    }, true);
  }

  chrome.runtime.onMessage.addListener((message) => {
    if (message?.type !== "fm-x-setup-changed") return;
    apply(message.setup);
    if (setup?.active) {
      watch();
      render(true);
    }
  });

  void send({ type: "fm-x-setup-state" }).then((response) => {
    if (!response?.setup?.active) return;
    apply(response.setup);
    watch();
    render(true);
  });
})();
