# Offline and low-bandwidth sync

`tools/forkmesh_offline_sync.py` moves a repository over removable media or an
unreliable, metered link. It **exports** refs (and with them the signed
`.forkmesh/` collaboration records) to a directory signed with an Ed25519 key,
**fetches** that directory in verified, resumable chunks, **imports** it in one
ref transaction, and **rolls back** any import.

Formats: manifest `forkmesh-offline-media-v1` inside the signed envelope
`forkmesh-offline-media-signed-v1`; checkpoint
`forkmesh-offline-transfer-checkpoint-v1`; import journal
`forkmesh-offline-import-journal-v1`; replica advertisement
`forkmesh-replica-scope-v1`.

## Start here: carry a repository on a USB stick

```sh
tools/forkmesh_offline_sync.py keygen ~/.forkmesh-offline.key   # prints the public key
tools/forkmesh_offline_sync.py export ~/src/project /media/usb/export-1 --signing-key ~/.forkmesh-offline.key
tools/forkmesh_offline_sync.py verify /media/usb/export-1 --trusted-key <public key>
tools/forkmesh_offline_sync.py import /media/usb/export-1 ~/src/project --trusted-key <public key>
```

Share the public key over a trusted channel. A key that starts with `-` goes
after an equals sign: `--trusted-key=-Ab…`. `export --since <earlier export>`
writes an incremental export listing its `prerequisites` and `previousExport`;
import refuses it until the base has been imported.

## Fetch over a link that keeps dropping

```sh
tools/forkmesh_offline_sync.py fetch https://files.example.org/export-1/ ./incoming \
  --rate 64k --budget 20M --schedule 22:00-06:00 --trusted-key <public key>
tools/forkmesh_offline_sync.py status ./incoming
```

The source is any static HTTP host that honours `Range`, or a local directory.
The signed manifest is fetched and verified once. Chunks resume from the first
missing byte and count only after their SHA-256 matches the manifest; a chunk
failing 3 times stops the fetch. The bundle is assembled only when every chunk
and the whole-file digest verify. Retries back off; `--retries N` failures
without progress pause with `link_down`; Ctrl-C pauses with `cancelled`;
`--budget` pauses with `budget_exhausted`; `--schedule` windows pause with
`outside_schedule` (`--wait` sleeps instead); `--restart` discards the
checkpoint. `status` reads only the local checkpoint and reports remaining
bytes and an ETA; `fetch --json` adds transfer counters.

## Limited exports

| Export option | Limitation code |
| --- | --- |
| `--metadata-only` | `metadata_only` |
| `--path PATH` | `path_filter` |
| `--defer-blobs-over SIZE` | `deferred_large_objects` |
| `--ref GLOB` | `branch_filter` |
| _(detected on import)_ | `missing_objects` |

The signed manifest advertises a replica scope of `full`, `filtered` or
`metadata-only`; `verify` recomputes it and fails an overclaiming export. Import
needs `--allow-limited` for a limited export and stores left-out contents as a
Git partial clone. `forkmesh_offline_sync.py replica REPO` prints what a
repository may advertise. The [availability proof](availability-proof.md)
treats any limited replica as `limited_replica` and never counts it toward a
quorum.

## Import, reconciliation, and rollback

Import verifies signature, trusted key, manifest, digests, scope, object format
and prerequisites before writing anything, then decides each ref:

| Decision | Result |
| --- | --- |
| `create` | Ref created. |
| `fast_forward` | Ref moved forward. |
| `unchanged` | Nothing. |
| `local_ahead` | Local kept. |
| `conflict` | Local kept; incoming saved under `refs/forkmesh-offline/conflicts/`. |
| `checked_out` | Local kept; incoming saved aside for `git merge --ff-only`. |

Refs are never deleted. All changes apply in one `git update-ref --stdin`
transaction after a journal is written to
`<git-dir>/forkmesh-offline/imports/<importId>.json`.
`forkmesh_offline_sync.py rollback REPO IMPORT_ID` reverses it only if every ref
still holds the imported value.

Signing is Ed25519 over `forkmesh-offline-media-v1\n` plus the canonical
manifest's SHA-256; verification is pure Python. Unpinned signers need
`--allow-unpinned`. `--telemetry FILE` appends one count-only
`forkmesh-offline-telemetry-v1` line per run, with no names, URLs, refs or keys.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Done. |
| `2` | Usage error or unreadable input. |
| `3` | Verification, integrity, or missing-object failure; nothing changed. |
| `4` | Paused with the checkpoint kept; re-run to resume. |
| `5` | Imported, with conflicts saved aside. |

Tests: `app/tests/test_offline_sync.py` (quality gate `offline-sync-conformance`).
