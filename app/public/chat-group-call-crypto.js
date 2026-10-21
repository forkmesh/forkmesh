const TRAILER = 9;
const TAG = 16;
const KEEP_KEYS = 3;

const peerKeys = new Map();
let sendKey = null;
let sendKeyId = 0;
let counter = 0n;

function clearBytes(kind, frame, length) {
  if (kind === "audio") return Math.min(1, length);
  return Math.min(frame.type === "key" ? 10 : 3, length);
}

async function importKey(raw) {
  return crypto.subtle.importKey("raw", raw, { name: "AES-GCM" }, false, ["encrypt", "decrypt"]);
}

function iv(value) {
  const bytes = new Uint8Array(12);
  new DataView(bytes.buffer).setBigUint64(4, value);
  return bytes;
}

async function encrypt(kind, frame, controller) {
  if (!sendKey) return;
  const data = new Uint8Array(frame.data);
  const clear = clearBytes(kind, frame, data.length);
  counter += 1n;
  const nonce = iv(counter);
  const sealed = new Uint8Array(await crypto.subtle.encrypt(
    { name: "AES-GCM", iv: nonce, additionalData: data.subarray(0, clear), tagLength: 128 },
    sendKey,
    data.subarray(clear),
  ));
  const out = new Uint8Array(clear + sealed.length + TRAILER);
  out.set(data.subarray(0, clear), 0);
  out.set(sealed, clear);
  out.set(nonce.subarray(4), clear + sealed.length);
  out[out.length - 1] = sendKeyId;
  frame.data = out.buffer;
  controller.enqueue(frame);
}

async function decrypt(kind, peer, frame, controller) {
  const data = new Uint8Array(frame.data);
  if (data.length < TRAILER + TAG + 1) return;
  const keyId = data[data.length - 1];
  const key = peerKeys.get(peer)?.get(keyId);
  if (!key) return;
  const clear = clearBytes(kind, frame, data.length - TRAILER - TAG);
  const nonce = new Uint8Array(12);
  nonce.set(data.subarray(data.length - TRAILER, data.length - 1), 4);
  let plain;
  try {
    plain = new Uint8Array(await crypto.subtle.decrypt(
      { name: "AES-GCM", iv: nonce, additionalData: data.subarray(0, clear), tagLength: 128 },
      key,
      data.subarray(clear, data.length - TRAILER),
    ));
  } catch (_) {
    return;
  }
  const out = new Uint8Array(clear + plain.length);
  out.set(data.subarray(0, clear), 0);
  out.set(plain, clear);
  frame.data = out.buffer;
  controller.enqueue(frame);
}

function pipe(readable, writable, options) {
  const transform = new TransformStream({
    transform(frame, controller) {
      if (options.direction === "encrypt") return encrypt(options.kind, frame, controller);
      return decrypt(options.kind, options.peer, frame, controller);
    },
  });
  readable.pipeThrough(transform).pipeTo(writable).catch(() => {});
}

self.onmessage = async (event) => {
  const message = event.data || {};
  if (message.op === "send-key") {
    sendKey = await importKey(message.raw);
    sendKeyId = message.keyId & 0xff;
    counter = 0n;
  } else if (message.op === "key") {
    const keys = peerKeys.get(message.peer) || new Map();
    keys.set(message.keyId & 0xff, await importKey(message.raw));
    while (keys.size > KEEP_KEYS) keys.delete(keys.keys().next().value);
    peerKeys.set(message.peer, keys);
  } else if (message.op === "forget") {
    peerKeys.delete(message.peer);
  } else if (message.op === "stream") {
    pipe(message.readable, message.writable, message);
  }
};

self.onrtctransform = (event) => {
  const transformer = event.transformer;
  pipe(transformer.readable, transformer.writable, transformer.options || {});
};
