#include "RoomSeal.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <random>

using namespace forkmesh::chatv3;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QJsonObject vectors;
QString room;
QString scope;
QStringList participants;
qint64 baseNow = 0;
qint64 nowMs = 0;

QJsonObject object(const QJsonObject &parent, const char *key)
{
    return parent.value(QLatin1String(key)).toObject();
}

QString kidOf(const QString &name)
{
    return object(vectors, "devices").value(name).toObject().value(QStringLiteral("kid")).toString();
}

QJsonObject frameNamed(const QString &name)
{
    return object(vectors, "frames").value(name).toObject();
}

QByteArray compact(const QJsonObject &value)
{
    return QJsonDocument(value).toJson(QJsonDocument::Compact);
}

struct Peer {
    std::shared_ptr<ChatDevice> device;
    RoomSeal seal;
};

Peer device(const QString &name, const QJsonValue &state = QJsonValue::Undefined)
{
    const QJsonObject secrets = object(vectors, "devices").value(name).toObject();
    Peer peer;
    peer.device = ChatDevice::inMemory(fromB64(secrets.value(QStringLiteral("identitySeed")).toString()));
    check(peer.device->kid() == secrets.value(QStringLiteral("kid")).toString(),
          "the vector identity seed gives the vector kid");
    peer.device->setClock([] { return nowMs; });
    for (const QJsonValue &value : secrets.value(QStringLiteral("prekeys")).toArray()) {
        const QJsonObject spk = value.toObject();
        const Prekey made = makePrekey(fromB64(spk.value(QStringLiteral("xPriv")).toString()),
                                       fromB64(spk.value(QStringLiteral("pqSeed")).toString()),
                                       qint64(spk.value(QStringLiteral("ts")).toDouble()));
        check(made.id == spk.value(QStringLiteral("id")).toString(),
              "the vector prekey derives its id");
        peer.device->addPrekey(made);
    }
    const qint64 saved = nowMs;
    nowMs = baseNow;
    peer.seal = RoomSeal(room, scope, Policy::Strict, name,
                         object(vectors, "nodes").value(name).toString(), peer.device);
    peer.seal.setParticipants(participants);
    const QJsonObject bundles = object(vectors, "bundles");
    for (auto it = bundles.constBegin(); it != bundles.constEnd(); ++it) {
        const RoomSeal::BundleResult result = peer.seal.acceptKeyFrame(it.value().toObject());
        check(result == RoomSeal::BundleResult::Approved ||
                  result == RoomSeal::BundleResult::Known,
              "both vector bundles are approved");
    }
    check(peer.device->prekeys().size() == 1, "no extra prekey is made for the vector device");
    nowMs = saved;
    if (state.isString())
        peer.device->importRatchets(room, object(object(vectors, "states"),
                                                 state.toString().toLatin1().constData()));
    return peer;
}

QString statusName(RoomSeal::Status status)
{
    switch (status) {
    case RoomSeal::Status::Opened:
        return QStringLiteral("opened");
    case RoomSeal::Status::Held:
        return QStringLiteral("held");
    case RoomSeal::Status::NotForMe:
        return QStringLiteral("notForMe");
    case RoomSeal::Status::Replay:
        return QStringLiteral("replay");
    case RoomSeal::Status::Invalid:
        return QStringLiteral("invalid");
    case RoomSeal::Status::Own:
        return QStringLiteral("own");
    case RoomSeal::Status::NoSession:
        return QStringLiteral("noSession");
    }
    return QString();
}

QJsonObject chat(const QString &text)
{
    return {{QStringLiteral("type"), QStringLiteral("chat")},
            {QStringLiteral("id"), text},
            {QStringLiteral("text"), text}};
}

RoomSeal::Status openAt(const Peer &peer, const QJsonObject &frame, qint64 at)
{
    nowMs = at;
    return peer.seal.open(frame).status;
}

QJsonObject sealAt(const Peer &peer, const QJsonObject &plain, qint64 at)
{
    nowMs = at;
    return peer.seal.sealObject(plain);
}

void testKdf()
{
    const QJsonObject k = object(vectors, "kdf");
    const QJsonObject s = object(k, "sessionId");
    check(forkmesh::chatv4::sessionId(fromB64u(s.value(QStringLiteral("dh")).toString()),
                                      fromB64(s.value(QStringLiteral("ct")).toString())) ==
              s.value(QStringLiteral("sid")).toString(),
          "the session id is id16(dh || ct)");
    const QJsonObject i = object(k, "init");
    const forkmesh::chatv4::KeyPair init = forkmesh::chatv4::kdfInit(
        i.value(QStringLiteral("room")).toString(), i.value(QStringLiteral("sender")).toString(),
        i.value(QStringLiteral("recipient")).toString(), i.value(QStringLiteral("spk")).toString(),
        i.value(QStringLiteral("sid")).toString(), fromB64(i.value(QStringLiteral("ss1")).toString()),
        fromB64(i.value(QStringLiteral("ss2")).toString()));
    check(b64(init.first) == i.value(QStringLiteral("rk")).toString() &&
              b64(init.second) == i.value(QStringLiteral("ck")).toString(),
          "KDF_INIT matches the reference");
    const QJsonObject r = object(k, "rootStep");
    const forkmesh::chatv4::KeyPair root =
        forkmesh::chatv4::kdfRk(fromB64(r.value(QStringLiteral("rk")).toString()),
                                fromB64(r.value(QStringLiteral("dhOut")).toString()),
                                fromB64(r.value(QStringLiteral("ss")).toString()));
    check(b64(root.first) == r.value(QStringLiteral("nextRk")).toString() &&
              b64(root.second) == r.value(QStringLiteral("ck")).toString(),
          "KDF_RK matches the reference");
    const QJsonObject c = object(k, "chainStep");
    const forkmesh::chatv4::KeyPair chain =
        forkmesh::chatv4::kdfCk(fromB64(c.value(QStringLiteral("ck")).toString()));
    check(b64(chain.first) == c.value(QStringLiteral("mk")).toString() &&
              b64(chain.second) == c.value(QStringLiteral("nextCk")).toString(),
          "KDF_CK matches the reference");
}

void testStatesRoundTrip()
{
    const QJsonObject states = object(vectors, "states");
    for (auto it = states.constBegin(); it != states.constEnd(); ++it) {
        auto holder = ChatDevice::inMemory(randomBytes(32));
        holder->importRatchets(room, it.value().toObject());
        check(compact(holder->exportRatchets(room)) == compact(it.value().toObject()),
              "a vector ratchet state imports and exports byte for byte");
    }
}

void testSequence()
{
    QHash<QString, Peer> devices;
    for (const QJsonValue &value : vectors.value(QStringLiteral("sequence")).toArray()) {
        const QJsonObject step = value.toObject();
        const QString name = step.value(QStringLiteral("device")).toString();
        if (step.contains(QStringLiteral("state")))
            devices.insert(name, device(name, step.value(QStringLiteral("state"))));
        const Peer &peer = devices[name];
        if (step.value(QStringLiteral("clearSeen")).toBool())
            peer.device->clearSeen();
        nowMs = qint64(step.value(QStringLiteral("at")).toDouble());
        const QString frameName = step.value(QStringLiteral("frame")).toString();
        const RoomSeal::Opened opened = peer.seal.open(frameNamed(frameName));
        const QString expect = step.value(QStringLiteral("expect")).toString();
        if (statusName(opened.status) != expect)
            std::fprintf(stderr, "  step %s/%s gave %s, want %s\n", qPrintable(name),
                         qPrintable(frameName), qPrintable(statusName(opened.status)),
                         qPrintable(expect));
        check(statusName(opened.status) == expect, "a vector sequence step gives its result");
        if (expect == QLatin1String("opened")) {
            check(opened.plain == object(vectors, "plain")
                                      .value(step.value(QStringLiteral("plain")).toString())
                                      .toObject(),
                  "an opened vector frame gives the reference plaintext");
            check(opened.senderKid ==
                      kidOf(name == QLatin1String("alice") ? QStringLiteral("bob")
                                                           : QStringLiteral("alice")),
                  "an opened vector frame names its sender");
        }
        if (frameName == QLatin1String("m1") && expect == QLatin1String("opened")) {
            const QJsonObject session =
                object(object(peer.device->exportRatchets(room), "peers"),
                       kidOf(QStringLiteral("alice")).toLatin1().constData())
                    .value(QStringLiteral("sessions"))
                    .toArray()
                    .first()
                    .toObject();
            const QJsonObject want = object(vectors, "bobAfterM1");
            check(session.value(QStringLiteral("rk")) == want.value(QStringLiteral("rk")) &&
                      session.value(QStringLiteral("ckr")) == want.value(QStringLiteral("ckr")) &&
                      session.value(QStringLiteral("nr")).toInteger() ==
                          want.value(QStringLiteral("nr")).toInteger() &&
                      session.value(QStringLiteral("skipped")).toArray().size() ==
                          want.value(QStringLiteral("skipped")).toInteger(),
                  "Bob's session after m2 then m1 matches the reference");
        }
    }
}

void testHeaders()
{
    const QJsonObject frame = frameNamed(QStringLiteral("m1"));
    const QJsonArray keys = frame.value(QStringLiteral("keys")).toArray();
    check(frame.value(QStringLiteral("v")).toInt() == 4 && keys.size() == 1 &&
              keys.first().toObject().value(QStringLiteral("kid")).toString() ==
                  kidOf(QStringLiteral("bob")),
          "a v4 frame has one entry per recipient and none for its sender");
    check(frameNamed(QStringLiteral("empty")).value(QStringLiteral("keys")).toArray().isEmpty() &&
              RoomSeal::isSealedFrame(frameNamed(QStringLiteral("empty"))) &&
              !RoomSeal::isLegacyFrame(frameNamed(QStringLiteral("empty"))),
          "a zero-entry v4 frame is a sealed frame");

    nowMs = baseNow;
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    const QJsonObject sealed = alice.seal.sealObject(chat(QStringLiteral("hi")));
    check(sealed.value(QStringLiteral("v")).toInt() == 4 &&
              sealed.value(QStringLiteral("keys")).toArray().size() == 1,
          "the desktop seals a DM as v4 with no entry for itself");
    check(alice.seal.open(sealed).status == RoomSeal::Status::Own,
          "the sender's own frame is own, not opened");
    const QJsonObject nobody = alice.seal.sealObjectFor(chat(QStringLiteral("nobody")), {});
    check(!nobody.isEmpty() && nobody.value(QStringLiteral("keys")).toArray().isEmpty() &&
              bob.seal.open(nobody).status == RoomSeal::Status::NotForMe,
          "a zero-entry frame is still sealed and is notForMe for the peer");
    check(bob.seal.open(sealed).status == RoomSeal::Status::Opened,
          "the peer opens the desktop's v4 frame");
}

void testConversationReordered()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    std::mt19937 rng(4);
    qint64 at = baseNow;
    bool all = true;
    for (int turn = 0; turn < 6; ++turn) {
        const Peer &sender = turn % 2 == 0 ? alice : bob;
        const Peer &receiver = turn % 2 == 0 ? bob : alice;
        QList<QJsonObject> frames;
        for (int i = 0; i < 5; ++i)
            frames.append(sealAt(sender, chat(QStringLiteral("%1-%2").arg(turn).arg(i)), at + i));
        std::shuffle(frames.begin(), frames.end(), rng);
        for (const QJsonObject &frame : std::as_const(frames))
            all = all && openAt(receiver, frame, at + 10) == RoomSeal::Status::Opened;
        at += 100;
    }
    check(all, "six turns of five shuffled messages all open");
}

void testLateMessage()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    const QJsonObject late = sealAt(alice, chat(QStringLiteral("late")), baseNow);
    check(openAt(bob, sealAt(alice, chat(QStringLiteral("a2")), baseNow + 1), baseNow + 2) ==
              RoomSeal::Status::Opened,
          "the second message opens first");
    check(openAt(alice, sealAt(bob, chat(QStringLiteral("b1")), baseNow + 3), baseNow + 4) ==
              RoomSeal::Status::Opened,
          "the reply opens");
    check(openAt(bob, sealAt(alice, chat(QStringLiteral("a3")), baseNow + 5), baseNow + 6) ==
              RoomSeal::Status::Opened,
          "a message on Alice's new chain opens");
    nowMs = baseNow + 7;
    const RoomSeal::Opened opened = bob.seal.open(late);
    check(opened.status == RoomSeal::Status::Opened &&
              opened.plain.value(QStringLiteral("text")).toString() == QStringLiteral("late"),
          "a message lost in the old chain opens from a skipped key");
    check(bob.seal.open(late).status == RoomSeal::Status::Replay,
          "the late message cannot be opened twice");
}

void testSimultaneousStart()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    const QJsonObject fromAlice = sealAt(alice, chat(QStringLiteral("a")), baseNow);
    const QJsonObject fromBob = sealAt(bob, chat(QStringLiteral("b")), baseNow);
    check(openAt(bob, fromAlice, baseNow + 1) == RoomSeal::Status::Opened &&
              openAt(alice, fromBob, baseNow + 1) == RoomSeal::Status::Opened,
          "two first messages sent at once both open");
    bool all = true;
    for (int i = 0; i < 3; ++i) {
        const qint64 at = baseNow + 10 * i;
        all = all && openAt(bob, sealAt(alice, chat(QStringLiteral("a%1").arg(i)), at), at) ==
                         RoomSeal::Status::Opened;
        all = all &&
              openAt(alice, sealAt(bob, chat(QStringLiteral("b%1").arg(i)), at + 5), at + 5) ==
                  RoomSeal::Status::Opened;
    }
    check(all, "the two sessions converge and keep opening");
}

void testResetRecovery()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    openAt(bob, sealAt(alice, chat(QStringLiteral("a1")), baseNow), baseNow);
    openAt(alice, sealAt(bob, chat(QStringLiteral("b1")), baseNow + 1), baseNow + 1);
    bob.device->importRatchets(room, QJsonObject{{QStringLiteral("v"), 4},
                                                 {QStringLiteral("peers"), QJsonObject()}});
    const QJsonObject broken = sealAt(alice, chat(QStringLiteral("a2")), baseNow + 2);
    nowMs = baseNow + 3;
    const RoomSeal::Opened lost = bob.seal.open(broken);
    check(lost.status == RoomSeal::Status::NoSession &&
              lost.senderKid == alice.device->kid(),
          "a frame for a lost session is noSession naming the sender");
    bob.seal.resetSessions(alice.device->kid());
    nowMs = baseNow + 4;
    const QJsonObject reset = bob.seal.sealObjectFor(
        QJsonObject{{QStringLiteral("type"), forkmesh::chatv4::resetType()}},
        {alice.device->kid()});
    const QJsonObject entry = reset.value(QStringLiteral("keys")).toArray().first().toObject();
    check(entry.value(QStringLiteral("sid")).toString() ==
              forkmesh::chatv4::sessionId(fromB64u(entry.value(QStringLiteral("dh")).toString()),
                                          fromB64(entry.value(QStringLiteral("ct")).toString())),
          "the reset frame starts a new session");
    nowMs = baseNow + 5;
    const RoomSeal::Opened opened = alice.seal.open(reset);
    check(opened.status == RoomSeal::Status::Opened && RoomSeal::isResetPlain(opened.plain),
          "the peer opens the reset");
    check(openAt(bob, sealAt(alice, chat(QStringLiteral("a2 again")), baseNow + 6),
                 baseNow + 7) == RoomSeal::Status::Opened,
          "messages open again after the reset");
}

void testPeekDoesNotAdvance()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    const QJsonObject frame = sealAt(alice, chat(QStringLiteral("a1")), baseNow);
    const QByteArray before = compact(bob.device->exportRatchets(room));
    check(bob.seal.open(frame, false, false).status == RoomSeal::Status::Opened &&
              compact(bob.device->exportRatchets(room)) == before,
          "opening with commit=false stores nothing");
    check(bob.seal.open(frame).status == RoomSeal::Status::Opened &&
              bob.seal.open(frame).status == RoomSeal::Status::Replay,
          "the frame opens once more with commit and then is a replay");
}

void testTooManySkipped()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    for (int i = 0; i <= forkmesh::chatv4::kMaxSkip; ++i)
        sealAt(alice, chat(QStringLiteral("lost")), baseNow);
    const QJsonObject far = sealAt(alice, chat(QStringLiteral("far")), baseNow);
    check(openAt(bob, far, baseNow) == RoomSeal::Status::NoSession,
          "more than MAX_SKIP missing messages need a reset");
}

void testExpiredPrekey()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    const QJsonObject frame = sealAt(alice, chat(QStringLiteral("a1")), baseNow);
    bob.device->wipeExpired(bob.device->prekeys().first().exp + 1);
    check(openAt(bob, frame, baseNow) == RoomSeal::Status::NoSession,
          "a wiped prekey cannot start a session");
}

void testOtherRoomAndTamper()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    const QJsonObject frame = sealAt(alice, chat(QStringLiteral("a1")), baseNow);
    RoomSeal other(QStringLiteral("another-room"), scope, Policy::Strict, QStringLiteral("bob"),
                   object(vectors, "nodes").value(QStringLiteral("bob")).toString(), bob.device);
    check(other.open(frame).status == RoomSeal::Status::Invalid,
          "a frame signed for one room is invalid in another");
    QJsonObject withSelf = frame;
    QJsonArray keys = withSelf.value(QStringLiteral("keys")).toArray();
    QJsonObject selfEntry = keys.first().toObject();
    selfEntry.insert(QStringLiteral("kid"), alice.device->kid());
    keys.append(selfEntry);
    QList<QJsonObject> sorted;
    for (const QJsonValue &value : std::as_const(keys))
        sorted.append(value.toObject());
    std::sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("kid")).toString() < b.value(QStringLiteral("kid")).toString();
    });
    keys = QJsonArray();
    for (const QJsonObject &entry : std::as_const(sorted))
        keys.append(entry);
    withSelf.insert(QStringLiteral("keys"), keys);
    check(openAt(bob, withSelf, baseNow) == RoomSeal::Status::Invalid,
          "a frame with an entry for its sender is invalid");
    QJsonObject tampered = frame;
    keys = tampered.value(QStringLiteral("keys")).toArray();
    QJsonObject entry = keys.first().toObject();
    entry.insert(QStringLiteral("n"), 7);
    keys.replace(0, entry);
    tampered.insert(QStringLiteral("keys"), keys);
    check(openAt(bob, tampered, baseNow) == RoomSeal::Status::Invalid,
          "a changed ratchet header breaks the signature");
    check(openAt(bob, frame, baseNow) == RoomSeal::Status::Opened,
          "refused frames do not burn the genuine frame");
}

void testSessionsBounded()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    bool all = true;
    for (int i = 0; i < forkmesh::chatv4::kSessionsKept + 3; ++i) {
        alice.seal.resetSessions(bob.device->kid());
        all = all && openAt(bob, sealAt(alice, chat(QString::number(i)), baseNow + i),
                            baseNow + i) == RoomSeal::Status::Opened;
    }
    const QJsonObject peers = object(bob.device->exportRatchets(room), "peers");
    check(all && object(peers, alice.device->kid().toLatin1().constData())
                         .value(QStringLiteral("sessions"))
                         .toArray()
                         .size() == forkmesh::chatv4::kSessionsKept,
          "at most four sessions are kept per peer");
}

void testVersionsStayInTheirRooms()
{
    Peer alice = device(QStringLiteral("alice"));
    Peer bob = device(QStringLiteral("bob"));
    RoomSeal v3(room, scope, Policy::Strict, QStringLiteral("alice"),
                object(vectors, "nodes").value(QStringLiteral("alice")).toString(), alice.device);
    v3.setParticipants(participants);
    v3.setRatchet(false);
    nowMs = baseNow;
    const QJsonObject v3Frame = v3.sealObject(chat(QStringLiteral("v3")));
    check(v3Frame.value(QStringLiteral("v")).toInt() == 3 &&
              bob.seal.open(v3Frame).status == RoomSeal::Status::Invalid,
          "a v3 frame in a direct room is ignored");
    const QByteArray aliceNodeSeed(32, 'a');
    const QByteArray bobNodeSeed(32, 'b');
    const QString aliceNode = b64u(ed25519Public(aliceNodeSeed));
    const QString bobNode = b64u(ed25519Public(bobNodeSeed));
    RoomSeal openRoom(QStringLiteral("open-room"), QString(), Policy::Open,
                      QStringLiteral("alice"), aliceNode, alice.device);
    RoomSeal openPeer(QStringLiteral("open-room"), QString(), Policy::Open, QStringLiteral("bob"),
                      bobNode, bob.device);
    openRoom.setNodeSigner([aliceNodeSeed](const QByteArray &m) {
        return ed25519Sign(aliceNodeSeed, m);
    });
    openPeer.setNodeSigner([bobNodeSeed](const QByteArray &m) {
        return ed25519Sign(bobNodeSeed, m);
    });
    openRoom.acceptKeyFrame(openPeer.keyFrame());
    openPeer.acceptKeyFrame(openRoom.keyFrame());
    const QJsonObject openFrame = openRoom.sealObject(chat(QStringLiteral("open")));
    check(openFrame.value(QStringLiteral("v")).toInt() == 3 &&
              openPeer.open(openFrame).status == RoomSeal::Status::Opened,
          "open rooms keep v3");
    QJsonObject relabelled = frameNamed(QStringLiteral("m1"));
    check(openPeer.open(relabelled).status == RoomSeal::Status::Invalid,
          "a v4 frame in an open room is ignored");
}

void testPersistence()
{
    clearStoredState();
    auto local = ChatDevice::local();
    auto peerDevice = ChatDevice::inMemory(randomBytes(32));
    const QString dmRoom = QStringLiteral("chat-direct:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee:v1");
    const QString dmScope = QStringLiteral("direct:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee");
    RoomSeal mine(dmRoom, dmScope, Policy::Strict, QStringLiteral("alice"),
                  QStringLiteral("alice-desk"), local);
    RoomSeal theirs(dmRoom, dmScope, Policy::Strict, QStringLiteral("bob"),
                    QStringLiteral("bob-desk"), peerDevice);
    mine.setParticipants({QStringLiteral("alice"), QStringLiteral("bob")});
    theirs.setParticipants({QStringLiteral("alice"), QStringLiteral("bob")});
    mine.acceptKeyFrame(theirs.keyFrame());
    theirs.acceptKeyFrame(mine.keyFrame());
    const QJsonObject frame = mine.sealObject(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("chat")},
                    {QStringLiteral("senderId"), QStringLiteral("alice-desk")}});
    const QJsonObject stored =
        QJsonDocument::fromJson(storedSetting(QStringLiteral("chat/e2ee/ratchetV4/") +
                                              b64u(dmRoom.toUtf8()))
                                    .toByteArray())
            .object();
    check(stored.value(QStringLiteral("v")).toInt() == 4 &&
              object(object(stored, "peers"), peerDevice->kid().toLatin1().constData())
                      .value(QStringLiteral("sessions"))
                      .toArray()
                      .size() == 1,
          "the ratchet is written to the settings store before the frame is sent");
    check(theirs.open(frame).status == RoomSeal::Status::Opened,
          "the persisted device's frame opens");
}

} // namespace

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ForkMeshTests"));
    app.setApplicationName(QStringLiteral("chat-v4"));
    QFile file(QStringLiteral(FORKMESH_CHAT_V4_VECTORS));
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", FORKMESH_CHAT_V4_VECTORS);
        return 1;
    }
    vectors = QJsonDocument::fromJson(file.readAll()).object();
    room = vectors.value(QStringLiteral("room")).toString();
    scope = vectors.value(QStringLiteral("scope")).toString();
    for (const QJsonValue &value : vectors.value(QStringLiteral("participants")).toArray())
        participants.append(value.toString());
    baseNow = vectors.value(QStringLiteral("now")).toInteger();
    nowMs = baseNow;
    check(vectors.value(QStringLiteral("version")).toInt() == 4, "the vectors are v4");

    testKdf();
    testStatesRoundTrip();
    testSequence();
    testHeaders();
    testConversationReordered();
    testLateMessage();
    testSimultaneousStart();
    testResetRecovery();
    testPeekDoesNotAdvance();
    testTooManySkipped();
    testExpiredPrekey();
    testOtherRoomAndTamper();
    testSessionsBounded();
    testVersionsStayInTheirRooms();
    testPersistence();

    if (failures) {
        std::fprintf(stderr, "%d chat v4 check(s) failed.\n", failures);
        return 1;
    }
    std::printf("All chat v4 checks passed.\n");
    return 0;
}
