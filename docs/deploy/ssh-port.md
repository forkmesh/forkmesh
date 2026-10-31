# Fleet SSH port

Every core, runner and autoscaled node runs sshd on `ssh_port` from
`deploy/fleet.toml` (822), not 22. Bots sweep port 22 all day, filling the
journal, fail2ban and the IDS feed with noise. Few of them try 822. The port is
below 1024, so only root can listen on it, even while sshd is down.

## What follows the port

| Piece | Where |
| --- | --- |
| sshd | `/etc/ssh/sshd_config.d/05-fmnext-port.conf` (`Port 822`), written by the `ssh-port` step; the `base` step's `10-fmnext.conf` stays port-free |
| Node firewall | `public_tcp` in `render.nftables_conf` (core and runner) |
| IDS | `ssh.port` and the `listeners.tcp` set in `ids.json`; the lockdown table (`ids.firewall_ruleset`) keeps SSH open on that port |
| fail2ban | `/etc/fail2ban/jail.d/fmnext-sshd.local` (`[sshd] port = 822`) |
| Operator tools | `next_common.Ssh`: `-o Port=822 -o HostKeyAlias=<ipv4>`; `next_git_host.py` push; `next_aws.py` security group (22 and 822, so a fresh instance is reachable before its `ssh-port` step) |
| Desktop log feed | `CoreNodeLogs.cpp` reads `ssh_port` from `~/projects/forkmesh/src/deploy/fleet.toml` (`FMNEXT_FLEET_TOML`, `FMNEXT_SSH_PORT` override) |
| Autoscale snapshot | `next_fleet.ssh_port_image_script` writes the drop-in into the image (no reload: the builder keeps its session) |

`HostKeyAlias` keeps `known_hosts` keyed by the bare address, so the pins
made on 22 stay valid on 822 (OpenSSH otherwise looks up `[ip]:822`), and
`ssh-keygen -R <ip>` still removes them.

## Moving a node

```
python3 tools/next_fleet.py install n2 --steps ssh-port --execute
```

The step connects on whatever port answers. `Ssh` tries `ssh_port` first and
falls back to 22 when only 22 answers. The step then:

1. tells the IDS to expect changes to sshd, `nftables.conf`, `ids.json`,
   fail2ban and ufw;
2. backs up those files to `/run/fmnext-ssh-port` and arms
   `fmnext-ssh-port-rollback.timer` (180 s) before changing anything;
3. installs the rendered `nftables.conf`, `ids.json` and fail2ban jail and the
   port drop-in, checks `sshd -t`, and checks that `sshd -T` lists exactly the
   new port (a `Port 22` line left in `sshd_config` would keep 22 open), rolling
   back at once otherwise;
4. opens the port: loads the new ruleset when ours is in charge, else
   `ufw allow 822/tcp` on a fresh Vultr image;
5. reloads sshd (open sessions survive) and restarts the IDS and fail2ban;
6. reconnects on the new port with strict host-key checking and checks the
   node's name, then disarms the timer.

If the new port does not answer, the step fails and the node puts the old
files back by itself and reloads sshd and the firewall.

Move one core at a time, then the runner. New nodes run `ssh-port` between
`base` and `files` in every step list. Before a `files` step, `install` also
moves any node that still answers only on 22, so an automatic roll after this
change cannot load the new firewall in front of an sshd that is still on 22.

## Autoscaled nodes

App replicas and burst runners boot from the autoscale snapshot. Rebuild it
(`python3 tools/next_autoscale.py snapshot build --execute`) so new nodes
start on 822; nodes made from an older snapshot keep sshd on 22 behind a
firewall that now admits only 822, until they are replaced.

## Changing the port again

Edit `ssh_port`, then run the `ssh-port` step on each node. `fleet-toml`
regeneration keeps the value.
