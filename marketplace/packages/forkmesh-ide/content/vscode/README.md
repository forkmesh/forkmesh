# ForkMesh for VS Code

This is the package-owned source for ForkMesh's optional VS Code-compatible
editor extension. It is catalogued in the ForkMesh Marketplace with
`install.default: false`; the base desktop application neither installs nor
activates it.

The current ForkMesh Marketplace host does not install VSIX files. Until an
IDE runtime is added, this package is presented as catalog/source content and
can be built manually for development:

```sh
cd marketplace/packages/forkmesh-ide/content/vscode
npm ci
npm run compile
npm run package
code --install-extension forkmesh-0.1.1.vsix
```

The `package` script expects the VS Code Extension Manager (`vsce`) to be
installed on `PATH`; it is not needed for TypeScript compilation.

The extension contributes a ForkMesh issue sidebar and commands that start
Claude Code or Codex in the open repository, plus a **ForkMesh: Become a
Member** command (heart icon in the sidebar title and a sidebar entry) that
opens <https://forkmesh.com/#membership>. Its local desktop handoff protocol
is retained in the package for compatibility, but no handoff listener is part
of the base ForkMesh desktop application.
