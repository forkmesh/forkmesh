// A compact cloud-notes desk for the World. It deliberately lives beside the
// renderer so opening an editor never reallocates the Three.js scene.
import {
  createWorldBackoff,
  markWorldHTTPFailure,
  withWorldBackoff,
} from "./world-backoff.js";

// Reads (the list, a note, the socket-driven reload) back off on any failure;
// writes only on transport and overload answers, so a rejected save stays
// retryable the moment the editor content changes.
const backoff = createWorldBackoff();
const session = () => {
  try { const value = JSON.parse(localStorage.getItem("forkmesh.session") || "null"); return value && typeof value === "object" ? value : null; }
  catch (_) { return null; }
};
const escapeHTML = (value) => String(value ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const markdown = (value) => String(value || "").split(/\r?\n/).map((line) => {
  const heading = line.match(/^(#{1,4})\s+(.+)/);
  if (heading) return `<h${heading[1].length}>${escapeHTML(heading[2])}</h${heading[1].length}>`;
  if (/^>\s?/.test(line)) return `<blockquote>${escapeHTML(line.replace(/^>\s?/, ""))}</blockquote>`;
  if (!line.trim()) return "";
  return `<p>${escapeHTML(line).replace(/\*\*(.+?)\*\*/g, "<strong>$1</strong>").replace(/`([^`]+)`/g, "<code>$1</code>")}</p>`;
}).join("");

class ForkMeshWorldNotes extends HTMLElement {
  constructor() {
    super(); this.attachShadow({ mode: "open" }); this.notes = []; this.note = null; this.socket = null; this.clientId = crypto.randomUUID(); this.draftTimer = 0;
    this.shadowRoot.innerHTML = `<style>
      :host{position:fixed;right:18px;bottom:18px;z-index:90;font:13px/1.45 "ForkMesh Sans",-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:#111827}.open{display:none;width:44px;height:44px;border:1px solid #e4e7eb;border-radius:12px;background:#fff;color:#374151;font-size:19px;cursor:pointer;box-shadow:0 8px 24px -8px rgb(15 23 42 / .16),0 2px 6px rgb(15 23 42 / .05)}.panel{display:none;width:min(760px,calc(100vw - 24px));height:min(620px,calc(100vh - 40px));background:#fff;border:1px solid #e7e9ed;border-radius:20px;box-shadow:0 20px 48px -16px rgb(15 23 42 / .24),0 4px 12px rgb(15 23 42 / .06);overflow:hidden}.panel.on{display:grid;grid-template-columns:200px 1fr}.side{border-right:1px solid #eef0f3;background:#f9fafb;overflow:auto}.head{display:flex;align-items:center;justify-content:space-between;padding:14px 12px 12px 16px;border-bottom:1px solid #eef0f3}.head strong{font-size:15px;font-weight:600}.head button,.tools button,.attach button{min-height:32px;border:1px solid #dfe3e8;background:#fff;color:#111827;border-radius:9px;padding:0 11px;font-family:inherit;font-size:13px;font-weight:500;line-height:1;cursor:pointer;box-shadow:0 1px 2px rgb(15 23 42 / .06)}.head button:hover,.tools button:hover,.attach button:hover{border-color:#c9ced6;background:#f9fafb}.tools [data-save]{border-color:#1f883d;background:#1f883d;color:#fff;box-shadow:3px 3px 0 #13632c}.tools [data-save]:hover{border-color:#1a7f37;background:#1a7f37;box-shadow:3px 3px 0 #115a27}.tools [data-save]:active{transform:translate(2px,2px);box-shadow:1px 1px 0 #115a27}.tools [data-close]{border:0;background:transparent;box-shadow:none;color:#6b7280;font-size:18px;padding:0 8px}.tools [data-close]:hover{background:#f3f4f6;color:#111827}.item{display:block;width:calc(100% - 12px);margin:2px 6px;border:0;border-radius:10px;background:transparent;color:#374151;text-align:left;padding:9px 10px;font:inherit;cursor:pointer}.item:hover{background:#f1f3f5;color:#111827}.item.sel{background:#fff;color:#111827;box-shadow:0 0 0 1px #e4e7eb,0 1px 2px rgb(15 23 42 / .06)}.editor{display:grid;grid-template-rows:auto 1fr auto;min-width:0}.tools{display:flex;gap:8px;align-items:center;padding:10px 12px;border-bottom:1px solid #eef0f3}.title{min-width:0;flex:1;background:transparent;border:0;color:#111827;font-family:inherit;font-size:15px;font-weight:600;line-height:1.3;outline:0}.title::placeholder{color:#9ca3af}.body{display:grid;grid-template-columns:1fr 1fr;min-height:0}.body textarea{resize:none;background:#fff;color:#111827;border:0;border-right:1px solid #eef0f3;padding:14px;outline:0;font:13px/1.6 "ForkMesh Mono",ui-monospace,monospace}.body textarea::placeholder{color:#9ca3af}.preview{overflow:auto;padding:14px;background:#fafbfc;color:#1f2937}.preview p{margin:.5em 0}.preview blockquote{border-left:3px solid #d1d5db;padding-left:10px;color:#4b5563}.attach{display:grid;grid-template-columns:1fr 1fr auto 58px auto;gap:8px;padding:10px 12px;border-top:1px solid #eef0f3}.attach input,.attach select{min-width:0;min-height:32px;border:1px solid #dfe3e8;background:#fff;color:#111827;border-radius:9px;padding:0 10px;font:inherit}.attach input:focus,.attach select:focus{border-color:#3b82f6;outline:0;box-shadow:0 0 0 3px rgb(59 130 246 / .22)}.status{padding:6px 12px;color:#6b7280}.hidden{display:none}.public{display:inline-flex;align-items:center;gap:6px;white-space:nowrap;color:#4b5563}.public input{accent-color:#3b82f6}@media(max-width:650px){.panel.on{grid-template-columns:120px 1fr}.body{grid-template-columns:1fr}.preview{display:none}.attach{grid-template-columns:1fr 1fr}.attach button{grid-column:span 2}}
    </style><button class="open" hidden title="Notes" aria-label="Open notes">✎</button><section class="panel" aria-label="ForkMesh notes"><aside class="side"><div class="head"><strong>Notes</strong><button data-new>＋</button></div><div data-list></div></aside><div class="editor"><div class="tools"><input class="title" data-title placeholder="Untitled note"><label class="public"><input data-public type="checkbox"> public</label><button data-save>Save</button><button data-close>×</button></div><div class="body"><textarea data-markdown placeholder="# Write a note"></textarea><article class="preview" data-preview></article></div><div><div class="attach"><input data-owner placeholder="owner"><input data-repo placeholder="repo"><select data-kind><option value="issue">Issue</option><option value="pull">PR</option><option value="discussion">Discussion</option></select><input data-number type="number" min="1" placeholder="#"><button data-attach>Attach</button></div><div class="status" data-status>Open Notes to begin.</div></div></div></section>`;
  }
  connectedCallback() {
    const $ = (q) => this.shadowRoot.querySelector(q);
    $(".open").onclick = () => this.open();
    $("[data-close]").onclick = () => this.close();
    $("[data-new]").onclick = () => this.create(); $("[data-save]").onclick = () => this.save(); $("[data-attach]").onclick = () => this.attachLink();
    $("[data-markdown]").oninput = () => { $("[data-preview]").innerHTML = markdown($("[data-markdown]").value); this.sendDraft(); };
    $("[data-title]").oninput = () => this.sendDraft();
    $("[data-list]").onclick = (event) => { const row = event.target.closest("[data-id]"); if (row) void this.select(row.dataset.id); };
  }
  open() {
    const openButton = this.shadowRoot.querySelector(".open");
    const panel = this.shadowRoot.querySelector(".panel");
    if (!openButton || !panel) return;
    openButton.hidden = true;
    panel.classList.add("on");
    void this.load();
  }
  close() {
    const openButton = this.shadowRoot.querySelector(".open");
    const panel = this.shadowRoot.querySelector(".panel");
    if (!panel || !openButton) return;
    panel.classList.remove("on");
    openButton.hidden = false;
  }
  async request(path, method = "GET", body) {
    return withWorldBackoff(backoff, `${method}:${path}`, async () => {
      const active = session(), headers = { accept: "application/json" };
      if (active?.sessionToken && active.sessionToken !== "cookie") headers.authorization = `Bearer ${active.sessionToken}`;
      if (body !== undefined) headers["content-type"] = "application/json";
      const response = await fetch(path, { method, headers, credentials: "same-origin", cache: "no-store", body: body === undefined ? undefined : JSON.stringify(body) });
      const payload = await response.json().catch(() => ({})); if (!response.ok) throw markWorldHTTPFailure(Object.assign(new Error(payload.error || `HTTP ${response.status}`), { payload, status: response.status }), response); return payload;
    }, { retryableOnly: method !== "GET" });
  }
  status(value) { this.shadowRoot.querySelector("[data-status]").textContent = value; }
  renderList() { this.shadowRoot.querySelector("[data-list]").innerHTML = this.notes.map((note) => `<button class="item ${note.id === this.note?.id ? "sel" : ""}" data-id="${note.id}"><strong>${escapeHTML(note.title)}</strong><br><small>v${note.version} · ${escapeHTML(note.role)}</small></button>`).join("") || '<div class="status">No cloud notes</div>'; }
  async load() { if (!session()) { this.status("Sign in to use cloud notes."); return; } try { const payload = await this.request("/api/notes"); this.notes = payload.notes || []; this.renderList(); if (!this.note && this.notes[0]) await this.select(this.notes[0].id); } catch (error) { this.status(error.message); } }
  async create() { try { const payload = await this.request("/api/notes", "POST", { title: "Untitled note", markdown: "" }); this.notes.unshift(payload.note); this.show(payload.note); } catch (error) { this.status(error.message); } }
  async select(id) { try { const payload = await this.request(`/api/notes/${id}`); this.show(payload.note); } catch (error) { this.status(error.message); } }
  show(note) { this.note = note; const $ = (q) => this.shadowRoot.querySelector(q); $("[data-title]").value = note.title || ""; $("[data-markdown]").value = note.markdown || ""; $("[data-preview]").innerHTML = markdown(note.markdown); $("[data-public]").checked = note.visibility === "public"; $("[data-public]").disabled = note.role !== "owner"; this.status(`${note.role} · version ${note.version} · ${(note.links || []).length} attachment(s)`); this.renderList(); this.connect(); }
  async save() { if (!this.note) return; const $ = (q) => this.shadowRoot.querySelector(q); try { const payload = await this.request(`/api/notes/${this.note.id}`, "PATCH", { baseVersion: this.note.version, title: $("[data-title]").value, markdown: $("[data-markdown]").value, visibility: $("[data-public]").checked ? "public" : "private" }); this.note = payload.note; this.notes = this.notes.map((item) => item.id === this.note.id ? this.note : item); this.show(this.note); this.status(`Saved version ${this.note.version}.`); } catch (error) { this.status(error.status === 409 ? "Changed elsewhere—latest version loaded." : error.message); if (error.payload?.note) this.show(error.payload.note); } }
  async attachLink() { if (!this.note) return; const $ = (q) => this.shadowRoot.querySelector(q); try { await this.request(`/api/notes/${this.note.id}/links`, "POST", { owner: $("[data-owner]").value, repo: $("[data-repo]").value, kind: $("[data-kind]").value, number: Number($("[data-number]").value) }); await this.select(this.note.id); this.status("Conversation attached."); } catch (error) { this.status(error.message); } }
  connect() { this.socket?.close(); if (!this.note) return; const url = new URL(`/api/notes/${this.note.id}/ws`, location.href); url.protocol = location.protocol === "https:" ? "wss:" : "ws:"; this.socket = new WebSocket(url); this.socket.onopen = () => { this.status(`Live · version ${this.note.version}`); this.socket.send(JSON.stringify({ type: "presence", clientId: this.clientId, name: session()?.nodeName || "collaborator" })); }; this.socket.onmessage = (event) => { try { const frame = JSON.parse(event.data); if (frame.type === "note-updated" && frame.version > this.note.version) void this.select(this.note.id); if (frame.type === "draft" && frame.clientId !== this.clientId && frame.baseVersion === this.note.version) { const $ = (q) => this.shadowRoot.querySelector(q); $("[data-title]").value = frame.title || "Untitled note"; $("[data-markdown]").value = frame.markdown || ""; $("[data-preview]").innerHTML = markdown(frame.markdown); this.status(`Live draft · ${String(frame.name || "collaborator").slice(0, 32)}`); } } catch (_) {} }; }
  sendDraft() { clearTimeout(this.draftTimer); this.draftTimer = setTimeout(() => { if (!this.note || this.socket?.readyState !== WebSocket.OPEN) return; const $ = (q) => this.shadowRoot.querySelector(q); this.socket.send(JSON.stringify({ type: "draft", clientId: this.clientId, name: session()?.nodeName || "collaborator", baseVersion: this.note.version, title: $("[data-title]").value, markdown: $("[data-markdown]").value })); }, 180); }
}
customElements.define("forkmesh-world-notes", ForkMeshWorldNotes);
