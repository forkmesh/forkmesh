// One exponential-backoff gate for every World HTTP call.
//
// The World opens dozens of boards, kiosks, panels, and refresh timers, and
// each of them used to decide on its own whether a failed request should be
// asked again immediately. On a degraded Worker that turns a single outage into
// a self-sustaining request storm: every timer fires, every request fails, and
// every failure is retried at full rate against an origin that is already
// answering 429 or 503. Keeping the growth rule here means a failing endpoint
// is asked again after 5s, then 10s, 20s, and so on up to five minutes, and a
// server that answered with Retry-After, 429, or 503 is obeyed rather than
// argued with.
//
// The gate only remembers failures; it never retries on its own. Callers keep
// their own timers and simply find the next attempt refused while that endpoint
// is still cooling down.

export const WORLD_BACKOFF_BASE_MS = 5_000;
export const WORLD_BACKOFF_MAX_MS = 5 * 60 * 1_000;
export const WORLD_BACKOFF_MAX_ATTEMPTS = 8;
// A rate-limited or unavailable origin is telling us how loaded it is even when
// it sends no Retry-After, so those two answers start well above the base step.
export const WORLD_BACKOFF_RATE_LIMIT_MS = 60_000;
export const WORLD_BACKOFF_UNAVAILABLE_MS = 30_000;
// Failure bookkeeping is per endpoint, so the map is bounded the same way the
// response cache is: a long visit must not accumulate a row per browsed path.
export const WORLD_BACKOFF_MAX_KEYS = 128;

// Statuses worth waiting out. Everything else in the 4xx range is a decision
// about this request (bad input, missing row, no permission) that a later
// identical attempt cannot change, so a repeat is the caller's business.
const RETRYABLE_STATUSES = new Set([408, 425, 429]);

// A failure with no HTTP status is a transport failure — DNS, TLS, an aborted
// timeout, an offline device — and those recover on their own.
export function isRetryableWorldFailure(error) {
  const status = Number(error?.status || 0);
  if (!status) return true;
  return status >= 500 || RETRYABLE_STATUSES.has(status);
}

export function worldRetryAfterMs(response) {
  const header = String(response?.headers?.get?.("retry-after") || "");
  if (!header) return 0;
  const seconds = Number(header);
  if (Number.isFinite(seconds)) return Math.max(0, seconds * 1_000);
  const date = Date.parse(header);
  return Number.isFinite(date) ? Math.max(0, date - Date.now()) : 0;
}

// Copies the two fields the gate reads off a Response onto the error a caller
// is about to throw, so every raw fetch site backs off on the same evidence
// fetchJSON does.
export function markWorldHTTPFailure(error, response) {
  if (!error || typeof error !== "object") return error;
  if (!error.status) error.status = Number(response?.status || 0);
  if (error.retryAfterMs === undefined) {
    error.retryAfterMs = worldRetryAfterMs(response);
  }
  return error;
}

export function worldBackoffDelayMs(attempts, failure = {}) {
  const tries = Math.min(
    WORLD_BACKOFF_MAX_ATTEMPTS,
    Math.max(1, Number(attempts) || 1),
  );
  const status = Number(failure?.status || 0);
  return Math.min(
    WORLD_BACKOFF_MAX_MS,
    Math.max(
      WORLD_BACKOFF_BASE_MS * 2 ** (tries - 1),
      Number(failure?.retryAfterMs) || 0,
      status === 429 ? WORLD_BACKOFF_RATE_LIMIT_MS : 0,
      status === 503 ? WORLD_BACKOFF_UNAVAILABLE_MS : 0,
    ),
  );
}

export function worldCoolingDownError(label, waitMs) {
  const seconds = Math.max(1, Math.ceil(Number(waitMs) / 1_000));
  const error = new Error(
    `${label} is cooling down after a failed request; retry in ${seconds}s.`,
  );
  error.coolingDown = true;
  error.retryAfterMs = Math.max(0, Number(waitMs) || 0);
  error.status = 0;
  return error;
}

// `store` is injectable so an owner that already exposes its failure map (the
// World root's `requestFailures`) keeps that identity instead of hiding a
// second copy behind this gate.
export function createWorldBackoff({
  store = new Map(),
  now = () => Date.now(),
  limit = WORLD_BACKOFF_MAX_KEYS,
} = {}) {
  const waitMs = (key) => {
    const failure = store.get(key);
    if (!failure) return 0;
    return Math.max(0, Number(failure.retryAt || 0) - now());
  };

  const fail = (key, failure) => {
    const attempts = Math.min(
      WORLD_BACKOFF_MAX_ATTEMPTS,
      Number(store.get(key)?.attempts || 0) + 1,
    );
    const delay = worldBackoffDelayMs(attempts, failure);
    if (!store.has(key) && store.size >= limit) {
      store.delete(store.keys().next().value);
    }
    store.set(key, { attempts, retryAt: now() + delay });
    return delay;
  };

  return Object.freeze({
    waitMs,
    fail,
    succeed: (key) => store.delete(key),
    attempts: (key) => Number(store.get(key)?.attempts || 0),
    clear: () => store.clear(),
    get size() {
      return store.size;
    },
  });
}

// The shape every raw fetch site uses: refuse while cooling down, clear the
// record on success, and grow the wait on failure. `retryableOnly` marks the
// call sites where a 4xx is an answer rather than an outage — user-driven
// writes, which must stay retryable the moment their input changes.
export async function withWorldBackoff(gate, key, run, options = {}) {
  const waiting = gate.waitMs(key);
  if (waiting > 0) throw worldCoolingDownError(key, waiting);
  try {
    const value = await run();
    gate.succeed(key);
    return value;
  } catch (error) {
    if (options.retryableOnly !== true || isRetryableWorldFailure(error)) {
      gate.fail(key, error);
    }
    throw error;
  }
}
