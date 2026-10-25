from __future__ import annotations

import importlib
import logging

ADDRESS_HEADERS = ("x-forwarded-for", "x-real-ip", "forwarded",
                   "true-client-ip")
GEO_HEADERS = ("cf-ipcountry",)


def module():
    try:
        return importlib.import_module("instance_privacy")
    except ImportError:
        return None


def active(env):
    found = module()
    return bool(found is not None and found.active(env))


async def load(env):
    found = module()
    if found is None:
        return False
    try:
        return bool(await found.load(env))
    except Exception:
        return bool(found.active(env))


def scrub_headers(headers, env):
    found = module()
    if found is None or not found.active(env):
        return False
    address = headers.get("cf-connecting-ip")
    if address:
        headers.set("cf-connecting-ip", found.pseudonym(address, env))
    for name in ADDRESS_HEADERS + GEO_HEADERS:
        if headers.has(name):
            headers.delete(name)
    return True


def masked_path(path):
    path = str(path or "")
    if path.startswith("/api/"):
        try:
            group = importlib.import_module("api_metrics")._masked_group(path)
        except Exception:
            group = "other"
        return "/api/" + str(group)
    if path in ("/", "/health", "/readyz"):
        return path
    return "/:page"


def log_path(env, path):
    return masked_path(path) if active(env) else path


class QuietLogFilter(logging.Filter):
    def __init__(self, env, below=logging.WARNING):
        super().__init__()
        self.env = env
        self.below = below

    def filter(self, record):
        return not (record.levelno < self.below and active(self.env))


def install_log_filters(env, names=("uvicorn.error", "uvicorn.access")):
    installed = []
    for name in names:
        logger = logging.getLogger(name)
        below = logging.CRITICAL + 1 if name.endswith(".access") \
            else logging.WARNING
        for existing in list(logger.filters):
            if isinstance(existing, QuietLogFilter):
                logger.removeFilter(existing)
        item = QuietLogFilter(env, below)
        logger.addFilter(item)
        installed.append(item)
    return installed
