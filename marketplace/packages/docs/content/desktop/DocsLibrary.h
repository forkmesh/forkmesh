#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace forkmesh::docs {

struct DocsArea {
    QString key;
    QString label;
    QString blurb;
};

struct DocsPage {
    QString path;
    QString title;
    QString description;
    QString area;
    QString markdown;
    QString searchText;
    QStringList links;
    QStringList backlinks;
    int words = 0;
    bool asset = false;
};

struct DocsHit {
    QString path;
    QString snippet;
    int score = 0;
};

struct DocsTarget {
    enum class Kind { None, Anchor, Page, External };
    Kind kind = Kind::None;
    QString path;
    QString fragment;
    QUrl url;
};

class DocsLibrary
{
public:
    static QString bundledRoot();
    static const QList<DocsArea> &areas();
    static const DocsArea *area(const QString &key);
    static QString areaFor(const QString &path);
    static QString webUrl(const QString &path);
    static QString sourceUrl(const QString &repositoryPath);
    static QString titleOf(const QString &markdown);
    static QString descriptionOf(const QString &markdown);
    static QStringList linkTargets(const QString &markdown);
    static int readingMinutes(int words);

    bool load(const QString &root);
    bool isEmpty() const { return m_pages.isEmpty(); }
    const QList<DocsPage> &pages() const { return m_pages; }
    const DocsPage *page(const QString &path) const;
    int indexOf(const QString &path) const;
    QList<const DocsPage *> pagesIn(const QString &area) const;
    QString homePath() const;
    QString previousPath(const QString &path) const;
    QString nextPath(const QString &path) const;
    QList<DocsHit> search(const QString &query, int limit) const;
    DocsTarget resolve(const QString &fromPath, const QString &href) const;

private:
    QString pagePathFor(const QString &repositoryPath) const;

    QList<DocsPage> m_pages;
    QHash<QString, int> m_index;
};

}
