#include "HomeLayout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace HomeLayout {

Pages resolve(const Pages &saved, const QStringList &present)
{
    if (saved.isEmpty())
        return {present};
    const QSet<QString> known(present.cbegin(), present.cend());
    QSet<QString> placed;
    Pages pages;
    for (const QStringList &page : saved) {
        QStringList kept;
        for (const QString &key : page) {
            if (!known.contains(key) || placed.contains(key))
                continue;
            placed.insert(key);
            kept.append(key);
        }
        pages.append(kept);
    }
    for (const QString &key : present)
        if (!placed.contains(key)) {
            placed.insert(key);
            pages.last().append(key);
        }
    return pages;
}

int pageOf(const Pages &pages, const QString &key)
{
    for (int i = 0; i < pages.size(); ++i)
        if (pages.at(i).contains(key))
            return i;
    return -1;
}

Pages moveToPage(Pages pages, const QString &key, int page)
{
    const int from = pageOf(pages, key);
    if (from < 0 || page < 0 || page == from)
        return pages;
    if (page >= pages.size()) {
        pages.append(QStringList());
        page = pages.size() - 1;
    }
    pages[from].removeAll(key);
    pages[page].append(key);
    if (pages.at(from).isEmpty() && pages.size() > 1)
        pages.removeAt(from);
    return pages;
}

Pages moveWithinPage(Pages pages, const QString &key, int index)
{
    const int page = pageOf(pages, key);
    if (page < 0)
        return pages;
    QStringList &keys = pages[page];
    keys.removeAll(key);
    keys.insert(qBound(0, index, int(keys.size())), key);
    return pages;
}

Pages addPage(Pages pages)
{
    pages.append(QStringList());
    return pages;
}

Pages removePage(Pages pages, int page)
{
    if (pages.size() <= 1 || page < 0 || page >= pages.size())
        return pages;
    const int into = page > 0 ? page - 1 : 1;
    pages[into].append(pages.at(page));
    pages.removeAt(page);
    return pages;
}

QString toJson(const Pages &pages)
{
    QJsonArray outer;
    for (const QStringList &page : pages)
        outer.append(QJsonArray::fromStringList(page));
    return QString::fromUtf8(QJsonDocument(outer).toJson(QJsonDocument::Compact));
}

Pages fromJson(const QString &json)
{
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    Pages pages;
    if (!document.isArray())
        return pages;
    for (const QJsonValue &value : document.array()) {
        if (!value.isArray())
            continue;
        QStringList page;
        for (const QJsonValue &key : value.toArray())
            if (key.isString() && !key.toString().isEmpty())
                page.append(key.toString());
        pages.append(page);
    }
    return pages;
}

}
