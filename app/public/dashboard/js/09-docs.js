  const DOCS_BOOK_REPO = Object.freeze({ owner: "forkmesh", name: "forkmesh" });
  const DOCS_BOOK_BATCH = 6;
  const docsBook = {
    docs: [],
    byPath: new Map(),
    canEdit: false,
    commit: "",
    renderToken: 0,
    menuRange: null,
    menuPlan: null,
  };

  async function docsBookRequest(method, path, body) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const init = { method, headers, cache: "no-store", credentials: "same-origin" };
    if (body !== undefined) {
      headers["content-type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    const response = await fetch(path, init);
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.message || payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.code = String(payload.error || "");
      throw error;
    }
    return payload;
  }

  function docsBookEl(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = String(text);
    return node;
  }

  function docsBookIcon(name) {
    const icon = document.createElement("i");
    icon.setAttribute("data-lucide", name);
    return icon;
  }

  function docsBookIcons() {
    window.lucide?.createIcons();
  }

  function docsBookId(path) {
    return `doc-${String(path).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "")}`;
  }

  function docsBookNotice(message, tone, action) {
    const notice = $("[data-docs-notice]");
    if (!notice) return;
    notice.replaceChildren();
    notice.hidden = !message;
    if (!message) return;
    notice.dataset.tone = tone || "";
    notice.append(document.createTextNode(message));
    if (action) {
      const button = docsBookEl("button", "", action.label);
      button.type = "button";
      button.addEventListener("click", action.run);
      notice.append(button);
    }
  }

  function docsBookDateLabel(doc) {
    if (!doc.updated) return "last change unknown";
    return `Updated ${formatDate(doc.updated)} · ${formatTimeAgo(doc.updated)}`;
  }

  function docsBookBlocks(source) {
    const lines = String(source || "").split("\n");
    const blocks = [];
    const isFence = (line) => /^\s*(```|~~~)/.test(line);
    const isHeading = (line) => /^#{1,6}\s/.test(line);
    let i = 0;
    while (i < lines.length) {
      if (!lines[i].trim()) {
        i += 1;
        continue;
      }
      const start = i;
      const fence = lines[i].match(/^\s*(```|~~~)/);
      if (fence) {
        i += 1;
        while (i < lines.length && !lines[i].trimStart().startsWith(fence[1])) i += 1;
        i = Math.min(i + 1, lines.length);
      } else if (isHeading(lines[i])) {
        i += 1;
      } else {
        i += 1;
        while (i < lines.length && lines[i].trim() && !isFence(lines[i]) && !isHeading(lines[i])) i += 1;
      }
      const heading = /^(#{1,6})\s+(.*)$/.exec(lines[start]);
      blocks.push({
        start,
        end: i,
        level: heading ? heading[1].length : 0,
        title: heading ? heading[2].replace(/\s+#+\s*$/, "") : "",
      });
    }
    return { lines, blocks };
  }

  function docsBookLinkTarget(doc, anchor) {
    const path = anchor.getAttribute("data-repo-markdown-path") || "";
    const hash = anchor.getAttribute("data-repo-markdown-anchor") || "";
    const raw = anchor.getAttribute("href") || "";
    if (raw.startsWith("#")) return `#${docsBookId(doc.path)}--${hash || raw.slice(1)}`;
    if (!path || !docsBook.byPath.has(path)) return "";
    return `#${docsBookId(path)}${hash ? `--${hash}` : ""}`;
  }

  function docsBookRenderBody(doc) {
    const body = doc.node.querySelector("[data-docs-body]");
    body.replaceChildren();
    if (typeof doc.content !== "string") {
      body.append(docsBookEl("p", "docs-missing", "This file could not be read (it may not be UTF-8 text)."));
      return;
    }
    const parsed = docsBookBlocks(doc.content);
    doc.lines = parsed.lines;
    doc.blocks = parsed.blocks;
    const context = { repo: DOCS_BOOK_REPO, basePath: doc.path };
    const seen = new Map();
    const docId = docsBookId(doc.path);
    parsed.blocks.forEach((block, index) => {
      const node = docsBookEl("div", "docs-block");
      node.dataset.docsBlock = String(index);
      const source = parsed.lines.slice(block.start, block.end).join("\n");
      node.innerHTML = renderMarkdown(source, context);
      const ordered = /^\s*(\d+)[.)]\s/.exec(parsed.lines[block.start]);
      const list = node.firstElementChild;
      if (ordered && list?.tagName === "OL" && Number(ordered[1]) > 1) list.start = Number(ordered[1]);
      for (const heading of node.querySelectorAll("[data-repo-markdown-heading]")) {
        const base = heading.getAttribute("data-repo-markdown-heading");
        const count = seen.get(base) || 0;
        seen.set(base, count + 1);
        heading.id = `${docId}--${count ? `${base}-${count}` : base}`;
      }
      for (const anchor of node.querySelectorAll("a[href]")) {
        const target = docsBookLinkTarget(doc, anchor);
        if (target) {
          anchor.setAttribute("href", target);
          anchor.removeAttribute("target");
          anchor.dataset.docsJump = "";
        }
      }
      body.append(node);
    });
    if (doc.truncated) {
      body.append(docsBookEl("p", "docs-missing", "This file is larger than 2 MB and is shown cut short; edit it in the repository."));
    }
  }

  function docsBookSeparator(doc) {
    const sep = docsBookEl("div", "docs-sep");
    sep.setAttribute("role", "separator");
    sep.setAttribute("aria-label", doc.path);
    const name = docsBookEl("a", "docs-sep-name", doc.path);
    name.href = `#${docsBookId(doc.path)}`;
    name.dataset.docsJump = "";
    const date = docsBookEl("time", "docs-sep-date", docsBookDateLabel(doc));
    date.dataset.docsDate = "";
    if (doc.updated) date.dateTime = doc.updated;
    date.title = [doc.subject, doc.author && `by ${doc.author}`].filter(Boolean).join(" ");
    sep.append(name, date);
    if (docsBook.canEdit && typeof doc.content === "string" && !doc.truncated) {
      const edit = docsBookEl("button", "docs-icon");
      edit.type = "button";
      edit.dataset.docsEdit = doc.path;
      edit.title = `Edit ${doc.path}`;
      edit.setAttribute("aria-label", `Edit ${doc.path}`);
      edit.append(docsBookIcon("pencil"));
      sep.append(edit);
    }
    const open = docsBookEl("a", "docs-icon");
    open.href = repoLinkUrl(DOCS_BOOK_REPO, "blob", doc.path);
    open.title = "Open in the repository";
    open.setAttribute("aria-label", `Open ${doc.path} in the repository`);
    open.append(docsBookIcon("external-link"));
    sep.append(open);
    return sep;
  }

  function docsBookArticle(doc) {
    const article = docsBookEl("article", "docs-doc");
    article.id = docsBookId(doc.path);
    article.dataset.docsPath = doc.path;
    const body = docsBookEl("div", "docs-body");
    body.dataset.docsBody = "";
    article.append(docsBookSeparator(doc), body);
    doc.node = article;
    return article;
  }

  function docsBookRefreshSeparator(doc) {
    const old = doc.node?.querySelector(".docs-sep");
    if (old) old.replaceWith(docsBookSeparator(doc));
    const entry = $(`[data-docs-contents-list] time[data-docs-for="${CSS.escape(doc.path)}"]`);
    if (entry) entry.textContent = doc.updated ? formatDate(doc.updated) : "";
    docsBookIcons();
  }

  function docsBookRenderContents() {
    const list = $("[data-docs-contents-list]");
    const panel = $("[data-docs-contents]");
    if (!list || !panel) return;
    list.replaceChildren(...docsBook.docs.map((doc) => {
      const item = docsBookEl("li");
      item.dataset.docsPath = doc.path;
      const link = docsBookEl("a", "", doc.path.replace(/^docs\//, ""));
      link.href = `#${docsBookId(doc.path)}`;
      link.dataset.docsJump = "";
      const time = docsBookEl("time", "", doc.updated ? formatDate(doc.updated) : "");
      time.dataset.docsFor = doc.path;
      item.append(link, time);
      return item;
    }));
    panel.hidden = !docsBook.docs.length;
  }

  function docsBookRenderMeta(payload) {
    const meta = $("[data-docs-meta]");
    if (!meta) return;
    const count = docsBook.docs.length;
    const parts = [`${count} file${count === 1 ? "" : "s"}`];
    if (payload?.commit) parts.push(`main at ${payload.commit.slice(0, 10)}`);
    if (payload?.latest?.date) parts.push(`last commit ${formatTimeAgo(payload.latest.date)}`);
    if (docsBook.canEdit) parts.push("you can edit: use the pencil beside a file name, or select text");
    meta.textContent = parts.join(" · ");
  }

  function docsBookRender(payload) {
    const list = $("[data-docs-list]");
    if (!list) return;
    const token = docsBook.renderToken += 1;
    list.replaceChildren(...docsBook.docs.map(docsBookArticle));
    docsBookRenderContents();
    docsBookRenderMeta(payload);
    docsBookApplyFilter();
    docsBookIcons();
    const wanted = decodeURIComponent(location.hash.slice(1));
    let index = 0;
    const step = () => {
      if (token !== docsBook.renderToken) return;
      const stop = Math.min(index + DOCS_BOOK_BATCH, docsBook.docs.length);
      for (; index < stop; index += 1) docsBookRenderBody(docsBook.docs[index]);
      if (wanted && !docsBook.jumped) {
        const target = document.getElementById(wanted);
        if (target && target.closest(".docs-doc")?.querySelector("[data-docs-body]")?.childElementCount) {
          docsBook.jumped = true;
          target.scrollIntoView({ block: "start" });
        }
      }
      if (index < docsBook.docs.length) window.setTimeout(step, 0);
    };
    step();
  }

  function docsBookApplyFilter() {
    const query = String($("[data-docs-filter]")?.value || "").trim().toLowerCase();
    for (const doc of docsBook.docs) {
      const match = !query || doc.path.toLowerCase().includes(query);
      if (doc.node) doc.node.hidden = !match;
      const item = $(`[data-docs-contents-list] li[data-docs-path="${CSS.escape(doc.path)}"]`);
      if (item) item.hidden = !match;
    }
  }

  async function docsBookLoad() {
    if (docsBookDirty() && !window.confirm("Discard your unsaved edits and reload?")) return;
    const button = $("[data-docs-refresh]");
    if (button) button.disabled = true;
    docsBookHideMenu();
    try {
      const payload = await docsBookRequest("GET", "/api/docs-book");
      docsBook.canEdit = payload.canEdit === true;
      docsBook.commit = String(payload.commit || "");
      docsBook.docs = (Array.isArray(payload.docs) ? payload.docs : []).map((doc) => ({ ...doc }));
      docsBook.byPath = new Map(docsBook.docs.map((doc) => [doc.path, doc]));
      docsBookNotice("");
      docsBookRender(payload);
    } catch (error) {
      const list = $("[data-docs-list]");
      if (list && !docsBook.docs.length) list.replaceChildren();
      docsBookNotice(`Could not load the docs: ${error.message}`, "bad", { label: "Try again", run: () => void docsBookLoad() });
    } finally {
      if (button) button.disabled = false;
    }
  }

  function docsBookDirty() {
    return docsBook.docs.some((doc) => doc.editor && doc.editor.querySelector("textarea").value !== doc.content);
  }

  function docsBookOpenEditor(doc, selection) {
    if (!docsBook.canEdit || !doc?.node) return;
    docsBookHideMenu();
    if (!doc.editor) {
      const form = docsBookEl("form", "docs-editor");
      const area = docsBookEl("textarea");
      area.spellcheck = true;
      area.setAttribute("aria-label", `Markdown of ${doc.path}`);
      area.value = doc.content;
      const message = docsBookEl("input");
      message.type = "text";
      message.maxLength = 200;
      message.placeholder = `Docs: edit ${doc.path} on the web`;
      message.setAttribute("aria-label", "Commit message");
      const bar = docsBookEl("div", "docs-editor-bar");
      const status = docsBookEl("span", "docs-editor-status", "Saving commits to main as you.");
      status.setAttribute("aria-live", "polite");
      const cancel = docsBookEl("button", "docs-btn", "Cancel");
      cancel.type = "button";
      const save = docsBookEl("button", "docs-btn docs-btn-primary");
      save.type = "submit";
      save.append(docsBookIcon("check"), document.createTextNode("Save"));
      bar.append(status, cancel, save);
      form.append(area, message, bar);
      const resize = () => {
        area.style.height = "auto";
        area.style.height = `${Math.min(Math.max(area.scrollHeight + 4, 256), window.innerHeight * 0.75)}px`;
      };
      area.addEventListener("input", resize);
      form.addEventListener("submit", (event) => {
        event.preventDefault();
        void docsBookSaveEditor(doc);
      });
      form.addEventListener("keydown", (event) => {
        if ((event.metaKey || event.ctrlKey) && event.key.toLowerCase() === "s") {
          event.preventDefault();
          void docsBookSaveEditor(doc);
        } else if (event.key === "Escape") {
          event.preventDefault();
          docsBookCloseEditor(doc);
        }
      });
      cancel.addEventListener("click", () => docsBookCloseEditor(doc));
      doc.editor = form;
      doc.node.querySelector("[data-docs-body]").hidden = true;
      doc.node.append(form);
      doc.node.querySelector("[data-docs-edit]")?.setAttribute("aria-pressed", "true");
      docsBookIcons();
      resize();
    }
    const area = doc.editor.querySelector("textarea");
    area.focus({ preventScroll: true });
    if (selection) {
      area.setSelectionRange(selection.start, selection.end);
      const before = area.value.slice(0, selection.start).split("\n").length - 1;
      const lineHeight = parseFloat(getComputedStyle(area).lineHeight) || 21;
      area.scrollTop = Math.max(0, before * lineHeight - area.clientHeight / 3);
    }
    doc.node.scrollIntoView({ block: "start", behavior: "smooth" });
  }

  function docsBookCloseEditor(doc, force) {
    if (!doc?.editor) return;
    const area = doc.editor.querySelector("textarea");
    if (!force && area.value !== doc.content && !window.confirm(`Discard your edits to ${doc.path}?`)) return;
    doc.editor.remove();
    doc.editor = null;
    doc.node.querySelector("[data-docs-body]").hidden = false;
    doc.node.querySelector("[data-docs-edit]")?.setAttribute("aria-pressed", "false");
  }

  function docsBookEditorStatus(doc, text, tone) {
    const status = doc.editor?.querySelector(".docs-editor-status");
    if (!status) return;
    status.textContent = text;
    status.dataset.tone = tone || "";
  }

  async function docsBookSave(doc, content, message) {
    const payload = await docsBookRequest("POST", "/api/docs-book/save", {
      path: doc.path,
      oid: doc.oid,
      content,
      message: message || "",
    });
    if (!payload.unchanged) {
      doc.content = content;
      doc.oid = payload.oid || doc.oid;
      doc.updated = payload.updated || doc.updated;
      doc.author = payload.author || doc.author;
      doc.subject = payload.subject || doc.subject;
      docsBook.commit = payload.commit || docsBook.commit;
    }
    return payload;
  }

  function docsBookSaveError(doc, error) {
    if (error.code === "conflict") {
      return `${error.message} Copy your text before reloading.`;
    }
    if (error.status === 403) return "Only admins can edit the docs.";
    return `Could not save ${doc.path}: ${error.message}`;
  }

  async function docsBookSaveEditor(doc) {
    if (!doc.editor || doc.saving) return;
    const area = doc.editor.querySelector("textarea");
    const message = doc.editor.querySelector("input").value.trim();
    const buttons = doc.editor.querySelectorAll("button");
    doc.saving = true;
    buttons.forEach((button) => { button.disabled = true; });
    docsBookEditorStatus(doc, "Saving...");
    try {
      const payload = await docsBookSave(doc, area.value, message);
      docsBookCloseEditor(doc, true);
      docsBookRenderBody(doc);
      docsBookRefreshSeparator(doc);
      docsBookNotice(payload.unchanged
        ? `${doc.path} was not changed.`
        : `Saved ${doc.path} as commit ${String(payload.commit || "").slice(0, 10)} on main.`, "good");
    } catch (error) {
      docsBookEditorStatus(doc, docsBookSaveError(doc, error), "bad");
    } finally {
      doc.saving = false;
      buttons.forEach((button) => { button.disabled = false; });
    }
  }

  function docsBookSelectionPlan() {
    const selection = window.getSelection();
    if (!selection || selection.isCollapsed || !selection.rangeCount) return null;
    const range = selection.getRangeAt(0);
    const list = $("[data-docs-list]");
    if (!list || !list.contains(range.commonAncestorContainer)) return null;
    const text = selection.toString();
    if (!text.trim()) return null;
    const startDoc = (range.startContainer.nodeType === 1 ? range.startContainer : range.startContainer.parentElement)?.closest(".docs-doc");
    const endDoc = (range.endContainer.nodeType === 1 ? range.endContainer : range.endContainer.parentElement)?.closest(".docs-doc");
    const doc = startDoc && startDoc === endDoc ? docsBook.byPath.get(startDoc.dataset.docsPath) : null;
    const plan = { range, text, doc: null, indexes: [] };
    if (!doc || doc.editor || !doc.blocks) return plan;
    const indexes = [];
    for (const node of doc.node.querySelectorAll("[data-docs-block]")) {
      if (range.intersectsNode(node)) indexes.push(Number(node.dataset.docsBlock));
    }
    if (!indexes.length) return plan;
    plan.doc = doc;
    plan.indexes = indexes;
    plan.section = docsBookSection(doc, indexes[0]);
    return plan;
  }

  function docsBookSection(doc, index) {
    let head = index;
    while (head >= 0 && !doc.blocks[head].level) head -= 1;
    if (head < 0) return null;
    const level = doc.blocks[head].level;
    let end = head + 1;
    while (end < doc.blocks.length && !(doc.blocks[end].level && doc.blocks[end].level <= level)) end += 1;
    const indexes = [];
    for (let i = head; i < end; i += 1) indexes.push(i);
    return { title: doc.blocks[head].title, indexes };
  }

  function docsBookSourceRange(doc, indexes) {
    const first = doc.blocks[indexes[0]];
    const last = doc.blocks[indexes[indexes.length - 1]];
    const offset = (line) => doc.lines.slice(0, line).reduce((sum, text) => sum + text.length + 1, 0);
    return { start: offset(first.start), end: Math.min(doc.content.length, offset(last.end) - 1) };
  }

  function docsBookWithout(doc, indexes) {
    const drop = new Set();
    for (const index of indexes) {
      const block = doc.blocks[index];
      for (let line = block.start; line < block.end; line += 1) drop.add(line);
      let next = block.end;
      while (next < doc.lines.length && !doc.lines[next].trim()) {
        drop.add(next);
        next += 1;
      }
    }
    const kept = doc.lines.filter((_, line) => !drop.has(line));
    while (kept.length && !kept[kept.length - 1].trim()) kept.pop();
    return kept.length ? `${kept.join("\n")}\n` : "";
  }

  function docsBookSelectBlocks(doc, indexes) {
    const nodes = indexes.map((index) => doc.node.querySelector(`[data-docs-block="${index}"]`)).filter(Boolean);
    if (!nodes.length) return;
    const range = document.createRange();
    range.setStartBefore(nodes[0]);
    range.setEndAfter(nodes[nodes.length - 1]);
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    docsBookShowMenu();
  }

  function docsBookMark(doc, indexes) {
    for (const node of $$("[data-docs-marked]")) node.removeAttribute("data-docs-marked");
    if (!doc) return;
    for (const index of indexes) doc.node.querySelector(`[data-docs-block="${index}"]`)?.setAttribute("data-docs-marked", "");
  }

  function docsBookMenuButton(label, icon, run, danger) {
    const button = docsBookEl("button");
    button.type = "button";
    if (danger) button.dataset.danger = "";
    button.append(docsBookIcon(icon), document.createTextNode(label));
    button.addEventListener("mousedown", (event) => event.preventDefault());
    button.addEventListener("click", run);
    return button;
  }

  function docsBookPlace(menu, rect) {
    menu.hidden = false;
    const width = menu.offsetWidth;
    const height = menu.offsetHeight;
    const left = Math.min(Math.max(8, rect.left + rect.width / 2 - width / 2), window.innerWidth - width - 8);
    const above = rect.top - height - 8;
    const top = above > 8 ? above : Math.min(rect.bottom + 8, window.innerHeight - height - 8);
    menu.style.left = `${Math.round(left)}px`;
    menu.style.top = `${Math.round(top)}px`;
  }

  function docsBookHideMenu() {
    const menu = $("[data-docs-menu]");
    if (menu) {
      menu.hidden = true;
      menu.replaceChildren();
    }
    docsBook.menuPlan = null;
    docsBookMark(null, []);
  }

  async function docsBookCopy(text, label) {
    try {
      await navigator.clipboard.writeText(text);
      docsBookNotice(`${label} copied.`, "good");
    } catch (_) {
      docsBookNotice("The clipboard is not available in this browser.", "bad");
    }
    docsBookHideMenu();
  }

  function docsBookConfirmDelete(plan, indexes, what) {
    const menu = $("[data-docs-menu]");
    if (!menu) return;
    const rect = menu.getBoundingClientRect();
    docsBookMark(plan.doc, indexes);
    menu.replaceChildren(
      docsBookEl("span", "docs-menu-text", `Delete ${what} from ${plan.doc.path}?`),
      docsBookMenuButton("Delete and save", "trash-2", () => void docsBookDelete(plan.doc, indexes, what), true),
      docsBookMenuButton("Cancel", "x", () => docsBookHideMenu()),
    );
    docsBookIcons();
    docsBookPlace(menu, rect);
    menu.querySelector("button[data-danger]")?.focus();
  }

  async function docsBookDelete(doc, indexes, what) {
    const menu = $("[data-docs-menu]");
    menu?.querySelectorAll("button").forEach((button) => { button.disabled = true; });
    const content = docsBookWithout(doc, indexes);
    try {
      const payload = await docsBookSave(doc, content, `Docs: delete ${what} from ${doc.path}`);
      docsBookHideMenu();
      window.getSelection()?.removeAllRanges();
      docsBookRenderBody(doc);
      docsBookRefreshSeparator(doc);
      docsBookNotice(`Deleted ${what} from ${doc.path} (commit ${String(payload.commit || "").slice(0, 10)}).`, "good");
    } catch (error) {
      docsBookHideMenu();
      docsBookNotice(docsBookSaveError(doc, error), "bad", error.code === "conflict"
        ? { label: "Reload", run: () => void docsBookLoad() } : null);
    }
  }

  function docsBookShowMenu() {
    const menu = $("[data-docs-menu]");
    if (!menu) return;
    const plan = docsBookSelectionPlan();
    if (!plan) {
      docsBookHideMenu();
      return;
    }
    docsBook.menuPlan = plan;
    docsBookMark(null, []);
    const items = [docsBookMenuButton("Copy", "copy", () => void docsBookCopy(plan.text, "Text"))];
    if (plan.doc) {
      const { doc, indexes, section } = plan;
      const range = docsBookSourceRange(doc, indexes);
      items.push(docsBookMenuButton("Copy Markdown", "file-code", () => void docsBookCopy(doc.content.slice(range.start, range.end), "Markdown")));
      if (section && section.indexes.length > indexes.length) {
        items.push(docsBookMenuButton("Select section", "text-select", () => docsBookSelectBlocks(doc, section.indexes)));
      }
      if (docsBook.canEdit && !doc.truncated) {
        const blocks = `${indexes.length} block${indexes.length === 1 ? "" : "s"}`;
        items.push(
          docsBookEl("span", "docs-menu-sep"),
          docsBookMenuButton("Edit", "pencil", () => docsBookOpenEditor(doc, range)),
          docsBookMenuButton(`Delete ${blocks}`, "trash-2", () => docsBookConfirmDelete(plan, indexes, `${blocks}`), true),
        );
        if (section && section.title) {
          const name = section.title.length > 32 ? `${section.title.slice(0, 31)}…` : section.title;
          items.push(docsBookMenuButton(`Delete section “${name}”`, "trash", () =>
            docsBookConfirmDelete(plan, section.indexes, `the section “${section.title}”`), true));
        }
      }
    }
    menu.replaceChildren(...items);
    docsBookIcons();
    docsBookPlace(menu, plan.range.getBoundingClientRect());
  }

  function initDocsPage() {
    const view = $('[data-view="docs"]');
    if (!view) return;
    let timer = 0;
    const schedule = () => {
      window.clearTimeout(timer);
      timer = window.setTimeout(() => {
        const menu = $("[data-docs-menu]");
        if (menu && !menu.hidden && menu.contains(document.activeElement)) return;
        if (menu && !menu.hidden && menu.querySelector(".docs-menu-text")) return;
        docsBookShowMenu();
      }, 180);
    };
    document.addEventListener("selectionchange", schedule);
    view.addEventListener("scroll", () => {
      const plan = docsBook.menuPlan;
      const menu = $("[data-docs-menu]");
      if (plan && menu && !menu.hidden) docsBookPlace(menu, plan.range.getBoundingClientRect());
    }, { passive: true });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && !$("[data-docs-menu]")?.hidden) docsBookHideMenu();
    });
    view.addEventListener("click", (event) => {
      const edit = event.target.closest("[data-docs-edit]");
      if (edit) {
        const doc = docsBook.byPath.get(edit.dataset.docsEdit);
        if (doc?.editor) docsBookCloseEditor(doc);
        else docsBookOpenEditor(doc);
        return;
      }
      const jump = event.target.closest("a[data-docs-jump]");
      if (jump) {
        const id = decodeURIComponent(jump.getAttribute("href").slice(1));
        const target = document.getElementById(id);
        if (target) {
          event.preventDefault();
          target.scrollIntoView({ block: "start", behavior: "smooth" });
          history.replaceState(history.state, "", `#${id}`);
        }
      }
    });
    $("[data-docs-refresh]")?.addEventListener("click", () => void docsBookLoad());
    $("[data-docs-filter]")?.addEventListener("input", docsBookApplyFilter);
    window.addEventListener("beforeunload", (event) => {
      if (docsBookDirty()) {
        event.preventDefault();
        event.returnValue = "";
      }
    });
    void docsBookLoad();
  }
