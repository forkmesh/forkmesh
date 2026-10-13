// The desktop client against the real Worker module: the same stdio bridge the
// JavaScript end-to-end test uses (tests/vault_stdio_server.py) answers every
// request, so what is exercised here is the code the window runs plus the code
// the deploy stages. Skipped when python3 or the bridge is not available.
#include "VaultClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

#include <cstdio>

using namespace forkmesh::vault;

void check(bool ok, const char *name);      // test_vault_crypto.cpp
QString fixturesDir();

namespace {

class Bridge
{
public:
    bool start()
    {
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        const QString script = QDir(fixturesDir()).filePath(QStringLiteral("../vault_stdio_server.py"));
        if (python.isEmpty() || !QFileInfo::exists(script))
            return false;
        m_process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_process.start(python, {script});
        return m_process.waitForStarted(10000);
    }
    ~Bridge()
    {
        m_process.closeWriteChannel();
        if (!m_process.waitForFinished(5000))
            m_process.kill();
    }
    QJsonObject send(const QJsonObject &message)
    {
        m_process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        while (!m_process.canReadLine()) {
            if (!m_process.waitForReadyRead(30000))
                return {};
        }
        return QJsonDocument::fromJson(m_process.readLine()).object();
    }
    QJsonObject control(const QString &name, QJsonObject extra = {})
    {
        extra.insert(QStringLiteral("control"), name);
        return send(extra).value(QStringLiteral("data")).toObject();
    }
    VaultClient::Request requestAs(const QString &actor)
    {
        return [this, actor](const QByteArray &method, const QString &path, const QJsonObject &body,
                             VaultClient::Reply reply) {
            const QJsonObject answer = send({{QStringLiteral("actor"), actor},
                                             {QStringLiteral("method"), QString::fromLatin1(method)},
                                             {QStringLiteral("path"), path},
                                             {QStringLiteral("body"), method == "GET" ? QJsonValue() : QJsonValue(body)}});
            const QJsonObject data = answer.value(QStringLiteral("data")).toObject();
            const int status = answer.value(QStringLiteral("status")).toInt();
            const bool ok = status >= 200 && status < 300 && data.value(QStringLiteral("ok")).toBool(true);
            reply(ok, data, ok ? QString() : QStringLiteral("HTTP %1").arg(status));
        };
    }

private:
    QProcess m_process;
};

// The bridge answers synchronously, so every callback has run by the time the
// call returns: these helpers just carry the result out.
QString run(const std::function<void(VaultClient::Done)> &action)
{
    QString code = QStringLiteral("never_called");
    action([&code](const QString &result) { code = result; });
    return code;
}

}  // namespace

void vaultClientFlow()
{
    Bridge bridge;
    if (!bridge.start()) {
        std::printf("client flow skipped: python3 or the stdio bridge is missing\n");
        return;
    }
    VaultClient alice(bridge.requestAs(QStringLiteral("alice"))), bob(bridge.requestAs(QStringLiteral("bob"))),
        carol(bridge.requestAs(QStringLiteral("carol")));
    for (VaultClient *client : {&alice, &bob, &carol})
        client->setNewKdfForTesting(19456, 2);

    check(run([&](auto done) { alice.status(done); }).isEmpty() && !alice.installed(), "a new account has no vault");
    QString recoveryCode;
    alice.setUp(QStringLiteral("alice master password"), [&](const QString &code, const QString &issued) {
        check(code.isEmpty() && issued.size() == 64, "setting up yields a recovery code");
        recoveryCode = issued;
    });
    bob.setUp(QStringLiteral("bob master password"), [](const QString &code, const QString &) {
        check(code.isEmpty(), "bob sets up");
    });

    QString loginId, bankId;
    alice.save({{QStringLiteral("title"), QStringLiteral("Registrar")}, {QStringLiteral("username"), QStringLiteral("ops@acme.test")},
                {QStringLiteral("password"), QStringLiteral("s3cret!")}, {QStringLiteral("url"), QStringLiteral("https://registrar.example/login")}},
               QString(), [&](const QString &code, const QString &id) { check(code.isEmpty(), "a login saves"); loginId = id; });
    alice.save({{QStringLiteral("title"), QStringLiteral("Bank")}, {QStringLiteral("password"), QStringLiteral("only-mine")}},
               QString(), [&](const QString &code, const QString &id) { check(code.isEmpty(), "a second login saves"); bankId = id; });
    alice.save({{QStringLiteral("title"), QStringLiteral("  ")}}, QString(), [](const QString &code, const QString &) {
        check(code == QLatin1String("title_required"), "a login needs a title");
    });
    check(run([&](auto done) { alice.load(done); }).isEmpty() && alice.items().size() == 2
              && alice.find(loginId)->field("password") == QLatin1String("s3cret!"),
          "the owner reads what she saved");

    // A second session of the same account.
    VaultClient elsewhere(bridge.requestAs(QStringLiteral("alice")));
    check(run([&](auto done) { elsewhere.unlock(QStringLiteral("not the password"), done); }) == QLatin1String("wrong_password"),
          "a wrong master password is refused");
    check(run([&](auto done) { elsewhere.unlock(QStringLiteral("alice master password"), done); }).isEmpty()
              && run([&](auto done) { elsewhere.load(done); }).isEmpty() && elsewhere.items().size() == 2,
          "another device unlocks with the password");

    const QByteArray dump = QJsonDocument(bridge.control(QStringLiteral("dump"))).toJson();
    for (const char *secret : {"s3cret!", "only-mine", "Registrar", "registrar.example", "master password"})
        check(!dump.contains(secret), "the database never sees a plaintext");

    // Share with one user: first sight, then pinned.
    alice.previewShare(QStringLiteral("user"), QStringLiteral("bob"), [&](const QString &code, const QList<VaultRecipient> &members) {
        check(code.isEmpty() && members.size() == 1 && members.first().state == QLatin1String("new")
                  && members.first().fingerprint == bob.myFingerprint(),
              "a first share shows bob's own fingerprint as new");
    });
    alice.share(loginId, QStringLiteral("user"), QStringLiteral("carol"), QStringLiteral("read"), {},
                [](const QString &code, const ShareOutcome &) { check(code == QLatin1String("recipient_has_no_vault"), "no vault, no share"); });
    alice.share(loginId, QStringLiteral("user"), QStringLiteral("bob"), QStringLiteral("read"), {},
                [](const QString &code, const ShareOutcome &outcome) { check(code.isEmpty() && outcome.sealedTo == QStringList{QStringLiteral("bob")}, "a share is sealed to bob"); });
    check(run([&](auto done) { bob.load(done); }).isEmpty() && bob.items().size() == 1
              && bob.find(loginId)->field("password") == QLatin1String("s3cret!") && bob.find(loginId)->permission == QLatin1String("read"),
          "bob opens the shared login, view only");
    bob.save(bob.find(loginId)->fields, loginId, [](const QString &code, const QString &) {
        check(code == QLatin1String("read_only"), "a reader cannot edit");
    });
    alice.previewShare(QStringLiteral("user"), QStringLiteral("bob"), [](const QString &, const QList<VaultRecipient> &members) {
        check(members.first().state == QLatin1String("pinned"), "bob's fingerprint is pinned now");
    });

    // Write permission.
    run([&](auto done) { alice.load(done); });
    alice.share(loginId, QStringLiteral("user"), QStringLiteral("bob"), QStringLiteral("write"), {}, [](const QString &code, const ShareOutcome &) { check(code.isEmpty(), "the share becomes editable"); });
    run([&](auto done) { bob.load(done); });
    QJsonObject edited = bob.find(loginId)->fields;
    edited.insert(QStringLiteral("password"), QStringLiteral("rotated-by-bob"));
    bob.save(edited, loginId, [](const QString &code, const QString &) { check(code.isEmpty(), "a writer saves"); });
    check(run([&](auto done) { alice.load(done); }).isEmpty() && alice.find(loginId)->field("password") == QLatin1String("rotated-by-bob")
              && alice.find(loginId)->updatedBy == QLatin1String("bob"),
          "a writer's change reaches the owner");

    // Organization share, a late joiner, and leaving.
    alice.share(bankId, QStringLiteral("organization"), QStringLiteral("acme"), QStringLiteral("read"), {},
                [](const QString &code, const ShareOutcome &outcome) {
                    check(code.isEmpty() && outcome.sealedTo == QStringList{QStringLiteral("bob")} && outcome.withoutVault == QStringList{QStringLiteral("carol")},
                          "an organization share seals to members with a vault");
                });
    alice.share(bankId, QStringLiteral("organization"), QStringLiteral("globex"), QStringLiteral("read"), {},
                [](const QString &code, const ShareOutcome &) { check(code == QLatin1String("organization_not_found"), "not your organization"); });
    carol.setUp(QStringLiteral("carol master password"), [](const QString &code, const QString &) { check(code.isEmpty(), "carol sets up later"); });
    check(run([&](auto done) { carol.load(done); }).isEmpty() && carol.items().isEmpty(), "a late joiner cannot read yet");
    run([&](auto done) { alice.load(done); });
    alice.syncPending([](const QString &code, int sealed) { check(code.isEmpty() && sealed == 1, "the owner's next unlock seals to her"); });
    alice.syncPending([](const QString &code, int sealed) { check(code.isEmpty() && sealed == 0, "and only once"); });
    check(run([&](auto done) { carol.load(done); }).isEmpty() && carol.items().size() == 1
              && carol.find(bankId)->field("password") == QLatin1String("only-mine") && carol.find(bankId)->viaName == QLatin1String("acme"),
          "carol reads it now");
    bridge.control(QStringLiteral("leave"), {{QStringLiteral("organization"), QStringLiteral("acme")}, {QStringLiteral("member"), QStringLiteral("bob")}});
    check(run([&](auto done) { bob.load(done); }).isEmpty() && !bob.find(bankId) && bob.find(loginId), "a former member loses the organization's items");

    // Unsharing rotates the key and keeps the remaining readers.
    check(run([&](auto done) { alice.unshare(loginId, QStringLiteral("user"), QStringLiteral("bob"), done); }).isEmpty(), "unshare and rotate");
    check(run([&](auto done) { alice.load(done); }).isEmpty() && alice.find(loginId)->field("password") == QLatin1String("rotated-by-bob")
              && alice.find(loginId)->shares.isEmpty(),
          "the owner still reads the rotated item");
    check(run([&](auto done) { bob.load(done); }).isEmpty() && bob.items().isEmpty(), "bob no longer sees it");
    alice.share(bankId, QStringLiteral("user"), QStringLiteral("bob"), QStringLiteral("read"), {}, [](const QString &code, const ShareOutcome &) { check(code.isEmpty(), "bob gets the bank login by name"); });
    check(run([&](auto done) { alice.unshare(bankId, QStringLiteral("user"), QStringLiteral("bob"), done); }).isEmpty(), "and loses it again");
    check(run([&](auto done) { carol.load(done); }).isEmpty() && carol.find(bankId) && carol.find(bankId)->field("password") == QLatin1String("only-mine"),
          "carol still reads after a rotation that removed bob");

    // A server that publishes other keys under bob's name is caught.
    Keyset impostorKeys;
    Bundle impostor;
    Failure failure;
    Keyset::generate(&impostorKeys, &failure);
    publicBundle(impostorKeys, QStringLiteral("bob"), &impostor, &failure);
    bridge.control(QStringLiteral("set_bundle"), {{QStringLiteral("member"), QStringLiteral("bob")}, {QStringLiteral("bundle"), impostor.toJson()}});
    alice.previewShare(QStringLiteral("user"), QStringLiteral("bob"), [](const QString &, const QList<VaultRecipient> &members) {
        check(members.first().state == QLatin1String("changed"), "swapped keys show as changed");
    });
    alice.share(loginId, QStringLiteral("user"), QStringLiteral("bob"), QStringLiteral("read"), {},
                [](const QString &code, const ShareOutcome &) { check(code == QLatin1String("recipient_keys_changed"), "and are not sealed to"); });

    // Master password change and recovery.
    check(run([&](auto done) { alice.changePassword(QStringLiteral("alice second password"), done); }).isEmpty(), "the master password changes");
    VaultClient old(bridge.requestAs(QStringLiteral("alice")));
    check(run([&](auto done) { old.unlock(QStringLiteral("alice master password"), done); }) == QLatin1String("wrong_password"), "the old password stops working");
    VaultClient recovered(bridge.requestAs(QStringLiteral("alice")));
    recovered.setNewKdfForTesting(19456, 2);
    recovered.recover(newRecoveryCode(), QStringLiteral("x"), [](const QString &code, const QString &) { check(code == QLatin1String("wrong_recovery_code"), "a wrong recovery code is refused"); });
    recovered.recover(recoveryCode, QStringLiteral("alice third password"), [&](const QString &code, const QString &renewed) {
        check(code.isEmpty() && !renewed.isEmpty() && renewed != recoveryCode, "recovery opens the vault and issues a new code");
    });
    check(run([&](auto done) { recovered.load(done); }).isEmpty() && recovered.items().size() == 2, "the recovered vault lists its items");
    VaultClient spent(bridge.requestAs(QStringLiteral("alice")));
    spent.recover(recoveryCode, QStringLiteral("y"), [](const QString &code, const QString &) { check(code == QLatin1String("wrong_recovery_code"), "a spent recovery code is refused"); });

    recovered.lock();
    check(!recovered.unlocked() && recovered.items().isEmpty(), "a locked client holds nothing");
    recovered.save({{QStringLiteral("title"), QStringLiteral("x")}}, QString(), [](const QString &code, const QString &) { check(code == QLatin1String("vault_locked"), "a locked client cannot save"); });
}
