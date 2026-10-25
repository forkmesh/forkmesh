# Embedded browser (ungoogled Chromium)

The Agents workspace's **Browser** tab and the side rail's browser are the
same widget, `WorkspaceBrowserPane` (library `forkmesh-browser-pane`). It
renders pages with Qt WebEngine, which is Chromium, and switches off the
parts of Chromium that talk to Google, following what
[ungoogled-chromium](https://github.com/ungoogled-software/ungoogled-chromium)
removes.

Code: `desktop/src/WorkspaceBrowserPane.*` (the widget, its profile and page),
`desktop/src/BrowserPrivacy.*` (the switches, the blocklist, the user agent and
address parsing), test `desktop/tests/test_browser_privacy.cpp`
(`forkmesh-browser-privacy-tests`).

## Build

`FORKMESH_BROWSER_PANE` is `ON` by default. When CMake finds Qt
WebEngineWidgets (the same `QUIET` lookup the agent browser windows use) the
pane embeds it; when it does not, the pane shows the address and opens pages in
the system browser, as before. `-DFORKMESH_BROWSER_PANE=OFF` forces the system
browser. On Debian and Ubuntu the package is `qt6-webengine-dev`.

The option used to default to `OFF` even though the app already linked
WebEngine for agent windows, so most builds had a Browser tab that could not
show a page.

## What "ungoogled" means here

We do not rebuild Chromium with the ungoogled-chromium patches; Qt WebEngine
is already a Chromium with Google's accounts, sync, Safe Browsing, crash upload
and metrics left out. On top of that the desktop turns off what remains at run
time.

**Chromium switches.** `main.cpp` calls
`browser::installUngoogledChromiumFlags()` before `QApplication` exists, so the
switches reach every WebEngine view in the process, agent browser windows
included. They are merged into `QTWEBENGINE_CHROMIUM_FLAGS`: the user's own
switches are kept and come after ours, so they can override, and a user
`--disable-features=` list is folded into ours (Chromium keeps only the last
copy of a switch). Set `FORKMESH_BROWSER_KEEP_GOOGLE=1` to skip it.

| Switch | Stops |
| --- | --- |
| `--disable-background-networking` | variations, component and other background fetches |
| `--disable-component-update` | component updater downloads from Google |
| `--disable-domain-reliability` | reliability beacons to `*.gvt2.com` |
| `--disable-breakpad` | crash reporting |
| `--disable-sync` | sync service |
| `--disable-client-side-phishing-detection` | phishing model downloads |
| `--no-pings` | `<a ping>` hyperlink auditing |
| `--disable-features=…` | autofill server, Certificate Transparency list updates, Cast discovery (`MediaRouter`, `DialMediaRouteProvider`), optimization hints, network time queries, Translate, feed suggestions, and the Privacy Sandbox ad APIs (Topics, FLEDGE, interest groups) |

Unknown feature names are ignored by Chromium, so the list is safe across
WebEngine versions.

**Blocked hosts.** The profile's request interceptor drops any request to the
Chromium service endpoints (`browser::isGoogleServiceHost`): update, Safe
Browsing, client services, optimization guide, autofill, sync passwords,
`clients2.google.com`, the `gvt1.com` download mirrors and `gvt2.com`. This is
the run-time stand-in for ungoogled-chromium's domain substitution. Google's
websites (search, Gmail, YouTube) still load; this is not an ad blocker.

**Profile and settings.** One profile, `forkmesh-browser`, is shared by every
pane. It keeps cookies and site data under `data/browser/profile` and its cache
under the cache root's `browser/` folder, so signing in to a site survives a
restart. Agent browser windows keep their own throwaway, memory-only profile.
The profile turns off spell-check dictionary downloads and the push service
(Firebase), sets hyperlink auditing and DNS prefetch off, limits WebRTC to the
public interface (no local IP leak), refuses unknown URL schemes and local
file access, and removes the `QtWebEngine/x.y` token from the user agent so
sites see an ordinary Chrome. Downloads go to the system Downloads folder.

**Navigation.** `browser::urlFromInput` turns a bare port (`3000`) or
`localhost:5173` into a local address, adds `http://` to a domain, and sends
anything with spaces or no dot to DuckDuckGo instead of Google. Only `http`
and `https` addresses are typed in; the page itself may also navigate to
`about:`, `data:` and `blob:` but never to `file:`. A link that asks for a new
window (`target=_blank`) opens in the same pane.
