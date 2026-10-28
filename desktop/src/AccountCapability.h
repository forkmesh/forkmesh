#pragma once

#include <QJsonObject>
#include <QString>

namespace AccountCapability {

inline QString normalizedAccount(const QString &account)
{
    return account.trimmed().toLower();
}

// A login is initiated for an account selected before credentials are entered.
// Never let a successful email/password response silently switch that selection:
// the response must name the exact requested account.  Besides avoiding confusing
// cross-account UI state, this keeps a session token bound to the cache key and
// desktop identity proof it was requested for.
inline bool loginResponseMatchesRequestedAccount(
    const QString &requestedAccount, const QString &returnedAccount)
{
    const QString requested = normalizedAccount(requestedAccount);
    const QString returned = normalizedAccount(returnedAccount);
    return !requested.isEmpty() && !returned.isEmpty() &&
           requested == returned;
}

// The words for a login that produced no recognised relay verdict. A request
// that never reached the relay (TLS, proxy, DNS, offline mode) and a relay
// answer without a JSON error both used to read "Login failed." -- the same
// dialog as a rejected password, with nothing to act on (#694). Say that the
// credentials were not checked, and carry the transport reason.
inline QString loginFailureFallbackMessage(int httpStatus, const QString &error,
                                           const QString &host,
                                           const QString &transportError)
{
    if (!error.isEmpty())
        return QStringLiteral("Login failed: %1.").arg(error);
    const QString relay =
        host.trimmed().isEmpty() ? QStringLiteral("the relay") : host.trimmed();
    if (httpStatus <= 0) {
        const QString detail = transportError.simplified();
        return QStringLiteral("Could not reach %1, so your email and password "
                              "were not checked.\n\n%2")
            .arg(relay, detail.isEmpty()
                            ? QStringLiteral("The relay did not answer.")
                            : detail);
    }
    return QStringLiteral("%1 answered HTTP %2 without a login result, so your "
                          "email and password were not checked. Try again in "
                          "a few minutes.")
        .arg(relay, QString::number(httpStatus));
}

inline QJsonObject passwordLoginRequest(const QString &email,
                                        const QString &password,
                                        const QString &totp)
{
    return QJsonObject{{QStringLiteral("identifier"), email.trimmed()},
                       {QStringLiteral("email"), email.trimmed()},
                       {QStringLiteral("password"), password},
                       {QStringLiteral("totp"), totp}};
}

inline QString loginWaitText(qint64 retryAfterMs)
{
    const qint64 seconds = qMax<qint64>(1, (retryAfterMs + 999) / 1000);
    if (seconds < 60)
        return seconds == 1 ? QStringLiteral("1 second")
                            : QStringLiteral("%1 seconds").arg(seconds);
    const qint64 minutes = (seconds + 59) / 60;
    return minutes == 1 ? QStringLiteral("1 minute")
                        : QStringLiteral("%1 minutes").arg(minutes);
}

inline QString loginLockoutMessage(qint64 retryAfterMs)
{
    const QString wait = retryAfterMs > 0
                             ? QStringLiteral("about %1").arg(
                                   loginWaitText(retryAfterMs))
                             : QStringLiteral("a few minutes");
    return QStringLiteral(
               "Too many failed attempts, so sign-in is paused. Wait %1, "
               "then try again. This is a short pause, not a wrong password: "
               "your password has not changed.")
        .arg(wait);
}

inline QString incorrectPasswordMessage(const QString &host)
{
    const QString site =
        host.trimmed().isEmpty() ? QStringLiteral("the ForkMesh website")
                                 : host.trimmed();
    return QStringLiteral(
               "Incorrect email or password. The desktop app uses the same "
               "account and password as %1, so use the email and password "
               "that work there. Click the eye in the password field to "
               "check what you typed.")
        .arg(site);
}

inline bool allowsPasswordOnlyFallback(const QString &error)
{
    return error == QLatin1String("pubkey_mismatch") ||
           error == QLatin1String("device_proof_required") ||
           error == QLatin1String("device_key_conflict");
}

inline bool ownerSigningAllowed(
    bool activeAccount, bool desktopCapable,
    const QString &sessionAccount = QString(),
    const QString &signerAccount = QString())
{
    if (!activeAccount || !desktopCapable)
        return false;
    const QString session = normalizedAccount(sessionAccount);
    if (session.isEmpty())
        return false;
    if (signerAccount.trimmed().isEmpty())
        return true;
    return session == normalizedAccount(signerAccount);
}

inline bool persistedMarkerMatches(const QString &storedAccount,
                                   const QString &storedPublicKey,
                                   const QString &currentAccount,
                                   const QString &currentPublicKey)
{
    const QString stored = normalizedAccount(storedAccount);
    return !stored.isEmpty() && !storedPublicKey.isEmpty() &&
           stored == normalizedAccount(currentAccount) &&
           storedPublicKey == currentPublicKey;
}

} // namespace AccountCapability
