# Start your own instance

A ForkMesh instance is your own Git host on your own server, under your own
name: repositories, pull requests, issues, chat, Actions and agents, with
your logo, colours and domain. Start one at
[forkmesh.com/start](https://forkmesh.com/start). No ForkMesh account is
needed, and it takes about fifteen minutes.

## What you need

- A fresh server with Debian 13 or Ubuntu 24.04, x86_64 or arm64, at least
  1 GB RAM and 6 GB free disk, with ports 80 and 443 free.
- SSH access as `root`, or as a user with passwordless `sudo`, by password
  or private key.
- Optional: a domain whose `A` record points at the server. Without one the
  instance gets a free `<ip-with-dashes>.sslip.io` address. A domain behind
  Cloudflare's proxy needs SSL/TLS mode Full (strict).
- Optional: a Mailtrap, SendGrid or Mailgun key, so the instance can send
  sign-up and password emails.

## Three ways to start

1. **Fill in the page.** Six short sections: name, logo and colour theme;
   your server; your address; your admin account and repository owner;
   email; and whether to list the instance on forkmesh.com and give it its
   own World.
2. **Upload a settings file.** Put every answer in one `.json` file, or a zip
   with the file and your logo, and upload it at the top of the page. See
   [Start from a settings file](start-config.md).
3. **Let your coding agent do it.** Copy the prompt from the page into
   Claude Code or another coding agent. It reads the
   [agent guide](https://app.forkmesh.com/forkmesh-agent.md), asks for your
   details and writes the settings file for you. See
   [Working with coding agents](coding-agents.md).

## What happens

ForkMesh connects to your server over SSH, checks the host key and runs the
installer. The page shows each step live: system check, packages, source,
build, configure, database, services, HTTPS, admin and repository. Your SSH
password or key is used once to connect and is never stored; secret fields
are cleared from the page once setup starts. If the connection drops, the
setup keeps running on the server.

When it is done:

- **Sign in within five minutes.** An instance started from the page that
  nobody signs in to removes itself, so an abandoned launch does not leave a
  server running.
- **Save your admin console address.** The page makes a private, random
  address such as `/admin-…`; `/admin` answers "not found". ForkMesh does
  not keep it, and the page shows it only until you leave.
- Your instance hosts its own copy of the platform source at
  `<owner>/forkmesh`.

## Running it yourself

If you would rather no secret passes through forkmesh.com, run the installer
on the server with the same settings file, without the SSH fields. The
installer also needs `domain` (use `<ip-with-dashes>.sslip.io` if you have
none), `publicAddress` (the server's public IP) and `adminPassword`:

```sh
curl -fsSL https://forkmesh.com/install-instance.sh -o install-instance.sh
sudo bash install-instance.sh --config forkmesh-instance.json
```

A manual install never removes itself. It joins the forkmesh.com directory
and the World only when you turn that on in the admin console, under
Platform → ForkMesh directory.

## After setup

- **Update** from the admin console (Platform → Update ForkMesh), or with
  `sudo bash install-instance.sh --update`. The new version builds beside
  the running one; data, secrets and the admin account are kept.
- **Update when a pull request is merged.** The console's Update page can
  rebuild the instance each time a pull request is merged into its own
  `<owner>/forkmesh` source. It refuses until that branch is protected, so
  only a change an owner or admin approved can deploy.
- **Branding.** The project name replaces "ForkMesh" across pages and email.
  Pick a ready theme or your own colours at setup; an admin can change the
  theme later under Settings → Instance theme. The "Powered by ForkMesh"
  badge is on by default.
- **Plugins.** Turn optional features on and off in the admin console,
  Platform → Plugins, without a reinstall. Examples: the copyable "fix this
  issue" prompt for coding agents and the Fediverse panels, both on by
  default.
- **Actions.** The installer adds a runner for repository workflows; an
  instance works without it if it fails.
- **Directory and World.** A listed instance appears on the forkmesh.com
  homepage and gets its own place in the ForkMesh World. Either can be
  switched off in the admin console.

Developer details (the API, installer steps, themes, plugins, white label and
limits) are in [docs/INSTANCES.md](../../docs/INSTANCES.md).
