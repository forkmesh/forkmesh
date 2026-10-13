# desktop_go_new: forkmesh-core and the integrated desktop

The Go engine behind the ForkMesh desktop and the phone apps, plus an overlay
that builds a desktop wired to it. Nothing outside this folder is changed.
Documentation: [docs/README.md](docs/README.md).

```sh
desktop_go_new/run.sh
desktop_go_new/desktop-overlay/build.sh
cd desktop_go_new && env -u FORKMESH_HOME -u FORKMESH_PROJECTS_DIR -u FORKMESH_DATA_DIR \
    -u FORKMESH_CONFIG_DIR -u FORKMESH_PROJECT -u FORKMESH_PROJECT_DIR go test ./...
```
