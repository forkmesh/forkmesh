#pragma once

#include "DocsLibrary.h"
#include "NativePackageExtension.h"

#include <QList>
#include <QSize>
#include <QString>

class QEvent;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QObject;
class QPushButton;
class QResizeEvent;
class QSplitter;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QUrl;
class QWidget;

namespace forkmesh::docs {

class DocsFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit DocsFeature(const forkmesh::market::NativePackageHost &host,
                         QWidget *parent = nullptr);

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override;

    QSize minimumSizeHint() const override;

    QString currentPath() const { return m_current; }
    void openPage(const QString &path, const QString &fragment = QString(),
                  bool record = true);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Visit {
        QString path;
        QString fragment;
    };

    void buildPage();
    void load();
    void renderNavigation();
    void selectInNavigation();
    void renderDocument(const DocsPage &page);
    void renderBreadcrumb();
    void renderOutline();
    void renderFooter();
    void syncDesign();
    void fitPage();
    void fitPager();
    void fitScrollBars();
    void scrollToFragment(const QString &fragment);
    void followLink(const QUrl &url);
    void goBack();
    void goForward();
    void updateHistoryButtons();
    QString lastPageKey() const;

    QString m_packageId;
    DocsLibrary m_library;
    QString m_current;
    QString m_filter;
    QList<Visit> m_back;
    QList<Visit> m_forward;
    bool m_loaded = false;
    bool m_designSynced = false;
    bool m_next = false;
    bool m_dark = true;
    bool m_classicHover = false;
    bool m_classicTracking = false;
    bool m_outlineHover = false;
    bool m_outlineTracking = false;
    int m_classicIndentation = 20;
    QString m_previousLabel;
    QString m_nextLabel;

    QLabel *m_summary = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QTreeWidget *m_navigation = nullptr;
    QLabel *m_breadcrumb = nullptr;
    QTextBrowser *m_view = nullptr;
    QListWidget *m_outline = nullptr;
    QLabel *m_backlinks = nullptr;
    QSplitter *m_splitter = nullptr;
    QWidget *m_readerPane = nullptr;
    QWidget *m_outlinePane = nullptr;
    QHBoxLayout *m_pager = nullptr;
    QPushButton *m_backButton = nullptr;
    QPushButton *m_forwardButton = nullptr;
    QPushButton *m_webButton = nullptr;
    QPushButton *m_previousButton = nullptr;
    QPushButton *m_nextButton = nullptr;
};

}
