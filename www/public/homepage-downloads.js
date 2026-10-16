(() => {
  "use strict";

  const list = document.querySelector("[data-home-download-list]");
  const status = document.querySelector("[data-home-download-status]");
  if (!list || !status) return;

  const publicKey =
    "MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0=";
  const apiOrigin = "https://app.forkmesh.com";
  const releaseRoot = `${apiOrigin}/api/repo/forkmesh/forkmesh/raw?path=`;
  const knownAssets = new Map([
    ["forkmesh-linux-x86_64.AppImage", ["Linux AppImage", "x86_64"]],
    ["forkmesh-android-universal.apk", ["Android APK", "Universal"]],
  ]);
  // Linked whenever the signed release carries them, "Coming soon" otherwise;
  // unlike knownAssets, the status probe does not require them.
  const pendingAssets = new Map([
    ["forkmesh-windows-x86_64-setup.exe", ["Windows desktop", "x86_64"]],
    ["forkmesh-macos-arm64.dmg", ["macOS desktop", "Apple silicon"]],
    ["forkmesh-macos-x86_64.dmg", ["macOS desktop", "Intel"]],
  ]);
  const comingSoon = [];
  const platforms = new Map([
    ["forkmesh-linux-x86_64.AppImage", ["linux", "Linux", "AppImage"]],
    ["forkmesh-android-universal.apk", ["android", "Android", "APK"]],
    ["forkmesh-windows-x86_64-setup.exe", ["windows", "Windows", "Installer"]],
    ["forkmesh-macos-arm64.dmg", ["macos", "macOS", "DMG"]],
    ["forkmesh-macos-x86_64.dmg", ["macos", "macOS", "DMG"]],
  ]);
  const icons = {
    linux:
      '<path fill-rule="evenodd" d="M12 1.8c-2.4 0-3.9 2-3.9 4.6 0 1.3.2 2.2.1 3.1-.1 1-.8 1.8-1.7 3-1.1 1.5-2.1 3.2-2.1 5 0 .7.2 1.2.5 1.6-.6.3-1.1.8-1.1 1.4 0 1.3 1.7 1.7 3.2 1.9 1 .1 1.9.5 2.6.9.7.4 1.5.6 2.4.6s1.7-.2 2.4-.6c.7-.4 1.6-.8 2.6-.9 1.5-.2 3.2-.6 3.2-1.9 0-.6-.5-1.1-1.1-1.4.3-.4.5-.9.5-1.6 0-1.8-1-3.5-2.1-5-.9-1.2-1.6-2-1.7-3-.1-.9.1-1.8.1-3.1 0-2.6-1.5-4.6-3.9-4.6ZM12 10.9c-1.9 0-3.4 2.3-3.4 5 0 2.2 1.5 3.5 3.4 3.5s3.4-1.3 3.4-3.5c0-2.7-1.5-5-3.4-5ZM10.45 5.35a.85 1 0 1 0 .01 0Zm3.1 0a.85 1 0 1 0 .01 0Z"/>',
    android:
      '<path fill-rule="evenodd" d="M2.8 18.6a9.2 9.2 0 0 1 18.4 0ZM7.35 14.6a1.25 1.25 0 1 0 2.5 0a1.25 1.25 0 1 0-2.5 0Zm6.8 0a1.25 1.25 0 1 0 2.5 0a1.25 1.25 0 1 0-2.5 0Z"/><path d="M7.7 11.2 5.9 8.1M16.3 11.2l1.8-3.1" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"/>',
    windows:
      '<path d="M3 5.6 10.4 4.6v6.9H3V5.6zm0 12.8 7.4 1v-6.8H3v5.8zM11.3 4.5 21 3v8.5h-9.7V4.5zm0 8.1H21V21l-9.7-1.3v-7.1z"/>',
    macos:
      '<path d="M16.4 12.9c0-2.3 1.9-3.4 2-3.5-1.1-1.6-2.8-1.8-3.4-1.8-1.4-.1-2.8.8-3.5.8-.7 0-1.9-.8-3.1-.8-1.6 0-3 .9-3.8 2.4-1.6 2.8-.4 7 1.2 9.3.8 1.1 1.7 2.4 2.9 2.3 1.2 0 1.6-.7 3-.7 1.4 0 1.8.7 3 .7 1.2 0 2-1.1 2.8-2.2.9-1.3 1.2-2.5 1.3-2.6-.1 0-2.4-.9-2.4-3.6zM14.1 5.9c.6-.8 1.1-1.9.9-3-1 0-2.1.6-2.8 1.4-.6.7-1.1 1.8-1 2.9 1.1.1 2.2-.6 2.9-1.3z"/>',
    download:
      '<path d="M12 4v10.5m0 0-4.2-4.2M12 14.5l4.2-4.2M5 19.5h14" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"/>',
  };

  function visitorPlatform() {
    const hint =
      (navigator.userAgentData && navigator.userAgentData.platform) || "";
    const agent = `${hint} ${navigator.userAgent || ""}`;
    if (/android/i.test(agent)) return "android";
    if (/iphone|ipad|ipod/i.test(agent)) return "";
    if (/mac/i.test(agent)) return navigator.maxTouchPoints > 1 ? "" : "macos";
    if (/win/i.test(agent)) return "windows";
    if (/linux|x11|cros/i.test(agent)) return "linux";
    return "";
  }

  const currentPlatform = visitorPlatform();

  const downloadCounts = fetch(
    `${apiOrigin}/api/repo/forkmesh/forkmesh/releases/downloads`,
    { cache: "no-store", credentials: "omit" },
  )
    .then((response) => (response.ok ? response.json() : null))
    .then((body) => (body && body.ok && body.counts) || {})
    .catch(() => ({}));

  function formatCount(count) {
    const total = Number(count);
    if (!Number.isSafeInteger(total) || total <= 0) return "";
    return `${total.toLocaleString("en-US")} download${total === 1 ? "" : "s"}`;
  }

  function showCount(link, sha256) {
    downloadCounts.then((counts) => {
      const text = formatCount(counts[sha256]);
      if (!text) return;
      const body = link.querySelector(".home-download-body");
      if (!body) return;
      const count = document.createElement("span");
      count.className = "home-download-count";
      count.textContent = text;
      body.append(count);
      link.setAttribute(
        "aria-label",
        `${link.getAttribute("aria-label")}, ${text}`,
      );
    });
  }

  function glyph(name) {
    const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("fill", "currentColor");
    svg.setAttribute("aria-hidden", "true");
    svg.setAttribute("focusable", "false");
    svg.innerHTML = icons[name] || "";
    return svg;
  }

  function platformFor(assetName, title) {
    return platforms.get(assetName) || ["", title, ""];
  }

  function formatSize(bytes) {
    const size = Number(bytes);
    if (!Number.isFinite(size) || size <= 0) return "";
    if (size >= 1024 ** 3) return `${(size / 1024 ** 3).toFixed(1)} GB`;
    return `${Math.max(0.1, size / 1024 ** 2).toFixed(1)} MB`;
  }

  function base64Bytes(value) {
    return Uint8Array.from(atob(value), (character) => character.charCodeAt(0));
  }

  async function verifiedManifest(channel = "latest") {
    if (!window.crypto || !crypto.subtle) throw new Error("WebCrypto unavailable");
    const manifestPath = encodeURIComponent(
      `.forkmesh/releases/${channel}/release.json`,
    );
    const signaturePath = encodeURIComponent(
      `.forkmesh/releases/${channel}/release.json.sig`,
    );
    const [manifestResponse, signatureResponse] = await Promise.all([
      fetch(`${releaseRoot}${manifestPath}&ref=main`, { cache: "no-store", credentials: "omit" }),
      fetch(`${releaseRoot}${signaturePath}&ref=main`, { cache: "no-store", credentials: "omit" }),
    ]);
    if (!manifestResponse.ok || !signatureResponse.ok) {
      throw new Error("Release metadata unavailable");
    }

    const manifestBytes = new Uint8Array(await manifestResponse.arrayBuffer());
    const signature = new Uint8Array(await signatureResponse.arrayBuffer());
    const key = await crypto.subtle.importKey(
      "spki",
      base64Bytes(publicKey),
      { name: "Ed25519" },
      false,
      ["verify"],
    );
    if (
      signature.length !== 64 ||
      !(await crypto.subtle.verify("Ed25519", key, signature, manifestBytes))
    ) {
      throw new Error("Release signature invalid");
    }

    const manifest = JSON.parse(new TextDecoder().decode(manifestBytes));
    if (
      manifest.schema !== "forkmesh-release-v2" ||
      manifest.repo !== "forkmesh/forkmesh" ||
      !Array.isArray(manifest.assets) ||
      typeof manifest.tag !== "string"
    ) {
      throw new Error("Unsupported release manifest");
    }
    return manifest;
  }

  function versionParts(tag) {
    const match = /^v?(\d+)\.(\d+)\.(\d+)$/.exec(String(tag || "").trim());
    return match ? match.slice(1, 4).map(Number) : null;
  }

  function validAsset(asset) {
    return Boolean(
      asset &&
      /^[0-9a-f]{64}$/.test(asset.blob_sha256 || "") &&
      Number.isSafeInteger(Number(asset.size)) &&
      Number(asset.size) > 0,
    );
  }

  async function earlierAsset(assetName, tag) {
    const parts = versionParts(tag);
    if (!parts) return null;
    const [major, minor] = parts;
    for (let patch = parts[2] - 1; patch >= 0 && patch >= parts[2] - 6; patch -= 1) {
      const version = `${major}.${minor}.${patch}`;
      try {
        const candidate = await verifiedManifest(`candidate-${version}`);
        if (candidate.tag !== `v${version}`) continue;
        const asset = candidate.assets.find((item) => item && item.name === assetName);
        if (validAsset(asset)) return { asset, tag: candidate.tag };
      } catch (_) {}
    }
    return null;
  }

  function isNewer(tag, than) {
    const a = versionParts(tag);
    const b = versionParts(than);
    if (!a || !b) return false;
    for (let index = 0; index < 3; index += 1) {
      if (a[index] !== b[index]) return a[index] > b[index];
    }
    return false;
  }

  // The newest signed release, which can be ahead of `latest`: promotion
  // holds a candidate back until every platform's asset is in it, so v1.2.1
  // shipped its Linux metadata while the homepage still offered v0.8.10 with
  // no hint that a newer version existed. The version comes from the desktop
  // project on main and only counts once its candidate manifest verifies
  // against the same release key and carries a build this card offers;
  // anything else leaves `latest` as newest.
  async function newestReleaseTag(latestTag) {
    const response = await fetch(
      `${releaseRoot}${encodeURIComponent("desktop/CMakeLists.txt")}&ref=main`,
      { cache: "no-store", credentials: "omit" },
    );
    if (!response.ok) return latestTag;
    const match = /project\(\s*ForkMesh\s+VERSION\s+(\d+\.\d+\.\d+)/.exec(
      await response.text(),
    );
    if (!match || !isNewer(match[1], latestTag)) return latestTag;
    const candidate = await verifiedManifest(`candidate-${match[1]}`);
    const offered = candidate.assets.some(
      (asset) => asset && platforms.has(asset.name) && validAsset(asset),
    );
    return candidate.tag === `v${match[1]}` && offered ? candidate.tag : latestTag;
  }

  function downloadHead(platform, tag) {
    const head = document.createElement("span");
    head.className = "home-download-head";
    const icon = document.createElement("span");
    icon.className = "home-download-icon";
    icon.setAttribute("aria-hidden", "true");
    icon.append(glyph(platform[0]));
    head.append(icon);
    if (tag) {
      const version = document.createElement("span");
      version.className = "home-download-version";
      version.textContent = tag;
      head.append(version);
    }
    return head;
  }

  function downloadName(platform) {
    const name = document.createElement("span");
    name.className = "home-download-name";
    name.append(platform[1]);
    if (platform[0] && platform[0] === currentPlatform) {
      const here = document.createElement("span");
      here.className = "home-download-here";
      here.textContent = "This device";
      name.append(here);
    }
    return name;
  }

  function downloadBody(platform, details, note = "") {
    const body = document.createElement("span");
    body.className = "home-download-body";
    const meta = document.createElement("span");
    meta.className = "home-download-meta";
    meta.textContent = details.filter(Boolean).join(" · ");
    body.append(downloadName(platform), meta);
    if (note) {
      const hint = document.createElement("span");
      hint.className = "home-download-note";
      hint.textContent = note;
      body.append(hint);
    }
    return body;
  }

  function downloadAction(text, icon = "") {
    const action = document.createElement("span");
    action.className = "home-download-action";
    if (icon) action.append(glyph(icon));
    action.append(text);
    return action;
  }

  function markCurrent(card, platform) {
    card.dataset.platform = platform[0];
    if (platform[0] && platform[0] === currentPlatform) {
      card.dataset.current = "true";
    }
  }

  const linkSources = new WeakMap();

  function twinLink(links, asset) {
    const platform = platformFor(asset.name, "")[0];
    return links.find((link) => {
      const source = linkSources.get(link);
      return Boolean(
        source &&
        source.asset.blob_sha256 === asset.blob_sha256 &&
        platformFor(source.asset.name, "")[0] === platform,
      );
    });
  }

  function universalLink(link) {
    const { asset, label, tag } = linkSources.get(link);
    return downloadLink(asset, [label[0], "Universal"], tag);
  }

  function downloadLink(asset, label, tag) {
    const platform = platformFor(asset.name, label[0]);
    const size = formatSize(asset.size);
    const link = document.createElement("a");
    link.className = "home-download";
    link.href =
      `${apiOrigin}/api/repo/forkmesh/forkmesh/releases/blob/sha256/` +
      `${asset.blob_sha256}` +
      `?filename=${encodeURIComponent(asset.name)}`;
    link.download = asset.name;
    link.dataset.forkmeshProductAction = "home-download";
    link.title = `${label[0]} ${tag}`;
    link.setAttribute(
      "aria-label",
      `Download ${label[0]} ${tag}, ${label[1]}${size ? `, ${size}` : ""}`,
    );
    markCurrent(link, platform);
    link.append(
      downloadHead(platform, tag),
      downloadBody(platform, [platform[2], label[1], size]),
      downloadAction("Download", "download"),
    );
    linkSources.set(link, { asset, label, tag });
    showCount(link, asset.blob_sha256);
    return link;
  }

  function unavailableLink(label, detail = "Not published", assetName = "") {
    const [title, architecture = ""] = Array.isArray(label) ? label : [label];
    const platform = platformFor(assetName, title);
    const soon = detail === "Coming soon";
    const item = document.createElement("span");
    item.className = "home-download";
    item.setAttribute("aria-disabled", "true");
    item.title = `${title}: ${detail}`;
    markCurrent(item, platform);
    item.append(
      downloadHead(platform, ""),
      downloadBody(platform, [platform[2], architecture], soon ? "" : detail),
      downloadAction(soon ? "Coming soon" : "Unavailable"),
    );
    return item;
  }

  function comingSoonLinks() {
    return comingSoon.map(([label, detail]) => unavailableLink(label, detail));
  }

  function showStatus(latest, detail) {
    const version = document.createElement("span");
    version.className = "home-downloads-version";
    version.textContent = latest;
    const summary = document.createElement("span");
    summary.textContent = detail;
    status.replaceChildren(version, summary);
    status.dataset.state = "verified";
    downloadCounts.then((counts) => {
      const total = Object.values(counts).reduce(
        (sum, count) => sum + (Number.isSafeInteger(count) && count > 0 ? count : 0),
        0,
      );
      const text = formatCount(total);
      if (!text || !summary.isConnected) return;
      summary.textContent = `${detail} · ${text} in total`;
    });
  }

  verifiedManifest()
    .then((manifest) => {
      const assetsByName = new Map(
        manifest.assets.map((asset) => [asset && asset.name, asset]),
      );
      const links = [];
      const absent = [];
      for (const [assetName, label] of knownAssets) {
        const asset = assetsByName.get(assetName);
        if (
          !asset ||
          !/^[0-9a-f]{64}$/.test(asset.blob_sha256 || "") ||
          !Number.isSafeInteger(Number(asset.size)) ||
          Number(asset.size) <= 0
        ) {
          // One platform missing from a release must not hide the others.
          // This used to throw, and the throw was caught below as a total
          // failure: v0.8.2 shipped the Linux AppImage but no Android APK,
          // so every download on the homepage rendered "temporarily
          // unavailable" even though the AppImage was published and valid.
          // An asset that is absent, or whose digest/size does not validate,
          // is still never linked -- it is just reported on its own.
          absent.push(
            unavailableLink(
              label,
              asset
                ? "Verified download unavailable"
                : `Not in ${manifest.tag}`,
              assetName,
            ),
          );
          continue;
        }
        links.push(downloadLink(asset, label, manifest.tag));
      }
      const pending = [];
      for (const [assetName, label] of pendingAssets) {
        const asset = assetsByName.get(assetName);
        if (
          asset &&
          /^[0-9a-f]{64}$/.test(asset.blob_sha256 || "") &&
          Number.isSafeInteger(Number(asset.size)) &&
          Number(asset.size) > 0
        ) {
          const twin = twinLink(links, asset);
          if (twin) links[links.indexOf(twin)] = universalLink(twin);
          else links.push(downloadLink(asset, label, manifest.tag));
        } else {
          const placeholder = unavailableLink(label, "Coming soon", assetName);
          pending.push(placeholder);
          earlierAsset(assetName, manifest.tag).then((found) => {
            if (!found || !placeholder.isConnected) return;
            const twin = twinLink(Array.from(list.children), found.asset);
            if (twin) {
              twin.replaceWith(universalLink(twin));
              placeholder.remove();
            } else {
              placeholder.replaceWith(downloadLink(found.asset, label, found.tag));
            }
          });
        }
      }
      list.replaceChildren(...links, ...absent, ...pending, ...comingSoonLinks());
      list.setAttribute("aria-busy", "false");
      const builds = `${links.length} verified download${links.length === 1 ? "" : "s"}`;
      showStatus(`Latest version ${manifest.tag}`, `${builds} of ${manifest.tag}`);
      return newestReleaseTag(manifest.tag)
        .catch(() => manifest.tag)
        .then((newest) => {
          if (newest === manifest.tag) return;
          showStatus(
            `Latest version ${newest}`,
            `Downloads below are ${manifest.tag} until ${newest} builds are published`,
          );
        });
    })
    .catch(() => {
      const unavailable = Array.from(knownAssets, ([assetName, label]) =>
        unavailableLink(
          label, "Verified download temporarily unavailable", assetName,
        ),
      );
      const pending = Array.from(pendingAssets, ([assetName, label]) =>
        unavailableLink(label, "Coming soon", assetName),
      );
      list.replaceChildren(...unavailable, ...pending, ...comingSoonLinks());
      list.setAttribute("aria-busy", "false");
      status.textContent = "Release verification unavailable";
      status.dataset.state = "unavailable";
    });
})();
