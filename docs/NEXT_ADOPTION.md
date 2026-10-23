# Adopting the self-hosted stack

The self-hosted stack described in [PLAN.md](PLAN.md) was built in its own
repository and is now production. This page covers how its code lives in this
repository, where its operator files are, and how the fleet grows.

## Code

The stack's code is part of this repository: `app/host/`, `deploy/`,
`tools/next_*.py` and `tools/cutover_*.py`. `tools/next_import.py` can replay
the old separate repository's history onto a branch here; it is
deterministic, refuses to rewind a diverged branch without `--force`, and is
tested by `deploy/tests/test_next_import.py`.

## Operator files

Operator state lives under the never-erase project folders:

| Folder | Holds |
|---|---|
| `~/projects/forkmesh/config/fleet/` | `secrets.env`, `DATA_KEY`, `ids-operator.key`, `fleet-state.json`, `fleet-instances.json`, `known_hosts`, provider credential files, deploy and autodeploy locks and manifests, `dns-health.json`, `add-core/` run state |
| `~/projects/forkmesh/ssh/fleet/` | the fleet SSH key and its `known_hosts` |
| `~/projects/forkmesh/data/fleet/` | `backups/`, `dist/` (release tarballs and the mirror-node binary), `dl/`, `bin/` (rqlite and Caddy for tests) |
| `~/projects/forkmesh/logs/fleet/` | tool and run logs |

`tools/forkmesh_paths.py` is the one source for these paths; `next_common`
derives `config_dir()`, `ssh_key_path()`, `data_dir()` and `logs_dir()` from it.
The overrides `FMNEXT_CONFIG_DIR`, `FMNEXT_SSH_KEY`,
`FMNEXT_DATA_DIR`, `FMNEXT_LOGS_DIR`, `FMNEXT_BACKUPS_DIR` and
`FMNEXT_DIST_DIR` let an operator point the tools elsewhere without moving
anything.

Rules:

- Copy, never move, operator state. Delete a copy only by explicit file name.
- The hourly minimal backup must not archive `config/fleet` or `ssh/fleet` in
  plaintext; those folders are backed up encrypted.
- The private backup decryption key belongs offline, not next to the backups.
- The operator units (`fmnext-backup`, `fmnext-unseal`, `fmnext-dns-health`)
  run the repository venv from `~/projects/forkmesh/src`; that venv needs
  `cryptography`.
- `~/projects/forkmesh-backup` is the retired folder of the old repository,
  kept as a backup. Never erase it.
- Remote node paths (`/etc/forkmesh-next`, `/opt/forkmesh-next`,
  `/var/lib/forkmesh-next`) and the `forkmesh-next` instance tag are identity
  names, not operator paths, and do not change.

## Adding a core

```sh
python3 tools/next_fleet.py add-core                 # plan, read-only
python3 tools/next_fleet.py add-core --execute       # create and join the next core
python3 tools/next_fleet.py add-core-status
```

The desktop offers the same through Network, Hosts, Add core node. The order
is: hold out of DNS, create the VM, fleet file and keys, re-render peers one
at a time, rqlite join (non-voter while three voters exist), daemon and
repository sync, activate and unseal, IDS baseline, health, DNS, commit the
new `[[nodes]]` block on a `fleet/add-core-nN` branch and merge it into
`main` (`remove-core` does the same on `fleet/remove-core-nN`), so autodeploy
deploys to the new set of cores. Run state is in
`config/fleet/add-core/nN.json`, so a re-run resumes at the failed step.
Details are in [deploy/README.md](deploy/README.md).

To abandon a new core: keep it out of DNS, `rqlite remove` it from a peer,
`next_fleet.py destroy nN --yes --execute`, delete its `[[nodes]]` block, and
re-render the peers with `next_fleet.py roll --steps files,start --execute`.
