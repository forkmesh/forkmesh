#include "Tooltips.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QScreen>
#include <QToolTip>
#include <QWidget>

#include <functional>
#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n";         \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static void pump(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

static void describe(const char *stage, QWidget *tip, int placements)
{
    const QRect f = tip ? tip->frameGeometry() : QRect();
    std::cout << stage << ": tip " << (tip ? "visible" : "absent") << " frame "
              << f.x() << "," << f.y() << " " << f.width() << "x" << f.height()
              << " pointer " << QCursor::pos().x() << "," << QCursor::pos().y()
              << " placements " << placements << "\n";
}

static bool qtTipOnScreen()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (Tooltips::Presenter::isQtTooltipWindow(w) && w->isVisible()
            && !w->testAttribute(Qt::WA_DontShowOnScreen))
            return true;
    return false;
}

static bool waitFor(const std::function<bool()> &done, int ms = 2000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return done();
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    // ---- preview(): bounded, escaped, honest about what it left out --------
    {
        const QString shortTip = Tooltips::preview(QStringLiteral("ls -la"));
        CHECK(shortTip.contains(QStringLiteral("ls -la")));
        CHECK(!shortTip.contains(QStringLiteral("more")));
        CHECK(Tooltips::preview(QString()).isEmpty());
        CHECK(Tooltips::preview(QStringLiteral("  \n\r\n")).isEmpty());
        CHECK(Tooltips::preview(QStringLiteral("a < b && c"))
                  .contains(QStringLiteral("a &lt; b &amp;&amp; c")));

        QStringList many;
        for (int i = 0; i < 40; ++i)
            many << QStringLiteral("line %1").arg(i);
        QString p = Tooltips::preview(many.join(QLatin1Char('\n')), 12, 100000);
        CHECK(p.contains(QStringLiteral("line 11")));
        CHECK(!p.contains(QStringLiteral("line 12")));
        CHECK(p.contains(QStringLiteral("28 more lines")));

        // 'Z' never appears in the markup, unlike 'x' (4px) or 'y' (style).
        p = Tooltips::preview(QString(5000, QLatin1Char('Z')), 12, 1000);
        CHECK(p.count(QLatin1Char('Z')) == 1000);
        CHECK(p.contains(QStringLiteral("4000 more characters")));

        p = Tooltips::preview(QStringLiteral("a\r\nb\r\n\r\n"));
        CHECK(p.contains(QStringLiteral("a\nb</pre>")));
        CHECK(!p.contains(QStringLiteral("more")));

        // The cut never splits a surrogate pair.
        const QString emoji = QString::fromUtf8("\xF0\x9F\x98\x80");
        p = Tooltips::preview(QString(600, QLatin1Char('W')) + emoji.repeated(300),
                              12, 601);
        CHECK(!p.contains(QChar(0xFFFD)));
        CHECK(p.count(QLatin1Char('W')) == 600);
    }

    // ---- placeBeside(): below, above, right, left, then a trimmed strip -----
    {
        const QRect screen(0, 0, 1920, 1080);
        QRect r = Tooltips::placeBeside(QRect(0, 0, 300, 100), QPoint(500, 500), screen);
        CHECK(r.top() >= 532 && r.size() == QSize(300, 100) && screen.contains(r));

        r = Tooltips::placeBeside(QRect(0, 0, 300, 700), QPoint(500, 900), screen);
        CHECK(r.bottom() < 900 && r.size() == QSize(300, 700) && screen.contains(r));

        r = Tooltips::placeBeside(QRect(0, 0, 300, 1000), QPoint(500, 540), screen);
        CHECK(r.left() >= 532 && r.height() == 1000 && screen.contains(r));
        CHECK(!r.contains(QPoint(500, 540)));

        r = Tooltips::placeBeside(QRect(0, 0, 300, 1000), QPoint(1800, 540), screen);
        CHECK(r.right() < 1800 && screen.contains(r));
        CHECK(!r.contains(QPoint(1800, 540)));

        const QRect small(0, 0, 800, 600);
        r = Tooltips::placeBeside(QRect(0, 0, 600, 900), QPoint(400, 300), small);
        CHECK(r.height() < 900 && small.contains(r));
        CHECK(!r.contains(QPoint(400, 300)));

        r = Tooltips::placeBeside(QRect(0, 0, 3000, 100), QPoint(400, 300), small);
        CHECK(r.width() == 800 && small.contains(r));

        // A screen that does not start at the origin (second monitor).
        const QRect second(1920, 0, 1920, 1080);
        r = Tooltips::placeBeside(QRect(0, 0, 400, 2000), QPoint(1930, 540), second);
        CHECK(second.contains(r) && !r.contains(QPoint(1930, 540)));
    }

    // ---- The live presenter, offscreen -----------------------------------
    Tooltips::Presenter *presenter = Tooltips::install();
    CHECK(presenter != nullptr);
    CHECK(Tooltips::install() == presenter);
    CHECK(!QApplication::isEffectEnabled(Qt::UI_FadeTooltip));
    CHECK(!QApplication::isEffectEnabled(Qt::UI_AnimateTooltip));

    QWidget popup(nullptr, Qt::ToolTip);
    CHECK(!Tooltips::Presenter::isQtTooltipWindow(&popup));

    QWidget host;
    host.setStyleSheet(QStringLiteral(
        "QLabel { background: transparent; color: #ff0000; }"
        "QWidget { background: transparent; }"));
    auto *child = new QWidget(&host);
    child->setGeometry(10, 10, 100, 100);
    const QRect area = QGuiApplication::primaryScreen()->availableGeometry();
    host.setGeometry(QRect(QPoint(0, 0), QSize(400, 300)).translated(
        area.center() - QPoint(200, 150)));
    host.show();
    pump(50);
    const QPoint pointer = host.mapToGlobal(host.rect().center());
    QCursor::setPos(pointer);
    pump(20);
    if (QCursor::pos() != pointer) {
        std::cout << "tooltip tests: platform cannot move the pointer; "
                     "skipping the live presenter checks\n";
    } else {
        QString huge;
        for (int i = 0; i < 400; ++i)
            huge += QStringLiteral("row %1 of a very tall tooltip\n").arg(i);
        QToolTip::showText(pointer, huge, &host);
        CHECK(waitFor([&] { return presenter->visibleWindow() != nullptr; }));
        QWidget *tip = presenter->visibleWindow();
        describe("tall tip", tip, presenter->placements());
        CHECK(!qtTipOnScreen());
        if (tip) {
            CHECK(!tip->frameGeometry().contains(QCursor::pos()));
            CHECK(area.contains(tip->frameGeometry()));
        }
        QToolTip::hideText();
        CHECK(waitFor([&] { return presenter->visibleWindow() == nullptr; }));

        QString longLine;
        for (int i = 0; i < 80; ++i)
            longLine += QStringLiteral("word%1 ").arg(i);
        longLine += QString(300, QLatin1Char('Q'));
        QToolTip::showText(pointer, longLine, &host);
        CHECK(waitFor([&] { return presenter->visibleWindow() != nullptr; }));
        tip = presenter->visibleWindow();
        describe("long line", tip, presenter->placements());
        if (tip) {
            CHECK(tip->width() <= Tooltips::Window::kMaxWidth);
            CHECK(tip->height() > 4 * Tooltips::Window::kFontPixels);
            CHECK(!tip->frameGeometry().contains(QCursor::pos()));
            const QImage image = tip->grab().toImage();
            const QColor base = QGuiApplication::palette().color(QPalette::ToolTipBase);
            const QColor corner = image.pixelColor(3, 3);
            CHECK(corner.alpha() == 255);
            CHECK(corner.rgb() == base.rgb());
            CHECK(image.pixelColor(image.width() - 4, image.height() - 4).alpha() == 255);
        }

        const QString steadyText = QStringLiteral("steady tip");
        QToolTip::showText(pointer, steadyText, &host);
        CHECK(waitFor([&] {
            return presenter->visibleWindow()
                && presenter->visibleWindow()->text() == steadyText;
        }));
        tip = presenter->visibleWindow();
        const QRect steady = tip ? tip->geometry() : QRect();
        const int placed = presenter->placements();
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(child, &leave);
        QEvent focusIn(QEvent::FocusIn);
        QCoreApplication::sendEvent(child, &focusIn);
        QEvent activate(QEvent::WindowActivate);
        QCoreApplication::sendEvent(&popup, &activate);
        pump(600);
        describe("after stray events", presenter->visibleWindow(), presenter->placements());
        CHECK(presenter->visibleWindow() == tip);
        CHECK(tip && tip->geometry() == steady);
        CHECK(QToolTip::isVisible());
        QToolTip::showText(pointer + QPoint(5, 5), steadyText, &host);
        pump(50);
        CHECK(presenter->placements() == placed);
        CHECK(tip && tip->geometry() == steady);

        QToolTip::hideText();
        CHECK(waitFor([] { return !QToolTip::isVisible(); }));
        QToolTip::showText(pointer + QPoint(3, 0), steadyText, &host);
        pump(300);
        describe("re-created", presenter->visibleWindow(), presenter->placements());
        CHECK(presenter->visibleWindow() == tip);
        CHECK(presenter->placements() == placed);
        CHECK(tip && tip->geometry() == steady);

        QToolTip::showText(pointer, QStringLiteral("another tip"), &host);
        CHECK(waitFor([&] { return presenter->placements() == placed + 1; }));
        CHECK(presenter->visibleWindow()
              && presenter->visibleWindow()->text() == QStringLiteral("another tip"));

        QMouseEvent press(QEvent::MouseButtonPress, QPointF(20, 20),
                          QPointF(child->mapToGlobal(QPoint(20, 20))),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(child, &press);
        CHECK(presenter->visibleWindow() == nullptr);
        CHECK(waitFor([] { return !QToolTip::isVisible(); }));

        QToolTip::showText(pointer, steadyText, &host);
        CHECK(waitFor([&] { return presenter->visibleWindow() != nullptr; }));
        QCursor::setPos(host.mapToGlobal(QPoint(-40, -40)));
        QEvent hostLeave(QEvent::Leave);
        QCoreApplication::sendEvent(&host, &hostLeave);
        CHECK(waitFor([&] { return presenter->visibleWindow() == nullptr; }));
        describe("pointer left", presenter->visibleWindow(), presenter->placements());
    }

    if (failures) {
        std::cerr << failures << " tooltip check(s) failed\n";
        return 1;
    }
    std::cout << "tooltip tests passed\n";
    return 0;
}
