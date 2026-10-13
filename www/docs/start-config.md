# Start your own instance from a settings file

The [Start your own instance](https://app.forkmesh.com/start) page can be
filled in from one zip file that holds your settings and your logo. Upload
the zip (or drop it on the page) and every field is set: project name, logo,
server, address, admin account, organization owners and email. When nothing
is missing, the page shows **Start my instance now**, so the whole setup is
one click.

Download the example:
[start-config.example.zip](https://app.forkmesh.com/start-config.example.zip).
It holds two files:

- `forkmesh-instance.json`, the settings below;
- `logo.svg`, the logo.

Edit the settings, put your own logo in place of `logo.svg`, and zip the two
files again. The settings alone are also
[start-config.example.json](https://app.forkmesh.com/start-config.example.json),
and the page's **Copy example settings** button copies them to the clipboard.

## The zip

- It needs exactly one `.json` file: the settings.
- The logo is the image the settings name in `logo` (for example
  `"logo": "logo.svg"`). Without a name, the zip's only image is the logo.
  With several images, name the one to use.
- The logo is a PNG, JPEG, WebP or SVG image of up to 200 KB; the zip is up
  to 1 MB.
- Files in a folder inside the zip are found too; `__MACOSX` folders and
  hidden files are skipped, so a zip made with Finder or Windows works.
- Zips with a password cannot be read.

You can still upload the `.json` file on its own; then the logo is not
included, and you can add it with the form's **Upload logo**.

## The file

```json
{
  "projectName": "Acme Code",
  "logo": "logo.svg",
  "host": "203.0.113.10",
  "sshUser": "root",
  "port": 22,
  "sshPassword": "change-me-ssh-password",
  "sshPrivateKey": "",
  "sshPassphrase": "",
  "hostKeyFingerprint": "",
  "domain": "code.example.com",
  "adminUsername": "ada",
  "adminEmail": "ada@example.com",
  "adminPassword": "change-me-admin-password",
  "ownerType": "org",
  "ownerName": "acme",
  "owners": [
    {
      "username": "grace",
      "email": "grace@example.com",
      "password": "change-me-grace-password"
    },
    {
      "username": "linus",
      "email": "linus@example.com",
      "password": "change-me-linus-password"
    }
  ],
  "mailProvider": "none",
  "mailApiKey": "",
  "mailFrom": "",
  "mailgunDomain": "",
  "mailgunRegion": "us",
  "ownWorld": true,
  "listOnHomepage": true
}
```

Every setting is optional in the file: anything left out keeps what the form
already has, and the page then points at the first field that still needs a
value. Keys that start with `_` or `$` (such as `"_docs"`) are notes and are
skipped. Any other key the page does not know is listed after the upload and
skipped.

| Setting | Form field | Value |
| --- | --- | --- |
| `projectName` | Project name | Up to 40 letters, numbers, spaces and `._&+-` |
| `theme` | Colour theme | Optional; `crimson`, or empty for ForkMesh's default colours |
| `logo` | Logo | Optional; the name of the logo image in the zip, such as `logo.svg` (a `data:image/…;base64,…` URL also works) |
| `host` | Server IP address | The server's public IP address or host name |
| `sshUser` | SSH user | Default `root` |
| `port` | SSH port | 1 to 65535, default `22` |
| `sshPassword` | SSH password | Sign in with a password |
| `sshPrivateKey` | Private key | Sign in with a key instead; the whole key file as one string |
| `sshPassphrase` | Key passphrase | Only if the key has one |
| `hostKeyFingerprint` | Host key fingerprint | Optional, `SHA256:` followed by 43 characters |
| `domain` | Domain | Optional; empty starts on a free `sslip.io` address |
| `adminUsername` | Username | Lowercase letters, numbers and dashes, starting with a letter |
| `adminEmail` | Email | The admin's email address |
| `adminPassword` | Password | At least 10 characters |
| `ownerType` | Repository belongs to | `org` (an organization) or `user` (the admin) |
| `ownerName` | Organization name | Used when `ownerType` is `org`; must differ from the admin username |
| `owners` | Other owners | Optional, `org` only; up to 20 people who own the organization with the admin, each `{"username", "email", "password"}` |
| `mailProvider` | Email service | `none`, `mailtrap`, `sendgrid` or `mailgun` |
| `mailApiKey` | API key | Needed unless `mailProvider` is `none` |
| `mailFrom` | Send from | Optional, default `no-reply@<domain>` |
| `mailgunDomain` | Mailgun sending domain | Needed for Mailgun |
| `mailgunRegion` | Mailgun region | `us` or `eu` |
| `ownWorld` | Give my instance its own world | `true` (default) or `false`; a hexagon joined to the ForkMesh World with a small landmark and your logo |
| `listOnHomepage` | List my instance on forkmesh.com | `true` (default) or `false`; your name and logo in the homepage's list of instances |

### Other owners

The admin always owns the organization. Everyone listed in `owners` gets an
account on your instance and owns the organization too, with the same rights
as the admin there (they are not instance admins). Each needs:

- `username`: lowercase letters, numbers and dashes, starting with a letter;
  different from the admin, the organization and the other owners;
- `email`: an address of their own;
- `password`: at least 10 characters. Give it to them; they can change it
  after they sign in.

On the page they appear under **Other owners** in section 4, where you can
add more with **Add an owner** or remove them.

### Password or key

Give `sshPassword` or `sshPrivateKey`. When the file has a key, the page signs
in with the key. A key goes in as one JSON string with `\n` line breaks; to
turn a key file into that string:

```sh
python3 -c 'import json,sys; print(json.dumps(open(sys.argv[1]).read()))' ~/.ssh/id_ed25519
```

You can also leave the key out of the file and upload it in the form's
**Private key** box after loading the settings.

## Keep the file private

The file holds your server's password or key, your admin password and the
other owners' passwords. The page reads it in your browser only: it is not uploaded on its own and not
kept. Only the settings sent with **Start my instance** go to ForkMesh, the
same as when you type them, and the secret fields are cleared once the setup
starts. Delete the file, or remove the passwords from it, when your instance
is running.

What happens after you press start, step by step, is on the page itself and
in `docs/INSTANCES.md` in the source.
