#pragma once

#include <QLatin1Char>
#include <QLatin1String>
#include <QString>
#include <QStringList>

struct MirrorPushOutcome {
    QStringList updated;   // refs this push advanced
    QStringList heldBack;  // refs Git protected from a rewind (see below)
    bool onlyHeldBack = false;
};

// Classify one mirror push from its porcelain stdout.
// The mirror push deliberately runs without --force and without --prune so an
// unattended desktop can never rewind or delete a branch that advanced on the
// gateway while this checkout was offline. Git enforces that per ref: it
// rejects only the divergent ref, advances every safe one, and exits non-zero.
// That exit code is the safety net working, not a broken mirror — the very
// case the push's own refspec comments describe (forkmesh/pulls moving ahead
// on the gateway before the source consumes it). Reported as a flat failure it
// buried the useful line under 300 characters of Git's "use 'git pull'" hint
// block and counted a healthy mirror as failed.
// Only the three rejections that resolve themselves are held back — a tip that
// is behind, diverged, or racing a concurrent update all clear once this node
// consumes the gateway's commits. A `[remote rejected]` (a hook or permission
// refusing the ref) and a clobbered tag do not converge on their own, so they
// stay hard failures.
// Lives in a header, rather than beside its one caller, so the fan-out's
// classification can be exercised directly by forkmesh-ssh-mirror-push-tests
// without linking the whole app. The caller pairs this with the exit code: a
// push that reports no ref status at all (an unreachable gateway, a refused
// key) leaves onlyHeldBack false and so stays a hard failure.
inline MirrorPushOutcome classifyMirrorPush(const QString &porcelainOutput)
{
    MirrorPushOutcome outcome;
    bool sawRejection = false;
    bool sawHardRejection = false;
    const QStringList lines = porcelainOutput.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.isEmpty() || line.startsWith(QLatin1String("To ")) ||
            line.startsWith(QLatin1String("Done")))
            continue;
        const QChar flag = line.at(0);
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 2 || flag == QLatin1Char('='))
            continue;
        QString ref = fields.at(1).section(QLatin1Char(':'), -1);
        if (ref.startsWith(QLatin1String("refs/heads/")))
            ref = ref.mid(11);
        else if (ref.startsWith(QLatin1String("refs/tags/")))
            ref = ref.mid(10);
        if (flag != QLatin1Char('!')) {
            outcome.updated.append(ref);
            continue;
        }
        sawRejection = true;
        const QString summary = fields.value(2);
        const bool protectedRewind =
            summary.contains(QLatin1String("[rejected]")) &&
            (summary.contains(QLatin1String("non-fast-forward")) ||
             summary.contains(QLatin1String("fetch first")) ||
             summary.contains(QLatin1String("stale info")));
        if (protectedRewind)
            outcome.heldBack.append(ref);
        else
            sawHardRejection = true;
    }
    outcome.onlyHeldBack = sawRejection && !sawHardRejection;
    return outcome;
}
