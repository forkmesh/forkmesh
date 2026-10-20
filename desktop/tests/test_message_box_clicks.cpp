#include "MessageBoxClicks.h"

#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QStringList directButtonClickedConnections(const QString &root)
{
    static const QRegularExpression direct(
        QStringLiteral("connect\\s*\\([^;]*&QMessageBox::buttonClicked"));
    QStringList hits;
    QDirIterator it(root, {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.endsWith(QLatin1String("/MessageBoxClicks.h")))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (direct.match(QString::fromUtf8(file.readAll())).hasMatch())
            hits.append(path);
    }
    return hits;
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    auto *box = new QMessageBox(QMessageBox::Question, QStringLiteral("Remove"),
                                QStringLiteral("Remove it?"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    QPushButton *remove =
        box->addButton(QStringLiteral("Remove"), QMessageBox::DestructiveRole);
    box->addButton(QMessageBox::Cancel);
    QPointer<QMessageBox> guarded(box);
    QObject context;
    int calls = 0;
    bool sawRemove = false;
    bool visibleInHandler = true;
    forkmesh::ui::onMessageBoxClicked(
        box, &context, [&](QAbstractButton *clicked) {
            ++calls;
            sawRemove = clicked == remove;
            visibleInHandler = guarded && guarded->isVisible();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            delete guarded.data();
        });
    box->show();
    QApplication::processEvents();

    remove->click();
    check(calls == 0,
          "the handler does not run inside QMessageBox's own click handling");
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    check(calls == 1, "the handler runs once on the next event-loop turn");
    check(sawRemove, "the handler receives the clicked button");
    check(!visibleInHandler,
          "the box has already finished when the handler runs");
    check(guarded.isNull(),
          "a handler may delete the box or pump deferred deletes safely");

#ifdef FORKMESH_SOURCE_DIR
    const QStringList direct =
        directButtonClickedConnections(QStringLiteral(FORKMESH_SOURCE_DIR "/src")) +
        directButtonClickedConnections(
            QStringLiteral(FORKMESH_SOURCE_DIR "/../marketplace/packages"));
    for (const QString &path : direct)
        std::fprintf(stderr, "direct QMessageBox::buttonClicked: %s\n",
                     qPrintable(path));
    check(direct.isEmpty(),
          "every QMessageBox::buttonClicked handler goes through "
          "forkmesh::ui::onMessageBoxClicked");
#endif

    if (failures == 0)
        std::printf("PASS: message box clicks\n");
    return failures == 0 ? 0 : 1;
}
