(function (root) {
  "use strict";

  var AREAS = ["Highlights", "Market", "Calls", "Projects", "Chat", "Agents", "World", "Settings",
    "iPhone", "Android", "Desktop", "Server", "Other improvements"];
  var MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September",
    "October", "November", "December"];
  var BOLD = /\*\*(.+?)\*\*/g;

  function escapeHtml(text) {
    return String(text).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }

  function rich(text) {
    return escapeHtml(text).replace(BOLD, "<strong>$1</strong>");
  }

  function squash(text) {
    return String(text == null ? "" : text).split(/\s+/).filter(Boolean).join(" ");
  }

  function normKey(text) {
    return (String(text).replace(BOLD, "$1").trim().toLowerCase().match(/[a-z0-9]+/g) || []).join(" ");
  }

  function versionParts(version) {
    return version.split(/[.-]/).filter(Boolean).map(function (part) {
      return /^\d+$/.test(part) ? parseInt(part, 10) : 0;
    });
  }

  function newerFirst(a, b) {
    if (a.date !== b.date) return a.date < b.date ? 1 : -1;
    var left = versionParts(a.version), right = versionParts(b.version);
    for (var i = 0; i < Math.max(left.length, right.length); i++) {
      if (i >= left.length) return 1;
      if (i >= right.length) return -1;
      if (left[i] !== right[i]) return right[i] - left[i];
    }
    return 0;
  }

  function entries(data) {
    var releases = {}, days = {}, seen = {}, order = [];
    (data.releases || []).forEach(function (release) {
      if (release && typeof release === "object") {
        releases[(release.date || "") + "\n" + (release.version || "")] = release;
      }
    });
    (data.notes || []).forEach(function (note) {
      if (!note || typeof note !== "object") return;
      var date = String(note.date || ""), version = String(note.version || ""), text = squash(note.text);
      var key = date + "\n" + version, seenKey = key + "\n" + normKey(text);
      if (!date || AREAS.indexOf(note.area) < 0 || !text || seen[seenKey]) return;
      seen[seenKey] = true;
      if (!days[key]) {
        days[key] = { date: date, version: version, areas: {} };
        order.push(days[key]);
      }
      (days[key].areas[note.area] = days[key].areas[note.area] || []).push(text);
    });
    return order.sort(newerFirst).map(function (day) {
      var release = releases[day.date + "\n" + day.version] || {};
      return {
        date: day.date,
        version: day.version,
        title: String(release.title || ""),
        pitch: String(release.pitch || ""),
        sections: AREAS.filter(function (area) { return day.areas[area]; }).map(function (area) {
          return { title: area, items: day.areas[area] };
        })
      };
    });
  }

  function when(date) {
    var parts = date.split("-").map(function (part) { return parseInt(part, 10); });
    var month = MONTHS[parts[1] - 1];
    return parts.length === 3 ? month + " " + parts[2] + ", " + parts[0] : month + " " + parts[0];
  }

  function entryHtml(entry) {
    var html = ['<section class="release" id="' + escapeHtml(entry.version ? "v" + entry.version : entry.date) + '">',
      '<div class="release-meta">'];
    if (entry.version) html.push('<span class="tag">v' + escapeHtml(entry.version) + "</span>");
    html.push('<time class="when" datetime="' + escapeHtml(entry.date) + '">' + when(entry.date) + "</time>",
      "</div>", '<div class="release-body">');
    if (entry.title) html.push("<h2>" + rich(entry.title) + "</h2>");
    if (entry.pitch) html.push('<p class="pitch">' + rich(entry.pitch) + "</p>");
    entry.sections.forEach(function (section) {
      if (entry.sections.length > 1) html.push("<h3>" + escapeHtml(section.title) + "</h3>");
      html.push("<ul>");
      section.items.forEach(function (item) { html.push("<li>" + rich(item) + "</li>"); });
      html.push("</ul>");
    });
    html.push("</div>", "</section>");
    return html.join("");
  }

  function pageHtml(data) {
    return entries(data).map(entryHtml).join("");
  }

  function show(document) {
    var list = document.getElementById("changelog-releases");
    if (!list) return;
    root.fetch(list.getAttribute("data-source") || "/changelog.json", { cache: "no-cache" })
      .then(function (response) {
        if (!response.ok) throw new Error("HTTP " + response.status);
        return response.json();
      })
      .then(function (data) {
        list.innerHTML = pageHtml(data);
        list.removeAttribute("aria-busy");
        var target = root.location.hash && document.getElementById(decodeURIComponent(root.location.hash.slice(1)));
        if (target) target.scrollIntoView();
      })
      .catch(function () {
        list.removeAttribute("aria-busy");
        list.innerHTML = '<p class="log-status">The changelog could not be loaded. ' +
          'The notes are also at <a href="/changelog.json">/changelog.json</a>.</p>';
      });
  }

  var api = { entries: entries, entryHtml: entryHtml, pageHtml: pageHtml };
  if (typeof module === "object" && module.exports) {
    module.exports = api;
  } else {
    root.ForkMeshChangelog = api;
    if (root.document.readyState === "loading") {
      root.document.addEventListener("DOMContentLoaded", function () { show(root.document); });
    } else {
      show(root.document);
    }
  }
})(typeof window !== "undefined" ? window : globalThis);
