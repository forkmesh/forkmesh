var ForkMeshWorkflows = (function () {
  "use strict";

  var SPEC = null;
  var LIMITS = {};
  var sessions = {};
  var nextSession = 1;

  var NODE_ID_RE = /^[A-Za-z0-9_-]{1,32}$/;
  var CREDENTIAL_ID_RE = /^[0-9a-f]{32}$/;
  var EXPRESSION_RE = /\{\{\s*([\s\S]*?)\s*\}\}/g;
  var WHOLE_EXPRESSION_RE = /^\{\{\s*([\s\S]*?)\s*\}\}$/;
  var PLACEHOLDER_RE = /\{([A-Za-z][A-Za-z0-9_]*)\}/g;
  var WHOLE_PLACEHOLDER_RE = /^\{([A-Za-z][A-Za-z0-9_]*)\}$/;
  var ROOT_RE = /^\$([A-Za-z_]+)/;
  var STEP_RE = /\.([A-Za-z_$][\p{L}\p{N}_$-]*)|\[\s*(-?[0-9]+)\s*\]|\[\s*"([^"]*)"\s*\]|\[\s*'([^']*)'\s*\]/uy;
  var NUMBER_RE = /^[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?$/;
  var SPECIAL_NUMBER_RE = /^[+-]?(inf|infinity|nan)$/i;
  var BODY_METHODS = ["POST", "PUT", "PATCH", "DELETE"];
  var TRIGGERS = ["core.manual", "core.webhook", "core.schedule"];

  function NodeError(message) {
    this.message = message;
  }

  function fail(message) {
    throw new NodeError(message);
  }

  function has(object, key) {
    return Object.prototype.hasOwnProperty.call(object, key);
  }

  function isObject(value) {
    return value !== null && typeof value === "object" && !Array.isArray(value);
  }

  function truthy(value) {
    if (value === null || value === undefined || value === false || value === 0 || value === "") return false;
    if (typeof value === "number" && isNaN(value)) return false;
    if (Array.isArray(value)) return value.length > 0;
    if (isObject(value)) return Object.keys(value).length > 0;
    return true;
  }

  function or(value, fallback) {
    return truthy(value) ? value : fallback;
  }

  function get(object, key) {
    if (!isObject(object) || !has(object, key)) return null;
    var value = object[key];
    return value === undefined ? null : value;
  }

  function pyNumber(value) {
    if (isNaN(value)) return "NaN";
    if (value === Infinity) return "Infinity";
    if (value === -Infinity) return "-Infinity";
    return String(value);
  }

  function pyString(text) {
    return JSON.stringify(text).replace(/[\u007f-\uffff]/g, function (ch) {
      return "\\u" + ("0000" + ch.charCodeAt(0).toString(16)).slice(-4);
    });
  }

  function dumps(value) {
    if (value === null || value === undefined) return "null";
    if (value === true) return "true";
    if (value === false) return "false";
    if (typeof value === "number") return pyNumber(value);
    if (typeof value === "string") return pyString(value);
    if (Array.isArray(value)) return "[" + value.map(dumps).join(", ") + "]";
    if (isObject(value)) {
      return "{" + Object.keys(value).map(function (key) {
        return pyString(key) + ": " + dumps(value[key]);
      }).join(", ") + "}";
    }
    return pyString(String(value));
  }

  function pyStr(value) {
    if (value === null || value === undefined) return "None";
    if (value === true) return "True";
    if (value === false) return "False";
    if (typeof value === "number") return pyNumber(value);
    if (typeof value === "string") return value;
    return dumps(value);
  }

  function stringify(value) {
    if (value === null || value === undefined) return "";
    if (value === true) return "true";
    if (value === false) return "false";
    if (Array.isArray(value) || isObject(value)) return dumps(value);
    return pyStr(value);
  }

  function pyFloat(value) {
    if (typeof value === "number") return value;
    if (typeof value !== "string") return null;
    var text = value.trim().replace(/_/g, "");
    if (NUMBER_RE.test(text)) return Number(text);
    if (SPECIAL_NUMBER_RE.test(text)) {
      if (/nan/i.test(text)) return NaN;
      return text.charAt(0) === "-" ? -Infinity : Infinity;
    }
    return null;
  }

  function percentEncode(text, safe, plus) {
    var out = "";
    var bytes = utf8(text);
    for (var i = 0; i < bytes.length; i++) {
      var code = bytes[i];
      var ch = String.fromCharCode(code);
      if ((code >= 48 && code <= 57) || (code >= 65 && code <= 90) || (code >= 97 && code <= 122) ||
          ch === "_" || ch === "." || ch === "-" || ch === "~" || (safe && safe.indexOf(ch) >= 0)) {
        out += ch;
      } else if (plus && ch === " ") {
        out += "+";
      } else {
        out += "%" + (code < 16 ? "0" : "") + code.toString(16).toUpperCase();
      }
    }
    return out;
  }

  function quote(text, safe) {
    return percentEncode(text, safe, false);
  }

  function urlencode(pairs) {
    return pairs.map(function (pair) {
      return percentEncode(pyStr(pair[0]), "", true) + "=" + percentEncode(pyStr(pair[1]), "", true);
    }).join("&");
  }

  function entries(object) {
    return Object.keys(object).map(function (key) { return [key, object[key]]; });
  }

  function utf8(text) {
    var bytes = [];
    for (var i = 0; i < text.length; i++) {
      var code = text.charCodeAt(i);
      if (code >= 0xd800 && code <= 0xdbff && i + 1 < text.length) {
        var low = text.charCodeAt(i + 1);
        if (low >= 0xdc00 && low <= 0xdfff) {
          code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
          i++;
        }
      }
      if (code < 0x80) {
        bytes.push(code);
      } else if (code < 0x800) {
        bytes.push(0xc0 | (code >> 6), 0x80 | (code & 63));
      } else if (code < 0x10000) {
        bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
      } else {
        bytes.push(0xf0 | (code >> 18), 0x80 | ((code >> 12) & 63), 0x80 | ((code >> 6) & 63), 0x80 | (code & 63));
      }
    }
    return bytes;
  }

  function base64(text) {
    var alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    var bytes = utf8(text);
    var out = "";
    for (var i = 0; i < bytes.length; i += 3) {
      var a = bytes[i];
      var b = i + 1 < bytes.length ? bytes[i + 1] : 0;
      var c = i + 2 < bytes.length ? bytes[i + 2] : 0;
      var triple = (a << 16) | (b << 8) | c;
      out += alphabet.charAt((triple >> 18) & 63) + alphabet.charAt((triple >> 12) & 63);
      out += i + 1 < bytes.length ? alphabet.charAt((triple >> 6) & 63) : "=";
      out += i + 2 < bytes.length ? alphabet.charAt(triple & 63) : "=";
    }
    return out;
  }

  function parseJson(text) {
    try {
      return { ok: true, value: JSON.parse(text) };
    } catch (error) {
      return { ok: false };
    }
  }

  function integrations() {
    return SPEC.integrations;
  }

  function coreNodes() {
    return SPEC.coreNodes;
  }

  function appKey(type) {
    return type.indexOf("app.") === 0 ? type.slice(4) : "";
  }

  function nodeSpec(type) {
    if (has(coreNodes(), type)) return coreNodes()[type];
    var key = appKey(type);
    if (!key || !has(integrations(), key)) return null;
    return { outputs: ["main"], app: integrations()[key] };
  }

  function isTrigger(type) {
    return has(coreNodes(), type) && coreNodes()[type].trigger === true;
  }

  function outputsFor(type) {
    var spec = nodeSpec(type);
    return spec ? spec.outputs.slice() : [];
  }

  function fieldsFor(type, params) {
    if (has(coreNodes(), type)) return coreNodes()[type].fields;
    var key = appKey(type);
    if (!key || !has(integrations(), key)) return [];
    var operations = integrations()[key].operations;
    var op = pyStr(or(get(params || {}, "operation"), ""));
    return has(operations, op) ? operations[op].fields : [];
  }

  function hostOf(url) {
    var match = /^[A-Za-z][A-Za-z0-9+.-]*:\/\/([^\/?#]*)/.exec(url);
    if (!match) return "";
    var hostinfo = match[1].slice(match[1].lastIndexOf("@") + 1);
    if (hostinfo.indexOf("[") >= 0) {
      return hostinfo.slice(hostinfo.indexOf("[") + 1).split("]")[0].toLowerCase();
    }
    return hostinfo.split(":")[0].toLowerCase();
  }

  function catalog() {
    var core = Object.keys(coreNodes()).map(function (key) {
      var spec = coreNodes()[key];
      var out = {};
      Object.keys(spec).forEach(function (name) { out[name] = spec[name]; });
      out.type = key;
      return out;
    });
    var apps = Object.keys(integrations()).map(function (key) {
      var app = integrations()[key];
      return {
        key: key,
        type: "app." + key,
        name: app.name,
        category: app.category,
        color: app.color,
        description: app.description,
        host: app.base.indexOf("https://") === 0 ? hostOf(app.base) : "",
        credential: app.auth.fields.map(function (field) {
          return { key: field.key, label: field.label, secret: field.secret === true, placeholder: field.placeholder || "" };
        }),
        operations: Object.keys(app.operations).map(function (opKey) {
          var op = app.operations[opKey];
          return { key: opKey, name: op.name, method: op.method, fields: op.fields };
        })
      };
    });
    var categories = [];
    apps.forEach(function (app) {
      if (categories.indexOf(app.category) < 0) categories.push(app.category);
    });
    categories.sort();
    return {
      core: core,
      integrations: apps,
      categories: categories,
      packs: packs(),
      operators: SPEC.operators.map(function (pair) { return { value: pair[0], label: pair[1] }; })
    };
  }

  function packs() {
    var found = {};
    var order = [];
    Object.keys(integrations()).forEach(function (key) {
      var category = integrations()[key].category;
      if (!has(found, category)) {
        found[category] = [];
        order.push(category);
      }
      found[category].push(key);
    });
    return order.map(function (category) {
      var id = SPEC.packPrefix + category.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
      return { id: id, category: category, integrations: found[category] };
    });
  }

  function cleanText(value, limit) {
    var text = pyStr(value === null || value === undefined ? "" : value).split("\u0000").join("");
    return Array.from(text.trim()).slice(0, limit).join("");
  }

  function cleanValue(value) {
    if (typeof value === "boolean" || typeof value === "number") return { value: value };
    if (Array.isArray(value) || isObject(value)) {
      return dumps(value).length > LIMITS.maxValueChars ? null : { value: value };
    }
    if (value === null || value === undefined) return { value: "" };
    var text = pyStr(value).split("\u0000").join("");
    return text.length > LIMITS.maxValueChars ? null : { value: text };
  }

  function cleanParams(type, raw) {
    var params = isObject(raw) ? raw : {};
    var clean = {};
    if (type.indexOf("app.") === 0) {
      var operations = integrations()[appKey(type)].operations;
      var operation = pyStr(or(get(params, "operation"), ""));
      if (!has(operations, operation)) operation = Object.keys(operations)[0];
      clean.operation = operation;
    }
    var fields = fieldsFor(type, Object.keys(clean).length ? clean : params);
    for (var i = 0; i < fields.length; i++) {
      var key = fields[i].key;
      if (!has(params, key)) continue;
      var value = cleanValue(params[key]);
      if (value === null) return null;
      clean[key] = value.value;
    }
    return clean;
  }

  function toInt(value, fallback) {
    var number = typeof value === "boolean" ? (value ? 1 : 0) : pyFloat(value);
    if (number === null || !isFinite(number)) return fallback;
    return Math.trunc(number);
  }

  function clean(data) {
    if (!isObject(data)) return { error: "invalid_workflow" };
    var name = cleanText(get(data, "name"), LIMITS.maxName) || "Untitled workflow";
    var rawNodes = or(get(data, "nodes"), []);
    var rawConnections = or(get(data, "connections"), []);
    if (!Array.isArray(rawNodes) || !Array.isArray(rawConnections)) return { error: "invalid_workflow" };
    if (rawNodes.length > LIMITS.maxNodes) return { error: "too_many_nodes" };
    if (rawConnections.length > LIMITS.maxConnections) return { error: "too_many_connections" };
    var nodes = [];
    var ids = {};
    var names = {};
    var triggers = 0;
    for (var i = 0; i < rawNodes.length; i++) {
      var raw = rawNodes[i];
      if (!isObject(raw)) return { error: "invalid_node" };
      var id = pyStr(or(get(raw, "id"), ""));
      var type = pyStr(or(get(raw, "type"), ""));
      if (!NODE_ID_RE.test(id) || has(ids, id)) return { error: "invalid_node" };
      if (nodeSpec(type) === null) return { error: "unknown_node_type" };
      var nodeName = cleanText(get(raw, "name"), LIMITS.maxNodeName);
      if (!nodeName || has(names, nodeName)) return { error: "duplicate_node_name" };
      var params = cleanParams(type, get(raw, "params"));
      if (params === null) return { error: "value_too_long" };
      var credential = pyStr(or(get(raw, "credential"), ""));
      if (credential && !CREDENTIAL_ID_RE.test(credential)) return { error: "invalid_credential" };
      if (isTrigger(type)) triggers++;
      ids[id] = true;
      names[nodeName] = true;
      nodes.push({
        id: id,
        type: type,
        name: nodeName,
        x: Math.max(-LIMITS.positionLimit, Math.min(LIMITS.positionLimit, toInt(get(raw, "x"), 0))),
        y: Math.max(-LIMITS.positionLimit, Math.min(LIMITS.positionLimit, toInt(get(raw, "y"), 0))),
        params: params,
        credential: type.indexOf("app.") === 0 ? credential : "",
        continueOnFail: get(raw, "continueOnFail") === true,
        disabled: get(raw, "disabled") === true
      });
    }
    if (triggers > 1) return { error: "too_many_triggers" };
    var byId = {};
    nodes.forEach(function (node) { byId[node.id] = node; });
    var connections = [];
    var seen = {};
    for (var j = 0; j < rawConnections.length; j++) {
      var conn = rawConnections[j];
      if (!isObject(conn)) return { error: "invalid_connection" };
      var source = pyStr(or(get(conn, "from"), ""));
      var target = pyStr(or(get(conn, "to"), ""));
      var output = pyStr(or(get(conn, "output"), "main"));
      if (!has(byId, source) || !has(byId, target) || source === target) return { error: "invalid_connection" };
      if (outputsFor(byId[source].type).indexOf(output) < 0) return { error: "invalid_connection" };
      if (isTrigger(byId[target].type)) return { error: "invalid_connection" };
      var key = JSON.stringify([source, output, target]);
      if (has(seen, key)) continue;
      seen[key] = true;
      connections.push({ from: source, output: output, to: target });
    }
    if (topologicalOrder(nodes, connections) === null) return { error: "cycle" };
    return { workflow: { name: name, nodes: nodes, connections: connections }, error: "" };
  }

  function topologicalOrder(nodes, connections) {
    var incoming = {};
    var edges = {};
    nodes.forEach(function (node) {
      incoming[node.id] = 0;
      edges[node.id] = [];
    });
    connections.forEach(function (conn) {
      edges[conn.from].push(conn.to);
      incoming[conn.to] += 1;
    });
    var ready = nodes.filter(function (node) { return incoming[node.id] === 0; }).map(function (node) { return node.id; });
    var order = [];
    while (ready.length) {
      var current = ready.shift();
      order.push(current);
      edges[current].forEach(function (target) {
        incoming[target] -= 1;
        if (incoming[target] === 0) ready.push(target);
      });
    }
    return order.length === nodes.length ? order : null;
  }

  function triggerNode(workflow) {
    var nodes = or(get(workflow, "nodes"), []);
    for (var i = 0; i < nodes.length; i++) {
      if (isTrigger(nodes[i].type)) return nodes[i];
    }
    return null;
  }

  function scheduleMinutes(workflow) {
    var node = triggerNode(workflow);
    if (!node || node.type !== "core.schedule" || node.disabled) return 0;
    var minutes = toInt(get(node.params, "every"), 0);
    if (minutes <= 0) return 0;
    return Math.max(LIMITS.scheduleMinMinutes, Math.min(LIMITS.scheduleMaxMinutes, minutes));
  }

  function manualPayload(workflow, payload) {
    if (isObject(payload)) return payload;
    var trigger = triggerNode(workflow);
    if (trigger && trigger.type === "core.manual") {
      var raw = get(trigger.params, "payload");
      if (isObject(raw)) return raw;
      if (typeof raw === "string" && raw.trim()) {
        var parsed = parseJson(raw);
        if (!parsed.ok) return {};
        return isObject(parsed.value) ? parsed.value : { value: parsed.value };
      }
    }
    if (trigger && trigger.type === "core.webhook") return { method: "POST", query: {}, body: {} };
    return {};
  }

  function safeUrl(value) {
    var raw = pyStr(or(value, "")).trim();
    if (!raw || raw.length > 4096 || /[\r\n\t \\]/.test(raw)) return "";
    var match = /^([A-Za-z][A-Za-z0-9+.-]*):\/\/([^\/?#]*)/.exec(raw);
    if (!match || match[1].toLowerCase() !== "https") return "";
    var netloc = match[2];
    if (netloc.indexOf("@") >= 0) return "";
    var host;
    var port;
    var open = netloc.indexOf("[");
    var close = netloc.indexOf("]");
    if (open >= 0 || close >= 0) {
      if (open < 0 || close < open) return "";
      host = netloc.slice(open + 1, close);
      var rest = netloc.slice(close + 1);
      port = rest.indexOf(":") >= 0 ? rest.slice(rest.indexOf(":") + 1) : "";
    } else {
      var colon = netloc.indexOf(":");
      host = colon >= 0 ? netloc.slice(0, colon) : netloc;
      port = colon >= 0 ? netloc.slice(colon + 1) : "";
    }
    host = host.toLowerCase().replace(/\.+$/, "");
    if (!host) return "";
    if (port) {
      if (!/^[0-9]+$/.test(port)) return "";
      var number = Number(port);
      if (number < 1 || number > 65535) return "";
    }
    var suffixes = SPEC.blockedHostSuffixes;
    for (var i = 0; i < suffixes.length; i++) {
      var suffix = suffixes[i];
      if (suffix.charAt(0) === ".") {
        if (host.slice(-suffix.length) === suffix) return "";
      } else if (host === suffix || host.slice(-(suffix.length + 1)) === "." + suffix) {
        return "";
      }
    }
    var v4 = ipv4(host);
    if (v4 !== null) return globalV4(v4) ? raw : "";
    if (host.indexOf(":") >= 0) {
      var v6 = ipv6(host);
      return v6 !== null && globalV6(v6) ? raw : "";
    }
    if (/^[0-9.]+$/.test(host) || /^0x[0-9a-f]+$/.test(host)) return "";
    if (host.indexOf(".") < 0) return "";
    return raw;
  }

  function ipv4(host) {
    var parts = host.split(".");
    if (parts.length !== 4) return null;
    var bytes = [];
    for (var i = 0; i < 4; i++) {
      if (!/^(0|[1-9][0-9]{0,2})$/.test(parts[i])) return null;
      var value = Number(parts[i]);
      if (value > 255) return null;
      bytes.push(value);
    }
    return bytes;
  }

  function globalV4(b) {
    var blocked = [
      [0, 0, 0, 0, 8], [10, 0, 0, 0, 8], [100, 64, 0, 0, 10], [127, 0, 0, 0, 8], [169, 254, 0, 0, 16],
      [172, 16, 0, 0, 12], [192, 0, 0, 0, 24], [192, 0, 2, 0, 24], [192, 88, 99, 0, 24], [192, 168, 0, 0, 16],
      [198, 18, 0, 0, 15], [198, 51, 100, 0, 24], [203, 0, 113, 0, 24], [224, 0, 0, 0, 4], [240, 0, 0, 0, 4]
    ];
    var address = ((b[0] << 24) >>> 0) + (b[1] << 16) + (b[2] << 8) + b[3];
    for (var i = 0; i < blocked.length; i++) {
      var net = blocked[i];
      var base = ((net[0] << 24) >>> 0) + (net[1] << 16) + (net[2] << 8) + net[3];
      var size = Math.pow(2, 32 - net[4]);
      if (address >= base && address < base + size) return false;
    }
    return true;
  }

  function ipv6(host) {
    var text = host.split("%")[0];
    if (/[^0-9a-f:.]/.test(text)) return null;
    var halves = text.split("::");
    if (halves.length > 2) return null;
    function groups(part) {
      if (!part) return [];
      var out = [];
      var pieces = part.split(":");
      for (var i = 0; i < pieces.length; i++) {
        var piece = pieces[i];
        if (piece.indexOf(".") >= 0 && i === pieces.length - 1) {
          var v4 = ipv4(piece);
          if (v4 === null) return null;
          out.push((v4[0] << 8) | v4[1], (v4[2] << 8) | v4[3]);
        } else if (/^[0-9a-f]{1,4}$/.test(piece)) {
          out.push(parseInt(piece, 16));
        } else {
          return null;
        }
      }
      return out;
    }
    var head = groups(halves[0]);
    var tail = halves.length === 2 ? groups(halves[1]) : [];
    if (head === null || tail === null) return null;
    if (halves.length === 1) return head.length === 8 ? head : null;
    if (head.length + tail.length > 7) return null;
    var fill = [];
    for (var i = head.length + tail.length; i < 8; i++) fill.push(0);
    return head.concat(fill, tail);
  }

  function globalV6(g) {
    if (g[0] < 0x2000 || g[0] > 0x3fff) return false;
    if (g[0] === 0x2001 && (g[1] === 0x0db8 || g[1] < 0x0200)) return false;
    if (g[0] === 0x2002) return false;
    if (g[0] === 0x3fff && g[1] < 0x1000) return false;
    return true;
  }

  function step(value, key) {
    if (Array.isArray(value)) {
      if (typeof key !== "number") return null;
      if (key >= -value.length && key < value.length) {
        var item = value[key < 0 ? value.length + key : key];
        return item === undefined ? null : item;
      }
      return null;
    }
    if (isObject(value) && typeof key === "string") return get(value, key);
    return null;
  }

  function evaluate(expr, context) {
    var text = pyStr(or(expr, "")).trim();
    var match = ROOT_RE.exec(text);
    if (!match) {
      fail("Expressions start with $json, $node, $trigger, $now, $today, $item, $vars or $workflow: " + text.slice(0, 80));
    }
    var root = match[1];
    if (!has(context, root)) fail("Unknown expression variable $" + root);
    var value = context[root];
    var position = match[0].length;
    while (position < text.length) {
      STEP_RE.lastIndex = position;
      var found = STEP_RE.exec(text);
      if (!found) fail("Cannot read expression: " + text.slice(0, 80));
      if (found[1] !== undefined) {
        value = step(value, found[1]);
      } else if (found[2] !== undefined) {
        value = step(value, parseInt(found[2], 10));
      } else {
        value = step(value, found[3] !== undefined ? found[3] : found[4]);
      }
      position = STEP_RE.lastIndex;
    }
    return value;
  }

  function resolve(value, context) {
    if (Array.isArray(value)) return value.map(function (item) { return resolve(item, context); });
    if (isObject(value)) {
      var out = {};
      Object.keys(value).forEach(function (key) { out[key] = resolve(value[key], context); });
      return out;
    }
    if (typeof value !== "string" || value.indexOf("{{") < 0) return value;
    var whole = WHOLE_EXPRESSION_RE.exec(value.trim());
    if (whole) return evaluate(whole[1], context);
    return value.replace(EXPRESSION_RE, function (all, inner) {
      return stringify(evaluate(inner, context));
    });
  }

  function coerce(field, value) {
    var kind = field.type || "string";
    var empty = value === null || value === undefined || (typeof value === "string" && !value.trim());
    if (empty) {
      if (field.required) fail(field.label + " is required");
      return null;
    }
    if (kind === "number") {
      if (typeof value === "boolean") fail(field.label + " must be a number");
      var number = pyFloat(value);
      if (number === null) fail(field.label + " must be a number");
      return number;
    }
    if (kind === "boolean") {
      if (typeof value === "boolean") return value;
      return ["true", "1", "yes", "on"].indexOf(pyStr(value).trim().toLowerCase()) >= 0;
    }
    if (kind === "json") {
      if (Array.isArray(value) || isObject(value)) return value;
      var parsed = parseJson(pyStr(value));
      if (!parsed.ok) fail(field.label + " is not valid JSON");
      return parsed.value;
    }
    if (kind === "value") return value;
    if (kind === "select") {
      var allowed = (field.options || []).map(function (option) { return option.value; });
      var text = stringify(value);
      if (allowed.length && allowed.indexOf(text) < 0) fail(field.label + " must be one of " + allowed.join(", "));
      return text;
    }
    return stringify(value);
  }

  function fieldValues(fields, params, context) {
    var values = {};
    fields.forEach(function (field) {
      var raw = has(params, field.key) ? params[field.key] : (field.default === undefined ? null : field.default);
      if (field.type === "code") {
        values[field.key] = coerce(field, raw);
        return;
      }
      if (field.type === "json" && typeof raw === "string" && raw.trim()) {
        var parsed = parseJson(raw);
        if (parsed.ok) raw = parsed.value;
      }
      values[field.key] = coerce(field, resolve(raw, context));
    });
    return values;
  }

  function fill(template, values, encode, slashes) {
    return template.replace(PLACEHOLDER_RE, function (all, key) {
      if (!has(values, key)) return all;
      var text = stringify(values[key]);
      if (!encode) return text;
      return quote(text, slashes && slashes.indexOf(key) >= 0 ? "/" : "");
    });
  }

  function placeholders(template) {
    var found = [];
    template.replace(PLACEHOLDER_RE, function (all, key) {
      found.push(key);
      return all;
    });
    return found;
  }

  function fillPresent(templates, cred) {
    var out = {};
    Object.keys(templates || {}).forEach(function (name) {
      var template = templates[name];
      var missing = placeholders(template).some(function (key) { return !pyStr(or(get(cred, key), "")); });
      if (!missing) out[name] = fill(template, cred, false);
    });
    return out;
  }

  function render(template, values) {
    if (Array.isArray(template)) {
      return template.map(function (item) { return render(item, values); }).filter(function (item) { return item !== null; });
    }
    if (isObject(template)) {
      var out = {};
      Object.keys(template).forEach(function (key) {
        var rendered = render(template[key], values);
        if (rendered !== null) out[key] = rendered;
      });
      return out;
    }
    if (typeof template === "string") {
      var whole = WHOLE_PLACEHOLDER_RE.exec(template);
      if (whole && has(values, whole[1])) return values[whole[1]];
      return fill(template, values, false);
    }
    return template === undefined ? null : template;
  }

  function templateKeys(template, found) {
    found = found || {};
    if (Array.isArray(template)) {
      template.forEach(function (item) { templateKeys(item, found); });
    } else if (isObject(template)) {
      Object.keys(template).forEach(function (key) { templateKeys(template[key], found); });
    } else if (typeof template === "string") {
      placeholders(template).forEach(function (key) { found[key] = true; });
    }
    return found;
  }

  function setPath(target, dotted, value) {
    var parts = dotted.split(".");
    for (var i = 0; i < parts.length - 1; i++) {
      var next = get(target, parts[i]);
      if (!isObject(next)) {
        next = {};
        target[parts[i]] = next;
      }
      target = next;
    }
    target[parts[parts.length - 1]] = value === undefined ? null : value;
  }

  function formPairs(body, prefix) {
    var pairs = [];
    Object.keys(body).forEach(function (key) {
      var value = body[key];
      var name = prefix ? prefix + "[" + key + "]" : key;
      if (isObject(value)) {
        pairs = pairs.concat(formPairs(value, name));
      } else if (Array.isArray(value)) {
        value.forEach(function (item) { pairs.push([name, stringify(item)]); });
      } else {
        pairs.push([name, stringify(value)]);
      }
    });
    return pairs;
  }

  function setDefault(headers, name, value) {
    if (!has(headers, name)) headers[name] = value;
  }

  function copy(object) {
    var out = {};
    Object.keys(object || {}).forEach(function (key) { out[key] = object[key]; });
    return out;
  }

  function baseUrl(app, cred) {
    var base = app.base;
    app.auth.fields.forEach(function (field) {
      var token = "{" + field.key + "}";
      if (base.indexOf(token) < 0) return;
      var value = pyStr(or(get(cred, field.key), ""));
      base = base.split(token).join(field.url ? value.replace(/\/+$/, "") : quote(value, ":"));
    });
    return base;
  }

  function buildAppRequest(key, params, values, cred) {
    var app = integrations()[key];
    var op = app.operations[params.operation];
    var pathKeys = [];
    placeholders(op.path).forEach(function (name) {
      if (pathKeys.indexOf(name) < 0) pathKeys.push(name);
    });
    pathKeys.forEach(function (name) {
      var value = has(values, name) ? values[name] : null;
      if (value === null || value === "") fail("Missing " + name);
    });
    var slashes = op.fields.filter(function (field) { return field.slashes; }).map(function (field) { return field.key; });
    var url = baseUrl(app, cred) + fill(op.path, values, true, slashes);
    var headers = copy(app.headers);
    var query = copy(op.query);
    var auth = app.auth;
    var authHeaders = fillPresent(auth.headers, cred);
    Object.keys(authHeaders).forEach(function (name) { headers[name] = authHeaders[name]; });
    var authQuery = fillPresent(auth.query, cred);
    Object.keys(authQuery).forEach(function (name) { query[name] = authQuery[name]; });
    if (auth.basic) {
      var pair = fill(auth.basic[0], cred, false) + ":" + fill(auth.basic[1], cred, false);
      headers.Authorization = "Basic " + base64(pair);
    }
    Object.keys(op.headers || {}).forEach(function (name) { headers[name] = fill(op.headers[name], values, false); });
    var bodyFields = [];
    op.fields.forEach(function (field) {
      var fieldKey = field.key;
      var value = has(values, fieldKey) ? values[fieldKey] : null;
      if (pathKeys.indexOf(fieldKey) >= 0 || value === null || field.in === "template") return;
      var where = field.in || (BODY_METHODS.indexOf(op.method) >= 0 ? "body" : "query");
      if (where === "query") {
        var parts = fieldKey.split(".");
        query[parts[parts.length - 1]] = stringify(value);
      } else if (where === "query_object") {
        if (!isObject(value)) fail(field.label + " must be a JSON object");
        Object.keys(value).forEach(function (name) { query[name] = stringify(value[name]); });
      } else if (where === "header") {
        headers[field.header || fieldKey] = stringify(value);
      } else {
        bodyFields.push([fieldKey, value]);
      }
    });
    var body = null;
    if (op.body !== undefined && op.body !== null) {
      body = render(op.body, values);
      if (isObject(body)) {
        var keys = templateKeys(op.body);
        bodyFields.forEach(function (pair) {
          if (!has(keys, pair[0])) setPath(body, pair[0], pair[1]);
        });
      }
    } else if (bodyFields.length) {
      body = {};
      bodyFields.forEach(function (pair) { setPath(body, pair[0], pair[1]); });
    }
    if (auth.body && BODY_METHODS.indexOf(op.method) >= 0) {
      if (body === null) body = {};
      if (isObject(body)) {
        Object.keys(auth.body).forEach(function (name) { body[name] = fill(auth.body[name], cred, false); });
      }
    }
    var encoding = op.encoding || app.encoding || "json";
    var text = null;
    if (body !== null && BODY_METHODS.indexOf(op.method) >= 0) {
      if (encoding === "form") {
        if (!isObject(body)) fail("A form body must be a JSON object");
        text = urlencode(formPairs(body, ""));
        setDefault(headers, "Content-Type", "application/x-www-form-urlencoded");
      } else {
        text = dumps(body);
        setDefault(headers, "Content-Type", "application/json");
      }
    }
    if (Object.keys(query).length) url += (url.indexOf("?") >= 0 ? "&" : "?") + urlencode(entries(query));
    setDefault(headers, "Accept", "application/json");
    return { method: op.method, url: url, headers: headers, body: text };
  }

  function buildHttpRequest(values) {
    var method = or(values.method, "GET");
    var url = pyStr(or(values.url, ""));
    var rawHeaders = or(values.headers, {});
    if (!isObject(rawHeaders)) fail("Headers must be a JSON object");
    var headers = {};
    Object.keys(rawHeaders).forEach(function (name) { headers[name] = stringify(rawHeaders[name]); });
    var query = or(values.query, {});
    if (!isObject(query)) fail("Query parameters must be a JSON object");
    if (Object.keys(query).length) {
      url += (url.indexOf("?") >= 0 ? "&" : "?") + urlencode(entries(query).map(function (pair) {
        return [pair[0], stringify(pair[1])];
      }));
    }
    var text = null;
    var body = has(values, "body") ? values.body : null;
    if (BODY_METHODS.indexOf(method) >= 0 && body !== null && body !== "") {
      var kind = or(values.bodyType, "json");
      if (kind === "text") {
        text = stringify(body);
        setDefault(headers, "Content-Type", "text/plain");
      } else {
        if (typeof body === "string") {
          var parsed = parseJson(body);
          if (!parsed.ok) fail("Body is not valid JSON");
          body = parsed.value;
        }
        if (kind === "form") {
          if (!isObject(body)) fail("A form body must be a JSON object");
          text = urlencode(formPairs(body, ""));
          setDefault(headers, "Content-Type", "application/x-www-form-urlencoded");
        } else {
          text = dumps(body);
          setDefault(headers, "Content-Type", "application/json");
        }
      }
    }
    return { method: method, url: url, headers: headers, body: text };
  }

  function parseResponse(text) {
    var body = pyStr(or(text, "")).slice(0, LIMITS.maxResponseChars);
    var stripped = body.trim();
    if (stripped.charAt(0) === "{" || stripped.charAt(0) === "[") {
      var parsed = parseJson(stripped);
      if (parsed.ok) return parsed.value;
    }
    return stripped ? { body: body } : {};
  }

  function texts(blocks) {
    if (!Array.isArray(blocks)) return "";
    return blocks.filter(function (block) {
      return isObject(block) && (has(block, "type") ? block.type : "text") === "text";
    }).map(function (block) { return pyStr(or(block.text, "")); }).join("");
  }

  function withText(output, parsed) {
    if (!output || !isObject(parsed)) return parsed;
    var text = null;
    if (output === "anthropic") {
      text = texts(get(parsed, "content"));
    } else if (output === "openai") {
      var choice = or(step(get(parsed, "choices"), 0), {});
      text = get(or(step(choice, "message"), {}), "content");
    } else if (output === "gemini") {
      var candidate = or(step(get(parsed, "candidates"), 0), {});
      text = texts(get(or(step(candidate, "content"), {}), "parts"));
    } else if (output === "cohere") {
      text = texts(get(or(get(parsed, "message"), {}), "content"));
    }
    if (text === null) return parsed;
    var out = copy(parsed);
    out.text = text;
    return out;
  }

  function itemsFrom(value) {
    if (Array.isArray(value)) {
      return value.slice(0, LIMITS.maxItems).map(function (item) { return isObject(item) ? item : { value: item }; });
    }
    if (isObject(value)) return [value];
    return [{ value: value === undefined ? null : value }];
  }

  function numberOf(value) {
    if (typeof value === "boolean") return null;
    return pyFloat(value);
  }

  function isEmptyValue(value) {
    return value === null || value === undefined || value === "" ||
      (Array.isArray(value) && value.length === 0) || (isObject(value) && Object.keys(value).length === 0);
  }

  function checkCondition(left, operator, right) {
    if (operator === "isEmpty") return isEmptyValue(left);
    if (operator === "isNotEmpty") return !isEmptyValue(left);
    if (operator === "isTrue") return left === true || stringify(left).toLowerCase() === "true";
    if (operator === "isFalse") return left === false || ["false", ""].indexOf(stringify(left).toLowerCase()) >= 0;
    if (["gt", "gte", "lt", "lte"].indexOf(operator) >= 0) {
      var a = numberOf(left);
      var b = numberOf(right);
      if (a === null || b === null) return false;
      return { gt: a > b, gte: a >= b, lt: a < b, lte: a <= b }[operator];
    }
    var x = stringify(left);
    var y = stringify(right);
    if (operator === "equals") return x === y;
    if (operator === "notEquals") return x !== y;
    if (operator === "contains") {
      if (Array.isArray(left)) return left.some(function (item) { return stringify(item) === y; });
      return x.indexOf(y) >= 0;
    }
    if (operator === "notContains") {
      if (Array.isArray(left)) return !left.some(function (item) { return stringify(item) === y; });
      return x.indexOf(y) < 0;
    }
    if (operator === "startsWith") return x.indexOf(y) === 0;
    if (operator === "endsWith") return x.length >= y.length && x.slice(x.length - y.length) === y;
    if (operator === "matches" || operator === "notMatches") {
      var compiled = Actions.regex(y, false);
      var hit = Script.rxSearch(compiled.compiled, x, 0) !== null;
      return operator === "matches" ? hit : !hit;
    }
    fail("Unknown operator " + pyStr(operator));
  }

  function summarizeItems(items) {
    return items.slice(0, LIMITS.maxRunItems).map(function (item) {
      var encoded = dumps(item);
      return encoded.length > LIMITS.maxRunItemChars ? { _truncated: encoded.slice(0, LIMITS.maxRunItemChars) } : item;
    });
  }

  function utf8Decode(bytes, strict) {
    var out = "";
    var i = 0;
    var replacement = "\ufffd";
    while (i < bytes.length) {
      var first = bytes[i];
      if (first < 0x80) {
        out += String.fromCharCode(first);
        i += 1;
        continue;
      }
      var needed = 0;
      var code = 0;
      var lower = 0x80;
      var upper = 0xbf;
      if (first >= 0xc2 && first <= 0xdf) {
        needed = 1;
        code = first & 0x1f;
      } else if (first >= 0xe0 && first <= 0xef) {
        needed = 2;
        code = first & 0xf;
        if (first === 0xe0) lower = 0xa0;
        if (first === 0xed) upper = 0x9f;
      } else if (first >= 0xf0 && first <= 0xf4) {
        needed = 3;
        code = first & 0x7;
        if (first === 0xf0) lower = 0x90;
        if (first === 0xf4) upper = 0x8f;
      } else {
        if (strict) return null;
        out += replacement;
        i += 1;
        continue;
      }
      var j = 1;
      var ok = true;
      for (; j <= needed; j++) {
        var byte = bytes[i + j];
        if (byte === undefined || byte < lower || byte > upper) {
          ok = false;
          break;
        }
        lower = 0x80;
        upper = 0xbf;
        code = (code << 6) | (byte & 0x3f);
      }
      if (!ok) {
        if (strict) return null;
        out += replacement;
        i += j;
        continue;
      }
      out += String.fromCodePoint(code);
      i += needed + 1;
    }
    return out;
  }

  function utf8Strict(bytes) {
    return utf8Decode(bytes, true);
  }

  var BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  function base64Bytes(bytes) {
    var out = "";
    for (var i = 0; i < bytes.length; i += 3) {
      var a = bytes[i];
      var b = i + 1 < bytes.length ? bytes[i + 1] : 0;
      var c = i + 2 < bytes.length ? bytes[i + 2] : 0;
      var triple = (a << 16) | (b << 8) | c;
      out += BASE64_ALPHABET.charAt((triple >> 18) & 63) + BASE64_ALPHABET.charAt((triple >> 12) & 63);
      out += i + 1 < bytes.length ? BASE64_ALPHABET.charAt((triple >> 6) & 63) : "=";
      out += i + 2 < bytes.length ? BASE64_ALPHABET.charAt(triple & 63) : "=";
    }
    return out;
  }

  function base64DecodeBytes(text) {
    var bytes = [];
    var buffer = 0;
    var bits = 0;
    for (var i = 0; i < text.length; i++) {
      buffer = (buffer << 6) | BASE64_ALPHABET.indexOf(text.charAt(i));
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        bytes.push((buffer >> bits) & 0xff);
      }
    }
    return bytes;
  }

  function hexBytes(bytes) {
    return bytes.map(function (byte) { return (byte < 16 ? "0" : "") + byte.toString(16); }).join("");
  }

  function wordsToBytes(words, littleEndian) {
    var out = [];
    words.forEach(function (word) {
      if (littleEndian) out.push(word & 0xff, (word >>> 8) & 0xff, (word >>> 16) & 0xff, (word >>> 24) & 0xff);
      else out.push((word >>> 24) & 0xff, (word >>> 16) & 0xff, (word >>> 8) & 0xff, word & 0xff);
    });
    return out;
  }

  function padMessage(bytes, blockSize, lengthBytes, littleEndian) {
    var out = bytes.slice();
    var bitLength = BigInt(bytes.length) * BigInt(8);
    out.push(0x80);
    while (out.length % blockSize !== blockSize - lengthBytes) out.push(0);
    var lengthOut = [];
    for (var i = 0; i < lengthBytes; i++) {
      lengthOut.push(Number((bitLength >> BigInt(8 * i)) & BigInt(0xff)));
    }
    if (!littleEndian) lengthOut.reverse();
    return out.concat(lengthOut);
  }

  function md5(bytes) {
    var s = [7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21];
    var k = [];
    for (var i = 0; i < 64; i++) k.push(Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) >>> 0);
    var message = padMessage(bytes, 64, 8, true);
    var a0 = 0x67452301;
    var b0 = 0xefcdab89;
    var c0 = 0x98badcfe;
    var d0 = 0x10325476;
    for (var offset = 0; offset < message.length; offset += 64) {
      var m = [];
      for (var w = 0; w < 16; w++) {
        var p = offset + w * 4;
        m.push((message[p] | (message[p + 1] << 8) | (message[p + 2] << 16) | (message[p + 3] << 24)) >>> 0);
      }
      var a = a0;
      var b = b0;
      var c = c0;
      var d = d0;
      for (var j = 0; j < 64; j++) {
        var f;
        var g;
        if (j < 16) {
          f = (b & c) | (~b & d);
          g = j;
        } else if (j < 32) {
          f = (d & b) | (~d & c);
          g = (5 * j + 1) % 16;
        } else if (j < 48) {
          f = b ^ c ^ d;
          g = (3 * j + 5) % 16;
        } else {
          f = c ^ (b | ~d);
          g = (7 * j) % 16;
        }
        f = (f + a + k[j] + m[g]) >>> 0;
        a = d;
        d = c;
        c = b;
        b = (b + ((f << s[j]) | (f >>> (32 - s[j])))) >>> 0;
      }
      a0 = (a0 + a) >>> 0;
      b0 = (b0 + b) >>> 0;
      c0 = (c0 + c) >>> 0;
      d0 = (d0 + d) >>> 0;
    }
    return wordsToBytes([a0, b0, c0, d0], true);
  }

  function rotl(x, n) {
    return ((x << n) | (x >>> (32 - n))) >>> 0;
  }

  function rotr(x, n) {
    return ((x >>> n) | (x << (32 - n))) >>> 0;
  }

  function bigWords(message, offset, count) {
    var out = [];
    for (var w = 0; w < count; w++) {
      var p = offset + w * 4;
      out.push(((message[p] << 24) | (message[p + 1] << 16) | (message[p + 2] << 8) | message[p + 3]) >>> 0);
    }
    return out;
  }

  function sha1(bytes) {
    var message = padMessage(bytes, 64, 8, false);
    var h = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0];
    for (var offset = 0; offset < message.length; offset += 64) {
      var w = bigWords(message, offset, 16);
      for (var t = 16; t < 80; t++) w.push(rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1));
      var a = h[0];
      var b = h[1];
      var c = h[2];
      var d = h[3];
      var e = h[4];
      for (var i = 0; i < 80; i++) {
        var f;
        var k;
        if (i < 20) {
          f = (b & c) | (~b & d);
          k = 0x5a827999;
        } else if (i < 40) {
          f = b ^ c ^ d;
          k = 0x6ed9eba1;
        } else if (i < 60) {
          f = (b & c) | (b & d) | (c & d);
          k = 0x8f1bbcdc;
        } else {
          f = b ^ c ^ d;
          k = 0xca62c1d6;
        }
        var temp = (rotl(a, 5) + f + e + k + w[i]) >>> 0;
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
      }
      h = [(h[0] + a) >>> 0, (h[1] + b) >>> 0, (h[2] + c) >>> 0, (h[3] + d) >>> 0, (h[4] + e) >>> 0];
    }
    return wordsToBytes(h, false);
  }

  var SHA256_K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ];

  function sha256(bytes) {
    var message = padMessage(bytes, 64, 8, false);
    var h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    for (var offset = 0; offset < message.length; offset += 64) {
      var w = bigWords(message, offset, 16);
      for (var t = 16; t < 64; t++) {
        var s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >>> 3);
        var s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >>> 10);
        w.push((w[t - 16] + s0 + w[t - 7] + s1) >>> 0);
      }
      var a = h[0];
      var b = h[1];
      var c = h[2];
      var d = h[3];
      var e = h[4];
      var f = h[5];
      var g = h[6];
      var hh = h[7];
      for (var i = 0; i < 64; i++) {
        var S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        var ch = (e & f) ^ (~e & g);
        var temp1 = (hh + S1 + ch + SHA256_K[i] + w[i]) >>> 0;
        var S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        var maj = (a & b) ^ (a & c) ^ (b & c);
        var temp2 = (S0 + maj) >>> 0;
        hh = g;
        g = f;
        f = e;
        e = (d + temp1) >>> 0;
        d = c;
        c = b;
        b = a;
        a = (temp1 + temp2) >>> 0;
      }
      h = [(h[0] + a) >>> 0, (h[1] + b) >>> 0, (h[2] + c) >>> 0, (h[3] + d) >>> 0, (h[4] + e) >>> 0, (h[5] + f) >>> 0,
        (h[6] + g) >>> 0, (h[7] + hh) >>> 0];
    }
    return wordsToBytes(h, false);
  }

  var SHA512_K = null;

  function sha512(bytes) {
    var mask = (BigInt(1) << BigInt(64)) - BigInt(1);
    function big(hex) {
      return BigInt("0x" + hex);
    }
    if (SHA512_K === null) {
      SHA512_K = [
        "428a2f98d728ae22", "7137449123ef65cd", "b5c0fbcfec4d3b2f", "e9b5dba58189dbbc", "3956c25bf348b538", "59f111f1b605d019",
        "923f82a4af194f9b", "ab1c5ed5da6d8118", "d807aa98a3030242", "12835b0145706fbe", "243185be4ee4b28c", "550c7dc3d5ffb4e2",
        "72be5d74f27b896f", "80deb1fe3b1696b1", "9bdc06a725c71235", "c19bf174cf692694", "e49b69c19ef14ad2", "efbe4786384f25e3",
        "0fc19dc68b8cd5b5", "240ca1cc77ac9c65", "2de92c6f592b0275", "4a7484aa6ea6e483", "5cb0a9dcbd41fbd4", "76f988da831153b5",
        "983e5152ee66dfab", "a831c66d2db43210", "b00327c898fb213f", "bf597fc7beef0ee4", "c6e00bf33da88fc2", "d5a79147930aa725",
        "06ca6351e003826f", "142929670a0e6e70", "27b70a8546d22ffc", "2e1b21385c26c926", "4d2c6dfc5ac42aed", "53380d139d95b3df",
        "650a73548baf63de", "766a0abb3c77b2a8", "81c2c92e47edaee6", "92722c851482353b", "a2bfe8a14cf10364", "a81a664bbc423001",
        "c24b8b70d0f89791", "c76c51a30654be30", "d192e819d6ef5218", "d69906245565a910", "f40e35855771202a", "106aa07032bbd1b8",
        "19a4c116b8d2d0c8", "1e376c085141ab53", "2748774cdf8eeb99", "34b0bcb5e19b48a8", "391c0cb3c5c95a63", "4ed8aa4ae3418acb",
        "5b9cca4f7763e373", "682e6ff3d6b2b8a3", "748f82ee5defb2fc", "78a5636f43172f60", "84c87814a1f0ab72", "8cc702081a6439ec",
        "90befffa23631e28", "a4506cebde82bde9", "bef9a3f7b2c67915", "c67178f2e372532b", "ca273eceea26619c", "d186b8c721c0c207",
        "eada7dd6cde0eb1e", "f57d4f7fee6ed178", "06f067aa72176fba", "0a637dc5a2c898a6", "113f9804bef90dae", "1b710b35131c471b",
        "28db77f523047d84", "32caab7b40c72493", "3c9ebe0a15c9bebc", "431d67c49c100d4c", "4cc5d4becb3e42b6", "597f299cfc657e2a",
        "5fcb6fab3ad6faec", "6c44198c4a475817"
      ].map(big);
    }
    function rot(x, n) {
      return ((x >> BigInt(n)) | (x << BigInt(64 - n))) & mask;
    }
    var message = padMessage(bytes, 128, 16, false);
    var h = ["6a09e667f3bcc908", "bb67ae8584caa73b", "3c6ef372fe94f82b", "a54ff53a5f1d36f1", "510e527fade682d1",
      "9b05688c2b3e6c1f", "1f83d9abfb41bd6b", "5be0cd19137e2179"].map(big);
    for (var offset = 0; offset < message.length; offset += 128) {
      var w = [];
      for (var t = 0; t < 16; t++) {
        var value = BigInt(0);
        for (var b8 = 0; b8 < 8; b8++) value = (value << BigInt(8)) | BigInt(message[offset + t * 8 + b8]);
        w.push(value);
      }
      for (t = 16; t < 80; t++) {
        var s0 = rot(w[t - 15], 1) ^ rot(w[t - 15], 8) ^ (w[t - 15] >> BigInt(7));
        var s1 = rot(w[t - 2], 19) ^ rot(w[t - 2], 61) ^ (w[t - 2] >> BigInt(6));
        w.push((w[t - 16] + s0 + w[t - 7] + s1) & mask);
      }
      var a = h[0];
      var bb = h[1];
      var c = h[2];
      var d = h[3];
      var e = h[4];
      var f = h[5];
      var g = h[6];
      var hh = h[7];
      for (var i = 0; i < 80; i++) {
        var S1 = rot(e, 14) ^ rot(e, 18) ^ rot(e, 41);
        var ch = (e & f) ^ (~e & mask & g);
        var temp1 = (hh + S1 + ch + SHA512_K[i] + w[i]) & mask;
        var S0 = rot(a, 28) ^ rot(a, 34) ^ rot(a, 39);
        var maj = (a & bb) ^ (a & c) ^ (bb & c);
        var temp2 = (S0 + maj) & mask;
        hh = g;
        g = f;
        f = e;
        e = (d + temp1) & mask;
        d = c;
        c = bb;
        bb = a;
        a = (temp1 + temp2) & mask;
      }
      h = [(h[0] + a) & mask, (h[1] + bb) & mask, (h[2] + c) & mask, (h[3] + d) & mask, (h[4] + e) & mask, (h[5] + f) & mask,
        (h[6] + g) & mask, (h[7] + hh) & mask];
    }
    var out = [];
    h.forEach(function (word) {
      for (var k = 7; k >= 0; k--) out.push(Number((word >> BigInt(8 * k)) & BigInt(0xff)));
    });
    return out;
  }

  var HASHES = { md5: [md5, 64], sha1: [sha1, 64], sha256: [sha256, 64], sha512: [sha512, 128] };

  function hmacBytes(algorithm, key, data) {
    var fn = HASHES[algorithm][0];
    var block = HASHES[algorithm][1];
    if (key.length > block) key = fn(key);
    var padded = key.slice();
    while (padded.length < block) padded.push(0);
    var inner = padded.map(function (byte) { return byte ^ 0x36; });
    var outer = padded.map(function (byte) { return byte ^ 0x5c; });
    return fn(outer.concat(fn(inner.concat(data))));
  }

  var Script = (function () {
    var MAX_STEPS = 100000;
    var MAX_DEPTH = 40;
    var MAX_NESTING = 50;
    var MAX_STRING = 1000000;
    var MAX_ARRAY = 100000;
    var MAX_LOGS = 100;
    var MAX_LOG_CHARS = 1000;
    var MAX_INT = 1e18;
    var JS_SPACE = "\t\n\u000b\u000c\r \u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000\ufeff";
    var SPACE = charSet(JS_SPACE);
    var LINE_ENDS = charSet("\n\r\u2028\u2029");
    var DIGITS = charSet("0123456789");
    var HEX = charSet("0123456789abcdefABCDEF");
    var KEYWORDS = charSet(["var", "let", "const", "if", "else", "for", "while", "do", "break", "continue", "return",
      "function", "true", "false", "null", "typeof", "instanceof", "new", "throw", "try", "catch", "finally", "switch",
      "case", "default", "delete", "this", "void", "in"]);
    var UNSUPPORTED = charSet(["class", "async", "await", "yield", "import", "export", "with", "debugger", "super", "enum",
      "extends", "static"]);
    var PUNCTUATORS = [">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "??=", "=>", "==", "!=", "<=",
      ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>", "{", "}",
      "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "?", ":", "=", "."];
    var REGEX_AFTER_KEYWORDS = charSet(["return", "typeof", "case", "do", "else", "in", "instanceof", "new", "delete", "void",
      "throw"]);
    var BINARY = {
      "||": 1, "??": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6, "===": 6, "!==": 6, "<": 7, ">": 7, "<=": 7,
      ">=": 7, "in": 7, "instanceof": 7, "<<": 8, ">>": 8, ">>>": 8, "+": 9, "-": 9, "*": 10, "/": 10, "%": 10, "**": 11
    };
    var ASSIGN = charSet(["=", "+=", "-=", "*=", "/=", "%=", "**=", "<<=", ">>=", ">>>=", "&=", "|=", "^=", "&&=", "||=", "??="]);
    var DECIMAL_RE = /^[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$/;
    var FLOAT_PREFIX_RE = /^[+-]?(?:Infinity|(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?)/;
    var INDEX_RE = /^(?:0|[1-9][0-9]*)$/;
    var ISO_DATE_RE = /^([+-][0-9]{6}|[0-9]{4})(?:-([0-9]{2})(?:-([0-9]{2}))?)?(?:T([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\.([0-9]{1,9}))?)?(Z|[+-][0-9]{2}:[0-9]{2})?)?$/;
    var URI_UNRESERVED = charSet("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.!~*'()");
    var DAY_NAMES = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    var MONTH_NAMES = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    var SURROGATE_RE = /[\ud800-\udfff]/;
    var ID_START_RE = /^\p{L}$/u;
    var ID_PART_RE = /^[\p{L}\p{N}]$/u;
    var BREAK = { signal: "break" };
    var CONTINUE = { signal: "continue" };
    var SHORT = { signal: "short" };
    var lastText = null;
    var lastPoints = null;

    function charSet(items) {
      var out = Object.create(null);
      var list = typeof items === "string" ? Array.from(items) : items;
      for (var i = 0; i < list.length; i++) out[list[i]] = true;
      return out;
    }

    function inSet(set, ch) {
      return ch !== undefined && set[ch] === true;
    }

    function dict() {
      return Object.create(null);
    }

    function isDict(value) {
      return value !== null && typeof value === "object" && !Array.isArray(value) && Object.getPrototypeOf(value) === null;
    }

    function hasKey(object, key) {
      return Object.prototype.hasOwnProperty.call(object, key);
    }

    function ScriptError(line, text) {
      this.line = line;
      this.text = text;
      this.message = "Line " + line + ": " + text;
    }

    function ScriptLimit(message) {
      this.message = message;
    }

    function Thrown(value, line) {
      this.value = value;
      this.line = line || 0;
    }

    function Return(value) {
      this.value = value;
    }

    function SFunction(name, params, rest, body, scope, arrow, expression) {
      this.name = name;
      this.params = params;
      this.rest = rest;
      this.body = body;
      this.scope = scope;
      this.arrow = arrow;
      this.expression = expression;
    }

    function SNative(name, fn, props, construct) {
      this.name = name;
      this.fn = fn;
      this.props = props || dict();
      this.construct = construct || null;
    }

    function SDate(ms) {
      this.ms = timeClip(ms);
    }

    function SRegExp(source, flags, compiled) {
      this.source = source;
      this.flags = flags;
      this.compiled = compiled;
      this.lastIndex = 0;
      this.names = compiled.names;
    }

    function SCollection(kind) {
      this.kind = kind;
      this.map = new Map();
    }

    function points(text) {
      if (text === lastText) return lastPoints;
      var out = SURROGATE_RE.test(text) ? Array.from(text) : null;
      lastText = text;
      lastPoints = out;
      return out;
    }

    function slen(text) {
      var cps = points(text);
      return cps ? cps.length : text.length;
    }

    function charAt(text, index) {
      var cps = points(text);
      return cps ? cps[index] : text.charAt(index);
    }

    function cslice(text, start, end) {
      var cps = points(text);
      if (!cps) return text.slice(start, end);
      return cps.slice(start, end === undefined ? cps.length : end).join("");
    }

    function toUnit(text, index) {
      var cps = points(text);
      if (!cps) return index;
      var unit = 0;
      for (var i = 0; i < index && i < cps.length; i++) unit += cps[i].length;
      return unit;
    }

    function toPoint(text, unit) {
      if (!SURROGATE_RE.test(text)) return unit;
      return Array.from(text.slice(0, unit)).length;
    }

    function cfind(text, needle, start) {
      var found = text.indexOf(needle, toUnit(text, start));
      return found < 0 ? -1 : toPoint(text, found);
    }

    function compareStrings(a, b) {
      if (!SURROGATE_RE.test(a) && !SURROGATE_RE.test(b)) return a < b ? -1 : (a > b ? 1 : 0);
      var x = Array.from(a);
      var y = Array.from(b);
      for (var i = 0; i < x.length && i < y.length; i++) {
        var p = x[i].codePointAt(0);
        var q = y[i].codePointAt(0);
        if (p !== q) return p < q ? -1 : 1;
      }
      return x.length < y.length ? -1 : (x.length > y.length ? 1 : 0);
    }

    function floorDiv(a, b) {
      return Math.floor(a / b);
    }

    function pyMod(a, b) {
      var r = a % b;
      return r !== 0 && (r < 0) !== (b < 0) ? r + b : r;
    }

    function pad(number, width) {
      var text = String(Math.abs(number));
      while (text.length < width) text = "0" + text;
      return text;
    }

    function timeClip(ms) {
      if (ms !== ms || ms === Infinity || ms === -Infinity || Math.abs(ms) > 8.64e15) return NaN;
      return Math.trunc(ms) + 0;
    }

    function Token(kind, value, line, newline, extra) {
      this.kind = kind;
      this.value = value;
      this.line = line;
      this.newline = newline;
      this.extra = extra === undefined ? null : extra;
    }

    function isIdStart(ch) {
      return ch === "_" || ch === "$" || (ch !== undefined && ID_START_RE.test(ch));
    }

    function isIdPart(ch) {
      return ch === "_" || ch === "$" || (ch !== undefined && ID_PART_RE.test(ch));
    }

    function startsAt(src, pos, text) {
      for (var i = 0; i < text.length; i++) {
        if (src[pos + i] !== text.charAt(i)) return false;
      }
      return true;
    }

    function findSeq(src, text, from) {
      for (var i = from; i + text.length <= src.length; i++) {
        if (startsAt(src, i, text)) return i;
      }
      return -1;
    }

    function Lexer(src, line) {
      this.src = src;
      this.pos = 0;
      this.line = line;
      this.tokens = [];
    }

    Lexer.prototype.error = function (message) {
      throw new ScriptError(this.line, message);
    };

    Lexer.prototype.skipSpace = function (newline) {
      var src = this.src;
      while (this.pos < src.length) {
        var ch = src[this.pos];
        if (inSet(LINE_ENDS, ch)) {
          if (!(ch === "\r" && src[this.pos + 1] === "\n")) this.line += 1;
          newline = true;
          this.pos += 1;
        } else if (inSet(SPACE, ch)) {
          this.pos += 1;
        } else if (ch === "/" && src[this.pos + 1] === "/") {
          while (this.pos < src.length && !inSet(LINE_ENDS, src[this.pos])) this.pos += 1;
        } else if (ch === "/" && src[this.pos + 1] === "*") {
          var end = findSeq(src, "*/", this.pos + 2);
          if (end < 0) this.error("Unterminated comment");
          for (var i = this.pos; i < end; i++) {
            var c = src[i];
            if (inSet(LINE_ENDS, c) && !(c === "\r" && src[i + 1] === "\n")) {
              this.line += 1;
              newline = true;
            }
          }
          this.pos = end + 2;
        } else {
          break;
        }
      }
      return newline;
    };

    Lexer.prototype.regexAllowed = function () {
      if (!this.tokens.length) return true;
      var last = this.tokens[this.tokens.length - 1];
      if (last.kind === "punct") return [")", "]", "}"].indexOf(last.value) < 0;
      if (last.kind === "kw") return inSet(REGEX_AFTER_KEYWORDS, last.value);
      return false;
    };

    Lexer.prototype.number = function () {
      var src = this.src;
      var start = this.pos;
      var value;
      if (src[start] === "0" && src[start + 1] !== undefined && "xXoObB".indexOf(src[start + 1]) >= 0) {
        var base = { x: 16, o: 8, b: 2 }[src[start + 1].toLowerCase()];
        var allowed = "0123456789abcdef".slice(0, base);
        this.pos += 2;
        var digits = "";
        while (this.pos < src.length && (src[this.pos] === "_" || allowed.indexOf(src[this.pos].toLowerCase()) >= 0 &&
            src[this.pos].toLowerCase().length === 1)) {
          digits += src[this.pos];
          this.pos += 1;
        }
        digits = digits.split("_").join("");
        if (!digits) this.error("Invalid number");
        value = Number(BigInt({ 16: "0x", 8: "0o", 2: "0b" }[base] + digits));
      } else {
        while (this.pos < src.length && "0123456789_".indexOf(src[this.pos]) >= 0) this.pos += 1;
        if (src[this.pos] === ".") {
          this.pos += 1;
          while (this.pos < src.length && "0123456789_".indexOf(src[this.pos]) >= 0) this.pos += 1;
        }
        if (src[this.pos] === "e" || src[this.pos] === "E") {
          var mark = this.pos;
          this.pos += 1;
          if (src[this.pos] === "+" || src[this.pos] === "-") this.pos += 1;
          if (inSet(DIGITS, src[this.pos])) {
            while (this.pos < src.length && inSet(DIGITS, src[this.pos])) this.pos += 1;
          } else {
            this.pos = mark;
          }
        }
        value = Number(src.slice(start, this.pos).join("").split("_").join(""));
      }
      if (this.pos < src.length && (isIdStart(src[this.pos]) || inSet(DIGITS, src[this.pos]))) this.error("Invalid number");
      return value;
    };

    function hexDigits(text) {
      for (var i = 0; i < text.length; i++) {
        if (!inSet(HEX, text.charAt(i))) return false;
      }
      return true;
    }

    Lexer.prototype.escape = function () {
      var src = this.src;
      this.pos += 1;
      if (this.pos >= src.length) this.error("Unterminated string");
      var ch = src[this.pos];
      this.pos += 1;
      var simple = { n: "\n", t: "\t", r: "\r", b: "\b", f: "\f", v: "\u000b" };
      if (hasKey(simple, ch)) return simple[ch];
      if (ch === "0" && !inSet(DIGITS, src[this.pos])) return "\u0000";
      var digits;
      if (ch === "x") {
        digits = src.slice(this.pos, this.pos + 2).join("");
        if (digits.length !== 2 || !hexDigits(digits)) this.error("Invalid escape");
        this.pos += 2;
        return String.fromCharCode(parseInt(digits, 16));
      }
      if (ch === "u") {
        if (src[this.pos] === "{") {
          var end = src.indexOf("}", this.pos);
          digits = end > 0 ? src.slice(this.pos + 1, end).join("") : "";
          if (!digits || !hexDigits(digits) || parseInt(digits, 16) > 0x10ffff) this.error("Invalid escape");
          this.pos = end + 1;
          var code = parseInt(digits, 16);
          return code >= 0xd800 && code <= 0xdfff ? String.fromCharCode(code) : String.fromCodePoint(code);
        }
        digits = src.slice(this.pos, this.pos + 4).join("");
        if (digits.length !== 4 || !hexDigits(digits)) this.error("Invalid escape");
        this.pos += 4;
        return String.fromCharCode(parseInt(digits, 16));
      }
      if (inSet(LINE_ENDS, ch)) {
        if (ch === "\r" && src[this.pos] === "\n") this.pos += 1;
        this.line += 1;
        return "";
      }
      return ch;
    };

    Lexer.prototype.string = function (quote) {
      var src = this.src;
      this.pos += 1;
      var out = [];
      while (true) {
        if (this.pos >= src.length || src[this.pos] === "\n" || src[this.pos] === "\r") this.error("Unterminated string");
        var ch = src[this.pos];
        if (ch === quote) {
          this.pos += 1;
          return out.join("");
        }
        if (ch === "\\") {
          out.push(this.escape());
        } else {
          out.push(ch);
          this.pos += 1;
        }
      }
    };

    Lexer.prototype.template = function () {
      var src = this.src;
      this.pos += 1;
      var strings = [];
      var parts = [];
      var out = [];
      while (true) {
        if (this.pos >= src.length) this.error("Unterminated template");
        var ch = src[this.pos];
        if (ch === "`") {
          this.pos += 1;
          strings.push(out.join(""));
          return [strings, parts];
        }
        if (ch === "\\") {
          out.push(this.escape());
          continue;
        }
        if (ch === "$" && src[this.pos + 1] === "{") {
          strings.push(out.join(""));
          out = [];
          this.pos += 2;
          var inner = new Lexer(src, this.line);
          inner.pos = this.pos;
          var depth = 0;
          var newline = false;
          while (true) {
            newline = inner.skipSpace(newline);
            if (inner.pos >= src.length) this.error("Unterminated template");
            if (src[inner.pos] === "}" && depth === 0) break;
            inner.stepOne(newline);
            newline = false;
            var token = inner.tokens[inner.tokens.length - 1];
            if (token.kind === "punct" && token.value === "{") depth += 1;
            else if (token.kind === "punct" && token.value === "}") depth -= 1;
          }
          inner.tokens.push(new Token("eof", null, inner.line, true));
          parts.push(inner.tokens);
          this.pos = inner.pos + 1;
          this.line = inner.line;
          continue;
        }
        if (inSet(LINE_ENDS, ch)) {
          if (!(ch === "\r" && src[this.pos + 1] === "\n")) this.line += 1;
          if (ch === "\r") {
            if (src[this.pos + 1] === "\n") this.pos += 1;
            ch = "\n";
          }
        }
        out.push(ch);
        this.pos += 1;
      }
    };

    Lexer.prototype.stepOne = function (newline) {
      var src = this.src;
      var ch = src[this.pos];
      var line = this.line;
      var token = null;
      if (inSet(DIGITS, ch) || (ch === "." && inSet(DIGITS, src[this.pos + 1]))) {
        token = new Token("num", this.number(), line, newline);
      } else if (ch === "\"" || ch === "'") {
        token = new Token("str", this.string(ch), line, newline);
      } else if (ch === "`") {
        var template = this.template();
        token = new Token("tmpl", template[0], line, newline, template[1]);
      } else if (isIdStart(ch)) {
        var name = this.identifier();
        if (inSet(UNSUPPORTED, name)) this.error(name + " is not supported in scripts");
        token = new Token(inSet(KEYWORDS, name) ? "kw" : "ident", name, line, newline);
      } else if (ch === "/" && this.regexAllowed()) {
        var regex = this.regex();
        token = new Token("regex", regex[0], line, newline, regex[1]);
      } else {
        for (var i = 0; i < PUNCTUATORS.length; i++) {
          var punct = PUNCTUATORS[i];
          if (startsAt(src, this.pos, punct)) {
            if (punct === "?." && inSet(DIGITS, src[this.pos + 2])) continue;
            this.pos += punct.length;
            token = new Token("punct", punct, line, newline);
            break;
          }
        }
        if (!token) this.error("Unexpected character " + ch);
      }
      this.tokens.push(token);
    };

    Lexer.prototype.identifier = function () {
      var src = this.src;
      var start = this.pos;
      while (this.pos < src.length && isIdPart(src[this.pos])) this.pos += 1;
      return src.slice(start, this.pos).join("");
    };

    Lexer.prototype.regex = function () {
      var src = this.src;
      this.pos += 1;
      var start = this.pos;
      var inClass = false;
      while (true) {
        if (this.pos >= src.length || inSet(LINE_ENDS, src[this.pos])) this.error("Unterminated regular expression");
        var ch = src[this.pos];
        if (ch === "\\") {
          this.pos += 2;
          continue;
        }
        if (ch === "[") inClass = true;
        else if (ch === "]") inClass = false;
        else if (ch === "/" && !inClass) break;
        this.pos += 1;
      }
      var source = src.slice(start, this.pos).join("");
      this.pos += 1;
      var flagStart = this.pos;
      while (this.pos < src.length && isIdPart(src[this.pos])) this.pos += 1;
      return [source, src.slice(flagStart, this.pos).join("")];
    };

    function lexAll(source) {
      var lexer = new Lexer(Array.from(source), 1);
      var newline = false;
      while (true) {
        newline = lexer.skipSpace(newline);
        if (lexer.pos >= lexer.src.length) {
          lexer.tokens.push(new Token("eof", null, lexer.line, true));
          return lexer.tokens;
        }
        lexer.stepOne(newline);
        newline = false;
      }
    }

    function Parser(tokens) {
      this.tokens = tokens;
      this.index = 0;
      this.nesting = 0;
      this.loops = 0;
      this.switches = 0;
      this.shorthand = new Map();
    }

    Parser.prototype.tok = function () {
      return this.tokens[this.index];
    };

    Parser.prototype.peek = function () {
      return this.tokens[Math.min(this.index + 1, this.tokens.length - 1)];
    };

    Parser.prototype.error = function (token) {
      token = token || this.tok();
      if (token.kind === "eof") throw new ScriptError(token.line, "Unexpected end of script");
      var text;
      if (token.kind === "str") text = "string";
      else if (token.kind === "num") text = "number";
      else if (token.kind === "tmpl") text = "template";
      else if (token.kind === "regex") text = "regular expression";
      else text = String(token.value);
      throw new ScriptError(token.line, "Unexpected token " + text);
    };

    Parser.prototype.isPunct = function (value, token) {
      token = token || this.tok();
      return token.kind === "punct" && token.value === value;
    };

    Parser.prototype.isKw = function (value) {
      var token = this.tok();
      return token.kind === "kw" && token.value === value;
    };

    Parser.prototype.advance = function () {
      var token = this.tokens[this.index];
      if (this.index < this.tokens.length - 1) this.index += 1;
      return token;
    };

    Parser.prototype.expect = function (value) {
      if (!this.isPunct(value)) this.error();
      return this.advance();
    };

    Parser.prototype.eat = function (value) {
      if (this.isPunct(value)) {
        this.advance();
        return true;
      }
      return false;
    };

    Parser.prototype.semicolon = function () {
      if (this.eat(";")) return;
      if (this.isPunct("}") || this.tok().kind === "eof" || this.tok().newline) return;
      this.error();
    };

    Parser.prototype.enter = function () {
      this.nesting += 1;
      if (this.nesting > MAX_NESTING) throw new ScriptError(this.tok().line, "The script nests too deeply");
    };

    Parser.prototype.leave = function () {
      this.nesting -= 1;
    };

    Parser.prototype.program = function () {
      var body = [];
      while (this.tok().kind !== "eof") body.push(this.statement());
      return body;
    };

    Parser.prototype.statement = function () {
      this.enter();
      try {
        return this.statementInner();
      } finally {
        this.leave();
      }
    };

    Parser.prototype.statementInner = function () {
      var tok = this.tok();
      var line = tok.line;
      if (tok.kind === "punct") {
        if (tok.value === "{") return ["block", line, this.block()];
        if (tok.value === ";") {
          this.advance();
          return ["empty", line];
        }
      }
      if (tok.kind === "kw") {
        var word = tok.value;
        if (word === "var" || word === "let" || word === "const") {
          var node = this.declaration(false);
          this.semicolon();
          return node;
        }
        if (word === "function") {
          this.advance();
          var fn = this.functionRest(line, true);
          return ["funcdecl", line, fn[2], fn];
        }
        if (word === "if") {
          this.advance();
          this.expect("(");
          var test = this.expression(false);
          this.expect(")");
          var then = this.statement();
          var other = null;
          if (this.isKw("else")) {
            this.advance();
            other = this.statement();
          }
          return ["if", line, test, then, other];
        }
        if (word === "for") return this.forStatement();
        if (word === "while") {
          this.advance();
          this.expect("(");
          var condition = this.expression(false);
          this.expect(")");
          return ["while", line, condition, this.loopBody()];
        }
        if (word === "do") {
          this.advance();
          var body = this.loopBody();
          if (!this.isKw("while")) this.error();
          this.advance();
          this.expect("(");
          var until = this.expression(false);
          this.expect(")");
          this.eat(";");
          return ["dowhile", line, body, until];
        }
        if (word === "return") {
          this.advance();
          var value = null;
          if (!(this.isPunct(";") || this.isPunct("}") || this.tok().kind === "eof" || this.tok().newline)) {
            value = this.expression(false);
          }
          this.semicolon();
          return ["return", line, value];
        }
        if (word === "break" || word === "continue") {
          this.advance();
          if (word === "break" && !(this.loops || this.switches)) throw new ScriptError(line, "Illegal break statement");
          if (word === "continue" && !this.loops) throw new ScriptError(line, "Illegal continue statement");
          this.semicolon();
          return [word, line];
        }
        if (word === "throw") {
          this.advance();
          if (this.tok().newline) throw new ScriptError(line, "Illegal newline after throw");
          var thrown = this.expression(false);
          this.semicolon();
          return ["throw", line, thrown];
        }
        if (word === "try") return this.tryStatement();
        if (word === "switch") return this.switchStatement();
      }
      var expr = this.expression(false);
      this.semicolon();
      return ["expr", line, expr];
    };

    Parser.prototype.loopBody = function () {
      this.loops += 1;
      try {
        return this.statement();
      } finally {
        this.loops -= 1;
      }
    };

    Parser.prototype.block = function () {
      this.expect("{");
      var body = [];
      while (!this.isPunct("}")) {
        if (this.tok().kind === "eof") this.error();
        body.push(this.statement());
      }
      this.advance();
      return body;
    };

    Parser.prototype.isOf = function () {
      var tok = this.tok();
      return tok.kind === "ident" && tok.value === "of";
    };

    Parser.prototype.declaration = function (inFor) {
      var line = this.tok().line;
      var kind = this.advance().value;
      var decls = [];
      while (true) {
        var target = this.bindingTarget();
        var init = null;
        if (this.eat("=")) {
          init = this.assignment(inFor);
        } else if (kind === "const" && !(inFor && (this.isKw("in") || this.isOf()))) {
          throw new ScriptError(this.tok().line, "Missing initializer in const declaration");
        } else if (target[0] !== "pid" && !(inFor && (this.isKw("in") || this.isOf()))) {
          throw new ScriptError(this.tok().line, "Missing initializer in destructuring declaration");
        }
        decls.push([target, init]);
        if (!this.eat(",")) break;
      }
      return ["var", line, kind, decls];
    };

    Parser.prototype.forStatement = function () {
      var line = this.advance().line;
      this.expect("(");
      var init = null;
      var kind;
      var subject;
      var body;
      var tok = this.tok();
      if (tok.kind === "kw" && (tok.value === "var" || tok.value === "let" || tok.value === "const")) {
        var decl = this.declaration(true);
        if (decl[3].length === 1 && decl[3][0][1] === null && (this.isOf() || this.isKw("in"))) {
          kind = this.isOf() ? "forof" : "forin";
          this.advance();
          subject = kind === "forof" ? this.assignment(false) : this.expression(false);
          this.expect(")");
          body = this.loopBody();
          return [kind, line, decl[2], decl[3][0][0], subject, body];
        }
        for (var i = 0; i < decl[3].length; i++) {
          if (decl[3][i][1] === null && (decl[2] === "const" || decl[3][i][0][0] !== "pid")) {
            throw new ScriptError(line, "Missing initializer in declaration");
          }
        }
        init = decl;
      } else if (!this.isPunct(";")) {
        var expr = this.expression(true);
        if (this.isOf() || this.isKw("in")) {
          kind = this.isOf() ? "forof" : "forin";
          this.advance();
          var target = this.toPattern(expr);
          subject = kind === "forof" ? this.assignment(false) : this.expression(false);
          this.expect(")");
          body = this.loopBody();
          return [kind, line, null, target, subject, body];
        }
        init = ["expr", line, expr];
      }
      this.expect(";");
      var test = this.isPunct(";") ? null : this.expression(false);
      this.expect(";");
      var update = this.isPunct(")") ? null : this.expression(false);
      this.expect(")");
      body = this.loopBody();
      return ["for", line, init, test, update, body];
    };

    Parser.prototype.tryStatement = function () {
      var line = this.advance().line;
      var block = this.block();
      var param = null;
      var handler = null;
      var final = null;
      if (this.isKw("catch")) {
        this.advance();
        if (this.eat("(")) {
          param = this.bindingTarget();
          this.expect(")");
        }
        handler = this.block();
      }
      if (this.isKw("finally")) {
        this.advance();
        final = this.block();
      }
      if (handler === null && final === null) throw new ScriptError(line, "Missing catch or finally after try");
      return ["try", line, block, param, handler, final];
    };

    Parser.prototype.switchStatement = function () {
      var line = this.advance().line;
      this.expect("(");
      var subject = this.expression(false);
      this.expect(")");
      this.expect("{");
      var cases = [];
      var seenDefault = false;
      this.switches += 1;
      try {
        while (!this.isPunct("}")) {
          var test;
          if (this.isKw("case")) {
            this.advance();
            test = this.expression(false);
          } else if (this.isKw("default")) {
            if (seenDefault) throw new ScriptError(this.tok().line, "More than one default clause in switch");
            seenDefault = true;
            this.advance();
            test = null;
          } else {
            this.error();
          }
          this.expect(":");
          var body = [];
          while (!(this.isKw("case") || this.isKw("default") || this.isPunct("}"))) {
            if (this.tok().kind === "eof") this.error();
            body.push(this.statement());
          }
          cases.push([test, body]);
        }
      } finally {
        this.switches -= 1;
      }
      this.advance();
      return ["switch", line, subject, cases];
    };

    Parser.prototype.bindingTarget = function () {
      var tok = this.tok();
      var rest = null;
      var target;
      var defaultValue;
      if (tok.kind === "ident") {
        this.advance();
        return ["pid", tok.line, tok.value];
      }
      if (this.isPunct("[")) {
        this.advance();
        var elements = [];
        while (!this.isPunct("]")) {
          if (this.isPunct(",")) {
            this.advance();
            elements.push(null);
            continue;
          }
          if (this.eat("...")) {
            rest = this.bindingTarget();
            break;
          }
          target = this.bindingTarget();
          defaultValue = this.eat("=") ? this.assignment(false) : null;
          elements.push([target, defaultValue]);
          if (!this.isPunct("]")) this.expect(",");
        }
        this.expect("]");
        return ["parr", tok.line, elements, rest];
      }
      if (this.isPunct("{")) {
        this.advance();
        var props = [];
        while (!this.isPunct("}")) {
          if (this.eat("...")) {
            rest = this.bindingTarget();
            break;
          }
          var keyTok = this.tok();
          var key;
          if (keyTok.kind === "ident" || keyTok.kind === "kw" || keyTok.kind === "str") key = keyTok.value;
          else if (keyTok.kind === "num") key = String(keyTok.value);
          else this.error();
          this.advance();
          if (this.eat(":")) {
            target = this.bindingTarget();
          } else {
            if (keyTok.kind !== "ident") this.error(keyTok);
            target = ["pid", keyTok.line, key];
          }
          defaultValue = this.eat("=") ? this.assignment(false) : null;
          props.push([key, target, defaultValue]);
          if (!this.isPunct("}")) this.expect(",");
        }
        this.expect("}");
        return ["pobj", tok.line, props, rest];
      }
      this.error();
    };

    Parser.prototype.toPattern = function (expr) {
      var kind = expr[0];
      var rest = null;
      if (kind === "pid" || kind === "parr" || kind === "pobj" || kind === "pexpr") return expr;
      if (kind === "ident") return ["pid", expr[1], expr[2]];
      if (kind === "member" || kind === "index") {
        if (expr[expr.length - 1]) throw new ScriptError(expr[1], "Invalid assignment target");
        return ["pexpr", expr[1], expr];
      }
      if (kind === "array") {
        var elements = [];
        for (var i = 0; i < expr[2].length; i++) {
          var element = expr[2][i];
          if (element === null) {
            elements.push(null);
          } else if (element[0] === "spread") {
            if (i !== expr[2].length - 1) throw new ScriptError(expr[1], "Invalid assignment target");
            rest = this.toPattern(element[2]);
          } else if (element[0] === "assign" && element[2] === "=") {
            elements.push([this.toPattern(element[3]), element[4]]);
          } else {
            elements.push([this.toPattern(element), null]);
          }
        }
        return ["parr", expr[1], elements, rest];
      }
      if (kind === "object") {
        this.shorthand.delete(expr);
        var props = [];
        for (var j = 0; j < expr[2].length; j++) {
          var prop = expr[2][j];
          if (prop[0] === "spread") {
            rest = this.toPattern(prop[1]);
            continue;
          }
          var key = prop[1];
          var value = prop[2];
          if (prop[3]) throw new ScriptError(expr[1], "Invalid assignment target");
          if (prop[4] !== null) props.push([key, ["pid", expr[1], key], prop[4]]);
          else if (value[0] === "assign" && value[2] === "=") props.push([key, this.toPattern(value[3]), value[4]]);
          else props.push([key, this.toPattern(value), null]);
        }
        return ["pobj", expr[1], props, rest];
      }
      throw new ScriptError(expr[1], "Invalid assignment target");
    };

    Parser.prototype.expression = function (noIn) {
      var line = this.tok().line;
      var expr = this.assignment(noIn);
      if (this.isPunct(",")) {
        var items = [expr];
        while (this.eat(",")) items.push(this.assignment(noIn));
        return ["seq", line, items];
      }
      return expr;
    };

    Parser.prototype.arrowAhead = function () {
      if (this.tok().kind === "ident" && this.isPunct("=>", this.peek())) return true;
      if (!this.isPunct("(")) return false;
      var depth = 0;
      for (var index = this.index; index < this.tokens.length; index++) {
        var token = this.tokens[index];
        if (token.kind === "eof") return false;
        if (token.kind === "punct") {
          if (token.value === "(" || token.value === "[" || token.value === "{") {
            depth += 1;
          } else if (token.value === ")" || token.value === "]" || token.value === "}") {
            depth -= 1;
            if (depth === 0) {
              var following = this.tokens[Math.min(index + 1, this.tokens.length - 1)];
              return following.kind === "punct" && following.value === "=>" && !following.newline;
            }
          }
        }
      }
      return false;
    };

    Parser.prototype.assignment = function (noIn) {
      this.enter();
      try {
        return this.assignmentInner(noIn);
      } finally {
        this.leave();
      }
    };

    Parser.prototype.assignmentInner = function (noIn) {
      if (this.arrowAhead()) return this.arrow();
      var line = this.tok().line;
      var left = this.conditional(noIn);
      var tok = this.tok();
      if (tok.kind === "punct" && inSet(ASSIGN, tok.value)) {
        var op = tok.value;
        var target;
        if (op === "=") {
          target = this.toPattern(left);
        } else if ((left[0] === "ident" || left[0] === "member" || left[0] === "index") &&
            !(left[0] !== "ident" && left[left.length - 1])) {
          target = left;
        } else {
          throw new ScriptError(line, "Invalid assignment target");
        }
        this.advance();
        var value = this.assignment(noIn);
        return ["assign", line, op, target, value];
      }
      return left;
    };

    Parser.prototype.arrow = function () {
      var line = this.tok().line;
      var params;
      var rest = null;
      if (this.tok().kind === "ident") {
        params = [[["pid", line, this.advance().value], null]];
      } else {
        var parsed = this.params();
        params = parsed[0];
        rest = parsed[1];
      }
      this.expect("=>");
      return this.functionBody(line, "", params, rest, true);
    };

    Parser.prototype.params = function () {
      this.expect("(");
      var params = [];
      var rest = null;
      while (!this.isPunct(")")) {
        if (this.eat("...")) {
          rest = this.bindingTarget();
          break;
        }
        var target = this.bindingTarget();
        var defaultValue = this.eat("=") ? this.assignment(false) : null;
        params.push([target, defaultValue]);
        if (!this.isPunct(")")) this.expect(",");
      }
      this.expect(")");
      return [params, rest];
    };

    Parser.prototype.functionBody = function (line, name, params, rest, arrow) {
      var saved = [this.loops, this.switches];
      this.loops = 0;
      this.switches = 0;
      try {
        if (arrow && !this.isPunct("{")) {
          var expr = this.assignment(false);
          return ["func", line, name, params, rest, expr, true, true];
        }
        var body = this.block();
        return ["func", line, name, params, rest, body, arrow, false];
      } finally {
        this.loops = saved[0];
        this.switches = saved[1];
      }
    };

    Parser.prototype.functionRest = function (line, declaration) {
      var name = "";
      if (this.tok().kind === "ident") name = this.advance().value;
      else if (declaration) this.error();
      var parsed = this.params();
      return this.functionBody(line, name, parsed[0], parsed[1], false);
    };

    Parser.prototype.conditional = function (noIn) {
      var line = this.tok().line;
      var test = this.binary(0, noIn);
      if (this.eat("?")) {
        var then = this.assignment(false);
        this.expect(":");
        var other = this.assignment(noIn);
        return ["cond", line, test, then, other];
      }
      return test;
    };

    Parser.prototype.binary = function (minPrec, noIn) {
      var left = this.unary();
      while (true) {
        var tok = this.tok();
        var op;
        if (tok.kind === "punct" || (tok.kind === "kw" && (tok.value === "in" || tok.value === "instanceof"))) op = tok.value;
        else break;
        if (op === "in" && noIn) break;
        var prec = hasKey(BINARY, op) ? BINARY[op] : null;
        if (prec === null || prec <= minPrec) break;
        var line = tok.line;
        this.advance();
        this.enter();
        var right;
        try {
          right = this.binary(op === "**" ? prec - 1 : prec, noIn);
        } finally {
          this.leave();
        }
        if (op === "&&" || op === "||" || op === "??") left = ["logic", line, op, left, right];
        else left = ["bin", line, op, left, right];
      }
      return left;
    };

    function updatable(node) {
      return (node[0] === "ident" || node[0] === "member" || node[0] === "index") && !(node[0] !== "ident" && node[node.length - 1]);
    }

    Parser.prototype.unary = function () {
      var tok = this.tok();
      var line = tok.line;
      if (tok.kind === "punct" && ["!", "-", "+", "~"].indexOf(tok.value) >= 0) {
        this.advance();
        this.enter();
        try {
          return ["unary", line, tok.value, this.unary()];
        } finally {
          this.leave();
        }
      }
      if (tok.kind === "punct" && (tok.value === "++" || tok.value === "--")) {
        this.advance();
        var target = this.unary();
        if (!updatable(target)) throw new ScriptError(line, "Invalid update target");
        return ["update", line, tok.value, true, target];
      }
      if (tok.kind === "kw" && (tok.value === "typeof" || tok.value === "delete" || tok.value === "void")) {
        this.advance();
        this.enter();
        try {
          return ["unary", line, tok.value, this.unary()];
        } finally {
          this.leave();
        }
      }
      var expr = this.postfix();
      tok = this.tok();
      if (tok.kind === "punct" && (tok.value === "++" || tok.value === "--") && !tok.newline) {
        if (!updatable(expr)) throw new ScriptError(line, "Invalid update target");
        this.advance();
        return ["update", line, tok.value, false, expr];
      }
      return expr;
    };

    Parser.prototype.postfix = function () {
      var line = this.tok().line;
      var expr = this.isKw("new") ? this.newExpression() : this.primary();
      var optional = false;
      while (true) {
        var tok = this.tok();
        if (tok.kind !== "punct") break;
        if (tok.value === ".") {
          this.advance();
          expr = ["member", tok.line, expr, this.propertyName(), false];
        } else if (tok.value === "?.") {
          optional = true;
          this.advance();
          if (this.isPunct("(")) {
            expr = ["call", tok.line, expr, this.arguments(), true];
          } else if (this.eat("[")) {
            var key = this.expression(false);
            this.expect("]");
            expr = ["index", tok.line, expr, key, true];
          } else {
            expr = ["member", tok.line, expr, this.propertyName(), true];
          }
        } else if (tok.value === "[") {
          this.advance();
          var index = this.expression(false);
          this.expect("]");
          expr = ["index", tok.line, expr, index, false];
        } else if (tok.value === "(") {
          expr = ["call", tok.line, expr, this.arguments(), false];
        } else {
          break;
        }
      }
      return optional ? ["chain", line, expr] : expr;
    };

    Parser.prototype.propertyName = function () {
      var tok = this.tok();
      if (tok.kind !== "ident" && tok.kind !== "kw") this.error();
      this.advance();
      return tok.value;
    };

    Parser.prototype.newExpression = function () {
      var line = this.advance().line;
      var callee = this.isKw("new") ? this.newExpression() : this.primary();
      while (true) {
        if (this.isPunct(".")) {
          this.advance();
          callee = ["member", line, callee, this.propertyName(), false];
        } else if (this.isPunct("[")) {
          this.advance();
          var key = this.expression(false);
          this.expect("]");
          callee = ["index", line, callee, key, false];
        } else {
          break;
        }
      }
      var args = this.isPunct("(") ? this.arguments() : [];
      return ["new", line, callee, args];
    };

    Parser.prototype.arguments = function () {
      this.expect("(");
      var args = [];
      while (!this.isPunct(")")) {
        if (this.eat("...")) args.push(["spread", this.tok().line, this.assignment(false)]);
        else args.push(this.assignment(false));
        if (!this.isPunct(")")) this.expect(",");
      }
      this.expect(")");
      return args;
    };

    Parser.prototype.primary = function () {
      var tok = this.tok();
      var line = tok.line;
      var kind = tok.kind;
      if (kind === "num" || kind === "str") {
        this.advance();
        return ["lit", line, tok.value];
      }
      if (kind === "tmpl") {
        this.advance();
        var parts = [];
        for (var i = 0; i < tok.extra.length; i++) {
          var sub = new Parser(tok.extra[i]);
          sub.nesting = this.nesting;
          sub.loops = this.loops;
          sub.switches = this.switches;
          sub.shorthand = this.shorthand;
          var expr = sub.expression(false);
          if (sub.tok().kind !== "eof") sub.error();
          parts.push(expr);
        }
        return ["tmpl", line, tok.value, parts];
      }
      if (kind === "regex") {
        this.advance();
        return ["regex", line, tok.value, tok.extra, compileRegex(tok.value, tok.extra, line)];
      }
      if (kind === "ident") {
        this.advance();
        return ["ident", line, tok.value];
      }
      if (kind === "kw") {
        var word = tok.value;
        if (word === "true" || word === "false") {
          this.advance();
          return ["lit", line, word === "true"];
        }
        if (word === "null") {
          this.advance();
          return ["lit", line, null];
        }
        if (word === "this") {
          this.advance();
          return ["this", line];
        }
        if (word === "function") {
          this.advance();
          return this.functionRest(line, false);
        }
      }
      if (kind === "punct") {
        if (tok.value === "(") {
          this.advance();
          var inner = this.expression(false);
          this.expect(")");
          return inner;
        }
        if (tok.value === "[") return this.arrayLiteral();
        if (tok.value === "{") return this.objectLiteral();
      }
      this.error();
    };

    Parser.prototype.arrayLiteral = function () {
      var line = this.advance().line;
      var elements = [];
      while (!this.isPunct("]")) {
        if (this.isPunct(",")) {
          this.advance();
          elements.push(null);
          continue;
        }
        if (this.eat("...")) elements.push(["spread", this.tok().line, this.assignment(false)]);
        else elements.push(this.assignment(false));
        if (!this.isPunct("]")) this.expect(",");
      }
      this.advance();
      return ["array", line, elements];
    };

    Parser.prototype.objectLiteral = function () {
      var line = this.advance().line;
      var props = [];
      var shorthand = false;
      while (!this.isPunct("}")) {
        if (this.eat("...")) {
          props.push(["spread", this.assignment(false)]);
        } else {
          var keyTok = this.tok();
          var computed = false;
          var key;
          if (keyTok.kind === "ident" || keyTok.kind === "kw" || keyTok.kind === "str") {
            key = keyTok.value;
            this.advance();
          } else if (keyTok.kind === "num") {
            key = String(keyTok.value);
            this.advance();
          } else if (this.isPunct("[")) {
            this.advance();
            key = this.assignment(false);
            this.expect("]");
            computed = true;
          } else {
            this.error();
          }
          if (this.eat(":")) {
            props.push(["prop", key, this.assignment(false), computed, null]);
          } else if (this.isPunct("(")) {
            var parsed = this.params();
            props.push(["prop", key, this.functionBody(keyTok.line, computed ? "" : key, parsed[0], parsed[1], false),
              computed, null]);
          } else if (keyTok.kind === "ident" && !computed) {
            var defaultValue = null;
            if (this.isPunct("=")) {
              this.advance();
              defaultValue = this.assignment(false);
              shorthand = true;
            }
            props.push(["prop", key, ["ident", keyTok.line, key], false, defaultValue]);
          } else {
            this.error();
          }
        }
        if (!this.isPunct("}")) this.expect(",");
      }
      this.advance();
      var node = ["object", line, props];
      if (shorthand) this.shorthand.set(node, line);
      return node;
    };

    function parse(source) {
      var parser = new Parser(lexAll(String(source)));
      var program = parser.program();
      if (parser.shorthand.size) {
        var line = Infinity;
        parser.shorthand.forEach(function (value) { line = Math.min(line, value); });
        throw new ScriptError(line, "Invalid shorthand property initializer");
      }
      return program;
    }

    function escapeCodePoint(code) {
      if (code < 0x10000) return "\\u" + ("0000" + code.toString(16)).slice(-4);
      var high = Math.floor((code - 0x10000) / 0x400) + 0xd800;
      var low = ((code - 0x10000) % 0x400) + 0xdc00;
      return "\\u" + high.toString(16) + "\\u" + low.toString(16);
    }

    function translateRegex(source, line) {
      var chars = Array.from(source);
      var out = [];
      var names = [];
      var index = 0;
      var inClass = false;
      var length = chars.length;
      function bad() {
        throw new ScriptError(line, "Invalid regular expression /" + source + "/");
      }
      function find(ch, from) {
        for (var i = from; i < length; i++) {
          if (chars[i] === ch) return i;
        }
        return -1;
      }
      while (index < length) {
        var ch = chars[index];
        if (ch === "\\") {
          if (index + 1 >= length) bad();
          var next = chars[index + 1];
          index += 2;
          if ((next === "D" || next === "W" || next === "S") && inClass) bad();
          if (next === "k") {
            if (chars[index] !== "<") bad();
            var close = find(">", index);
            if (close < 0) bad();
            out.push("\\k" + chars.slice(index, close + 1).join(""));
            index = close + 1;
          } else if (next === "u" && chars[index] === "{") {
            var brace = find("}", index);
            var digits = brace < 0 ? "" : chars.slice(index + 1, brace).join("");
            if (!digits || !hexDigits(digits)) bad();
            out.push(escapeCodePoint(parseInt(digits, 16)));
            index = brace + 1;
          } else if (next === "u") {
            if (!hexDigits(chars.slice(index, index + 4).join("")) || chars.slice(index, index + 4).length !== 4) bad();
            out.push("\\u" + chars.slice(index, index + 4).join(""));
            index += 4;
          } else if (next === "x") {
            if (!hexDigits(chars.slice(index, index + 2).join("")) || chars.slice(index, index + 2).length !== 2) bad();
            out.push("\\x" + chars.slice(index, index + 2).join(""));
            index += 2;
          } else if (next === "U") {
            var long = chars.slice(index, index + 8).join("");
            if (long.length !== 8 || !hexDigits(long) || parseInt(long, 16) > 0x10ffff) bad();
            out.push(escapeCodePoint(parseInt(long, 16)));
            index += 8;
          } else if (next === "c") {
            if (index < length && /^[A-Za-z]$/.test(chars[index])) {
              out.push("\\c" + chars[index]);
              index += 1;
            } else {
              bad();
            }
          } else if (next === "a") {
            out.push("\\x07");
          } else if (next === "A" || next === "Z") {
            if (inClass) bad();
            out.push(next === "A" ? "(?<![\\s\\S])" : "(?![\\s\\S])");
          } else if (/^[A-Za-z]$/.test(next) && "dDwWsSbBnrtfv0".indexOf(next) < 0) {
            bad();
          } else {
            out.push("\\" + next);
          }
          continue;
        }
        if (inClass) {
          if (ch === "]") inClass = false;
          out.push(ch === "[" ? "\\[" : ch);
          index += 1;
          continue;
        }
        if (ch === "[") {
          if (chars[index + 1] === "^" && chars[index + 2] === "]") {
            out.push("[\\s\\S]");
            index += 3;
            continue;
          }
          if (chars[index + 1] === "]") {
            out.push("(?!)");
            index += 2;
            continue;
          }
          inClass = true;
          out.push("[");
          index += 1;
          if (chars[index] === "^") {
            out.push("^");
            index += 1;
          }
          continue;
        }
        if (ch === "(" && chars[index + 1] === "?" && chars[index + 2] === "<" && chars[index + 3] !== "=" &&
            chars[index + 3] !== "!") {
          var end = find(">", index);
          if (end < 0) bad();
          names.push(chars.slice(index + 3, end).join(""));
          out.push(chars.slice(index, end + 1).join(""));
          index = end + 1;
          continue;
        }
        out.push(ch);
        index += 1;
      }
      return { pattern: out.join(""), names: names };
    }

    function compileRegex(source, flags, line) {
      line = line || 0;
      var seen = {};
      for (var i = 0; i < flags.length; i++) {
        var flag = flags.charAt(i);
        if ("gimsuyd".indexOf(flag) < 0 || seen[flag]) throw new ScriptError(line, "Invalid regular expression flags");
        seen[flag] = true;
      }
      var translated = translateRegex(source, line);
      var native = (seen.i ? "i" : "") + (seen.m ? "m" : "") + (seen.s ? "s" : "");
      var modes = ["u", ""];
      for (var m = 0; m < modes.length; m++) {
        try {
          return {
            search: new RegExp(translated.pattern, "g" + modes[m] + native),
            sticky: new RegExp(translated.pattern, "y" + modes[m] + native),
            names: translated.names
          };
        } catch (error) {
          continue;
        }
      }
      throw new ScriptError(line, "Invalid regular expression /" + source + "/");
    }

    function wrapMatch(found, subject) {
      if (!found) return null;
      var u0 = found.index;
      var u1 = found.index + found[0].length;
      var groups = [];
      for (var i = 1; i < found.length; i++) groups.push(found[i]);
      return {
        u0: u0, u1: u1, start: toPoint(subject, u0), end: toPoint(subject, u1), text: found[0], groups: groups,
        named: found.groups || null
      };
    }

    function rxSearch(compiled, subject, position) {
      var re = compiled.search;
      re.lastIndex = toUnit(subject, position);
      return wrapMatch(re.exec(subject), subject);
    }

    function rxMatch(compiled, subject, position) {
      var re = compiled.sticky;
      re.lastIndex = toUnit(subject, position);
      return wrapMatch(re.exec(subject), subject);
    }

    function stringToNumber(text) {
      text = text.trim();
      if (!text) return 0;
      if (text === "Infinity" || text === "+Infinity") return Infinity;
      if (text === "-Infinity") return -Infinity;
      if (text.length > 2 && text.charAt(0) === "0" && "xXoObB".indexOf(text.charAt(1)) >= 0) {
        if (text.indexOf("_") >= 0) return NaN;
        var base = { x: 16, o: 8, b: 2 }[text.charAt(1).toLowerCase()];
        var body = text.slice(2).trim();
        var sign = 1;
        if (body.charAt(0) === "+" || body.charAt(0) === "-") {
          sign = body.charAt(0) === "-" ? -1 : 1;
          body = body.slice(1);
        }
        var allowed = "0123456789abcdef".slice(0, base);
        if (!body) return NaN;
        for (var i = 0; i < body.length; i++) {
          if (allowed.indexOf(body.charAt(i).toLowerCase()) < 0) return NaN;
        }
        return sign * Number(BigInt({ 16: "0x", 8: "0o", 2: "0b" }[base] + body));
      }
      if (DECIMAL_RE.test(text)) return Number(text);
      return NaN;
    }

    function dateFromParts(year, month, day, hours, minutes, seconds, ms) {
      var values = [year, month, day === undefined ? 1 : day, hours || 0, minutes || 0, seconds || 0, ms || 0];
      for (var i = 0; i < values.length; i++) {
        if (values[i] !== values[i] || values[i] === Infinity || values[i] === -Infinity) return NaN;
        values[i] = Math.trunc(values[i]);
      }
      var yearTotal = values[0] + floorDiv(values[1], 12);
      var monthIndex = pyMod(values[1], 12);
      var days = daysFromCivil(yearTotal, monthIndex + 1, 1) + values[2] - 1;
      return days * 86400000 + values[3] * 3600000 + values[4] * 60000 + values[5] * 1000 + values[6];
    }

    function daysFromCivil(year, month, day) {
      year -= month <= 2 ? 1 : 0;
      var era = floorDiv(year >= 0 ? year : year - 399, 400);
      var yoe = year - era * 400;
      var doy = floorDiv(153 * (month + (month > 2 ? -3 : 9)) + 2, 5) + day - 1;
      var doe = yoe * 365 + floorDiv(yoe, 4) - floorDiv(yoe, 100) + doy;
      return era * 146097 + doe - 719468;
    }

    function civilFromMs(ms) {
      ms = Math.trunc(ms);
      var days = floorDiv(ms, 86400000);
      var rem = ms - days * 86400000;
      var z = days + 719468;
      var era = floorDiv(z >= 0 ? z : z - 146096, 146097);
      var doe = z - era * 146097;
      var yoe = floorDiv(doe - floorDiv(doe, 1460) + floorDiv(doe, 36524) - floorDiv(doe, 146096), 365);
      var year = yoe + era * 400;
      var doy = doe - (365 * yoe + floorDiv(yoe, 4) - floorDiv(yoe, 100));
      var mp = floorDiv(5 * doy + 2, 153);
      var day = doy - floorDiv(153 * mp + 2, 5) + 1;
      var month = mp + (mp < 10 ? 3 : -9);
      if (month <= 2) year += 1;
      var weekday = pyMod(days + 4, 7);
      return [year, month, day, floorDiv(rem, 3600000), pyMod(floorDiv(rem, 60000), 60), pyMod(floorDiv(rem, 1000), 60),
        pyMod(rem, 1000), weekday];
    }

    function monthDays(year, month) {
      if (month === 2) return (year % 4 === 0 && year % 100 !== 0) || year % 400 === 0 ? 29 : 28;
      return [4, 6, 9, 11].indexOf(month) >= 0 ? 30 : 31;
    }

    function parseDateString(text) {
      text = text.trim();
      var match = ISO_DATE_RE.exec(text);
      if (!match) return NaN;
      if (match[1] === "-000000") return NaN;
      var year = parseInt(match[1], 10);
      var month = parseInt(match[2] || "1", 10);
      var day = parseInt(match[3] || "1", 10);
      var hours = parseInt(match[4] || "0", 10);
      var minutes = parseInt(match[5] || "0", 10);
      var seconds = parseInt(match[6] || "0", 10);
      var ms = parseInt(((match[7] || "0").slice(0, 3) + "000").slice(0, 3), 10);
      if (!(month >= 1 && month <= 12 && day >= 1 && day <= 31 && hours <= 24 && minutes <= 59 && seconds <= 59)) return NaN;
      if (hours === 24 && (minutes || seconds || ms)) return NaN;
      if (day > monthDays(year, month)) return NaN;
      var value = dateFromParts(year, month - 1, day, hours, minutes, seconds, ms);
      var offset = match[8];
      if (offset && offset !== "Z") {
        var sign = offset.charAt(0) === "+" ? 1 : -1;
        value -= sign * (parseInt(offset.slice(1, 3), 10) * 60 + parseInt(offset.slice(4, 6), 10)) * 60000;
      }
      return timeClip(value);
    }

    function isoString(ms) {
      var parts = civilFromMs(ms);
      var year = parts[0];
      var yearText = year >= 0 && year <= 9999 ? pad(year, 4) : (year > 0 ? "+" : "-") + pad(year, 6);
      return yearText + "-" + pad(parts[1], 2) + "-" + pad(parts[2], 2) + "T" + pad(parts[3], 2) + ":" + pad(parts[4], 2) +
        ":" + pad(parts[5], 2) + "." + pad(parts[6], 3) + "Z";
    }

    function dateToString(ms) {
      if (ms !== ms) return "Invalid Date";
      var parts = civilFromMs(ms);
      var year = parts[0];
      var yearText = year < 0 ? "-" + pad(-year, 6) : pad(year, 4);
      return DAY_NAMES[parts[7]] + " " + MONTH_NAMES[parts[1] - 1] + " " + pad(parts[2], 2) + " " + yearText + " " +
        pad(parts[3], 2) + ":" + pad(parts[4], 2) + ":" + pad(parts[5], 2) + " GMT+0000 (Coordinated Universal Time)";
    }

    function fromJson(value) {
      if (value === null || value === undefined) return null;
      if (typeof value === "boolean" || typeof value === "string" || typeof value === "number") return value;
      if (Array.isArray(value)) return value.map(fromJson);
      if (typeof value === "object") {
        var out = dict();
        Object.keys(value).forEach(function (key) { out[key] = fromJson(value[key]); });
        return out;
      }
      return String(value);
    }

    function setOwn(target, key, value) {
      if (key === "__proto__") Object.defineProperty(target, key, { value: value, enumerable: true, writable: true, configurable: true });
      else target[key] = value;
    }

    function toJson(value, seen) {
      if (value === undefined || value === null) return null;
      if (typeof value === "boolean" || typeof value === "string") return value;
      if (typeof value === "number") {
        if (value !== value || value === Infinity || value === -Infinity) return null;
        return value === 0 ? 0 : value;
      }
      if (value instanceof SDate) return value.ms !== value.ms ? null : isoString(value.ms);
      if (value instanceof SFunction || value instanceof SNative) return null;
      if (value instanceof SRegExp || value instanceof SCollection) return {};
      seen = seen || [];
      if (seen.indexOf(value) >= 0) throw new ScriptLimit("The script returned a value that contains itself");
      seen.push(value);
      try {
        if (isDict(value)) {
          var out = {};
          Object.keys(value).forEach(function (key) {
            var item = value[key];
            if (item === undefined || item instanceof SFunction || item instanceof SNative) return;
            setOwn(out, key, toJson(item, seen));
          });
          return out;
        }
        if (Array.isArray(value)) return value.map(function (item) { return toJson(item, seen); });
      } finally {
        seen.pop();
      }
      return null;
    }

    function Scope(parent) {
      this.vars = Object.create(null);
      this.consts = Object.create(null);
      this.parent = parent || null;
    }

    Scope.prototype.declare = function (name, value, kind) {
      if ((kind === "let" || kind === "const") && name in this.vars) {
        throw new Thrown(errorObject("SyntaxError", "Identifier '" + name + "' has already been declared"));
      }
      this.vars[name] = value;
      if (kind === "const") this.consts[name] = true;
      else delete this.consts[name];
    };

    Scope.prototype.lookup = function (name) {
      var scope = this;
      while (scope !== null) {
        if (name in scope.vars) return scope;
        scope = scope.parent;
      }
      return null;
    };

    function errorObject(name, message) {
      var out = dict();
      out.name = name;
      out.message = message;
      return out;
    }

    function typeError(message) {
      return new Thrown(errorObject("TypeError", message));
    }

    function rangeError(message) {
      return new Thrown(errorObject("RangeError", message));
    }

    function isCallable(value) {
      return value instanceof SFunction || value instanceof SNative;
    }

    function typeOf(value) {
      if (value === undefined) return "undefined";
      if (value === null) return "object";
      if (typeof value === "boolean" || typeof value === "number" || typeof value === "string") return typeof value;
      if (isCallable(value)) return "function";
      return "object";
    }

    function truthy(value) {
      return !!value;
    }

    function toNumber(value) {
      if (typeof value === "number") return value;
      if (typeof value === "boolean") return value ? 1 : 0;
      if (value === null) return 0;
      if (value === undefined) return NaN;
      if (typeof value === "string") return stringToNumber(value);
      if (value instanceof SDate) return value.ms;
      if (Array.isArray(value)) return stringToNumber(toString(value));
      return NaN;
    }

    function toInteger(value) {
      var number = toNumber(value);
      if (number !== number) return 0;
      if (number === Infinity || number === -Infinity) return number;
      return Math.trunc(number);
    }

    function toUint32(value) {
      return toNumber(value) >>> 0;
    }

    function toString(value, seen) {
      if (typeof value === "string") return value;
      if (typeof value === "number") return String(value);
      if (typeof value === "boolean") return value ? "true" : "false";
      if (value === null) return "null";
      if (value === undefined) return "undefined";
      if (Array.isArray(value)) {
        seen = seen || [];
        if (seen.indexOf(value) >= 0) return "";
        seen.push(value);
        try {
          return joinList(value, ",", seen);
        } finally {
          seen.splice(seen.indexOf(value), 1);
        }
      }
      if (value instanceof SDate) return dateToString(value.ms);
      if (value instanceof SRegExp) return "/" + value.source + "/" + value.flags;
      if (value instanceof SFunction) return "function " + value.name + "() { [code] }";
      if (value instanceof SNative) return "function " + value.name + "() { [native code] }";
      if (value instanceof SCollection) return "[object " + value.kind + "]";
      return "[object Object]";
    }

    function joinList(items, separator, seen) {
      var parts = [];
      var total = 0;
      var sepLength = slen(separator);
      for (var i = 0; i < items.length; i++) {
        var item = items[i];
        var text = item === null || item === undefined ? "" : toString(item, seen);
        total += slen(text) + sepLength;
        if (total > MAX_STRING + sepLength) throw rangeError("Invalid string length");
        parts.push(text);
      }
      return parts.join(separator);
    }

    function toPrimitive(value, hint) {
      if (value instanceof SDate) return hint === "number" ? value.ms : dateToString(value.ms);
      if (Array.isArray(value) || isDict(value) || isCallable(value) || value instanceof SRegExp || value instanceof SCollection) {
        return toString(value);
      }
      return value;
    }

    function propertyKey(value) {
      return typeof value === "string" ? value : toString(value);
    }

    function arrayIndex(key) {
      if (typeof key === "number") return Number.isInteger(key) && key >= 0 && key < 4294967295 ? key : null;
      if (typeof key === "string" && INDEX_RE.test(key) && Number(key) < 4294967295) return Number(key);
      return null;
    }

    function sameValueZero(a, b) {
      return a === b || (typeof a === "number" && typeof b === "number" && a !== a && b !== b);
    }

    function isObjectLike(value) {
      return Array.isArray(value) || isDict(value) || value instanceof SDate || value instanceof SRegExp || isCallable(value) ||
        value instanceof SCollection;
    }

    function looseEquals(a, b) {
      var aNull = a === null || a === undefined;
      var bNull = b === null || b === undefined;
      if (aNull || bNull) return aNull && bNull;
      if (typeOf(a) === typeOf(b)) return a === b;
      if (typeof a === "number" && typeof b === "string") return a === stringToNumber(b);
      if (typeof a === "string" && typeof b === "number") return stringToNumber(a) === b;
      if (typeof a === "boolean") return looseEquals(a ? 1 : 0, b);
      if (typeof b === "boolean") return looseEquals(a, b ? 1 : 0);
      if (isObjectLike(a) && !isObjectLike(b)) return looseEquals(toPrimitive(a), b);
      if (isObjectLike(b) && !isObjectLike(a)) return looseEquals(a, toPrimitive(b));
      return false;
    }

    function checkString(text) {
      if (text.length > MAX_STRING && slen(text) > MAX_STRING) throw rangeError("Invalid string length");
      return text;
    }

    function checkArray(items) {
      if (items.length > MAX_ARRAY) throw rangeError("Invalid array length");
      return items;
    }

    function relative(value, length, fallback) {
      if (value === undefined) return fallback;
      var number = toInteger(value);
      if (number === -Infinity) return 0;
      if (number === Infinity) return length;
      if (number < 0) return Math.max(length + number, 0);
      return Math.min(number, length);
    }

    function clampIndex(value, length) {
      var number = toInteger(value);
      if (number === Infinity) return length;
      if (number === -Infinity) return 0;
      return Math.max(0, Math.min(number, length));
    }

    function jsonQuote(text) {
      var out = "\"";
      var cps = Array.from(text);
      for (var i = 0; i < cps.length; i++) {
        var ch = cps[i];
        var code = ch.charCodeAt(0);
        if (ch === "\"") out += "\\\"";
        else if (ch === "\\") out += "\\\\";
        else if (ch === "\n") out += "\\n";
        else if (ch === "\r") out += "\\r";
        else if (ch === "\t") out += "\\t";
        else if (ch === "\b") out += "\\b";
        else if (ch === "\f") out += "\\f";
        else if (code < 0x20 || (ch.length === 1 && code >= 0xd800 && code <= 0xdfff)) out += "\\u" + ("0000" + code.toString(16)).slice(-4);
        else out += ch;
      }
      return out + "\"";
    }

    function jsonStringify(value, indent) {
      indent = indent || "";
      var seen = [];
      var budget = 0;
      function emit(text) {
        budget += slen(text);
        if (budget > MAX_STRING) throw rangeError("Invalid string length");
        return text;
      }
      function serialize(item, current) {
        if (item instanceof SDate) item = item.ms !== item.ms ? null : isoString(item.ms);
        if (item === null) return emit("null");
        if (item === true) return emit("true");
        if (item === false) return emit("false");
        if (typeof item === "string") return emit(jsonQuote(item));
        if (typeof item === "number") {
          if (item !== item || item === Infinity || item === -Infinity) return emit("null");
          return emit(String(item));
        }
        if (item === undefined || isCallable(item)) return null;
        if (item instanceof SRegExp || item instanceof SCollection) return emit("{}");
        if (seen.indexOf(item) >= 0) throw typeError("Converting circular structure to JSON");
        seen.push(item);
        var inner = current + indent;
        var parts = [];
        try {
          if (Array.isArray(item)) {
            for (var i = 0; i < item.length; i++) {
              var text = serialize(item[i], inner);
              parts.push(text === null ? "null" : text);
            }
            if (!parts.length) return "[]";
            if (indent) return "[\n" + inner + parts.join(",\n" + inner) + "\n" + current + "]";
            return "[" + parts.join(",") + "]";
          }
          var keys = Object.keys(item);
          for (var j = 0; j < keys.length; j++) {
            var entry = serialize(item[keys[j]], inner);
            if (entry === null) continue;
            parts.push(jsonQuote(keys[j]) + (indent ? ": " : ":") + entry);
          }
          if (!parts.length) return "{}";
          if (indent) return "{\n" + inner + parts.join(",\n" + inner) + "\n" + current + "}";
          return "{" + parts.join(",") + "}";
        } finally {
          seen.pop();
        }
      }
      var result = serialize(value, "");
      return result === null ? undefined : result;
    }

    function jsonParse(text) {
      try {
        return fromJson(JSON.parse(text));
      } catch (error) {
        throw new Thrown(errorObject("SyntaxError", "JSON.parse: the text is not valid JSON"));
      }
    }

    function uriEncode(text) {
      var out = "";
      var cps = Array.from(text);
      for (var i = 0; i < cps.length; i++) {
        var ch = cps[i];
        if (inSet(URI_UNRESERVED, ch)) {
          out += ch;
          continue;
        }
        if (ch.length === 1 && ch.charCodeAt(0) >= 0xd800 && ch.charCodeAt(0) <= 0xdfff) {
          throw new Thrown(errorObject("URIError", "URI malformed"));
        }
        utf8(ch).forEach(function (byte) { out += "%" + (byte < 16 ? "0" : "") + byte.toString(16).toUpperCase(); });
      }
      return out;
    }

    function uriByte(cps, index) {
      var pair = cps.slice(index + 1, index + 3).join("");
      if (cps[index] !== "%" || pair.length !== 2 || !hexDigits(pair)) throw new Thrown(errorObject("URIError", "URI malformed"));
      return parseInt(pair, 16);
    }

    function uriDecode(text) {
      var cps = Array.from(text);
      var out = "";
      var index = 0;
      while (index < cps.length) {
        var ch = cps[index];
        if (ch !== "%") {
          out += ch;
          index += 1;
          continue;
        }
        var first = uriByte(cps, index);
        index += 3;
        if (first < 0x80) {
          out += String.fromCharCode(first);
          continue;
        }
        var count;
        if ((first & 0xe0) === 0xc0) count = 1;
        else if ((first & 0xf0) === 0xe0) count = 2;
        else if ((first & 0xf8) === 0xf0) count = 3;
        else throw new Thrown(errorObject("URIError", "URI malformed"));
        var data = [first];
        for (var i = 0; i < count; i++) {
          var byte = uriByte(cps, index);
          if ((byte & 0xc0) !== 0x80) throw new Thrown(errorObject("URIError", "URI malformed"));
          data.push(byte);
          index += 3;
        }
        var decoded = utf8Strict(data);
        if (decoded === null) throw new Thrown(errorObject("URIError", "URI malformed"));
        out += decoded;
      }
      return out;
    }

    function expandReplacement(template, matched, unitPosition, subject, groups, named) {
      var cps = Array.from(template);
      var out = "";
      var index = 0;
      while (index < cps.length) {
        var ch = cps[index];
        if (ch !== "$" || index + 1 >= cps.length) {
          out += ch;
          index += 1;
          continue;
        }
        var next = cps[index + 1];
        var value;
        if (next === "$") {
          out += "$";
          index += 2;
        } else if (next === "&") {
          out += matched;
          index += 2;
        } else if (next === "`") {
          out += subject.slice(0, unitPosition);
          index += 2;
        } else if (next === "'") {
          out += subject.slice(unitPosition + matched.length);
          index += 2;
        } else if (inSet(DIGITS, next)) {
          var two = cps.slice(index + 1, index + 3).join("");
          if (two.length === 2 && inSet(DIGITS, two.charAt(1)) && parseInt(two, 10) >= 1 && parseInt(two, 10) <= groups.length) {
            value = groups[parseInt(two, 10) - 1];
            out += value === undefined ? "" : value;
            index += 3;
          } else if (parseInt(next, 10) >= 1 && parseInt(next, 10) <= groups.length) {
            value = groups[parseInt(next, 10) - 1];
            out += value === undefined ? "" : value;
            index += 2;
          } else {
            out += "$" + next;
            index += 2;
          }
        } else if (next === "<" && named !== null && named !== undefined) {
          var end = -1;
          for (var i = index; i < cps.length; i++) {
            if (cps[i] === ">") {
              end = i;
              break;
            }
          }
          if (end < 0) {
            out += "$<";
            index += 2;
          } else {
            var name = cps.slice(index + 2, end).join("");
            value = isDict(named) && hasKey(named, name) ? named[name] : undefined;
            out += value === undefined ? "" : toString(value);
            index = end + 1;
          }
        } else {
          out += "$";
          index += 1;
        }
      }
      return out;
    }

    function namedGroups(regex, match) {
      if (!regex.names.length) return null;
      var out = dict();
      regex.names.forEach(function (name) {
        var value = match.named && hasKey(match.named, name) ? match.named[name] : undefined;
        out[name] = value === undefined ? undefined : value;
      });
      return out;
    }

    function matchArray(regex, match, subject) {
      var out = [match.text].concat(match.groups);
      Object.defineProperty(out, "$match", {
        value: { index: match.start, input: subject, groups: regex.names.length ? namedGroups(regex, match) : undefined },
        enumerable: false
      });
      return out;
    }

    function regexExec(regex, subject) {
      var sticky = regex.flags.indexOf("y") >= 0;
      var useLast = regex.flags.indexOf("g") >= 0 || sticky;
      var start = useLast ? regex.lastIndex : 0;
      if (start > slen(subject)) {
        regex.lastIndex = 0;
        return null;
      }
      var match = sticky ? rxMatch(regex.compiled, subject, start) : rxSearch(regex.compiled, subject, start);
      if (match === null) {
        if (useLast) regex.lastIndex = 0;
        return null;
      }
      if (useLast) regex.lastIndex = match.end;
      return matchArray(regex, match, subject);
    }

    function regexAll(regex, subject) {
      var matches = [];
      var position = 0;
      var length = slen(subject);
      while (position <= length) {
        var match = rxSearch(regex.compiled, subject, position);
        if (match === null) break;
        matches.push(match);
        if (matches.length > MAX_ARRAY) throw rangeError("Invalid array length");
        position = match.end > match.start ? match.end : match.end + 1;
      }
      return matches;
    }

    function Interpreter(nowMs, random, clock, deadline) {
      this.nowMs = nowMs;
      this.random = random || function () { return 0.5; };
      this.clock = clock || null;
      this.deadline = deadline === undefined ? null : deadline;
      this.steps = 0;
      this.depth = 0;
      this.line = 1;
      this.logs = [];
      this.globals = new Scope(null);
      var globals = makeGlobals(this);
      var self = this;
      Object.keys(globals).forEach(function (name) { self.globals.declare(name, globals[name], "const"); });
    }

    Interpreter.prototype.tick = function () {
      this.steps += 1;
      if (this.steps > MAX_STEPS) throw new ScriptLimit("The script ran more than " + MAX_STEPS + " steps");
      if (this.steps % 1000 === 0 && this.clock !== null && this.deadline !== null && this.clock() > this.deadline) {
        throw new ScriptLimit("This run ran out of time");
      }
    };

    Interpreter.prototype.run = function (program, values) {
      var scope = new Scope(this.globals);
      for (var i = 0; i < values.length; i++) {
        scope.declare(values[i][0], values[i][1], values[i][0].charAt(0) === "$" ? "const" : "let");
      }
      var body = new Scope(scope);
      body.declare("this", undefined, "const");
      this.line = 1;
      var signal;
      try {
        signal = this.execBlock(program, body);
      } catch (error) {
        if (error instanceof Thrown) throw new ScriptError(error.line || this.line, this.describeError(error.value));
        if (error instanceof ScriptLimit) throw new ScriptError(this.line, error.message);
        if (error instanceof RangeError) throw new ScriptError(this.line, "Maximum call stack size exceeded");
        throw error;
      }
      if (signal instanceof Return) return signal.value;
      return undefined;
    };

    Interpreter.prototype.describeError = function (value) {
      if (isDict(value) && hasKey(value, "message")) {
        var name = hasKey(value, "name") ? value.name : null;
        var message = toString(value.message);
        if (typeof name === "string" && name && name !== "Error") return message ? name + ": " + message : name;
        return message || "Error";
      }
      return "Uncaught " + this.display(value);
    };

    Interpreter.prototype.display = function (value) {
      if (typeof value === "string") return value;
      if (Array.isArray(value) || isDict(value)) {
        var text;
        try {
          text = jsonStringify(value, "");
        } catch (error) {
          if (error instanceof Thrown) return "[object]";
          throw error;
        }
        return text === undefined ? "undefined" : text;
      }
      return toString(value);
    };

    Interpreter.prototype.log = function (args) {
      if (this.logs.length >= MAX_LOGS) return;
      var self = this;
      var text = args.map(function (arg) { return self.display(arg); }).join(" ");
      if (slen(text) > MAX_LOG_CHARS) text = cslice(text, 0, MAX_LOG_CHARS) + "\u2026";
      this.logs.push(text);
    };

    Interpreter.prototype.execBlock = function (statements, scope) {
      var i;
      for (i = 0; i < statements.length; i++) {
        if (statements[i][0] === "funcdecl") scope.declare(statements[i][2], this.makeFunction(statements[i][3], scope), "var");
      }
      for (i = 0; i < statements.length; i++) {
        var signal = this.exec(statements[i], scope);
        if (signal !== null) return signal;
      }
      return null;
    };

    Interpreter.prototype.exec = function (statement, scope) {
      this.line = statement[1];
      this.tick();
      try {
        return this.execInner(statement, scope);
      } catch (error) {
        if (error instanceof Thrown && !error.line) error.line = statement[1];
        throw error;
      }
    };

    Interpreter.prototype.execInner = function (statement, scope) {
      switch (statement[0]) {
        case "expr":
          this.eval(statement[2], scope);
          return null;
        case "var":
          return this.execVar(statement, scope);
        case "if":
          if (truthy(this.eval(statement[2], scope))) return this.exec(statement[3], scope);
          if (statement[4] !== null) return this.exec(statement[4], scope);
          return null;
        case "block":
          return this.execBlock(statement[2], new Scope(scope));
        case "while":
          return this.execWhile(statement, scope);
        case "dowhile":
          return this.execDoWhile(statement, scope);
        case "for":
          return this.execFor(statement, scope);
        case "forof":
          return this.execForOf(statement, scope);
        case "forin":
          return this.execForIn(statement, scope);
        case "return":
          return new Return(statement[2] === null ? undefined : this.eval(statement[2], scope));
        case "break":
          return BREAK;
        case "continue":
          return CONTINUE;
        case "throw":
          throw new Thrown(this.eval(statement[2], scope), statement[1]);
        case "try":
          return this.execTry(statement, scope);
        case "switch":
          return this.execSwitch(statement, scope);
        default:
          return null;
      }
    };

    Interpreter.prototype.execVar = function (statement, scope) {
      var kind = statement[2];
      var decls = statement[3];
      for (var i = 0; i < decls.length; i++) {
        var target = decls[i][0];
        var init = decls[i][1];
        var value = init === null ? undefined : this.eval(init, scope);
        if (init !== null && init[0] === "func" && target[0] === "pid" && value instanceof SFunction && !value.name) {
          value.name = target[2];
        }
        this.bind(target, value, scope, kind);
      }
      return null;
    };

    Interpreter.prototype.execWhile = function (statement, scope) {
      while (truthy(this.eval(statement[2], scope))) {
        var signal = this.exec(statement[3], scope);
        if (signal === BREAK) break;
        if (signal === CONTINUE || signal === null) {
          this.tick();
          continue;
        }
        return signal;
      }
      return null;
    };

    Interpreter.prototype.execDoWhile = function (statement, scope) {
      while (true) {
        var signal = this.exec(statement[2], scope);
        if (signal === BREAK) break;
        if (signal !== null && signal !== CONTINUE) return signal;
        this.tick();
        if (!truthy(this.eval(statement[3], scope))) break;
      }
      return null;
    };

    Interpreter.prototype.execFor = function (statement, scope) {
      var init = statement[2];
      var test = statement[3];
      var update = statement[4];
      var body = statement[5];
      var loop = new Scope(scope);
      var perIteration = false;
      if (init !== null) {
        if (init[0] === "var") {
          this.execVar(init, loop);
          perIteration = init[2] === "let" || init[2] === "const";
        } else {
          this.eval(init[2], loop);
        }
      }
      while (true) {
        if (test !== null && !truthy(this.eval(test, loop))) break;
        var signal = this.exec(body, loop);
        if (signal === BREAK) break;
        if (signal !== null && signal !== CONTINUE) return signal;
        this.tick();
        if (perIteration) {
          var fresh = new Scope(scope);
          var name;
          for (name in loop.vars) fresh.vars[name] = loop.vars[name];
          for (name in loop.consts) fresh.consts[name] = true;
          loop = fresh;
        }
        if (update !== null) this.eval(update, loop);
      }
      return null;
    };

    Interpreter.prototype.iterate = function (value, node) {
      if (Array.isArray(value)) return value;
      if (typeof value === "string") return Array.from(value);
      if (value instanceof SCollection) return collectionEntries(value);
      throw typeError(this.describeNode(node) + " is not iterable");
    };

    Interpreter.prototype.execForOf = function (statement, scope) {
      var sequence = this.iterate(this.eval(statement[4], scope), statement[4]);
      var index = 0;
      while (index < sequence.length) {
        var item = sequence[index];
        index += 1;
        var inner = new Scope(scope);
        this.bind(statement[3], item, inner, statement[2]);
        var signal = this.exec(statement[5], inner);
        if (signal === BREAK) break;
        if (signal !== null && signal !== CONTINUE) return signal;
        this.tick();
      }
      return null;
    };

    Interpreter.prototype.execForIn = function (statement, scope) {
      var value = this.eval(statement[4], scope);
      var keys;
      if (isDict(value)) {
        keys = Object.keys(value);
      } else if (Array.isArray(value) || typeof value === "string") {
        keys = [];
        var length = Array.isArray(value) ? value.length : slen(value);
        for (var i = 0; i < length; i++) keys.push(String(i));
      } else {
        keys = [];
      }
      for (var j = 0; j < keys.length; j++) {
        if (isDict(value) && !hasKey(value, keys[j])) continue;
        var inner = new Scope(scope);
        this.bind(statement[3], keys[j], inner, statement[2]);
        var signal = this.exec(statement[5], inner);
        if (signal === BREAK) break;
        if (signal !== null && signal !== CONTINUE) return signal;
        this.tick();
      }
      return null;
    };

    Interpreter.prototype.execTry = function (statement, scope) {
      var block = statement[2];
      var param = statement[3];
      var handler = statement[4];
      var final = statement[5];
      var signal;
      var extra;
      try {
        signal = this.execBlock(block, new Scope(scope));
      } catch (error) {
        if (!(error instanceof Thrown)) throw error;
        if (handler === null) {
          if (final !== null) {
            extra = this.execBlock(final, new Scope(scope));
            if (extra !== null) return extra;
          }
          throw error;
        }
        var inner = new Scope(scope);
        if (param !== null) this.bind(param, error.value, inner, "let");
        try {
          signal = this.execBlock(handler, inner);
        } catch (again) {
          if (!(again instanceof Thrown)) throw again;
          if (final !== null) {
            extra = this.execBlock(final, new Scope(scope));
            if (extra !== null) return extra;
          }
          throw again;
        }
      }
      if (final !== null) {
        extra = this.execBlock(final, new Scope(scope));
        if (extra !== null) return extra;
      }
      return signal;
    };

    Interpreter.prototype.execSwitch = function (statement, scope) {
      var value = this.eval(statement[2], scope);
      var cases = statement[3];
      var start = null;
      var i;
      for (i = 0; i < cases.length; i++) {
        if (cases[i][0] !== null && value === this.eval(cases[i][0], scope)) {
          start = i;
          break;
        }
      }
      if (start === null) {
        for (i = 0; i < cases.length; i++) {
          if (cases[i][0] === null) {
            start = i;
            break;
          }
        }
      }
      if (start === null) return null;
      var statements = [];
      for (i = start; i < cases.length; i++) statements = statements.concat(cases[i][1]);
      var signal = this.execBlock(statements, new Scope(scope));
      return signal === BREAK ? null : signal;
    };

    Interpreter.prototype.bind = function (target, value, scope, kind) {
      var tag = target[0];
      var i;
      if (tag === "pid") {
        if (kind === null) this.assignName(target[2], value, scope);
        else scope.declare(target[2], value, kind);
        return;
      }
      if (tag === "pexpr") {
        this.assignTarget(target[2], value, scope);
        return;
      }
      if (tag === "parr") {
        if (!(Array.isArray(value) || typeof value === "string" || value instanceof SCollection)) {
          throw typeError(this.displayShort(value) + " is not iterable");
        }
        var sequence = this.iterate(value, null);
        for (i = 0; i < target[2].length; i++) {
          var element = target[2][i];
          if (element === null) continue;
          var item = i < sequence.length ? sequence[i] : undefined;
          if (item === undefined && element[1] !== null) item = this.eval(element[1], scope);
          this.bind(element[0], item, scope, kind);
        }
        if (target[3] !== null) this.bind(target[3], sequence.slice(target[2].length), scope, kind);
        return;
      }
      if (tag === "pobj") {
        if (value === null || value === undefined) throw typeError("Cannot destructure " + toString(value));
        var used = [];
        for (i = 0; i < target[2].length; i++) {
          var key = target[2][i][0];
          var pattern = target[2][i][1];
          var fallback = target[2][i][2];
          used.push(key);
          var found = this.get(value, key);
          if (found === undefined && fallback !== null) {
            found = this.eval(fallback, scope);
            if (fallback[0] === "func" && found instanceof SFunction && !found.name && pattern[0] === "pid") found.name = pattern[2];
          }
          this.bind(pattern, found, scope, kind);
        }
        if (target[3] !== null) {
          var rest = dict();
          if (isDict(value)) {
            Object.keys(value).forEach(function (name) {
              if (used.indexOf(name) < 0) rest[name] = value[name];
            });
          }
          this.bind(target[3], rest, scope, kind);
        }
        return;
      }
      throw typeError("Invalid assignment target");
    };

    Interpreter.prototype.displayShort = function (value) {
      if (value === undefined) return "undefined";
      if (value === null) return "null";
      return typeOf(value);
    };

    Interpreter.prototype.assignName = function (name, value, scope) {
      var owner = scope.lookup(name);
      if (owner === null) throw new Thrown(errorObject("ReferenceError", name + " is not defined"));
      if (owner.consts[name]) throw typeError("Assignment to constant variable.");
      owner.vars[name] = value;
    };

    Interpreter.prototype.assignTarget = function (node, value, scope) {
      if (node[0] === "ident") {
        this.assignName(node[2], value, scope);
      } else if (node[0] === "member") {
        this.put(this.eval(node[2], scope), node[3], value);
      } else if (node[0] === "index") {
        var obj = this.eval(node[2], scope);
        var key = this.eval(node[3], scope);
        this.put(obj, key, value);
      } else {
        throw typeError("Invalid assignment target");
      }
    };

    Interpreter.prototype.eval = function (node, scope) {
      switch (node[0]) {
        case "lit":
          return node[2];
        case "ident":
          return this.evalIdent(node, scope);
        case "tmpl":
          return this.evalTemplate(node, scope);
        case "regex":
          return new SRegExp(node[2], node[3], node[4]);
        case "array":
          return this.evalArray(node, scope);
        case "object":
          return this.evalObject(node, scope);
        case "member":
          var target = this.eval(node[2], scope);
          if (node[4] && (target === null || target === undefined)) throw SHORT;
          return this.get(target, node[3], node[2]);
        case "index":
          var owner = this.eval(node[2], scope);
          if (node[4] && (owner === null || owner === undefined)) throw SHORT;
          return this.get(owner, this.eval(node[3], scope), node[2]);
        case "call":
          return this.evalCall(node, scope);
        case "chain":
          try {
            return this.eval(node[2], scope);
          } catch (error) {
            if (error === SHORT) return undefined;
            throw error;
          }
        case "new":
          return this.evalNew(node, scope);
        case "unary":
          return this.evalUnary(node, scope);
        case "update":
          return this.evalUpdate(node, scope);
        case "bin":
          var left = this.eval(node[3], scope);
          var right = this.eval(node[4], scope);
          return this.binary(node[2], left, right);
        case "logic":
          var first = this.eval(node[3], scope);
          if (node[2] === "&&") return truthy(first) ? this.eval(node[4], scope) : first;
          if (node[2] === "||") return truthy(first) ? first : this.eval(node[4], scope);
          return first === null || first === undefined ? this.eval(node[4], scope) : first;
        case "cond":
          return truthy(this.eval(node[2], scope)) ? this.eval(node[3], scope) : this.eval(node[4], scope);
        case "assign":
          return this.evalAssign(node, scope);
        case "func":
          return this.makeFunction(node, scope);
        case "seq":
          var value;
          for (var i = 0; i < node[2].length; i++) value = this.eval(node[2][i], scope);
          return value;
        case "this":
          var holder = scope.lookup("this");
          return holder !== null ? holder.vars["this"] : undefined;
        default:
          throw typeError("Unknown expression");
      }
    };

    Interpreter.prototype.evalIdent = function (node, scope) {
      var name = node[2];
      var current = scope;
      while (current !== null) {
        if (name in current.vars) return current.vars[name];
        current = current.parent;
      }
      throw new Thrown(errorObject("ReferenceError", name + " is not defined"), node[1]);
    };

    Interpreter.prototype.evalTemplate = function (node, scope) {
      var strings = node[2];
      var out = [strings[0]];
      var total = slen(strings[0]);
      for (var i = 0; i < node[3].length; i++) {
        var text = toString(toPrimitive(this.eval(node[3][i], scope)));
        out.push(text);
        out.push(strings[i + 1]);
        total += slen(text) + slen(strings[i + 1]);
        if (total > MAX_STRING) throw rangeError("Invalid string length");
      }
      return out.join("");
    };

    Interpreter.prototype.evalArray = function (node, scope) {
      var out = [];
      for (var i = 0; i < node[2].length; i++) {
        var element = node[2][i];
        if (element === null) {
          out.push(undefined);
        } else if (element[0] === "spread") {
          var items = this.iterate(this.eval(element[2], scope), element[2]);
          for (var j = 0; j < items.length; j++) out.push(items[j]);
          checkArray(out);
        } else {
          out.push(this.eval(element, scope));
        }
      }
      return out;
    };

    Interpreter.prototype.evalObject = function (node, scope) {
      var out = dict();
      for (var i = 0; i < node[2].length; i++) {
        var prop = node[2][i];
        if (prop[0] === "spread") {
          var value = this.eval(prop[1], scope);
          if (isDict(value)) {
            Object.keys(value).forEach(function (key) { out[key] = value[key]; });
          } else if (Array.isArray(value)) {
            value.forEach(function (item, index) { out[String(index)] = item; });
          } else if (typeof value === "string") {
            Array.from(value).forEach(function (ch, index) { out[String(index)] = ch; });
          }
          continue;
        }
        var key = prop[3] ? propertyKey(this.eval(prop[1], scope)) : prop[1];
        var item = this.eval(prop[2], scope);
        if (prop[2][0] === "func" && item instanceof SFunction && !item.name) item.name = key;
        out[key] = item;
      }
      return out;
    };

    Interpreter.prototype.arguments = function (nodes, scope) {
      var args = [];
      for (var i = 0; i < nodes.length; i++) {
        if (nodes[i][0] === "spread") {
          var items = this.iterate(this.eval(nodes[i][2], scope), nodes[i][2]);
          for (var j = 0; j < items.length; j++) args.push(items[j]);
          checkArray(args);
        } else {
          args.push(this.eval(nodes[i], scope));
        }
      }
      return args;
    };

    Interpreter.prototype.evalCall = function (node, scope) {
      var callee = node[2];
      var self;
      var fn;
      if (callee[0] === "member" || callee[0] === "index") {
        var obj = this.eval(callee[2], scope);
        if (callee[4] && (obj === null || obj === undefined)) throw SHORT;
        var key = callee[0] === "member" ? callee[3] : this.eval(callee[3], scope);
        fn = this.get(obj, key, callee[2]);
        self = obj;
      } else {
        fn = this.eval(callee, scope);
      }
      if (node[4] && (fn === null || fn === undefined)) throw SHORT;
      var args = this.arguments(node[3], scope);
      if (!isCallable(fn)) throw new Thrown(errorObject("TypeError", this.describeNode(callee) + " is not a function"), node[1]);
      return this.call(fn, self, args);
    };

    Interpreter.prototype.describeNode = function (node) {
      if (node === null) return "expression";
      var kind = node[0];
      if (kind === "ident") return node[2];
      if (kind === "member") return this.describeNode(node[2]) + "." + node[3];
      if (kind === "index") return this.describeNode(node[2]) + "[...]";
      if (kind === "call") return this.describeNode(node[2]) + "(...)";
      if (kind === "this") return "this";
      if (kind === "chain") return this.describeNode(node[2]);
      if (kind === "lit") return typeof node[2] === "string" ? jsonQuote(node[2]) : toString(node[2]);
      return "expression";
    };

    Interpreter.prototype.call = function (fn, self, args) {
      if (fn instanceof SNative) return fn.fn(this, self, args);
      this.depth += 1;
      if (this.depth > MAX_DEPTH) {
        this.depth -= 1;
        throw new ScriptLimit("Maximum call stack size exceeded");
      }
      try {
        this.tick();
        var scope = new Scope(fn.scope);
        if (!fn.arrow) scope.declare("this", self, "let");
        for (var i = 0; i < fn.params.length; i++) {
          var value = i < args.length ? args[i] : undefined;
          if (value === undefined && fn.params[i][1] !== null) value = this.eval(fn.params[i][1], scope);
          this.bind(fn.params[i][0], value, scope, "let");
        }
        if (fn.rest !== null) this.bind(fn.rest, args.slice(fn.params.length), scope, "let");
        if (fn.expression) return this.eval(fn.body, scope);
        var saved = this.line;
        var signal = this.execBlock(fn.body, scope);
        this.line = saved;
        if (signal instanceof Return) return signal.value;
        return undefined;
      } finally {
        this.depth -= 1;
      }
    };

    Interpreter.prototype.makeFunction = function (node, scope) {
      return new SFunction(node[2], node[3], node[4], node[5], scope, node[6], node[7]);
    };

    Interpreter.prototype.evalNew = function (node, scope) {
      var callee = this.eval(node[2], scope);
      var args = this.arguments(node[3], scope);
      if (callee instanceof SNative && callee.construct !== null) return callee.construct(this, args);
      throw new Thrown(errorObject("TypeError", this.describeNode(node[2]) + " is not a constructor"), node[1]);
    };

    Interpreter.prototype.evalUnary = function (node, scope) {
      var op = node[2];
      var operand = node[3];
      if (op === "typeof") {
        if (operand[0] === "ident" && scope.lookup(operand[2]) === null) return "undefined";
        return typeOf(this.eval(operand, scope));
      }
      if (op === "delete") {
        if (operand[0] === "member" || operand[0] === "index") {
          var obj = this.eval(operand[2], scope);
          var key = operand[0] === "member" ? operand[3] : propertyKey(this.eval(operand[3], scope));
          if (obj === null || obj === undefined) throw typeError("Cannot convert undefined or null to object");
          if (isDict(obj)) {
            delete obj[key];
          } else if (Array.isArray(obj)) {
            var index = arrayIndex(key);
            if (index !== null && index < obj.length) obj[index] = undefined;
          }
        }
        return true;
      }
      var value = this.eval(operand, scope);
      if (op === "!") return !truthy(value);
      if (op === "-") return -toNumber(value);
      if (op === "+") return toNumber(value);
      if (op === "~") return ~toNumber(value);
      if (op === "void") return undefined;
      throw typeError("Unknown operator " + op);
    };

    Interpreter.prototype.readTarget = function (target, scope) {
      if (target[0] === "ident") return [null, null, this.evalIdent(target, scope)];
      var obj = this.eval(target[2], scope);
      var key = target[0] === "member" ? target[3] : this.eval(target[3], scope);
      return [obj, key, this.get(obj, key, target[2])];
    };

    Interpreter.prototype.writeTarget = function (target, obj, key, value, scope) {
      if (target[0] === "ident") this.assignName(target[2], value, scope);
      else this.put(obj, key, value);
    };

    Interpreter.prototype.evalUpdate = function (node, scope) {
      var read = this.readTarget(node[4], scope);
      var old = toNumber(read[2]);
      var next = node[2] === "++" ? old + 1 : old - 1;
      this.writeTarget(node[4], read[0], read[1], next, scope);
      return node[3] ? next : old;
    };

    Interpreter.prototype.evalAssign = function (node, scope) {
      var op = node[2];
      var target = node[3];
      var value;
      if (op === "=") {
        value = this.eval(node[4], scope);
        if (node[4][0] === "func" && target[0] === "pid" && value instanceof SFunction && !value.name) value.name = target[2];
        this.bind(target, value, scope, null);
        return value;
      }
      var read = this.readTarget(target, scope);
      var old = read[2];
      if (op === "&&=" || op === "||=" || op === "??=") {
        if (op === "&&=" && !truthy(old)) return old;
        if (op === "||=" && truthy(old)) return old;
        if (op === "??=" && old !== null && old !== undefined) return old;
        value = this.eval(node[4], scope);
      } else {
        value = this.binary(op.slice(0, -1), old, this.eval(node[4], scope));
      }
      this.writeTarget(target, read[0], read[1], value, scope);
      return value;
    };

    Interpreter.prototype.binary = function (op, a, b) {
      var pa;
      var pb;
      switch (op) {
        case "+":
          if (typeof a === "number" && typeof b === "number") return a + b;
          pa = toPrimitive(a);
          pb = toPrimitive(b);
          if (typeof pa === "string" || typeof pb === "string") return checkString(toString(pa) + toString(pb));
          return toNumber(pa) + toNumber(pb);
        case "-":
          return toNumber(a) - toNumber(b);
        case "*":
          return toNumber(a) * toNumber(b);
        case "/":
          return toNumber(a) / toNumber(b);
        case "%":
          return toNumber(a) % toNumber(b);
        case "**":
          return Math.pow(toNumber(a), toNumber(b));
        case "===":
          return a === b;
        case "!==":
          return a !== b;
        case "==":
          return looseEquals(a, b);
        case "!=":
          return !looseEquals(a, b);
        case "<":
        case ">":
        case "<=":
        case ">=":
          pa = toPrimitive(a, "number");
          pb = toPrimitive(b, "number");
          if (typeof pa === "string" && typeof pb === "string") {
            var order = compareStrings(pa, pb);
            if (op === "<") return order < 0;
            if (op === ">") return order > 0;
            if (op === "<=") return order <= 0;
            return order >= 0;
          }
          var x = toNumber(pa);
          var y = toNumber(pb);
          if (op === "<") return x < y;
          if (op === ">") return x > y;
          if (op === "<=") return x <= y;
          return x >= y;
        case "&":
          return toNumber(a) & toNumber(b);
        case "|":
          return toNumber(a) | toNumber(b);
        case "^":
          return toNumber(a) ^ toNumber(b);
        case "<<":
          return toNumber(a) << toNumber(b);
        case ">>":
          return toNumber(a) >> toNumber(b);
        case ">>>":
          return toNumber(a) >>> toNumber(b);
        case "in":
          if (isDict(b)) return hasKey(b, propertyKey(a));
          if (Array.isArray(b)) {
            var key = propertyKey(a);
            var index = arrayIndex(key);
            return key === "length" || (index !== null && index < b.length);
          }
          throw typeError("Cannot use 'in' operator to search for '" + toString(a) + "' in " + toString(b));
        case "instanceof":
          return this.instanceOf(a, b);
        default:
          throw typeError("Unknown operator " + op);
      }
    };

    Interpreter.prototype.instanceOf = function (value, kind) {
      if (!isCallable(kind)) throw typeError("Right-hand side of 'instanceof' is not callable");
      var name = kind instanceof SNative ? kind.name : "";
      if (name === "Array") return Array.isArray(value);
      if (name === "Object") return isObjectLike(value);
      if (name === "Date") return value instanceof SDate;
      if (name === "RegExp") return value instanceof SRegExp;
      if (name === "Set" || name === "Map") return value instanceof SCollection && value.kind === name;
      if (name === "Function") return isCallable(value);
      if (["Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "URIError"].indexOf(name) >= 0) {
        if (!(isDict(value) && hasKey(value, "name") && typeof value.name === "string" && hasKey(value, "message"))) return false;
        return name === "Error" || value.name === name;
      }
      return false;
    };

    function native(name, method) {
      return new SNative(name, method);
    }

    Interpreter.prototype.get = function (obj, key) {
      var name;
      var index;
      if (isDict(obj)) {
        name = typeof key === "string" ? key : propertyKey(key);
        if (hasKey(obj, name)) return obj[name];
        return hasKey(OBJECT_METHODS, name) ? native(name, OBJECT_METHODS[name]) : undefined;
      }
      if (Array.isArray(obj)) {
        index = arrayIndex(key);
        if (index !== null) return index < obj.length ? obj[index] : undefined;
        name = propertyKey(key);
        if (name === "length") return obj.length;
        if (obj.$match !== undefined && (name === "index" || name === "input" || name === "groups")) return obj.$match[name];
        return hasKey(ARRAY_METHODS, name) ? native(name, ARRAY_METHODS[name]) : undefined;
      }
      if (typeof obj === "string") {
        index = arrayIndex(key);
        if (index !== null) return index < slen(obj) ? charAt(obj, index) : undefined;
        name = propertyKey(key);
        if (name === "length") return slen(obj);
        return hasKey(STRING_METHODS, name) ? native(name, STRING_METHODS[name]) : undefined;
      }
      if (obj === null || obj === undefined) {
        throw typeError("Cannot read properties of " + toString(obj) + " (reading '" + propertyKey(key) + "')");
      }
      name = propertyKey(key);
      if (typeof obj === "number") return hasKey(NUMBER_METHODS, name) ? native(name, NUMBER_METHODS[name]) : undefined;
      if (typeof obj === "boolean") {
        return name === "toString" ? native(name, function (i, t) { return toString(t); }) : undefined;
      }
      if (obj instanceof SDate) return hasKey(DATE_METHODS, name) ? native(name, DATE_METHODS[name]) : undefined;
      if (obj instanceof SCollection) {
        if (name === "size") return obj.map.size;
        var table = obj.kind === "Set" ? SET_METHODS : MAP_METHODS;
        return hasKey(table, name) ? native(name, table[name]) : undefined;
      }
      if (obj instanceof SRegExp) {
        if (name === "source") return obj.source;
        if (name === "flags") return obj.flags.split("").sort(function (a, b) { return "dgimsuy".indexOf(a) - "dgimsuy".indexOf(b); }).join("");
        if (name === "global") return obj.flags.indexOf("g") >= 0;
        if (name === "ignoreCase") return obj.flags.indexOf("i") >= 0;
        if (name === "multiline") return obj.flags.indexOf("m") >= 0;
        if (name === "lastIndex") return obj.lastIndex;
        return hasKey(REGEXP_METHODS, name) ? native(name, REGEXP_METHODS[name]) : undefined;
      }
      if (obj instanceof SNative) {
        if (hasKey(obj.props, name)) return obj.props[name];
        if (name === "name") return obj.name;
        return hasKey(FUNCTION_METHODS, name) ? native(name, FUNCTION_METHODS[name]) : undefined;
      }
      if (obj instanceof SFunction) {
        if (name === "name") return obj.name;
        if (name === "length") return obj.params.length;
        return hasKey(FUNCTION_METHODS, name) ? native(name, FUNCTION_METHODS[name]) : undefined;
      }
      return undefined;
    };

    Interpreter.prototype.put = function (obj, key, value) {
      var name;
      if (isDict(obj)) {
        obj[propertyKey(key)] = value;
        return;
      }
      if (Array.isArray(obj)) {
        var index = arrayIndex(key);
        if (index !== null) {
          if (index < obj.length) {
            obj[index] = value;
            return;
          }
          if (index >= MAX_ARRAY) throw rangeError("Invalid array length");
          while (obj.length < index) obj.push(undefined);
          obj.push(value);
          return;
        }
        name = propertyKey(key);
        if (name === "length") {
          var length = toNumber(value);
          if (length !== length || length < 0 || !Number.isInteger(length) || length > 4294967295) throw rangeError("Invalid array length");
          if (length > MAX_ARRAY) throw rangeError("Invalid array length");
          if (length < obj.length) obj.splice(length, obj.length - length);
          else while (obj.length < length) obj.push(undefined);
          return;
        }
        throw typeError("Cannot add property " + name + " to a list");
      }
      if (obj === null || obj === undefined) {
        throw typeError("Cannot set properties of " + toString(obj) + " (setting '" + propertyKey(key) + "')");
      }
      if (obj instanceof SRegExp && propertyKey(key) === "lastIndex") {
        var position = toInteger(value);
        obj.lastIndex = position !== Infinity ? Math.max(0, position) : MAX_STRING + 1;
        return;
      }
      throw typeError("Cannot create property '" + propertyKey(key) + "' on " + typeOf(obj) + " '" + toString(obj) + "'");
    };

    function arg(args, index) {
      return index < args.length ? args[index] : undefined;
    }

    function collectionEntries(collection) {
      var out = [];
      collection.map.forEach(function (value, key) { out.push(collection.kind === "Set" ? key : [key, value]); });
      return out;
    }

    function collectionPut(collection, key, value) {
      if (!collection.map.has(key) && collection.map.size >= MAX_ARRAY) throw rangeError("Invalid array length");
      collection.map.set(key, value);
    }

    function iterateCallback(interp, list, args, visit) {
      var fn = arg(args, 0);
      if (!isCallable(fn)) throw typeError(interp.displayShort(fn) + " is not a function");
      var self = arg(args, 1);
      var length = list.length;
      for (var index = 0; index < length && index < list.length; index++) {
        var value = list[index];
        if (visit(index, value, interp.call(fn, self, [value, index, list]))) return true;
      }
      return false;
    }

    function timSort(list, less) {
      var MIN_GALLOP = 7;
      var a = list.slice();
      var total = a.length;
      var minGallop = MIN_GALLOP;
      var pending = [];

      function reverse(lo, hi) {
        hi -= 1;
        while (lo < hi) {
          var swap = a[lo];
          a[lo] = a[hi];
          a[hi] = swap;
          lo += 1;
          hi -= 1;
        }
      }

      function countRun(lo, remaining) {
        var n;
        for (n = 1; n < remaining; n++) {
          if (less(a[lo + n], a[lo + n - 1])) break;
        }
        if (n === remaining) return n;
        if (n > 1) {
          if (less(a[lo], a[lo + n - 1])) return n;
          reverse(lo, lo + n);
        }
        n += 1;
        var neq = 0;
        for (; n < remaining; n++) {
          if (less(a[lo + n], a[lo + n - 1])) {
            if (neq) {
              neq += 1;
              reverse(lo + n - neq, lo + n);
              neq = 0;
            }
          } else if (less(a[lo + n - 1], a[lo + n])) {
            break;
          } else {
            neq += 1;
          }
        }
        if (neq) {
          neq += 1;
          reverse(lo + n - neq, lo + n);
        }
        reverse(lo, lo + n);
        for (; n < remaining; n++) {
          if (less(a[lo + n], a[lo + n - 1])) break;
        }
        return n;
      }

      function binarySort(lo, hi, start) {
        if (lo === start) start += 1;
        for (; start < hi; start++) {
          var left = lo;
          var right = start;
          var pivot = a[right];
          do {
            var p = left + ((right - left) >> 1);
            if (less(pivot, a[p])) right = p;
            else left = p + 1;
          } while (left < right);
          for (var m = start; m > left; m--) a[m] = a[m - 1];
          a[left] = pivot;
        }
      }

      function minRun(n) {
        var r = 0;
        while (n >= 64) {
          r |= n & 1;
          n >>= 1;
        }
        return n + r;
      }

      function gallopLeft(key, base, n, hint, source) {
        var lastofs = 0;
        var ofs = 1;
        var maxofs;
        var k;
        if (less(source[base + hint], key)) {
          maxofs = n - hint;
          while (ofs < maxofs) {
            if (less(source[base + hint + ofs], key)) {
              lastofs = ofs;
              ofs = (ofs << 1) + 1;
            } else {
              break;
            }
          }
          if (ofs > maxofs) ofs = maxofs;
          lastofs += hint;
          ofs += hint;
        } else {
          maxofs = hint + 1;
          while (ofs < maxofs) {
            if (less(source[base + hint - ofs], key)) break;
            lastofs = ofs;
            ofs = (ofs << 1) + 1;
          }
          if (ofs > maxofs) ofs = maxofs;
          k = lastofs;
          lastofs = hint - ofs;
          ofs = hint - k;
        }
        lastofs += 1;
        while (lastofs < ofs) {
          var m = lastofs + ((ofs - lastofs) >> 1);
          if (less(source[base + m], key)) lastofs = m + 1;
          else ofs = m;
        }
        return ofs;
      }

      function gallopRight(key, base, n, hint, source) {
        var lastofs = 0;
        var ofs = 1;
        var maxofs;
        var k;
        if (less(key, source[base + hint])) {
          maxofs = hint + 1;
          while (ofs < maxofs) {
            if (less(key, source[base + hint - ofs])) {
              lastofs = ofs;
              ofs = (ofs << 1) + 1;
            } else {
              break;
            }
          }
          if (ofs > maxofs) ofs = maxofs;
          k = lastofs;
          lastofs = hint - ofs;
          ofs = hint - k;
        } else {
          maxofs = n - hint;
          while (ofs < maxofs) {
            if (less(key, source[base + hint + ofs])) break;
            lastofs = ofs;
            ofs = (ofs << 1) + 1;
          }
          if (ofs > maxofs) ofs = maxofs;
          lastofs += hint;
          ofs += hint;
        }
        lastofs += 1;
        while (lastofs < ofs) {
          var m = lastofs + ((ofs - lastofs) >> 1);
          if (less(key, source[base + m])) ofs = m;
          else lastofs = m + 1;
        }
        return ofs;
      }

      function mergeLo(pa, na, pb, nb) {
        var temp = a.slice(pa, pa + na);
        var ia = 0;
        var dest = pa;
        var k;
        a[dest++] = a[pb++];
        nb -= 1;
        var finish = function () {
          while (na > 0) {
            a[dest++] = temp[ia++];
            na -= 1;
          }
        };
        var copyB = function () {
          while (nb > 0) {
            a[dest++] = a[pb++];
            nb -= 1;
          }
          a[dest] = temp[ia];
        };
        if (nb === 0) return finish();
        if (na === 1) return copyB();
        var gallop = minGallop;
        while (true) {
          var acount = 0;
          var bcount = 0;
          while (true) {
            if (less(a[pb], temp[ia])) {
              a[dest++] = a[pb++];
              bcount += 1;
              acount = 0;
              nb -= 1;
              if (nb === 0) return finish();
              if (bcount >= gallop) break;
            } else {
              a[dest++] = temp[ia++];
              acount += 1;
              bcount = 0;
              na -= 1;
              if (na === 1) return copyB();
              if (acount >= gallop) break;
            }
          }
          gallop += 1;
          do {
            gallop -= gallop > 1 ? 1 : 0;
            minGallop = gallop;
            k = gallopRight(a[pb], ia, na, 0, temp);
            acount = k;
            if (k) {
              for (var i = 0; i < k; i++) a[dest++] = temp[ia++];
              na -= k;
              if (na === 1) return copyB();
              if (na === 0) return finish();
            }
            a[dest++] = a[pb++];
            nb -= 1;
            if (nb === 0) return finish();
            k = gallopLeft(temp[ia], pb, nb, 0, a);
            bcount = k;
            if (k) {
              for (var j = 0; j < k; j++) a[dest++] = a[pb++];
              nb -= k;
              if (nb === 0) return finish();
            }
            a[dest++] = temp[ia++];
            na -= 1;
            if (na === 1) return copyB();
          } while (acount >= MIN_GALLOP || bcount >= MIN_GALLOP);
          gallop += 1;
          minGallop = gallop;
        }
      }

      function mergeHi(pa, na, pb, nb) {
        var temp = a.slice(pb, pb + nb);
        var dest = pb + nb - 1;
        var ib = nb - 1;
        var ia = pa + na - 1;
        var k;
        a[dest--] = a[ia--];
        na -= 1;
        var finish = function () {
          for (var i = nb - 1; i >= 0; i--) a[dest - (nb - 1) + i] = temp[i];
        };
        var copyA = function () {
          dest -= na;
          ia -= na;
          for (var i = na; i >= 1; i--) a[dest + i] = a[ia + i];
          a[dest] = temp[ib];
        };
        if (na === 0) return finish();
        if (nb === 1) return copyA();
        var gallop = minGallop;
        while (true) {
          var acount = 0;
          var bcount = 0;
          while (true) {
            if (less(temp[ib], a[ia])) {
              a[dest--] = a[ia--];
              acount += 1;
              bcount = 0;
              na -= 1;
              if (na === 0) return finish();
              if (acount >= gallop) break;
            } else {
              a[dest--] = temp[ib--];
              bcount += 1;
              acount = 0;
              nb -= 1;
              if (nb === 1) return copyA();
              if (bcount >= gallop) break;
            }
          }
          gallop += 1;
          do {
            gallop -= gallop > 1 ? 1 : 0;
            minGallop = gallop;
            k = gallopRight(temp[ib], pa, na, na - 1, a);
            k = na - k;
            acount = k;
            if (k) {
              dest -= k;
              ia -= k;
              for (var i = k; i >= 1; i--) a[dest + i] = a[ia + i];
              na -= k;
              if (na === 0) return finish();
            }
            a[dest--] = temp[ib--];
            nb -= 1;
            if (nb === 1) return copyA();
            k = gallopLeft(a[ia], 0, nb, nb - 1, temp);
            k = nb - k;
            bcount = k;
            if (k) {
              dest -= k;
              ib -= k;
              for (var j = k; j >= 1; j--) a[dest + j] = temp[ib + j];
              nb -= k;
              if (nb === 1) return copyA();
              if (nb === 0) return finish();
            }
            a[dest--] = a[ia--];
            na -= 1;
            if (na === 0) return finish();
          } while (acount >= MIN_GALLOP || bcount >= MIN_GALLOP);
          gallop += 1;
          minGallop = gallop;
        }
      }

      function mergeAt(i) {
        var pa = pending[i].base;
        var na = pending[i].len;
        var pb = pending[i + 1].base;
        var nb = pending[i + 1].len;
        pending[i].len = na + nb;
        pending.splice(i + 1, 1);
        var k = gallopRight(a[pb], pa, na, 0, a);
        pa += k;
        na -= k;
        if (na === 0) return;
        nb = gallopLeft(a[pa + na - 1], pb, nb, nb - 1, a);
        if (nb <= 0) return;
        if (na <= nb) mergeLo(pa, na, pb, nb);
        else mergeHi(pa, na, pb, nb);
      }

      function powerLoop(s1, n1, n2, n) {
        var result = 0;
        var x = 2 * s1 + n1;
        var y = x + n1 + n2;
        while (true) {
          result += 1;
          if (x >= n) {
            x -= n;
            y -= n;
          } else if (y >= n) {
            break;
          }
          x *= 2;
          y *= 2;
        }
        return result;
      }

      function foundNewRun(n2) {
        if (!pending.length) return;
        var last = pending[pending.length - 1];
        var power = powerLoop(last.base, last.len, n2, total);
        while (pending.length > 1 && pending[pending.length - 2].power > power) mergeAt(pending.length - 2);
        pending[pending.length - 1].power = power;
      }

      if (total < 2) return a;
      var lo = 0;
      var remaining = total;
      var minrun = minRun(total);
      do {
        var n = countRun(lo, remaining);
        if (n < minrun) {
          var force = remaining <= minrun ? remaining : minrun;
          binarySort(lo, lo + force, lo + n);
          n = force;
        }
        foundNewRun(n);
        pending.push({ base: lo, len: n, power: 0 });
        lo += n;
        remaining -= n;
      } while (remaining);
      while (pending.length > 1) {
        var index = pending.length - 2;
        if (index > 0 && pending[index - 1].len < pending[index + 1].len) index -= 1;
        mergeAt(index);
      }
      return a;
    }

    function sortValues(interp, items, comparator) {
      var compare;
      if (comparator === undefined) {
        compare = function (a, b) {
          if (a === undefined) return b === undefined ? 0 : 1;
          if (b === undefined) return -1;
          return compareStrings(toString(a), toString(b));
        };
      } else {
        if (!isCallable(comparator)) throw typeError("The comparison function must be either a function or undefined");
        compare = function (a, b) {
          if (a === undefined) return b === undefined ? 0 : 1;
          if (b === undefined) return -1;
          var result = toNumber(interp.call(comparator, undefined, [a, b]));
          if (result !== result) return 0;
          return result > 0 ? 1 : (result < 0 ? -1 : 0);
        };
      }
      return timSort(items, function (x, y) { return compare(x, y) < 0; });
    }

    function flatten(items, depth, out) {
      for (var i = 0; i < items.length; i++) {
        if (Array.isArray(items[i]) && depth >= 1) flatten(items[i], depth - 1, out);
        else out.push(items[i]);
        checkArray(out);
      }
      return out;
    }

    function reduceList(interp, list, args, reverse) {
      var fn = arg(args, 0);
      if (!isCallable(fn)) throw typeError(interp.displayShort(fn) + " is not a function");
      var indexes = [];
      for (var i = 0; i < list.length; i++) indexes.push(i);
      if (reverse) indexes.reverse();
      var acc;
      if (args.length >= 2) {
        acc = args[1];
      } else {
        if (!indexes.length) throw typeError("Reduce of empty array with no initial value");
        acc = list[indexes.shift()];
      }
      for (var j = 0; j < indexes.length; j++) {
        if (indexes[j] >= list.length) continue;
        acc = interp.call(fn, undefined, [acc, list[indexes[j]], indexes[j], list]);
      }
      return acc;
    }

    function findFromEnd(interp, list, args, wantIndex) {
      var fn = arg(args, 0);
      if (!isCallable(fn)) throw typeError(interp.displayShort(fn) + " is not a function");
      for (var index = list.length - 1; index >= 0; index--) {
        var value = index < list.length ? list[index] : undefined;
        if (truthy(interp.call(fn, arg(args, 1), [value, index, list]))) return wantIndex ? index : value;
      }
      return wantIndex ? -1 : undefined;
    }

    var ARRAY_METHODS = dict();
    ARRAY_METHODS.push = function (i, t, a) {
      for (var k = 0; k < a.length; k++) t.push(a[k]);
      checkArray(t);
      return t.length;
    };
    ARRAY_METHODS.pop = function (i, t) { return t.length ? t.pop() : undefined; };
    ARRAY_METHODS.shift = function (i, t) { return t.length ? t.shift() : undefined; };
    ARRAY_METHODS.unshift = function (i, t, a) {
      t.unshift.apply(t, a);
      checkArray(t);
      return t.length;
    };
    ARRAY_METHODS.slice = function (i, t, a) {
      var start = relative(arg(a, 0), t.length, 0);
      var end = relative(arg(a, 1), t.length, t.length);
      return start < end ? t.slice(start, end) : [];
    };
    ARRAY_METHODS.splice = function (i, t, a) {
      var length = t.length;
      var start = relative(arg(a, 0), length, 0);
      var count;
      if (!a.length) count = 0;
      else if (a.length === 1) count = length - start;
      else count = Math.max(0, Math.min(toInteger(a[1]), length - start));
      var removed = t.splice.apply(t, [start, count].concat(a.slice(2)));
      checkArray(t);
      return removed;
    };
    ARRAY_METHODS.concat = function (i, t, a) {
      var out = t.slice();
      for (var k = 0; k < a.length; k++) {
        if (Array.isArray(a[k])) {
          for (var m = 0; m < a[k].length; m++) out.push(a[k][m]);
        } else {
          out.push(a[k]);
        }
        checkArray(out);
      }
      return out;
    };
    ARRAY_METHODS.join = function (i, t, a) {
      var separator = arg(a, 0);
      return joinList(t, separator === undefined ? "," : toString(separator), [t]);
    };
    ARRAY_METHODS.reverse = function (i, t) { return t.reverse(); };
    ARRAY_METHODS.sort = function (i, t, a) {
      var sorted = sortValues(i, t, arg(a, 0));
      for (var k = 0; k < sorted.length; k++) t[k] = sorted[k];
      return t;
    };
    ARRAY_METHODS.toSorted = function (i, t, a) { return sortValues(i, t.slice(), arg(a, 0)); };
    ARRAY_METHODS.toReversed = function (i, t) { return t.slice().reverse(); };
    ARRAY_METHODS.indexOf = function (i, t, a) {
      var target = arg(a, 0);
      for (var k = relative(arg(a, 1), t.length, 0); k < t.length; k++) {
        if (t[k] === target) return k;
      }
      return -1;
    };
    ARRAY_METHODS.lastIndexOf = function (i, t, a) {
      var target = arg(a, 0);
      var length = t.length;
      var start;
      if (a.length > 1) {
        start = toInteger(a[1]);
        start = start >= 0 ? Math.min(start, length - 1) : length + start;
      } else {
        start = length - 1;
      }
      for (var k = start === -Infinity ? -1 : start; k >= 0; k--) {
        if (t[k] === target) return k;
      }
      return -1;
    };
    ARRAY_METHODS.includes = function (i, t, a) {
      var target = arg(a, 0);
      for (var k = relative(arg(a, 1), t.length, 0); k < t.length; k++) {
        if (sameValueZero(t[k], target)) return true;
      }
      return false;
    };
    ARRAY_METHODS.find = function (i, t, a) {
      var found;
      iterateCallback(i, t, a, function (index, value, result) {
        if (truthy(result)) {
          found = value;
          return true;
        }
        return false;
      });
      return found;
    };
    ARRAY_METHODS.findIndex = function (i, t, a) {
      var found = -1;
      iterateCallback(i, t, a, function (index, value, result) {
        if (truthy(result)) {
          found = index;
          return true;
        }
        return false;
      });
      return found;
    };
    ARRAY_METHODS.findLast = function (i, t, a) { return findFromEnd(i, t, a, false); };
    ARRAY_METHODS.findLastIndex = function (i, t, a) { return findFromEnd(i, t, a, true); };
    ARRAY_METHODS.filter = function (i, t, a) {
      var out = [];
      iterateCallback(i, t, a, function (index, value, result) {
        if (truthy(result)) out.push(value);
        return false;
      });
      return out;
    };
    ARRAY_METHODS.map = function (i, t, a) {
      var out = [];
      iterateCallback(i, t, a, function (index, value, result) {
        out.push(result);
        return false;
      });
      return out;
    };
    ARRAY_METHODS.forEach = function (i, t, a) {
      iterateCallback(i, t, a, function () { return false; });
      return undefined;
    };
    ARRAY_METHODS.some = function (i, t, a) {
      return iterateCallback(i, t, a, function (index, value, result) { return truthy(result); });
    };
    ARRAY_METHODS.every = function (i, t, a) {
      return !iterateCallback(i, t, a, function (index, value, result) { return !truthy(result); });
    };
    ARRAY_METHODS.reduce = function (i, t, a) { return reduceList(i, t, a, false); };
    ARRAY_METHODS.reduceRight = function (i, t, a) { return reduceList(i, t, a, true); };
    ARRAY_METHODS.flat = function (i, t, a) {
      var depth = arg(a, 0);
      return flatten(t, depth === undefined ? 1 : toInteger(depth), []);
    };
    ARRAY_METHODS.flatMap = function (i, t, a) { return flatten(ARRAY_METHODS.map(i, t, a), 1, []); };
    ARRAY_METHODS.fill = function (i, t, a) {
      var start = relative(arg(a, 1), t.length, 0);
      var end = relative(arg(a, 2), t.length, t.length);
      for (var k = start; k < end; k++) t[k] = arg(a, 0);
      return t;
    };
    ARRAY_METHODS.at = function (i, t, a) {
      var index = toInteger(arg(a, 0));
      if (index < 0) index += t.length;
      if (index < 0 || index >= t.length) return undefined;
      return t[index];
    };
    ARRAY_METHODS.keys = function (i, t) { return t.map(function (value, index) { return index; }); };
    ARRAY_METHODS.values = function (i, t) { return t.slice(); };
    ARRAY_METHODS.entries = function (i, t) { return t.map(function (value, index) { return [index, value]; }); };
    ARRAY_METHODS.toString = function (i, t) { return toString(t); };

    function noRegex(value, name) {
      if (value instanceof SRegExp) throw typeError("First argument to String.prototype." + name + " must not be a regular expression");
    }

    function padText(text, args, atStart) {
      var target = toInteger(arg(args, 0));
      var filler = arg(args, 1);
      filler = filler === undefined ? " " : toString(filler);
      var length = slen(text);
      if (target <= length || !filler) return text;
      if (target > MAX_STRING) throw rangeError("Invalid string length");
      var needed = target - length;
      var fillLength = slen(filler);
      var repeated = new Array(Math.floor(needed / fillLength) + 2).join(filler);
      var padding = cslice(repeated, 0, needed);
      return atStart ? padding + text : text + padding;
    }

    function asRegExp(interp, value, flags) {
      if (value instanceof SRegExp) return value;
      var source = value === undefined ? "(?:)" : toString(value);
      var compiled;
      try {
        compiled = compileRegex(source, flags || "", interp.line);
      } catch (error) {
        if (error instanceof ScriptError) throw new Thrown(errorObject("SyntaxError", error.text));
        throw error;
      }
      return new SRegExp(source, flags || "", compiled);
    }

    function replaceOne(interp, subject, match, groups, named, replacement) {
      if (isCallable(replacement)) {
        var callArgs = [match.text].concat(groups).concat([match.start, subject]);
        if (named !== null && named !== undefined) callArgs.push(named);
        return toString(interp.call(replacement, undefined, callArgs));
      }
      return expandReplacement(toString(replacement), match.text, match.u0, subject, groups, named);
    }

    function replaceText(interp, text, args, all) {
      var pattern = arg(args, 0);
      var replacement = arg(args, 1);
      var out = [];
      var last = 0;
      var matches;
      var k;
      if (pattern instanceof SRegExp) {
        if (all && pattern.flags.indexOf("g") < 0) throw typeError("replaceAll must be called with a global RegExp");
        if (pattern.flags.indexOf("g") >= 0) {
          matches = regexAll(pattern, text);
          pattern.lastIndex = 0;
        } else {
          var single = rxSearch(pattern.compiled, text, 0);
          matches = single ? [single] : [];
        }
        for (k = 0; k < matches.length; k++) {
          var match = matches[k];
          out.push(text.slice(last, match.u0));
          out.push(replaceOne(interp, text, match, match.groups, namedGroups(pattern, match), replacement));
          last = match.u1;
        }
        out.push(text.slice(last));
        return checkString(out.join(""));
      }
      var needle = toString(pattern);
      var positions = [];
      if (all) {
        var start = 0;
        var step = Math.max(1, needle.length);
        while (true) {
          var found = text.indexOf(needle, start);
          if (found < 0) break;
          positions.push(found);
          start = found + step;
          if (start > text.length) break;
        }
      } else {
        var first = text.indexOf(needle);
        if (first >= 0) positions.push(first);
      }
      for (k = 0; k < positions.length; k++) {
        var at = positions[k];
        out.push(text.slice(last, at));
        out.push(replaceOne(interp, text, { text: needle, u0: at, start: toPoint(text, at) }, [], null, replacement));
        last = at + needle.length;
      }
      out.push(text.slice(last));
      return checkString(out.join(""));
    }

    function splitText(interp, text, args) {
      var separator = arg(args, 0);
      var limit = arg(args, 1);
      limit = limit === undefined ? 4294967295 : toUint32(limit);
      if (limit === 0) return [];
      if (separator === undefined) return [text];
      if (separator instanceof SRegExp) {
        var size = slen(text);
        if (size === 0) return rxMatch(separator.compiled, text, 0) ? [] : [text];
        var out = [];
        var p = 0;
        var q = 0;
        while (q < size) {
          var match = rxMatch(separator.compiled, text, q);
          if (match === null) {
            q += 1;
            continue;
          }
          var end = Math.min(match.end, size);
          if (end === p) {
            q += 1;
            continue;
          }
          out.push(cslice(text, p, q));
          if (out.length >= limit) return out;
          p = end;
          for (var g = 0; g < match.groups.length; g++) {
            out.push(match.groups[g]);
            if (out.length >= limit) return out;
          }
          checkArray(out);
          q = p;
        }
        out.push(cslice(text, p));
        return out;
      }
      separator = toString(separator);
      if (separator === "") return checkArray(Array.from(text)).slice(0, limit);
      return checkArray(text.split(separator)).slice(0, limit);
    }

    function stringMethod(fn) {
      return function (interp, self, args) {
        if (self === null || self === undefined) throw typeError("String.prototype method called on null or undefined");
        return fn(interp, typeof self === "string" ? self : toString(self), args);
      };
    }

    var STRING_METHODS = dict();
    var stringTable = {
      charAt: function (i, t, a) {
        var index = toInteger(arg(a, 0));
        return index >= 0 && index < slen(t) ? charAt(t, index) : "";
      },
      codePointAt: function (i, t, a) {
        var index = toInteger(arg(a, 0));
        return index >= 0 && index < slen(t) ? charAt(t, index).codePointAt(0) : NaN;
      },
      at: function (i, t, a) {
        var index = toInteger(arg(a, 0));
        var length = slen(t);
        if (index < 0) index += length;
        return index >= 0 && index < length ? charAt(t, index) : undefined;
      },
      indexOf: function (i, t, a) {
        return cfind(t, toString(arg(a, 0)), clampIndex(arg(a, 1), slen(t)));
      },
      lastIndexOf: function (i, t, a) {
        var needle = toString(arg(a, 0));
        var position = toNumber(arg(a, 1));
        var length = slen(t);
        var start = position !== position ? length : clampIndex(position, length);
        var found = t.lastIndexOf(needle, toUnit(t, start));
        return found < 0 ? -1 : toPoint(t, found);
      },
      includes: function (i, t, a) {
        noRegex(arg(a, 0), "includes");
        return cfind(t, toString(arg(a, 0)), clampIndex(arg(a, 1), slen(t))) >= 0;
      },
      startsWith: function (i, t, a) {
        noRegex(arg(a, 0), "startsWith");
        return t.startsWith(toString(arg(a, 0)), toUnit(t, clampIndex(arg(a, 1), slen(t))));
      },
      endsWith: function (i, t, a) {
        noRegex(arg(a, 0), "endsWith");
        var end = arg(a, 1) === undefined ? slen(t) : clampIndex(a[1], slen(t));
        return cslice(t, 0, end).endsWith(toString(arg(a, 0)));
      },
      slice: function (i, t, a) {
        var length = slen(t);
        var start = relative(arg(a, 0), length, 0);
        var end = relative(arg(a, 1), length, length);
        return start < end ? cslice(t, start, end) : "";
      },
      substring: function (i, t, a) {
        var length = slen(t);
        var start = clampIndex(arg(a, 0), length);
        var end = arg(a, 1) === undefined ? length : clampIndex(a[1], length);
        if (start > end) {
          var swap = start;
          start = end;
          end = swap;
        }
        return cslice(t, start, end);
      },
      substr: function (i, t, a) {
        var length = slen(t);
        var start = relative(arg(a, 0), length, 0);
        var count = arg(a, 1) === undefined ? length - start : toInteger(a[1]);
        count = count !== Infinity ? Math.max(0, Math.min(count, length - start)) : length - start;
        return cslice(t, start, start + count);
      },
      toUpperCase: function (i, t) { return checkString(t.toUpperCase()); },
      toLowerCase: function (i, t) { return t.toLowerCase(); },
      toLocaleUpperCase: function (i, t) { return checkString(t.toUpperCase()); },
      toLocaleLowerCase: function (i, t) { return t.toLowerCase(); },
      trim: function (i, t) { return t.trim(); },
      trimStart: function (i, t) { return t.replace(/^[\t\n\u000b\u000c\r \u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff]+/, ""); },
      trimEnd: function (i, t) { return t.replace(/[\t\n\u000b\u000c\r \u00a0\u1680\u2000-\u200a\u2028\u2029\u202f\u205f\u3000\ufeff]+$/, ""); },
      padStart: function (i, t, a) { return padText(t, a, true); },
      padEnd: function (i, t, a) { return padText(t, a, false); },
      repeat: function (i, t, a) {
        var count = toInteger(arg(a, 0));
        if (count < 0 || count === Infinity) throw rangeError("Invalid count value: " + toString(toNumber(arg(a, 0))));
        if (slen(t) * count > MAX_STRING) throw rangeError("Invalid string length");
        return t.repeat(count);
      },
      split: function (i, t, a) { return splitText(i, t, a); },
      replace: function (i, t, a) { return replaceText(i, t, a, false); },
      replaceAll: function (i, t, a) { return replaceText(i, t, a, true); },
      match: function (i, t, a) {
        var regex = asRegExp(i, arg(a, 0), "");
        if (regex.flags.indexOf("g") < 0) return regexExec(regex, t);
        var matches = regexAll(regex, t);
        regex.lastIndex = 0;
        if (!matches.length) return null;
        return matches.map(function (match) { return match.text; });
      },
      matchAll: function (i, t, a) {
        var regex = arg(a, 0);
        if (regex instanceof SRegExp && regex.flags.indexOf("g") < 0) throw typeError("matchAll must be called with a global RegExp");
        regex = asRegExp(i, regex, "g");
        return regexAll(regex, t).map(function (match) { return matchArray(regex, match, t); });
      },
      search: function (i, t, a) {
        var regex = asRegExp(i, arg(a, 0), "");
        var match = rxSearch(regex.compiled, t, 0);
        return match ? match.start : -1;
      },
      concat: function (i, t, a) { return checkString(t + a.map(function (item) { return toString(item); }).join("")); },
      localeCompare: function (i, t, a) { return compareStrings(t, toString(arg(a, 0))); },
      normalize: function (i, t, a) {
        var form = arg(a, 0);
        form = form === undefined ? "NFC" : toString(form);
        if (["NFC", "NFD", "NFKC", "NFKD"].indexOf(form) < 0) throw rangeError("The normalization form should be one of NFC, NFD, NFKC, NFKD.");
        return t.normalize(form);
      },
      toString: function (i, t) { return t; },
      valueOf: function (i, t) { return t; }
    };
    stringTable.charCodeAt = stringTable.codePointAt;
    Object.keys(stringTable).forEach(function (name) { STRING_METHODS[name] = stringMethod(stringTable[name]); });

    var NUMBER_METHODS = dict();
    NUMBER_METHODS.toFixed = function (i, t, a) {
      var digits = toInteger(arg(a, 0));
      if (digits < 0 || digits > 100) throw rangeError("toFixed() digits argument must be between 0 and 100");
      if (t !== t) return "NaN";
      if (Math.abs(t) >= 1e21 || t === Infinity || t === -Infinity) return String(t);
      return t.toFixed(digits);
    };
    NUMBER_METHODS.toString = function (i, t, a) {
      var radix = arg(a, 0);
      radix = radix === undefined ? 10 : toInteger(radix);
      if (radix < 2 || radix > 36) throw rangeError("toString() radix must be between 2 and 36");
      if (radix === 10 || t !== t || t === Infinity || t === -Infinity) return String(t);
      if (!Number.isInteger(t)) throw rangeError("toString() with a radix needs a whole number");
      var number = BigInt(Math.abs(t));
      var base = BigInt(radix);
      var digits = "0123456789abcdefghijklmnopqrstuvwxyz";
      var out = "";
      while (true) {
        out = digits.charAt(Number(number % base)) + out;
        number = number / base;
        if (number === BigInt(0)) break;
      }
      return (t < 0 ? "-" : "") + out;
    };
    NUMBER_METHODS.valueOf = function (i, t) { return t; };

    function dateField(index) {
      return function (i, t) {
        if (t.ms !== t.ms) return NaN;
        return civilFromMs(t.ms)[index] - (index === 1 ? 1 : 0);
      };
    }

    function dateSetter(index) {
      return function (i, t, a) {
        if (t.ms !== t.ms && index !== 0) return NaN;
        var parts = civilFromMs(t.ms !== t.ms ? 0 : t.ms).slice(0, 7);
        parts[1] -= 1;
        var values = a.map(toNumber);
        if (!values.length) {
          t.ms = NaN;
          return NaN;
        }
        var limit = [3, 2, 1, 4, 3, 2, 1][index];
        for (var k = 0; k < values.length && k < limit; k++) parts[index + k] = values[k];
        t.ms = timeClip(dateFromParts(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5], parts[6]));
        return t.ms;
      };
    }

    var DATE_METHODS = dict();
    DATE_METHODS.getTime = function (i, t) { return t.ms; };
    DATE_METHODS.valueOf = function (i, t) { return t.ms; };
    DATE_METHODS.toISOString = function (i, t) {
      if (t.ms !== t.ms) throw rangeError("Invalid time value");
      return isoString(t.ms);
    };
    DATE_METHODS.toJSON = function (i, t) { return t.ms !== t.ms ? null : isoString(t.ms); };
    DATE_METHODS.toString = function (i, t) { return dateToString(t.ms); };
    DATE_METHODS.getTimezoneOffset = function (i, t) { return t.ms !== t.ms ? NaN : 0; };
    ["get", "getUTC"].forEach(function (prefix) {
      ["FullYear", "Month", "Date", "Hours", "Minutes", "Seconds", "Milliseconds", "Day"].forEach(function (name, index) {
        DATE_METHODS[prefix + name] = dateField(index);
      });
    });
    ["set", "setUTC"].forEach(function (prefix) {
      ["FullYear", "Month", "Date", "Hours", "Minutes", "Seconds", "Milliseconds"].forEach(function (name, index) {
        DATE_METHODS[prefix + name] = dateSetter(index);
      });
    });
    DATE_METHODS.setTime = function (i, t, a) {
      t.ms = timeClip(toNumber(arg(a, 0)));
      return t.ms;
    };

    var REGEXP_METHODS = dict();
    REGEXP_METHODS.test = function (i, t, a) { return regexExec(t, toString(arg(a, 0))) !== null; };
    REGEXP_METHODS.exec = function (i, t, a) { return regexExec(t, toString(arg(a, 0))); };
    REGEXP_METHODS.toString = function (i, t) { return "/" + t.source + "/" + t.flags; };

    function callableArg(interp, args) {
      var fn = arg(args, 0);
      if (!isCallable(fn)) throw typeError(interp.displayShort(fn) + " is not a function");
      return fn;
    }

    function collectionForEach(i, t, a) {
      var fn = callableArg(i, a);
      var pairs = [];
      t.map.forEach(function (value, key) { pairs.push([key, value]); });
      pairs.forEach(function (pair) { i.call(fn, arg(a, 1), [t.kind === "Map" ? pair[1] : pair[0], pair[0], t]); });
      return undefined;
    }

    var SET_METHODS = dict();
    SET_METHODS.add = function (i, t, a) {
      collectionPut(t, arg(a, 0), arg(a, 0));
      return t;
    };
    SET_METHODS.has = function (i, t, a) { return t.map.has(arg(a, 0)); };
    SET_METHODS["delete"] = function (i, t, a) { return t.map["delete"](arg(a, 0)); };
    SET_METHODS.clear = function (i, t) {
      t.map.clear();
      return undefined;
    };
    SET_METHODS.forEach = collectionForEach;
    SET_METHODS.values = function (i, t) { return Array.from(t.map.keys()); };
    SET_METHODS.keys = function (i, t) { return Array.from(t.map.keys()); };
    SET_METHODS.entries = function (i, t) { return Array.from(t.map.keys()).map(function (key) { return [key, key]; }); };
    var MAP_METHODS = dict();
    MAP_METHODS.get = function (i, t, a) { return t.map.has(arg(a, 0)) ? t.map.get(arg(a, 0)) : undefined; };
    MAP_METHODS.set = function (i, t, a) {
      collectionPut(t, arg(a, 0), arg(a, 1));
      return t;
    };
    MAP_METHODS.has = SET_METHODS.has;
    MAP_METHODS["delete"] = SET_METHODS["delete"];
    MAP_METHODS.clear = SET_METHODS.clear;
    MAP_METHODS.forEach = collectionForEach;
    MAP_METHODS.values = function (i, t) { return Array.from(t.map.values()); };
    MAP_METHODS.keys = function (i, t) { return Array.from(t.map.keys()); };
    MAP_METHODS.entries = function (i, t) { return collectionEntries(t); };

    function makeCollection(kind) {
      return new SNative(kind, function () {
        throw typeError("Constructor " + kind + " requires 'new'");
      }, dict(), function (interp, args) {
        var out = new SCollection(kind);
        var source = arg(args, 0);
        if (source === undefined || source === null) return out;
        var entries = interp.iterate(source, ["ident", 0, "iterable"]);
        for (var i = 0; i < entries.length; i++) {
          var entry = entries[i];
          if (kind === "Set") {
            collectionPut(out, entry, entry);
          } else {
            if (!Array.isArray(entry)) throw typeError("Iterator value " + toString(entry) + " is not an entry object");
            collectionPut(out, arg(entry, 0), arg(entry, 1));
          }
        }
        return out;
      });
    }

    var OBJECT_METHODS = dict();
    OBJECT_METHODS.hasOwnProperty = function (i, t, a) { return hasKey(t, propertyKey(arg(a, 0))); };
    OBJECT_METHODS.toString = function () { return "[object Object]"; };

    var FUNCTION_METHODS = dict();
    FUNCTION_METHODS.call = function (i, t, a) { return i.call(t, arg(a, 0), a.slice(1)); };
    FUNCTION_METHODS.apply = function (i, t, a) {
      var extra = arg(a, 1);
      if (extra === undefined || extra === null) extra = [];
      if (!Array.isArray(extra)) throw typeError("CreateListFromArrayLike called on non-object");
      return i.call(t, arg(a, 0), extra.slice());
    };

    function unaryMath(fn) {
      return new SNative("", function (i, t, a) {
        var value = toNumber(arg(a, 0));
        if (value !== value) return NaN;
        return fn(value);
      });
    }

    function makeMath() {
      var props = dict();
      props.PI = Math.PI;
      props.E = Math.E;
      props.LN2 = Math.LN2;
      props.LN10 = Math.LN10;
      props.LOG2E = Math.LOG2E;
      props.LOG10E = Math.LOG10E;
      props.SQRT2 = Math.SQRT2;
      props.SQRT1_2 = Math.SQRT1_2;
      ["abs", "floor", "ceil", "round", "trunc", "sign", "sqrt", "cbrt", "exp", "log", "log10", "log2", "sin", "cos", "tan", "asin",
        "acos", "atan"].forEach(function (name) {
        var fn = unaryMath(Math[name]);
        fn.name = name;
        props[name] = fn;
      });
      props.atan2 = new SNative("atan2", function (i, t, a) { return Math.atan2(toNumber(arg(a, 0)), toNumber(arg(a, 1))); });
      props.hypot = new SNative("hypot", function (i, t, a) { return Math.hypot.apply(Math, a.map(toNumber)); });
      props.pow = new SNative("pow", function (i, t, a) { return Math.pow(toNumber(arg(a, 0)), toNumber(arg(a, 1))); });
      props.min = new SNative("min", function (i, t, a) { return Math.min.apply(Math, a.map(toNumber)); });
      props.max = new SNative("max", function (i, t, a) { return Math.max.apply(Math, a.map(toNumber)); });
      props.random = new SNative("random", function (i) { return Number(i.random()); });
      return props;
    }

    function objectKeys(value, kind) {
      if (isDict(value)) {
        var keys = Object.keys(value);
        if (kind === "keys") return keys;
        if (kind === "values") return keys.map(function (key) { return value[key]; });
        return keys.map(function (key) { return [key, value[key]]; });
      }
      if (Array.isArray(value) || typeof value === "string") {
        var items = Array.isArray(value) ? value : Array.from(value);
        if (kind === "keys") return items.map(function (item, index) { return String(index); });
        if (kind === "values") return items.slice();
        return items.map(function (item, index) { return [String(index), item]; });
      }
      if (value === null || value === undefined) throw typeError("Cannot convert undefined or null to object");
      return [];
    }

    function makeObject() {
      var props = dict();
      ["keys", "values", "entries"].forEach(function (kind) {
        props[kind] = new SNative(kind, function (i, t, a) { return objectKeys(arg(a, 0), kind); });
      });
      props.assign = new SNative("assign", function (i, t, a) {
        var target = arg(a, 0);
        if (!isDict(target)) {
          if (target === null || target === undefined) throw typeError("Cannot convert undefined or null to object");
          return target;
        }
        a.slice(1).forEach(function (source) {
          if (isDict(source)) Object.keys(source).forEach(function (key) { target[key] = source[key]; });
          else if (Array.isArray(source)) source.forEach(function (item, index) { target[String(index)] = item; });
          else if (typeof source === "string") Array.from(source).forEach(function (item, index) { target[String(index)] = item; });
        });
        return target;
      });
      props.fromEntries = new SNative("fromEntries", function (i, t, a) {
        var out = dict();
        var entries = i.iterate(arg(a, 0), ["ident", 0, "entries"]);
        for (var k = 0; k < entries.length; k++) {
          if (!Array.isArray(entries[k])) throw typeError("Iterator value " + toString(entries[k]) + " is not an entry object");
          out[propertyKey(arg(entries[k], 0))] = arg(entries[k], 1);
        }
        return out;
      });
      props.freeze = new SNative("freeze", function (i, t, a) { return arg(a, 0); });
      props.hasOwn = new SNative("hasOwn", function (i, t, a) { return isDict(arg(a, 0)) && hasKey(a[0], propertyKey(arg(a, 1))); });
      var construct = function (interp, args) {
        var value = arg(args, 0);
        return isObjectLike(value) ? value : dict();
      };
      return new SNative("Object", function (i, t, a) { return construct(i, a); }, props, construct);
    }

    function newArray(interp, args) {
      if (args.length === 1 && typeof args[0] === "number") {
        var length = args[0];
        if (length < 0 || !Number.isInteger(length) || length > MAX_ARRAY) throw rangeError("Invalid array length");
        var out = [];
        for (var i = 0; i < length; i++) out.push(undefined);
        return out;
      }
      return args.slice();
    }

    function arrayFrom(interp, self, args) {
      var source = arg(args, 0);
      var fn = arg(args, 1);
      var items;
      if (Array.isArray(source)) {
        items = source.slice();
      } else if (typeof source === "string") {
        items = Array.from(source);
      } else if (source instanceof SCollection) {
        items = collectionEntries(source);
      } else if (isDict(source) && hasKey(source, "length")) {
        var length = toInteger(source.length);
        if (length > MAX_ARRAY) throw rangeError("Invalid array length");
        items = [];
        for (var i = 0; i < Math.max(0, length); i++) items.push(hasKey(source, String(i)) ? source[String(i)] : undefined);
      } else if (source === null || source === undefined) {
        throw typeError(toString(source) + " is not iterable");
      } else {
        items = [];
      }
      if (fn !== undefined) {
        if (!isCallable(fn)) throw typeError(interp.displayShort(fn) + " is not a function");
        items = items.map(function (item, index) { return interp.call(fn, undefined, [item, index]); });
      }
      return items;
    }

    function makeArray() {
      var props = dict();
      props.isArray = new SNative("isArray", function (i, t, a) { return Array.isArray(arg(a, 0)); });
      props.from = new SNative("from", arrayFrom);
      props.of = new SNative("of", function (i, t, a) { return a.slice(); });
      return new SNative("Array", function (i, t, a) { return newArray(i, a); }, props, newArray);
    }

    function parseFloatText(interp, self, args) {
      var text = stringTable.trimStart(interp, toString(arg(args, 0)));
      var match = FLOAT_PREFIX_RE.exec(text);
      if (!match) return NaN;
      var token = match[0];
      if (token.replace(/^[+-]+/, "") === "Infinity") return token.charAt(0) === "-" ? -Infinity : Infinity;
      return Number(token);
    }

    function parseIntText(interp, self, args) {
      var text = toString(arg(args, 0)).trim();
      var radix = toNumber(arg(args, 1)) | 0;
      var sign = 1;
      if (text.charAt(0) === "+" || text.charAt(0) === "-") {
        sign = text.charAt(0) === "-" ? -1 : 1;
        text = text.slice(1);
      }
      var stripPrefix = true;
      if (radix !== 0) {
        if (radix < 2 || radix > 36) return NaN;
        if (radix !== 16) stripPrefix = false;
      } else {
        radix = 10;
      }
      if (stripPrefix && text.slice(0, 2).toLowerCase() === "0x") {
        text = text.slice(2);
        radix = 16;
      }
      var digits = "0123456789abcdefghijklmnopqrstuvwxyz".slice(0, radix);
      var value = BigInt(0);
      var base = BigInt(radix);
      var end = 0;
      while (end < text.length && digits.indexOf(text.charAt(end).toLowerCase()) >= 0 && text.charAt(end).toLowerCase().length === 1) {
        value = value * base + BigInt(digits.indexOf(text.charAt(end).toLowerCase()));
        end += 1;
      }
      if (end === 0) return NaN;
      return sign * Number(value);
    }

    function makeNumber() {
      var props = dict();
      props.isInteger = new SNative("isInteger", function (i, t, a) { return typeof arg(a, 0) === "number" && Number.isInteger(a[0]); });
      props.isSafeInteger = new SNative("isSafeInteger", function (i, t, a) { return typeof arg(a, 0) === "number" && Number.isSafeInteger(a[0]); });
      props.isFinite = new SNative("isFinite", function (i, t, a) { return typeof arg(a, 0) === "number" && isFinite(a[0]); });
      props.isNaN = new SNative("isNaN", function (i, t, a) { return typeof arg(a, 0) === "number" && a[0] !== a[0]; });
      props.parseFloat = new SNative("parseFloat", parseFloatText);
      props.parseInt = new SNative("parseInt", parseIntText);
      props.MAX_SAFE_INTEGER = 9007199254740991;
      props.MIN_SAFE_INTEGER = -9007199254740991;
      props.EPSILON = Math.pow(2, -52);
      props.MAX_VALUE = 1.7976931348623157e308;
      props.MIN_VALUE = 5e-324;
      props.POSITIVE_INFINITY = Infinity;
      props.NEGATIVE_INFINITY = -Infinity;
      props.NaN = NaN;
      return new SNative("Number", function (i, t, a) { return a.length ? toNumber(a[0]) : 0; }, props);
    }

    function makeString() {
      var props = dict();
      props.fromCharCode = new SNative("fromCharCode", function (i, t, a) {
        return a.map(function (item) { return String.fromCharCode(toUint32(item) % 65536); }).join("");
      });
      props.fromCodePoint = new SNative("fromCodePoint", function (i, t, a) {
        return a.map(function (item) {
          var value = toNumber(item);
          if (value !== value || !Number.isInteger(value) || value < 0 || value > 0x10ffff) {
            throw rangeError("Invalid code point " + toString(item));
          }
          return value >= 0xd800 && value <= 0xdfff ? String.fromCharCode(value) : String.fromCodePoint(value);
        }).join("");
      });
      return new SNative("String", function (i, t, a) { return a.length ? toString(a[0]) : ""; }, props);
    }

    function makeError(name) {
      var construct = function (interp, args) {
        var message = arg(args, 0);
        return errorObject(name, message === undefined ? "" : toString(message));
      };
      return new SNative(name, function (i, t, a) { return construct(i, a); }, dict(), construct);
    }

    function newDate(interp, args) {
      if (!args.length) return new SDate(interp.nowMs);
      if (args.length === 1) {
        var value = args[0];
        if (value instanceof SDate) return new SDate(value.ms);
        value = toPrimitive(value);
        if (typeof value === "string") return new SDate(parseDateString(value));
        return new SDate(toNumber(value));
      }
      var numbers = args.slice(0, 7).map(toNumber);
      var year = numbers[0];
      if (year === year && Math.trunc(year) >= 0 && Math.trunc(year) <= 99) numbers[0] = 1900 + Math.trunc(year);
      return new SDate(dateFromParts.apply(null, numbers));
    }

    function dateUtc(interp, self, args) {
      var numbers = args.slice(0, 7).map(toNumber);
      if (!numbers.length) numbers = [NaN];
      var year = numbers[0];
      if (year === year && Math.trunc(year) >= 0 && Math.trunc(year) <= 99) numbers[0] = 1900 + Math.trunc(year);
      if (numbers.length === 1) numbers.push(0);
      return timeClip(dateFromParts.apply(null, numbers));
    }

    function makeDate() {
      var props = dict();
      props.now = new SNative("now", function (i) { return i.nowMs; });
      props.parse = new SNative("parse", function (i, t, a) { return parseDateString(toString(arg(a, 0))); });
      props.UTC = new SNative("UTC", dateUtc);
      return new SNative("Date", function (i) { return dateToString(i.nowMs); }, props, newDate);
    }

    function newRegExp(interp, args) {
      var pattern = arg(args, 0);
      var flags = arg(args, 1);
      var source;
      if (pattern instanceof SRegExp) {
        source = pattern.source;
        flags = flags === undefined ? pattern.flags : toString(flags);
      } else {
        source = pattern === undefined ? "(?:)" : toString(pattern);
        flags = flags === undefined ? "" : toString(flags);
      }
      var compiled;
      try {
        compiled = compileRegex(source, flags, interp.line);
      } catch (error) {
        if (error instanceof ScriptError) throw new Thrown(errorObject("SyntaxError", error.text));
        throw error;
      }
      return new SRegExp(source, flags, compiled);
    }

    function makeGlobals() {
      var consoleObject = dict();
      ["log", "info", "warn", "error", "debug"].forEach(function (name) {
        consoleObject[name] = new SNative(name, function (i, t, a) {
          i.log(a);
          return undefined;
        });
      });
      var json = dict();
      json.parse = new SNative("parse", function (i, t, a) { return jsonParse(toString(arg(a, 0))); });
      json.stringify = new SNative("stringify", function (i, t, a) {
        var space = arg(a, 2);
        var indent = "";
        if (typeof space === "number") indent = new Array(Math.max(0, Math.min(10, toInteger(space))) + 1).join(" ");
        else if (typeof space === "string") indent = cslice(space, 0, 10);
        return jsonStringify(arg(a, 0), indent);
      });
      var out = {};
      out.undefined = undefined;
      out.NaN = NaN;
      out.Infinity = Infinity;
      out.Math = makeMath();
      out.JSON = json;
      out.Object = makeObject();
      out.Array = makeArray();
      out.Number = makeNumber();
      out.String = makeString();
      out.Boolean = new SNative("Boolean", function (i, t, a) { return truthy(arg(a, 0)); });
      out.Date = makeDate();
      out.RegExp = new SNative("RegExp", function (i, t, a) { return newRegExp(i, a); }, dict(), newRegExp);
      out.Set = makeCollection("Set");
      out.Map = makeCollection("Map");
      out.Function = new SNative("Function", function () { throw typeError("Function is not a function"); });
      ["Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "URIError"].forEach(function (name) {
        out[name] = makeError(name);
      });
      out.parseInt = new SNative("parseInt", parseIntText);
      out.parseFloat = new SNative("parseFloat", parseFloatText);
      out.isNaN = new SNative("isNaN", function (i, t, a) { var n = toNumber(arg(a, 0)); return n !== n; });
      out.isFinite = new SNative("isFinite", function (i, t, a) { return isFinite(toNumber(arg(a, 0))); });
      out.encodeURIComponent = new SNative("encodeURIComponent", function (i, t, a) { return uriEncode(toString(arg(a, 0))); });
      out.decodeURIComponent = new SNative("decodeURIComponent", function (i, t, a) { return uriDecode(toString(arg(a, 0))); });
      out.console = consoleObject;
      return out;
    }

    function unwrapItems(value, fallback) {
      if (value === undefined) value = fallback;
      if (value === null) return [];
      if (isDict(value)) value = [value];
      if (!Array.isArray(value)) return [{ value: toJson(value) }];
      return value.map(function (item) {
        if (isDict(item) && isDict(item.json) && Object.keys(item).every(function (key) {
          return ["json", "binary", "pairedItem", "index"].indexOf(key) >= 0;
        })) {
          item = item.json;
        }
        var converted = toJson(item);
        return converted !== null && typeof converted === "object" && !Array.isArray(converted) ? converted : { value: converted };
      });
    }

    return {
      parse: parse,
      Interpreter: Interpreter,
      ScriptError: ScriptError,
      ScriptLimit: ScriptLimit,
      Thrown: Thrown,
      SNative: SNative,
      SDate: SDate,
      SRegExp: SRegExp,
      compileRegex: compileRegex,
      rxSearch: rxSearch,
      regexAll: regexAll,
      namedGroups: namedGroups,
      expandReplacement: expandReplacement,
      fromJson: fromJson,
      toJson: toJson,
      unwrapItems: unwrapItems,
      dict: dict,
      errorObject: errorObject,
      toString: toString,
      toNumber: toNumber,
      toInteger: toInteger,
      truthy: truthy,
      dateFromParts: dateFromParts,
      civilFromMs: civilFromMs,
      monthDays: monthDays,
      slen: slen,
      cslice: cslice,
      toPoint: toPoint,
      setOwn: setOwn
    };
  })();

  var Actions = (function () {
    var ASCII_SPACE = " \t\n\r\u000b\u000c";
    var PY_SPACE = "\t\n\u000b\u000c\r\u001c\u001d\u001e\u001f \u0085\u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029\u202f\u205f\u3000";
    var VARIABLE_RE = /^[A-Za-z_][A-Za-z0-9_]{0,63}$/;
    var OFFSET_RE = /^([+-])([0-9]{2}):?([0-9]{2})$/;
    var DATE_RE = /^([0-9]{4})-([0-9]{2})-([0-9]{2})(?:[T ]([0-9]{2}):([0-9]{2})(?::([0-9]{2})(?:\.([0-9]{1,9}))?)?)?[ ]*(Z|z|[+-][0-9]{2}:?[0-9]{2})?$/;
    var NUMBER_TEXT_RE = /^[+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$/;
    var URL_RE = /https?:\/\/[^ \t\n\r\u000b\u000c<>"'`]+/gi;
    var EMAIL_RE = /[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}/g;
    var NUMBER_RE = /-?[0-9]{1,3}(?:,[0-9]{3})+(?:\.[0-9]+)?|-?[0-9]+(?:\.[0-9]+)?/g;
    var PHONE_RE = /\+?[0-9][0-9 ().-]{5,}[0-9]/g;
    var HASHTAG_RE = /#[A-Za-z0-9_]+/g;
    var MENTION_RE = /@[A-Za-z0-9_]+/g;
    var URI_RE = /^(?:([A-Za-z][A-Za-z0-9+.-]*):)?(?:\/\/([^\/?#]*))?([^?#]*)(?:\?([^#]*))?(?:#([\s\S]*))?$/;
    var TOKEN_RE = /\[[^\]]*\]|YYYY|YY|MMMM|MMM|MM|M|DD|D|dddd|ddd|HH|H|hh|h|mm|m|ss|s|SSS|A|a|ZZ|Z|X|x/g;
    var HTML_ENTITY_RE = /&(#[0-9]{1,7}|#[xX][0-9A-Fa-f]{1,6}|[A-Za-z]+);/g;
    var ZONE_NAME_RE = /^[A-Za-z][A-Za-z0-9_+-]*(?:\/[A-Za-z0-9_+-]+)*$/;
    var MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November",
      "December"];
    var WEEKDAYS = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
    var UNIT_MS = { seconds: 1000, minutes: 60000, hours: 3600000, days: 86400000, weeks: 604800000 };
    var MAX_TEXT = 1000000;
    var DATE_MIN = -62135596800000;
    var DATE_MAX = 253402300799999;
    var HTML_NAMED = { amp: "&", lt: "<", gt: ">", quot: "\"", apos: "'", nbsp: "\u00a0" };
    var formatters = {};

    function StopRun(out) {
      this.out = out;
    }

    function pyStrip(text, chars) {
      chars = chars || PY_SPACE;
      var cps = Array.from(text);
      var start = 0;
      var end = cps.length;
      while (start < end && chars.indexOf(cps[start]) >= 0) start++;
      while (end > start && chars.indexOf(cps[end - 1]) >= 0) end--;
      return cps.slice(start, end).join("");
    }

    function numberOut(value) {
      return value === 0 ? 0 : value;
    }

    function toNumber(value, label) {
      if (typeof value === "boolean" || value === null || value === undefined) fail(label + " must be a number");
      var number;
      if (typeof value === "number") {
        number = value;
      } else {
        var text = pyStrip(pyStr(value));
        if (!NUMBER_TEXT_RE.test(text)) fail(label + " must be a number");
        number = Number(text);
      }
      if (number !== number || number === Infinity || number === -Infinity) fail(label + " must be a number");
      return number;
    }

    function copyJson(value) {
      if (Array.isArray(value)) return value.map(copyJson);
      if (isObject(value)) {
        var out = {};
        Object.keys(value).forEach(function (key) { Script.setOwn(out, key, copyJson(value[key])); });
        return out;
      }
      return value;
    }

    function withField(item, field, value) {
      var out = copy(item);
      var parts = pyStr(field).split(".");
      var target = out;
      for (var i = 0; i < parts.length - 1; i++) {
        var next = get(target, parts[i]);
        next = isObject(next) ? copy(next) : {};
        target[parts[i]] = next;
        target = next;
      }
      target[parts[parts.length - 1]] = value === undefined ? null : value;
      return out;
    }

    function upper(ch) {
      return ch.toUpperCase();
    }

    function changeCase(text, mode) {
      if (mode === "upper") return text.toUpperCase();
      if (mode === "lower") return text.toLowerCase();
      var cps = Array.from(text);
      if (mode === "capitalize") return cps.length ? upper(cps[0]) + cps.slice(1).join("") : "";
      var out = "";
      var previousSpace = true;
      cps.forEach(function (ch) {
        out += previousSpace ? upper(ch) : ch;
        previousSpace = ASCII_SPACE.indexOf(ch) >= 0;
      });
      return out;
    }

    function regex(pattern, ignoreCase) {
      var flags = "g" + (ignoreCase ? "i" : "");
      var compiled;
      try {
        compiled = Script.compileRegex(pattern, flags, 0);
      } catch (error) {
        if (error instanceof Script.ScriptError) fail("Invalid regular expression " + pattern);
        throw error;
      }
      return new Script.SRegExp(pattern, "g", compiled);
    }

    function escapePattern(text) {
      return Array.from(text).map(function (ch) { return "\\^$.|?*+()[]{}/".indexOf(ch) >= 0 ? "\\" + ch : ch; }).join("");
    }

    function replaceText(text, find, replacement, useRegex, ignoreCase) {
      if (!find) fail("Find is required");
      if (!useRegex && !ignoreCase) return text.split(find).join(replacement);
      var pattern = regex(useRegex ? find : escapePattern(find), ignoreCase);
      var template = useRegex ? replacement : replacement.split("$").join("$$");
      var out = [];
      var last = 0;
      Script.regexAll(pattern, text).forEach(function (match) {
        out.push(text.slice(last, match.u0));
        out.push(Script.expandReplacement(template, match.text, match.u0, text, match.groups, Script.namedGroups(pattern, match)));
        last = match.u1;
      });
      out.push(text.slice(last));
      var result = out.join("");
      if (Script.slen(result) > MAX_TEXT) fail("The text is too long");
      return result;
    }

    function matchText(text, pattern, mode, ignoreCase) {
      var compiled = regex(pattern, ignoreCase);
      if (mode === "test") return Script.rxSearch(compiled.compiled, text, 0) !== null;
      if (mode === "all") return Script.regexAll(compiled, text).map(function (match) { return match.text; });
      var match = Script.rxSearch(compiled.compiled, text, 0);
      if (mode === "first") return match ? match.text : "";
      if (!match) return [];
      return match.groups.map(function (group) { return group === undefined ? "" : group; });
    }

    function splitText(text, separator, custom, skipEmpty) {
      var parts;
      if (separator === "newlines") parts = text.split(/\r\n|\n|\r/);
      else if (separator === "spaces") parts = text.split(/[ \t\n\r\u000b\u000c]+/);
      else if (separator === "commas") parts = text.split(",").map(function (part) { return pyStrip(part, ASCII_SPACE); });
      else if (separator === "characters") parts = Array.from(text);
      else {
        if (!custom) fail("Enter a custom separator");
        parts = text.split(custom);
      }
      if (skipEmpty) parts = parts.filter(function (part) { return part !== ""; });
      return parts;
    }

    function combineText(value, separator, custom) {
      var joiner = { newlines: "\n", spaces: " ", commas: ", ", none: "", custom: custom }[separator];
      var items = Array.isArray(value) ? value : (value === null || value === undefined || value === "" ? [] : [value]);
      var result = items.map(function (item) { return stringify(item); }).join(joiner);
      if (Script.slen(result) > MAX_TEXT) fail("The text is too long");
      return result;
    }

    function base64Decode(text) {
      var clean = Array.from(text).filter(function (ch) { return ASCII_SPACE.indexOf(ch) < 0; }).join("");
      clean = clean.split("-").join("+").split("_").join("/").replace(/=+$/, "");
      if (clean.length % 4 === 1 || /[^A-Za-z0-9+\/]/.test(clean)) fail("The text is not valid Base64");
      return utf8Decode(base64DecodeBytes(clean), false);
    }

    function urlEncode(text) {
      return utf8(text).map(function (byte) {
        var ch = String.fromCharCode(byte);
        return /^[A-Za-z0-9_.~-]$/.test(ch) ? ch : "%" + (byte < 16 ? "0" : "") + byte.toString(16).toUpperCase();
      }).join("");
    }

    function percentDecode(text, plus) {
      var cps = Array.from(text);
      var data = [];
      var out = "";
      var index = 0;
      while (index < cps.length) {
        var ch = cps[index];
        var pair = cps.slice(index + 1, index + 3).join("");
        if (ch === "%" && pair.length === 2 && /^[0-9a-fA-F]{2}$/.test(pair)) {
          data.push(parseInt(pair, 16));
          index += 3;
          continue;
        }
        if (data.length) {
          out += utf8Decode(data, false);
          data = [];
        }
        out += plus && ch === "+" ? " " : ch;
        index += 1;
      }
      if (data.length) out += utf8Decode(data, false);
      return out;
    }

    function htmlEscape(text) {
      return text.split("&").join("&amp;").split("<").join("&lt;").split(">").join("&gt;").split("\"").join("&quot;")
        .split("'").join("&#x27;");
    }

    function htmlUnescape(text) {
      return text.replace(HTML_ENTITY_RE, function (all, body) {
        if (body.charAt(0) === "#") {
          var code = body.charAt(1) === "x" || body.charAt(1) === "X" ? parseInt(body.slice(2), 16) : parseInt(body.slice(1), 10);
          if (code === 0 || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return all;
          return String.fromCodePoint(code);
        }
        return has(HTML_NAMED, body) ? HTML_NAMED[body] : all;
      });
    }

    function encodeText(text, method) {
      if (method === "base64Encode") return base64(text);
      if (method === "base64Decode") return base64Decode(text);
      if (method === "urlEncode") return urlEncode(text);
      if (method === "urlDecode") return percentDecode(text, false);
      if (method === "htmlEscape") return htmlEscape(text);
      return htmlUnescape(text);
    }

    function hashText(text, algorithm, key, encoding) {
      if (!has(HASHES, algorithm)) fail("Unknown hash algorithm " + pyStr(algorithm));
      var data = utf8(text);
      var digest = key ? hmacBytes(algorithm, utf8(key), data) : HASHES[algorithm][0](data);
      return encoding === "base64" ? base64Bytes(digest) : hexBytes(digest);
    }

    function trimUrl(url) {
      url = url.replace(/[.,;:!?'"]+$/, "");
      while (url.charAt(url.length - 1) === ")" && url.split("(").length < url.split(")").length) {
        url = url.slice(0, -1).replace(/[.,;:!?'"]+$/, "");
      }
      return url;
    }

    function allMatches(re, text) {
      var out = [];
      re.lastIndex = 0;
      var found;
      while ((found = re.exec(text)) !== null) {
        out.push(found);
        if (found[0] === "") re.lastIndex += 1;
      }
      return out;
    }

    function guarded(re, text, before) {
      var out = [];
      re.lastIndex = 0;
      var found;
      while ((found = re.exec(text)) !== null) {
        var previous = found.index > 0 ? text.charAt(found.index - 1) : "";
        if (previous && before.test(previous)) {
          re.lastIndex = found.index + 1;
          continue;
        }
        out.push(found[0]);
      }
      return out;
    }

    function extract(text, kind) {
      if (kind === "urls") return allMatches(URL_RE, text).map(function (m) { return trimUrl(m[0]); });
      if (kind === "emails") return allMatches(EMAIL_RE, text).map(function (m) { return m[0]; });
      if (kind === "numbers") return allMatches(NUMBER_RE, text).map(function (m) { return numberOut(Number(m[0].split(",").join(""))); });
      if (kind === "phones") {
        return allMatches(PHONE_RE, text).map(function (m) { return m[0]; }).filter(function (found) {
          var digits = found.replace(/[^0-9]/g, "").length;
          return digits >= 7 && digits <= 15;
        });
      }
      if (kind === "hashtags") return guarded(HASHTAG_RE, text, /[A-Za-z0-9_&]/);
      return guarded(MENTION_RE, text, /[A-Za-z0-9._%+-]/);
    }

    function count(value, what) {
      if (what === "items") {
        if (Array.isArray(value)) return value.length;
        if (isObject(value)) return Object.keys(value).length;
        return value === null || value === undefined || value === "" ? 0 : 1;
      }
      var text = stringify(value);
      if (what === "characters") return Array.from(text).length;
      if (what === "words") return text.split(/[ \t\n\r\u000b\u000c]+/).filter(function (part) { return part; }).length;
      if (!text) return 0;
      var lines = text.split(/\r\n|\n|\r/);
      if (lines.length && lines[lines.length - 1] === "") lines.pop();
      return lines.length;
    }

    function pyMod(a, b) {
      var result = a % b;
      if (result !== 0 && (b < 0) !== (result < 0)) result += b;
      return result;
    }

    function calculate(a, operator, b) {
      if (operator === "sqrt") {
        if (a < 0) fail("Cannot take the square root of a negative number");
        return Math.sqrt(a);
      }
      if (operator === "abs") return Math.abs(a);
      if (b === null) fail("Second number must be a number");
      var result;
      if (operator === "add") result = a + b;
      else if (operator === "subtract") result = a - b;
      else if (operator === "multiply") result = a * b;
      else if (operator === "divide") {
        if (b === 0) fail("Cannot divide by zero");
        result = a / b;
      } else if (operator === "modulo") {
        if (b === 0) fail("Cannot divide by zero");
        result = pyMod(a, b);
      } else if (operator === "power") {
        result = Math.pow(a, b);
        if (result !== result) fail("The result is not a number");
      } else if (operator === "min") result = a <= b ? a : b;
      else result = a >= b ? a : b;
      if (result === Infinity || result === -Infinity) fail("The result is too large");
      return result;
    }

    function decimalOf(value) {
      var text = String(Math.abs(value));
      var negative = value < 0 || (value === 0 && 1 / value < 0);
      var exponent = 0;
      var e = text.indexOf("e");
      if (e >= 0) {
        exponent = parseInt(text.slice(e + 1), 10);
        text = text.slice(0, e);
      }
      var point = text.indexOf(".");
      if (point >= 0) {
        exponent -= text.length - point - 1;
        text = text.slice(0, point) + text.slice(point + 1);
      }
      return { negative: negative, coefficient: BigInt(text), exponent: exponent };
    }

    function pow10(n) {
      return BigInt(10) ** BigInt(n);
    }

    function quantize(dec, target, mode) {
      if (dec.exponent >= target) {
        return { negative: dec.negative, coefficient: dec.coefficient * pow10(dec.exponent - target), exponent: target };
      }
      var divisor = pow10(target - dec.exponent);
      var quotient = dec.coefficient / divisor;
      var remainder = dec.coefficient % divisor;
      var up = false;
      if (remainder !== BigInt(0)) {
        if (mode === "halfUp") up = remainder * BigInt(2) >= divisor;
        else if (mode === "ceiling") up = !dec.negative;
        else if (mode === "floor") up = dec.negative;
      }
      if (up) quotient += BigInt(1);
      return { negative: dec.negative, coefficient: quotient, exponent: target };
    }

    function decimalNumber(dec) {
      return Number((dec.negative ? "-" : "") + dec.coefficient.toString() + "e" + dec.exponent) + 0;
    }

    function roundNumber(value, decimals, mode) {
      var places = Math.trunc(Math.max(-15, Math.min(15, decimals)));
      var rounding = mode === "up" ? "ceiling" : (mode === "down" ? "floor" : "halfUp");
      return decimalNumber(quantize(decimalOf(value), -places, rounding));
    }

    function formatNumber(value, decimals, grouping) {
      var places = Math.trunc(Math.max(0, Math.min(15, decimals)));
      var dec = quantize(decimalOf(value), -places, "halfUp");
      var digits = dec.coefficient.toString();
      while (digits.length <= places) digits = "0" + digits;
      var whole = places ? digits.slice(0, digits.length - places) : digits;
      var fraction = places ? digits.slice(digits.length - places) : "";
      if (grouping) {
        var groups = [];
        while (whole.length > 3) {
          groups.unshift(whole.slice(-3));
          whole = whole.slice(0, -3);
        }
        groups.unshift(whole);
        whole = groups.join(",");
      }
      var result = whole + (fraction ? "." + fraction : "");
      if (dec.negative && /[1-9]/.test(digits)) result = "-" + result;
      return result;
    }

    function statistics(values, operation) {
      var numbers = [];
      values.forEach(function (value) {
        (Array.isArray(value) ? value : [value]).forEach(function (entry) {
          if (typeof entry === "boolean" || entry === null || entry === undefined) return;
          if (typeof entry === "number") numbers.push(entry);
          else if (typeof entry === "string" && NUMBER_TEXT_RE.test(pyStrip(entry))) numbers.push(Number(pyStrip(entry)));
        });
      });
      if (operation === "count") return numbers.length;
      var total = 0;
      numbers.forEach(function (number) { total += number; });
      if (operation === "sum") return total;
      if (!numbers.length) fail("There are no numbers to calculate with");
      if (operation === "average") return total / numbers.length;
      var ordered = numbers.slice().sort(function (a, b) { return a - b; });
      if (operation === "median") {
        var middle = Math.floor(ordered.length / 2);
        if (ordered.length % 2) return ordered[middle];
        return (ordered[middle - 1] + ordered[middle]) / 2;
      }
      if (operation === "min") return ordered[0];
      if (operation === "max") return ordered[ordered.length - 1];
      if (operation === "range") return ordered[ordered.length - 1] - ordered[0];
      if (numbers.length < 2) fail("A standard deviation needs at least two numbers");
      var mean = total / numbers.length;
      var squares = 0;
      numbers.forEach(function (number) { squares += (number - mean) * (number - mean); });
      return Math.sqrt(squares / (numbers.length - 1));
    }

    function sortKey(value) {
      if (value === null || value === undefined) return null;
      if (typeof value === "number") return [0, value, ""];
      return [1, 0, stringify(value)];
    }

    function compareKeys(a, b) {
      if (a[0] !== b[0]) return a[0] < b[0] ? -1 : 1;
      if (a[1] !== b[1]) return a[1] < b[1] ? -1 : 1;
      return codePointCompare(a[2], b[2]);
    }

    function codePointCompare(a, b) {
      var x = Array.from(a);
      var y = Array.from(b);
      for (var i = 0; i < x.length && i < y.length; i++) {
        var p = x[i].codePointAt(0);
        var q = y[i].codePointAt(0);
        if (p !== q) return p < q ? -1 : 1;
      }
      return x.length < y.length ? -1 : (x.length > y.length ? 1 : 0);
    }

    function stableSort(items, compare) {
      return items.map(function (item, index) { return [item, index]; }).sort(function (a, b) {
        return compare(a[0], b[0]) || a[1] - b[1];
      }).map(function (pair) { return pair[0]; });
    }

    function sortItems(items, keys, order, random) {
      if (order === "reverse") return items.slice().reverse();
      if (order === "shuffle") {
        var out = items.slice();
        for (var index = out.length - 1; index > 0; index--) {
          var other = Math.floor(random() * (index + 1));
          var swap = out[index];
          out[index] = out[other];
          out[other] = swap;
        }
        return out;
      }
      var present = [];
      var missing = [];
      items.forEach(function (item, i) {
        if (keys[i] === null) missing.push(item);
        else present.push([keys[i], item]);
      });
      var sign = order === "descending" ? -1 : 1;
      present = stableSort(present, function (a, b) { return sign * compareKeys(a[0], b[0]); });
      return present.map(function (pair) { return pair[1]; }).concat(missing);
    }

    function parseOffset(text) {
      var match = OFFSET_RE.exec(text);
      if (!match) return null;
      var hours = parseInt(match[2], 10);
      var minutes = parseInt(match[3], 10);
      if (hours > 23 || minutes > 59) return null;
      var total = hours * 60 + minutes;
      return match[1] === "+" ? total : -total;
    }

    function checkRange(ms) {
      if (!(ms >= DATE_MIN && ms <= DATE_MAX)) fail("The date is out of range");
      return ms;
    }

    function formatter(name) {
      if (!has(formatters, name)) {
        formatters[name] = new Intl.DateTimeFormat("en-US", {
          timeZone: name, hourCycle: "h23", year: "numeric", month: "numeric", day: "numeric", hour: "numeric",
          minute: "numeric", second: "numeric", era: "short"
        });
      }
      return formatters[name];
    }

    function Zone(text) {
      text = pyStrip(pyStr(text === null || text === undefined ? "" : text));
      this.name = text;
      this.fixed = null;
      if (!text || ["UTC", "Z", "GMT"].indexOf(text.toUpperCase()) >= 0) {
        this.fixed = 0;
        return;
      }
      var offset = parseOffset(text);
      if (offset !== null) {
        this.fixed = offset;
        return;
      }
      if (!ZONE_NAME_RE.test(text)) fail("Unknown time zone " + text);
      var resolved;
      try {
        resolved = formatter(text).resolvedOptions().timeZone;
      } catch (error) {
        fail("Unknown time zone " + text);
      }
      if (resolved !== text && String(resolved).toLowerCase() === text.toLowerCase()) fail("Unknown time zone " + text);
    }

    Zone.prototype.offsetSeconds = function (ms) {
      var instant = Math.floor(ms / 1000) * 1000;
      var parts = {};
      formatter(this.name).formatToParts(new Date(instant)).forEach(function (part) { parts[part.type] = part.value; });
      var year = parseInt(parts.year, 10);
      if (parts.era && /^B/.test(parts.era)) year = 1 - year;
      var hour = parseInt(parts.hour, 10) % 24;
      var wall = Script.dateFromParts(year, parseInt(parts.month, 10) - 1, parseInt(parts.day, 10), hour,
        parseInt(parts.minute, 10), parseInt(parts.second, 10), 0);
      return (wall - instant) / 1000;
    };

    Zone.prototype.offsetMinutes = function (ms) {
      if (this.fixed !== null) return this.fixed;
      checkRange(ms);
      return Math.floor(this.offsetSeconds(ms) / 60);
    };

    Zone.prototype.localToUtc = function (localMs) {
      if (this.fixed !== null) return localMs - this.fixed * 60000;
      checkRange(localMs);
      var seconds = Math.floor(localMs / 1000) * 1000;
      var before = this.offsetSeconds(seconds - 50400000);
      var after = this.offsetSeconds(seconds + 50400000);
      var chosen = before;
      if (before !== after) {
        if (this.offsetSeconds(seconds - before * 1000) === before) chosen = before;
        else if (this.offsetSeconds(seconds - after * 1000) === after) chosen = after;
      }
      return localMs - chosen * 1000;
    };

    function parseDate(value, zone) {
      if (typeof value === "boolean" || value === null || value === undefined || value === "") {
        fail("Cannot read the date " + stringify(value));
      }
      var number;
      if (typeof value === "number") {
        number = value;
      } else {
        var text = pyStrip(pyStr(value));
        if (NUMBER_TEXT_RE.test(text)) {
          number = Number(text);
        } else {
          var match = DATE_RE.exec(text);
          if (!match) fail("Cannot read the date " + Script.cslice(text, 0, 80));
          var year = parseInt(match[1], 10);
          var month = parseInt(match[2], 10);
          var day = parseInt(match[3], 10);
          var hours = parseInt(match[4] || "0", 10);
          var minutes = parseInt(match[5] || "0", 10);
          var seconds = parseInt(match[6] || "0", 10);
          var millis = parseInt(((match[7] || "0").slice(0, 3) + "000").slice(0, 3), 10);
          if (!(month >= 1 && month <= 12 && day >= 1 && day <= Script.monthDays(year, month) && hours <= 23 && minutes <= 59 &&
              seconds <= 59)) {
            fail("Cannot read the date " + Script.cslice(text, 0, 80));
          }
          var local = Script.dateFromParts(year, month - 1, day, hours, minutes, seconds, millis);
          var offset = match[8];
          if (offset === "Z" || offset === "z") return checkRange(local);
          if (offset) {
            var minutesOffset = parseOffset(offset);
            if (minutesOffset === null) fail("Cannot read the date " + Script.cslice(text, 0, 80));
            return checkRange(local - minutesOffset * 60000);
          }
          return checkRange(zone.localToUtc(local));
        }
      }
      if (number !== number || number === Infinity || number === -Infinity) fail("Cannot read the date " + stringify(value));
      var ms = Math.abs(number) >= 1e11 ? number : number * 1000;
      return checkRange(Math.floor(ms));
    }

    function localParts(ms, zone) {
      var offset = zone.offsetMinutes(ms);
      return [Script.civilFromMs(ms + offset * 60000), offset];
    }

    function two(number) {
      return (number < 10 ? "0" : "") + number;
    }

    function digits(number, width) {
      var text = String(number);
      while (text.length < width) text = "0" + text;
      return text;
    }

    function offsetText(offset, colon) {
      var sign = offset >= 0 ? "+" : "-";
      var total = Math.abs(offset);
      return sign + two(Math.floor(total / 60)) + (colon === false ? "" : ":") + two(total % 60);
    }

    function isoDate(ms, zone) {
      var local = localParts(ms, zone);
      var p = local[0];
      var text = digits(p[0], 4) + "-" + two(p[1]) + "-" + two(p[2]) + "T" + two(p[3]) + ":" + two(p[4]) + ":" + two(p[5]);
      if (p[6]) text += "." + digits(p[6], 3);
      return text + (zone.fixed === 0 ? "Z" : offsetText(local[1]));
    }

    function formatPattern(ms, zone, pattern) {
      var local = localParts(ms, zone);
      var p = local[0];
      var offset = local[1];
      var hours = p[3];
      var twelve = hours % 12 || 12;
      return pattern.replace(TOKEN_RE, function (text) {
        if (text.charAt(0) === "[") return text.slice(1, -1);
        switch (text) {
          case "YYYY": return digits(p[0], 4);
          case "YY": return two(p[0] % 100);
          case "MMMM": return MONTHS[p[1] - 1];
          case "MMM": return MONTHS[p[1] - 1].slice(0, 3);
          case "MM": return two(p[1]);
          case "M": return String(p[1]);
          case "DD": return two(p[2]);
          case "D": return String(p[2]);
          case "dddd": return WEEKDAYS[p[7]];
          case "ddd": return WEEKDAYS[p[7]].slice(0, 3);
          case "HH": return two(hours);
          case "H": return String(hours);
          case "hh": return two(twelve);
          case "h": return String(twelve);
          case "mm": return two(p[4]);
          case "m": return String(p[4]);
          case "ss": return two(p[5]);
          case "s": return String(p[5]);
          case "SSS": return digits(p[6], 3);
          case "A": return hours < 12 ? "AM" : "PM";
          case "a": return hours < 12 ? "am" : "pm";
          case "ZZ": return offsetText(offset, false);
          case "Z": return offsetText(offset, true);
          case "X": return String(Math.floor(ms / 1000));
          default: return String(ms);
        }
      });
    }

    function formatDate(value, fmt, pattern, zoneName) {
      var zone = new Zone(zoneName);
      var ms = parseDate(value, zone);
      if (fmt === "iso") return isoDate(ms, zone);
      if (fmt === "unix") return Math.floor(ms / 1000);
      if (fmt === "unixMs") return ms;
      var patterns = { date: "YYYY-MM-DD", time: "HH:mm", datetime: "YYYY-MM-DD HH:mm", long: "dddd, D MMMM YYYY" };
      if (fmt === "custom") {
        if (!pattern) fail("Enter a custom pattern");
        return formatPattern(ms, zone, pattern);
      }
      return formatPattern(ms, zone, patterns[fmt]);
    }

    function addMonths(ms, months, zone) {
      var p = localParts(ms, zone)[0];
      var total = p[0] * 12 + (p[1] - 1) + months;
      var year = Math.floor(total / 12);
      var month = total - year * 12;
      var day = Math.min(p[2], Script.monthDays(year, month + 1));
      return zone.localToUtc(Script.dateFromParts(year, month, day, p[3], p[4], p[5], p[6]));
    }

    function addDays(ms, days, zone) {
      var p = localParts(ms, zone)[0];
      return zone.localToUtc(Script.dateFromParts(p[0], p[1] - 1, p[2] + days, p[3], p[4], p[5], p[6]));
    }

    function startOf(ms, unit, zone) {
      var p = localParts(ms, zone)[0];
      var local;
      if (unit === "seconds") local = Script.dateFromParts(p[0], p[1] - 1, p[2], p[3], p[4], p[5]);
      else if (unit === "minutes") local = Script.dateFromParts(p[0], p[1] - 1, p[2], p[3], p[4]);
      else if (unit === "hours") local = Script.dateFromParts(p[0], p[1] - 1, p[2], p[3]);
      else if (unit === "days") local = Script.dateFromParts(p[0], p[1] - 1, p[2]);
      else if (unit === "weeks") local = Script.dateFromParts(p[0], p[1] - 1, p[2] - (p[7] + 6) % 7);
      else if (unit === "months") local = Script.dateFromParts(p[0], p[1] - 1, 1);
      else local = Script.dateFromParts(p[0], 0, 1);
      return zone.localToUtc(local);
    }

    function shift(ms, amount, unit, zone) {
      if (unit === "seconds" || unit === "minutes" || unit === "hours") return ms + Math.trunc(amount * UNIT_MS[unit]);
      if (!Number.isInteger(amount)) fail("Use a whole number of " + unit);
      if (unit === "days") return addDays(ms, amount, zone);
      if (unit === "weeks") return addDays(ms, amount * 7, zone);
      if (unit === "months") return addMonths(ms, amount, zone);
      return addMonths(ms, amount * 12, zone);
    }

    function adjustDate(value, operation, amount, unit, zoneName) {
      var zone = new Zone(zoneName);
      var ms = parseDate(value, zone);
      var result;
      if (operation === "startOf" || operation === "endOf") {
        result = startOf(ms, unit, zone);
        if (operation === "endOf") result = shift(result, 1, unit, zone) - 1000;
      } else {
        if (amount === null || amount === undefined) fail("Amount is required");
        result = shift(ms, operation === "add" ? amount : -amount, unit, zone);
      }
      return isoDate(checkRange(result), zone);
    }

    function monthsBetween(a, b) {
      if (b < a) return -monthsBetween(b, a);
      var utc = new Zone("");
      var pa = Script.civilFromMs(a);
      var pb = Script.civilFromMs(b);
      var months = (pb[0] - pa[0]) * 12 + (pb[1] - pa[1]);
      while (months > 0 && addMonths(a, months, utc) > b) months -= 1;
      return months;
    }

    function dateDifference(start, end, unit) {
      var utc = new Zone("");
      var a = parseDate(start, utc);
      var b = parseDate(end, utc);
      if (has(UNIT_MS, unit)) return Math.trunc((b - a) / UNIT_MS[unit]) + 0;
      var months = monthsBetween(a, b);
      if (unit === "months") return months;
      return Math.trunc(months / 12) + 0;
    }

    function urlParts(text) {
      var match = URI_RE.exec(pyStrip(text));
      var scheme = match[1];
      var authority = match[2];
      var path = match[3];
      var query = match[4];
      var fragment = match[5];
      var host = "";
      var port = null;
      var user = "";
      if (authority !== undefined) {
        var at = authority.lastIndexOf("@");
        if (at >= 0) {
          user = authority.slice(0, at);
          authority = authority.slice(at + 1);
        }
        if (authority.charAt(0) === "[") {
          var end = authority.indexOf("]");
          host = end > 0 ? authority.slice(0, end + 1) : authority;
          var rest = end > 0 ? authority.slice(end + 1) : "";
          if (rest.charAt(0) === ":" && /^[0-9]+$/.test(rest.slice(1))) port = parseInt(rest.slice(1), 10);
        } else {
          var colon = authority.lastIndexOf(":");
          if (colon >= 0) {
            var head = authority.slice(0, colon);
            var tail = authority.slice(colon + 1);
            if (/^[0-9]+$/.test(tail)) {
              host = head;
              port = parseInt(tail, 10);
            } else if (!tail) {
              host = head;
            } else {
              host = authority;
            }
          } else {
            host = authority;
          }
        }
      }
      var params = {};
      (query || "").split("&").forEach(function (piece) {
        if (!piece) return;
        var eq = piece.indexOf("=");
        var name = eq >= 0 ? piece.slice(0, eq) : piece;
        var item = eq >= 0 ? piece.slice(eq + 1) : "";
        Script.setOwn(params, percentDecode(name, true), percentDecode(item, true));
      });
      var out = {
        scheme: (scheme || "").toLowerCase(),
        host: host.toLowerCase(),
        port: port,
        path: path,
        query: params,
        fragment: fragment || ""
      };
      out.origin = scheme && host ? out.scheme + "://" + out.host + (port !== null ? ":" + port : "") : "";
      if (user) out.user = percentDecode(user.split(":")[0], false);
      return out;
    }

    function utilsObject(interp) {
      function arg(args, index, fallback) {
        var value = index < args.length ? args[index] : undefined;
        if (value === undefined || value === null) return fallback === undefined ? "" : fallback;
        return Script.toString(value);
      }
      function guard(fn) {
        return function (i, self, args) {
          try {
            return fn(args);
          } catch (error) {
            if (error instanceof NodeError) throw new Script.Thrown(Script.errorObject("Error", error.message));
            throw error;
          }
        };
      }
      function dateValue(value) {
        if (value instanceof Script.SDate) {
          if (value.ms !== value.ms) fail("Cannot read the date Invalid Date");
          return value.ms;
        }
        if (typeof value === "number") return value;
        return Script.toString(value);
      }
      var natives = {
        hash: function (args) { return hashText(arg(args, 0), arg(args, 1, "sha256"), "", arg(args, 2, "hex")); },
        hmac: function (args) { return hashText(arg(args, 0), arg(args, 2, "sha256"), arg(args, 1), arg(args, 3, "hex")); },
        base64Encode: function (args) { return base64(arg(args, 0)); },
        base64Decode: function (args) { return base64Decode(arg(args, 0)); },
        urlEncode: function (args) { return urlEncode(arg(args, 0)); },
        urlDecode: function (args) { return percentDecode(arg(args, 0), false); },
        formatDate: function (args) {
          var fmt = arg(args, 1, "iso");
          var value = args.length ? args[0] : undefined;
          if (value === undefined) value = interp.nowMs;
          if (["iso", "date", "time", "datetime", "long", "unix", "unixMs"].indexOf(fmt) >= 0) {
            return formatDate(dateValue(value), fmt, "", arg(args, 2));
          }
          return formatDate(dateValue(value), "custom", fmt, arg(args, 2));
        },
        formatNumber: function (args) {
          var value = args.length ? Script.toNumber(args[0]) : NaN;
          if (value !== value || value === Infinity || value === -Infinity) return Script.toString(value);
          var decimals = args.length > 1 && args[1] !== undefined ? Script.toInteger(args[1]) : 2;
          var grouping = args.length < 3 ? true : Script.truthy(args[2]);
          return formatNumber(value, decimals, grouping);
        }
      };
      var out = Script.dict();
      Object.keys(natives).forEach(function (name) { out[name] = new Script.SNative(name, guard(natives[name])); });
      return out;
    }

    return {
      StopRun: StopRun,
      VARIABLE_RE: VARIABLE_RE,
      numberOut: numberOut,
      toNumber: toNumber,
      copyJson: copyJson,
      withField: withField,
      changeCase: changeCase,
      regex: regex,
      replaceText: replaceText,
      matchText: matchText,
      splitText: splitText,
      combineText: combineText,
      encodeText: encodeText,
      hashText: hashText,
      extract: extract,
      count: count,
      calculate: calculate,
      roundNumber: roundNumber,
      formatNumber: formatNumber,
      statistics: statistics,
      sortKey: sortKey,
      sortItems: sortItems,
      formatDate: formatDate,
      adjustDate: adjustDate,
      dateDifference: dateDifference,
      urlParts: urlParts,
      utilsObject: utilsObject,
      pyStrip: pyStrip
    };
  })();

  function isoTime(ms) {
    return new Date(Math.floor(ms / 1000) * 1000).toISOString().slice(0, 19) + "Z";
  }

  function hostName(url) {
    return hostOf(url) || url;
  }

  function dumpsIndent(value, current) {
    var inner = current + "  ";
    if (Array.isArray(value)) {
      if (!value.length) return "[]";
      return "[\n" + inner + value.map(function (item) { return dumpsIndent(item, inner); }).join(",\n" + inner) + "\n" + current + "]";
    }
    if (isObject(value)) {
      var keys = Object.keys(value);
      if (!keys.length) return "{}";
      return "{\n" + inner + keys.map(function (key) {
        return pyString(key) + ": " + dumpsIndent(value[key], inner);
      }).join(",\n" + inner) + "\n" + current + "}";
    }
    return dumps(value);
  }

  function Execution(input) {
    this.workflow = input.workflow;
    this.credentials = or(input.credentials, {});
    this.maxRequests = input.maxRequests;
    this.deadline = input.deadlineMs;
    this.clock = input.clock;
    this.now = input.nowMs;
    this.workflowId = input.workflowId || "";
    this.random = input.random;
    this.workflows = input.workflows || [];
    this.depth = input.depth || 0;
    this.counter = input.counter || { requests: 0 };
    this.outputs = {};
    this.triggerItem = {};
    this.byName = [];
    this.variables = {};
    this.logs = {};
    this.stopped = "";
  }

  Execution.prototype.firstOutput = function (id) {
    var out = or(this.outputs[id], {});
    var names = Object.keys(out);
    for (var i = 0; i < names.length; i++) {
      if (out[names[i]].length) return out[names[i]];
    }
    return [];
  };

  Execution.prototype.context = function (item, index) {
    var nodes = {};
    var self = this;
    this.byName.forEach(function (pair) {
      var main = self.firstOutput(pair[1]);
      if (main.length) nodes[pair[0]] = { json: index < main.length ? main[index] : main[0] };
    });
    return {
      json: item,
      item: { index: index },
      node: nodes,
      trigger: this.triggerItem,
      now: isoTime(this.now),
      today: isoTime(this.now).slice(0, 10),
      vars: this.variables,
      workflow: { id: this.workflowId, name: get(this.workflow, "name") || "" }
    };
  };

  Execution.prototype.values = function (node, item, index) {
    var params = or(node.params, {});
    return fieldValues(fieldsFor(node.type, params), params, this.context(item, index));
  };

  Execution.prototype.readPath = function (path, item, index) {
    path = Actions.pyStrip(pyStr(or(path, "")));
    if (!path) return null;
    return evaluate(path.charAt(0) === "$" ? path : "$json." + path, this.context(item, index));
  };

  Execution.prototype.idOf = function (name) {
    for (var i = 0; i < this.byName.length; i++) {
      if (this.byName[i][0] === name) return this.byName[i][1];
    }
    return null;
  };

  Execution.prototype.request = function* (request) {
    if (!safeUrl(request.url)) fail("Only public https:// URLs can be called");
    if (this.counter.requests >= this.maxRequests) fail("This run reached its limit of " + this.maxRequests + " requests");
    if (this.clock() > this.deadline) fail("This run ran out of time");
    this.counter.requests += 1;
    var reply = yield request;
    var status = reply && typeof reply.status === "number" ? reply.status : 0;
    var text = reply && typeof reply.text === "string" ? reply.text : "";
    if (!status) fail("No response from " + hostName(request.url));
    var parsed = parseResponse(text);
    if (status >= 400) fail("HTTP " + status + ": " + text.trim().slice(0, 300));
    return parsed;
  };

  Execution.prototype.runNode = function* (node, items) {
    var type = node.type;
    var params = or(node.params, {});
    if (TRIGGERS.indexOf(type) >= 0) return { main: [this.triggerItem] };
    if (node.disabled) {
      var passed = {};
      passed[outputsFor(type)[0]] = items;
      return passed;
    }
    if (type === "core.merge" || type === "core.noop" || type === "core.comment") return { main: items.slice(0, LIMITS.maxItems) };
    if (type === "core.limit") {
      var limit = fieldValues(fieldsFor(type, params), params, this.context({}, 0));
      return { main: items.slice(0, Math.max(0, Math.trunc(limit.max))) };
    }
    if (has(ACTION_TYPES, type)) return yield* this.runAction(node, items.slice(0, LIMITS.maxItems));
    var out = {};
    outputsFor(type).forEach(function (name) { out[name] = []; });
    var list = items.slice(0, LIMITS.maxItems);
    for (var index = 0; index < list.length; index++) {
      var item = list[index];
      var context = this.context(item, index);
      var values = fieldValues(fieldsFor(type, params), params, context);
      if (type === "core.set") {
        var extra = values.values;
        if (!isObject(extra)) fail("Fields must be a JSON object");
        var merged = values.keep !== false ? copy(item) : {};
        Object.keys(extra).forEach(function (key) { setPath(merged, key, extra[key]); });
        out.main.push(merged);
      } else if (type === "core.if" || type === "core.filter") {
        var ok = checkCondition(values.left, values.operator, values.right);
        if (type === "core.filter") {
          if (ok) out.main.push(item);
        } else {
          out[ok ? "true" : "false"].push(item);
        }
      } else if (type === "core.split") {
        var field = values.field;
        var found = evaluate(field.charAt(0) === "$" ? field : "$json." + field, context);
        if (Array.isArray(found)) out.main = out.main.concat(itemsFrom(found));
      } else if (type === "core.http") {
        var response = yield* this.request(buildHttpRequest(values));
        out.main = out.main.concat(itemsFrom(response));
      } else {
        var key = appKey(type);
        var app = integrations()[key];
        var cred = {};
        if (app.auth.fields.length) {
          var record = get(this.credentials, pyStr(or(node.credential, "")));
          if (!isObject(record) || record.integration !== key) fail("Choose a " + app.name + " credential for this step");
          cred = or(record.values, {});
        }
        var parsed = yield* this.request(buildAppRequest(key, params, values, cred));
        var output = app.operations[params.operation].output;
        out.main = out.main.concat(itemsFrom(withText(output, parsed)));
      }
      Object.keys(out).forEach(function (name) { out[name] = out[name].slice(0, LIMITS.maxItems); });
    }
    return out;
  };

  var ACTION_TYPES = {};
  ["core.code", "core.variable", "core.switch", "core.repeat", "core.wait", "core.runWorkflow", "core.stop", "core.comment",
    "core.textCase", "core.textReplace", "core.textMatch", "core.textSplit", "core.textCombine", "core.textEncode", "core.hash",
    "core.textExtract", "core.count", "core.math", "core.round", "core.formatNumber", "core.random", "core.statistics",
    "core.combineItems", "core.sort", "core.dedupe", "core.pick", "core.json", "core.url", "core.dateFormat", "core.dateAdjust",
    "core.dateDiff"].forEach(function (type) { ACTION_TYPES[type] = true; });

  Execution.prototype.runAction = function* (node, items) {
    var type = node.type;
    var self = this;
    var values;
    if (type === "core.code") return { main: this.runScript(node, items).slice(0, LIMITS.maxItems) };
    if (type === "core.wait") {
      yield* this.wait(this.values(node, items[0], 0).seconds);
      return { main: items };
    }
    if (type === "core.stop") {
      values = this.values(node, items[0], 0);
      if (values.outcome === "error") fail(or(values.message, "The workflow was stopped"));
      throw new Actions.StopRun({ main: items });
    }
    if (type === "core.pick") return { main: this.pick(this.values(node, items[0], 0), items) };
    var params = or(node.params, {});
    if (type === "core.sort") {
      values = this.values(node, items[0], 0);
      var keys = items.map(function (item, index) { return Actions.sortKey(self.sortValue(get(params, "by"), item, index)); });
      return { main: Actions.sortItems(items, keys, values.order, this.random) };
    }
    if (type === "core.dedupe") {
      var by = get(params, "by");
      var useBy = !!Actions.pyStrip(pyStr(or(by, "")));
      var seen = {};
      var kept = [];
      items.forEach(function (item, index) {
        var marker = dumps(useBy ? self.readPath(by, item, index) : item);
        if (!has(seen, marker)) {
          seen[marker] = true;
          kept.push(item);
        }
      });
      return { main: kept };
    }
    if (type === "core.statistics") {
      var numbers = items.map(function (item, index) { return self.values(node, item, index).value; });
      values = this.values(node, items[0], 0);
      var result = Actions.statistics(numbers, values.operation);
      return { main: [Actions.withField({}, values.field, Actions.numberOut(result))] };
    }
    if (type === "core.combineItems") {
      var whole = !Actions.pyStrip(pyStr(or(get(params, "value"), "")));
      var gathered = items.map(function (item, index) {
        return whole ? Actions.copyJson(item) : self.values(node, item, index).value;
      });
      var field = this.values(node, items[0], 0).field;
      return { main: [Actions.withField({}, field, gathered)] };
    }
    var out = {};
    outputsFor(type).forEach(function (name) { out[name] = []; });
    for (var index = 0; index < items.length; index++) {
      var item = items[index];
      values = this.values(node, item, index);
      if (type === "core.switch") {
        var text = stringify(values.value);
        var routed = false;
        var cases = ["case1", "case2", "case3"];
        for (var c = 0; c < cases.length; c++) {
          var expected = values[cases[c]];
          if (expected !== null && expected !== "" && expected === text) {
            out[cases[c]].push(item);
            routed = true;
            break;
          }
        }
        if (!routed) out.otherwise.push(item);
      } else if (type === "core.repeat") {
        var times = Math.trunc(Math.max(0, Math.min(SPEC.limits.maxRepeat, values.times)));
        for (var count = 1; count <= times; count++) out.main.push(Actions.withField(item, values.field, count));
      } else if (type === "core.variable") {
        this.setVariable(values);
        out.main.push(item);
      } else if (type === "core.runWorkflow") {
        var produced = yield* this.runChild(values.workflow, item);
        out.main = out.main.concat(produced);
      } else {
        out.main.push(Actions.withField(item, values.field, this.compute(type, values)));
      }
      Object.keys(out).forEach(function (name) { out[name] = out[name].slice(0, LIMITS.maxItems); });
    }
    return out;
  };

  Execution.prototype.compute = function (type, values) {
    var text = values.value === null || values.value === undefined ? "" : stringify(values.value);
    if (type === "core.textCase") return Actions.changeCase(text, values["case"]);
    if (type === "core.textReplace") {
      return Actions.replaceText(text, or(values.find, ""), or(values.replace, ""), values.regex === true, values.ignoreCase === true);
    }
    if (type === "core.textMatch") return Actions.matchText(text, values.pattern, values.result, values.ignoreCase === true);
    if (type === "core.textSplit") return Actions.splitText(text, values.separator, or(values.custom, ""), values.skipEmpty !== false);
    if (type === "core.textCombine") return Actions.combineText(values.value, values.separator, or(values.custom, ""));
    if (type === "core.textEncode") return Actions.encodeText(text, values.method);
    if (type === "core.hash") return Actions.hashText(text, values.algorithm, or(values.key, ""), values.encoding);
    if (type === "core.textExtract") return Actions.extract(text, values.kind);
    if (type === "core.count") return Actions.count(values.value, values.what);
    if (type === "core.math") {
      var second = values.b;
      return Actions.numberOut(Actions.calculate(Actions.toNumber(values.a, "Number"), values.operator,
        second === null || second === undefined ? null : Actions.toNumber(second, "Second number")));
    }
    if (type === "core.round") {
      return Actions.numberOut(Actions.roundNumber(Actions.toNumber(values.value, "Number"), values.decimals, values.mode));
    }
    if (type === "core.formatNumber") {
      return Actions.formatNumber(Actions.toNumber(values.value, "Number"), values.decimals, values.grouping !== false);
    }
    if (type === "core.random") return Actions.numberOut(this.randomNumber(values));
    if (type === "core.json") return this.jsonValue(values);
    if (type === "core.url") return Actions.urlParts(text);
    if (type === "core.dateFormat") {
      return Actions.formatDate(values.value, values.format, or(values.pattern, ""), or(values.timeZone, ""));
    }
    if (type === "core.dateAdjust") {
      return Actions.adjustDate(values.value, values.operation, values.amount, values.unit, or(values.timeZone, ""));
    }
    if (type === "core.dateDiff") return Actions.dateDifference(values.start, values.end, values.unit);
    fail("Unknown step " + type);
  };

  Execution.prototype.jsonValue = function (values) {
    var value = values.value;
    if (values.operation === "stringify") return values.pretty === true ? dumpsIndent(value, "") : dumps(value);
    if (value === null || value === undefined) fail("There is no JSON text to read");
    if (typeof value !== "string") return value;
    var parsed = parseJson(value);
    if (!parsed.ok) fail("The text is not valid JSON");
    return parsed.value;
  };

  Execution.prototype.randomNumber = function (values) {
    var low = values.min;
    var high = values.max;
    if (values.whole !== false) {
      low = Math.ceil(low);
      high = Math.floor(high);
      if (low > high) fail("From must not be larger than To");
      return low + Math.floor(this.random() * (high - low + 1));
    }
    if (low > high) fail("From must not be larger than To");
    return low + this.random() * (high - low);
  };

  Execution.prototype.setVariable = function (values) {
    var name = or(values.name, "");
    if (!Actions.VARIABLE_RE.test(name)) fail("Variable names use letters, digits and _");
    var value = Actions.copyJson(values.value === undefined ? null : values.value);
    var existing = has(this.variables, name) ? this.variables[name] : null;
    if (values.mode === "set") {
      this.variables[name] = value;
    } else if (values.mode === "append") {
      var list = Array.isArray(existing) ? existing.slice() : (existing === null ? [] : [existing]);
      list.push(value);
      this.variables[name] = list.slice(-LIMITS.maxItems * 10);
    } else {
      var base = existing === null ? 0 : Actions.toNumber(existing, "The variable " + name);
      this.variables[name] = Actions.numberOut(base + Actions.toNumber(value, "Value"));
    }
  };

  Execution.prototype.sortValue = function (by, item, index) {
    if (Actions.pyStrip(pyStr(or(by, "")))) return this.readPath(by, item, index);
    var keys = Object.keys(item);
    return keys.length ? item[keys[0]] : null;
  };

  Execution.prototype.pick = function (values, items) {
    var which = values.which;
    if (which === "first") return items.slice(0, 1);
    if (which === "last") return items.slice(-1);
    if (which === "random") return [items[Math.floor(this.random() * items.length)]];
    var position = Math.trunc(or(values.index, 0));
    if (which === "index") {
      if (position < 0) position += items.length + 1;
      return position >= 1 && position <= items.length ? [items[position - 1]] : [];
    }
    var end = Math.trunc(or(values.to, 0));
    return items.slice(Math.max(0, position - 1), Math.max(0, end));
  };

  Execution.prototype.wait = function* (seconds) {
    if (seconds < 0) fail("Seconds must be 0 or more");
    if (seconds > SPEC.limits.maxWaitSeconds) fail("Wait at most " + SPEC.limits.maxWaitSeconds + " seconds");
    var ms = Math.trunc(seconds * 1000);
    if (this.clock() + ms > this.deadline) fail("Waiting would run past this run's time limit");
    yield { wait: ms };
  };

  Execution.prototype.findWorkflow = function (ref) {
    var list = Array.isArray(this.workflows) ? this.workflows : [];
    var i;
    for (i = 0; i < list.length; i++) {
      if (isObject(list[i]) && list[i].id === ref) return list[i];
    }
    for (i = 0; i < list.length; i++) {
      if (isObject(list[i]) && Actions.pyStrip(pyStr(or(list[i].name, ""))).toLowerCase() === ref.toLowerCase()) return list[i];
    }
    return null;
  };

  Execution.prototype.runChild = function* (ref, item) {
    ref = Actions.pyStrip(pyStr(or(ref, "")));
    var maxDepth = SPEC.limits.maxWorkflowDepth;
    if (this.depth + 1 >= maxDepth) fail("Workflows can run other workflows at most " + (maxDepth - 1) + " deep");
    var entry = this.findWorkflow(ref);
    var checked = entry ? clean(entry.workflow) : null;
    if (!checked || checked.error) fail("No workflow named " + ref);
    var child = new Execution({
      workflow: checked.workflow,
      credentials: this.credentials,
      maxRequests: this.maxRequests,
      deadlineMs: this.deadline,
      clock: this.clock,
      nowMs: this.now,
      workflowId: pyStr(or(entry.id, "")),
      random: this.random,
      workflows: this.workflows,
      depth: this.depth + 1,
      counter: this.counter
    });
    var report = yield* child.run(item);
    if (report.status !== "success") fail(or(get(checked.workflow, "name"), ref) + " failed: " + report.error);
    return child.finalItems(report);
  };

  Execution.prototype.finalItems = function (report) {
    var nodes = or(get(this.workflow, "nodes"), []);
    var connections = or(get(this.workflow, "connections"), []);
    var ids;
    if (this.stopped) {
      ids = [this.idOf(this.stopped)];
    } else {
      var sources = {};
      connections.forEach(function (conn) { sources[conn.from] = true; });
      ids = (topologicalOrder(nodes, connections) || []).filter(function (id) { return !has(sources, id); });
    }
    var found = [];
    var self = this;
    ids.forEach(function (id) {
      if (get(or(report.nodes[id], {}), "status") !== "success") return;
      var out = or(self.outputs[id], {});
      Object.keys(out).forEach(function (name) { found = found.concat(out[name]); });
    });
    return found.slice(0, LIMITS.maxItems);
  };

  Execution.prototype.scriptInput = function (scriptItems, current) {
    function wrap(item) {
      var out = Script.dict();
      out.json = item;
      return out;
    }
    var out = Script.dict();
    out.all = new Script.SNative("all", function () { return scriptItems.map(wrap); });
    out.first = new Script.SNative("first", function () { return scriptItems.length ? wrap(scriptItems[0]) : undefined; });
    out.last = new Script.SNative("last", function () {
      return scriptItems.length ? wrap(scriptItems[scriptItems.length - 1]) : undefined;
    });
    if (current !== null) out.item = wrap(current);
    return out;
  };

  Execution.prototype.runScript = function (node, items) {
    var values = this.values(node, items[0], 0);
    var program;
    try {
      program = Script.parse(values.code);
    } catch (error) {
      if (error instanceof Script.ScriptError) fail(error.message);
      throw error;
    }
    var interp = new Script.Interpreter(this.now, this.random, this.clock, this.deadline);
    var scriptItems = items.map(Script.fromJson);
    var vars = Script.fromJson(this.variables);
    var shared = [
      ["$trigger", Script.fromJson(this.triggerItem)],
      ["$vars", vars],
      ["$now", isoTime(this.now)],
      ["$today", isoTime(this.now).slice(0, 10)],
      ["$workflow", Script.fromJson({ id: this.workflowId, name: get(this.workflow, "name") || "" })],
      ["$utils", Actions.utilsObject(interp)]
    ];
    var out = [];
    var variables;
    try {
      if (values.mode === "each") {
        for (var index = 0; index < scriptItems.length; index++) {
          var item = scriptItems[index];
          var current = shared.concat([
            ["items", scriptItems],
            ["item", item],
            ["$json", item],
            ["$index", index],
            ["$node", Script.fromJson(this.context(items[index], index).node)],
            ["$input", this.scriptInput(scriptItems, item)]
          ]);
          out = out.concat(Script.unwrapItems(interp.run(program, current), item));
        }
      } else {
        var all = shared.concat([
          ["items", scriptItems],
          ["$node", Script.fromJson(this.context(items[0], 0).node)],
          ["$input", this.scriptInput(scriptItems, null)]
        ]);
        out = Script.unwrapItems(interp.run(program, all), scriptItems);
      }
      variables = Script.toJson(vars);
    } catch (error) {
      if (error instanceof Script.ScriptError || error instanceof Script.ScriptLimit) fail(error.message);
      throw error;
    } finally {
      if (interp.logs.length) this.logs[node.id] = interp.logs.slice();
    }
    this.variables = isObject(variables) ? variables : {};
    return out;
  };

  Execution.prototype.report = function (id, out, started, error) {
    var outputs = {};
    Object.keys(out).forEach(function (name) {
      outputs[name] = { count: out[name].length, items: summarizeItems(out[name]) };
    });
    var entry = { status: "success", ms: Math.max(0, this.clock() - started), outputs: outputs };
    if (error) entry.error = error;
    if (has(this.logs, id)) entry.logs = this.logs[id];
    return entry;
  };

  Execution.prototype.run = function* (triggerItem) {
    this.triggerItem = isObject(triggerItem) ? triggerItem : {};
    var nodes = or(get(this.workflow, "nodes"), []);
    var connections = or(get(this.workflow, "connections"), []);
    var byId = {};
    var self = this;
    nodes.forEach(function (node) { byId[node.id] = node; });
    var seenNames = {};
    this.byName = [];
    nodes.forEach(function (node) {
      if (has(seenNames, node.name)) {
        self.byName[seenNames[node.name]][1] = node.id;
      } else {
        seenNames[node.name] = self.byName.length;
        self.byName.push([node.name, node.id]);
      }
    });
    var order = topologicalOrder(nodes, connections) || [];
    var start = triggerNode(this.workflow);
    var report = {};
    var failed = "";
    for (var i = 0; i < order.length; i++) {
      var id = order[i];
      var node = byId[id];
      var started = this.clock();
      if (failed || this.stopped) {
        report[id] = { status: "skipped" };
        continue;
      }
      var items;
      if (isTrigger(node.type)) {
        if (!start || id !== start.id) {
          report[id] = { status: "skipped" };
          continue;
        }
        items = [this.triggerItem];
      } else {
        items = [];
        var fed = false;
        for (var j = 0; j < connections.length; j++) {
          var conn = connections[j];
          if (conn.to !== id) continue;
          var produced = or(this.outputs[conn.from], {})[conn.output];
          if (produced !== undefined && produced !== null) {
            fed = true;
            items = items.concat(produced);
          }
        }
        if (!fed || !items.length) {
          report[id] = { status: "skipped" };
          continue;
        }
      }
      var out;
      try {
        out = yield* this.runNode(node, items);
      } catch (error) {
        if (error instanceof Actions.StopRun) {
          this.outputs[id] = error.out;
          report[id] = this.report(id, error.out, started, "");
          this.stopped = node.name;
          continue;
        }
        var message = error instanceof NodeError ? error.message : String(error && error.message || error);
        if (node.continueOnFail) {
          out = {};
          outputsFor(node.type).forEach(function (name) { out[name] = []; });
          out[outputsFor(node.type)[0]] = [{ error: message }];
          this.outputs[id] = out;
          report[id] = this.report(id, out, started, message);
          continue;
        }
        report[id] = { status: "error", error: message, ms: Math.max(0, this.clock() - started) };
        if (has(this.logs, id)) report[id].logs = this.logs[id];
        failed = node.name + ": " + message;
        continue;
      }
      this.outputs[id] = out;
      report[id] = this.report(id, out, started, "");
    }
    var result = { status: failed ? "error" : "success", error: failed, requests: this.counter.requests, nodes: report };
    if (this.stopped) result.stopped = this.stopped;
    return result;
  };

  function load(spec) {
    SPEC = typeof spec === "string" ? JSON.parse(spec) : spec;
    LIMITS = SPEC.limits;
    return SPEC.version;
  }

  function begin(input) {
    var kind = input.trigger || "manual";
    var limits = has(SPEC.runLimits, kind) ? SPEC.runLimits[kind] : SPEC.runLimits.manual;
    var clock = typeof input.clock === "function" ? input.clock : function () { return Date.now(); };
    var now = typeof input.nowMs === "number" ? input.nowMs : clock();
    var checked = clean(input.workflow);
    if (checked.error) return { done: { status: "error", error: "Workflow: " + checked.error, requests: 0, nodes: {}, trigger: kind } };
    var workflow = checked.workflow;
    var payload = manualPayload(workflow, input.payload);
    var random;
    if (Array.isArray(input.randoms)) {
      var randoms = input.randoms.slice();
      random = function () { return randoms.length ? randoms.shift() : 0.5; };
    } else {
      random = function () { return Math.random(); };
    }
    var execution = new Execution({
      workflow: workflow,
      credentials: input.credentials,
      maxRequests: limits[0],
      deadlineMs: clock() + limits[1],
      clock: clock,
      nowMs: now,
      workflowId: input.workflowId,
      random: random,
      workflows: Array.isArray(input.workflows) ? input.workflows : []
    });
    var iterator = execution.run(payload);
    return { iterator: iterator, kind: kind };
  }

  function advance(state, reply) {
    var next = state.iterator.next(reply);
    if (next.done) {
      next.value.trigger = state.kind;
      return { done: next.value };
    }
    if (isObject(next.value) && has(next.value, "wait")) return { wait: next.value.wait };
    return { request: next.value };
  }

  function runWith(input, http, wait) {
    var state = begin(input);
    if (state.done) return state.done;
    var stepOut = advance(state, undefined);
    while (!stepOut.done) {
      if (stepOut.request) {
        stepOut = advance(state, http(stepOut.request));
      } else {
        if (typeof wait === "function") wait(stepOut.wait);
        stepOut = advance(state, undefined);
      }
    }
    return stepOut.done;
  }

  function start(inputJSON) {
    var state = begin(JSON.parse(inputJSON));
    if (state.done) return JSON.stringify({ done: state.done });
    var id = String(nextSession++);
    sessions[id] = state;
    return resume(id, "null");
  }

  function resume(id, replyJSON) {
    var state = sessions[id];
    if (!state) return JSON.stringify({ done: { status: "error", error: "This run is no longer open", requests: 0, nodes: {} } });
    var out;
    try {
      out = advance(state, JSON.parse(replyJSON) || undefined);
    } catch (error) {
      delete sessions[id];
      return JSON.stringify({ done: { status: "error", error: String(error && error.message || error), requests: 0, nodes: {} } });
    }
    if (out.done) {
      delete sessions[id];
      return JSON.stringify(out);
    }
    out.session = id;
    return JSON.stringify(out);
  }

  function cancel(id) {
    delete sessions[id];
    return true;
  }

  function call(name, argsJSON) {
    var args = JSON.parse(argsJSON || "[]");
    var api = {
      catalog: catalog,
      clean: clean,
      manualPayload: manualPayload,
      scheduleMinutes: scheduleMinutes,
      triggerNode: triggerNode,
      outputsFor: outputsFor,
      safeUrl: safeUrl,
      topologicalOrder: topologicalOrder,
      version: function () { return SPEC.version; }
    };
    if (!has(api, name)) return JSON.stringify({ error: "unknown_call" });
    var result = api[name].apply(null, args);
    return JSON.stringify({ result: result === undefined ? null : result });
  }

  return {
    load: load,
    call: call,
    start: start,
    resume: resume,
    cancel: cancel,
    runWith: runWith,
    catalog: catalog,
    clean: clean,
    manualPayload: manualPayload,
    scheduleMinutes: scheduleMinutes,
    safeUrl: safeUrl,
    evaluate: evaluate,
    resolve: resolve,
    buildAppRequest: buildAppRequest,
    buildHttpRequest: buildHttpRequest,
    checkCondition: checkCondition
  };
})();
