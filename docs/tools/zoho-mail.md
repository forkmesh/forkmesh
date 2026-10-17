# Sending email through Zoho Mail

`tools/zoho_mail.py` sends an email from a Zoho Mail mailbox with no prompts,
so an agent given a request like "email the weekly summary to
ops@example.com" can send it and simply report that it went out. Agents call it
through the ForkMesh MCP tool `send_email`, or run the CLI from a shell.

Every email it sends is also logged in the CRM (see
[Logged in the CRM](#logged-in-the-crm)).

It is unrelated to platform mail (Mailtrap and the cores' Postfix,
[../MAIL_LOG.md](../MAIL_LOG.md)) and to the CRM's Zoho connection, which
reads mail and, since CRM 1.36.0, sends the CRM's automated emails through
the Zoho Mail API (`ZohoMail.messages.CREATE`,
`marketplace/packages/crm/README.md`, "Automated outreach email"). This tool
sends as one person's mailbox from their own machine.

## Code map

| Piece | Where |
| --- | --- |
| SMTP sender, setup, check, CLI | `tools/zoho_mail.py` |
| MCP tool `send_email` | `tool_send_email` in `tools/forkmesh_mcp_server.py` |
| CRM logging after a send | `record_sent_email`, `crm_record_sent_email` in `tools/forkmesh_mcp_server.py` |
| Tests (fake SMTP, sandboxed paths) | `tools/test_zoho_mail.py` |

## Set up once

Zoho does not let SMTP sign in with the account password once two-factor
sign-in is on, so make an app password: Zoho Accounts → Security → App
Passwords → Generate. Then:

```sh
python3 tools/zoho_mail.py setup --address you@example.com --from-name "Example Team"
```

It asks for the app password (or reads it from stdin with `--password-stdin`),
saves `<config>/zoho-mail.json` with mode `0600` (`<config>` is
`~/projects/forkmesh/config` in the projects layout,
[projects-layout.md](projects-layout.md)) and signs in once to prove it works.

- `--region` picks the data centre: `com` (default), `eu`, `in`, `com.au`,
  `jp`, `ca`, `sa`.
- Mailboxes on your own domain use `smtppro.zoho.<region>`, the default.
  Free `@zoho.com` mailboxes need `--personal` (`smtp.zoho.<region>`).
  `--host` sets any other host.
- `python3 tools/zoho_mail.py check` signs in again without sending.

Environment variables override the file, for CI or a one-off account:
`ZOHO_MAIL_ADDRESS`, `ZOHO_MAIL_APP_PASSWORD`, `ZOHO_MAIL_HOST`,
`ZOHO_MAIL_PORT`, `ZOHO_MAIL_FROM_NAME`, and `ZOHO_MAIL_CONFIG` (another
config file path).

## Sending

From an agent, the MCP tool:

```json
{"name": "send_email", "arguments": {
  "to": ["ada@example.org"], "cc": "bob@example.org",
  "subject": "Weekly summary", "body": "Here is this week's summary."}}
```

From a shell:

```sh
python3 tools/zoho_mail.py send --to ada@example.org --subject "Weekly summary" --body "Here is this week's summary."
echo "body from a pipe" | python3 tools/zoho_mail.py send --to ada@example.org --subject Hi
```

Both return a confirmation such as
`Sent to ada@example.org: Weekly summary (<...@example.com>)`; the CLI's
`--json` and the MCP reply also give the full result (Message-ID, recipients,
any refused addresses, time). The CLI exits `1` with the reason when nothing
was sent.

- `to`, `cc` and `bcc` take one address, a comma-separated list or an array;
  `Name <a@example.org>` is accepted. Bcc never appears in the headers.
- `html: true` (`--html`) sends the body as HTML with a plain-text copy.
- `reply_to` sets one Reply-To address.

## Logged in the CRM

After Zoho accepts a message, both the CLI and `send_email` record it in the
CRM, so outreach sent this way shows up next to the rest of the contact's
history:

- Recipients on the sender's own domain (colleagues on cc) are left out.
  Everyone else (to, cc and bcc) is linked through the CRM's
  `POST /api/crm/<org>/emails/link` as an outbound Zoho email: the contact is
  matched by address, or created with source `zoho email` when the CRM does
  not know them, and its last-contacted date moves forward. A contact whose
  pipeline has a `contacted` stage and who sits before it (`sourced`,
  `prospect`) moves to `contacted` (CRM 1.33.0; the CRM's Zoho Mail sync does
  the same for its Sent folder, see `marketplace/packages/crm/README.md`).
- Each of them joins the **Initiatives** list. The list is created the first
  time it is needed and matched by name (any case) after that.
- The organization is the one saved with `setup --crm-organization`, else the
  account's only CRM organization; with several and none saved, nothing is
  logged and the reply says so. `setup --crm-list` changes the default list.
  `ZOHO_MAIL_CRM_ORGANIZATION` and `ZOHO_MAIL_CRM_LIST` override both.
- Per email: `crm: false`, `crm_organization`, `crm_list` on the MCP tool;
  `--no-crm`, `--crm-organization`, `--crm-list` on the CLI.
- Requests are signed as the desktop's signed-in account, like the other CRM
  tools. A CRM failure never turns a sent email into an error: the reply adds
  `CRM not updated: <reason>` under the `Sent to ...` line.
- The CRM row's external id is the Message-ID followed by the recipient's
  address, one row per person, so sending again logs a new email rather than
  overwriting the last one.

## Rules and limits

- Nothing is sent without a recipient, subject and body. Every address is
  checked, and a line break in an address or subject is refused, so a request
  cannot inject headers.
- At most 50 recipients and a 2 MiB body per message.
- The From address is always the configured mailbox; Zoho refuses others.
- Each send appends one line (recipients, subject, Message-ID, time, never the
  body) to `<logs>/zoho-mail-sent.jsonl`, mode `0600`.
- `send_email` is an MCP write tool: once the node publishes an MCP
  connector it needs the matching `FORKMESH_MCP_TOKEN`, like every other write
  ([../agents/tools.md](../agents/tools.md#forkmesh-mcp-server)). There is no
  confirmation step by design: the request that asks for the email is the
  confirmation.
- The app password is never printed or returned by any tool.
