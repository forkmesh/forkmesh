#include "RoomSeal.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#include <cstdio>
#include <memory>

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
qint64 nowMs = 0;

QString nodeFor(const char *name)
{
    return vectors.value(QStringLiteral("nodes")).toObject().value(QLatin1String(name)).toString();
}

QJsonObject node(const char *path)
{
    QJsonObject current = vectors;
    for (const QString &part : QString::fromLatin1(path).split(QLatin1Char('.')))
        current = current.value(part).toObject();
    return current;
}

qint64 integer(const QJsonObject &object, const char *key)
{
    return qint64(object.value(QLatin1String(key)).toDouble());
}

std::shared_ptr<ChatDevice> device(const QString &name)
{
    const QJsonObject secrets =
        vectors.value(QStringLiteral("devices")).toObject().value(name).toObject();
    auto built = ChatDevice::inMemory(fromB64(secrets.value(QStringLiteral("identitySeed")).toString()));
    check(built->idkText() == secrets.value(QStringLiteral("idk")).toString(),
          "the identity seed derives the vector's Ed25519 public key");
    check(built->kid() == secrets.value(QStringLiteral("kid")).toString(),
          "the device kid is id16 of the identity key");
    built->setClock([] { return nowMs; });
    for (const QJsonValue &value : secrets.value(QStringLiteral("prekeys")).toArray()) {
        const QJsonObject spk = value.toObject();
        const Prekey made = makePrekey(fromB64(spk.value(QStringLiteral("xPriv")).toString()),
                                       fromB64(spk.value(QStringLiteral("pqSeed")).toString()),
                                       integer(spk, "ts"));
        check(made.id == spk.value(QStringLiteral("id")).toString(),
              "the prekey id is id16(x || pq)");
        check(b64u(made.x) == spk.value(QStringLiteral("x")).toString(),
              "the X25519 prekey public half matches");
        check(b64u(made.pq) == spk.value(QStringLiteral("pq")).toString(),
              "the ML-KEM-768 key derived from the 64-byte seed matches");
        check(made.exp == integer(spk, "exp"), "a prekey lives for 8 days");
        built->addPrekey(made);
    }
    return built;
}

QJsonObject bundle(const char *name)
{
    return vectors.value(QStringLiteral("bundles")).toObject().value(QLatin1String(name)).toObject();
}

QJsonObject sealedFrame(const char *name)
{
    return node("sealed").value(QLatin1String(name)).toObject().value(QStringLiteral("frame")).toObject();
}

struct BobView {
    std::shared_ptr<ChatDevice> bob;
    RoomSeal seal;
};

BobView bobWithTrust()
{
    BobView view;
    view.bob = device(QStringLiteral("bob"));
    view.seal = RoomSeal(room, scope, Policy::Strict, QStringLiteral("bob"), nodeFor("bob"),
                         view.bob);
    view.seal.setRatchet(false);
    view.seal.setParticipants(participants);
    check(view.seal.acceptKeyFrame(bundle("alice")) == RoomSeal::BundleResult::Approved,
          "the first device of an account is approved on first use");
    check(view.seal.acceptKeyFrame(bundle("bob")) == RoomSeal::BundleResult::Known &&
              view.seal.isApproved(view.bob->kid()),
          "this device's own bundle is its own approved record");
    check(view.seal.acceptKeyFrame(bundle("alice2")) == RoomSeal::BundleResult::Pending,
          "a second device of an approved account is pending in a strict room");
    return view;
}

void testBundles()
{
    for (const char *name : {"alice", "alice2", "bob", "bobOld", "injectedOutsider",
                             "injectedAsBob"})
        check(verifyBundle(bundle(name), room, nowMs), "a vector bundle verifies");
    check(node("bundles.invalid").contains(QStringLiteral("wrongNode")),
          "the vectors cover a bundle whose node was changed");
    const QJsonObject invalid = node("bundles.invalid");
    for (auto it = invalid.constBegin(); it != invalid.constEnd(); ++it)
        check(!verifyBundle(it.value().toObject(), room, nowMs),
              "an invalid vector bundle is refused");
    check(!verifyBundle(bundle("alice"), room, integer(node("bundleChecks"), "expiredAt")),
          "a bundle whose prekey expired is refused");

    auto alice = device(QStringLiteral("alice"));
    RoomSeal aliceSeal(room, scope, Policy::Strict, QStringLiteral("alice"),
                       nodeFor("alice"), alice);
    check(aliceSeal.keyFrame() == bundle("alice"),
          "the desktop's own bundle is byte-identical to the reference");

    BobView view = bobWithTrust();
    QJsonObject changed = bundle("alice");
    changed.insert(QStringLiteral("acct"), QStringLiteral("mallory"));
    check(view.seal.acceptKeyFrame(changed) == RoomSeal::BundleResult::Rejected &&
              view.seal.acctForKeyId(alice->kid()) == QStringLiteral("alice"),
          "a known kid cannot change its account");
    QJsonObject renamed = bundle("alice");
    renamed.insert(QStringLiteral("node"), QStringLiteral("node-mallory"));
    check(view.seal.acceptKeyFrame(renamed) == RoomSeal::BundleResult::Rejected &&
              view.seal.nodeForKeyId(alice->kid()) == nodeFor("alice"),
          "a known kid cannot change its sender id");
    check(view.seal.acceptKeyFrame(bundle("injectedOutsider")) ==
              RoomSeal::BundleResult::Rejected,
          "a server-injected device of a non-participant is dropped in the DM");
    check(view.seal.acceptKeyFrame(bundle("injectedAsBob")) ==
              RoomSeal::BundleResult::Pending,
          "a server-injected device claiming Bob's account is held as pending");
    RoomSeal open(room, QStringLiteral("open-scope"), Policy::Open, QStringLiteral("bob"),
                  nodeFor("bob"), view.bob);
    check(open.acceptKeyFrame(bundle("injectedOutsider")) == RoomSeal::BundleResult::Approved,
          "an open room approves any valid bundle");
    check(node("bundleChecks").value(QStringLiteral("pendingStateForAlice2AtBob")).toString() ==
              trustStateName(TrustState::Pending),
          "the reference agrees the second phone is pending");
}

void testOpenSequence()
{
    BobView view = bobWithTrust();
    const QJsonObject sealed = node("sealed");
    nowMs = vectors.value(QStringLiteral("now")).toInteger();
    const RoomSeal::Opened first = view.seal.open(sealedFrame("first"));
    check(first.status == RoomSeal::Status::Opened &&
              first.plain == sealed.value(QStringLiteral("first")).toObject()
                                 .value(QStringLiteral("plain")).toObject() &&
              first.senderKid == sealed.value(QStringLiteral("first")).toObject()
                                     .value(QStringLiteral("senderKid")).toString() &&
              first.senderAcct == QStringLiteral("alice") &&
              first.senderNode == nodeFor("alice") &&
              first.plain.value(QStringLiteral("senderId")).toString() == nodeFor("alice"),
          "the first frame opens to the reference plaintext and sender");
    check(view.seal.open(sealedFrame("replay")).status == RoomSeal::Status::Replay,
          "the same frame again is a replay");
    check(view.seal.open(sealedFrame("replay"), true).status == RoomSeal::Status::Opened,
          "the relay's history replay skips only the already-seen check");
    QJsonObject replayTampered = sealedFrame("tampered");
    check(view.seal.open(replayTampered, true).status == RoomSeal::Status::Invalid,
          "a history replay still needs a valid signature");
    nowMs += 1000;
    const RoomSeal::Opened second = view.seal.open(sealedFrame("second"));
    check(second.status == RoomSeal::Status::Opened &&
              second.plain == sealed.value(QStringLiteral("second")).toObject()
                                  .value(QStringLiteral("plain")).toObject(),
          "the second frame, sealed to Bob's newer prekey, opens");
    nowMs -= 1000;
    check(view.seal.open(sealedFrame("tampered")).status == RoomSeal::Status::Invalid,
          "a body changed in transit is invalid");
    nowMs += 2000;
    check(view.seal.open(sealedFrame("forgedSenderId")).status == RoomSeal::Status::Invalid,
          "a plaintext senderId that is not the device's account is a forgery");
    nowMs += 1000;
    const QJsonObject pending = sealed.value(QStringLiteral("fromPendingDevice")).toObject();
    QStringList kids;
    for (const QJsonValue &value : pending.value(QStringLiteral("frame")).toObject()
                                       .value(QStringLiteral("keys")).toArray())
        kids.append(value.toObject().value(QStringLiteral("kid")).toString());
    QStringList expected;
    for (const QJsonValue &value : pending.value(QStringLiteral("keys")).toArray())
        expected.append(value.toString());
    check(kids == expected, "the pending device's frame lists its recipients in kid order");
    const RoomSeal::Opened held = view.seal.open(sealedFrame("fromPendingDevice"));
    check(held.status == RoomSeal::Status::Held &&
              held.senderKid == node("devices.alice2").value(QStringLiteral("kid")).toString(),
          "a frame from a pending device is held");
    check(view.seal.takeReleasedHeld().isEmpty(),
          "a held frame stays held while its device is pending");
    view.seal.approve(node("devices.alice2").value(QStringLiteral("kid")).toString());
    const QList<QJsonObject> released = view.seal.takeReleasedHeld();
    check(released.size() == 1, "approving the device releases its held frame");
    check(!released.isEmpty() &&
              view.seal.open(released.first()).status == RoomSeal::Status::Opened,
          "the released frame opens once the device is approved");
    nowMs = vectors.value(QStringLiteral("now")).toInteger();
}

void testForgedFrameKeepsCounter()
{
    BobView view = bobWithTrust();
    check(view.seal.open(sealedFrame("tampered")).status == RoomSeal::Status::Invalid,
          "the tampered frame is refused first");
    check(view.seal.open(sealedFrame("first")).status == RoomSeal::Status::Opened,
          "a forged frame does not burn the genuine frame's counter");
}

void testBlockedSender()
{
    BobView view = bobWithTrust();
    view.seal.block(node("devices.alice").value(QStringLiteral("kid")).toString());
    check(view.seal.open(sealedFrame("first")).status == RoomSeal::Status::Held,
          "a blocked device's frame is held, not opened");
    check(view.seal.takeReleasedHeld().isEmpty(),
          "a blocked device's frames are dropped, never released");
}

void testWipedPrekey()
{
    BobView view = bobWithTrust();
    view.bob->wipeExpired(integer(node("afterBobWipesOldPrekey"), "now"));
    check(view.seal.open(sealedFrame("first")).status == RoomSeal::Status::NotForMe,
          "after the old prekey is wiped its frames cannot be opened");
}

void testOtherRoom()
{
    BobView view = bobWithTrust();
    RoomSeal other(QStringLiteral("another-room"), scope, Policy::Strict,
                   QStringLiteral("bob"), nodeFor("bob"), view.bob);
    other.setRatchet(false);
    check(other.open(sealedFrame("first")).status == RoomSeal::Status::Invalid,
          "a frame signed for one room is invalid in another");
}

void testKek()
{
    const QJsonObject k = node("kek");
    auto bob = device(QStringLiteral("bob"));
    const Prekey *spk = bob->findPrekey(k.value(QStringLiteral("spkId")).toString());
    check(spk != nullptr, "the KEK vector names Bob's prekey");
    if (!spk)
        return;
    const QByteArray ct = fromB64(k.value(QStringLiteral("ct")).toString());
    check(mlkemDecapsulate(spk->pqSeed, ct) == fromB64(k.value(QStringLiteral("ss2")).toString()),
          "ML-KEM-768 decapsulation recovers the reference shared secret");
    check(x25519Shared(fromB64(k.value(QStringLiteral("ephPriv")).toString()), spk->x) ==
              fromB64(k.value(QStringLiteral("ss1")).toString()),
          "X25519 from the sender side matches");
    check(x25519Shared(spk->xPriv, fromB64u(k.value(QStringLiteral("ephPub")).toString())) ==
              fromB64(k.value(QStringLiteral("ss1")).toString()),
          "X25519 from the recipient side matches");
    const QByteArray derived =
        kek(k.value(QStringLiteral("room")).toString(),
            k.value(QStringLiteral("sender")).toString(),
            k.value(QStringLiteral("recipient")).toString(),
            k.value(QStringLiteral("spkId")).toString(),
            fromB64u(k.value(QStringLiteral("ephPub")).toString()), ct,
            fromB64(k.value(QStringLiteral("ss1")).toString()),
            fromB64(k.value(QStringLiteral("ss2")).toString()));
    check(b64(derived) == k.value(QStringLiteral("kek")).toString(),
          "the HKDF key-encryption key matches the reference");
    QByteArray encapCt;
    QByteArray encapSs;
    check(mlkemEncapsulate(spk->pq, &encapCt, &encapSs) &&
              mlkemDecapsulate(spk->pqSeed, encapCt) == encapSs,
          "a fresh encapsulation to the prekey decapsulates to the same secret");
}

void testSafetyNumbers()
{
    for (const char *name : {"twoDevices", "threeDevices"}) {
        const QJsonObject s = node("safety").value(QLatin1String(name)).toObject();
        QList<QByteArray> idks;
        for (const QJsonValue &value : s.value(QStringLiteral("idks")).toArray())
            idks.append(fromB64u(value.toString()));
        const SafetyNumber number = safetyNumber(s.value(QStringLiteral("scope")).toString(), idks);
        check(number.digits == s.value(QStringLiteral("digits")).toString() &&
                  number.digits.size() == 60,
              "the 60 safety digits match the reference");
        check(number.qr == s.value(QStringLiteral("qr")).toString(),
              "the safety QR string matches the reference");
        QList<QByteArray> reversed(idks.crbegin(), idks.crend());
        check(safetyNumber(s.value(QStringLiteral("scope")).toString(), reversed).digits ==
                  number.digits,
              "both people see the same number whatever the key order");
        check(safetyGroups(number.digits).size() == 12, "the number is shown in 12 groups");
    }
    check(node("safety.twoDevices").value(QStringLiteral("digits")) !=
              node("safety.threeDevices").value(QStringLiteral("digits")),
          "another approved device changes the safety number");

    BobView view = bobWithTrust();
    check(view.seal.safetyNumber().digits ==
              node("safety.twoDevices").value(QStringLiteral("digits")).toString(),
          "the room's safety number covers its approved devices");
    view.seal.markVerified();
    check(view.seal.isVerified(), "comparing the number marks the devices verified");
    int changes = 0;
    const int token = view.bob->subscribe(
        [&changes](ChatDevice::Event event, const QString &, const QString &) {
            if (event == ChatDevice::Event::SafetyNumberChanged)
                ++changes;
        });
    view.seal.approve(node("devices.alice2").value(QStringLiteral("kid")).toString());
    view.bob->unsubscribe(token);
    check(changes == 1 && !view.seal.isVerified() &&
              view.seal.safetyNumber().digits ==
                  node("safety.threeDevices").value(QStringLiteral("digits")).toString(),
          "approving a device changes the number, clears verified and says so");
}

void testLink()
{
    const QJsonObject link = node("link");
    const QJsonObject iHello = link.value(QStringLiteral("initiatorHello")).toObject();
    const QJsonObject rHello = link.value(QStringLiteral("responderHello")).toObject();
    check(isLinkHello(iHello) && isLinkHello(rHello), "both vector hellos are well formed");
    const QByteArray transcript = linkTranscript(iHello, rHello);
    check(b64u(transcript) == link.value(QStringLiteral("transcript")).toString(),
          "the link transcript hash matches");
    const QByteArray iIdk = fromB64u(iHello.value(QStringLiteral("idk")).toString());
    const QByteArray rIdk = fromB64u(rHello.value(QStringLiteral("idk")).toString());
    const QByteArray iSig = fromB64u(link.value(QStringLiteral("initiatorAuthSig")).toString());
    const QByteArray rSig = fromB64u(link.value(QStringLiteral("responderAuthSig")).toString());
    check(ed25519Verify(iIdk, iSig, linkAuthMessage(QStringLiteral("i"), transcript)),
          "the initiator's auth signature verifies");
    check(ed25519Verify(rIdk, rSig, linkAuthMessage(QStringLiteral("r"), transcript)),
          "the responder's auth signature verifies");
    check(!ed25519Verify(iIdk, iSig, linkAuthMessage(QStringLiteral("r"), transcript)),
          "an auth signature cannot be replayed in the other role");
    check(ed25519Sign(fromB64(link.value(QStringLiteral("initiatorIdentitySeed")).toString()),
                      linkAuthMessage(QStringLiteral("i"), transcript)) == iSig,
          "the desktop signs the initiator auth exactly as the reference");
    const QByteArray iEph = fromB64(link.value(QStringLiteral("initiatorEphPriv")).toString());
    const QByteArray rEph = fromB64(link.value(QStringLiteral("responderEphPriv")).toString());
    check(b64u(x25519Public(iEph)) == iHello.value(QStringLiteral("eph")).toString(),
          "the initiator's ephemeral key matches its hello");
    QByteArray i2r;
    QByteArray r2i;
    QByteArray i2rPeer;
    QByteArray r2iPeer;
    check(linkKeys(iEph, x25519Public(rEph), transcript, &i2r, &r2i) &&
              linkKeys(rEph, x25519Public(iEph), transcript, &i2rPeer, &r2iPeer) &&
              i2r == i2rPeer && r2i == r2iPeer,
          "both ends derive the same direction keys");
    check(b64(i2r) == link.value(QStringLiteral("keyI2R")).toString() &&
              b64(r2i) == link.value(QStringLiteral("keyR2I")).toString(),
          "the direction keys match the reference");
    for (const char *name : {"frame0", "frame1"}) {
        const QJsonObject frame = link.value(QLatin1String(name)).toObject();
        QByteArray plain;
        check(linkOpen(i2r, quint64(integer(frame, "counter")),
                       fromB64(frame.value(QStringLiteral("data")).toString()), &plain) &&
                  QJsonDocument::fromJson(plain).object() ==
                      frame.value(QStringLiteral("plain")).toObject(),
              "a reference link frame opens under its counter");
    }
    QByteArray ignored;
    check(!linkOpen(i2r, 1,
                    fromB64(link.value(QStringLiteral("frame0")).toObject()
                                .value(QStringLiteral("data")).toString()),
                    &ignored),
          "a link frame does not open under another counter");
    check(linkNonce(1) == QByteArray::fromHex("000000000000000000000001"),
          "the link nonce is four zero bytes and the big-endian counter");
    const QByteArray sealed = linkSeal(r2i, 7, QByteArrayLiteral("{\"type\":\"focus\"}"));
    QByteArray opened;
    check(linkOpen(r2i, 7, sealed, &opened) &&
              opened == QByteArrayLiteral("{\"type\":\"focus\"}") &&
              !linkOpen(i2r, 7, sealed, &ignored),
          "a frame sealed in one direction opens only with that direction's key");
}

void testRoundTrips()
{
    nowMs = 1800000000000ll;
    auto alice = ChatDevice::inMemory(randomBytes(32));
    auto bob = ChatDevice::inMemory(randomBytes(32));
    auto carol = ChatDevice::inMemory(randomBytes(32));
    for (auto *device : {&alice, &bob, &carol})
        (*device)->setClock([] { return nowMs; });
    const QString dmRoom = QStringLiteral("chat-direct:ffffffffffffffffffffffffffffffff:v1");
    const QString dmScope = QStringLiteral("direct:ffffffffffffffffffffffffffffffff");
    RoomSeal aliceSeal(dmRoom, dmScope, Policy::Strict, QStringLiteral("alice"),
                       QStringLiteral("alice-desk"), alice);
    RoomSeal bobSeal(dmRoom, dmScope, Policy::Strict, QStringLiteral("bob"),
                     QStringLiteral("bob-desk"), bob);
    RoomSeal carolSeal(dmRoom, dmScope, Policy::Strict, QStringLiteral("carol"),
                       QStringLiteral("carol-desk"), carol);
    aliceSeal.setParticipants({QStringLiteral("Alice"), QStringLiteral("bob")});
    bobSeal.setParticipants({QStringLiteral("alice"), QStringLiteral("bob")});
    for (RoomSeal *v3Seal : {&aliceSeal, &bobSeal, &carolSeal})
        v3Seal->setRatchet(false);
    check(aliceSeal.isValid() && alice->newestPrekey() != nullptr,
          "a new device makes its first prekey on start");
    check(RoomSeal::isKeyFrame(aliceSeal.keyFrame()) &&
              verifyBundle(aliceSeal.keyFrame(), dmRoom, nowMs),
          "a fresh bundle verifies");
    bobSeal.acceptKeyFrame(aliceSeal.keyFrame());
    aliceSeal.acceptKeyFrame(bobSeal.keyFrame());
    carolSeal.acceptKeyFrame(aliceSeal.keyFrame());

    const QJsonObject plain{{QStringLiteral("type"), QStringLiteral("chat")},
                            {QStringLiteral("id"), QStringLiteral("rt-1")},
                            {QStringLiteral("senderId"), QStringLiteral("alice-desk")},
                            {QStringLiteral("text"), QStringLiteral("round trip")}};
    const QJsonObject frame = aliceSeal.sealObject(plain);
    check(RoomSeal::isSealedFrame(frame) && frame.value(QStringLiteral("keys")).toArray().size() == 2,
          "a DM is sealed to the approved peer and to the sender");
    check(!QJsonDocument(frame).toJson().contains("round trip"),
          "a sealed frame carries no plaintext");
    const RoomSeal::Opened opened = bobSeal.open(frame);
    check(opened.status == RoomSeal::Status::Opened && opened.plain == plain &&
              opened.senderAcct == QStringLiteral("alice") &&
              opened.senderNode == QStringLiteral("alice-desk"),
          "the recipient opens the desktop's own frame");
    check(aliceSeal.open(frame).status == RoomSeal::Status::Opened &&
              aliceSeal.open(frame).status == RoomSeal::Status::Replay,
          "the sender opens its own copy once");
    check(carolSeal.open(frame).status == RoomSeal::Status::NotForMe,
          "a device the sender never approved has no key entry");
    check(bobSeal.open(frame).status == RoomSeal::Status::Replay,
          "a relayed copy of the same frame is a replay");

    QJsonObject forged = frame;
    QJsonArray keys = forged.value(QStringLiteral("keys")).toArray();
    QJsonObject entry = keys.at(0).toObject();
    entry.insert(QStringLiteral("nonce"), b64(randomBytes(12)));
    keys.replace(0, entry);
    forged.insert(QStringLiteral("keys"), keys);
    check(bobSeal.open(forged).status == RoomSeal::Status::Invalid,
          "a changed key entry breaks the signature");

    const QJsonObject onlySelf = aliceSeal.sealObjectFor(plain, QStringList());
    check(onlySelf.value(QStringLiteral("keys")).toArray().size() == 1 &&
              bobSeal.open(onlySelf).status == RoomSeal::Status::NotForMe,
          "sealing to a subset leaves everyone else out");

    nowMs += kPrekeyRotateMs;
    QStringList rotated;
    const int token = alice->subscribe(
        [&rotated](ChatDevice::Event event, const QString &, const QString &kid) {
            if (event == ChatDevice::Event::PrekeyRotated)
                rotated.append(kid);
        });
    const QJsonObject later = aliceSeal.sealObject(plain);
    alice->unsubscribe(token);
    check(rotated.size() == 1 && alice->prekeys().size() == 2,
          "a prekey 24 hours old is rotated before the next seal");
    QJsonObject latePlain = plain;
    latePlain.insert(QStringLiteral("id"), QStringLiteral("rt-2"));
    check(bobSeal.open(aliceSeal.sealObject(latePlain)).status == RoomSeal::Status::Opened,
          "frames keep opening across a rotation");
    nowMs += kPrekeyLifeMs + 1;
    alice->maintainPrekeys();
    check(alice->prekeys().size() == 1 && alice->newestPrekey()->ts == nowMs,
          "expired prekeys are wiped and a fresh one is made");
    for (int i = 0; i < 12; ++i) {
        nowMs += kPrekeyRotateMs;
        alice->maintainPrekeys();
    }
    check(alice->prekeys().size() <= kMaxPrekeys, "at most ten prekeys are kept");
    Q_UNUSED(later);

    auto bobPhone = ChatDevice::inMemory(randomBytes(32));
    bobPhone->setClock([] { return nowMs; });
    RoomSeal bobPhoneSeal(dmRoom, dmScope, Policy::Strict, QStringLiteral("bob"),
                          QStringLiteral("bob-phone"), bobPhone);
    check(aliceSeal.acceptKeyFrame(carolSeal.keyFrame()) == RoomSeal::BundleResult::Rejected,
          "a third account's bundle is dropped in a DM");
    aliceSeal.acceptKeyFrame(bobSeal.keyFrame());
    check(aliceSeal.acceptKeyFrame(bobPhoneSeal.keyFrame()) == RoomSeal::BundleResult::Pending,
          "the other person's new phone is pending");
    QJsonObject toBob = plain;
    toBob.insert(QStringLiteral("id"), QStringLiteral("rt-3"));
    const QJsonObject pendingFrame = aliceSeal.sealObject(toBob);
    bool phoneListed = false;
    for (const QJsonValue &value : pendingFrame.value(QStringLiteral("keys")).toArray())
        phoneListed = phoneListed ||
                      value.toObject().value(QStringLiteral("kid")).toString() == bobPhone->kid();
    check(!phoneListed, "a pending device gets nothing sealed to it");

    RoomSeal openRoom(QStringLiteral("#mesh-room"), QString(), Policy::Open,
                      QStringLiteral("node-a"), QStringLiteral("node-a"), alice);
    RoomSeal openPeer(QStringLiteral("#mesh-room"), QString(), Policy::Open,
                      QStringLiteral("node-b"), QStringLiteral("node-b"), bob);
    RoomSeal openPeer2(QStringLiteral("#mesh-room"), QString(), Policy::Open,
                       QStringLiteral("node-b"), QStringLiteral("node-b2"), bobPhone);
    check(openRoom.acceptKeyFrame(openPeer.keyFrame()) == RoomSeal::BundleResult::Approved &&
              openRoom.acceptKeyFrame(openPeer2.keyFrame()) ==
                  RoomSeal::BundleResult::Approved,
          "an open room approves every valid bundle");
    const QJsonObject unknownSender = openPeer.sealObject(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("chat")},
                    {QStringLiteral("senderId"), QStringLiteral("node-b")}});
    RoomSeal stranger(QStringLiteral("#mesh-room"), QString(), Policy::Open,
                      QStringLiteral("node-c"), QStringLiteral("node-c"), carol);
    check(stranger.open(unknownSender).status == RoomSeal::Status::Held,
          "a frame from a device whose bundle has not arrived is held");
    stranger.acceptKeyFrame(openPeer.keyFrame());
    const QList<QJsonObject> waiting = stranger.takeReleasedHeld();
    check(waiting.size() == 1 && stranger.open(waiting.first()).status ==
                                     RoomSeal::Status::NotForMe,
          "the held frame is released once the sender's bundle arrives");
}

void testCountersPersist()
{
    clearStoredState();
    auto local = ChatDevice::local();
    check(local->isValid() && local->newestPrekey() != nullptr,
          "the installation's identity and prekey are created on first use");
    const QString keyRoom = QStringLiteral("chat-counter-room");
    const qint64 first = local->nextCounter(keyRoom);
    const qint64 second = local->nextCounter(keyRoom);
    check(first == 1 && second == 2, "counters start at 1 and increase");
    check(QSettings()
                  .value(QStringLiteral("chat/e2ee/ctrV3/") + b64u(keyRoom.toUtf8()))
                  .toLongLong() >= second,
          "a counter is persisted before the frame that uses it is sent");
    check(!QSettings().value(QStringLiteral("chat/e2ee/ed25519IdentityV3")).toString().isEmpty() &&
              !QSettings().value(QStringLiteral("chat/e2ee/prekeysV3")).toString().isEmpty(),
          "the identity seed and prekeys live in the settings store");
}

QJsonObject heldFrame(int index, int bodyBytes)
{
    return QJsonObject{{QStringLiteral("sig"), QStringLiteral("sig-%1").arg(index)},
                       {QStringLiteral("kid"), QStringLiteral("pendingKid000001")},
                       {QStringLiteral("body"), QString(bodyBytes, QLatin1Char('a'))}};
}

qsizetype heldBytes(const QList<QJsonObject> &frames)
{
    qsizetype bytes = 0;
    for (const QJsonObject &frame : frames)
        bytes += QJsonDocument(frame).toJson(QJsonDocument::Compact).size();
    return bytes;
}

void testHeldBudget()
{
    auto memory = ChatDevice::inMemory(randomBytes(32));
    const QString heldScope = QStringLiteral("held-scope");
    const QString heldRoom = QStringLiteral("held-room");
    for (int i = 0; i < 40; ++i)
        memory->hold(heldScope, heldRoom, heldFrame(i, 8 * 1024));
    const QList<QJsonObject> big = memory->held(heldScope, heldRoom);
    check(!big.isEmpty() && big.size() < 40 && heldBytes(big) <= kMaxHeldBytes,
          "held frames from a pending sender stay within the byte budget");
    check(!big.isEmpty() &&
              big.last().value(QStringLiteral("sig")).toString() ==
                  QStringLiteral("sig-39"),
          "the byte budget drops the oldest held frames first");

    const QString smallRoom = QStringLiteral("held-small-room");
    for (int i = 0; i < kMaxHeldFrames + 10; ++i)
        memory->hold(heldScope, smallRoom, heldFrame(i, 16));
    check(memory->held(heldScope, smallRoom).size() == kMaxHeldFrames,
          "small held frames are still capped by count");

    const QString hugeRoom = QStringLiteral("held-huge-room");
    memory->hold(heldScope, hugeRoom, heldFrame(1, 64 * 1024));
    memory->hold(heldScope, hugeRoom, heldFrame(2, int(kMaxHeldBytes) + 1024));
    const QList<QJsonObject> huge = memory->held(heldScope, hugeRoom);
    check(huge.size() == 1 &&
              huge.first().value(QStringLiteral("sig")).toString() ==
                  QStringLiteral("sig-2"),
          "a single oversized frame is kept on its own");

    const QString storedScope = QStringLiteral("held-stored-scope");
    QJsonArray stored;
    for (int i = 0; i < kMaxHeldFrames; ++i)
        stored.append(heldFrame(i, 8 * 1024));
    const QJsonObject root{{QStringLiteral("policy"), QStringLiteral("open")},
                           {QStringLiteral("records"), QJsonArray()},
                           {QStringLiteral("held"), QJsonObject{{heldRoom, stored}}}};
    QSettings().setValue(QStringLiteral("chat/e2ee/trustV3/") +
                             b64u(storedScope.toUtf8()),
                         QJsonDocument(root).toJson(QJsonDocument::Compact));
    const QList<QJsonObject> loaded = ChatDevice::local()->held(storedScope, heldRoom);
    check(!loaded.isEmpty() && heldBytes(loaded) <= kMaxHeldBytes &&
              loaded.last().value(QStringLiteral("sig")).toString() ==
                  QStringLiteral("sig-%1").arg(kMaxHeldFrames - 1),
          "held frames saved before the byte budget are trimmed on load");
}

void testStoredStateLeavesSettingsFile()
{
    clearStoredState();
    const QString trustKey =
        QStringLiteral("chat/e2ee/trustV3/") + b64u(QByteArray("migrated-scope"));
    const QString seenKey =
        QStringLiteral("chat/e2ee/seenV3/") + b64u(QByteArray("migrated-room"));
    const QByteArray trust = QJsonDocument(QJsonObject{
        {QStringLiteral("policy"), QStringLiteral("strict")},
        {QStringLiteral("records"), QJsonArray()},
        {QStringLiteral("held"), QJsonObject{{QStringLiteral("migrated-room"),
                                              QJsonArray{heldFrame(1, 4096)}}}}})
                                 .toJson(QJsonDocument::Compact);
    QSettings().setValue(trustKey, trust);
    QSettings().setValue(seenKey, QByteArray("{}"));
    QSettings().setValue(QStringLiteral("chat/e2ee/ctrV3/kept"), 7);
    check(storedSetting(trustKey).toByteArray() == trust,
          "state still in the settings file is read before it moves");
    check(migrateStoredState(), "the stored state moves out of the settings file");
    check(!QSettings().contains(trustKey) && !QSettings().contains(seenKey),
          "the moved keys are gone from the settings file");
    check(QSettings().value(QStringLiteral("chat/e2ee/ctrV3/kept")).toInt() == 7,
          "counters stay in the settings file");
    QFile trustFile(storedStateKeyFile(trustKey));
    check(storedSetting(trustKey).toByteArray() == trust &&
              trustFile.open(QIODevice::ReadOnly) && trustFile.readAll() == trust &&
              !QSettings(storedStateFile(), QSettings::IniFormat).contains(trustKey),
          "the moved trust state is in its own file, byte for byte");
    trustFile.close();
    check(ChatDevice::local()->held(QStringLiteral("migrated-scope"),
                                    QStringLiteral("migrated-room"))
                  .size() == 1,
          "a held frame survives the move");
    ChatDevice::local()->setPolicy(QStringLiteral("migrated-scope"), Policy::Open);
    ChatDevice::local()->setHeld(QStringLiteral("migrated-scope"),
                                 QStringLiteral("migrated-room"), {});
    check(migrateStoredState(), "moving twice is harmless");
    check(!QSettings().contains(trustKey) &&
              QJsonDocument::fromJson(storedSetting(trustKey).toByteArray())
                      .object()
                      .value(QStringLiteral("policy"))
                      .toString() == QStringLiteral("open"),
          "later writes land in the state file, never back in the settings file");
}

void testStoredStateKeysHaveTheirOwnFiles()
{
    clearStoredState();
    const QString ratchetKey =
        QStringLiteral("chat/e2ee/ratchetV4/") + b64u(QByteArray("own-file-room"));
    const QString trustKey =
        QStringLiteral("chat/e2ee/trustV3/") + b64u(QByteArray("own-file-scope"));
    const QString ratchetPath = storedStateKeyFile(ratchetKey);
    check(!ratchetPath.isEmpty() && ratchetPath != storedStateFile() &&
              QFileInfo(ratchetPath).absolutePath().startsWith(
                  QFileInfo(storedStateFile()).absolutePath()),
          "each stored-state key has its own file beside state.ini");
    QSettings ini(storedStateFile(), QSettings::IniFormat);
    ini.setValue(trustKey, QByteArray("{\"policy\":\"open\"}"));
    ini.setValue(ratchetKey, QByteArray("older"));
    ini.sync();
    check(migrateStoredState(), "state.ini keys move into their own files");
    QSettings reread(storedStateFile(), QSettings::IniFormat);
    check(!reread.contains(trustKey) && !reread.contains(ratchetKey) &&
              storedSetting(ratchetKey).toByteArray() == QByteArray("older"),
          "moved keys leave state.ini and read back from their files");
    check((QFileInfo(ratchetPath).permissions() &
           (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0,
          "a ratchet file is readable by its owner only");
    const qint64 iniSize = QFileInfo(storedStateFile()).size();
    ChatDevice::local()->setPolicy(QStringLiteral("own-file-scope"), Policy::Strict);
    migrateStoredState();
    QFile policyFile(storedStateKeyFile(trustKey));
    check(QFileInfo(storedStateFile()).size() == iniSize &&
              policyFile.open(QIODevice::ReadOnly) &&
              policyFile.readAll().contains("\"strict\"") &&
              QJsonDocument::fromJson(storedSetting(trustKey).toByteArray())
                      .object()
                      .value(QStringLiteral("policy"))
                      .toString() == QStringLiteral("strict"),
          "a trust write rewrites its own file, not state.ini");
    QSettings older(storedStateFile(), QSettings::IniFormat);
    older.setValue(ratchetKey, QByteArray("written-by-an-older-build"));
    older.sync();
    check(migrateStoredState() &&
              storedSetting(ratchetKey).toByteArray() ==
                  QByteArray("written-by-an-older-build"),
          "a key an older build wrote to state.ini replaces the file on the next start");
    clearStoredState();
    check(!QFileInfo::exists(ratchetPath) &&
              !QFileInfo::exists(storedStateKeyFile(trustKey)),
          "clearing the stored state removes the per-key files");
}

} // namespace

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ForkMeshTests"));
    app.setApplicationName(QStringLiteral("chat-v3"));
    QFile file(QStringLiteral(FORKMESH_CHAT_V3_VECTORS));
    if (!file.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "FAIL: cannot read %s\n", FORKMESH_CHAT_V3_VECTORS);
        return 1;
    }
    vectors = QJsonDocument::fromJson(file.readAll()).object();
    room = vectors.value(QStringLiteral("room")).toString();
    scope = vectors.value(QStringLiteral("scope")).toString();
    for (const QJsonValue &value : vectors.value(QStringLiteral("participants")).toArray())
        participants.append(value.toString());
    nowMs = vectors.value(QStringLiteral("now")).toInteger();
    check(vectors.value(QStringLiteral("version")).toInt() == 3, "the vectors are v3");

    testBundles();
    testOpenSequence();
    testForgedFrameKeepsCounter();
    testBlockedSender();
    testWipedPrekey();
    testOtherRoom();
    testKek();
    testSafetyNumbers();
    testLink();
    testRoundTrips();
    testCountersPersist();
    testHeldBudget();
    testStoredStateLeavesSettingsFile();
    testStoredStateKeysHaveTheirOwnFiles();

    if (failures) {
        std::fprintf(stderr, "%d chat v3 check(s) failed.\n", failures);
        return 1;
    }
    std::printf("All chat v3 vector checks passed.\n");
    return 0;
}
