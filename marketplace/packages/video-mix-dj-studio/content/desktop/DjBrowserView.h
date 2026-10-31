#pragma once

#include <QString>
#include <QWidget>

#include <functional>

class QKeyEvent;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace forkmesh::djstudio {

class DjButton;
class DjModel;
struct DjFocus;

class DjBrowserView : public QWidget
{
public:
    explicit DjBrowserView(DjModel *model, QWidget *parent = nullptr);

    void refreshSources();
    void refreshList();
    void refreshIcons();
    void showSource(const QString &key);
    void focusSearch();
    void setQuery(const QString &query);
    int bestDeck() const;

    std::function<void(const QString &)> songEditRequested;
    std::function<void(const QString &)> similarRequested;
    std::function<void(const DjFocus &)> exportRequested;
    std::function<void()> importRequested;
    std::function<void()> pasteRequested;
    std::function<bool(QKeyEvent *)> shortcut;

protected:
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Row {
        QString id;
        QString title;
        QString author;
        QString info;
        int plays = 0;
        double seconds = 0;
        QString queueId;
        QString effect;
    };

    QList<Row> rows() const;
    QString source() const;
    void activate(const Row &row, int deck = -1);
    void rowMenu(const QPoint &global);
    void sourceMenu(QTreeWidgetItem *item, const QPoint &global);
    void runSearch();
    void showInfo(const Row &row);
    void refreshSuggestion();
    void startDrag(QTreeWidgetItem *item);
    Row rowFor(QTreeWidgetItem *item) const;

    DjModel *m_model;
    QTreeWidget *m_sources = nullptr;
    QTreeWidget *m_list = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_count = nullptr;
    QLabel *m_infoArt = nullptr;
    QLabel *m_infoTitle = nullptr;
    QLabel *m_infoArtist = nullptr;
    QLabel *m_infoDetails = nullptr;
    QLabel *m_suggestion = nullptr;
    QString m_infoId;
    QList<DjButton *> m_loadButtons;
    QList<Row> m_rows;
    QList<Row> m_suggestions;
    int m_suggestionIndex = 0;
    QString m_suggestionSeed;
    QString m_source = QStringLiteral("history");
    QPoint m_dragStart;
    bool m_building = false;
};

}
