#include "MirrorGatewayHealth.h"
#include "MirrorPushOutcome.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSet>
#include <QTemporaryDir>

#include <cstdio>

// The mirror-fleet regression this suite pins down: the unattended SSH fan-out
// pushes refs/heads/* and refs/tags/* in one non-atomic, non-forcing, pruning
// push, so
// a branch the gateway advanced first (forkmesh/pulls, a live agent branch) is
// rejected while every other ref lands. git still exits non-zero for that
// partial rejection. Judging the push by its exit code alone reported the
// healthy fan-out as "SSH mirror push of forkmesh/forkmesh to
// forkmesh-mirror10-sync failed: … a pushed branch tip is behind its remote
// counterpart", every five-second safety pass, and told a release push that a
// mirror had failed — while main had in fact reached the whole fleet.
//
// Only the rejections that converge on their own are tolerated. A hook denial,
// a clobbered tag, or a gateway that never answered stay hard failures.

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

// The rule pushToSshMirrorRemotes applies to a finished push. Mirrored here so
// the tests exercise the decision the fan-out actually makes, not just the
// classifier in isolation: the exit code alone is meaningless without the
// per-ref status, and the status alone cannot see an unreachable gateway.
bool pushFailed(const MirrorPushOutcome &outcome, int exitCode)
{
    return exitCode != 0 && !outcome.onlyHeldBack;
}

// The gateways pushToSshMirrorRemotes would actually spawn a `git push` for on
// one pass. Mirrored here for the same reason as pushFailed: the rotation rule
// is a property of the fan-out, not of the health tracker alone — a release
// rollout deliberately ignores every cooldown.
QStringList rotationTargets(const MirrorGatewayHealth &health,
                            const QStringList &hosts, qint64 nowMs,
                            bool userInitiated = false)
{
    QStringList targets;
    for (const QString &host : hosts) {
        if (userInitiated || health.readyToPush(host, nowMs))
            targets.append(host);
    }
    return targets;
}

// The gateways a pass would spawn a push for once per-gateway serialization is
// applied, given the set already in flight. Mirrored from pushToSshMirrorRemotes
// for the same reason as rotationTargets: the rule is a property of the fan-out.
QStringList unblockedTargets(const QSet<QString> &inFlight,
                             const QString &repoKey, const QStringList &urls)
{
    QStringList ready;
    for (const QString &url : urls) {
        if (!inFlight.contains(repoKey + QLatin1Char('\x1f') + url))
            ready.append(url);
    }
    return ready;
}

// The periodic safety pass the retry policy measures itself against: the
// shipped default mirror sync interval.
constexpr qint64 kPeriodicPassMs = 60LL * 1000;

// The reported outage, verbatim: mirror13 was decommissioned, its remote stayed
// configured, and every five-second safety pass paid a full TCP connect timeout
// to it and logged the same red line.
const QString kRetiredGatewayStderr = QStringLiteral(
    "ssh: connect to host 149.28.230.243 port 22: Connection timed out\r\n"
    "fatal: Could not read from remote repository.\n"
    "Please make sure you have the correct access rights\n"
    "and the repository exists.");

bool git(const QString &path, const QStringList &arguments,
         QByteArray *output = nullptr, int *exitCode = nullptr)
{
    QProcess process;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_AUTHOR_NAME"),
                       QStringLiteral("Mirror Push Test"));
    environment.insert(QStringLiteral("GIT_AUTHOR_EMAIL"),
                       QStringLiteral("mirror-push@example.invalid"));
    environment.insert(QStringLiteral("GIT_COMMITTER_NAME"),
                       QStringLiteral("Mirror Push Test"));
    environment.insert(QStringLiteral("GIT_COMMITTER_EMAIL"),
                       QStringLiteral("mirror-push@example.invalid"));
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"),
                       QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-C"), path} + arguments);
    if (!process.waitForFinished(30000))
        return false;
    if (output)
        *output = process.readAllStandardOutput();
    if (!exitCode)
        return process.exitCode() == 0;
    // A caller that asks for the exit code is inspecting a push that is
    // *expected* to fail; running to completion is all it needs from here.
    *exitCode = process.exitCode();
    return true;
}

bool commitFile(const QString &repo, const QString &name,
                const QByteArray &bytes, const QString &message)
{
    QFile file(QDir(repo).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size())
        return false;
    file.close();
    return git(repo, {QStringLiteral("add"), name}) &&
           git(repo, {QStringLiteral("commit"), QStringLiteral("-q"),
                      QStringLiteral("-m"), message});
}

// The exact shape git emits when the gateway is ahead on one branch: main
// advances, the divergent branch is rejected, exit status 1.
void testPartialRejectionIsNotAFailure()
{
    const QString output = QStringLiteral(
        "To ssh://gateway/srv/mirrors/forkmesh-forkmesh.git\n"
        " \trefs/heads/main:refs/heads/main\t349f2db..b0f0298\n"
        "!\trefs/heads/pulls:refs/heads/pulls\t[rejected] (non-fast-forward)\n"
        "Done\n");
    const MirrorPushOutcome outcome = classifyMirrorPush(output);
    check(!pushFailed(outcome, 1),
          "a divergent branch alone is not a push failure");
    check(outcome.onlyHeldBack, "the only rejection was rewind protection");
    check(outcome.updated == QStringList{QStringLiteral("main")},
          "main still advanced on the gateway");
    check(outcome.heldBack == QStringList{QStringLiteral("pulls")},
          "the held-back ref is named by its short branch name");
}

// "fetch first" is the same condition seen from a checkout with no
// remote-tracking ref for the gateway — the mirror-node fan-out's usual case.
void testFetchFirstAndStaleInfoAreHeldBack()
{
    const QString output = QStringLiteral(
        "To ssh://gateway/repo.git\n"
        "*\trefs/heads/topic:refs/heads/topic\t[new branch]\n"
        "!\trefs/heads/pulls:refs/heads/pulls\t[rejected] (fetch first)\n"
        "!\trefs/heads/wip:refs/heads/wip\t[rejected] (stale info)\n"
        "Done\n");
    const MirrorPushOutcome outcome = classifyMirrorPush(output);
    check(!pushFailed(outcome, 1), "both rejections resolve themselves");
    check(outcome.updated == QStringList{QStringLiteral("topic")},
          "the new branch was created");
    check(outcome.heldBack.size() == 2, "two refs were left alone");
}

// A pre-receive hook refusing the push is a genuine failure: nothing will
// converge on its own, so it must stay loud.
void testHookDenialStaysAFailure()
{
    const QString output = QStringLiteral(
        "To ssh://gateway/repo.git\n"
        " \trefs/heads/main:refs/heads/main\t349f2db..b0f0298\n"
        "!\trefs/heads/topic:refs/heads/topic\t[remote rejected] "
        "(pre-receive hook declined)\n"
        "Done\n");
    const MirrorPushOutcome outcome = classifyMirrorPush(output);
    check(pushFailed(outcome, 1), "a hook denial is a real failure");
    check(!outcome.onlyHeldBack, "a hook denial is not rewind protection");
    check(outcome.heldBack.isEmpty(), "nothing was merely held back");
}

// A tag the gateway already carries at a different commit will never converge
// on its own — unlike a branch, nothing here fast-forwards later.
void testClobberedTagStaysAFailure()
{
    const QString output = QStringLiteral(
        "To ssh://gateway/repo.git\n"
        "!\trefs/tags/v1.0:refs/tags/v1.0\t[rejected] (already exists)\n"
        "Done\n");
    const MirrorPushOutcome outcome = classifyMirrorPush(output);
    check(pushFailed(outcome, 1), "a clobbered tag is a real failure");
    check(outcome.heldBack.isEmpty(), "a tag clash is not rewind protection");
}

// An unreachable gateway or a refused key never reaches ref negotiation: no
// status lines at all, so the failure must stay loud.
void testUnreachableGatewayIsAFailure()
{
    const MirrorPushOutcome outcome = classifyMirrorPush(QString());
    check(pushFailed(outcome, 128),
          "no ref status with a non-zero exit is a failure");
    check(!outcome.onlyHeldBack, "nothing was held back; nothing was reached");
    check(outcome.updated.isEmpty(), "nothing moved");
}

// A clean, fully up-to-date push must stay silent: this runs on the
// five-second safety pass and would otherwise spam the network log.
void testUpToDatePushIsSilent()
{
    const QString output = QStringLiteral(
        "To ssh://gateway/repo.git\n"
        "=\trefs/heads/main:refs/heads/main\t[up to date]\n"
        "=\trefs/tags/v1.0:refs/tags/v1.0\t[up to date]\n"
        "Done\n");
    const MirrorPushOutcome outcome = classifyMirrorPush(output);
    check(!pushFailed(outcome, 0), "an up-to-date push is not a failure");
    check(outcome.updated.isEmpty(), "nothing moved, so nothing is logged");
    check(outcome.heldBack.isEmpty(), "up to date is not rewind protection");
}

// "To <url>" and "Done" carry no ref status and must never be mistaken for
// one — an early line scan counted "To …" as an update.
void testFramingLinesAreIgnored()
{
    const QString output = QStringLiteral(
        "To ssh://gateway/repo.git\n"
        "Done\n");
    const MirrorPushOutcome outcome = classifyMirrorPush(output);
    check(outcome.updated.isEmpty(), "framing lines are not ref updates");
    check(!pushFailed(outcome, 0),
          "a clean exit with no refs is not a failure");
}

// A retired node still listed as a push remote must not define the fan-out's
// cadence: it rotates out, the rest of the fleet keeps syncing on the pass it
// always used.
void testRetiredGatewayRotatesOutOfTheFanOut()
{
    const QStringList fleet = {QStringLiteral("forkmesh-mirror10-sync"),
                               QStringLiteral("forkmesh-mirror13-sync"),
                               QStringLiteral("forkmesh-mirror14-sync")};
    check(mirrorGatewayUnreachable(kRetiredGatewayStderr),
          "a connect timeout is the gateway never answering");

    MirrorGatewayHealth health;
    const qint64 start = 1'700'000'000'000LL;
    check(rotationTargets(health, fleet, start) == fleet,
          "every gateway is tried before anything has failed");

    health.noteUnreachable(QStringLiteral("forkmesh-mirror13-sync"), start);
    const QStringList next = rotationTargets(health, fleet, start + 5'000);
    check(next == QStringList{QStringLiteral("forkmesh-mirror10-sync"),
                              QStringLiteral("forkmesh-mirror14-sync")},
          "the retired gateway sits out the next pass");
    check(rotationTargets(health, fleet, start + 5'000,
                          /*userInitiated=*/true) == fleet,
          "a release rollout still tries every configured gateway");
}

// The cooldown grows so a dead node is probed less and less, but stays bounded
// so a gateway that comes back is picked up within the quarter hour.
void testUnreachableCooldownGrowsAndCaps()
{
    MirrorGatewayHealth health;
    const QString host = QStringLiteral("forkmesh-mirror13-sync");
    qint64 now = 1'700'000'000'000LL;
    const MirrorGatewayHealth::Notice first =
        health.noteUnreachable(host, now);
    check(first.cooldownMs >= MirrorGatewayHealth::kBaseCooldownMs,
          "the first failure holds the gateway off for at least a pass");

    qint64 previous = first.cooldownMs;
    bool grew = false;
    for (int i = 0; i < 3; ++i) {
        now += previous + 1;
        const MirrorGatewayHealth::Notice notice =
            health.noteUnreachable(host, now);
        grew = grew || notice.cooldownMs > previous;
        previous = notice.cooldownMs;
    }
    check(grew, "repeated silence widens the retry interval");

    for (int i = 0; i < 10; ++i) {
        now += previous + 1;
        previous = health.noteUnreachable(host, now).cooldownMs;
    }
    // NetworkBackoff adds up to an eighth of the delay as jitter so separately
    // failing gateways do not all retry on the same tick.
    check(previous <= MirrorGatewayHealth::kMaxCooldownMs * 9 / 8,
          "the cooldown never grows past the ceiling");
}

// Rotating out is not giving up: the gateway is tried again when its cooldown
// expires, and one success puts it straight back on the normal pass.
void testGatewayReturnsToRotationAfterItsCooldown()
{
    MirrorGatewayHealth health;
    const QString host = QStringLiteral("forkmesh-mirror13-sync");
    const qint64 start = 1'700'000'000'000LL;
    const qint64 cooldown = health.noteUnreachable(host, start).cooldownMs;
    check(!health.readyToPush(host, start + cooldown / 2),
          "the gateway is skipped while it is cooling down");
    check(health.cooldownRemainingMs(host, start + cooldown / 2) > 0,
          "the caller can say how long it is held off");
    check(health.readyToPush(host, start + cooldown + 1),
          "it is retried once the cooldown expires");

    check(health.noteReachable(host),
          "the recovery is worth reporting after an outage was reported");
    check(health.readyToPush(host, start + cooldown + 2),
          "one success restores the normal cadence");
    check(!health.noteReachable(host),
          "a gateway that never failed reports no recovery");
}

// A gateway that answers and then refuses is a different failure: someone has
// to fix it, so it must never be quietly rotated out.
void testAnsweredRefusalsAreNotRotatedOut()
{
    check(!mirrorGatewayUnreachable(QStringLiteral(
              "git@gateway: Permission denied (publickey).\n"
              "fatal: Could not read from remote repository.")),
          "a refused key answered; it is not an unreachable gateway");
    check(!mirrorGatewayUnreachable(QStringLiteral(
              "remote: pre-receive hook declined\n"
              "! [remote rejected] main -> main (pre-receive hook declined)")),
          "a hook denial answered; it is not an unreachable gateway");
    check(!mirrorGatewayUnreachable(QString()),
          "no stderr at all is not evidence of an unreachable gateway");
    check(mirrorGatewayUnreachable(QStringLiteral(
              "ssh: Could not resolve hostname forkmesh-mirror13-sync: "
              "Name or service not known")),
          "a hostname that no longer resolves is a removed node");
}

// The log line is the point of the whole exercise: say it when it happens, then
// once an hour, and after a full day say what to do about it — once.
void testUnreachableIsAnnouncedOnceAnHourThenNamedRetired()
{
    MirrorGatewayHealth health;
    const QString host = QStringLiteral("forkmesh-mirror13-sync");
    const qint64 start = 1'700'000'000'000LL;
    check(health.noteUnreachable(host, start).announce,
          "the first failure is reported");
    check(!health.noteUnreachable(host, start + 60'000).announce,
          "the next pass does not repeat it");
    const MirrorGatewayHealth::Notice hourly = health.noteUnreachable(
        host, start + MirrorGatewayHealth::kRepeatNoticeMs + 1);
    check(hourly.announce, "a still-dead gateway is repeated once an hour");
    check(!hourly.retired, "an hour of silence is not yet a retired node");

    const MirrorGatewayHealth::Notice day = health.noteUnreachable(
        host, start + MirrorGatewayHealth::kRetiredAfterMs + 1);
    check(day.retired && day.downForMs >= MirrorGatewayHealth::kRetiredAfterMs,
          "a day of silence suggests dropping the remote");
    check(!health
               .noteUnreachable(
                   host, start + MirrorGatewayHealth::kRetiredAfterMs +
                             MirrorGatewayHealth::kRepeatNoticeMs + 2)
               .retired,
          "the removal hint is given once per outage, not every hour");

    health.noteReachable(host);
    const MirrorGatewayHealth::Notice fresh = health.noteUnreachable(
        host, start + MirrorGatewayHealth::kRetiredAfterMs + 3);
    check(fresh.announce && !fresh.retired,
          "a new outage starts its own streak");
}

// The second half of the same outage: mirror10, mirror13 and mirror15 were
// destroyed, their remotes stayed configured, and the desktop kept dialling
// them. It never reached the day-long retirement threshold because the streak
// lived only in memory — every relaunch reset it, re-announced each dead
// gateway as a brand-new failure, and restarted the full retry cadence.
void testOutageStreakSurvivesARestart()
{
    const QString host = QStringLiteral("forkmesh-mirror13-sync");
    const qint64 start = 1'700'000'000'000LL;

    MirrorGatewayHealth before;
    const MirrorGatewayHealth::Notice first = before.noteUnreachable(host, start);
    check(first.announce && first.persist,
          "the first failure of an outage is reported and written down");
    const QStringList saved = before.persistedState();
    check(saved.size() == 1 && saved.first().startsWith(host),
          "the outage is saved under the gateway host");

    // Relaunch a day later: the same outage, not a fresh one.
    MirrorGatewayHealth after;
    const qint64 nextDay = start + MirrorGatewayHealth::kRetiredAfterMs + 1;
    after.restoreState(saved, nextDay);
    check(after.downForMs(host, nextDay) >= MirrorGatewayHealth::kRetiredAfterMs,
          "the restarted desktop knows the gateway has been silent all day");
    check(after.retired(host, nextDay),
          "a day of silence carried across the restart retires the gateway");
    const MirrorGatewayHealth::Notice resumed =
        after.noteUnreachable(host, nextDay);
    check(!resumed.announce,
          "the relaunch does not re-announce a gateway that was already down");
    check(resumed.retired, "it names the retirement instead, once");
    check(resumed.parked && resumed.cooldownMs > MirrorGatewayHealth::kMaxCooldownMs,
          "and parks it on the slow probe from that single failure");

    // A saved state written by a machine with a further-ahead clock, or one
    // restored after the clock moved back, must not strand the gateway.
    MirrorGatewayHealth skewed;
    skewed.restoreState(saved, start - 60'000);
    check(skewed.downForMs(host, start) == 0,
          "an outage that starts in the future is discarded, not trusted");
}

// Parked is not abandoned: the probe keeps running, just four times a day
// instead of ninety-six, and one answer restores the normal cadence.
void testRetiredGatewayIsProbedSlowlyAndSilently()
{
    MirrorGatewayHealth health;
    const QString host = QStringLiteral("forkmesh-mirror10-sync");
    const qint64 start = 1'700'000'000'000LL;
    health.noteUnreachable(host, start);

    qint64 now = start + MirrorGatewayHealth::kRetiredAfterMs + 1;
    const MirrorGatewayHealth::Notice parked = health.noteUnreachable(host, now);
    check(parked.parked && parked.retired,
          "the day-long streak parks the gateway and says so once");
    check(parked.cooldownMs >= MirrorGatewayHealth::kRetiredCooldownMs,
          "a parked gateway waits hours, not a quarter of an hour");
    check(!health.readyToPush(host, now + MirrorGatewayHealth::kMaxCooldownMs),
          "it is not retried on the old fifteen-minute cadence");
    check(health.readyToPush(
              host, now + MirrorGatewayHealth::kRetiredCooldownMs * 2),
          "but it is still retried, so a node that comes back is picked up");

    now += MirrorGatewayHealth::kRetiredCooldownMs * 2;
    for (int i = 0; i < 8; ++i) {
        const MirrorGatewayHealth::Notice notice =
            health.noteUnreachable(host, now);
        check(!notice.announce && !notice.retired,
              "a parked gateway never writes another log line");
        now += notice.cooldownMs + 1;
    }
    check(health.noteReachable(host), "the recovery is still reported");
    check(health.persistedState().isEmpty(),
          "and the saved outage is cleared when it answers again");
}

// The complaint this half of the suite pins down: "I pushed and the mirrors did
// not update." Rotating a cooling gateway out of a pass drops the refs it would
// have carried, and nothing re-pushed them — the next attempt was whenever the
// periodic safety pass next came round, so one failed connect cost that mirror
// its cooldown *plus* up to a full sync interval. The fan-out now wakes itself
// the moment the earliest skipped gateway is eligible again.
void testCoolingGatewayIsRetriedWhenItsCooldownExpires()
{
    MirrorGatewayHealth health;
    const QString host = QStringLiteral("forkmesh-mirror13-sync");
    const qint64 start = 1'700'000'000'000LL;
    const qint64 cooldown = health.noteUnreachable(host, start).cooldownMs;

    const qint64 delay = mirrorPushRetryDelayMs(health, {host}, start,
                                                kPeriodicPassMs);
    check(delay > 0, "a skipped gateway earns a wake-up of its own");
    check(delay > cooldown,
          "the retry lands after the cooldown, never on the same tick");
    check(delay < cooldown + 5'000,
          "and immediately after it, not on the next periodic pass");
    check(health.readyToPush(host, start + delay),
          "the gateway really is eligible by the time the retry fires");

    // Halfway through the cooldown the wait is correspondingly shorter: a later
    // pass re-reporting the same host must not restart the whole cooldown.
    const qint64 midway = start + cooldown / 2;
    check(mirrorPushRetryDelayMs(health, {host}, midway, kPeriodicPassMs) <
              delay,
          "a later pass asks for the time still remaining, not a fresh wait");
}

// Nothing to wake up for: a healthy pass, and a gateway held off longer than the
// periodic pass (that pass reaches it first, so an extra timer is pure noise).
void testNothingWorthWakingForSchedulesNoRetry()
{
    MirrorGatewayHealth health;
    const qint64 start = 1'700'000'000'000LL;
    check(mirrorPushRetryDelayMs(health, {}, start, kPeriodicPassMs) == 0,
          "a pass that skipped nothing arms no retry");

    const QString parked = QStringLiteral("forkmesh-mirror13-sync");
    health.noteUnreachable(parked, start);
    const qint64 dayLater = start + MirrorGatewayHealth::kRetiredAfterMs + 1;
    health.noteUnreachable(parked, dayLater);
    check(mirrorPushRetryDelayMs(health, {parked}, dayLater, kPeriodicPassMs) ==
              0,
          "a parked gateway is left to the periodic pass, not woken for");

    // The earliest eligible gateway sets the wake-up when several are cooling.
    MirrorGatewayHealth fleet;
    const QString soon = QStringLiteral("forkmesh-mirror9-sync");
    const QString late = QStringLiteral("forkmesh-mirror14-sync");
    fleet.noteUnreachable(late, start);
    for (int i = 0; i < 6; ++i)
        fleet.noteUnreachable(late, start + i);
    fleet.noteUnreachable(soon, start);
    const qint64 wake =
        mirrorPushRetryDelayMs(fleet, {late, soon}, start, kPeriodicPassMs);
    check(wake > 0 && wake <= fleet.cooldownRemainingMs(soon, start) + 250,
          "the fleet is woken for whichever gateway returns first");
}

// The other half of the same complaint: the fan-out used to mark the whole
// repository "pushing" for the duration of the slowest gateway, so a node that
// was timing out held every healthy mirror at the previous commit until it gave
// up. Slots are per gateway now, and each frees as its own push finishes.
void testOneStuckGatewayDoesNotHoldTheFleet()
{
    const QString repoKey = QStringLiteral("forkmesh/forkmesh");
    const QStringList fleet = {
        QStringLiteral("ssh://git@forkmesh-mirror9-sync/repo.git"),
        QStringLiteral("ssh://git@forkmesh-mirror13-sync/repo.git"),
        QStringLiteral("ssh://git@forkmesh-mirror14-sync/repo.git")};

    QSet<QString> inFlight;
    check(unblockedTargets(inFlight, repoKey, fleet) == fleet,
          "an idle repository pushes to every gateway");
    for (const QString &url : fleet)
        inFlight.insert(repoKey + QLatin1Char('\x1f') + url);

    // mirror9 and mirror14 answer; mirror13 is still dialling into a black hole.
    inFlight.remove(repoKey + QLatin1Char('\x1f') + fleet.at(0));
    inFlight.remove(repoKey + QLatin1Char('\x1f') + fleet.at(2));
    const QStringList next = unblockedTargets(inFlight, repoKey, fleet);
    check(next == QStringList{fleet.at(0), fleet.at(2)},
          "the commit that just landed reaches the mirrors that are free");
    check(!next.contains(fleet.at(1)),
          "and is not pushed twice to the gateway still working on the last one");

    // A second repository is never blocked by the first one's stuck gateway.
    check(unblockedTargets(inFlight, QStringLiteral("forkmesh/world"), fleet)
              .size() == fleet.size(),
          "in-flight slots are scoped to one repository");
}

void testManagedRefPushesCoalescePerHost()
{
    MirrorRefPushGate gate;
    const QString repo = QStringLiteral("forkmesh/forkmesh");
    const QStringList hosts = {QStringLiteral("45.77.111.241"),
                               QStringLiteral("209.222.10.90"),
                               QStringLiteral("149.28.235.153")};

    const MirrorRefPushGate::Reservation first = gate.reserve(repo, hosts);
    check(first.readyHosts.size() == 3 && !first.replayQueued,
          "the first managed ref fan-out reserves the whole fleet");

    const MirrorRefPushGate::Reservation overlapping = gate.reserve(
        QStringLiteral("ForkMesh/ForkMesh"),
        {QStringLiteral(" 45.77.111.241 "), QStringLiteral("209.222.10.90"),
         QStringLiteral("149.28.235.153")});
    check(overlapping.readyHosts.isEmpty() && overlapping.replayQueued,
          "an overlapping fan-out starts no duplicate ref writers");
    check(!gate.finish(repo, hosts.at(0)) &&
              !gate.finish(repo, hosts.at(1)),
          "a queued replay waits for every host in the batch");
    check(gate.finish(repo, hosts.at(2)),
          "the last completed host releases one coalesced replay");

    const MirrorRefPushGate::Reservation replay = gate.reserve(repo, hosts);
    check(replay.readyHosts.size() == 3 && !replay.replayQueued,
          "the replay starts from a fully released fleet");
    check(!gate.finish(repo, hosts.at(0)) &&
              !gate.finish(repo, hosts.at(1)) &&
              !gate.finish(repo, hosts.at(2)),
          "a clean replay does not schedule itself forever");
}

void testManagedRefPushGateDoesNotCrossRepositories()
{
    MirrorRefPushGate gate;
    const QString host = QStringLiteral("mirror31.forkmesh.com");
    const auto first = gate.reserve(QStringLiteral("forkmesh/forkmesh"),
                                    {host});
    const auto second = gate.reserve(QStringLiteral("another/project"),
                                     {host});
    check(first.readyHosts.contains(host) && second.readyHosts.contains(host),
          "the same host may update different repositories independently");
    check(gate.busy(QStringLiteral("forkmesh/forkmesh"), {host}),
          "release sequencing can see an in-flight destination");
    check(!gate.busy(QStringLiteral("forkmesh/forkmesh"),
                     {QStringLiteral("mirror30.forkmesh.com")}),
          "an unrelated managed host remains available");
    gate.finish(QStringLiteral("forkmesh/forkmesh"), host);
    gate.finish(QStringLiteral("another/project"), host);
}

// Unwiring a deleted node must hit its remotes and nothing else. This is the
// match rule removeSshMirrorRemotesForNode applies to every ssh:// remote.
void testNodeRemoteMatchingIsBounded()
{
    // The fleet as it is actually saved: the Hosts page calls the node
    // "mirror10", ssh_config calls it "forkmesh-mirror10-sync", and the remote
    // URL carries the alias. All three have to name the same node.
    const QString node = QStringLiteral("mirror10");
    check(mirrorGatewayHostBelongsToNode(node, node),
          "the bare node name is its own gateway");
    check(mirrorGatewayHostBelongsToNode(
              QStringLiteral("forkmesh-mirror10-sync"), node),
          "the ssh_config alias the provisioner writes belongs to the node");
    check(mirrorGatewayHostBelongsToNode(
              QStringLiteral("mirror10.forkmesh.com"), node),
          "a DNS name under the node belongs to it");
    check(mirrorGatewayHostBelongsToNode(
              QStringLiteral("FORKMESH-MIRROR10-SYNC"), node),
          "host names are matched case-insensitively");
    check(mirrorGatewayHostBelongsToNode(
              QStringLiteral("forkmesh-mirror10-sync"),
              QStringLiteral("forkmesh-mirror10")),
          "a node saved under its fully qualified alias still matches");

    // The one mistake this must never make.
    check(!mirrorGatewayHostBelongsToNode(
              QStringLiteral("forkmesh-mirror10-sync"),
              QStringLiteral("mirror1")),
          "retiring mirror1 must never unwire mirror10");
    check(!mirrorGatewayHostBelongsToNode(
              QStringLiteral("forkmesh-mirror9-sync"), node),
          "a live gateway is left wired when a different node is deleted");
    check(!mirrorGatewayHostBelongsToNode(
              QStringLiteral("forkmesh-mirror10-sync"),
              QStringLiteral("mirror10-sync-gateway")),
          "a node name longer than the host it is checked against matches "
          "nothing");
    check(!mirrorGatewayHostBelongsToNode(QString(), node) &&
              !mirrorGatewayHostBelongsToNode(QStringLiteral("gateway"),
                                              QString()),
          "an empty host or node matches nothing");
}

// End to end against real git: reproduce the reported outage — the gateway
// advanced a branch this checkout has not consumed — and prove the fan-out's
// own push arguments still deliver main while the outcome reads as healthy.
void testAgainstRealGit()
{
    QTemporaryDir root;
    if (!root.isValid()) {
        check(false, "temporary directory");
        return;
    }
    const QDir dir(root.path());
    const QString gateway = dir.filePath(QStringLiteral("gateway.git"));
    const QString source = dir.filePath(QStringLiteral("source"));
    const QString peer = dir.filePath(QStringLiteral("peer"));

    if (!git(root.path(), {QStringLiteral("init"), QStringLiteral("-q"),
                           QStringLiteral("--bare"), gateway}) ||
        !git(root.path(), {QStringLiteral("init"), QStringLiteral("-q"),
                           QStringLiteral("-b"), QStringLiteral("main"),
                           source})) {
        check(false, "git init");
        return;
    }
    if (!commitFile(source, QStringLiteral("a.txt"), "a", QStringLiteral("one")) ||
        !git(source, {QStringLiteral("branch"), QStringLiteral("pulls")}) ||
        !git(source, {QStringLiteral("push"), QStringLiteral("-q"), gateway,
                      QStringLiteral("refs/heads/*:refs/heads/*")}) ||
        // The bare gateway is born pointing at whatever init.defaultBranch
        // says; aim HEAD at a branch that exists so the peer clone checks out.
        !git(gateway, {QStringLiteral("symbolic-ref"), QStringLiteral("HEAD"),
                       QStringLiteral("refs/heads/main")})) {
        check(false, "seed the gateway");
        return;
    }
    // A branch that was already merged and deleted on the source must not
    // remain as an extra public ref forever. That exact drift made every live
    // managed mirror amber even though their main tips were current.
    if (!git(gateway,
             {QStringLiteral("update-ref"),
              QStringLiteral("refs/heads/deleted-after-merge"),
              QStringLiteral("refs/heads/main")})) {
        check(false, "seed a stale remote-only branch");
        return;
    }
    // Another node advances the collaboration branch on the gateway.
    if (!git(root.path(), {QStringLiteral("clone"), QStringLiteral("-q"),
                           gateway, peer}) ||
        !git(peer, {QStringLiteral("checkout"), QStringLiteral("-q"),
                    QStringLiteral("pulls")}) ||
        !commitFile(peer, QStringLiteral("b.txt"), "b", QStringLiteral("two")) ||
        !git(peer, {QStringLiteral("push"), QStringLiteral("-q"),
                    QStringLiteral("origin"), QStringLiteral("pulls")})) {
        check(false, "advance pulls on the gateway");
        return;
    }
    // Meanwhile this checkout commits to main and runs the fan-out push.
    if (!commitFile(source, QStringLiteral("c.txt"), "c",
                    QStringLiteral("three"))) {
        check(false, "commit on the source");
        return;
    }
    QByteArray out;
    int exitCode = 0;
    const bool ran =
        git(source,
            {QStringLiteral("push"), QStringLiteral("--porcelain"),
             QStringLiteral("--no-force"), QStringLiteral("--prune"),
             gateway, QStringLiteral("refs/heads/*:refs/heads/*"),
             QStringLiteral("refs/tags/*:refs/tags/*")},
            &out, &exitCode);
    check(ran, "the fan-out push ran");
    check(exitCode != 0, "git reports the partial rejection as a failure");

    const MirrorPushOutcome outcome =
        classifyMirrorPush(QString::fromUtf8(out));
    check(!pushFailed(outcome, exitCode),
          "a gateway that is merely ahead on pulls is not a failed mirror");
    check(outcome.updated.contains(QStringLiteral("main")),
          "main advanced despite the rejected branch");
    check(outcome.heldBack == QStringList{QStringLiteral("pulls")},
          "only pulls was left alone");
    check(!git(gateway,
               {QStringLiteral("show-ref"), QStringLiteral("--verify"),
                QStringLiteral("--quiet"),
                QStringLiteral("refs/heads/deleted-after-merge")}),
          "the fan-out prunes a public branch deleted on the source");

    // The point of tolerating the rejection: main really did reach the mirror.
    QByteArray sourceMain;
    QByteArray gatewayMain;
    git(source, {QStringLiteral("rev-parse"), QStringLiteral("main")},
        &sourceMain);
    git(gateway, {QStringLiteral("rev-parse"), QStringLiteral("main")},
        &gatewayMain);
    check(!sourceMain.trimmed().isEmpty() &&
              sourceMain.trimmed() == gatewayMain.trimmed(),
          "the gateway carries the new main commit");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    testPartialRejectionIsNotAFailure();
    testFetchFirstAndStaleInfoAreHeldBack();
    testHookDenialStaysAFailure();
    testClobberedTagStaysAFailure();
    testUnreachableGatewayIsAFailure();
    testUpToDatePushIsSilent();
    testFramingLinesAreIgnored();
    testRetiredGatewayRotatesOutOfTheFanOut();
    testUnreachableCooldownGrowsAndCaps();
    testGatewayReturnsToRotationAfterItsCooldown();
    testAnsweredRefusalsAreNotRotatedOut();
    testUnreachableIsAnnouncedOnceAnHourThenNamedRetired();
    testOutageStreakSurvivesARestart();
    testRetiredGatewayIsProbedSlowlyAndSilently();
    testCoolingGatewayIsRetriedWhenItsCooldownExpires();
    testNothingWorthWakingForSchedulesNoRetry();
    testOneStuckGatewayDoesNotHoldTheFleet();
    testManagedRefPushesCoalescePerHost();
    testManagedRefPushGateDoesNotCrossRepositories();
    testNodeRemoteMatchingIsBounded();
    testAgainstRealGit();
    if (failures == 0)
        std::fprintf(stderr, "ssh mirror push tests passed\n");
    return failures == 0 ? 0 : 1;
}
