#include "../src/DesktopCommands.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <openssl/evp.h>

namespace {

namespace dc = forkmesh::desktopcommands;

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        qInfo("PASS: %s", message);
    } else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

QString b64url(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                              QByteArray::OmitTrailingEquals));
}

struct SigningKey {
    EVP_PKEY *key = nullptr;
    QString publicKey;

    SigningKey()
    {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_keygen(ctx, &key);
        EVP_PKEY_CTX_free(ctx);
        QByteArray pub(32, Qt::Uninitialized);
        size_t length = 32;
        EVP_PKEY_get_raw_public_key(
            key, reinterpret_cast<unsigned char *>(pub.data()), &length);
        publicKey = b64url(pub);
    }
    ~SigningKey() { EVP_PKEY_free(key); }
    SigningKey(const SigningKey &) = delete;
    SigningKey &operator=(const SigningKey &) = delete;

    QString sign(const QByteArray &data) const
    {
        QByteArray sig(64, Qt::Uninitialized);
        size_t length = 64;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char *>(sig.data()),
                       &length,
                       reinterpret_cast<const unsigned char *>(data.constData()),
                       size_t(data.size()));
        EVP_MD_CTX_free(ctx);
        return b64url(sig);
    }
};

constexpr qint64 kNow = 1800000000000LL;

QJsonObject row(const SigningKey &key, qint64 id, const QString &owner,
                const QString &repo, const QString &command,
                const QString &target, qint64 ts, const QString &account = {})
{
    const QString tsText = QString::number(ts);
    return QJsonObject{
        {QStringLiteral("id"), double(id)},
        {QStringLiteral("command"), command},
        {QStringLiteral("target"), target},
        {QStringLiteral("ts"), tsText},
        {QStringLiteral("sig"),
         key.sign(dc::canonical(owner, repo, command, target, tsText))},
        {QStringLiteral("signer"), key.publicKey},
        {QStringLiteral("ownerAccount"),
         account.isEmpty() ? owner.toLower() : account},
        {QStringLiteral("canonical"),
         QString::fromUtf8(dc::canonical(owner, repo, command, target, tsText))},
        {QStringLiteral("queuedAt"), double(ts + 10)},
    };
}

dc::Verdict verifyRow(const QJsonObject &object,
                      const QList<dc::TrustedKey> &trusted,
                      const dc::ReplayLedger &ledger = dc::ReplayLedger(),
                      const QString &owner = QStringLiteral("alice"),
                      const QString &repo = QStringLiteral("repo"),
                      qint64 now = kNow)
{
    return dc::verify(dc::parse(object), owner, repo, QStringLiteral("alice"),
                      trusted, ledger, now);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    check(dc::canonical(QStringLiteral("alice"), QStringLiteral("repo"),
                        QStringLiteral("pull.merge"), QStringLiteral("pull:7"),
                        QStringLiteral("1800000000000")) ==
              QByteArray("forkmesh-desktop-command-v1\nalice\nrepo\npull.merge\n"
                         "pull:7\n1800000000000"),
          "the canonical string is the server's forkmesh-desktop-command-v1 layout");

    check(dc::verifyEd25519(
              QStringLiteral("6kpsY-KcUgq-9VB7Ey7F-ZVHdq6-vnuSQh7qaRRG0iw"),
              QStringLiteral("a13FYSRIB4s_7l5xjj4WNISL-txH4EABZbYZa6Un2P6SOaWTUn_"
                             "XL5GGu6-PAOJbkI9OQOrkPIJFX1KfPaf9CQ"),
              dc::canonical(QStringLiteral("alice"), QStringLiteral("repo"),
                            QStringLiteral("pull.merge"), QStringLiteral("pull:7"),
                            QStringLiteral("1800000000000"))),
          "a signature made by Python cryptography over the server canonical verifies");

    check(dc::supportedCommands().size() == 11 &&
              dc::kindFor(QStringLiteral("pull.merge")) == dc::Kind::PullMerge &&
              dc::kindFor(QStringLiteral("release.publish")) == dc::Kind::ReleasePublish &&
              dc::kindFor(QStringLiteral("action.rerun")) == dc::Kind::ActionRerun &&
              dc::kindFor(QStringLiteral("agent.stop")) == dc::Kind::Unknown &&
              dc::kindFor(QStringLiteral("PULL.MERGE")) == dc::Kind::Unknown,
          "the allow-list is exactly the server's eleven commands");

    {
        dc::Command c;
        check(dc::parseTarget(dc::Kind::PullClose, QStringLiteral("pull:42"), &c) &&
                  c.pullNumber == 42,
              "pull targets carry the pull number");
        check(!dc::parseTarget(dc::Kind::PullClose, QStringLiteral("pull:0"), &c) &&
                  !dc::parseTarget(dc::Kind::PullClose, QStringLiteral("pull:1234567890"), &c) &&
                  !dc::parseTarget(dc::Kind::PullClose, QStringLiteral("pull:7\n"), &c) &&
                  !dc::parseTarget(dc::Kind::PullClose, QStringLiteral("run:7"), &c),
              "pull targets reject zero, ten digits, trailing newlines and other shapes");
        check(dc::parseTarget(dc::Kind::ActionRerun, QStringLiteral("run:12"), &c) &&
                  c.runId == QLatin1String("12") &&
                  !dc::parseTarget(dc::Kind::ActionRerun, QStringLiteral("run:-1"), &c) &&
                  !dc::parseTarget(dc::Kind::ActionRerun, QStringLiteral("run:a/b"), &c),
              "run targets follow the server's shape");
        check(dc::parseTarget(dc::Kind::ReleasePublish, QStringLiteral("release:v1.2.0+b1"), &c) &&
                  c.tag == QLatin1String("v1.2.0+b1") &&
                  !dc::parseTarget(dc::Kind::ReleasePublish, QStringLiteral("release:../x"), &c) &&
                  !dc::parseTarget(dc::Kind::ReleasePublish, QStringLiteral("release:"), &c),
              "release targets follow the server's shape");
    }

    check(dc::runsUnattended(dc::Kind::ReleaseCreate) &&
              dc::runsUnattended(dc::Kind::ReleasePublish) &&
              !dc::runsUnattended(dc::Kind::ReleaseDraft) &&
              !dc::runsUnattended(dc::Kind::ReleaseNotes) &&
              !dc::runsUnattended(dc::Kind::PullMerge) &&
              !dc::runsUnattended(dc::Kind::PullDelete) &&
              !dc::runsUnattended(dc::Kind::ActionRerun) &&
              !dc::runsUnattended(dc::Kind::Unknown),
          "only creating and publishing a release may run without the approval prompt");

    SigningKey phone;
    SigningKey stranger;
    const QList<dc::TrustedKey> trusted{
        {phone.publicKey, QStringLiteral("Phone \"Pixel\" (alice)")}};

    {
        const dc::Verdict v = verifyRow(
            row(phone, 1, QStringLiteral("alice"), QStringLiteral("repo"),
                QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow - 1000),
            trusted);
        check(v.status == dc::Status::Ok && v.signerKey == phone.publicKey &&
                  v.signerLabel == QLatin1String("Phone \"Pixel\" (alice)"),
              "a request signed by a known key verifies and names the device");
    }
    {
        const dc::Verdict v = verifyRow(
            row(stranger, 2, QStringLiteral("alice"), QStringLiteral("repo"),
                QStringLiteral("pull.close"), QStringLiteral("pull:7"), kNow),
            trusted);
        check(v.status == dc::Status::UnrecognizedSigner &&
                  v.signerKey == stranger.publicKey &&
                  v.signerLabel.startsWith(QLatin1String("unrecognized device ")),
              "a valid signature by a key this desktop does not know is flagged, not trusted");
    }
    {
        QJsonObject forged = row(phone, 3, QStringLiteral("alice"), QStringLiteral("repo"),
                                 QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        forged.insert(QStringLiteral("target"), QStringLiteral("pull:8"));
        forged.remove(QStringLiteral("canonical"));
        check(verifyRow(forged, trusted).status == dc::Status::BadSignature,
              "changing the target breaks the signature");
        forged = row(phone, 3, QStringLiteral("alice"), QStringLiteral("repo"),
                     QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        forged.insert(QStringLiteral("command"), QStringLiteral("pull.delete"));
        forged.remove(QStringLiteral("canonical"));
        check(verifyRow(forged, trusted).status == dc::Status::BadSignature,
              "changing the command breaks the signature");
        forged = row(phone, 3, QStringLiteral("alice"), QStringLiteral("repo"),
                     QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        forged.insert(QStringLiteral("signer"), phone.publicKey);
        forged.insert(QStringLiteral("sig"), stranger.sign("x"));
        check(verifyRow(forged, trusted).status == dc::Status::BadSignature,
              "a signature over other bytes is refused");
    }
    {
        const QJsonObject other = row(phone, 4, QStringLiteral("alice"), QStringLiteral("other"),
                                      QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        check(verifyRow(other, trusted).status == dc::Status::WrongRepo,
              "a relay canonical naming another repository is refused");
        QJsonObject stripped = other;
        stripped.remove(QStringLiteral("canonical"));
        check(verifyRow(stripped, trusted).status == dc::Status::BadSignature,
              "a request signed for another repository does not verify for this one");
        const QJsonObject cased = row(phone, 5, QStringLiteral("alice"), QStringLiteral("Repo"),
                                      QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        check(verifyRow(cased, trusted, dc::ReplayLedger(), QStringLiteral("alice"),
                        QStringLiteral("repo")).status == dc::Status::BadSignature,
              "only lower-case variants of this desktop's own names are tried");
        const QJsonObject lower = row(phone, 6, QStringLiteral("alice"), QStringLiteral("repo"),
                                      QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        check(verifyRow(lower, trusted, dc::ReplayLedger(), QStringLiteral("Alice"),
                        QStringLiteral("Repo")).status == dc::Status::Ok,
              "a phone that signed the lower-cased names still matches the same repository");
    }
    {
        const QJsonObject bob = row(phone, 7, QStringLiteral("bob"), QStringLiteral("repo"),
                                    QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        check(dc::verify(dc::parse(bob), QStringLiteral("bob"), QStringLiteral("repo"),
                         QStringLiteral("alice"), trusted, dc::ReplayLedger(), kNow)
                      .status == dc::Status::WrongOwner,
              "a repository owned by another account is never acted on");
        const QJsonObject mismatched =
            row(phone, 8, QStringLiteral("alice"), QStringLiteral("repo"),
                QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow,
                QStringLiteral("mallory"));
        check(verifyRow(mismatched, trusted).status == dc::Status::WrongOwner,
              "an ownerAccount other than the repository owner is refused");
    }
    {
        QJsonObject org = row(phone, 20, QStringLiteral("acme"), QStringLiteral("repo"),
                              QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow,
                              QStringLiteral("alice"));
        org.insert(QStringLiteral("repoOwner"), QStringLiteral("acme"));
        const dc::Command parsed = dc::parse(org);
        check(parsed.repoOwner == QLatin1String("acme") &&
                  dc::signedOwner(parsed, QStringLiteral("alice")) == QLatin1String("acme") &&
                  verifyRow(org, trusted).status == dc::Status::Ok,
              "a request signed under the organization name verifies for the member's queue");
        check(dc::promptText(parsed, QStringLiteral("alice"), QStringLiteral("repo"),
                             verifyRow(org, trusted), kNow)
                  .contains(QLatin1String(" in acme/repo.")),
              "the prompt names the organization repository the phone asked about");
        QJsonObject swapped = org;
        swapped.insert(QStringLiteral("repoOwner"), QStringLiteral("alice"));
        swapped.remove(QStringLiteral("canonical"));
        check(verifyRow(swapped, trusted).status == dc::Status::BadSignature,
              "a relay that renames the signed repository owner breaks the signature");
        QJsonObject unsigned_ = row(phone, 21, QStringLiteral("alice"), QStringLiteral("repo"),
                                    QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        unsigned_.insert(QStringLiteral("repoOwner"), QStringLiteral("acme"));
        check(verifyRow(unsigned_, trusted).status == dc::Status::WrongRepo,
              "a relay canonical that disagrees with repoOwner is refused");
    }
    {
        check(verifyRow(row(phone, 9, QStringLiteral("alice"), QStringLiteral("repo"),
                            QStringLiteral("pull.merge"), QStringLiteral("pull:7"),
                            kNow - dc::kRetentionMs - 1),
                        trusted).status == dc::Status::Expired,
              "requests older than a day are refused");
        check(verifyRow(row(phone, 10, QStringLiteral("alice"), QStringLiteral("repo"),
                            QStringLiteral("pull.merge"), QStringLiteral("pull:7"),
                            kNow + dc::kMaxSkewMs + 1),
                        trusted).status == dc::Status::FromFuture,
              "requests stamped more than five minutes ahead are refused");
        check(verifyRow(row(phone, 11, QStringLiteral("alice"), QStringLiteral("repo"),
                            QStringLiteral("pull.merge"), QStringLiteral("pull:7"),
                            kNow + dc::kMaxSkewMs - 1),
                        trusted).status == dc::Status::Ok,
              "clock skew inside five minutes is accepted");
    }
    {
        check(verifyRow(row(phone, 12, QStringLiteral("alice"), QStringLiteral("repo"),
                            QStringLiteral("agent.stop"), QStringLiteral("pull:7"), kNow),
                        trusted).status == dc::Status::UnknownKind,
              "unknown kinds are refused even when correctly signed");
        check(verifyRow(row(phone, 13, QStringLiteral("alice"), QStringLiteral("repo"),
                            QStringLiteral("release.publish"), QStringLiteral("pull:7"), kNow),
                        trusted).status == dc::Status::BadTarget,
              "a target of the wrong shape for its kind is refused");
        QJsonObject noTs = row(phone, 14, QStringLiteral("alice"), QStringLiteral("repo"),
                               QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        noTs.insert(QStringLiteral("ts"), QStringLiteral("12a"));
        check(verifyRow(noTs, trusted).status == dc::Status::Malformed,
              "a non-numeric timestamp is malformed");
        QJsonObject noId = row(phone, 0, QStringLiteral("alice"), QStringLiteral("repo"),
                               QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        check(verifyRow(noId, trusted).status == dc::Status::Malformed,
              "a row without an id is malformed");
    }
    {
        const QJsonObject once = row(phone, 15, QStringLiteral("alice"), QStringLiteral("repo"),
                                     QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow);
        dc::ReplayLedger ledger;
        check(verifyRow(once, trusted, ledger).status == dc::Status::Ok,
              "a fresh request is accepted");
        ledger.record(dc::parse(once), dc::Outcome::Done, kNow);
        check(verifyRow(once, trusted, ledger).status == dc::Status::Replayed,
              "the same signature is never run twice");
        QJsonObject renumbered = once;
        renumbered.insert(QStringLiteral("id"), 99);
        check(verifyRow(renumbered, trusted, ledger).status == dc::Status::Replayed,
              "a replay under a new relay id is still refused");
        const dc::ReplayLedger restored = dc::ReplayLedger::fromJson(ledger.toJson());
        bool found = false;
        check(restored.contains(once.value(QStringLiteral("sig")).toString()) &&
                  restored.containsId(15) &&
                  restored.outcomeFor(once.value(QStringLiteral("sig")).toString(), &found) ==
                      dc::Outcome::Done &&
                  found,
              "the replay ledger survives a save and load");
        dc::ReplayLedger aged = restored;
        aged.prune(kNow + dc::kLedgerKeepMs - 1);
        check(aged.size() == 1, "ledger entries outlive the relay retention window");
        aged.prune(kNow + dc::kLedgerKeepMs + 1);
        check(aged.size() == 0, "ledger entries are dropped once no replay can verify");
        check(!(dc::kLedgerKeepMs < dc::kRetentionMs + dc::kMaxSkewMs),
              "the ledger keeps entries longer than any accepted timestamp age");
    }
    {
        dc::ReplayLedger big;
        for (int i = 0; i < dc::kMaxLedgerEntries + 10; ++i) {
            dc::Command c;
            c.id = i + 1;
            c.sig = QStringLiteral("sig-%1").arg(i);
            big.record(c, dc::Outcome::Declined, kNow + i);
        }
        dc::Command newest;
        newest.sig = QStringLiteral("sig-%1").arg(dc::kMaxLedgerEntries + 9);
        check(big.size() == dc::kMaxLedgerEntries && big.contains(newest.sig) &&
                  !big.contains(QStringLiteral("sig-0")),
              "the ledger is bounded and drops the oldest entries first");
    }
    {
        QJsonArray rows;
        for (int i = 1; i <= dc::kMaxCommandsPerDrain + 5; ++i)
            rows.append(row(phone, i, QStringLiteral("alice"), QStringLiteral("repo"),
                            QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow));
        rows.append(row(phone, 1, QStringLiteral("alice"), QStringLiteral("repo"),
                        QStringLiteral("pull.merge"), QStringLiteral("pull:7"), kNow));
        rows.append(QStringLiteral("junk"));
        const QList<dc::Command> parsed =
            dc::parseDrain(QJsonObject{{QStringLiteral("commands"), rows}});
        check(parsed.size() == dc::kMaxCommandsPerDrain && parsed.first().id == 1 &&
                  parsed.first().pullNumber == 7 && parsed.first().queuedAtMs == kNow + 10,
              "a drain is bounded, de-duplicated by id and parsed");
    }
    {
        dc::Command c = dc::parse(row(phone, 21, QStringLiteral("alice"), QStringLiteral("repo"),
                                      QStringLiteral("pull.merge_delete"),
                                      QStringLiteral("pull:3"), kNow - 3 * 60000));
        dc::Verdict v;
        v.status = dc::Status::Ok;
        v.signerKey = phone.publicKey;
        v.signerLabel = QStringLiteral("Phone \"Pixel\" (alice)");
        const QString text =
            dc::promptText(c, QStringLiteral("alice"), QStringLiteral("repo"), v, kNow);
        check(text.contains(QLatin1String("Phone \"Pixel\" (alice) asks this desktop to:")) &&
                  text.contains(QLatin1String(
                      "Merge pull request #3, then delete it and its branch in alice/repo.")) &&
                  text.contains(QLatin1String("3 minutes ago")) &&
                  text.contains(dc::keyFingerprint(phone.publicKey)) &&
                  text.contains(QLatin1String("cannot be undone")),
              "the prompt names the device, the action, the repository and the age");
        v.status = dc::Status::UnrecognizedSigner;
        check(dc::promptText(c, QStringLiteral("alice"), QStringLiteral("repo"), v, kNow)
                  .contains(QLatin1String("not one of the keys this desktop knows")),
              "an unrecognized key is called out in the prompt");
        c.kind = dc::Kind::ActionRerun;
        c.runId = QStringLiteral("12");
        check(dc::actionLabel(c) == QLatin1String("Rerun Actions run #12"),
              "reruns are labelled with the run id");
        c.kind = dc::Kind::ReleasePublish;
        c.tag = QStringLiteral("v2");
        check(dc::actionLabel(c) == QLatin1String("Publish release v2 to the mirrors"),
              "release publishing is labelled with the tag");
    }
    {
        QHash<qint64, dc::Outcome> outcomes{{1, dc::Outcome::Done}, {2, dc::Outcome::Declined}};
        const QJsonObject body = dc::ackBody({1, 2, 0, 3}, outcomes);
        const QJsonArray ids = body.value(QStringLiteral("ids")).toArray();
        const QJsonArray results = body.value(QStringLiteral("results")).toArray();
        check(ids.size() == 3 && ids.at(0).toInt() == 1 && ids.at(2).toInt() == 3 &&
                  results.at(1).toObject().value(QStringLiteral("outcome")).toString() ==
                      QLatin1String("declined") &&
                  results.at(2).toObject().value(QStringLiteral("outcome")).toString() ==
                      QLatin1String("rejected"),
              "the ack carries the ids the server reads and one outcome per id");
        const dc::Command c = dc::parse(row(phone, 30, QStringLiteral("alice"),
                                            QStringLiteral("repo"), QStringLiteral("pull.close"),
                                            QStringLiteral("pull:3"), kNow));
        const QJsonObject audit = dc::auditRecord(c, QStringLiteral("alice"), QStringLiteral("repo"),
                                                  QStringLiteral("Phone"), dc::Outcome::Done,
                                                  QStringLiteral("closed"), kNow);
        check(audit.value(QStringLiteral("repo")).toString() == QLatin1String("alice/repo") &&
                  audit.value(QStringLiteral("outcome")).toString() == QLatin1String("done") &&
                  !audit.contains(QStringLiteral("sig")) &&
                  audit.value(QStringLiteral("signer")).toString() ==
                      dc::keyFingerprint(phone.publicKey),
              "the audit record keeps the facts and a key fingerprint, never the signature");
    }
    check(dc::ageLabel(30000) == QLatin1String("just now") &&
              dc::ageLabel(60000) == QLatin1String("1 minute ago") &&
              dc::ageLabel(2 * 3600000) == QLatin1String("2 hours ago"),
          "ages read naturally");
    check(dc::keyFingerprint(QStringLiteral("not a key")).isEmpty() &&
              dc::keyFingerprint(phone.publicKey).size() == 19,
          "fingerprints are four groups of four hex digits");

    if (failures) {
        qCritical("%d check(s) failed", failures);
        return 1;
    }
    qInfo("All desktop command checks passed");
    return 0;
}
