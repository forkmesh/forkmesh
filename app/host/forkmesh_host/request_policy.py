from __future__ import annotations

import asyncio
import ipaddress
import json
import os
import socket
import time

SELECT_RULES = (
    "SELECT rule_id, match_type, pattern FROM request_block_rules "
    "WHERE enabled=1 ORDER BY priority DESC, created_at, rule_id LIMIT 1000")
REFRESH_S = 60
RETRY_S = 10
MAX_KEYWORD = 256
GOOGLE_KEYWORD = "google"
GOOGLE_DOMAINS = (".googlebot.com", ".google.com", ".googleusercontent.com")
WELCOME_AGENTS = ("chatgpt-user", "oai-searchbot")
CRAWLER_CACHE_S = 86400
CRAWLER_LOOKUP_S = 2.0
MAX_CRAWLER_CACHE = 4096
DEFAULT_RATE_REQUESTS = 240
DEFAULT_RATE_PERIOD_S = 10
DEFAULT_RATE_TIMEOUT_S = 30
MAX_RATE_CLIENTS = 50000
EXEMPT_PATHS = frozenset({"/health", "/readyz"})
EXEMPT_PREFIXES = ("/api/fleet/",)
EXPORT_ENV = "FORKMESH_REQUEST_RULES_FILE"
EXPORT_VERSION = 1


def _int_env(name, default, minimum=0):
    try:
        value = int(os.environ.get(name, "") or default)
    except ValueError:
        return default
    return value if value >= minimum else default


def welcome_agent(user_agent):
    agent = str(user_agent or "").lower()
    return any(token in agent for token in WELCOME_AGENTS)


def parse_ip(value):
    text = str(value or "").strip()
    if text.startswith("[") and "]" in text:
        text = text[1:text.index("]")]
    try:
        address = ipaddress.ip_address(text.split("%")[0])
    except ValueError:
        return None
    mapped = getattr(address, "ipv4_mapped", None)
    return mapped or address


def rate_key(address):
    if address.version == 6:
        return str(ipaddress.ip_network((address, 64), strict=False))
    return str(address)


class Rules:
    def __init__(self, keywords=(), networks=(), ranges=()):
        self.keywords = tuple(keywords)
        self.networks = tuple(networks)
        self.ranges = tuple(ranges)

    def __len__(self):
        return len(self.keywords) + len(self.networks) + len(self.ranges)

    @classmethod
    def from_rows(cls, rows):
        keywords, networks, ranges = [], [], []
        for row in rows or ():
            kind = str(row.get("match_type") or "").strip().lower()
            pattern = str(row.get("pattern") or "").strip()
            if not pattern:
                continue
            try:
                if kind == "user_agent_keyword":
                    if len(pattern) <= MAX_KEYWORD:
                        keywords.append(pattern.lower())
                elif kind == "ip":
                    address = parse_ip(pattern)
                    if address is not None:
                        networks.append(ipaddress.ip_network(address))
                elif kind == "cidr":
                    networks.append(ipaddress.ip_network(pattern, strict=False))
                elif kind == "ip_range":
                    start_text, end_text = pattern.split("-", 1)
                    start = ipaddress.ip_address(start_text.strip())
                    end = ipaddress.ip_address(end_text.strip())
                    if start.version == end.version and start <= end:
                        ranges.append((start, end))
            except ValueError:
                continue
        return cls(keywords, networks, ranges)

    def address_blocked(self, address):
        if address is None:
            return False
        for network in self.networks:
            if address.version == network.version and address in network:
                return True
        for start, end in self.ranges:
            if address.version == start.version and start <= address <= end:
                return True
        return False

    def agent_blocked(self, user_agent):
        agent = str(user_agent or "").lower()
        return any(keyword in agent for keyword in self.keywords)

    def export(self):
        return json.dumps({
            "version": EXPORT_VERSION,
            "agents": sorted(set(self.keywords)),
            "networks": sorted({str(network) for network in self.networks}),
            "ranges": sorted({(str(start), str(end))
                              for start, end in self.ranges}),
        }, sort_keys=True) + "\n"


def write_export(path, text):
    try:
        with open(path, encoding="utf-8") as handle:
            if handle.read() == text:
                return False
    except OSError:
        pass
    temporary = "%s.%d.tmp" % (path, os.getpid())
    with open(temporary, "w", encoding="utf-8") as handle:
        handle.write(text)
    os.replace(temporary, path)
    return True


def _forward_confirmed(ip):
    try:
        host = socket.gethostbyaddr(ip)[0].rstrip(".").lower()
    except OSError:
        return False
    if not host.endswith(GOOGLE_DOMAINS):
        return False
    try:
        answers = socket.getaddrinfo(host, None)
    except OSError:
        return False
    target = parse_ip(ip)
    return any(parse_ip(answer[4][0]) == target for answer in answers)


class CrawlerVerifier:
    def __init__(self, lookup=_forward_confirmed, clock=time.monotonic,
                 ttl=CRAWLER_CACHE_S, timeout=CRAWLER_LOOKUP_S):
        self.lookup = lookup
        self.clock = clock
        self.ttl = ttl
        self.timeout = timeout
        self._cache = {}

    async def verified(self, ip):
        now = self.clock()
        cached = self._cache.get(ip)
        if cached is not None and cached[1] > now:
            return cached[0]
        loop = asyncio.get_running_loop()
        try:
            result = bool(await asyncio.wait_for(
                loop.run_in_executor(None, self.lookup, ip), self.timeout))
        except (asyncio.TimeoutError, OSError):
            return False
        if len(self._cache) >= MAX_CRAWLER_CACHE:
            self._cache.clear()
        self._cache[ip] = (result, now + self.ttl)
        return result


class RateLimiter:
    def __init__(self, requests=None, period=None, timeout=None,
                 clock=time.monotonic, max_clients=MAX_RATE_CLIENTS):
        self.requests = requests if requests is not None else _int_env(
            "FORKMESH_RATE_LIMIT_REQUESTS", DEFAULT_RATE_REQUESTS)
        self.period = period if period is not None else _int_env(
            "FORKMESH_RATE_LIMIT_PERIOD_SECONDS", DEFAULT_RATE_PERIOD_S, 1)
        self.timeout = timeout if timeout is not None else _int_env(
            "FORKMESH_RATE_LIMIT_TIMEOUT_SECONDS", DEFAULT_RATE_TIMEOUT_S)
        self.clock = clock
        self.max_clients = max_clients
        self._clients = {}

    @property
    def enabled(self):
        return self.requests > 0

    def _prune(self, now):
        if len(self._clients) < self.max_clients:
            return
        for key, (start, _count, until) in list(self._clients.items()):
            if now - start >= self.period and until <= now:
                del self._clients[key]
        if len(self._clients) >= self.max_clients:
            self._clients.clear()

    def retry_after(self, key):
        if not self.enabled:
            return 0
        now = self.clock()
        state = self._clients.get(key)
        if state is None:
            self._prune(now)
            state = self._clients[key] = [now, 0, 0.0]
        if state[2] > now:
            return max(1, int(state[2] - now + 0.999))
        if now - state[0] >= self.period:
            state[0], state[1] = now, 0
        state[1] += 1
        if state[1] <= self.requests:
            return 0
        wait = max(self.timeout, self.period - (now - state[0]))
        state[2] = now + wait
        return max(1, int(wait + 0.999))


class RequestPolicy:
    def __init__(self, *, limiter=None, verifier=None, clock=time.monotonic,
                 refresh=REFRESH_S, export_path=None):
        self.limiter = limiter or RateLimiter()
        self.verifier = verifier or CrawlerVerifier()
        self.clock = clock
        self.refresh = refresh
        self.rules = Rules()
        self.loaded = False
        self._next_load = 0.0
        self._task = None
        self.export_path = (os.environ.get(EXPORT_ENV, "").strip()
                            if export_path is None else export_path)
        self.stats = {"blocked": 0, "rate_limited": 0, "load_failures": 0,
                      "exports": 0, "export_failures": 0}

    async def load(self, env):
        db = getattr(env, "DB", None) if env is not None else None
        if db is None:
            self.rules, self.loaded = Rules(), True
            self._next_load = self.clock() + self.refresh
            return True
        try:
            result = await db.prepare(SELECT_RULES).all()
            rows = getattr(result, "results", None)
            if rows is None and isinstance(result, dict):
                rows = result.get("results")
            self.rules = Rules.from_rows([dict(row) for row in rows or []])
            self.loaded = True
            self._next_load = self.clock() + self.refresh
            self._export()
            return True
        except Exception as error:
            self.stats["load_failures"] += 1
            self._next_load = self.clock() + RETRY_S
            print("request policy load failed: %r" % (error,))
            return False

    def _export(self):
        if not self.export_path:
            return
        try:
            if write_export(self.export_path, self.rules.export()):
                self.stats["exports"] += 1
        except OSError as error:
            self.stats["export_failures"] += 1
            print("request policy export failed: %r" % (error,))

    def _schedule(self, env):
        if self.clock() < self._next_load:
            return
        if self._task is not None and not self._task.done():
            return
        self._task = asyncio.ensure_future(self.load(env))

    async def blocked(self, env, ip, user_agent):
        self._schedule(env)
        if not self.loaded and self._task is not None:
            await asyncio.wait({self._task}, timeout=CRAWLER_LOOKUP_S)
        address = parse_ip(ip)
        if self.rules.address_blocked(address):
            return True
        if welcome_agent(user_agent):
            return False
        if not self.rules.agent_blocked(user_agent):
            return False
        if (address is not None and GOOGLE_KEYWORD in str(user_agent).lower()
                and await self.verifier.verified(str(address))):
            return False
        return True

    @staticmethod
    def exempt(path):
        return path in EXEMPT_PATHS or path.startswith(EXEMPT_PREFIXES)

    async def check(self, env, path, ip, user_agent, loopback_peer):
        if self.exempt(path):
            return None
        if await self.blocked(env, ip, user_agent):
            self.stats["blocked"] += 1
            return 403, 0
        address = parse_ip(ip)
        if address is None or (loopback_peer and address.is_loopback):
            return None
        wait = self.limiter.retry_after(rate_key(address))
        if wait:
            self.stats["rate_limited"] += 1
            return 429, wait
        return None

    async def close(self):
        if self._task is not None and not self._task.done():
            self._task.cancel()
            await asyncio.gather(self._task, return_exceptions=True)
        self._task = None
