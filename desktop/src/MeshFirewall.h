#pragma once

#include <QString>
#include <QStringList>

// What stands between the local mesh room and the devices around it: a host
// firewall that drops unsolicited inbound traffic. Discovery beacons and mesh
// links both arrive unsolicited, so behind such a firewall a device announces
// itself, hears nothing back, and the room stays empty.
//
// This only works out which firewall is in the way and the one command that
// lets the mesh through it. Nothing here runs with privileges: the window
// shows the command to the user and runs it, on their click, through the
// platform's own elevation prompt.
namespace forkmesh::meshfirewall {

enum class Kind { None, Ufw, Firewalld, Windows };

struct Plan {
    Kind kind = Kind::None;
    // True when the firewall's own configuration says inbound traffic is
    // dropped. False for a firewall that cannot be read without privileges
    // (Windows): there the plan is only offered once peers are heard but
    // cannot be linked.
    bool blocksInbound = false;
    QString firewallName;      // "ufw", for the sentence shown to the user
    QString displayCommand;    // what a user would type in a terminal
    QString privilegedCommand; // what the Allow button runs when elevated
    // Remembers that this exact plan was applied, so the banner stays away.
    QString settingsStamp;

    bool isValid() const { return kind != Kind::None; }
};

// The private IPv4 ranges the mesh accepts peers from. The firewall rule is
// scoped to the same ranges, so opening the port never exposes it to a
// routable host.
QStringList localSourceRanges();

// ufw is on when /etc/ufw/ufw.conf says ENABLED=yes, and drops inbound unless
// /etc/default/ufw sets DEFAULT_INPUT_POLICY to ACCEPT.
bool ufwBlocksInbound(const QString &ufwConf, const QString &ufwDefaults);

// Shell commands, without sudo, that allow the mesh port (TCP and UDP).
QString ufwAllowCommand(int port);
QString firewalldAllowCommand(int port);
QString windowsAllowCommand(int port);

// The firewall on this machine and the command that opens `port`, or an
// invalid plan when nothing recognisable is in the way. Reads a few small
// files; never blocks and never spawns a process.
Plan detect(int port);

} // namespace forkmesh::meshfirewall
