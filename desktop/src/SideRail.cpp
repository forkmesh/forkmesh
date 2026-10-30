#include "SideRail.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSet>

namespace forkmesh::siderail {

namespace {

QString field(const QJsonObject &object, const char *name)
{
    return object.value(QLatin1String(name)).toString().trimmed();
}

QString shortTitle(const QString &title)
{
    const QString simple = title.simplified();
    return simple.size() > kMaxTitleChars ? simple.left(kMaxTitleChars) : simple;
}

QJsonObject itemJson(const Item &item)
{
    QJsonObject object{{QStringLiteral("kind"), item.kind}};
    if (!item.repo.isEmpty())
        object.insert(QStringLiteral("repo"), item.repo);
    if (!item.id.isEmpty())
        object.insert(QStringLiteral("id"), item.id);
    if (!item.url.isEmpty())
        object.insert(QStringLiteral("url"), item.url);
    if (!item.title.isEmpty())
        object.insert(QStringLiteral("title"), item.title);
    return object;
}

Item itemFromJson(const QJsonObject &object)
{
    Item item;
    item.kind = field(object, "kind");
    item.repo = field(object, "repo");
    item.id = field(object, "id");
    item.url = field(object, "url");
    item.title = shortTitle(field(object, "title"));
    if (item.kind == kLinkKind)
        item = linkItem(QUrl(item.url), item.title);
    return item;
}

}

QString Item::key() const
{
    if (kind == kWorkflowKind)
        return kind + QLatin1Char(':') + repo.toLower() + QLatin1Char('|') + id;
    if (kind == kPageKind)
        return kind + QLatin1Char(':') + id;
    if (kind == kLinkKind)
        return kind + QLatin1Char(':') + url;
    return QString();
}

bool Item::valid() const
{
    if (kind == kWorkflowKind)
        return !repo.isEmpty() && !id.isEmpty() && repo.contains(QLatin1Char('/'));
    if (kind == kPageKind)
        return !id.isEmpty();
    if (kind == kLinkKind)
        return acceptableLink(QUrl(url));
    return false;
}

Item workflowItem(const QString &repo, const QString &id)
{
    Item item;
    item.kind = kWorkflowKind;
    item.repo = repo.trimmed();
    item.id = id.trimmed();
    return item;
}

Item pageItem(const QString &railKey, const QString &title)
{
    Item item;
    item.kind = kPageKind;
    item.id = railKey.trimmed();
    item.title = shortTitle(title);
    return item;
}

Item linkItem(const QUrl &url, const QString &title)
{
    Item item;
    item.kind = kLinkKind;
    if (acceptableLink(url))
        item.url = url.adjusted(QUrl::StripTrailingSlash).toString(QUrl::FullyEncoded);
    item.title = shortTitle(title.isEmpty() ? url.host() : title);
    return item;
}

QList<Item> parseItems(const QString &json)
{
    QList<Item> items;
    QSet<QString> seen;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &value : array) {
        const Item item = itemFromJson(value.toObject());
        if (!item.valid() || seen.contains(item.key()))
            continue;
        seen.insert(item.key());
        items.append(item);
        if (items.size() >= kMaxItems)
            break;
    }
    return items;
}

QString serializeItems(const QList<Item> &items)
{
    QJsonArray array;
    QSet<QString> seen;
    for (const Item &item : items) {
        if (!item.valid() || seen.contains(item.key()))
            continue;
        seen.insert(item.key());
        array.append(itemJson(item));
        if (array.size() >= kMaxItems)
            break;
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<Item> withWorkflows(const QList<Item> &items, const QList<WorkflowRef> &pinnedRight)
{
    QSet<QString> pinned;
    for (const WorkflowRef &ref : pinnedRight)
        pinned.insert(workflowItem(ref.repo, ref.id).key());
    QList<Item> merged;
    QSet<QString> seen;
    for (const Item &item : items) {
        if (!item.valid() || seen.contains(item.key()))
            continue;
        if (item.kind == kWorkflowKind && !pinned.contains(item.key()))
            continue;
        seen.insert(item.key());
        merged.append(item);
    }
    for (const WorkflowRef &ref : pinnedRight) {
        const Item item = workflowItem(ref.repo, ref.id);
        if (!item.valid() || seen.contains(item.key()))
            continue;
        seen.insert(item.key());
        merged.append(item);
    }
    return merged;
}

QList<Item> insertItem(const QList<Item> &items, const Item &item, int index)
{
    if (!item.valid())
        return items;
    QList<Item> result;
    int target = index < 0 ? int(items.size()) : index;
    for (int i = 0; i < items.size(); ++i) {
        if (items.at(i).key() == item.key()) {
            if (i < target)
                --target;
            continue;
        }
        result.append(items.at(i));
    }
    target = qBound(0, target, int(result.size()));
    result.insert(target, item);
    while (result.size() > kMaxItems)
        result.removeLast();
    return result;
}

QList<Item> removeItem(const QList<Item> &items, const QString &key)
{
    QList<Item> result;
    for (const Item &item : items)
        if (item.key() != key)
            result.append(item);
    return result;
}

int indexOfKey(const QList<Item> &items, const QString &key)
{
    for (int i = 0; i < items.size(); ++i)
        if (items.at(i).key() == key)
            return i;
    return -1;
}

bool acceptableLink(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty())
        return false;
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    return url.toString(QUrl::FullyEncoded).size() <= kMaxUrlChars;
}

int clampBrowserWidth(int width)
{
    if (width <= 0)
        return kDefaultBrowserWidth;
    return qBound(kMinBrowserWidth, width, kMaxBrowserWidth);
}

QByteArray encodeWorkflow(const QString &repo, const QString &id)
{
    return QJsonDocument(QJsonObject{{QStringLiteral("repo"), repo.trimmed()},
                                     {QStringLiteral("id"), id.trimmed()}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encodeWorkflowPin(const QString &repo, const QString &id)
{
    return (repo.trimmed() + QLatin1Char('\n') + id.trimmed()).toUtf8();
}

bool decodeWorkflow(const QByteArray &data, QString *repo, QString *id)
{
    const QJsonObject object = QJsonDocument::fromJson(data).object();
    const Item item = workflowItem(field(object, "repo"), field(object, "id"));
    if (!item.valid())
        return false;
    if (repo)
        *repo = item.repo;
    if (id)
        *id = item.id;
    return true;
}

QList<Item> itemsFromMime(const QMimeData *mime, const QString &railItemMime)
{
    QList<Item> items;
    if (!mime)
        return items;
    if (mime->hasFormat(QLatin1String(kItemMime))) {
        const Item item = itemFromJson(
            QJsonDocument::fromJson(mime->data(QLatin1String(kItemMime))).object());
        if (item.valid())
            items.append(item);
        return items;
    }
    if (mime->hasFormat(QLatin1String(kWorkflowMime))) {
        QString repo;
        QString id;
        if (decodeWorkflow(mime->data(QLatin1String(kWorkflowMime)), &repo, &id))
            items.append(workflowItem(repo, id));
        return items;
    }
    if (mime->hasFormat(QLatin1String(kWorkflowPinMime))) {
        const QStringList parts =
            QString::fromUtf8(mime->data(QLatin1String(kWorkflowPinMime))).split(QLatin1Char('\n'));
        const Item item = parts.size() == 2 ? workflowItem(parts.at(0), parts.at(1)) : Item();
        if (item.valid())
            items.append(item);
        return items;
    }
    if (!railItemMime.isEmpty() && mime->hasFormat(railItemMime)) {
        const Item item = pageItem(QString::fromUtf8(mime->data(railItemMime)));
        if (item.valid())
            items.append(item);
        return items;
    }
    QList<QUrl> urls = mime->hasUrls() ? mime->urls() : QList<QUrl>();
    if (urls.isEmpty() && mime->hasText())
        urls.append(QUrl(mime->text().trimmed()));
    for (const QUrl &url : std::as_const(urls)) {
        const Item item = linkItem(url);
        if (item.valid())
            items.append(item);
    }
    return items;
}

}
