# Start your own instance from a settings file

The [Start your own instance](https://app.forkmesh.com/start) page can be
filled from one zip holding a settings `.json` file and a logo. Examples:
[start-config.example.zip](https://app.forkmesh.com/start-config.example.zip)
and [start-config.example.json](https://app.forkmesh.com/start-config.example.json).

## The zip

Exactly one `.json` file; the logo is the image named by `logo`, or the only
image. Logo: PNG, JPEG, WebP or SVG up to 200 KB; zip up to 1 MB; no password.
Hidden files and `__MACOSX` folders are skipped.

## Settings

Every key is optional; missing keys keep the form's value. Keys starting with
`_` or `$` are notes; unknown keys are listed and skipped.

| Setting | Value |
| --- | --- |
| `projectName`, `theme`, `logo` | Name (up to 40 characters), optional theme, logo file name or data URL |
| `host`, `sshUser`, `port` | Server address, SSH user (default `root`), port (default `22`) |
| `sshPassword`, `sshPrivateKey`, `sshPassphrase` | Password, or a key as one JSON string with `\n` breaks |
| `hostKeyFingerprint` | Optional `SHA256:` fingerprint |
| `domain` | Optional; empty uses a free `sslip.io` address |
| `adminUsername`, `adminEmail`, `adminPassword` | Admin account; password at least 10 characters |
| `ownerType`, `ownerName` | `org` or `user`; organization name when `org` |
| `owners` | Up to 20 extra organization owners, each `{"username", "email", "password"}` |
| `mailProvider`, `mailApiKey`, `mailFrom` | `none`, `mailtrap`, `sendgrid` or `mailgun`; key; sender |
| `mailgunDomain`, `mailgunRegion` | Mailgun only; region `us` or `eu` |
| `ownWorld`, `listOnHomepage` | Booleans, default `true` |

## Privacy

The file contains secrets. The page reads it in the browser only; just the
submitted settings reach ForkMesh, and secret fields are cleared once setup
starts. Delete the file afterwards.
