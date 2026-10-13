#pragma once

#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyLedger.h"
#include "AgentToolImprovementLog.h"

#include <QRect>
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

// Compact dashboard of per-tool-family cost and efficiency, painted from a
// ledger snapshot: a KPI row of stat tiles, then one card per family with
// the headline saving, call/error/latency facts, an actual-versus-baseline
// bar, and a sparkline of recent per-call savings. Between the two sits the
// realtime feed of potential improvements. Plain QWidget so it needs no moc
// registration; it re-reads the ledger and the log on refresh().
class AgentToolEfficiencyBoard : public QWidget
{
public:
    explicit AgentToolEfficiencyBoard(QWidget *parent = nullptr);

    void setLedger(const AgentToolEfficiencyLedger *ledger); // not owned
    // Not owned; takes effect on the next refresh().
    void setImprovementLog(const AgentToolImprovementLog *log);
    void refresh(qint64 nowMs = -1);

    // Operator switches. Clicking a card's toggle reports the request here;
    // the owner persists it and then pushes the new set back with
    // setDisabledFamilies() so the board only ever paints stored policy.
    void setDisabledFamilies(const QSet<QString> &families);
    bool familyEnabled(const QString &family) const;
    QRect toggleRect(const QString &family) const;
    std::function<void(const QString &family, bool enabled)> familyToggled;
    // Clicking a card (outside its toggle) selects it and asks the owner to
    // open that family's details beside the board.
    std::function<void(const QString &family)> familyActivated;
    void setSelectedFamily(const QString &family);
    QString selectedFamily() const { return m_selectedFamily; }
    // Rich-text details for the side panel: every ledger fact about the
    // family plus its improvement signals.
    QString familyDetailsHtml(const QString &family) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Probes for tests and tooltips.
    int cardCount() const { return m_cards.size(); }
    int observedCardCount() const;
    int columns() const { return m_columns; }
    QRect cardRect(const QString &family) const;
    QString familyAt(const QPoint &point) const;
    QString cardHeadline(const QString &family) const;
    QString tileValue(const QString &label) const;
    int contentHeight() const { return m_contentHeight; }
    int improvementRowCount() const { return m_improvementRows.size(); }
    QString improvementRowTitle(int index) const;
    QRect improvementRowRect(int index) const;

    static QString compactNumber(qint64 value);
    static QString compactUsd(double value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Tile {
        QString label;
        QString value;
        QString note;
        QVector<qint64> trend;
        QRect rect;
    };
    struct Card {
        AgentToolFamily family;
        AgentToolFamilyStats stats;
        QRect rect;
        QRect toggle;
        bool observed = false;
        bool enabled = true;
        QString headline;
        QString headlineNote;
        QString facts;
    };
    struct ImprovementRow {
        AgentToolImprovement signal;
        QString title;
        QString meta;
        QString note;
        QRect rect;
    };
    struct Section {
        QString title;
        QRect rect;
    };

    void rebuildModel(qint64 nowMs);
    void relayout(int width);
    void paintTile(QPainter &painter, const Tile &tile) const;
    void paintCard(QPainter &painter, const Card &card, bool hovered) const;
    void paintSparkline(QPainter &painter, const QRect &rect,
                        const QVector<qint64> &values) const;
    void paintImprovementRow(QPainter &painter, const ImprovementRow &row) const;
    QString cardTooltip(const Card &card) const;
    QString improvementTooltip(const AgentToolImprovement &signal) const;
    bool darkSurface() const;
    QColor accentColor() const;
    QColor dangerColor() const;

    const AgentToolEfficiencyLedger *m_ledger = nullptr;
    const AgentToolImprovementLog *m_improvementLog = nullptr;
    QString m_selectedFamily;
    QVector<ImprovementRow> m_improvementRows;
    QRect m_improvementEmptyRect;
    QSet<QString> m_disabled;
    AgentToolLedgerTotals m_totals;
    QVector<Tile> m_tiles;
    QVector<Card> m_cards;
    QVector<Section> m_sections;
    qint64 m_maxConsumed = 0;
    int m_columns = 1;
    int m_contentHeight = 0;
    int m_layoutWidth = -1;
    int m_hoverCard = -1;
    int m_hoverImprovement = -1;
};
