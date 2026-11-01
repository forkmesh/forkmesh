# Phone apps

Guides and contracts for the ForkMesh iPhone, Apple Watch and Android apps. A
phone is a mobile node of your account: it holds its own keys, clones and
commits with its own Git engine, reads your encrypted agent sessions, and can
control your desktop. Which app does what is on [ForkMesh apps](../apps.md);
how the apps are built is on the [phone app internals](../native/README.md)
home page.

| Page | Covers |
| --- | --- |
| [Mobile node contract](mobile-node-contract.md) | The wire and policy contract that makes a phone a node of your account |
| [Owned Git engine](owned-git-engine-spec.md) | The Swift and Kotlin Git engine the phones carry, and its security boundary |
| [Agent access](mobile-agent-access.md) | Reading encrypted agent sessions, queueing prompts and controlling a desktop from the phone |
| [File editing](mobile-file-editing.md) | Editing one file and opening a pull request from the phone, with no clone or fork |
| [Siri](siri.md) | Siri and Shortcuts commands for agents on iPhone |
| [Apple Health](apple-health.md) | The seven optional tools that read or write Apple Health, and what reaches your account |

Core pages: [architecture](../architecture.md),
[encryption](../encryption.md), [mirroring](../mirroring.md),
[censorship resilience](../censorship-resilience.md). Back to the
[documentation index](../README.md).
