#pragma once

#include <QList>
#include <QString>

class MainWindow;
class QHBoxLayout;
class QMenu;
class QPushButton;
class QWidget;

namespace forkmesh::ui {

class SessionSidePanels;
enum class LayoutMode;

int railColumnWidth();

struct NextAccess {
    static void install(MainWindow &window);
    static void themeChanged(MainWindow &window);
    static void addThemeHook(void (*hook)(MainWindow &));
    static void showSection(MainWindow &window, int index);
    static int currentSection(const MainWindow &window);
    static int sectionCount(const MainWindow &window);
    static void setRailExpanded(MainWindow &window, bool expanded);
    static void toggleRailExpanded(MainWindow &window);
    static QWidget *overlayParent(MainWindow &window);
    static QWidget *railToggleButton(MainWindow &window);
    static QWidget *railResizeHandle(MainWindow &window);
    static void setRailWidth(MainWindow &window, int width, bool remember);
    static QWidget *buildSidebarHeader(MainWindow &window);
    static QWidget *buildSidebarFooter(MainWindow &window);
    static void refreshSidebarChrome(MainWindow &window);
    static void installTopBar(MainWindow &window, QHBoxLayout *row,
                              QWidget *history, QWidget *before,
                              const QList<QPushButton *> &windowButtons);
    static void showPingsPopover(MainWindow &window, QWidget *anchor);

    static void installWelcome(MainWindow &window);
    static bool maybeShowWelcome(MainWindow &window);
    static void showWelcome(MainWindow &window);
    static void openSignIn(MainWindow &window);
    static void openCreateAccount(MainWindow &window);
    static bool promptInstanceAccount(MainWindow &window,
                                      const QString &suggestedAccount,
                                      const QString &suggestedInstance,
                                      QString *account, QString *instance);
    static bool runLoginPanel(MainWindow &window, const QString &accountName);
    static bool accountNotice(MainWindow &window, const QString &title,
                              const QString &text, bool error);

    static void installCommandPalette(MainWindow &window);
    static void showCommandPalette(MainWindow &window, bool refresh = false);

    static void installWorkspace(MainWindow &window);
    static void syncWorkspace(MainWindow &window);
    static void applyLayoutMode(MainWindow &window, LayoutMode mode);
    static void workspaceCommand(MainWindow &window, int command);
    static void workspaceSessionStarted(MainWindow &window, int sessionId);
    static void fillSessionSidePanels(MainWindow &window, SessionSidePanels *panels,
                                      int sessionId);
    static void feedSessionSidePanels(MainWindow &window, SessionSidePanels *panels,
                                      int sessionId);
    static void sessionStarted(MainWindow &window, int sessionId);
    static void fillWorkspaceMenu(MainWindow &window, QMenu *menu, int which);
    static void placeWorkspaceComposer(MainWindow &window, bool overDetail);
};

}
