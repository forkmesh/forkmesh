#include "CompactCelebration.h"

#include <QApplication>
#include <QFontMetrics>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using forkmesh::ui::CompactCelebrationLayout;
using forkmesh::ui::CompactCelebrationOverlay;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    std::fprintf(condition ? stdout : stderr, "%s: %s\n", condition ? "PASS" : "FAIL", message);
    if (!condition)
        ++failures;
}

bool linesFit(const CompactCelebrationLayout &layout)
{
    const int room = layout.card.width() - 36;
    const QFontMetrics head(layout.headlineFont);
    const QFontMetrics detail(layout.detailFont);
    for (const QString &line : layout.headlineLines)
        if (head.horizontalAdvance(line) > room)
            return false;
    for (const QString &line : layout.detailLines)
        if (detail.horizontalAdvance(line) > room)
            return false;
    return true;
}

bool elided(const QStringList &lines)
{
    for (const QString &line : lines)
        if (line.contains(QChar(0x2026)))
            return true;
    return false;
}

void savePicture(QWidget *window, const QString &name)
{
    const QByteArray folder = qgetenv("FORKMESH_CELEBRATION_SHOTS");
    if (folder.isEmpty())
        return;
    window->grab().save(QString::fromLocal8Bit(folder) + QLatin1Char('/') + name);
}

void checkLayout()
{
    const QString headline = QStringLiteral("\U0001F389 Mastodon post tracked in the CRM");
    const QString detail = QStringLiteral("“Hello from ForkMesh” with Ada Example");
    const CompactCelebrationLayout wide =
        CompactCelebrationOverlay::layoutFor(QApplication::font(), headline, detail, QSize(2560, 1400));
    check(wide.card.width() <= CompactCelebrationOverlay::kMaxCardWidth,
          "a wide window keeps the card at most 420 px wide");
    check(qFuzzyCompare(wide.headlineFont.pointSizeF(), 14.0) && wide.headlineFont.bold(),
          "the headline is 14 pt bold whatever the window width");
    check(qFuzzyCompare(wide.detailFont.pointSizeF(), 11.0), "the detail is 11 pt");
    check(linesFit(wide) && !elided(wide.headlineLines) && !elided(wide.detailLines),
          "the ordinary toot ping fits without clipping or ellipsis");
    const int expected = 24 + int(wide.headlineLines.size()) * wide.headlineLineHeight + 4 +
                         int(wide.detailLines.size()) * wide.detailLineHeight;
    check(wide.card.height() == expected, "the card height comes from the wrapped lines");

    const CompactCelebrationLayout narrow =
        CompactCelebrationOverlay::layoutFor(QApplication::font(), headline, detail, QSize(300, 500));
    check(narrow.card.width() <= 300 - 48, "a narrow window keeps 24 px either side");
    check(linesFit(narrow), "wrapped lines fit the narrow card");
    check(narrow.headlineLines.size() >= 2, "the headline wraps on a narrow window");
    check(!elided(narrow.headlineLines), "a wrapped headline is not cut");

    const QString link = QStringLiteral("https://mastodon.example/@someone/") + QString(110, QLatin1Char('7'));
    const CompactCelebrationLayout url =
        CompactCelebrationOverlay::layoutFor(QApplication::font(), headline, link, QSize(1200, 800));
    check(linesFit(url), "an unbreakable link wraps inside the card instead of overflowing");
    check(!elided(url.detailLines), "a link that fits in four lines is wrapped, not cut");

    const QString essay = QString(QStringLiteral("word ")).repeated(400);
    const CompactCelebrationLayout longBody =
        CompactCelebrationOverlay::layoutFor(QApplication::font(), headline, essay, QSize(1200, 800));
    check(longBody.detailLines.size() <= 4 && elided(longBody.detailLines) && linesFit(longBody),
          "a body too long to ever fit is elided on its last line");
}

void checkOverlay()
{
    QWidget window;
    window.resize(1600, 900);
    auto *layout = new QVBoxLayout(&window);
    auto *field = new QLineEdit;
    layout->addWidget(field);
    layout->addStretch();
    window.show();
    field->setFocus();
    QApplication::processEvents();

    QPointer<CompactCelebrationOverlay> overlay = new CompactCelebrationOverlay(
        &window, QStringLiteral("\U0001F389 Mastodon post tracked in the CRM"),
        QStringLiteral("“Hello from ForkMesh” with Ada Example"));
    QApplication::processEvents();
    const QRect card = overlay->cardGeometry();
    check(card.width() <= 420, "the overlay card is at most 420 px wide");
    check(qAbs(card.center().x() - window.width() / 2) <= 1 &&
              qAbs(card.center().y() - window.height() / 2) <= 1,
          "the card sits in the middle of the window");
    check(overlay->width() < window.width() && overlay->height() < window.height(),
          "the overlay covers only the card, not the whole window");
    check(overlay->testAttribute(Qt::WA_TransparentForMouseEvents), "clicks go through the card");
    check(overlay->focusPolicy() == Qt::NoFocus, "the card never takes focus");
    check(field->hasFocus() || !window.isActiveWindow(), "the focused field keeps its focus");
    overlay->advanceForTest(400);
    savePicture(&window, QStringLiteral("compact-celebration-wide.png"));

    overlay->showMessage(QStringLiteral("\U0001F389 3 Mastodon posts tracked in the CRM"),
                         QStringLiteral("“Third toot” with Grace Example"));
    check(overlay->headline().startsWith(QStringLiteral("\U0001F389 3 ")),
          "a new ping updates the same card");
    check(overlay->elapsedMs() <= CompactCelebrationOverlay::kPopMs, "an update restarts the timer");
    overlay->advanceForTest(CompactCelebrationOverlay::kDurationMs - 600);
    check(!overlay.isNull(), "the card is still up before its 3.5 s run out");

    window.resize(320, 560);
    overlay->advanceForTest(CompactCelebrationOverlay::kTickMs);
    check(overlay->cardGeometry().width() <= 320 - 48, "the card follows a resized window");
    check(qAbs(overlay->cardGeometry().center().x() - 160) <= 1, "and stays centred");
    overlay->showMessage(overlay->headline(), overlay->detail());
    overlay->advanceForTest(400);
    savePicture(&window, QStringLiteral("compact-celebration-narrow.png"));

    overlay->advanceForTest(CompactCelebrationOverlay::kDurationMs);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    check(overlay.isNull(), "the card deletes itself after about 3.5 s");
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    checkLayout();
    checkOverlay();
    if (failures == 0)
        std::printf("compact celebration tests passed\n");
    return failures == 0 ? 0 : 1;
}
