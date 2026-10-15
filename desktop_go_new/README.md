# desktop_go_new: forkmesh-core and the integrated desktop

The Go engine behind the ForkMesh desktop and the phone apps, plus an overlay
that builds a desktop wired to it. The engine runs on its own: it keeps working
after the window closes, can start at login, and hosts signed background
service plug-ins; optional desktop packages build as lazily loaded modules.
Outside this folder only the iOS mini-apps (`FMMiniApps`, `Features/MiniApps`) and
the row in `docs/README.md` change; the desktop and marketplace changes are overlay patches.
Documentation: [docs/README.md](docs/README.md),
[docs/service-plugins.md](docs/service-plugins.md).

```sh
desktop_go_new/run.sh
desktop_go_new/desktop-overlay/build.sh
cd desktop_go_new && env -u FORKMESH_HOME -u FORKMESH_PROJECTS_DIR -u FORKMESH_DATA_DIR \
    -u FORKMESH_CONFIG_DIR -u FORKMESH_PROJECT -u FORKMESH_PROJECT_DIR go test ./...
forkmesh-core start | stop | status
forkmesh-core background on|off
forkmesh-core autostart enable|disable|status
forkmesh-core plugins list|sign|verify|trust
```
