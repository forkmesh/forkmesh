#pragma once

#include <QIcon>
#include <QList>
#include <QString>
#include <QVector>
#include <QWidget>

class PullBadgeWidget : public QWidget
{
public:
    struct FileEntry {
        QString path;
        int adds = 0;
        int dels = 0;
        QIcon icon;
    };

    explicit PullBadgeWidget(QWidget *parent = nullptr);

    void setPull(const QString &title, int number, const QString &author,
                 int additions, int deletions, const QList<FileEntry> &files);
    void clearPull();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *e) override; // per-tile tooltips (path +adds -dels)

private:
    struct Tile {
        QRect rect; // icon square; the ratio bar is painted just below it
        int fileIndex = -1;
    };
    struct GroupSegment { // one contiguous run of a directory's tiles in a row
        QRect lineRect;
        bool labeled = false; // the group's label sits under its last segment
        QString label;
    };

    void relayout(); // recompute tile/segment geometry for the current width

    QString m_title;
    int m_number = 0;
    QString m_author;
    int m_additions = 0;
    int m_deletions = 0;
    QList<FileEntry> m_files; // sorted by path so directories cluster
    QVector<Tile> m_tiles;
    QVector<GroupSegment> m_segments;
    int m_contentHeight = 0;
};
