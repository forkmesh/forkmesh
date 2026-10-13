# ForkMesh desktop app (Qt)

Build and launch from the repository root so ForkMesh records the whole checkout
as its workspace:

```sh
./desktop/run.sh
```

Run the sub-minute critical client contracts:

```sh
./desktop/run.sh test
```

Other commands: `./desktop/run.sh clean` (delete `desktop/build`) and
`./desktop/run.sh rebuild` (clean, build, launch).

The build needs Qt 6.4+, CMake 3.16+, a C++17 compiler, OpenSSL 3.0+ and
Python 3. Private mirrors and the Password Vault need OpenSSL 3.5 at run time;
on Ubuntu 24.04 and Debian 12 (OpenSSL 3.0) configure once with
`-DFORKMESH_BUNDLED_OPENSSL=ON` to build a pinned 3.5
([details](../docs/desktop/build-and-test.md#openssl)). For dependencies, first-run setup, UI workflows, pull requests, data
locations, logs, and troubleshooting, use the
[Qt client guide](../www/docs/qt-client.md).

Developer documentation (code map, architecture, build and test internals,
diagnostics, networking, Windows, agents): [docs/desktop/](../docs/desktop/README.md).

New to ForkMesh? Start with [Getting started](../www/docs/getting-started.md).
Changing the project? Read [Contributing](../CONTRIBUTING.md) before opening a
pull request.
