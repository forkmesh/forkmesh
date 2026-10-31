#pragma once

#include "SessionInsights.h"

#include <QColor>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QAbstractButton;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace forkmesh::ui {

class SessionSidePanels : public QScrollArea
{
public:
    struct Details {
        bool session = false;
        QString machine;
        QString machineNote;
        QString machineIcon;
        QString path;
        QString pathFull;
        QString branch;
        QString branchNote;
        QString agent;
        QString mode;
        qint64 contextUsed = 0;
        qint64 contextWindow = 0;
        qint64 totalTokens = 0;
        qint64 promptTokens = 0;
        qint64 completionTokens = 0;
        int turns = 0;
        double costUsd = 0.0;
        int files = -1;
        int added = -1;
        int removed = -1;
        int dirty = -1;
        int prNumber = 0;
        bool conflicted = false;
        bool remoteAvailable = false;
        bool remoteOn = true;
        QStringList remotePhones;
        bool remoteLocal = false;
        QString remotePaired;
        qint64 remoteLastSeenMs = 0;
    };

    struct SubSession {
        QString id;
        QString icon;
        QString text;
        QString trailing;
        QString tip;
        int depth = 0;
    };

    struct Feed {
        int session = 0;
        int consumed = 0;
        int cloudTurns = -1;
        qint64 cloudTail = -1;
    };

    explicit SessionSidePanels(QWidget *parent = nullptr);

    void clearActivity();
    void append(const QJsonObject &event);
    void replaceActivity(const QList<QJsonObject> &events);
    void setDetails(const Details &details);
    void setRemoteActions(std::function<void(bool)> toggle, std::function<void()> connect);
    void setSubSessions(const QList<SubSession> &subSessions, bool canStop,
                        std::function<void(const QString &)> stop);
    Feed &feed() { return m_feed; }
    void reloadLayout();

    QStringList cardOrder() const { return m_order; }
    QStringList visibleCards() const;
    void moveCard(const QString &key, int toIndex);
    void setCardHidden(const QString &key, bool hidden);
    void resetCards();
    bool cardCollapsed(const QString &key) const { return m_collapsed.contains(key); }
    void setCardCollapsed(const QString &key, bool collapsed);
    const sessioninsights::Collector &collector() const { return m_collector; }
    QFrame *cardFrame(const QString &key) const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Row {
        QPointer<QWidget> row;
        QPointer<QLabel> swatch;
        QPointer<QLabel> icon;
        QPointer<QLabel> text;
        QPointer<QLabel> trailing;
        QPointer<QLabel> share;
        QPointer<QToolButton> stop;
    };
    struct Item {
        QString icon;
        QString text;
        QString trailing;
        QString tip;
        QString open;
        QColor swatch = QColor();
        QString share = QString();
    };
    struct Card {
        QString key;
        QPointer<QFrame> frame;
        QPointer<QPushButton> head;
        QPointer<QWidget> body;
        QPointer<QLabel> chevron;
        QPointer<QLabel> meta;
        QPointer<QLabel> hint;
        QPointer<QPushButton> more;
        QList<Row> rows;
        bool expanded = false;
    };

    Card &makeCard(const QString &key, const QString &title, const QString &hint);
    Row &addRow(Card &card);
    void setRow(const Row &row, const Item &item);
    void fillList(Card &card, const QList<Item> &items, const QString &meta);
    void fillFixed(Card &card, const QList<Item> &items, const QString &meta);
    void refreshActivity();
    void refreshDetails();
    void refreshRemote();
    void applyOrder();
    void applyCollapsed(Card &card);
    void saveLayout() const;
    void scheduleSlice();
    void runSlice();
    void cardMenu(const QString &key, const QPoint &globalPos);
    void startDrag(const QString &key);
    int dropIndex(int y, const QString &dragging) const;

    QWidget *m_inner = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QHash<QString, Card> m_cards;
    QStringList m_order;
    QSet<QString> m_hidden;
    QSet<QString> m_collapsed;
    QPointer<QWidget> m_contextBarRow;
    QPointer<QProgressBar> m_contextBar;
    QPointer<QLabel> m_contextUsed;
    QPointer<QLabel> m_contextWindow;
    QPointer<QWidget> m_remoteRow;
    QPointer<QAbstractButton> m_remoteSwitch;
    QPointer<QLabel> m_remoteStatus;
    std::function<void(bool)> m_remoteToggle;
    std::function<void()> m_remoteConnect;
    Details m_details;
    Feed m_feed;
    std::function<void(const QString &)> m_stopSubSession;
    sessioninsights::Collector m_collector;
    QList<QJsonObject> m_queue;
    int m_cursor = 0;
    bool m_sliceQueued = false;
    quint64 m_generation = 0;
    QString m_pressKey;
    QPoint m_pressPos;
    QString m_dragKey;
};

}
