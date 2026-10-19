#include "NativePackageExtension.h"

#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <utility>

namespace forkmesh::market {
namespace {

QMap<QString, NativePackageContribution> &contributions()
{
    static QMap<QString, NativePackageContribution> registry;
    return registry;
}

std::function<bool(const QString &)> &loader()
{
    static std::function<bool(const QString &)> hook;
    return hook;
}

QString contributionError(const NativePackageContribution &contribution)
{
    static const QRegularExpression entrypointPattern(
        QStringLiteral("^(?:builtin|native):[a-z][a-z0-9.-]{0,95}$"));
    static const QRegularExpression typePattern(
        QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)*$"));
    static const QRegularExpression iconPattern(
        QStringLiteral("^[a-z][a-z0-9-]{0,39}$"));
    static const QRegularExpression apiPattern(
        QStringLiteral("^/api/[a-z0-9][a-z0-9._/-]{0,154}$"));
    static const QRegularExpression proofPattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));

    if (!entrypointPattern.match(contribution.entrypoint).hasMatch())
        return QStringLiteral("invalid desktop entrypoint");
    if (contribution.navigationLabel.trimmed() !=
            contribution.navigationLabel ||
        contribution.navigationLabel.isEmpty() ||
        contribution.navigationLabel.size() > 80)
        return QStringLiteral("invalid navigation label");
    if (!iconPattern.match(contribution.navigationIcon).hasMatch())
        return QStringLiteral("invalid navigation icon");
    static const QRegularExpression browserPathPattern(
        QStringLiteral("^/[a-z0-9][a-z0-9_-]{0,63}(?:/[a-z0-9][a-z0-9_-]{0,63}){0,3}$"));
    if (contribution.browserPath.isEmpty()) {
        if (!contribution.factory)
            return QStringLiteral("missing page factory");
    } else if (contribution.factory ||
               !browserPathPattern.match(contribution.browserPath).hasMatch() ||
               !contribution.apiPrefix.isEmpty()) {
        return QStringLiteral("invalid browser link");
    }

    QSet<QString> types;
    for (const QString &type : contribution.supportedTypes) {
        if (!typePattern.match(type).hasMatch() || types.contains(type))
            return QStringLiteral("invalid or duplicate package type");
        types.insert(type);
    }

    if (contribution.apiPrefix.isEmpty()) {
        if (!contribution.requestProof.isEmpty())
            return QStringLiteral("request proof without an API prefix");
    } else if (!apiPattern.match(contribution.apiPrefix).hasMatch() ||
               contribution.apiPrefix.contains(QStringLiteral("//")) ||
               contribution.apiPrefix.contains(QStringLiteral("/../")) ||
               !proofPattern.match(contribution.requestProof).hasMatch()) {
        return QStringLiteral("invalid package API boundary");
    }
    return QString();
}

} // namespace

NativePackageRegistration::NativePackageRegistration(
    NativePackageContribution contribution)
{
    m_error = contributionError(contribution);
    if (!m_error.isEmpty())
        return;
    if (contributions().contains(contribution.entrypoint)) {
        m_error = QStringLiteral("duplicate desktop entrypoint");
        return;
    }
    contributions().insert(contribution.entrypoint, std::move(contribution));
    m_accepted = true;
}

const NativePackageContribution *nativePackageContribution(
    const QString &entrypoint, const QString &packageType)
{
    auto found = contributions().constFind(entrypoint);
    if (found == contributions().cend() && loader() && loader()(entrypoint))
        found = contributions().constFind(entrypoint);
    if (found == contributions().cend())
        return nullptr;
    if (!packageType.isEmpty() && !found->supportedTypes.isEmpty() &&
        !found->supportedTypes.contains(packageType))
        return nullptr;
    return &found.value();
}

QList<QString> nativePackageEntrypoints()
{
    return contributions().keys();
}

void setNativePackageLoader(std::function<bool(const QString &entrypoint)> hook)
{
    loader() = std::move(hook);
}

void unregisterNativePackageEntrypoint(const QString &entrypoint)
{
    contributions().remove(entrypoint);
}

QString nativePackageLifecyclePath(const QString &organization,
                                   const QString &packageId,
                                   const QString &operation)
{
    static const QRegularExpression segmentPattern(
        QStringLiteral("^[a-z][a-z0-9-]{0,62}$"));
    static const QSet<QString> operations{
        QStringLiteral("install"),
        QStringLiteral("uninstall"),
        QStringLiteral("export"),
    };
    if (!segmentPattern.match(organization).hasMatch() ||
        !segmentPattern.match(packageId).hasMatch() ||
        (!operation.isEmpty() && !operations.contains(operation))) {
        return {};
    }
    QString path = QStringLiteral("/api/plugins/%1/%2")
                       .arg(organization, packageId);
    if (!operation.isEmpty())
        path += QLatin1Char('/') + operation;
    return path;
}

bool nativePackageRequestPathAllowed(const QString &packageId,
                                     const QString &apiPrefix,
                                     const QString &path)
{
    const QUrl relative(path);
    if (!relative.isValid() || !relative.isRelative() || relative.hasFragment())
        return false;
    const QString cleanPath = relative.path(QUrl::FullyDecoded);
    const QStringList cleanParts = cleanPath.split(
        QLatin1Char('/'), Qt::KeepEmptyParts);
    if (!cleanPath.startsWith(QLatin1Char('/')) ||
        cleanPath.contains(QStringLiteral("//")) ||
        cleanParts.contains(QStringLiteral(".")) ||
        cleanParts.contains(QStringLiteral(".."))) {
        return false;
    }
    if (!apiPrefix.isEmpty() &&
        (cleanPath == apiPrefix ||
         cleanPath.startsWith(apiPrefix + QLatin1Char('/')))) {
        return true;
    }
    const QStringList parts = cleanPath.split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() != 4 && parts.size() != 5)
        return false;
    const QString operation = parts.size() == 5 ? parts.at(4) : QString();
    return parts.at(0) == QLatin1String("api") &&
           parts.at(1) == QLatin1String("plugins") &&
           parts.at(3) == packageId &&
           !nativePackageLifecyclePath(parts.at(2), parts.at(3), operation)
                .isEmpty();
}

QUrl nativePackageWebPageUrl(const QUrl &base, const QString &path)
{
    if (!base.isValid() || base.host().isEmpty() ||
        (base.scheme() != QLatin1String("http") &&
         base.scheme() != QLatin1String("https")))
        return {};
    const QUrl relative(path);
    if (!relative.isValid() || !relative.isRelative() ||
        !relative.host().isEmpty() || relative.hasFragment())
        return {};
    const QString cleanPath = relative.path(QUrl::FullyDecoded);
    const QStringList parts = cleanPath.split(QLatin1Char('/'));
    if (!cleanPath.startsWith(QLatin1Char('/')) ||
        cleanPath.contains(QStringLiteral("//")) ||
        parts.contains(QStringLiteral(".")) ||
        parts.contains(QStringLiteral("..")))
        return {};
    QUrl url = base;
    url.setUserName(QString());
    url.setPassword(QString());
    url.setPath(cleanPath);
    url.setQuery(relative.query(QUrl::FullyEncoded));
    url.setFragment(QString());
    return url;
}

} // namespace forkmesh::market
