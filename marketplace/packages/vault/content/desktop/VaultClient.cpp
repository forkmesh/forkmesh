#include "VaultClient.h"

#include <QJsonValue>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace forkmesh::vault {
namespace {

const QString kApi = QStringLiteral("/api/vault");

QString errorCode(const QJsonObject &payload, const QString &error)
{
    const QString code = payload.value(QStringLiteral("error")).toString();
    return !code.isEmpty() ? code : !error.isEmpty() ? error : QStringLiteral("request_failed");
}

QString encoded(const QString &name)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(name));
}

}  // namespace

VaultClient::VaultClient(Request request, Stretcher stretcher)
    : m_request(std::move(request)), m_stretcher(std::move(stretcher))
{
}

VaultClient::~VaultClient() { lock(); }

void VaultClient::setNewKdfForTesting(int memoryKiB, int passes)
{
    m_newMemoryKiB = memoryKiB;
    m_newPasses = passes;
}

const VaultItem *VaultClient::find(const QString &id) const
{
    for (const VaultItem &item : m_items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

void VaultClient::forgetItemKeys()
{
    for (QByteArray &key : m_itemKeys)
        wipe(key);
    m_itemKeys.clear();
}

void VaultClient::lock()
{
    forgetItemKeys();
    m_keyset.clear();
    m_items.clear();
    m_pins.clear();
}

void VaultClient::call(const QByteArray &method, const QString &path, const QJsonObject &body,
                       std::function<void(const QJsonObject &)> ok, Done failed)
{
    if (!m_request) {
        failed(QStringLiteral("request_failed"));
        return;
    }
    m_request(method, path, body,
              [ok = std::move(ok), failed = std::move(failed)](bool good, const QJsonObject &payload,
                                                                const QString &error) {
                  if (good)
                      ok(payload);
                  else
                      failed(errorCode(payload, error));
              });
}

void VaultClient::stretchWith(const QString &password, const Kdf &kdf, Stretched done)
{
    if (m_stretcher) {
        m_stretcher(password, kdf, std::move(done));
        return;
    }
    QByteArray masterKey;
    Failure failure;
    const bool ok = stretch(password, kdf, &masterKey, &failure);
    done(ok, masterKey, failure.code);
    wipe(masterKey);
}

void VaultClient::status(Done done)
{
    call("GET", kApi, {}, [this, done](const QJsonObject &payload) {
        m_account = payload.value(QStringLiteral("actor")).toString();
        m_installed = payload.value(QStringLiteral("installed")).toBool();
        m_server = m_installed ? payload.value(QStringLiteral("account")).toObject() : QJsonObject();
        done(QString());
    }, done);
}

// ---- trust -----------------------------------------------------------------------

// "new" on first sight, "pinned" when it matches, "changed" when it does not.
QString VaultClient::trustState(const Bundle &bundle, QString *fingerprintText) const
{
    Failure failure;
    if (!fingerprint(bundle, fingerprintText, &failure))
        return QStringLiteral("invalid_keys");
    const QString pinned = m_pins.value(bundle.account);
    return pinned.isEmpty() ? QStringLiteral("new")
           : pinned == *fingerprintText ? QStringLiteral("pinned") : QStringLiteral("changed");
}

void VaultClient::saveTrust(Done done)
{
    QJsonObject pins;
    for (auto it = m_pins.constBegin(); it != m_pins.constEnd(); ++it)
        pins.insert(it.key(), it.value());
    QString blob;
    Failure failure;
    if (!sealPrivate(m_keyset, QStringLiteral("trust"), {{QStringLiteral("pins"), pins}}, &blob, &failure)) {
        done(failure.code);
        return;
    }
    call("PUT", kApi + QStringLiteral("/trust"), {{QStringLiteral("trust"), blob}},
         [this, blob, done](const QJsonObject &) {
             m_server.insert(QStringLiteral("trust"), blob);
             done(QString());
         }, done);
}

// ---- unlocking ---------------------------------------------------------------------

QString VaultClient::adopt(const Keyset &opened)
{
    // The keyset must be the one whose public half the server publishes:
    // otherwise shares sealed to this account could never be opened.
    Bundle mine;
    Failure failure;
    if (!publicBundle(opened, m_account, &mine, &failure))
        return failure.code;
    const QJsonObject published = m_server.value(QStringLiteral("bundle")).toObject();
    if (toBase64Url(mine.x25519) != published.value(QStringLiteral("x25519")).toString()
        || toBase64Url(mine.mlkem768) != published.value(QStringLiteral("mlkem768")).toString()
        || toBase64Url(mine.ed25519) != published.value(QStringLiteral("ed25519")).toString())
        return QStringLiteral("keyset_mismatch");
    m_keyset = opened;
    m_pins.clear();
    const QString trust = m_server.value(QStringLiteral("trust")).toString();
    if (!trust.isEmpty()) {
        QJsonObject stored;
        if (!openPrivate(m_keyset, QStringLiteral("trust"), trust, &stored, &failure)) {
            m_keyset.clear();
            return failure.code;
        }
        const QJsonObject pins = stored.value(QStringLiteral("pins")).toObject();
        for (auto it = pins.constBegin(); it != pins.constEnd(); ++it)
            m_pins.insert(it.key(), it.value().toString());
    }
    return QString();
}

void VaultClient::setUp(const QString &masterPassword,
                        std::function<void(const QString &, const QString &)> done)
{
    const auto failed = [done](const QString &code) { done(code, QString()); };
    status([this, masterPassword, done, failed](const QString &code) {
        if (!code.isEmpty()) { failed(code); return; }
        if (m_installed) { failed(QStringLiteral("vault_exists")); return; }
        Kdf kdf = Kdf::fresh();
        kdf.memoryKiB = m_newMemoryKiB;
        kdf.passes = m_newPasses;
        stretchWith(masterPassword, kdf, [this, kdf, done, failed](bool ok, const QByteArray &masterKey,
                                                                    const QString &why) {
            if (!ok) { failed(why); return; }
            Failure failure;
            Keyset fresh;
            Bundle bundle;
            QByteArray recoveryKey;
            QString wrappedKeyset, wrappedRecovery;
            const QString recoveryCode = newRecoveryCode();
            if (!Keyset::generate(&fresh, &failure) || !recoverySecret(recoveryCode, &recoveryKey, &failure)
                || !wrapKeyset(masterKey, QStringLiteral("master"), m_account, fresh, &wrappedKeyset, &failure)
                || !wrapKeyset(recoveryKey, QStringLiteral("recovery"), m_account, fresh, &wrappedRecovery, &failure)
                || !publicBundle(fresh, m_account, &bundle, &failure)) {
                failed(failure.code);
                return;
            }
            wipe(recoveryKey);
            call("POST", kApi + QStringLiteral("/install"),
                 {{QStringLiteral("kdf"), kdf.toJson()},
                  {QStringLiteral("wrappedKeyset"), wrappedKeyset},
                  {QStringLiteral("wrappedRecovery"), wrappedRecovery},
                  {QStringLiteral("bundle"), bundle.toJson()}},
                 [this, fresh, recoveryCode, done](const QJsonObject &payload) {
                     m_installed = true;
                     m_server = payload.value(QStringLiteral("account")).toObject();
                     m_keyset = fresh;
                     m_pins.clear();
                     m_items.clear();
                     done(QString(), recoveryCode);
                 }, failed);
        });
    });
}

void VaultClient::unlock(const QString &masterPassword, Done done)
{
    status([this, masterPassword, done](const QString &code) {
        if (!code.isEmpty()) { done(code); return; }
        if (!m_installed) { done(QStringLiteral("vault_not_set_up")); return; }
        Kdf kdf;
        Failure failure;
        if (!Kdf::fromJson(m_server.value(QStringLiteral("kdf")).toObject(), &kdf, &failure)) {
            done(failure.code);
            return;
        }
        stretchWith(masterPassword, kdf, [this, done](bool ok, const QByteArray &masterKey, const QString &why) {
            if (!ok) { done(why); return; }
            Keyset opened;
            Failure inner;
            if (!unwrapKeyset(masterKey, QStringLiteral("master"), m_account,
                              m_server.value(QStringLiteral("wrappedKeyset")).toString(), &opened, &inner)) {
                done(inner.code);
                return;
            }
            done(adopt(opened));
        });
    });
}

void VaultClient::rewrap(const QString *masterPassword, bool newRecovery,
                         std::function<void(const QString &, const QString &)> done)
{
    if (!unlocked()) { done(QStringLiteral("vault_locked"), QString()); return; }
    const auto failed = [done](const QString &code) { done(code, QString()); };
    const auto send = [this, newRecovery, done, failed](QJsonObject body) {
        QString recoveryCode;
        if (newRecovery) {
            recoveryCode = newRecoveryCode();
            QByteArray recoveryKey;
            QString wrapped;
            Failure failure;
            if (!recoverySecret(recoveryCode, &recoveryKey, &failure)
                || !wrapKeyset(recoveryKey, QStringLiteral("recovery"), m_account, m_keyset, &wrapped, &failure)) {
                failed(failure.code);
                return;
            }
            wipe(recoveryKey);
            body.insert(QStringLiteral("wrappedRecovery"), wrapped);
        }
        body.insert(QStringLiteral("revision"), m_server.value(QStringLiteral("revision")).toInt(1));
        call("PUT", kApi + QStringLiteral("/keys"), body, [this, recoveryCode, done](const QJsonObject &payload) {
            m_server = payload.value(QStringLiteral("account")).toObject();
            done(QString(), recoveryCode);
        }, failed);
    };
    if (!masterPassword) {
        send({});
        return;
    }
    Kdf kdf = Kdf::fresh();
    kdf.memoryKiB = m_newMemoryKiB;
    kdf.passes = m_newPasses;
    stretchWith(*masterPassword, kdf, [this, kdf, send, failed](bool ok, const QByteArray &masterKey, const QString &why) {
        if (!ok) { failed(why); return; }
        QString wrapped;
        Failure failure;
        if (!wrapKeyset(masterKey, QStringLiteral("master"), m_account, m_keyset, &wrapped, &failure)) {
            failed(failure.code);
            return;
        }
        send({{QStringLiteral("kdf"), kdf.toJson()}, {QStringLiteral("wrappedKeyset"), wrapped}});
    });
}

void VaultClient::changePassword(const QString &masterPassword, Done done)
{
    rewrap(&masterPassword, false, [done](const QString &code, const QString &) { done(code); });
}

void VaultClient::renewRecoveryCode(std::function<void(const QString &, const QString &)> done)
{
    rewrap(nullptr, true, std::move(done));
}

// A used recovery code is spent: recovering sets a new master password and a
// new code in one step.
void VaultClient::recover(const QString &recoveryCode, const QString &newMasterPassword,
                          std::function<void(const QString &, const QString &)> done)
{
    const auto failed = [done](const QString &code) { done(code, QString()); };
    status([this, recoveryCode, newMasterPassword, done, failed](const QString &code) {
        if (!code.isEmpty()) { failed(code); return; }
        if (!m_installed) { failed(QStringLiteral("vault_not_set_up")); return; }
        QByteArray recoveryKey;
        Failure failure;
        if (!recoverySecret(recoveryCode, &recoveryKey, &failure)) { failed(failure.code); return; }
        call("GET", kApi + QStringLiteral("/recovery"), {},
             [this, recoveryKey, newMasterPassword, done, failed](const QJsonObject &payload) {
                 Keyset opened;
                 Failure inner;
                 if (!unwrapKeyset(recoveryKey, QStringLiteral("recovery"), m_account,
                                   payload.value(QStringLiteral("wrappedRecovery")).toString(), &opened, &inner)) {
                     failed(inner.code == QLatin1String("wrong_password") ? QStringLiteral("wrong_recovery_code")
                                                                          : inner.code);
                     return;
                 }
                 const QString adopted = adopt(opened);
                 if (!adopted.isEmpty()) { failed(adopted); return; }
                 rewrap(&newMasterPassword, true, done);
             }, failed);
    });
}

// ---- reading -------------------------------------------------------------------------

void VaultClient::load(Done done)
{
    if (!unlocked()) { done(QStringLiteral("vault_locked")); return; }
    call("GET", kApi + QStringLiteral("/items"), {}, [this, done](const QJsonObject &payload) {
        QList<VaultItem> next;
        forgetItemKeys();
        const auto common = [](const QJsonObject &row) {
            VaultItem item;
            item.id = row.value(QStringLiteral("id")).toString();
            item.owner = row.value(QStringLiteral("owner")).toString();
            item.permission = row.value(QStringLiteral("permission")).toString();
            item.revision = row.value(QStringLiteral("revision")).toInt(1);
            item.updatedAt = qint64(row.value(QStringLiteral("updatedAt")).toDouble());
            item.updatedBy = row.value(QStringLiteral("updatedBy")).toString();
            return item;
        };
        for (const QJsonValue &value : payload.value(QStringLiteral("items")).toArray()) {
            const QJsonObject row = value.toObject();
            VaultItem item = common(row);
            item.owned = true;
            for (const QJsonValue &shared : row.value(QStringLiteral("shares")).toArray()) {
                const QJsonObject share = shared.toObject();
                item.shares.append({share.value(QStringLiteral("kind")).toString(),
                                    share.value(QStringLiteral("name")).toString(),
                                    share.value(QStringLiteral("permission")).toString()});
            }
            QByteArray itemKey;
            Failure failure;
            if (unwrapItemKey(m_keyset, item.id, row.value(QStringLiteral("wrappedKey")).toString(), &itemKey, &failure)
                && openItem(itemKey, item.id, row.value(QStringLiteral("body")).toString(), &item.fields, &failure))
                m_itemKeys.insert(item.id, itemKey);
            else
                item.error = failure.code;
            next.append(item);
        }
        bool pinsChanged = false;
        const QJsonObject bundles = payload.value(QStringLiteral("bundles")).toObject();
        for (const QJsonValue &value : payload.value(QStringLiteral("shared")).toArray()) {
            const QJsonObject row = value.toObject();
            VaultItem item = common(row);
            const QJsonObject via = row.value(QStringLiteral("via")).toObject();
            item.viaKind = via.value(QStringLiteral("kind")).toString();
            item.viaName = via.value(QStringLiteral("name")).toString();
            Bundle sender;
            Failure failure;
            QByteArray itemKey;
            QString print;
            if (!bundles.contains(item.owner)) {
                item.error = QStringLiteral("sender_keys_missing");
            } else if (!Bundle::fromJson(bundles.value(item.owner).toObject(), item.owner, &sender, &failure)) {
                item.error = failure.code;
            } else {
                const QString state = trustState(sender, &print);
                if (state == QLatin1String("changed")) {
                    item.error = QStringLiteral("sender_keys_changed");
                } else if (openGrant(m_keyset, m_account, sender, item.id, row.value(QStringLiteral("grant")).toObject(),
                                     item.permission, &itemKey, &failure)
                           && openItem(itemKey, item.id, row.value(QStringLiteral("body")).toString(), &item.fields, &failure)) {
                    // Pinned only after the share's signature held under these keys.
                    if (state == QLatin1String("new")) {
                        m_pins.insert(sender.account, print);
                        pinsChanged = true;
                    }
                    m_itemKeys.insert(item.id, itemKey);
                } else {
                    item.error = failure.code;
                }
            }
            next.append(item);
        }
        std::sort(next.begin(), next.end(), [](const VaultItem &a, const VaultItem &b) {
            if (a.readable() != b.readable())
                return a.readable();
            return a.field("title").compare(b.field("title"), Qt::CaseInsensitive) < 0;
        });
        m_items = next;
        if (pinsChanged)
            saveTrust(done);
        else
            done(QString());
    }, done);
}

// ---- writing -------------------------------------------------------------------------

QJsonObject VaultClient::cleanFields(const QJsonObject &fields, QString *code)
{
    static const QList<std::pair<const char *, int>> limits{
        {"title", 200}, {"username", 320}, {"password", 4096}, {"url", 2048}, {"notes", 8000}};
    QJsonObject out;
    for (const auto &[name, limit] : limits)
        out.insert(QLatin1String(name), fields.value(QLatin1String(name)).toString().left(limit));
    out.insert(QStringLiteral("title"), out.value(QStringLiteral("title")).toString().trimmed());
    out.insert(QStringLiteral("url"), out.value(QStringLiteral("url")).toString().trimmed());
    if (out.value(QStringLiteral("title")).toString().isEmpty() && code)
        *code = QStringLiteral("title_required");
    return out;
}

QList<QStringList> VaultClient::parseCsv(const QString &text)
{
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false, atFieldStart = true;
    const QString source = text.startsWith(QChar(0xFEFF)) ? text.mid(1) : text;
    const auto endField = [&] { row.append(field); field.clear(); atFieldStart = true; };
    const auto endRow = [&] {
        endField();
        if (!(row.size() == 1 && row.first().isEmpty()))
            rows.append(row);
        row.clear();
    };
    for (qsizetype i = 0; i < source.size(); ++i) {
        const QChar c = source.at(i);
        const QChar next = i + 1 < source.size() ? source.at(i + 1) : QChar();
        if (quoted) {
            if (c != QLatin1Char('"')) {
                field.append(c);
            } else if (next == QLatin1Char('"')) {
                field.append(c);
                ++i;
            } else {
                quoted = false;
            }
        } else if (c == QLatin1Char('"') && atFieldStart) {
            quoted = true;
            atFieldStart = false;
        } else if (c == QLatin1Char(',')) {
            endField();
        } else if (c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
            if (c == QLatin1Char('\r') && next == QLatin1Char('\n'))
                ++i;
            endRow();
        } else {
            field.append(c);
            atFieldStart = false;
        }
    }
    if (!field.isEmpty() || !row.isEmpty() || quoted)
        endRow();
    return rows;
}

bool VaultClient::readImportCsv(const QByteArray &bytes, ImportFile *file, QString *code)
{
    static const QList<std::pair<QString, QStringList>> columns{
        {QStringLiteral("title"), {QStringLiteral("title"), QStringLiteral("name"), QStringLiteral("account"), QStringLiteral("item name"),
                                   QStringLiteral("login name"), QStringLiteral("display name")}},
        {QStringLiteral("url"), {QStringLiteral("url"), QStringLiteral("login_uri"), QStringLiteral("uri"), QStringLiteral("website"),
                                 QStringLiteral("web site"), QStringLiteral("web address"), QStringLiteral("site"), QStringLiteral("hostname"),
                                 QStringLiteral("login url"), QStringLiteral("urls")}},
        {QStringLiteral("username"), {QStringLiteral("username"), QStringLiteral("login_username"), QStringLiteral("user name"),
                                      QStringLiteral("user"), QStringLiteral("login"), QStringLiteral("email"),
                                      QStringLiteral("email address"), QStringLiteral("user id")}},
        {QStringLiteral("password"), {QStringLiteral("password"), QStringLiteral("login_password"), QStringLiteral("pass")}},
        {QStringLiteral("notes"), {QStringLiteral("notes"), QStringLiteral("note"), QStringLiteral("comments"), QStringLiteral("comment"),
                                   QStringLiteral("extra")}},
        {QStringLiteral("otpauth"), {QStringLiteral("otpauth"), QStringLiteral("login_totp"), QStringLiteral("totp"), QStringLiteral("otp"),
                                     QStringLiteral("one-time password"), QStringLiteral("otp auth"), QStringLiteral("otp url")}},
    };
    const auto fail = [code](const char *why) {
        if (code)
            *code = QLatin1String(why);
        return false;
    };
    if (bytes.size() > kImportMaxBytes)
        return fail("csv_too_large");
    QString text = QString::fromUtf8(bytes);
    if (text.contains(QChar::ReplacementCharacter) && !bytes.contains("\xEF\xBF\xBD"))
        text = QString::fromLatin1(bytes);
    const QList<QStringList> rows = parseCsv(text);
    if (rows.isEmpty())
        return fail("csv_empty");
    QStringList names;
    for (const QString &name : rows.first())
        names.append(name.trimmed().toLower());
    QHash<QString, qsizetype> index;
    for (const auto &[field, aliases] : columns) {
        for (const QString &alias : aliases) {
            if (names.contains(alias)) {
                index.insert(field, names.indexOf(alias));
                break;
            }
        }
    }
    if (!index.contains(QStringLiteral("password")))
        return fail("csv_unknown_columns");
    ImportFile out;
    for (qsizetype r = 1; r < rows.size(); ++r) {
        const QStringList &row = rows.at(r);
        const auto value = [&](const char *field) {
            const qsizetype at = index.value(QLatin1String(field), -1);
            return at >= 0 && at < row.size() ? row.at(at) : QString();
        };
        const ImportLogin login{value("title").trimmed(), value("url").trimmed(), value("username"),
                                value("password"), value("notes"), value("otpauth").trimmed()};
        if (login.password.isEmpty() && login.username.trimmed().isEmpty()) {
            if (std::any_of(row.cbegin(), row.cend(), [](const QString &cell) { return !cell.trimmed().isEmpty(); }))
                ++out.skippedRows;
            continue;
        }
        out.logins.append(login);
    }
    if (out.logins.isEmpty())
        return fail("csv_no_logins");
    const auto has = [&names](std::initializer_list<const char *> wanted) {
        return std::all_of(wanted.begin(), wanted.end(), [&names](const char *name) { return names.contains(QLatin1String(name)); });
    };
    if (has({"login_uri"}) || has({"login_password"}))
        out.source = QStringLiteral("bitwarden");
    else if (has({"httprealm"}) || has({"formactionorigin"}) || has({"guid"}))
        out.source = QStringLiteral("firefox");
    else if (has({"archived"}) || has({"one-time password"}))
        out.source = QStringLiteral("onePassword");
    else if (has({"otpauth"}))
        out.source = QStringLiteral("applePasswords");
    else if (has({"name", "url", "username", "password", "note"}))
        out.source = QStringLiteral("chrome");
    else if (has({"title", "url", "username", "password", "notes"}))
        out.source = QStringLiteral("duckDuckGo");
    else
        out.source = QStringLiteral("other");
    static const QHash<QString, QString> labels{
        {QStringLiteral("applePasswords"), QStringLiteral("Apple Passwords")}, {QStringLiteral("duckDuckGo"), QStringLiteral("DuckDuckGo")},
        {QStringLiteral("chrome"), QStringLiteral("Chrome")}, {QStringLiteral("firefox"), QStringLiteral("Firefox")},
        {QStringLiteral("bitwarden"), QStringLiteral("Bitwarden")}, {QStringLiteral("onePassword"), QStringLiteral("1Password")},
        {QStringLiteral("other"), QStringLiteral("a password manager")}};
    out.sourceLabel = labels.value(out.source);
    if (file)
        *file = out;
    return true;
}

namespace {

QString importTitle(const QString &title, const QString &url, const QString &username)
{
    if (!title.trimmed().isEmpty())
        return title.trimmed();
    const QString host = loginHost(url);
    if (!host.isEmpty())
        return host;
    return username.trimmed().isEmpty() ? QStringLiteral("Imported login") : username.trimmed();
}

QString importIdentity(const QString &title, const QString &url, const QString &username, const QString &password)
{
    const QString host = loginHost(url);
    const QString place = host.isEmpty() ? importTitle(title, url, username).toLower() : host;
    return place + QChar(0) + username.trimmed().toLower() + QChar(0) + password;
}

}  // namespace

QJsonObject VaultClient::importFields(const ImportLogin &login)
{
    QString notes = login.notes.trimmed();
    const QString code = login.otpauth.trimmed();
    if (!code.isEmpty() && !notes.contains(code))
        notes = notes.isEmpty() ? code : notes + QStringLiteral("\n\n") + code;
    return {{QStringLiteral("title"), importTitle(login.title, login.url, login.username)},
            {QStringLiteral("url"), login.url.trimmed()},
            {QStringLiteral("username"), login.username},
            {QStringLiteral("password"), login.password},
            {QStringLiteral("notes"), notes}};
}

ImportPlan VaultClient::planImport(const QList<ImportLogin> &logins, const QList<VaultItem> &items)
{
    QSet<QString> seen;
    for (const VaultItem &item : items) {
        if (item.readable())
            seen.insert(importIdentity(item.field("title"), item.field("url"), item.field("username"), item.field("password")));
    }
    ImportPlan plan;
    for (const ImportLogin &login : logins) {
        const QString key = importIdentity(login.title, login.url, login.username, login.password);
        if (seen.contains(key)) {
            ++plan.duplicates;
            continue;
        }
        seen.insert(key);
        plan.fresh.append(login);
    }
    return plan;
}

void VaultClient::save(const QJsonObject &fields, const QString &id,
                       std::function<void(const QString &, const QString &)> done)
{
    const auto failed = [done](const QString &code) { done(code, QString()); };
    if (!unlocked()) { failed(QStringLiteral("vault_locked")); return; }
    QString code;
    const QJsonObject clean = cleanFields(fields, &code);
    if (!code.isEmpty()) { failed(code); return; }
    Failure failure;
    QString body, wrapped;
    if (id.isEmpty()) {
        const QString itemId = newItemId();
        const QByteArray itemKey = randomBytes(32);
        if (!sealItem(itemKey, itemId, clean, &body, &failure)
            || !wrapItemKey(m_keyset, itemId, itemKey, &wrapped, &failure)) {
            failed(failure.code);
            return;
        }
        call("POST", kApi + QStringLiteral("/items"),
             {{QStringLiteral("id"), itemId}, {QStringLiteral("body"), body}, {QStringLiteral("wrappedKey"), wrapped}},
             [this, itemId, itemKey, done](const QJsonObject &) {
                 m_itemKeys.insert(itemId, itemKey);
                 done(QString(), itemId);
             }, failed);
        return;
    }
    const VaultItem *current = find(id);
    if (!current || !m_itemKeys.contains(id)) { failed(QStringLiteral("item_not_found")); return; }
    if (current->permission == QLatin1String("read")) { failed(QStringLiteral("read_only")); return; }
    if (!sealItem(m_itemKeys.value(id), id, clean, &body, &failure)) { failed(failure.code); return; }
    call("PUT", kApi + QStringLiteral("/items/") + id,
         {{QStringLiteral("revision"), current->revision}, {QStringLiteral("body"), body}},
         [id, done](const QJsonObject &) { done(QString(), id); }, failed);
}

void VaultClient::remove(const QString &id, Done done)
{
    if (!unlocked()) { done(QStringLiteral("vault_locked")); return; }
    call("DELETE", kApi + QStringLiteral("/items/") + id, {}, [this, id, done](const QJsonObject &) {
        if (m_itemKeys.contains(id)) {
            wipe(m_itemKeys[id]);
            m_itemKeys.remove(id);
        }
        done(QString());
    }, done);
}

// ---- sharing ---------------------------------------------------------------------------

QList<VaultRecipient> VaultClient::recipientsFrom(const QJsonArray &members) const
{
    QList<VaultRecipient> out;
    for (const QJsonValue &value : members) {
        const QJsonObject member = value.toObject();
        VaultRecipient recipient;
        recipient.name = member.value(QStringLiteral("name")).toString();
        const QJsonObject bundle = member.value(QStringLiteral("bundle")).toObject();
        Failure failure;
        if (bundle.isEmpty())
            recipient.state = QStringLiteral("no_vault");
        else if (!Bundle::fromJson(bundle, recipient.name, &recipient.bundle, &failure))
            recipient.state = QStringLiteral("invalid_keys");
        else
            recipient.state = trustState(recipient.bundle, &recipient.fingerprint);
        out.append(recipient);
    }
    return out;
}

void VaultClient::previewShare(const QString &kind, const QString &name,
                               std::function<void(const QString &, const QList<VaultRecipient> &)> done)
{
    const auto failed = [done](const QString &code) { done(code, {}); };
    if (!unlocked()) { failed(QStringLiteral("vault_locked")); return; }
    const QString clean = name.trimmed().toLower();
    if (kind == QLatin1String("user")) {
        call("GET", kApi + QStringLiteral("/users/") + encoded(clean), {}, [this, done](const QJsonObject &payload) {
            done(QString(), recipientsFrom({QJsonObject{{QStringLiteral("name"), payload.value(QStringLiteral("name"))},
                                                         {QStringLiteral("bundle"), payload.value(QStringLiteral("bundle"))}}}));
        }, failed);
        return;
    }
    call("GET", kApi + QStringLiteral("/organizations/") + encoded(clean) + QStringLiteral("/members"), {},
         [this, done](const QJsonObject &payload) {
             done(QString(), recipientsFrom(payload.value(QStringLiteral("members")).toArray()));
         }, failed);
}

// Grants for everyone in `members` who may be sealed to. `accepted` names
// people whose changed fingerprint the owner has just re-verified.
VaultClient::Sealed VaultClient::grantsFor(const QString &id, const QByteArray &itemKey, const QString &kind,
                                           const QString &name, const QString &permission,
                                           const QList<VaultRecipient> &members, const QStringList &accepted)
{
    Sealed sealed;
    const QJsonObject via{{QStringLiteral("kind"), kind}, {QStringLiteral("name"), name}};
    for (const VaultRecipient &member : members) {
        if (member.name == m_account || member.state == QLatin1String("no_vault"))
            continue;
        if (member.state == QLatin1String("invalid_keys")
            || (member.state == QLatin1String("changed") && !accepted.contains(member.name))) {
            sealed.skipped.append(member.name);
            continue;
        }
        QJsonObject grant;
        Failure failure;
        if (!sealGrant(m_keyset, m_account, member.bundle, id, itemKey, permission, &grant, &failure)) {
            sealed.skipped.append(member.name);
            continue;
        }
        sealed.grants.append(QJsonObject{{QStringLiteral("recipient"), member.name},
                                         {QStringLiteral("via"), via},
                                         {QStringLiteral("grant"), grant}});
        m_pins.insert(member.name, member.fingerprint);
    }
    return sealed;
}

void VaultClient::share(const QString &id, const QString &kind, const QString &name, const QString &permission,
                        const QStringList &accepted, std::function<void(const QString &, const ShareOutcome &)> done)
{
    const auto failed = [done](const QString &code) { done(code, {}); };
    const VaultItem *current = find(id);
    if (!unlocked()) { failed(QStringLiteral("vault_locked")); return; }
    if (!current || !current->owned || !m_itemKeys.contains(id)) { failed(QStringLiteral("owner_required")); return; }
    previewShare(kind, name, [=, this](const QString &code, const QList<VaultRecipient> &members) {
        if (!code.isEmpty()) { failed(code); return; }
        QString target = name.trimmed().toLower();
        if (kind == QLatin1String("user")) {
            if (members.isEmpty() || members.first().state == QLatin1String("no_vault")) {
                failed(QStringLiteral("recipient_has_no_vault"));
                return;
            }
            if (members.first().state == QLatin1String("invalid_keys")) { failed(QStringLiteral("invalid_public_keys")); return; }
            if (members.first().state == QLatin1String("changed") && !accepted.contains(members.first().name)) {
                failed(QStringLiteral("recipient_keys_changed"));
                return;
            }
            target = members.first().name;
        }
        const Sealed sealed = grantsFor(id, m_itemKeys.value(id), kind, target, permission, members, accepted);
        ShareOutcome outcome;
        outcome.skipped = sealed.skipped;
        for (const QJsonValue &grant : sealed.grants)
            outcome.sealedTo.append(grant.toObject().value(QStringLiteral("recipient")).toString());
        for (const VaultRecipient &member : members) {
            if (member.state == QLatin1String("no_vault"))
                outcome.withoutVault.append(member.name);
        }
        call("POST", kApi + QStringLiteral("/items/") + id + QStringLiteral("/shares"),
             {{QStringLiteral("target"), QJsonObject{{QStringLiteral("kind"), kind}, {QStringLiteral("name"), target}}},
              {QStringLiteral("permission"), permission},
              {QStringLiteral("grants"), sealed.grants}},
             [this, outcome, done](const QJsonObject &) {
                 saveTrust([outcome, done](const QString &why) { done(why, outcome); });
             }, failed);
    });
}

// Re-encrypt an item under a fresh key and seal that key to everyone its
// remaining shares cover. Whoever was removed keeps only a dead key.
void VaultClient::rotate(const QString &id, Done done)
{
    const VaultItem *current = find(id);
    if (!current || !current->owned || !current->readable()) { done(QStringLiteral("owner_required")); return; }
    call("GET", kApi + QStringLiteral("/items"), {}, [this, id, done](const QJsonObject &listing) {
        QJsonObject fresh;
        for (const QJsonValue &value : listing.value(QStringLiteral("items")).toArray()) {
            if (value.toObject().value(QStringLiteral("id")).toString() == id)
                fresh = value.toObject();
        }
        call("GET", kApi + QStringLiteral("/items/") + id + QStringLiteral("/recipients"), {},
             [this, id, fresh, done](const QJsonObject &payload) {
                 const VaultItem *item = find(id);
                 if (!item) { done(QStringLiteral("item_not_found")); return; }
                 // Someone with write access may have saved since this list
                 // was loaded: re-encrypt the server's current body.
                 QJsonObject fields = item->fields;
                 Failure failure;
                 if (!fresh.isEmpty() && m_itemKeys.contains(id)) {
                     QJsonObject current;
                     if (openItem(m_itemKeys.value(id), id, fresh.value(QStringLiteral("body")).toString(), &current, &failure))
                         fields = current;
                 }
                 const QByteArray itemKey = randomBytes(32);
                 QJsonArray grants;
                 for (const QJsonValue &value : payload.value(QStringLiteral("recipients")).toArray()) {
                     const QJsonObject group = value.toObject();
                     const Sealed sealed = grantsFor(id, itemKey, group.value(QStringLiteral("kind")).toString(),
                                                     group.value(QStringLiteral("name")).toString(),
                                                     group.value(QStringLiteral("permission")).toString(),
                                                     recipientsFrom(group.value(QStringLiteral("members")).toArray()), {});
                     for (const QJsonValue &grant : sealed.grants)
                         grants.append(grant);
                 }
                 QString body, wrapped;
                 if (!sealItem(itemKey, id, fields, &body, &failure) || !wrapItemKey(m_keyset, id, itemKey, &wrapped, &failure)) {
                     done(failure.code);
                     return;
                 }
                 call("PUT", kApi + QStringLiteral("/items/") + id,
                      {{QStringLiteral("revision"), fresh.isEmpty() ? item->revision : fresh.value(QStringLiteral("revision")).toInt(1)},
                       {QStringLiteral("body"), body}, {QStringLiteral("wrappedKey"), wrapped}, {QStringLiteral("grants"), grants}},
                      [this, id, itemKey, done](const QJsonObject &) {
                          if (m_itemKeys.contains(id))
                              wipe(m_itemKeys[id]);
                          m_itemKeys.insert(id, itemKey);
                          done(QString());
                      }, done);
             }, done);
    }, done);
}

void VaultClient::unshare(const QString &id, const QString &kind, const QString &name, Done done)
{
    if (!unlocked()) { done(QStringLiteral("vault_locked")); return; }
    call("POST", kApi + QStringLiteral("/items/") + id + QStringLiteral("/unshare"),
         {{QStringLiteral("kind"), kind}, {QStringLiteral("name"), name}},
         [this, id, done](const QJsonObject &) { rotate(id, done); }, done);
}

// Members who joined an organization, or made their vault, after an item was
// shared with it. Only a holder of the item key can seal to them, so the
// owner's client catches up each time it unlocks.
void VaultClient::syncPending(std::function<void(const QString &, int)> done)
{
    if (!unlocked()) { done(QStringLiteral("vault_locked"), 0); return; }
    call("GET", kApi + QStringLiteral("/pending"), {}, [this, done](const QJsonObject &payload) {
        struct Work { QJsonArray entries; int at = 0; int sealed = 0; };
        auto work = std::make_shared<Work>();
        work->entries = payload.value(QStringLiteral("pending")).toArray();
        // Each pending network reply holds the step alive; the step itself
        // only holds a weak reference, so nothing leaks when the chain ends.
        auto step = std::make_shared<std::function<void()>>();
        *step = [this, work, weak = std::weak_ptr<std::function<void()>>(step), done] {
            const auto self = weak.lock();
            while (self && work->at < work->entries.size()) {
                const QJsonObject entry = work->entries.at(work->at++).toObject();
                const QString id = entry.value(QStringLiteral("itemId")).toString();
                if (!m_itemKeys.contains(id))
                    continue;
                const Sealed sealed = grantsFor(id, m_itemKeys.value(id), QStringLiteral("organization"),
                                                entry.value(QStringLiteral("organization")).toString(),
                                                entry.value(QStringLiteral("permission")).toString(),
                                                recipientsFrom(entry.value(QStringLiteral("members")).toArray()), {});
                if (sealed.grants.isEmpty())
                    continue;
                const int count = int(sealed.grants.size());
                call("POST", kApi + QStringLiteral("/items/") + id + QStringLiteral("/grants"),
                     {{QStringLiteral("grants"), sealed.grants}},
                     [work, self, count](const QJsonObject &) { work->sealed += count; (*self)(); },
                     [work, done](const QString &code) { done(code, work->sealed); });
                return;
            }
            const int total = work->sealed;
            if (total > 0)
                saveTrust([done, total](const QString &code) { done(code, total); });
            else
                done(QString(), total);
        };
        (*step)();
    }, [done](const QString &code) { done(code, 0); });
}

void VaultClient::organizations(std::function<void(const QString &, const QStringList &)> done)
{
    call("GET", kApi + QStringLiteral("/organizations"), {}, [done](const QJsonObject &payload) {
        QStringList names;
        for (const QJsonValue &value : payload.value(QStringLiteral("organizations")).toArray())
            names.append(value.toObject().value(QStringLiteral("name")).toString());
        done(QString(), names);
    }, [done](const QString &code) { done(code, {}); });
}

QString VaultClient::myFingerprint() const
{
    Bundle mine;
    Failure failure;
    QString text;
    if (!unlocked() || !publicBundle(m_keyset, m_account, &mine, &failure) || !fingerprint(mine, &text, &failure))
        return QString();
    return text;
}

void VaultClient::importLogins(const QList<ImportLogin> &logins, std::function<void(int, int)> progress,
                               std::function<void(const ImportReport &)> done)
{
    if (!unlocked()) {
        done({0, 0, int(logins.size()), QStringLiteral("vault_locked")});
        return;
    }
    const ImportPlan plan = planImport(logins, m_items);
    auto report = std::make_shared<ImportReport>();
    report->duplicates = plan.duplicates;
    const auto finish = [this, report, done] {
        load([report, done](const QString &) { done(*report); });
    };
    auto step = std::make_shared<std::function<void(qsizetype)>>();
    *step = [this, plan, report, progress, finish, weak = std::weak_ptr<std::function<void(qsizetype)>>(step)](qsizetype at) {
        if (at >= plan.fresh.size()) {
            finish();
            return;
        }
        save(importFields(plan.fresh.at(at)), QString(), [=](const QString &code, const QString &) {
            if (!code.isEmpty()) {
                report->stoppedBy = code;
                report->remaining = int(plan.fresh.size() - at);
                finish();
                return;
            }
            ++report->added;
            if (progress)
                progress(report->added, int(plan.fresh.size()));
            if (const auto next = weak.lock())
                (*next)(at + 1);
        });
    };
    (*step)(0);
}

void VaultClient::exportData(std::function<void(const QString &, const QJsonObject &)> done)
{
    call("GET", kApi + QStringLiteral("/export"), {}, [done](const QJsonObject &payload) { done(QString(), payload); },
         [done](const QString &code) { done(code, {}); });
}

void VaultClient::destroy(std::function<void(const QString &, const QJsonObject &)> done)
{
    call("POST", kApi + QStringLiteral("/uninstall"), {}, [this, done](const QJsonObject &payload) {
        lock();
        m_installed = false;
        m_server = {};
        done(QString(), payload);
    }, [done](const QString &code) { done(code, {}); });
}

}  // namespace forkmesh::vault
