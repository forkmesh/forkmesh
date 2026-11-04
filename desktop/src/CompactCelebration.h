#pragma once

#include <QFont>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

class QTimer;

namespace forkmesh::ui {

struct CompactCelebrationLayout {
    QSize card;
    QFont headlineFont;
    QFont detailFont;
    QStringList headlineLines;
    QStringList detailLines;
    int headlineLineHeight = 0;
    int detailLineHeight = 0;
};

class CompactCelebrationOverlay final : public QWidget
{
public:
    static constexpr int kDurationMs = 3500;
    static constexpr int kFadeMs = 900;
    static constexpr int kPopMs = 220;
    static constexpr int kTickMs = 33;
    static constexpr int kMaxCardWidth = 420;
    static constexpr int kWindowMargin = 24;
    static constexpr int kSparkleMargin = 14;
    static constexpr qreal kHeadlinePoints = 14.0;
    static constexpr qreal kDetailPoints = 11.0;

    explicit CompactCelebrationOverlay(QWidget *parent, const QString &headline,
                                       const QString &detail = QString());

    void showMessage(const QString &headline, const QString &detail = QString());
    QString headline() const { return m_headline; }
    QString detail() const { return m_detail; }
    QRect cardGeometry() const;
    int elapsedMs() const { return m_elapsedMs; }
    void advanceForTest(int ms);

    static CompactCelebrationLayout layoutFor(const QFont &base, const QString &headline,
                                              const QString &detail, const QSize &window);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void relayout();
    void tick(int ms);

    QTimer *m_timer = nullptr;
    QString m_headline;
    QString m_detail;
    CompactCelebrationLayout m_layout;
    QSize m_window;
    int m_elapsedMs = 0;
};

}
