#!/usr/bin/env bash
# Applies the ForkMesh D1 migrations. Hooked into wrangler.toml as the [build]
# command, so it runs automatically before every `pywrangler deploy` (and dev).
# Uses D1's native migration tracking (`d1 migrations apply`), so each numbered
# file in migrations/ runs exactly once per database — that lets a migration
# do one-time, non-idempotent things (e.g. dropping a superseded table).
set -euo pipefail
cd "$(dirname "$0")"
. ./pywrangler.sh

DB="${FORKMESH_D1_NAME:-forkmesh}"

# A migration cannot reconstruct remote follower relationships after deleting
# them. Visibility changes belong in routing/policy code, never in destructive
# DDL/DML against the durable ActivityPub social graph.
if grep -RinE \
    'DELETE[[:space:]]+FROM[[:space:]]+ap_followers|DROP[[:space:]]+TABLE([[:space:]]+IF[[:space:]]+EXISTS)?[[:space:]]+ap_followers' \
    migrations; then
  echo "migrate.sh: refusing a migration that deletes the ActivityPub follower graph." >&2
  exit 1
fi

# Nothing to do until a real D1 database is wired up: skip (don't fail the
# build) while wrangler.toml still has the placeholder id.
if grep -q "REPLACE_WITH_D1_DATABASE_ID" wrangler.toml; then
  echo "migrate.sh: D1 not configured yet (placeholder database_id) — skipping."
  echo "  Run: uvx --from workers-py pywrangler d1 create $DB, then paste the id."
  exit 0
fi

# Target the deployed (remote) D1 by default; set FORKMESH_D1_LOCAL=1 to apply
# to the local dev database instead.
SCOPE="--remote"
[ "${FORKMESH_D1_LOCAL:-0}" = "1" ] && SCOPE="--local"

echo "migrate.sh: applying D1 migrations to '$DB' ($SCOPE)"
# Non-interactive (build subprocess): wrangler auto-confirms when stdout is not
# a TTY. Each migration is tracked in d1_migrations and applied at most once.
#
# Capture the output (while still streaming it live via tee) so a raw Cloudflare
# *auth* rejection — which otherwise aborts the whole deploy from inside the
# wrangler [build] hook with an opaque stack trace, with wrangler's version
# banner as the most prominent line — can be turned into actionable remediation.
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
set +e
pywrangler d1 migrations apply "$DB" "$SCOPE" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

if [ "$status" -ne 0 ]; then
  # Cloudflare rejects the token with code 9109 (Invalid access token) / 10000
  # (Authentication error). This is the most common reason a deploy dies here,
  # and the bare error never says what to do. The usual culprit is account drift:
  # the token/account in .env.production isn't the one that owns the database_id
  # pinned in wrangler.toml.
  if grep -qE 'code: (9109|10000)|Invalid access token|Authentication error' "$log"; then
    db_id="$(sed -n 's/^[[:space:]]*database_id[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' wrangler.toml | head -1)"
    {
      echo ""
      echo "migrate.sh: Cloudflare rejected the remote D1 migration as UNAUTHENTICATED."
      echo "  CLOUDFLARE_API_TOKEN (from app/.env.production) is missing, expired,"
      echo "  lacks the 'D1: Edit' permission, or belongs to a DIFFERENT Cloudflare account than"
      echo "  the one that owns D1 database '$DB'${db_id:+ (id $db_id)}. A wrong-account token is"
      echo "  the classic cause of this failure (the 'wrangler ... (update available)' banner above"
      echo "  is just a version notice, not the error)."
      echo "  Fix: set a CLOUDFLARE_API_TOKEN that (1) has Workers Scripts: Edit AND D1: Edit, and"
      echo "  (2) is for the SAME account as CLOUDFLARE_ACCOUNT_ID — the account that owns the"
      echo "  database_id in wrangler.toml — then redeploy."
      echo "  To migrate the LOCAL dev DB instead (no Cloudflare auth needed): FORKMESH_D1_LOCAL=1 ./migrate.sh"
    } >&2
  fi
  exit "$status"
fi
echo "migrate.sh: migrations applied."
