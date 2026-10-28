# Embedded browser (ungoogled Chromium)

The Agents workspace's **Browser** tab and the side rail's browser are the
same widget, `WorkspaceBrowserPane` (library `forkmesh-browser-pane`). It
renders pages with Qt WebEngine, which is Chromium, and switches off the
parts of Chromium that talk to Google, following what
[ungoogled-chromium](https://github.com/ungoogled-software/ungoogled-chromium)
removes.

Code: `desktop/src/WorkspaceBrowserPane.*` (the widget, its profile and page),
`desktop/src/BrowserPrivacy.*` (the switches, the blocklist, the user agent and
address parsing), `desktop/src/WebEngine.*` (the engine interface) and
`desktop/src/WebEngineQt.cpp` (the only file that includes Qt WebEngine), test
`desktop/tests/test_browser_privacy.cpp` (`forkmesh-browser-privacy-tests`).

## The engine is a module

Linking Qt WebEngine costs about 36 MB of resident memory in every process
that links it, before a page is opened: the library's relocations and startup
code, plus Qt Quick and QML, which it pulls in. Measured 2026-10-28 with a Qt
Widgets program that only shows an empty window, offscreen: 24 MB linked
without WebEngine, 60 MB with it. Most people never open the Browser tab or an
agent browser window, so since 2026-10-28 the app no longer links it on Linux:

- `WebEngineQt.cpp` is built as `packages/web-engine.so` with a sidecar
  (`web-engine.json`, entrypoint `core:web-engine`), next to the package
  modules and loaded by the same loader with the same API, ABI and trust
  checks ([package-modules](package-modules.md)). It is not a Market package
  and is never gated on an install.
- The core talks to it only through `forkmesh::web` (`WebEngine.h`): a profile
  factory, a view factory and a small `View` interface (load, history, title,
  scripts in the main or an isolated world, console messages, and url, title
  and load-finished listeners). The module calls `registerEngine` from a
  static initializer when it is loaded.
- The engine has no policy. Which URLs a page may open, which requests are
  blocked (Google's services for the Browser tab, everything but the widget
  allowlist for agent widgets), where downloads go and how widget events are
  read stay in core code and reach the engine as callbacks
  (`ProfileOptions`, `PageOptions`), so the security rules are tested where
  they always were.
- `web::available()` reads only the sidecar, so showing the Browser tab or the
  agent tools' "embedded" answer never loads the engine. The first
  `createProfile` or `createView` does (`modules::loadForEntrypoint`). A
  refused or missing module leaves the pane on its "opens in your own browser"
  placeholder and agent browser windows without a page.
- `main.cpp` still sets `AA_ShareOpenGLContexts` and the ungoogled Chromium
  switches before `QApplication` when the build has an engine
  (`FORKMESH_WEB_ENGINE_BUILT`), which is what lets Qt WebEngine start from a
  library loaded later.
- macOS and Windows, where package modules are off, compile `WebEngineQt.cpp`
  into the app as before (`forkmesh_embed_web_engine`); so do the test
  programs that drive real pages (`forkmesh-agent-windows-tests`, window and
  perf tests). `forkmesh-web-engine-module-tests` checks the module path: Qt
  WebEngine is not mapped after start, is mapped after the first profile, and
  a page loads and runs a script.

## Build

`FORKMESH_BROWSER_PANE` is `ON` by default. When CMake finds Qt
WebEngineWidgets (the same `QUIET` lookup the agent browser windows use) the
pane embeds it (`FORKMESH_BROWSER_PANE_EMBEDS`); when it does not, the pane shows the address and opens pages in
the system browser, as before. `-DFORKMESH_BROWSER_PANE=OFF` forces the system
browser. On Debian and Ubuntu the package is `qt6-webengine-dev`.

The installer's source build installs that package when it is missing
(`ensure_webengine` in `www/public/install.sh`: `qt6-webengine-dev`,
`qt6-qtwebengine-devel`, `qt6-webengine`, `qt6-webengine-devel` or
`qt6-qtwebengine-dev` by package manager). It is optional: a failed install only
warns, because the build works without it, but the Browser tab, agent browser
windows and the Video Mix DJ Studio's YouTube decks then open outside the app.

A build tree configured before WebEngine was installed caches
`Qt6WebEngineWidgets_DIR-NOTFOUND`, and Rebuild & restart and `desktop/run.sh`
skip the configure step, so installing the package used to change nothing until
someone deleted the build. When the lookup fails, CMake now adds Qt's CMake
folder (the parent of `Qt6_DIR`) to `CMAKE_CONFIGURE_DEPENDS`. Installing any Qt
module adds a folder there, Ninja sees the new modification time and re-runs
CMake, and the lookup finds WebEngine on the next build.

### Install embedded browser button

A build without WebEngine says "This build has no embedded browser" in the
pane, and next to it shows an **Install embedded browser** button
(`WorkspaceBrowserPane::setInstallHandler`, shown only when the pane was
compiled without `FORKMESH_HAVE_WEBENGINE`; an offscreen run that has WebEngine
does not offer it). The button calls `MainWindow::installEmbeddedBrowser`
(`MainWindowSettings.cpp`), which reuses the Settings → Apps install path: the
package names live in `SuggestedApps.cpp` under the `webengine` id, the same
list as `ensure_webengine` plus Homebrew's `qt`, and
`embeddedBrowserComponent()` describes it without adding a row to the Apps
list (there is no executable to find on `PATH`). It asks before running the
package manager, goes through `pkexec` when it needs root (or shows the
`sudo` command when `pkexec` is missing), and on success starts Rebuild &
restart; the configure-depends folder above makes that build find WebEngine.
Windows has no package for it, so the button there only explains what to
install.

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
copy of a switch). Qt WebEngine splits that variable on plain spaces with no
quoting, so the merge splits it the same way; a switch whose value needs a
space has to go on the command line instead. Set
`FORKMESH_BROWSER_KEEP_GOOGLE=1` to skip it.

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
