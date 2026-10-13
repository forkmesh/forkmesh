#pragma once

#include "AgentGuardrailLedger.h"

#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

// A repository's guardrail settings as the Guardrails tab reports them.
struct AgentGuardrailRepositoryPolicy {
    QString repository; // "owner/name"
    bool enabled = false;
    QStringList allowedDirectories;
    QStringList missingDirectories; // allowed but absent from the checkout
};

// Compact dashboard of repository guardrails, in the same form as the Tool
// efficiency board: a KPI row of stat tiles, then one card per repository
// with its policy, blocked calls, and Approve/Deny answers. Plain QWidget so
// it needs no moc registration; it re-reads its inputs on refresh().
class AgentGuardrailBoard : public QWidget
{
public:
    explicit AgentGuardrailBoard(QWidget *parent = nullptr);

    void setLedger(const AgentGuardrailLedger *ledger); // not owned
    void setRepositories(const QVector<AgentGuardrailRepositoryPolicy> &policies);
    void refresh(qint64 nowMs = -1);

    // Clicking a card asks the owner to open that repository's settings.
    std::function<void(const QString &repository)> repositoryActivated;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    // Probes for tests and tooltips.
    int cardCount() const { return m_cards.size(); }
    QRect cardRect(const QString &repository) const;
    QString repositoryAt(const QPoint &point) const;
    QString cardHeadline(const QString &repository) const;
    QString cardNote(const QString &repository) const;
    QString tileValue(const QString &label) const;
    QString tileNote(const QString &label) const;
    int contentHeight() const { return m_contentHeight; }

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
        bool attention = false;
        QRect rect;
    };
    struct Card {
        AgentGuardrailRepositoryPolicy policy;
        AgentGuardrailRepoStats stats;
        QRect rect;
        bool attention = false;
        QString headline;
        QString note;
        QString facts;
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
    QString cardTooltip(const Card &card) const;
    const Card *cardFor(const QString &repository) const;
    const Tile *tileFor(const QString &label) const;
    bool darkSurface() const;
    QColor accentColor() const;
    QColor dangerColor() const;

    const AgentGuardrailLedger *m_ledger = nullptr;
    QVector<AgentGuardrailRepositoryPolicy> m_policies;
    AgentGuardrailTotals m_totals;
    QVector<Tile> m_tiles;
    QVector<Card> m_cards;
    QVector<Section> m_sections;
    int m_columns = 1;
    int m_contentHeight = 0;
    int m_layoutWidth = -1;
    int m_hoverCard = -1;
};
