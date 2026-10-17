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
      fail("Expressions start with $json, $node, $trigger, $now, $today, $item or $workflow: " + text.slice(0, 80));
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
    fail("Unknown operator " + pyStr(operator));
  }

  function summarizeItems(items) {
    return items.slice(0, LIMITS.maxRunItems).map(function (item) {
      var encoded = dumps(item);
      return encoded.length > LIMITS.maxRunItemChars ? { _truncated: encoded.slice(0, LIMITS.maxRunItemChars) } : item;
    });
  }

  function isoTime(ms) {
    return new Date(Math.floor(ms / 1000) * 1000).toISOString().slice(0, 19) + "Z";
  }

  function hostName(url) {
    return hostOf(url) || url;
  }

  function Execution(input) {
    this.workflow = input.workflow;
    this.credentials = or(input.credentials, {});
    this.maxRequests = input.maxRequests;
    this.deadline = input.deadlineMs;
    this.clock = input.clock;
    this.now = input.nowMs;
    this.workflowId = input.workflowId || "";
    this.requests = 0;
    this.outputs = {};
    this.triggerItem = {};
    this.byName = [];
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
      workflow: { id: this.workflowId, name: get(this.workflow, "name") || "" }
    };
  };

  Execution.prototype.request = function* (request) {
    if (!safeUrl(request.url)) fail("Only public https:// URLs can be called");
    if (this.requests >= this.maxRequests) fail("This run reached its limit of " + this.maxRequests + " requests");
    if (this.clock() > this.deadline) fail("This run ran out of time");
    this.requests += 1;
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
    if (type === "core.merge" || type === "core.noop") return { main: items.slice(0, LIMITS.maxItems) };
    if (type === "core.limit") {
      var limit = fieldValues(fieldsFor(type, params), params, this.context({}, 0));
      return { main: items.slice(0, Math.max(0, Math.trunc(limit.max))) };
    }
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

  Execution.prototype.report = function (out, started, error) {
    var outputs = {};
    Object.keys(out).forEach(function (name) {
      outputs[name] = { count: out[name].length, items: summarizeItems(out[name]) };
    });
    var entry = { status: "success", ms: Math.max(0, this.clock() - started), outputs: outputs };
    if (error) entry.error = error;
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
      if (failed) {
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
        var message = error instanceof NodeError ? error.message : String(error && error.message || error);
        if (node.continueOnFail) {
          out = {};
          outputsFor(node.type).forEach(function (name) { out[name] = []; });
          out[outputsFor(node.type)[0]] = [{ error: message }];
          this.outputs[id] = out;
          report[id] = this.report(out, started, message);
          continue;
        }
        report[id] = { status: "error", error: message, ms: Math.max(0, this.clock() - started) };
        failed = node.name + ": " + message;
        continue;
      }
      this.outputs[id] = out;
      report[id] = this.report(out, started, "");
    }
    return { status: failed ? "error" : "success", error: failed, requests: this.requests, nodes: report };
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
    var execution = new Execution({
      workflow: workflow,
      credentials: input.credentials,
      maxRequests: limits[0],
      deadlineMs: clock() + limits[1],
      clock: clock,
      nowMs: now,
      workflowId: input.workflowId
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
    return { request: next.value };
  }

  function runWith(input, http) {
    var state = begin(input);
    if (state.done) return state.done;
    var stepOut = advance(state, undefined);
    while (stepOut.request) stepOut = advance(state, http(stepOut.request));
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
