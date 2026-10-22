# Privacy mode

Privacy mode is an admin setting for self-hosted instances. When it is on, the
instance keeps no records about visitors or users beyond what it needs to run
and to diagnose faults. It sends nothing about them to anyone. Diagnostics
that remain hold no personal data: error class, route group, status and
counts, kept for a short time.

forkmesh.com does not offer privacy mode. Its console page says the setting
is for self-hosted instances.

## Turning it on

There are two switches. Either one turns the app-level enforcement on.

| Switch | Where | Covers |
| --- | --- | --- |
| Admin console, Platform → Privacy mode | `instance_settings` row `privacy`, `{"mode":"strict"}` or `{"mode":"off"}` | Everything the app does (the tables below). It applies within 30 s (the cache in `app/src/instance_privacy.py`). |
| Install setting `privacyMode: true` | `INSTANCE_PRIVACY_MODE=1` in `/etc/forkmesh-instance/app.env`, `privacyMode` in `instance.json` | The app (forced on: the console cannot turn it off), plus Caddy, uvicorn's access log, the git node gateway and the system journal |

Only the install setting reaches the parts outside the app. Caddy, journald
and the git node read files the installer writes; they cannot read the
database. Use the install setting for full coverage. The console switch is
for trying the setting out, or for an instance whose operator cannot run the
installer.

### On an existing instance

1. Edit `/etc/forkmesh-instance/instance.json` as root and add
   `"privacyMode": true`. `--update` reads its settings from this file.
2. Run `bash /opt/forkmesh-instance/src/www/public/install-instance.sh --update`
   (or the copy you installed with).
3. The update does the following:
   - writes `INSTANCE_PRIVACY_MODE=1` to `app.env` and restarts the app;
   - rewrites the Caddyfile with the logger exclusions below and restarts Caddy;
   - adds `FORKMESH_PRIVACY_MODE=1` to `forkmesh-mirror-node.service` and restarts the git node;
   - writes `/etc/systemd/journald.conf.d/forkmesh-privacy.conf` and restarts `systemd-journald`.
4. Optional: older journal lines expire on their own once they pass the
   retention. To drop them now, run `journalctl --vacuum-time=3d`.

To turn it off, set `"privacyMode": false` and run `--update` again. The
installer removes the journald drop-in, but only if the file still carries
its `managed by install-instance.sh` line.

Without the installer, an admin can switch it in the console under Platform →
Privacy mode. That covers only the app rows of the tables below.

The admin console asks for an update through `forkmesh-instance-update.path`
(Platform → Update ForkMesh). That runs the same `--update`, so step 1 plus a
console update works as well.

## How it works

The app reads the visitor address from `cf-connecting-ip` in about thirty
places: rate limits, sessions, sign-up evidence, view counters, the 404 log,
probe bans, error rows and Turnstile. Guarding each reader would leave the
next new one unguarded. So the host replaces the address before the app sees
it.

`forkmesh_host.privacy.scrub_headers` is called from `HostASGI._request` for
HTTP and WebSocket requests. It does two things:

- **Replaces `cf-connecting-ip` with a pseudonym.** The pseudonym is an HMAC
  with a 32-byte key. The key is random, lives only in process memory, and is
  replaced every 24 hours (`instance_privacy.pseudonym`). It cannot be turned
  back into the address, and after a day nothing can link it to one.
- **Deletes headers.** `cf-ipcountry`, `x-forwarded-for`, `x-real-ip`,
  `forwarded` and `true-client-ip` are removed, so `request.cf.country` is
  empty.

The pseudonym is shaped to keep rate limits and checks working:

- **It is a valid IPv6 address in `fd00::/8`.** Parsers, rate limiters and
  per-client keys keep working. One visitor stays one bucket for the day.
- **It is never a public address.** Nothing can mistake it for one: probe
  bans and Turnstile skip it.
- **It keeps IPv6 /64 grouping.** For IPv6 visitors, the upper 64 bits hash
  the visitor's /64 and the lower 64 bits hash the full address. A limiter
  that groups by /64 still groups one network together.
- **Some addresses are not replaced.** Loopback keeps its address, so the
  host's own sub-requests keep it. So do addresses inside
  `SSH_GATEWAY_ALLOWED_ADDRESSES`, the operator's own SSH relay: the
  authorize pin compares against the real address.

`instance_privacy.is_pseudonym` lets a reader that has no `env` see that
privacy mode replaced the address.

Rate limits that the host keeps in memory still use the real address and are
never written anywhere: `RequestPolicy`, the reverse-DNS crawler check and
admin block rules. Rows that the app writes to the database get the
pseudonym, or a blind index of it. That includes `login_attempts`,
`signup_rate`, `password_reset_rate` and `agent_guestbook_rate`. Those rows
expire with their windows. The key that made them is gone the next day.

The setting is cached for 30 s. The host loads it before every request
(`privacy.load` in `HostASGI._http` / `_websocket`), and so does
`Default.fetch` on branded instances. The first request after a start
therefore already follows a setting saved in the console.

## Audit: what is collected normally and what privacy mode does

"Off" is today's behaviour, which stays unchanged when privacy mode is off.

### Web server, process logs, journal

| Where | Normally | Privacy mode |
| --- | --- | --- |
| Caddy access log | None: the installer's Caddyfile has no `log` directive | Unchanged. A `log` that an operator adds in `/etc/caddy/site.d` is the operator's own choice. |
| Caddy runtime log (journald) | `http.log.error` and `http.handlers.reverse_proxy` lines carry the request (remote IP, URI, headers); `http.stdlib` TLS handshake errors carry `IP:port` | Global `log default { exclude http.log.access http.log.error http.handlers.reverse_proxy http.stdlib }`. TLS and ACME lines stay, and the installer's certificate progress reads them. |
| uvicorn access log | Off (`--access-log` not passed) | Forced off, even when the flag is passed (`serve.py`) |
| uvicorn error logger | INFO lines such as `('127.0.0.1', port) - "WebSocket /path" [accepted]` | `QuietLogFilter` drops INFO lines while privacy mode is on, whichever switch turned it on. Warnings and errors stay. |
| `slow request <ms> <method> <path>`, `worker fetch raised on <path>` (asgi.py) | Raw path, which may name users or repositories | `privacy.masked_path`: `/api/<group>` (the masked groups of `api_metrics`), `/`, `/health`, `/readyz`, or `/:page` |
| rqlite caller label (`d1_caller`, cluster only) | `GET /path, IP x, user alice, session abcd` | `GET /:page`, with no IP, user or session |
| `web-requests` tally lines (`request_tally.py`) | Masked route group, status class, count and time per 10 s window. No IP or user agent. | Unchanged. These are the PII-free diagnostics. |
| D1 statement log (`FORKMESH_HOST_D1_LOG=1`) | Off by default. When on, it prints every statement. | Not enforced. Keep it off. |
| journald retention | Distro default (often unbounded up to the disk cap) | Drop-in `MaxRetentionSec=3day`, `MaxFileSec=1day`. This is journal-wide: it covers sshd, Caddy, the app and the git node. |
| sshd (admin SSH, optional SSH relay) | INFO logs client IPs and key fingerprints. fail2ban, when installed by `mirror_fail2ban.sh`, depends on those lines. | Not changed: these are system security settings. Only the 3-day journal retention applies. An operator who wants less can set `LogLevel ERROR` in `/etc/ssh/sshd_config.d/`, which loses fail2ban's input. |
| Setup log `/var/log/forkmesh-instance-setup.log` | Installer progress, no visitor data | Unchanged |

### App records

| What | Table / place | Normally | Privacy mode |
| --- | --- | --- | --- |
| Session sign-in address | `account_sessions.client_ip` | Raw IP, shown to the owner | Empty (`_account_session_client_ip` returns "" for a pseudonym) |
| Session device label, `last_seen_at` | `account_sessions` | Coarse label ("Firefox on Linux") and last use | Unchanged. The label is not the user agent, and `last_seen_at` drives session expiry and the owner's session list. |
| Sign-up evidence | `users.data` `signup` (encrypted), `users.ip_bi` | Raw IP, user agent and country; blind index of the IP | Only `clientCategory` and `at`; `ip_bi` empty |
| Page-view attribution | `attribution_sessions`, `attribution_events`, `attribution_ip_hourly`, `fm_attr` cookie | Landing path, referrer, UTM, salted IP hash | Not recorded, no cookie (`tracking_declined` is true); `/api/attribution/capture` and `/events` are discarded |
| Site referrers | `site_referrers` | Referrer host, last URL | Not recorded |
| Blog views | `blog_post_*` | Counters, HLL of IP and user agent, referrer hosts | Not recorded |
| Repository views | `repo_views`, `repo_view_totals` | Hashed viewer, hits, timestamps | Not recorded (`_repo_viewer_key` returns "") |
| Note views | `note_views` | Hashed IP and user agent per note | Not recorded (no address to count) |
| World visitors | `world_visit_unique_hll` | HLL of hashed IP and user agent | Not recorded (no address to count) |
| 404 log | `not_found_paths`, `not_found_hours` | Path, referrer, sealed `{ip, country, ua}` | Not recorded (`not_found_log.record`); `/api/not-found` relay discarded |
| Probe auto-ban | `probe_visitors`, `probe_bans`, `request_block_rules` | Hashed IP with probed paths, sealed client, raw IP rule | Off (`probe_ban.mode` is `off`). Rate limits and admin block rules still apply. Bans made before privacy mode was turned on stay until they expire. |
| Release downloads | `release_downloads` | Repository, file, time, raw user agent (512 chars) | Count and time only, no user agent |
| Browser error reports | `error_log` (`/client-error/*`) | Redacted message, blind index of IP, account name | Not stored: `/api/client-errors` answers 202 `stored:false`, and `analytics.js` does not send them |
| Desktop error reports | `error_log` (`/desktop-error/*`) | Message, owner hash, account | Discarded (`/api/desktop-errors`) |
| Server errors (5xx) | `error_log` | Path (private repositories masked), message, ray, account name | Kept for diagnosis, but `actor` is always empty. The table is bounded by `MAX_ERROR_LOG`. |
| Product analytics (marketplace Analytics package) | `/api/analytics/_/capture`, `/decide` | Opt-in, first-party events | The consent prompt is never shown, the SDK is never loaded, and the endpoints are discarded |
| Email opens, clicks, page views | `email_sends` counters, mail log notes | Open pixel, rewritten tracked links, view counter | No pixel and no rewritten links (`_mail_tracked_html`); `mail_activity.tracked` records nothing. Provider-side tracking (SendGrid, Mailgun, Mailtrap) is configured at the provider; turn it off there. |
| Marketplace package visitor info | `request_visitor` in `entry.py` | Raw IP and user agent passed to packages (CRM) | Empty |
| Sensitive audit log | `sensitive_audit_log` | Actor account, action, target token, outcome; never IP or user agent (`sanitize_audit_details`) | Unchanged: an admin audit trail is needed to operate |
| Rate-limit rows | `login_attempts`, `signup_rate`, `password_reset_rate`, `agent_guestbook_rate` | Blind index of the raw IP | Blind index of the daily pseudonym, so the rows cannot be linked after a day |
| Feedback and bug reports | `feedback`, `bug_reports` | Submitted text, page, coarse client category, hashed IP or account | Kept: content the user chose to send. The hash is of the pseudonym. |
| Own activity, presence, office visits | `account_activity_days`, `account_presence`, `org_office_visits` | A user's own daily activity counts, chat online state, Office attendance | Kept. These are features the user sees, not visitor logging. See "Not covered". |

### Outbound

| Call | Normally | Privacy mode |
| --- | --- | --- |
| Sentry (`capture_sentry_error`, cron check-ins) | Only when `SENTRY_DSN` is set (not on instances by default) | Never sent |
| Cloudflare Turnstile siteverify | Token and `remoteip` | Token only. The widget still loads from Cloudflare in the browser while Turnstile keys are configured; leave the keys unset to avoid it. |
| Directory announcement to app.forkmesh.com | Opt-in. `{domain, ts, nonce, signature}`; forkmesh.com then reads `/.well-known/forkmesh-instance.json` (name, logo, URL, public key) and polls `/readyz` | Unchanged. It is opt-in, carries only instance data and never visitor data. Turn the listing off under Platform → ForkMesh directory to stop it. |
| World neighbour scenes, federation, mail provider API, updates (`git fetch` of the source) | Needed for those features | Unchanged. They carry no visitor data beyond what a feature sends by design (for example the recipient of an email). |
| Client-side requests | Same-origin only on an instance (`api-client.js` stays same-origin unless `forkmesh.apiOrigin` is set) | Unchanged; plus `analytics.js` sends nothing |

### Git node and gateways

| Where | Normally | Privacy mode |
| --- | --- | --- |
| `forkmesh-mirror-node` (Go) logs | Operations only; it reads no client address or user agent | Unchanged |
| `mirror_gateway.py` request lines | `requestId`, operation, status, duration, bytes; no client address, path or user agent | Unchanged (already PII-free) |
| `service-counters.json` (clone and website totals per repository, last served time, coarse user-agent class) | Recorded | Not recorded (`FORKMESH_PRIVACY_MODE=1` in the daemon unit; `record_service_counter` returns early). Existing totals stay as they were. |
| `ssh_gateway.py` | Per-client slot lock files in `/run/forkmesh-ssh-gateway` (tmpfs), named by an unsalted hash of the IP or /64 | Unchanged. The files are transient and in memory, but the hash is easy to reverse. The SSH relay is installed separately (`tools/install_ssh_relay.sh`) and does not read the install setting. |

## The UI note

`GET /api/instance` returns `privacyMode: true|false`. `instance-brand.js`
then adds "Privacy mode: no visitor logs":

- to the "Powered by ForkMesh" pill when there is one;
- otherwise in a pill of its own.

`analytics.js` reads the same document, through the shared
`window.__forkmeshInstanceInfo` promise or its own fetch on non-forkmesh.com
hosts. When privacy mode is on, it never offers analytics and never sends
error reports.

## Not covered

- **Records made before privacy mode was turned on.** Nothing is erased
  automatically. That includes old `account_sessions.client_ip`, `users`
  sign-up evidence, the 404 log, attribution tables, `repo_views`,
  `probe_visitors` and `release_downloads.ua`. Deleting them is the operator's
  call, for example with `sqlite3 /var/lib/forkmesh-instance/app.db` and
  `UPDATE account_sessions SET client_ip=''` (back up the file first). There is no erase button. A bulk delete of user data needs
  its own reviewed design (docs/engineering/destructive-actions.md).
- **The console switch alone does not reach Caddy, journald or the git
  node.** Use the install setting for those.
- **Requests answered before the app.** `RequestPolicy` (in-memory rate
  limits and block rules) still sees the real address, by design and never
  persisted. The `/api/repo/` fast path (`repo_fastpath.py`) records nothing
  about clients.
- **sshd and fail2ban** keep their default logging (see above), within the
  3-day journal.
- **Feature data that users see.** Chat presence, Office attendance and the
  user's own activity counts stay, because turning them off changes features
  rather than logging.
- **Turnstile and mail providers** are third parties chosen by the operator.
  Privacy mode sends them less, but the operator decides whether to use them.
- **forkmesh.com's own www pages** (`site-header.js` calling app.forkmesh.com)
  are not served by instances.

## Code

- `app/src/instance_privacy.py`: the setting (parse, dump, cache, `active`,
  `forced`, `load`), the pseudonym, the discarded beacon paths.
- `app/host/forkmesh_host/privacy.py`: header scrubbing, masked log paths,
  uvicorn log filters.
- `app/src/instance_console.py`: the Privacy mode page (`view=privacy`), audited
  as `admin.console_save_privacy`.
- `www/public/install-instance.sh`: the `privacyMode` setting,
  `write_journald_privacy`, the Caddy global `log`, the daemon unit
  environment.
- Tests:
  - `app/tests/test_instance_privacy_mode.py`
  - `app/tests/host/test_host_privacy_mode.py`
  - `app/tests/test_install_instance_privacy_mode.py`
  - `app/tests/test_privacy_mode_clients.py`
