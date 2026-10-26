/* ForkMesh World Discord bridge — on-demand UI only.
 *
 * This module never sees a Discord token and intentionally performs no polling
 * or background relay. Every network operation is initiated by a click, so the
 * World render/movement loop remains independent from connector activity.
 *
 * world-discord.js is the eager entry point and imports this module the first
 * time the bridge is actually asked for, so a visitor who never opens it never
 * pays for it. Nothing here runs until that loader calls bootWorldDiscord().
 */

import {
  createWorldBackoff,
  markWorldHTTPFailure,
  withWorldBackoff,
} from "./world-backoff.js";

const SESSION_KEY = "forkmesh.session";
// Every call here is click-initiated, so the cooldown is about a visitor who
// keeps pressing a button against a bridge that is down, and about the relay
// hop to Discord answering 429 on our behalf.
const backoff = createWorldBackoff();
const state = {
  open: false,
  reading: false,
  writing: false,
  organizations: [],
  organization: "",
  connector: null,
  messages: [],
  selectedChannel: "",
  notice: "",
  oauthNotice: "",
};

const OAUTH_OUTCOMES = {
  connected: "Discord authorization completed. Choose the public channels ForkMesh may use.",
  denied: "Discord did not authorize that Server ID. Use an account with Owner, Administrator, or Manage Server permission.",
  failed: "Discord authorization returned, but its token or account verification failed. Please connect again.",
  invalid: "Discord returned an invalid or expired authorization. Please connect again in this same browser tab.",
  invalid_state_format: "Discord returned a malformed authorization state (stage: state format).",
  invalid_state_missing: "Discord returned an unknown or already-used authorization state (stage: state lookup).",
  invalid_state_expired: "Discord authorization expired before it returned (stage: state expiry).",
  invalid_state_claim: "ForkMesh could not exclusively claim the authorization state (stage: state claim).",
  invalid_record: "ForkMesh could not validate the encrypted authorization record (stage: record validation).",
  invalid_record_storage: "ForkMesh received an incomplete stored authorization record (stage: record storage).",
  invalid_record_decrypt: "ForkMesh could not decrypt the authorization record (stage: record decryption).",
  invalid_record_state: "The callback state did not match its encrypted authorization record (stage: record state binding).",
  invalid_record_verifier: "The authorization record contained an invalid PKCE verifier (stage: record PKCE validation).",
  invalid_record_guild: "The authorization record contained an invalid Server ID (stage: record guild validation).",
  invalid_context: "Your ForkMesh login or organization-owner binding changed during authorization (stage: session context).",
  invalid_code: "Discord did not return a valid authorization code (stage: authorization code).",
  setup: "Discord OAuth configuration changed during authorization. Please connect again.",
};

function consumeOAuthOutcome() {
  const url = new URL(window.location.href);
  const outcome = text(url.searchParams.get("discord"), 20);
  if (!Object.hasOwn(OAUTH_OUTCOMES, outcome)) return;
  state.oauthNotice = OAUTH_OUTCOMES[outcome];
  state.open = true;
  url.searchParams.delete("discord");
  window.history.replaceState(null, "", `${url.pathname}${url.search}${url.hash}`);
}

function sessionToken() {
  try {
    const parsed = JSON.parse(localStorage.getItem(SESSION_KEY) || "null");
    const token = String(parsed?.sessionToken || "").trim();
    return token.length && token.length <= 2048 ? token : "";
  } catch {
    return "";
  }
}

function text(value, maximum = 2000) {
  return String(value || "").replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ").trim().slice(0, maximum);
}

function node(tag, value, className = "") {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (value !== undefined && value !== null) element.textContent = String(value);
  return element;
}

function button(label, action, className = "", disabled = false) {
  const element = node("button", label, className);
  element.type = "button";
  element.disabled = Boolean(disabled);
  element.addEventListener("click", action);
  return element;
}

async function api(path, options = {}) {
  const method = options.method || "GET";
  return withWorldBackoff(backoff, `${method}:${path}`, async () => {
    const headers = new Headers(options.headers || {});
    headers.set("accept", "application/json");
    const token = sessionToken();
    if (token) headers.set("authorization", `Bearer ${token}`);
    if (options.body !== undefined) headers.set("content-type", "application/json");
    const response = await fetch(path, {
      method,
      headers,
      body: options.body === undefined ? undefined : JSON.stringify(options.body),
      credentials: "same-origin",
      cache: "no-store",
    });
    let data = {};
    try { data = await response.json(); } catch { /* bounded generic failure */ }
    if (!response.ok) {
      const error = text(data?.error || `Request failed (${response.status})`, 180);
      const failure = new Error(error);
      failure.payload = data;
      throw markWorldHTTPFailure(failure, response);
    }
    return data;
  }, { retryableOnly: method !== "GET" });
}

function selectedChannels(connector) {
  const ids = Array.isArray(connector?.connector?.channelIds)
    ? connector.connector.channelIds.map((id) => text(id, 32)).filter(Boolean)
    : [];
  const channels = Array.isArray(connector?.channels) ? connector.channels : [];
  return ids.map((id) => {
    const record = channels.find((entry) => text(entry?.id, 32) === id);
    return { id, name: text(record?.name || `Channel ${id}`, 100) };
  });
}

function organizationRole() {
  const record = state.organizations.find((item) =>
    text(item?.name || item, 80) === state.organization);
  return text(record?.role, 24);
}

function renderSetup(container, connector) {
  const humanTask = connector?.humanTask;
  if (!humanTask) return;
  const setup = node("section", null, "world-discord-setup");
  setup.append(node("strong", text(humanTask.title, 160)));
  const list = node("ol");
  for (const item of Array.isArray(humanTask.instructions) ? humanTask.instructions : []) {
    list.append(node("li", text(item, 500)));
  }
  setup.append(list);
  if (humanTask.organizationTaskId) {
    setup.append(node("p", `Setup task ${text(humanTask.organizationTaskId, 40)} was added to the organization task list.`));
  }
  container.append(setup);
}

function render() {
  const root = document.querySelector("[data-world-discord-root]");
  const opener = document.querySelector("[data-world-discord-open]");
  if (!root || !opener) return;
  root.replaceChildren();
  root.hidden = !state.open;
  opener.setAttribute("aria-expanded", String(state.open));
  if (!state.open) return;

  const panel = node("aside", null, "world-discord-panel");
  panel.setAttribute("aria-label", "Discord bridge");
  const title = node("div", null, "world-discord-title");
  title.append(node("span", "Discord bridge"));
  title.append(button("×", () => { state.open = false; render(); }, "world-discord-close"));
  panel.append(title);

  if (!sessionToken()) {
    panel.append(node("p", "Sign in to view an organization’s Discord bridge."));
    root.append(panel);
    return;
  }

  const organizationSelect = node("select", null, "world-discord-select");
  organizationSelect.setAttribute("aria-label", "Organization");
  organizationSelect.disabled = state.reading || state.writing;
  for (const organization of state.organizations) {
    const option = node("option", organization.name || organization, "");
    option.value = organization.name || organization;
    option.selected = option.value === state.organization;
    organizationSelect.append(option);
  }
  organizationSelect.addEventListener("change", () => {
    state.organization = organizationSelect.value;
    state.connector = null;
    state.messages = [];
    state.selectedChannel = "";
    refreshConnector();
  });
  panel.append(organizationSelect);

  const tools = node("div", null, "world-discord-tools");
  tools.append(button(
    state.reading ? "Loading…" : "Refresh",
    refreshConnector,
    "",
    state.reading || state.writing,
  ));
  panel.append(tools);
  if (state.oauthNotice) {
    panel.append(node("p", state.oauthNotice, "world-discord-notice"));
  }
  if (state.notice) panel.append(node("p", state.notice, "world-discord-notice"));
  if (!state.connector) {
    panel.append(node("p", state.reading ? "Loading connector…" : "Choose an organization to begin."));
    root.append(panel);
    return;
  }

  const connector = state.connector;
  panel.append(node("p", `State: ${text(connector.state || "unknown", 80)}`, "world-discord-state"));
  renderSetup(panel, connector);

  // Setup responses are deliberately 409s; retain the role from /api/orgs as
  // a fallback so an owner can still begin authorization before a connector is
  // configured or an older Worker response includes no role projection.
  const role = text(connector.role || organizationRole(), 24);
  const isOwner = role === "owner";
  const canSend = role === "owner" || role === "admin";
  if (isOwner && connector.state === "guild_authorization_required") {
    const form = node("div", null, "world-discord-connect");
    const guild = document.createElement("input");
    guild.name = "discord-guild";
    guild.inputMode = "numeric";
    guild.autocomplete = "off";
    guild.placeholder = "Discord Server ID";
    guild.maxLength = 20;
    guild.disabled = state.reading || state.writing;
    guild.setAttribute("aria-label", "Discord Server ID");
    form.append(guild);
    form.append(button("Connect Discord server", async () => {
      if (state.writing) return;
      state.oauthNotice = "";
      const guildId = text(guild.value, 20);
      if (!/^\d{17,20}$/.test(guildId)) {
        state.notice = "Enter a valid Discord Server ID.";
        render();
        return;
      }
      state.writing = true;
      render();
      try {
        const result = await api(`/api/orgs/${encodeURIComponent(state.organization)}/discord/oauth/start`, {
          method: "POST", body: { guildId },
        });
        const authorizationUrl = String(result.authorizationUrl || "");
        if (!authorizationUrl.startsWith("https://discord.com/")) throw new Error("authorization unavailable");
        // This URL was generated from the Worker’s fixed Discord origin and
        // configured callback URI. Same-tab navigation avoids popup blockers.
        window.location.assign(authorizationUrl);
        return;
      } catch (error) {
        state.notice = text(error?.message || "Could not start Discord authorization.", 180);
      } finally {
        state.writing = false;
      }
      render();
    }, "", state.reading || state.writing));
    panel.append(form);
  }

  if (isOwner && connector.state === "channel_required" && Array.isArray(connector.channels)) {
    const chooser = node("fieldset", null, "world-discord-channel-chooser");
    chooser.append(node("legend", "Public channels"));
    const selected = new Set(selectedChannels(connector).map((channel) => channel.id));
    for (const channel of connector.channels) {
      const id = text(channel?.id, 32);
      if (!id) continue;
      const label = node("label");
      const checkbox = document.createElement("input");
      checkbox.name = "discord-channel";
      checkbox.type = "checkbox";
      checkbox.value = id;
      checkbox.checked = selected.has(id);
      checkbox.disabled = state.reading || state.writing;
      checkbox.dataset.discordChannel = "true";
      label.append(checkbox, document.createTextNode(` #${text(channel?.name || id, 100)}`));
      chooser.append(label);
    }
    chooser.append(button("Save selected channels", async () => {
      if (state.writing) return;
      const ids = [...chooser.querySelectorAll("input[data-discord-channel]:checked")]
        .map((input) => text(input.value, 32)).filter(Boolean);
      state.writing = true;
      render();
      try {
        await api(`/api/orgs/${encodeURIComponent(state.organization)}/discord`, {
          method: "PUT", body: { guildId: connector.guildId, channelIds: ids },
        });
        state.notice = "Discord channel policy saved.";
        await refreshConnector();
      } catch (error) {
        state.notice = text(error?.message || "Could not save channel policy.", 180);
      } finally {
        state.writing = false;
      }
      render();
    }, "", state.reading || state.writing));
    panel.append(chooser);
  }

  if (isOwner && (connector.connector || connector.guildId)) {
    panel.append(button("Disconnect Discord", async () => {
      if (state.writing || !window.confirm("Disconnect this organization’s Discord bridge? This revokes its stored guild consent and selected channel policy.")) return;
      state.writing = true;
      render();
      try {
        await api(`/api/orgs/${encodeURIComponent(state.organization)}/discord`, { method: "DELETE" });
        state.messages = [];
        state.selectedChannel = "";
        state.notice = "Discord bridge disconnected.";
        await refreshConnector();
      } catch (error) {
        state.notice = text(error?.message || "Could not disconnect Discord.", 180);
      } finally {
        state.writing = false;
        render();
      }
    }, "", state.reading || state.writing));
  }

  const channels = selectedChannels(connector);
  if (connector.state === "configured" && connector.configured && channels.length) {
    const channelSelect = node("select", null, "world-discord-select");
    channelSelect.setAttribute("aria-label", "Discord channel");
    channelSelect.disabled = state.reading || state.writing;
    for (const channel of channels) {
      const option = node("option", `#${channel.name}`);
      option.value = channel.id;
      option.selected = channel.id === state.selectedChannel;
      channelSelect.append(option);
    }
    channelSelect.addEventListener("change", () => {
      state.selectedChannel = channelSelect.value;
      loadMessages();
    });
    panel.append(channelSelect);
    panel.append(button("Load messages", loadMessages, "", state.reading || state.writing));
    const stream = node("div", null, "world-discord-messages");
    for (const message of state.messages) {
      const item = node("article");
      item.append(node("strong", text(message?.author?.name || "Discord member", 100)));
      item.append(node("p", text(message?.content || "(no readable message content)", 2000)));
      stream.append(item);
    }
    panel.append(stream);
    if (canSend) {
      const composer = node("div", null, "world-discord-compose");
      const input = document.createElement("textarea");
      input.name = "discord-message";
      input.maxLength = 2000;
      input.disabled = state.reading || state.writing;
      input.placeholder = "Send to the selected Discord channel";
      input.setAttribute("aria-label", "Discord message");
      composer.append(input);
      composer.append(button("Send", async () => {
        if (state.writing) return;
        state.writing = true;
        render();
        try {
          const content = text(input.value, 2000);
          if (!content) throw new Error("Write a message first.");
          await api(`/api/orgs/${encodeURIComponent(state.organization)}/discord/messages`, {
            method: "POST", body: { channelId: state.selectedChannel, content },
          });
          input.value = "";
          state.notice = "Message sent to Discord.";
          await loadMessages();
        } catch (error) {
          state.notice = text(error?.message || "Could not send message.", 180);
        } finally {
          state.writing = false;
          render();
        }
      }, "", state.reading || state.writing));
      panel.append(composer);
    }
  }
  root.append(panel);
}

async function refreshConnector() {
  if (!state.organization || state.reading) return;
  state.reading = true;
  render();
  try {
    state.connector = await api(`/api/orgs/${encodeURIComponent(state.organization)}/discord`);
    const channels = selectedChannels(state.connector);
    state.selectedChannel = channels.some((channel) => channel.id === state.selectedChannel)
      ? state.selectedChannel : (channels[0]?.id || "");
    state.notice = "";
  } catch (error) {
    // Setup states intentionally use a non-2xx status so callers cannot act
    // until consent exists. Retain their redacted task payload for the owner
    // instead of collapsing it into an unexplained generic error.
    state.connector = error?.payload?.state ? error.payload : null;
    state.notice = text(error?.message || "Could not load Discord connector.", 180);
  } finally {
    state.reading = false;
    render();
  }
}

async function loadMessages() {
  if (!state.organization || !state.selectedChannel || state.reading) return;
  state.reading = true;
  render();
  try {
    const result = await api(
      `/api/orgs/${encodeURIComponent(state.organization)}/discord/messages?channelId=${encodeURIComponent(state.selectedChannel)}`);
    state.messages = Array.isArray(result.messages) ? result.messages.slice(0, 50) : [];
    const diagnostic = result?.messageContentDiagnostic?.humanTask;
    state.notice = diagnostic ? text(diagnostic.title, 180) : "";
  } catch (error) {
    state.notice = text(error?.message || "Could not load messages.", 180);
  } finally {
    state.reading = false;
    render();
  }
}

// The bridge mounts inside the settings panel, so opening it from the 3D
// Discord board or an OAuth return has to open settings first.
function openWorldSettings() {
  // Re-opening an already-open panel re-runs its focus handoff, which would
  // pull focus off the bridge the visitor just asked for.
  if (document.querySelector("[data-world-settings]")?.dataset.open === "true")
    return;
  document.querySelector("forkmesh-world")?.toggleSettings?.(true);
}

function revealInSettings() {
  const root = document.querySelector("[data-world-discord-root]");
  root?.scrollIntoView?.({ block: "nearest" });
}

async function showPanel() {
  state.open = true;
  openWorldSettings();
  render();
  revealInSettings();
  if (state.organizations.length || state.reading || !sessionToken()) return;
  state.reading = true;
  render();
  try {
    const result = await api("/api/orgs");
    state.organizations = Array.isArray(result.orgs) ? result.orgs : [];
    state.organization = text(state.organizations[0]?.name || "", 80);
    if (!state.organization) state.notice = "You are not a member of an organization yet.";
  } catch (error) {
    state.notice = text(error?.message || "Could not load organizations.", 180);
  } finally {
    state.reading = false;
    render();
  }
  if (state.organization) refreshConnector();
}

async function openPanel() {
  if (state.open) {
    state.open = false;
    render();
    return;
  }
  await showPanel();
}

export function bootWorldDiscord() {
  consumeOAuthOutcome();
  window.addEventListener("forkmesh:open-discord", showPanel);

  const bindSettingsTrigger = () => {
    const trigger = document.querySelector("[data-world-discord-open]");
    if (!trigger || trigger.dataset.worldDiscordBound === "true") return false;
    trigger.dataset.worldDiscordBound = "true";
    trigger.setAttribute("aria-expanded", "false");
    trigger.addEventListener("click", () => {
      void openPanel();
    });
    render();
    if (state.open) void showPanel();
    return true;
  };

  if (!bindSettingsTrigger()) {
    const observer = new MutationObserver(() => {
      if (!bindSettingsTrigger()) return;
      observer.disconnect();
    });
    observer.observe(document.body, { childList: true, subtree: true });
  }
}

// The loader consumed the click (or the open event) that got us imported, so
// it reopens the panel here rather than asking the visitor to click twice.
export function showWorldDiscordPanel() {
  return showPanel();
}
