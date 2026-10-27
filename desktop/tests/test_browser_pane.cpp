#include "WorkspaceBrowserPane.h"

#include <QApplication>
#include <QPushButton>

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

QPushButton *installButton(QWidget &pane)
{
    const auto buttons = pane.findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
        if (button->text() == QStringLiteral("Install embedded browser"))
            return button;
    return nullptr;
}

void testInstallButton()
{
    using forkmesh::ui::WorkspaceBrowserPane;
    WorkspaceBrowserPane pane;
    QPushButton *install = installButton(pane);
    check(install != nullptr, "the placeholder has an install button");
    if (!install)
        return;
    check(!install->isVisibleTo(&pane), "no install button without a handler");

    int calls = 0;
    pane.setInstallHandler([&calls] { ++calls; });
    const bool installable = WorkspaceBrowserPane::embeddedInstallable();
    check(install->isVisibleTo(&pane) == installable,
          "the install button shows only when the build lacks WebEngine");
    if (installable) {
        install->click();
        check(calls == 1, "clicking install calls the handler");
    }

    pane.navigate(QStringLiteral("example.com"));
    check(install->isVisibleTo(&pane) == installable,
          "a page opened outside the app still offers the install");

    pane.setInstallHandler({});
    check(!install->isVisibleTo(&pane), "clearing the handler hides the button");
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    testInstallButton();
    if (failures == 0)
        std::printf("browser pane tests passed\n");
    return failures == 0 ? 0 : 1;
}
