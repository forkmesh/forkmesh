# Special Agents

Special Agents: named agents with their own instructions and skills, from a repository's .forkmesh folder. Needs Agents.

Surfaces: desktop, iPhone, Android.

The install state follows your ForkMesh account: removing it on one device
removes it from every device, and installing it brings it back everywhere.

It is off in projects and accounts created from 2026-10-11; older ones keep it
installed until it is removed (`install.installedBefore`).

It needs [agents](../agents/README.md): installing it installs that too, and removing that removes this.

The feature itself is built into each app; the package is its on/off
switch. See `docs/marketplace/core-plugins.md`.
