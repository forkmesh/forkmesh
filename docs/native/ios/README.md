# iOS core modules

API notes for the Swift package `native/ios/ForkMeshCore`, which holds
everything in the iPhone app that is not a SwiftUI view and builds and tests on
Linux. Each page covers one module (or two closely tied ones): its public
types, how the app target wires it, and the surprises worth knowing before
editing it. Layout and build steps are in
[native/ios/README.md](../../../native/ios/README.md).

| Page | Covers |
| --- | --- |
| [FMBase.md](FMBase.md) | FMBase (ordered JSON, encodings, formatting, config, storage protocols, clock, log, services) and FMNet (HTTP and WebSocket behind protocols, with fakes) |
| [FMCrypto.md](FMCrypto.md) | Every cryptographic primitive, owner crypto, and chat protocol v3 and v4 sealing |
| [FMAccount.md](FMAccount.md) | Settings, relays, auth and the session, performance monitor, fresh-install reset, permission pacing, trust lists |
| [FMApi.md](FMApi.md) | The REST client and account services, the wire models, the account and shell view models |
| [FMChat.md](FMChat.md) | Relay chat, the local mesh room, local control, direct messages, calls, chat history |
| [FMCode.md](FMCode.md) | Remote repositories, issues and pull requests, and on-device Git |
| [FMAgents.md](FMAgents.md) | The on-device coding agent, providers, sessions, quick launch |
| [FMLocalAI.md](FMLocalAI.md) | On-device models: catalog, device fit, download, engine protocol, mesh node |
| [FMAssistant.md](FMAssistant.md) | The on-device Gemma assistant over the user's own data |
| [FMTools.md](FMTools.md) | Activity, telemetry, notes, tasks, health tools, Docs library, bug reports |
| [FMPackages.md](FMPackages.md) | Organization packages and the Market |
| [FMVault.md](FMVault.md) | The Password Vault: fmv1 suite, client, service, screens |

Modules without a note of their own are covered by the feature pages in
[../README.md](../README.md) (for example FMWorkflows in
[WORKFLOWS.md](../WORKFLOWS.md), FMControl in
[DESKTOP_AGENTS.md](../DESKTOP_AGENTS.md), the per-area modules in
[DESKTOP_PARITY.md](../DESKTOP_PARITY.md)). Keys and signatures:
[encryption](../../encryption.md); where the phone sits:
[architecture](../../architecture.md).

Back to [phone apps](../README.md).
