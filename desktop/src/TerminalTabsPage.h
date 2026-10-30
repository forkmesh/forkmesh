#pragma once

#include <QHash>
#include <QIcon>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QTabWidget;
class QToolButton;
class TerminalWidget;

namespace forkmesh::ui {

class TerminalTabsPage : public QWidget
{
public:
    struct Spec {
        QString id;
        QString kind;
        QString command;
        QString cwd;
        QString title;
        QString openedBy;
        QStringList env;
        bool keepShell = false;
        bool shared = true;
    };

    struct Info {
        QString id;
        QString kind;
        QString title;
        QString cwd;
        QString command;
        QString openedBy;
        bool running = false;
        bool shared = true;
        bool current = false;
        int exitCode = -1;
        int cols = 0;
        int rows = 0;
        qint64 startedAtMs = 0;
        qint64 lastOutputMs = 0;
    };

    struct Icons {
        QIcon add;
        QIcon more;
        QIcon hide;
    };

    explicit TerminalTabsPage(QWidget *parent = nullptr);
    ~TerminalTabsPage() override;

    void setNewTabHandler(std::function<void(const QString &kind)> handler);
    void setClosedHandler(std::function<void(const QString &)> handler);
    void setHideHandler(std::function<void()> handler);
    void setIcons(const Icons &icons);
    void setEmptyText(const QString &text);
    void focusCurrent();

    QString openTab(const Spec &spec);
    bool closeTab(const QString &id);
    bool focusTab(const QString &id);
    void setShared(const QString &id, bool shared);
    void noteAgentAction(const QString &id, const QString &text);

    int count() const;
    QStringList ids() const;
    QList<Info> tabs() const;
    bool info(const QString &id, Info *out) const;
    TerminalWidget *terminal(const QString &id) const;
    QString currentId() const;

private:
    struct Tab {
        Info info;
        QPointer<TerminalWidget> terminal;
        QString agentAction;
    };

    int indexOf(const QString &id) const;
    QString idAt(int index) const;
    void syncChrome();
    void retitle(const QString &id);

    QTabWidget *m_tabs = nullptr;
    QPushButton *m_new = nullptr;
    QToolButton *m_kinds = nullptr;
    QPushButton *m_hide = nullptr;
    QPushButton *m_share = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_empty = nullptr;
    QHash<QString, Tab> m_byId;
    std::function<void(const QString &)> m_newTab;
    std::function<void(const QString &)> m_closed;
    std::function<void()> m_hideHandler;
};

}
