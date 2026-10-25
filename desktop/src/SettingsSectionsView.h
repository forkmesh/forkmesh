#pragma once

#include <QIcon>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QListWidget;
class QScrollArea;
class QTimer;
class QToolButton;
class QVBoxLayout;

// Settings as one scrolling page. Every section is stacked in order under its
// own heading; the list on the left jumps to a section and follows the one
// being read, and the search box above it narrows the page to the sections
// that mention every word typed, marking each matching control (Enter steps
// through them).
//
// It keeps the QTabWidget vocabulary its callers already used -- count,
// currentIndex, setCurrentIndex, tabText, widget, currentChanged -- so saved
// navigation places and "open Settings at Voice" links keep working.
class SettingsSectionsView : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsSectionsView(QWidget *parent = nullptr);

    // Appends a section; `body` is reparented into the page.
    int addSection(QWidget *body, const QString &title,
                   const QString &iconName = QString());
    int addLazySection(std::function<QWidget *()> build, const QString &title,
                       const QString &iconName = QString());
    bool isBuilt(int index) const;
    bool isExpanded(int index) const;
    void setExpanded(int index, bool expanded);
    QStringList collapsedTitles() const;
    void setCollapsedTitles(const QStringList &titles);
    int count() const { return int(m_sections.size()); }
    QString tabText(int index) const;
    QWidget *widget(int index) const; // the section's body
    QString sectionIconName(int index) const;
    void setSectionIcon(int index, const QIcon &icon);
    void setSectionHidden(int index, bool hidden);
    bool isSectionHidden(int index) const;

    // The section at the top of the page.
    int currentIndex() const { return m_current; }
    // Scrolls `index` to the top of the page. A section the search is hiding
    // clears the search first.
    void setCurrentIndex(int index);
    // Scrolls to the section holding `target`, then brings `target` itself
    // into view once the page has laid out (e.g. one ping kind's row).
    void revealWidget(QWidget *target);

    QStringList subSectionTitles(int index) const;
    int currentSubSection() const { return m_currentSub; }
    QWidget *litHeading() const { return m_litHeading; }
    void setCurrentSubSection(int index, int sub);

    // Ctrl+F on the Settings page lands here (MainWindow::openPageFind).
    void focusSearch();
    QString searchText() const;
    // Applies at once, without the typing delay.
    void setSearchText(const QString &text);
    // Titles of the sections the search leaves on the page.
    QStringList visibleSectionTitles() const;
    // Controls the search marked, in page order.
    QList<QWidget *> searchHits() const;
    // The hit Enter last moved to, or -1.
    int currentHit() const { return m_hitCursor; }
    void nextSearchHit();

    // A scroll area borrowed from another page (the node profile panel) would
    // scroll inside this one. Flattened, it drops its own scroll bar and grows
    // to its content; unflattened, it is restored for its home page.
    static void setScrollAreaFlattened(QScrollArea *area, bool flattened);

signals:
    // The section at the top of the page changed, by scrolling or a jump.
    void currentChanged(int index);
    // The person picked a section: a click in the list, or Enter in search.
    void sectionActivated(int index);
    // A section scrolled into view (again, after having left it).
    void sectionRevealed(int index);
    void expandedChanged(int index, bool expanded);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct SubSection {
        QPointer<QWidget> anchor;
        QPointer<QWidget> label;
        QString title;
    };

    struct Section {
        QWidget *frame = nullptr; // heading + body
        QWidget *body = nullptr;
        QWidget *header = nullptr;
        QToolButton *toggle = nullptr;
        QLabel *icon = nullptr;
        QLabel *titleLabel = nullptr;
        std::function<QWidget *()> build;
        QString title;
        QString iconName;
        QVector<SubSection> subs;
        bool expanded = true;
        bool hidden = false;
        bool inView = false;
        bool subsDirty = true;
    };

    int appendSection(QWidget *body, std::function<QWidget *()> build,
                      const QString &title, const QString &iconName);
    void ensureBuilt(int index);
    void applyExpanded(int index);
    bool searching() const;
    void applySearch();
    void scheduleRefresh();
    void refreshGeometry();
    void updateCurrentFromScroll();
    void updateRevealed();
    void scrollToSection(int index);
    void scrollToWidget(QWidget *target);
    void setCurrent(int index);
    void syncNavRow();
    void collectSubSections(int index);
    QVector<SubSection> visibleSubSections(int index);
    void updateCurrentSub();
    void syncNavSubs();
    void syncLitHeading();
    bool handleNavMouse(QEvent *event);
    void buildPendingAbove(int index);

    QLineEdit *m_search = nullptr;
    QListWidget *m_nav = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_content = nullptr;
    QVBoxLayout *m_sectionsLayout = nullptr;
    QWidget *m_bottomSpacer = nullptr;
    QLabel *m_noMatches = nullptr;
    QWidget *m_hitOverlay = nullptr;
    QTimer *m_searchTimer = nullptr;
    QVector<Section> m_sections;
    QList<QPointer<QWidget>> m_hits;
    int m_hitCursor = -1;
    int m_current = -1;
    int m_currentSub = -1;
    int m_navSubRow = -1;
    int m_navHoverRow = -1;
    int m_navHoverSub = -2;
    int m_pendingJump = -1;
    QPointer<QWidget> m_pendingReveal;
    QPointer<QWidget> m_litHeading;
    bool m_refreshQueued = false;
    bool m_jumping = false;
    bool m_syncingNav = false;
};
