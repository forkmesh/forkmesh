#pragma once

#include <QIcon>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

namespace forkmesh::ui {

class WorkspaceBrowserPane : public QWidget
{
public:
    struct Icons {
        QIcon back;
        QIcon forward;
        QIcon reload;
        QIcon external;
    };

    explicit WorkspaceBrowserPane(QWidget *parent = nullptr);

    static bool embeddedAvailable();
    static bool embeddedInstallable();
    static QUrl urlFromInput(const QString &text);

    void navigate(const QString &text);
    QUrl currentUrl() const;
    bool showsEmbeddedView() const;
    void focusAddress();
    void setIcons(const Icons &icons);
    void setInstallHandler(std::function<void()> handler);

private:
    void ensureView();
    void openExternally();
    void showPlaceholder(const QString &title, const QString &body,
                         bool offerExternal, bool offerInstall = false);
    void syncInstallButton();
    void syncNavigation();

    QLineEdit *m_address = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_forward = nullptr;
    QPushButton *m_reload = nullptr;
    QPushButton *m_external = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_placeholder = nullptr;
    QLabel *m_placeholderTitle = nullptr;
    QLabel *m_placeholderBody = nullptr;
    QPushButton *m_placeholderOpen = nullptr;
    QPushButton *m_placeholderInstall = nullptr;
    std::function<void()> m_installHandler;
    bool m_placeholderOffersInstall = false;
    QWidget *m_view = nullptr;
    QUrl m_url;
};

}
