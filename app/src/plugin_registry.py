"""Empty source registry for optional application plugin sockets.

The deploy build replaces this module only inside ``app/src_build`` with a
registry discovered from Marketplace manifests.  Keeping the source registry
empty is deliberate: package identities and package code never become core
application source or checked-in generated output.
"""

PLUGIN_DASHBOARD_PAGE_ASSETS = {}
PLUGIN_DEVICE_PAGES = {}
PLUGIN_STATIC_HTML_PATHS = frozenset()
PLUGIN_WWW_SITE_ROUTES = frozenset()
HTTP_ROUTES = ()
PLUGINS = {}
MARKET_PACKAGES = ()


def api_prefixes():
    """Every package Worker API prefix, for host request placement.

    The stateless dispatcher needs to recognise package-owned API paths
    without naming, importing, or hard-coding any package.
    """
    return tuple(sorted(
        str(plugin.get("api") or "")
        for plugin in PLUGINS.values() if plugin.get("api")))


async def dispatch_http(request, env, path, method):
    """Return a plugin response for a staged route, or ``None`` in core."""
    del request, env, path, method
    return None
