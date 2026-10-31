#include "BusyRing.h"

#include <QApplication>
#include <QCheckBox>
#include <QImage>
#include <QLineEdit>
#include <QPushButton>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

using namespace forkmesh::ui;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (ok) {
        std::printf("PASS: %s\n", what);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

QWidget *overlayOf(QWidget *host)
{
    return host->findChild<QWidget *>(QStringLiteral("fmBusyRing"),
                                      Qt::FindDirectChildrenOnly);
}

void settle()
{
    for (int i = 0; i < 4; ++i)
        QCoreApplication::processEvents();
}

QIcon squareIcon()
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::black);
    return QIcon(pixmap);
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    busyring::installClickWatcher();

    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *working = new QPushButton(squareIcon(), QStringLiteral("Sync"));
    auto *quick = new QPushButton(squareIcon(), QStringLiteral("Open"));
    auto *deferred = new QPushButton(QStringLiteral("Save"));
    auto *toggle = new QCheckBox(QStringLiteral("Option"));
    auto *checkable = new QPushButton(QStringLiteral("Pin"));
    auto *message = new QLineEdit;
    auto *send = new QPushButton(squareIcon(), QStringLiteral("Send"));
    send->setEnabled(false);
    QObject::connect(message, &QLineEdit::textChanged, send,
                     [send](const QString &text) { send->setEnabled(!text.isEmpty()); });
    QObject::connect(send, &QPushButton::clicked, message, [message] { message->clear(); });
    checkable->setCheckable(true);
    for (QWidget *widget : {static_cast<QWidget *>(working), static_cast<QWidget *>(quick),
                            static_cast<QWidget *>(deferred), static_cast<QWidget *>(toggle),
                            static_cast<QWidget *>(checkable), static_cast<QWidget *>(message),
                            static_cast<QWidget *>(send)})
        layout->addWidget(widget);
    QObject::connect(working, &QPushButton::clicked, working,
                     [working] { working->setEnabled(false); });
    QObject::connect(deferred, &QPushButton::clicked, deferred, [deferred] {
        QTimer::singleShot(0, deferred, [deferred] { deferred->setEnabled(false); });
    });
    QObject::connect(toggle, &QCheckBox::clicked, toggle,
                     [toggle] { toggle->setEnabled(false); });
    QObject::connect(checkable, &QPushButton::clicked, checkable,
                     [checkable] { checkable->setEnabled(false); });
    window.resize(240, 220);
    window.show();
    QTest::qWaitForWindowExposed(&window);

    int notified = 0;
    QObject context;
    busyring::observe(&context, [&notified] { ++notified; });

    QTest::mouseClick(working, Qt::LeftButton);
    settle();
    check(busyring::isBusy(working) && overlayOf(working),
          "a button that disables itself on click shows the ring");
    check(notified > 0, "observers hear that a button became busy");
    const QRectF icon = busyring::iconRect(working);
    check(!icon.isEmpty() && icon.width() == working->iconSize().width() &&
              icon.left() > 0 && icon.right() < working->width() / 2.0 + 1,
          "the ring is placed around the icon left of the caption");
    check(overlayOf(working)->geometry() == working->rect(),
          "the overlay covers the button");
    working->resize(working->width() + 30, working->height());
    settle();
    check(overlayOf(working)->geometry() == working->rect(),
          "the overlay follows the button's size");

    working->setEnabled(true);
    settle();
    check(!busyring::isBusy(working) && !overlayOf(working),
          "enabling the button again removes the ring");

    QTest::mouseClick(quick, Qt::LeftButton);
    settle();
    check(!busyring::isBusy(quick), "a click that leaves the button enabled shows no ring");

    QTest::mouseClick(deferred, Qt::LeftButton);
    settle();
    check(busyring::isBusy(deferred) && overlayOf(deferred),
          "work a handler defers by one turn still counts");
    check(busyring::iconRect(deferred).isEmpty(),
          "a button without an icon gets the outline sweep");

    QTest::mouseClick(toggle, Qt::LeftButton);
    QTest::mouseClick(checkable, Qt::LeftButton);
    settle();
    check(!busyring::isBusy(toggle) && !busyring::isBusy(checkable),
          "checkboxes and checkable buttons never ring");

    window.activateWindow();
    QTest::qWaitForWindowActive(&window);
    message->setFocus();
    message->setText(QStringLiteral("hello"));
    settle();
    QTest::mouseClick(send, Qt::LeftButton);
    settle();
    check(message->text().isEmpty() && !send->isEnabled() && !busyring::isBusy(send),
          "a send button disabled because its message box was cleared does not ring");

    auto *post = new QPushButton(squareIcon(), QStringLiteral("Post"), &window);
    post->show();
    QObject::connect(post, &QPushButton::clicked, post, [post] { post->setEnabled(false); });
    message->setFocus();
    message->setText(QStringLiteral("draft"));
    settle();
    QTest::mouseClick(post, Qt::LeftButton);
    settle();
    check(busyring::isBusy(post), "a post that is still sending rings");
    message->clear();
    settle();
    check(!busyring::isBusy(post) && !post->isEnabled(),
          "the ring stops when the sent text is cleared even though the button stays disabled");

    auto *self = new QPushButton(QStringLiteral("Rail"), &window);
    self->setProperty("fmBusyPaintsSelf", true);
    busyring::setBusy(self, true);
    check(busyring::isBusy(self) && !overlayOf(self),
          "a widget that paints its own ring gets only the property");
    busyring::setBusy(self, false);
    check(!busyring::isBusy(self), "clearing busy clears the property");

    auto *tool = new QToolButton(&window);
    tool->setIcon(squareIcon());
    tool->setIconSize(QSize(16, 16));
    tool->resize(28, 28);
    const QRectF toolIcon = busyring::iconRect(tool);
    check(toolIcon.center() == QRectF(tool->rect()).center(),
          "an icon-only tool button rings its centred icon");

    QImage image(40, 40, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        busyring::paint(painter, QRectF(4, 4, 32, 32), Qt::blue, 0.25);
    }
    int lit = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qAlpha(image.pixel(x, y)) > 0)
                ++lit;
    check(lit > 40 && qAlpha(image.pixel(20, 20)) == 0,
          "the ring paints a stroke and leaves the icon area clear");

    if (failures == 0)
        std::printf("\nAll busy ring tests passed.\n");
    else
        std::fprintf(stderr, "\n%d busy ring test(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
