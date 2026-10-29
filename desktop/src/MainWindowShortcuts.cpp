
#include "LocalControl.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QProcessEnvironment>

#ifndef Q_OS_WIN
#include <csignal>
#endif

using namespace forkmesh::ui;

namespace {

bool shortcutIsScript(const QString &filePath)
{
    const QFileInfo info(filePath);
    const QString suffix = info.suffix().toLower();
    if (suffix == QLatin1String("sh") || suffix == QLatin1String("bash"))
        return true;
    return suffix.isEmpty() && info.isExecutable();
}

QString shortcutKind(const QString &filePath)
{
    if (shortcutIsScript(filePath))
        return QStringLiteral("script");
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (suffix == QLatin1String("skill"))
        return QStringLiteral("skill");
    if (suffix == QLatin1String("md") || suffix == QLatin1String("txt") ||
        suffix == QLatin1String("prompt"))
        return QStringLiteral("prompt");
    return QStringLiteral("file");
}

void trimAgentShortcutUses(QHash<int, QSet<QString>> &uses)
{
    while (uses.size() > 64) {
        const QList<int> ids = uses.keys();
        uses.remove(*std::min_element(ids.cbegin(), ids.cend()));
    }
}

struct ShortcutMeta
{
    QString name;
    QString description;
    QString agent;
    QString model;
    QList<forkmesh::localcontrol::insights::ShortcutStep> steps;
};

ShortcutMeta shortcutMeta(const QString &filePath)
{
    namespace insightctl = forkmesh::localcontrol::insights;
    constexpr qint64 kMaxShortcutReadBytes = 64 * 1024;
    ShortcutMeta meta;
    const QString fileName = QFileInfo(filePath).fileName();
    meta.name = fileName;
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return meta;
    const QString text = QString::fromUtf8(file.read(kMaxShortcutReadBytes));
    const insightctl::ShortcutMeta parsed = insightctl::parseShortcutMeta(fileName, text);
    meta.name = parsed.name;
    meta.description = parsed.description;
    meta.agent = parsed.agent;
    meta.model = parsed.model;
    if (shortcutIsScript(filePath))
        meta.steps = insightctl::parseShortcutSteps(text);
    return meta;
}

QString shortcutPromptText(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QString text;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine());
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QLatin1String("# name:")) ||
            trimmed.startsWith(QLatin1String("# description:")) ||
            trimmed.startsWith(QLatin1String("# agent:")) ||
            trimmed.startsWith(QLatin1String("# model:")))
            continue;
        text += line;
    }
    return text.trimmed();
}
// One row of the merged shortcuts+skills list. Both sources are read into
// this shape first so a single sort can order them by launch count.
struct PanelEntry
{
    QString path;
    QString title;
    QString description;
    QString kind;  // script | prompt | file | skill
    QString scope; // "shortcut", or the skill scope for skills
    ShortcutMeta meta;
    bool isSkill = false;
    int uses = 0;
    QDateTime added;
    QDateTime modified;
    bool addedRecorded = true;
};

void stampFileTimes(PanelEntry &entry, const QFileInfo &info)
{
    entry.modified = info.lastModified();
    entry.added = info.birthTime();
    entry.addedRecorded = entry.added.isValid();
    if (!entry.addedRecorded)
        entry.added = entry.modified;
}

enum ShortcutTableColumn {
    ShortcutRunButtonColumn,
    ShortcutNameColumn,
    ShortcutTypeColumn,
    ShortcutScopeColumn,
    ShortcutRunsColumn,
    ShortcutSuccessColumn,
    ShortcutLastRunColumn,
    ShortcutTookColumn,
    ShortcutAddedColumn,
    ShortcutModifiedColumn,
    ShortcutFileColumn,
    ShortcutDescriptionColumn,
    ShortcutColumnCount
};

constexpr int kShortcutPathRole = Qt::UserRole + 1;
constexpr int kShortcutScopeRole = Qt::UserRole + 2;
constexpr int kShortcutIsSkillRole = Qt::UserRole + 3;
constexpr int kShortcutActionRole = Qt::UserRole + 4;
constexpr int kShortcutLinksRole = Qt::UserRole + 5;
constexpr int kMaxCardStepRows = 6;

QString shortcutStepsTooltip(const QList<forkmesh::localcontrol::insights::ShortcutStep> &steps)
{
    QStringList lines;
    for (int index = 0; index < steps.size(); ++index)
        lines << (steps.at(index).shortcut.isEmpty()
                      ? QStringLiteral("%1. %2").arg(index + 1).arg(steps.at(index).name)
                      : QStringLiteral("%1. %2 → %3")
                            .arg(index + 1)
                            .arg(steps.at(index).name, steps.at(index).shortcut));
    return lines.join(QLatin1Char('\n'));
}

QStringList shortcutStepLinks(const QList<forkmesh::localcontrol::insights::ShortcutStep> &steps)
{
    QStringList links;
    for (const auto &step : steps)
        if (!step.shortcut.isEmpty() && !links.contains(step.shortcut))
            links << step.shortcut;
    return links;
}

QTableWidgetItem *shortcutTimeItem(const QDateTime &when)
{
    auto *item = new SortTableWidgetItem(
        when.isValid() ? when.toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                       : QStringLiteral("—"));
    item->setData(kTableSortRole, when.isValid() ? when.toMSecsSinceEpoch() : 0);
    if (when.isValid())
        item->setToolTip(formatIssueRelativeTime(when.toMSecsSinceEpoch()));
    return item;
}

QString shortcutProgressText(qint64 elapsedMs, qint64 expectedMs)
{
    if (expectedMs <= 0)
        return QStringLiteral("Running for %1 · no earlier run to estimate from")
            .arg(formatDuration(elapsedMs));
    if (elapsedMs < expectedMs)
        return QStringLiteral("%1 of about %2 · %3 left")
            .arg(formatDuration(elapsedMs), formatDuration(expectedMs),
                 formatDuration(expectedMs - elapsedMs));
    return QStringLiteral("%1 · %2 longer than the last run")
        .arg(formatDuration(elapsedMs), formatDuration(elapsedMs - expectedMs));
}

void setShortcutProgress(QProgressBar *bar, QLabel *label, qint64 elapsedMs,
                         qint64 expectedMs, bool waiting)
{
    if (expectedMs > 0) {
        bar->setRange(0, 1000);
        bar->setValue(waiting ? 0 : int(qMin<qint64>(990, elapsedMs * 1000 / expectedMs)));
    } else {
        bar->setRange(0, 0);
    }
    bar->setToolTip(expectedMs > 0
                        ? QStringLiteral("Estimated from the last run, which took %1")
                              .arg(formatDuration(expectedMs))
                        : QStringLiteral("The bar estimates the time left once a "
                                         "run has finished and been timed."));
    if (label)
        label->setText(waiting ? QStringLiteral("Waiting to start")
                               : shortcutProgressText(elapsedMs, expectedMs));
}

class ShortcutCard final : public QPushButton
{
public:
    explicit ShortcutCard(QWidget *parent = nullptr) : QPushButton(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    }

    QSize sizeHint() const override
    {
        return layout() ? layout()->sizeHint() : QPushButton::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        return layout() ? layout()->minimumSize() : QPushButton::minimumSizeHint();
    }
};

class ShortcutCardGrid final : public QWidget
{
public:
    explicit ShortcutCardGrid(QWidget *parent = nullptr) : QWidget(parent)
    {
        m_grid = new QGridLayout(this);
        m_grid->setContentsMargins(0, 0, 0, 0);
        m_grid->setHorizontalSpacing(10);
        m_grid->setVerticalSpacing(10);
    }

    void clearCards()
    {
        for (QWidget *card : std::as_const(m_cards))
            card->deleteLater();
        m_cards.clear();
        while (QLayoutItem *item = m_grid->takeAt(0))
            delete item;
        m_columns = 0;
    }

    QWidget *cardForFile(const QString &fileName) const
    {
        for (QWidget *card : m_cards)
            if (card->property("shortcutFile").toString() == fileName)
                return card;
        return nullptr;
    }

    void addCard(QWidget *card)
    {
        card->setParent(this);
        m_cards.append(card);
        reflow(true);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        reflow(false);
    }

private:
    int columnsForWidth() const
    {
        return qBound(1, (width() + m_grid->horizontalSpacing()) /
                             (320 + m_grid->horizontalSpacing()),
                      4);
    }

    void reflow(bool force)
    {
        const int columns = columnsForWidth();
        if (!force && columns == m_columns)
            return;
        m_columns = columns;
        while (QLayoutItem *item = m_grid->takeAt(0))
            delete item; // widgets stay alive; only the layout items go
        for (int i = 0; i < m_cards.size(); ++i)
            m_grid->addWidget(m_cards.at(i), i / columns, i % columns);
        for (int column = 0; column < 4; ++column)
            m_grid->setColumnStretch(column, column < columns ? 1 : 0);
        const int rows = (m_cards.size() + columns - 1) / columns;
        for (int row = 0; row < m_lastRowCount; ++row)
            m_grid->setRowStretch(row, 0);
        m_grid->setRowStretch(rows, 1);
        m_lastRowCount = rows + 1;
    }

    QGridLayout *m_grid = nullptr;
    QList<QWidget *> m_cards;
    int m_columns = 0;
    int m_lastRowCount = 0;
};

class ShortcutStepList final : public QWidget
{
public:
    using Step = forkmesh::localcontrol::insights::ShortcutStep;

    ShortcutStepList(const QList<Step> &steps, const QHash<QString, QString> &titles,
                     int maxRows, QWidget *parent = nullptr)
        : QWidget(parent), m_steps(steps), m_titles(titles), m_maxRows(qMax(2, maxRows))
    {
        setObjectName(QStringLiteral("shortcutStepList"));
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setToolTip(QStringLiteral("%1 step%2\n%3")
                       .arg(m_steps.size())
                       .arg(m_steps.size() == 1 ? QString() : QStringLiteral("s"),
                            shortcutStepsTooltip(m_steps)));
    }

    enum class Progress { None, Running, Succeeded, Failed, Stopped };

    std::function<void(const QString &)> onLinkClicked;

    void setProgress(Progress progress, int current)
    {
        if (progress == m_progress && current == m_current)
            return;
        m_progress = progress;
        m_current = current;
        update();
    }

    QSize sizeHint() const override { return QSize(220, rowCount() * rowHeight() + 2); }
    QSize minimumSizeHint() const override { return QSize(80, rowCount() * rowHeight() + 2); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor muted = Theme::nextOr(Theme::Role::FgMuted, QColor(Theme::kTextTertiary));
        QColor line = muted;
        line.setAlpha(140);
        const QColor text = Theme::nextOr(Theme::Role::Fg, palette().color(QPalette::Text));
        const QColor link = Theme::nextOr(Theme::Role::Accent, palette().color(QPalette::Link));
        const QColor success = Theme::nextOr(Theme::Role::Success, QColor(QStringLiteral("#3fb950")));
        const QColor danger = Theme::nextOr(Theme::Role::Danger, QColor(QStringLiteral("#f85149")));
        const int rows = rowCount();
        const int height = rowHeight();
        painter.setPen(QPen(line, 1.2));
        for (int row = 0; row + 1 < rows; ++row)
            painter.drawLine(QPointF(kDotX, row * height + height / 2.0 + kDotRadius + 1.5),
                             QPointF(kDotX, (row + 1) * height + height / 2.0 - kDotRadius - 1.5));
        for (int row = 0; row < rows; ++row) {
            const QPointF centre(kDotX, row * height + height / 2.0);
            if (row >= shownSteps()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(muted);
                for (int dot = -1; dot <= 1; ++dot)
                    painter.drawEllipse(QPointF(kDotX, centre.y() + dot * 3.5), 1.1, 1.1);
                const StepState hidden =
                    m_current >= shownSteps() && m_current < m_steps.size()
                        ? stepState(m_current)
                        : StepState::Pending;
                const QString rest = QStringLiteral("+%1 more").arg(m_steps.size() - shownSteps());
                const QString label =
                    hidden == StepState::Pending || hidden == StepState::Done
                        ? rest
                        : QStringLiteral("%1 · step %2: %3")
                              .arg(rest)
                              .arg(m_current + 1)
                              .arg(m_steps.at(m_current).name);
                painter.setPen(hidden == StepState::Active   ? link
                               : hidden == StepState::Failed ? danger
                                                             : muted);
                painter.drawText(rowTextRect(row), Qt::AlignLeft | Qt::AlignVCenter,
                                 fontMetrics().elidedText(label, Qt::ElideRight,
                                                          rowTextRect(row).width()));
                continue;
            }
            const Step &step = m_steps.at(row);
            const bool linked = !step.shortcut.isEmpty();
            const StepState state = stepState(row);
            if (state == StepState::Done) {
                painter.setPen(QPen(success, 1.4));
                painter.setBrush(success);
                painter.drawEllipse(centre, kDotRadius, kDotRadius);
            } else if (state == StepState::Active || state == StepState::Failed ||
                       state == StepState::Halted) {
                const QColor tone = state == StepState::Active   ? link
                                    : state == StepState::Failed ? danger
                                                                 : muted;
                QColor halo = tone;
                halo.setAlpha(70);
                painter.setPen(Qt::NoPen);
                painter.setBrush(halo);
                painter.drawEllipse(centre, kDotRadius + 2.5, kDotRadius + 2.5);
                painter.setPen(QPen(tone, 1.4));
                painter.setBrush(tone);
                painter.drawEllipse(centre, kDotRadius, kDotRadius);
            } else {
                painter.setPen(QPen(linked ? link : muted, 1.4));
                painter.setBrush(linked ? QBrush(link) : QBrush(Qt::NoBrush));
                painter.drawEllipse(centre, kDotRadius, kDotRadius);
            }
            QRect nameRect;
            QRect linkRect;
            layoutRow(row, &nameRect, &linkRect);
            QFont nameFont = font();
            nameFont.setBold(state == StepState::Active || state == StepState::Failed);
            painter.setFont(nameFont);
            painter.setPen(state == StepState::Failed ? danger : text);
            painter.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(nameFont).elidedText(step.name, Qt::ElideRight,
                                                               nameRect.width()));
            painter.setFont(font());
            if (!linked || linkRect.width() <= 0)
                continue;
            QFont linkFont = font();
            linkFont.setUnderline(row == m_hoverRow);
            painter.setFont(linkFont);
            painter.setPen(link);
            painter.drawText(linkRect, Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(linkFont).elidedText(linkText(step), Qt::ElideRight,
                                                               linkRect.width()));
            painter.setFont(font());
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int row = linkRowAt(event->position().toPoint());
        if (row != m_hoverRow) {
            m_hoverRow = row;
            update();
        }
        QWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (m_hoverRow >= 0) {
            m_hoverRow = -1;
            update();
        }
        QWidget::leaveEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressedRow = event->button() == Qt::LeftButton
                           ? linkRowAt(event->position().toPoint())
                           : -1;
        if (m_pressedRow < 0) {
            event->ignore();
            return;
        }
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int pressed = m_pressedRow;
        m_pressedRow = -1;
        if (pressed < 0) {
            event->ignore();
            return;
        }
        event->accept();
        if (pressed == linkRowAt(event->position().toPoint()) && onLinkClicked)
            onLinkClicked(m_steps.at(pressed).shortcut);
    }

private:
    enum class StepState { Pending, Done, Active, Failed, Halted };

    StepState stepState(int row) const
    {
        if (m_progress == Progress::None)
            return StepState::Pending;
        if (m_progress == Progress::Succeeded)
            return StepState::Done;
        if (row < m_current)
            return StepState::Done;
        if (row > m_current)
            return StepState::Pending;
        return m_progress == Progress::Running  ? StepState::Active
               : m_progress == Progress::Failed ? StepState::Failed
                                                : StepState::Halted;
    }

    static constexpr int kDotX = 6;
    static constexpr qreal kDotRadius = 3.5;
    static constexpr int kTextX = 16;

    int rowHeight() const { return fontMetrics().height() + 5; }

    int shownSteps() const
    {
        return m_steps.size() > m_maxRows ? m_maxRows - 1 : int(m_steps.size());
    }

    int rowCount() const { return qMin(int(m_steps.size()), m_maxRows); }

    QRect rowTextRect(int row) const
    {
        return QRect(kTextX, row * rowHeight(), qMax(0, width() - kTextX), rowHeight());
    }

    QString linkText(const Step &step) const
    {
        const QString title = m_titles.value(step.shortcut);
        return QStringLiteral("→ %1").arg(title.isEmpty() ? step.shortcut : title);
    }

    void layoutRow(int row, QRect *nameRect, QRect *linkRect) const
    {
        const QRect area = rowTextRect(row);
        const Step &step = m_steps.at(row);
        if (step.shortcut.isEmpty()) {
            *nameRect = area;
            *linkRect = QRect();
            return;
        }
        constexpr int kGap = 8;
        QFont linkFont = font();
        linkFont.setUnderline(true);
        const int linkWanted = QFontMetrics(linkFont).horizontalAdvance(linkText(step)) + 2;
        const int nameWanted = fontMetrics().horizontalAdvance(step.name) + 2;
        const int available = qMax(0, area.width() - kGap);
        int linkWidth = qMin(linkWanted, qMax(available / 2, available - nameWanted));
        const int nameWidth = qMin(nameWanted, available - linkWidth);
        linkWidth = qMin(linkWanted, available - nameWidth);
        *nameRect = QRect(area.left(), area.top(), nameWidth, area.height());
        *linkRect = QRect(area.left() + nameWidth + kGap, area.top(), linkWidth, area.height());
    }

    int linkRowAt(const QPoint &point) const
    {
        const int row = point.y() / qMax(1, rowHeight());
        if (point.y() < 0 || row >= shownSteps() || m_steps.at(row).shortcut.isEmpty())
            return -1;
        QRect nameRect;
        QRect linkRect;
        layoutRow(row, &nameRect, &linkRect);
        return linkRect.contains(point) ? row : -1;
    }

    QList<Step> m_steps;
    QHash<QString, QString> m_titles;
    int m_maxRows = 6;
    Progress m_progress = Progress::None;
    int m_current = -1;
    int m_hoverRow = -1;
    int m_pressedRow = -1;
};

ShortcutStepList::Progress shortcutStepProgress(
    const forkmesh::localcontrol::insights::ShortcutRun &run)
{
    if (run.running)
        return ShortcutStepList::Progress::Running;
    if (run.ok)
        return ShortcutStepList::Progress::Succeeded;
    return run.stopped ? ShortcutStepList::Progress::Stopped : ShortcutStepList::Progress::Failed;
}

QString shortcutRunStatusText(const forkmesh::localcontrol::insights::ShortcutRun &run)
{
    if (!run.running)
        return QStringLiteral("%1 — %2").arg(run.file, run.message);
    if (run.step < 0 || run.step >= run.steps.size())
        return QStringLiteral("running %1…").arg(run.file);
    return QStringLiteral("running %1… step %2 of %3: %4")
        .arg(run.file)
        .arg(run.step + 1)
        .arg(run.steps.size())
        .arg(run.steps.at(run.step).name);
}

bool locateShortcutFile(QStackedWidget *stack, QTableWidget *table, ShortcutCardGrid *grid,
                        const QString &file)
{
    if (stack && table && stack->currentWidget() == table) {
        for (int row = 0; row < table->rowCount(); ++row) {
            const QTableWidgetItem *item = table->item(row, ShortcutNameColumn);
            if (!item || item->data(kShortcutIsSkillRole).toBool() ||
                QFileInfo(item->data(kShortcutPathRole).toString()).fileName() != file)
                continue;
            table->selectRow(row);
            table->scrollToItem(item, QAbstractItemView::PositionAtCenter);
            return true;
        }
        return false;
    }
    QWidget *card = grid ? grid->cardForFile(file) : nullptr;
    if (!card)
        return false;
    const QColor accent = Theme::nextOr(Theme::Role::Accent, card->palette().color(QPalette::Link));
    card->setStyleSheet(QStringLiteral("QPushButton#shortcutCard { border: 2px solid %1; }")
                            .arg(accent.name()));
    card->setProperty("shortcutHighlighted", true);
    QTimer::singleShot(0, card, [card] {
        for (QWidget *parent = card->parentWidget(); parent; parent = parent->parentWidget())
            if (auto *area = qobject_cast<QScrollArea *>(parent)) {
                area->ensureWidgetVisible(card, 0, 24);
                break;
            }
    });
    QTimer::singleShot(1800, card, [card] {
        card->setStyleSheet(QString());
        card->setProperty("shortcutHighlighted", false);
    });
    return true;
}

void revealShortcutFile(QObject *context, QStackedWidget *stack, QTableWidget *table,
                        ShortcutCardGrid *grid, QLineEdit *search, QComboBox *filter,
                        std::function<void()> reload,
                        std::function<void(const QString &)> notice, const QString &file)
{
    QPointer<QStackedWidget> stackGuard(stack);
    QPointer<QTableWidget> tableGuard(table);
    QPointer<QLineEdit> searchGuard(search);
    QPointer<QComboBox> filterGuard(filter);
    QPointer<ShortcutCardGrid> gridGuard(grid);
    QTimer::singleShot(0, context, [=] {
        if (locateShortcutFile(stackGuard, tableGuard, gridGuard, file))
            return;
        const bool narrowed =
            (searchGuard && !searchGuard->text().isEmpty()) ||
            (filterGuard && filterGuard->currentData().toString() == QLatin1String("skills"));
        if (narrowed) {
            if (searchGuard) {
                const QSignalBlocker blocker(searchGuard.data());
                searchGuard->clear();
            }
            if (filterGuard) {
                const QSignalBlocker blocker(filterGuard.data());
                filterGuard->setCurrentIndex(
                    qMax(0, filterGuard->findData(QStringLiteral("all"))));
            }
            reload();
            if (locateShortcutFile(stackGuard, tableGuard, gridGuard, file))
                return;
        }
        notice(QStringLiteral("%1 is not in this repository's .forkmesh/shortcuts folder.")
                   .arg(file));
    });
}

} // namespace

QString MainWindow::shortcutsDirPath() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    if (repo.previewOnly || repo.localPath.isEmpty())
        return {};
    return repo.localPath + QStringLiteral("/.forkmesh/shortcuts");
}

QStringList MainWindow::missingStarterShortcuts() const
{
    const QString dirPath = shortcutsDirPath();
    if (dirPath.isEmpty())
        return {};
    QStringList missing;
    const QStringList names =
        QDir(QStringLiteral(":/starter-shortcuts")).entryList(QDir::Files, QDir::Name);
    for (const QString &name : names) {
        if (!QFileInfo::exists(QDir(dirPath).filePath(name)))
            missing << name;
    }
    return missing;
}

void MainWindow::addStarterShortcuts()
{
    const QString dirPath = shortcutsDirPath();
    if (dirPath.isEmpty()) {
        setRepoDetailNotice(
            "This is a read-only mirror; shortcuts can't be added here.", true);
        return;
    }
    const QStringList missing = missingStarterShortcuts();
    if (missing.isEmpty() || !QDir().mkpath(dirPath)) {
        loadShortcutsPanel();
        return;
    }
    QStringList added;
    for (const QString &name : missing) {
        QFile source(QStringLiteral(":/starter-shortcuts/") + name);
        if (!source.open(QIODevice::ReadOnly))
            continue;
        const QByteArray text = source.readAll();
        QSaveFile out(QDir(dirPath).filePath(name));
        if (!out.open(QIODevice::WriteOnly) || out.write(text) != text.size() ||
            !out.commit())
            continue;
        added << name;
    }
    if (added.isEmpty())
        setRepoDetailNotice(
            QStringLiteral("Couldn't write the starter shortcuts to %1.").arg(dirPath), true);
    else
        setRepoDetailNotice(
            QStringLiteral("Added %1 starter shortcut%2 to .forkmesh/shortcuts. Commit "
                           "them to share them with everyone who uses this repository.")
                .arg(added.size())
                .arg(added.size() == 1 ? QString() : QStringLiteral("s")),
            added.size() != missing.size());
    loadShortcutsPanel();
}

QWidget *MainWindow::buildShortcutsTab()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(10);

    auto *heading = new QLabel("Shortcuts & Skills");
    heading->setObjectName("channelTitle");
    auto *subtitle = new QLabel(
        "One list of everything repeatable here: shortcuts you run and skills "
        "your special agents install. The ones you and your agents use most often come first.");
    subtitle->setObjectName("statusLine");
    subtitle->setWordWrap(true);
    outer->addWidget(heading);
    outer->addWidget(subtitle);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    m_shortcutsSummary = new QLabel;
    m_shortcutsSummary->setObjectName("statusLine");
    auto *newButton = new QPushButton("New shortcut");
    newButton->setObjectName("primaryButton");
    newButton->setCursor(Qt::PointingHandCursor);
    setOcticon(newButton, "plus", 16);
    connect(newButton, &QPushButton::clicked, this,
            [this] { openShortcutEditor(QString()); });
    auto *newSkill = new QPushButton(QStringLiteral("New skill"));
    newSkill->setObjectName(QStringLiteral("ghostButton"));
    newSkill->setCursor(Qt::PointingHandCursor);
    setOcticon(newSkill, QStringLiteral("plus"), 16);
    connect(newSkill, &QPushButton::clicked, this,
            [this] { openSkillEditor(); });
    m_shortcutsStarterButton = new QPushButton(QStringLiteral("Add starter shortcuts"));
    m_shortcutsStarterButton->setObjectName(QStringLiteral("ghostButton"));
    m_shortcutsStarterButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_shortcutsStarterButton, QStringLiteral("download"), 16);
    m_shortcutsStarterButton->setVisible(false);
    connect(m_shortcutsStarterButton, &QPushButton::clicked, this,
            &MainWindow::addStarterShortcuts);
    auto *refreshButton = new QPushButton("Refresh");
    refreshButton->setObjectName("ghostButton");
    refreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(refreshButton, "sync", 16);
    connect(refreshButton, &QPushButton::clicked, this,
            &MainWindow::loadShortcutsPanel);
    addRefreshSpin(refreshButton);
    headerRow->addWidget(m_shortcutsSummary);
    headerRow->addStretch();
    headerRow->addWidget(m_shortcutsStarterButton);
    headerRow->addWidget(newButton);
    headerRow->addWidget(newSkill);
    headerRow->addWidget(refreshButton);
    outer->addLayout(headerRow);

    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    m_shortcutsSearch = new QLineEdit;
    m_shortcutsSearch->setObjectName(QStringLiteral("shortcutsSearch"));
    m_shortcutsSearch->setPlaceholderText(
        QStringLiteral("Search shortcuts, skills and descriptions"));
    m_shortcutsSearch->setClearButtonEnabled(true);
    m_shortcutsFilter = new QComboBox;
    m_shortcutsFilter->setObjectName(QStringLiteral("shortcutsFilter"));
    m_shortcutsFilter->addItem(QStringLiteral("Shortcuts & skills"),
                               QStringLiteral("all"));
    m_shortcutsFilter->addItem(QStringLiteral("Shortcuts only"),
                               QStringLiteral("shortcuts"));
    m_shortcutsFilter->addItem(QStringLiteral("Skills only"),
                               QStringLiteral("skills"));
    m_shortcutsFilter->setToolTip(
        QStringLiteral("Show both kinds together, or narrow the list to just "
                       "shortcuts or just skills."));
    m_skillsScope = new QComboBox;
    m_skillsScope->setObjectName(QStringLiteral("skillsScope"));
    m_skillsScope->addItem(QStringLiteral("Repository skills"),
                           QStringLiteral("repository"));
    m_skillsScope->addItem(QStringLiteral("App skills"), QStringLiteral("app"));
    m_skillsScope->addItem(QStringLiteral("Organization skills"),
                           QStringLiteral("organization"));
    m_skillsScope->setToolTip(
        QStringLiteral("Which skill scope this list includes, and where a new "
                       "skill is saved. Repository skills version with this "
                       "code. App skills are available everywhere on this "
                       "device. Organization skills are shared across "
                       "repositories for this owner."));
    m_shortcutsView = new QComboBox;
    m_shortcutsView->setObjectName(QStringLiteral("shortcutsView"));
    m_shortcutsView->addItem(QStringLiteral("Cards"), QStringLiteral("cards"));
    m_shortcutsView->addItem(QStringLiteral("Table"), QStringLiteral("table"));
    m_shortcutsView->setToolTip(
        QStringLiteral("Cards show each entry with its run history; the table "
                       "fits one row per entry and sorts by any column, "
                       "including when each file was added and last "
                       "modified."));
    const int savedView = m_shortcutsView->findData(
        QSettings().value(kShortcutViewSetting).toString());
    if (savedView >= 0)
        m_shortcutsView->setCurrentIndex(savedView);
    filterRow->addWidget(m_shortcutsSearch, 1);
    filterRow->addWidget(m_shortcutsFilter);
    filterRow->addWidget(m_skillsScope);
    filterRow->addWidget(m_shortcutsView);
    outer->addLayout(filterRow);

    auto *hint = new QLabel(
        "Shortcuts are scripts and prompts in this repo's .forkmesh/shortcuts/ "
        "folder; skills are reusable instruction packages special agents install "
        "from the Special Agents page. Click a script card to run it (bash, from the "
        "repo root) — its output streams below. While a script runs, a card's queue "
        "button lines it up to start once the running ones finish. A prompt or skill card fills "
        "the composer with its text, ready to send to an agent; one with "
        "# agent: and # model: starts that dedicated agent in one click.");
    hint->setObjectName("statusLine");
    hint->setWordWrap(true);
    outer->addWidget(hint);

    m_shortcutCardsHost = new ShortcutCardGrid;
    // Card titles share one rule here rather than a sheet per card on reload.
    m_shortcutCardsHost->setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QLabel#shortcutCardTitle "
        "{ font-weight:600; font-size:14px; background:transparent; }"
        "#shortcutStepList { font-size:12px; }")));
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_shortcutCardsHost);

    m_shortcutsTable = new QTableWidget(0, ShortcutColumnCount);
    m_shortcutsTable->setObjectName(QStringLiteral("shortcutsTable"));
    enableHoverRowHighlight(m_shortcutsTable);
    m_shortcutsTable->setHorizontalHeaderLabels(
        {QString(), QStringLiteral("Name"), QStringLiteral("Type"), QStringLiteral("Scope"),
         QStringLiteral("Runs"), QStringLiteral("Success"),
         QStringLiteral("Last run"), QStringLiteral("Took"),
         QStringLiteral("Added"),
         QStringLiteral("Modified"), QStringLiteral("File"),
         QStringLiteral("Description")});
    m_shortcutsTable->verticalHeader()->setVisible(false);
    m_shortcutsTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_shortcutsTable->verticalHeader()->setDefaultSectionSize(
        m_shortcutsTable->fontMetrics().height() + 10);
    m_shortcutsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_shortcutsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_shortcutsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_shortcutsTable->setShowGrid(false);
    m_shortcutsTable->setWordWrap(false);
    m_shortcutsTable->setTextElideMode(Qt::ElideRight);
    m_shortcutsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_shortcutsTable->setToolTip(
        QStringLiteral("Click a row's play button, or double-click the row, to run or "
                       "draft it, the same as clicking its card. Right-click to queue, "
                       "edit or delete. Click a column header to sort."));
    QHeaderView *tableHeader = m_shortcutsTable->horizontalHeader();
    tableHeader->setHighlightSections(false);
    tableHeader->setSectionsClickable(true);
    tableHeader->setSectionResizeMode(QHeaderView::ResizeToContents);
    tableHeader->setSectionResizeMode(ShortcutRunButtonColumn, QHeaderView::Fixed);
    m_shortcutsTable->setColumnWidth(ShortcutRunButtonColumn, 34);
    tableHeader->setSectionResizeMode(ShortcutNameColumn, QHeaderView::Interactive);
    m_shortcutsTable->setColumnWidth(ShortcutNameColumn, 240);
    tableHeader->setSectionResizeMode(ShortcutDescriptionColumn,
                                      QHeaderView::Stretch);
    tableHeader->setSortIndicator(ShortcutRunsColumn, Qt::DescendingOrder);
    m_shortcutsTable->setSortingEnabled(true);

    const auto activateShortcutRow = [this](int row) {
        const QTableWidgetItem *item =
            m_shortcutsTable->item(row, ShortcutNameColumn);
        if (!item)
            return;
        const QString path = item->data(kShortcutPathRole).toString();
        const QString action = item->data(kShortcutActionRole).toString();
        if (action == QLatin1String("run"))
            runShortcut(path);
        else if (action == QLatin1String("draft"))
            draftShortcutPrompt(path, item->data(kShortcutScopeRole).toString());
        else
            openShortcutEditor(path);
    };
    connect(m_shortcutsTable, &QTableWidget::cellActivated, this,
            [activateShortcutRow](int row, int) { activateShortcutRow(row); });
    connect(m_shortcutsTable, &QWidget::customContextMenuRequested, this,
            [this, activateShortcutRow](const QPoint &pos) {
                const int row = m_shortcutsTable->rowAt(pos.y());
                const QTableWidgetItem *item =
                    row >= 0 ? m_shortcutsTable->item(row, ShortcutNameColumn)
                             : nullptr;
                if (!item)
                    return;
                const QString path = item->data(kShortcutPathRole).toString();
                const bool isSkill = item->data(kShortcutIsSkillRole).toBool();
                const QString action = item->data(kShortcutActionRole).toString();
                QMenu menu(m_shortcutsTable);
                const QStringList links = item->data(kShortcutLinksRole).toStringList();
                if (action == QLatin1String("run"))
                    menu.addAction(QStringLiteral("Run"),
                                   [activateShortcutRow, row] {
                                       activateShortcutRow(row);
                                   });
                if (action == QLatin1String("run") && shortcutQueuePosition(path) > 0)
                    menu.addAction(QStringLiteral("Remove from queue"),
                                   [this, path] { unqueueShortcut(path); });
                else if (action == QLatin1String("run") &&
                         shortcutRepoBusy(shortcutRepoKey(m_repoDetailIndex)))
                    menu.addAction(QStringLiteral("Queue after the running shortcuts"),
                                   [this, path] { queueShortcut(path); });
                else if (action == QLatin1String("draft"))
                    menu.addAction(QStringLiteral("Draft in composer"),
                                   [activateShortcutRow, row] {
                                       activateShortcutRow(row);
                                   });
                if (!links.isEmpty()) {
                    menu.addSeparator();
                    for (const QString &link : links)
                        menu.addAction(QStringLiteral("Go to linked %1").arg(link),
                                       [this, link] {
                                           revealShortcutFile(
                                               this, m_shortcutsViewStack, m_shortcutsTable,
                                               static_cast<ShortcutCardGrid *>(
                                                   m_shortcutCardsHost),
                                               m_shortcutsSearch, m_shortcutsFilter,
                                               [this] { loadShortcutsPanel(); },
                                               [this](const QString &message) {
                                                   setRepoDetailNotice(message, true);
                                               },
                                               link);
                                       });
                    menu.addSeparator();
                }
                menu.addAction(isSkill ? QStringLiteral("Edit skill")
                                       : QStringLiteral("Edit shortcut"),
                               [this, path, isSkill] {
                                   if (isSkill)
                                       openSkillEditor(path);
                                   else
                                       openShortcutEditor(path);
                               });
                menu.addAction(isSkill ? QStringLiteral("Delete skill")
                                       : QStringLiteral("Delete shortcut"),
                               [this, path, isSkill] {
                                   if (isSkill)
                                       deleteSkill(path);
                                   else
                                       deleteShortcut(path);
                               });
                menu.exec(m_shortcutsTable->viewport()->mapToGlobal(pos));
            });

    m_shortcutsViewStack = new QStackedWidget;
    m_shortcutsViewStack->addWidget(scroll);
    m_shortcutsViewStack->addWidget(m_shortcutsTable);
    outer->addWidget(m_shortcutsViewStack, 2);

    auto *outputRow = new QHBoxLayout;
    outputRow->setContentsMargins(0, 0, 0, 0);
    auto *outputHeading = new QLabel("Output");
    outputHeading->setStyleSheet(Theme::remapSheet("font-weight:600;background:transparent;"));
    m_shortcutRunStatus = new QLabel;
    m_shortcutRunStatus->setObjectName("statusLine");
    m_shortcutStopButton = new QPushButton("Stop");
    m_shortcutStopButton->setObjectName("ghostButton");
    m_shortcutStopButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_shortcutStopButton, "stop", 14);
    m_shortcutStopButton->hide();
    connect(m_shortcutStopButton, &QPushButton::clicked, this,
            &MainWindow::stopShortcut);
    m_shortcutRunPicker = new QComboBox;
    m_shortcutRunPicker->setObjectName(QStringLiteral("shortcutRunPicker"));
    m_shortcutRunPicker->setToolTip(QStringLiteral("Which run's output to show"));
    m_shortcutRunPicker->hide();
    connect(m_shortcutRunPicker, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index >= 0)
            showShortcutRun(m_shortcutRunPicker->itemData(index).toInt());
    });
    outputRow->addWidget(outputHeading);
    outputRow->addWidget(m_shortcutRunPicker);
    outputRow->addWidget(m_shortcutRunStatus);
    m_shortcutRunProgress = new QProgressBar;
    m_shortcutRunProgress->setObjectName(QStringLiteral("shortcutRunProgress"));
    m_shortcutRunProgress->setTextVisible(false);
    m_shortcutRunProgress->setFixedSize(120, 4);
    m_shortcutRunProgress->hide();
    m_shortcutRunEta = new QLabel;
    m_shortcutRunEta->setObjectName(QStringLiteral("shortcutRunEta"));
    m_shortcutRunEta->hide();
    outputRow->addWidget(m_shortcutRunProgress, 0, Qt::AlignVCenter);
    outputRow->addWidget(m_shortcutRunEta);
    outputRow->addStretch();
    m_shortcutQueueLabel = new QLabel;
    m_shortcutQueueLabel->setObjectName(QStringLiteral("shortcutQueueLabel"));
    m_shortcutQueueLabel->setToolTip(
        QStringLiteral("Queued shortcuts start one at a time, in this order, once the "
                       "shortcuts running in their repository finish."));
    m_shortcutQueueClear = new QPushButton(QStringLiteral("Clear queue"));
    m_shortcutQueueClear->setObjectName(QStringLiteral("ghostButton"));
    m_shortcutQueueClear->setProperty("buttonSize", "sm");
    m_shortcutQueueClear->setCursor(Qt::PointingHandCursor);
    setOcticon(m_shortcutQueueClear, QStringLiteral("x"), 14);
    connect(m_shortcutQueueClear, &QPushButton::clicked, this, &MainWindow::clearShortcutQueue);
    outputRow->addWidget(m_shortcutQueueLabel);
    outputRow->addWidget(m_shortcutQueueClear);
    refreshShortcutQueueLabel();
    outputRow->addWidget(m_shortcutStopButton);
    outer->addLayout(outputRow);

    m_shortcutOutput = new QPlainTextEdit;
    m_shortcutOutput->setObjectName("networkLog");
    m_shortcutOutput->setReadOnly(true);
    m_shortcutOutput->setMaximumBlockCount(5000);
    m_shortcutOutput->setPlaceholderText("Run a shortcut to see its output here.");
    outer->addWidget(m_shortcutOutput, 1);
    showShortcutRun(m_shortcutShownRun);

    connect(m_shortcutsFilter, &QComboBox::currentIndexChanged, this,
            [this] { loadShortcutsPanel(); });
    connect(m_skillsScope, &QComboBox::currentIndexChanged, this,
            [this] { loadShortcutsPanel(); });
    connect(m_shortcutsSearch, &QLineEdit::textChanged, this,
            [this] { loadShortcutsPanel(); });
    connect(m_shortcutsView, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue(kShortcutViewSetting,
                             m_shortcutsView->currentData().toString());
        loadShortcutsPanel();
    });

    return page;
}

QString MainWindow::shortcutUsageKey(const QString &filePath,
                                     const QString &scope, int repoIndex) const
{
    const QString file = QFileInfo(filePath).fileName();
    if (file.isEmpty())
        return {};
    // App and organization skills follow the device, so their tallies are
    // global. Repository entries are namespaced by repo: two checkouts both
    // carrying run-tests.sh are different shortcuts and count separately.
    if (scope == QLatin1String("app") || scope == QLatin1String("organization"))
        return scope + QLatin1Char(':') + file;
    QString repo = QStringLiteral("-");
    if (repoIndex < 0)
        repoIndex = m_repoDetailIndex;
    if (repoIndex >= 0 && repoIndex < m_repositories.size()) {
        const RepositoryRecord &record = m_repositories.at(repoIndex);
        repo = record.owner + QLatin1Char('/') + record.name;
    }
    return scope + QLatin1Char(':') + repo + QLatin1Char(':') + file;
}

int MainWindow::shortcutUsageCount(const QString &usageKey) const
{
    if (!m_shortcutUsageLoaded) {
        const QVariantMap stored =
            QSettings().value(kShortcutUsageSetting).toMap();
        for (auto it = stored.cbegin(); it != stored.cend(); ++it)
            m_shortcutUsage.insert(it.key(), it.value().toInt());
        m_shortcutUsageLoaded = true;
    }
    if (usageKey.isEmpty())
        return 0;
    return m_shortcutUsage.value(usageKey, 0);
}

QString MainWindow::recordShortcutUsage(const QString &filePath,
                                        const QString &scope, int repoIndex)
{
    const QString key = shortcutUsageKey(filePath, scope, repoIndex);
    if (key.isEmpty())
        return {};
    m_shortcutUsage.insert(key, shortcutUsageCount(key) + 1);
    QVariantMap stored;
    for (auto it = m_shortcutUsage.cbegin(); it != m_shortcutUsage.cend(); ++it)
        stored.insert(it.key(), it.value());
    ShortcutRunStats stats = shortcutRunStats(key);
    stats.lastRunAtMs = QDateTime::currentMSecsSinceEpoch();
    m_shortcutRunStats.insert(key, stats);
    QSettings settings;
    settings.setValue(kShortcutUsageSetting, stored);
    settings.setValue(kShortcutRunStatsSetting, shortcutRunStatsSettingValue());
    return key;
}

void MainWindow::forgetShortcutUsage(const QString &filePath,
                                     const QString &scope)
{
    const QString key = shortcutUsageKey(filePath, scope);
    if (key.isEmpty())
        return;
    // Priming both caches first keeps a delete from writing back a tally that
    // was never read, which would resurrect it for a same-named replacement.
    const bool counted = shortcutUsageCount(key) > 0;
    shortcutRunStats(key);
    const bool timed = m_shortcutRunStats.contains(key);
    if (!counted && !timed)
        return;
    QSettings settings;
    if (counted) {
        m_shortcutUsage.remove(key);
        QVariantMap stored;
        for (auto it = m_shortcutUsage.cbegin(); it != m_shortcutUsage.cend(); ++it)
            stored.insert(it.key(), it.value());
        settings.setValue(kShortcutUsageSetting, stored);
    }
    if (m_shortcutRunStats.remove(key) > 0)
        settings.setValue(kShortcutRunStatsSetting, shortcutRunStatsSettingValue());
}

MainWindow::ShortcutRunStats
MainWindow::shortcutRunStats(const QString &usageKey) const
{
    if (!m_shortcutRunStatsLoaded) {
        QSettings settings;
        const QVariantMap stored = settings.value(kShortcutRunStatsSetting).toMap();
        for (auto it = stored.cbegin(); it != stored.cend(); ++it) {
            const QVariantMap row = it.value().toMap();
            ShortcutRunStats stats;
            stats.lastRunAtMs = row.value(QStringLiteral("lastRunAtMs")).toLongLong();
            stats.finished = row.value(QStringLiteral("finished")).toInt();
            stats.succeeded = row.value(QStringLiteral("succeeded")).toInt();
            stats.timed = row.value(QStringLiteral("timed")).toInt();
            stats.totalMs = row.value(QStringLiteral("totalMs")).toLongLong();
            stats.lastMs = row.value(QStringLiteral("lastMs"), -1).toLongLong();
            stats.lastOk = row.value(QStringLiteral("lastOk")).toBool();
            stats.lastOutput = row.value(QStringLiteral("lastOutput")).toString();
            m_shortcutRunStats.insert(it.key(), stats);
        }
        const QVariantMap agents = settings.value(kShortcutAgentRunsSetting).toMap();
        for (auto it = agents.cbegin(); it != agents.cend(); ++it) {
            const int sessionId = it.key().toInt();
            if (sessionId > 0 && !it.value().toString().isEmpty())
                m_shortcutAgentRuns.insert(sessionId, it.value().toString());
        }
        m_shortcutRunStatsLoaded = true;
    }
    if (usageKey.isEmpty())
        return {};
    return m_shortcutRunStats.value(usageKey);
}

QVariantMap MainWindow::shortcutRunStatsSettingValue() const
{
    shortcutRunStats(QString()); // load before serializing
    QVariantMap stored;
    for (auto it = m_shortcutRunStats.cbegin(); it != m_shortcutRunStats.cend();
         ++it) {
        const ShortcutRunStats &stats = it.value();
        stored.insert(it.key(),
                      QVariantMap{
                          {QStringLiteral("lastRunAtMs"), stats.lastRunAtMs},
                          {QStringLiteral("finished"), stats.finished},
                          {QStringLiteral("succeeded"), stats.succeeded},
                          {QStringLiteral("timed"), stats.timed},
                          {QStringLiteral("totalMs"), stats.totalMs},
                          {QStringLiteral("lastMs"), stats.lastMs},
                          {QStringLiteral("lastOk"), stats.lastOk},
                          {QStringLiteral("lastOutput"), stats.lastOutput},
                      });
    }
    return stored;
}

void MainWindow::recordShortcutRunResult(const QString &usageKey, bool ok,
                                         qint64 durationMs)
{
    if (usageKey.isEmpty())
        return;
    ShortcutRunStats stats = shortcutRunStats(usageKey);
    ++stats.finished;
    stats.lastOk = ok;
    if (ok)
        ++stats.succeeded;
    if (durationMs >= 0) {
        ++stats.timed;
        stats.totalMs += durationMs;
        stats.lastMs = durationMs;
    }
    m_shortcutRunStats.insert(usageKey, stats);
    QSettings().setValue(kShortcutRunStatsSetting, shortcutRunStatsSettingValue());
}

void MainWindow::trackShortcutAgentRun(int sessionId, const QString &usageKey)
{
    if (sessionId <= 0 || usageKey.isEmpty())
        return;
    shortcutRunStats(QString());
    m_agentShortcutUses[sessionId].insert(usageKey);
    trimAgentShortcutUses(m_agentShortcutUses);
    m_shortcutAgentRuns.insert(sessionId, usageKey);
    // An agent deleted before it ever finishes leaves its entry behind; keep
    // only the newest few so the setting cannot grow without bound.
    while (m_shortcutAgentRuns.size() > 64) {
        const QList<int> ids = m_shortcutAgentRuns.keys();
        m_shortcutAgentRuns.remove(*std::min_element(ids.cbegin(), ids.cend()));
    }
    QVariantMap stored;
    for (auto it = m_shortcutAgentRuns.cbegin(); it != m_shortcutAgentRuns.cend();
         ++it)
        stored.insert(QString::number(it.key()), it.value());
    QSettings().setValue(kShortcutAgentRunsSetting, stored);
    if (m_localControl)
        m_localControl->broadcast(QStringLiteral("insights.changed"));
}

void MainWindow::recordShortcutAgentFinished(int sessionId, bool ok,
                                             qint64 durationMs)
{
    shortcutRunStats(QString());
    // Only the first finish counts: a later turn in the same conversation is
    // the user's follow-up, not the shortcut's task.
    const QString key = m_shortcutAgentRuns.take(sessionId);
    if (key.isEmpty())
        return;
    QVariantMap stored;
    for (auto it = m_shortcutAgentRuns.cbegin(); it != m_shortcutAgentRuns.cend();
         ++it)
        stored.insert(QString::number(it.key()), it.value());
    QSettings().setValue(kShortcutAgentRunsSetting, stored);
    recordShortcutRunResult(key, ok, durationMs);
    loadShortcutsPanel();
    if (m_localControl)
        m_localControl->broadcast(QStringLiteral("insights.changed"));
}

void MainWindow::recordAgentShortcutUse(int sessionId, const QString &filePath,
                                        const QString &scope, int repoIndex)
{
    const QString key = shortcutUsageKey(filePath, scope, repoIndex);
    if (key.isEmpty() || !QFileInfo(filePath).isFile())
        return;
    QSet<QString> &used = m_agentShortcutUses[sessionId];
    if (used.contains(key))
        return;
    used.insert(key);
    trimAgentShortcutUses(m_agentShortcutUses);
    recordShortcutUsage(filePath, scope, repoIndex);
    loadShortcutsPanel();
}

void MainWindow::recordAgentToolShortcutUses(int sessionId,
                                             const QJsonObject &toolUse)
{
    const QString name = toolUse.value(QStringLiteral("name")).toString();
    if (name == QLatin1String("Edit") || name == QLatin1String("Write") ||
        name == QLatin1String("MultiEdit") ||
        name == QLatin1String("NotebookEdit") ||
        name == QLatin1String("FileChange"))
        return;
    const AgentSession *session = findAgentSession(sessionId);
    if (!session)
        return;
    const int repoIndex = repoIndexFor(session->owner, session->name);
    if (repoIndex < 0)
        return;
    const QString localPath = m_repositories.at(repoIndex).localPath;
    const QJsonObject input = toolUse.value(QStringLiteral("input")).toObject();

    if (name == QLatin1String("Skill")) {
        const QString skill =
            QFileInfo(input.value(QStringLiteral("skill")).toString().trimmed())
                .fileName();
        if (skill.isEmpty())
            return;
        for (const QString &scope : {QStringLiteral("repository"),
                                     QStringLiteral("app"),
                                     QStringLiteral("organization")}) {
            const QString directory = skillsDirPath(scope, repoIndex);
            if (directory.isEmpty())
                continue;
            for (const QString &suffix :
                 {QStringLiteral(".skill"), QStringLiteral(".md"),
                  QStringLiteral(".prompt"), QStringLiteral(".txt")}) {
                const QString path = QDir(directory).filePath(skill + suffix);
                if (QFileInfo(path).isFile()) {
                    recordAgentShortcutUse(sessionId, path, scope, repoIndex);
                    return;
                }
            }
        }
        return;
    }

    QStringList texts;
    for (const QString &field :
         {QStringLiteral("command"), QStringLiteral("file_path"),
          QStringLiteral("path"), QStringLiteral("cmd")}) {
        const QJsonValue value = input.value(field);
        if (value.isString())
            texts << value.toString();
        else if (value.isArray())
            for (const QJsonValue &part : value.toArray())
                texts << part.toString();
    }
    const QString text = texts.join(QLatin1Char('\n'));
    if (!text.contains(QLatin1String("shortcuts/")) &&
        !text.contains(QLatin1String("skills/")))
        return;

    const QString file = QStringLiteral("([^\\s'\"`;|&<>()/\\\\]+)");
    const QString dataRoot = QDir(forkmesh::paths::dataRoot()).absolutePath();
    const struct
    {
        QRegularExpression pattern;
        QString scope;
        QString directory;
    } sources[] = {
        {QRegularExpression(QStringLiteral("\\.forkmesh/shortcuts/") + file),
         QStringLiteral("shortcut"),
         localPath.isEmpty()
             ? QString()
             : QDir(localPath).filePath(QStringLiteral(".forkmesh/shortcuts"))},
        {QRegularExpression(QStringLiteral("\\.forkmesh/skills/") + file),
         QStringLiteral("repository"),
         skillsDirPath(QStringLiteral("repository"), repoIndex)},
        {QRegularExpression(QRegularExpression::escape(dataRoot) +
                            QStringLiteral("/skills/") + file),
         QStringLiteral("app"), skillsDirPath(QStringLiteral("app"), repoIndex)},
        {QRegularExpression(QRegularExpression::escape(dataRoot) +
                            QStringLiteral("/organization-skills/[^/\\s]+/") +
                            file),
         QStringLiteral("organization"),
         skillsDirPath(QStringLiteral("organization"), repoIndex)},
    };
    for (const auto &source : sources) {
        if (source.directory.isEmpty())
            continue;
        QRegularExpressionMatchIterator matches =
            source.pattern.globalMatch(text);
        while (matches.hasNext())
            recordAgentShortcutUse(
                sessionId,
                QDir(source.directory).filePath(matches.next().captured(1)),
                source.scope, repoIndex);
    }
}

void MainWindow::recordTeammateShortcutUses(
    int sessionId, const QList<forkmesh::teammates::Config> &teammates,
    int repoIndex)
{
    if (sessionId <= 0 || repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    QList<QPair<QString, QString>> entries;
    const QString localPath = m_repositories.at(repoIndex).localPath;
    for (const forkmesh::teammates::Config &teammate : teammates) {
        if (!teammate.shortcut.isEmpty() && !localPath.isEmpty())
            entries.append(
                {QDir(localPath).filePath(
                     QStringLiteral(".forkmesh/shortcuts/") +
                     QFileInfo(teammate.shortcut).fileName()),
                 QStringLiteral("shortcut")});
        for (const QString &ref : teammate.skillRefs) {
            const QString scope =
                ref.section(QLatin1Char(':'), 0, 0).trimmed().toLower();
            const QString fileName =
                QFileInfo(ref.section(QLatin1Char(':'), 1)).fileName();
            const QString directory = skillsDirPath(scope, repoIndex);
            if (!fileName.isEmpty() && !directory.isEmpty())
                entries.append({QDir(directory).filePath(fileName), scope});
        }
    }
    QSet<QString> counted;
    for (const auto &[path, scope] : std::as_const(entries)) {
        const QString key = shortcutUsageKey(path, scope, repoIndex);
        if (key.isEmpty() || counted.contains(key) || !QFileInfo(path).isFile())
            continue;
        recordShortcutUsage(path, scope, repoIndex);
        m_agentShortcutUses[sessionId].insert(key);
        counted.insert(key);
    }
    trimAgentShortcutUses(m_agentShortcutUses);
    if (!counted.isEmpty())
        loadShortcutsPanel();
}

void MainWindow::loadShortcutsPanel()
{
    if (!m_shortcutCardsHost)
        return;
    auto *cardsGrid = static_cast<ShortcutCardGrid *>(m_shortcutCardsHost);
    cardsGrid->clearCards();

    const QString filter = m_shortcutsFilter
                               ? m_shortcutsFilter->currentData().toString()
                               : QStringLiteral("all");
    const bool wantShortcuts = filter != QLatin1String("skills");
    const bool wantSkills = filter != QLatin1String("shortcuts");
    const QString skillScope = m_skillsScope
                                   ? m_skillsScope->currentData().toString()
                                   : QStringLiteral("repository");
    const QString search =
        m_shortcutsSearch ? m_shortcutsSearch->text().trimmed() : QString();
    if (m_skillsScope)
        m_skillsScope->setEnabled(wantSkills);

    QList<PanelEntry> entries;
    QHash<QString, QString> shortcutTitles;
    int shortcutTotal = 0;
    int skillTotal = 0;
    QStringList unavailable;

    const auto matches = [&search](const PanelEntry &entry) {
        if (search.isEmpty())
            return true;
        const QString haystack = entry.title + QLatin1Char(' ') +
                                 entry.description + QLatin1Char(' ') +
                                 QFileInfo(entry.path).fileName();
        return haystack.contains(search, Qt::CaseInsensitive);
    };

    if (wantShortcuts) {
        const QString dirPath = shortcutsDirPath();
        if (dirPath.isEmpty()) {
            unavailable << QStringLiteral("shortcuts");
        } else {
            const QFileInfoList files = QDir(dirPath).entryInfoList(
                QDir::Files | QDir::Readable, QDir::Name);
            shortcutTotal = files.size();
            for (const QFileInfo &info : files) {
                PanelEntry entry;
                entry.path = info.absoluteFilePath();
                entry.scope = QStringLiteral("shortcut");
                entry.meta = shortcutMeta(entry.path);
                entry.title = entry.meta.name;
                entry.description = entry.meta.description;
                entry.kind = shortcutKind(entry.path);
                stampFileTimes(entry, info);
                entry.uses = shortcutUsageCount(
                    shortcutUsageKey(entry.path, entry.scope));
                shortcutTitles.insert(info.fileName(), entry.title);
                if (matches(entry))
                    entries.append(entry);
            }
        }
    }

    if (wantSkills) {
        const QString directory = skillsDirPath(skillScope);
        if (directory.isEmpty()) {
            unavailable << QStringLiteral("repository skills");
        } else {
            const QFileInfoList files = QDir(directory).entryInfoList(
                {QStringLiteral("*.skill"), QStringLiteral("*.md"),
                 QStringLiteral("*.prompt"), QStringLiteral("*.txt")},
                QDir::Files | QDir::Readable, QDir::Name);
            skillTotal = files.size();
            for (const QFileInfo &info : files) {
                PanelEntry entry;
                entry.path = info.absoluteFilePath();
                entry.scope = skillScope;
                entry.isSkill = true;
                entry.meta = shortcutMeta(entry.path);
                entry.title = entry.meta.name == info.fileName()
                                  ? info.completeBaseName()
                                  : entry.meta.name;
                entry.description = entry.meta.description;
                entry.kind = QStringLiteral("skill");
                stampFileTimes(entry, info);
                entry.uses = shortcutUsageCount(
                    shortcutUsageKey(entry.path, entry.scope));
                if (matches(entry))
                    entries.append(entry);
            }
        }
    }

    // Most-launched first, then alphabetical, so a list nobody has run yet
    // reads the same as it always did instead of in directory-scan order.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const PanelEntry &a, const PanelEntry &b) {
                         if (a.uses != b.uses)
                             return a.uses > b.uses;
                         return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
                     });

    if (m_shortcutsSummary) {
        QStringList totals;
        if (wantShortcuts)
            totals << QStringLiteral("%1 shortcut%2")
                          .arg(shortcutTotal)
                          .arg(shortcutTotal == 1 ? QString()
                                                  : QStringLiteral("s"));
        if (wantSkills)
            totals << QStringLiteral("%1 skill%2")
                          .arg(skillTotal)
                          .arg(skillTotal == 1 ? QString() : QStringLiteral("s"));
        QString summary = totals.join(QStringLiteral(" · "));
        if (!search.isEmpty())
            summary = QStringLiteral("%1 of %2 matching “%3”")
                          .arg(entries.size())
                          .arg(summary, search);
        if (!unavailable.isEmpty()) {
            // Both halves fail for the same reason on a mirror, so say it once.
            const QString note =
                QStringLiteral("read-only mirror — %1 need a working tree")
                    .arg(unavailable.join(QStringLiteral(" and ")));
            summary = summary.isEmpty()
                          ? note
                          : QStringLiteral("%1 — %2").arg(summary, note);
        }
        m_shortcutsSummary->setText(summary);
    }
    if (m_shortcutsStarterButton) {
        const QString dirPath = shortcutsDirPath();
        const bool noShortcuts =
            !dirPath.isEmpty() &&
            QDir(dirPath).entryList(QDir::Files | QDir::Readable).isEmpty();
        const int missingStarters = noShortcuts ? missingStarterShortcuts().size() : 0;
        m_shortcutsStarterButton->setVisible(missingStarters > 0);
        m_shortcutsStarterButton->setToolTip(
            QStringLiteral("Copy %1 ready-made prompt shortcut%2 (review changes, UX audit, "
                           "stale-code cleanup, docs refresh, feature retirement) into "
                           "this repository's .forkmesh/shortcuts folder.")
                .arg(missingStarters)
                .arg(missingStarters == 1 ? QString() : QStringLiteral("s")));
    }

    const auto runEntryLater = [this](const QString &path, const QString &scope, bool script) {
        QTimer::singleShot(0, this, [this, path, scope, script] {
            if (script)
                runShortcut(path);
            else
                draftShortcutPrompt(path, scope);
        });
    };
    const auto revealLinkedShortcut = [this, cardsGrid](const QString &file) {
        revealShortcutFile(
            this, m_shortcutsViewStack, m_shortcutsTable, cardsGrid, m_shortcutsSearch,
            m_shortcutsFilter, [this] { loadShortcutsPanel(); },
            [this](const QString &message) { setRepoDetailNotice(message, true); }, file);
    };
    const bool tableView =
        m_shortcutsView &&
        m_shortcutsView->currentData().toString() == QLatin1String("table");
    if (m_shortcutsViewStack)
        m_shortcutsViewStack->setCurrentIndex(tableView ? 1 : 0);
    if (m_shortcutsTable) {
        m_shortcutsTable->setSortingEnabled(false);
        m_shortcutsTable->setRowCount(0);
    }
    if (tableView && m_shortcutsTable) {
        m_shortcutsTable->setRowCount(entries.size());
        for (int row = 0; row < entries.size(); ++row) {
            const PanelEntry &entry = entries.at(row);
            const bool script = !entry.isSkill && shortcutIsScript(entry.path);
            const bool prompt =
                entry.isSkill || entry.kind == QLatin1String("prompt");
            const bool dedicated = prompt && !entry.meta.agent.isEmpty();
            const ShortcutRunStats stats =
                shortcutRunStats(shortcutUsageKey(entry.path, entry.scope));
            const int successRate =
                stats.finished > 0
                    ? qRound(stats.succeeded * 100.0 / stats.finished)
                    : -1;

            auto *name = new SortTableWidgetItem(entry.title);
            name->setIcon(themedOcticon(entry.isSkill ? "sparkle"
                                        : script || dedicated
                                            ? "rocket"
                                            : prompt ? "comment" : "file",
                                        Theme::nextOr(Theme::Role::FgMuted,
                                                      entry.isSkill
                                                          ? QColor("#7c3aed")
                                                          : QColor("#2ea043")),
                                        16));
            name->setData(kShortcutPathRole, entry.path);
            name->setData(kShortcutScopeRole, entry.scope);
            name->setData(kShortcutIsSkillRole, entry.isSkill);
            name->setData(kShortcutActionRole,
                          script   ? QStringLiteral("run")
                          : prompt ? QStringLiteral("draft")
                                   : QStringLiteral("edit"));
            name->setToolTip(entry.title);
            m_shortcutsTable->setItem(row, ShortcutNameColumn, name);
            if (script || prompt) {
                auto *runButton = new QPushButton;
                runButton->setObjectName(QStringLiteral("ghostButton"));
                runButton->setProperty("buttonSize", "sm");
                runButton->setProperty("shortcutRowRun", entry.path);
                runButton->setCursor(Qt::PointingHandCursor);
                setOcticon(runButton, QStringLiteral("play"), 14);
                runButton->setToolTip(script ? QStringLiteral("Run %1").arg(entry.title)
                                      : dedicated
                                          ? QStringLiteral("Start a new %1 agent with %2")
                                                .arg(entry.meta.agent, entry.title)
                                          : QStringLiteral("Draft %1 in the composer")
                                                .arg(entry.title));
                connect(runButton, &QPushButton::clicked, this,
                        [runEntryLater, path = entry.path, scope = entry.scope, script] {
                            runEntryLater(path, scope, script);
                        });
                m_shortcutsTable->setCellWidget(row, ShortcutRunButtonColumn, runButton);
            }

            name->setData(kShortcutLinksRole, shortcutStepLinks(entry.meta.steps));
            const QString typeText =
                dedicated && !entry.meta.model.isEmpty() ? entry.meta.model : entry.kind;
            auto *type = new SortTableWidgetItem(
                entry.meta.steps.isEmpty()
                    ? typeText
                    : QStringLiteral("%1 · %2 step%3")
                          .arg(typeText)
                          .arg(entry.meta.steps.size())
                          .arg(entry.meta.steps.size() == 1 ? QString() : QStringLiteral("s")));
            if (!entry.meta.steps.isEmpty()) {
                const QString steps = shortcutStepsTooltip(entry.meta.steps);
                type->setToolTip(steps);
                name->setToolTip(entry.title + QStringLiteral("\n") + steps);
            }
            m_shortcutsTable->setItem(row, ShortcutTypeColumn, type);
            m_shortcutsTable->setItem(
                row, ShortcutScopeColumn,
                new SortTableWidgetItem(
                    !entry.isSkill ? QStringLiteral("Shortcut")
                    : entry.scope == QLatin1String("repository")
                        ? QStringLiteral("Repo")
                    : entry.scope == QLatin1String("organization")
                        ? QStringLiteral("Org")
                        : QStringLiteral("App")));

            auto *runs = new SortTableWidgetItem(QString::number(entry.uses));
            runs->setData(kTableSortRole, entry.uses);
            runs->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_shortcutsTable->setItem(row, ShortcutRunsColumn, runs);

            auto *success = new SortTableWidgetItem(
                successRate >= 0 ? QStringLiteral("%1%").arg(successRate)
                                 : QStringLiteral("—"));
            success->setData(kTableSortRole, successRate);
            success->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (successRate >= 0)
                success->setToolTip(
                    QStringLiteral("%1 of %2 finished run%3 succeeded")
                        .arg(stats.succeeded)
                        .arg(stats.finished)
                        .arg(stats.finished == 1 ? QString()
                                                 : QStringLiteral("s")));
            m_shortcutsTable->setItem(row, ShortcutSuccessColumn, success);

            QTableWidgetItem *lastRun = shortcutTimeItem(
                stats.lastRunAtMs > 0
                    ? QDateTime::fromMSecsSinceEpoch(stats.lastRunAtMs)
                    : QDateTime());
            if (stats.lastRunAtMs <= 0)
                lastRun->setText(QStringLiteral("Never"));
            m_shortcutsTable->setItem(row, ShortcutLastRunColumn, lastRun);

            auto *took = new SortTableWidgetItem(
                stats.lastMs >= 0 ? formatDuration(stats.lastMs) : QStringLiteral("—"));
            took->setData(kTableSortRole, stats.lastMs);
            took->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (stats.lastMs >= 0)
                took->setToolTip(QStringLiteral("Last run took %1, %2 on average")
                                     .arg(formatDuration(stats.lastMs),
                                          formatDuration(stats.totalMs /
                                                         qMax(1, stats.timed))));
            m_shortcutsTable->setItem(row, ShortcutTookColumn, took);
            if (QWidget *progress = activeShortcutProgress(entry.path, entry.scope,
                                                           script, dedicated, stats.lastMs,
                                                           true)) {
                took->setText(QString());
                m_shortcutsTable->setCellWidget(row, ShortcutTookColumn, progress);
            } else if (const int position = script ? shortcutQueuePosition(entry.path) : 0) {
                took->setText(QStringLiteral("queued #%1").arg(position));
                took->setToolTip(QStringLiteral("Starts once the shortcuts running ahead of it "
                                                "finish"));
            }

            QTableWidgetItem *added = shortcutTimeItem(entry.added);
            if (!entry.addedRecorded)
                added->setToolTip(
                    QStringLiteral("This file system does not record when the "
                                   "file was created; showing its last "
                                   "modification instead."));
            m_shortcutsTable->setItem(row, ShortcutAddedColumn, added);
            m_shortcutsTable->setItem(row, ShortcutModifiedColumn,
                                      shortcutTimeItem(entry.modified));

            m_shortcutsTable->setItem(
                row, ShortcutFileColumn,
                new SortTableWidgetItem(QFileInfo(entry.path).fileName()));
            auto *description = new SortTableWidgetItem(entry.description);
            description->setToolTip(entry.description);
            m_shortcutsTable->setItem(row, ShortcutDescriptionColumn,
                                      description);
        }
    }
    if (m_shortcutsTable)
        m_shortcutsTable->setSortingEnabled(true);
    if (tableView)
        return;
    const bool repoBusy = shortcutRepoBusy(shortcutRepoKey(m_repoDetailIndex));

    for (const PanelEntry &entry : std::as_const(entries)) {
        const QString path = entry.path;
        const QString scope = entry.scope;
        const bool script = !entry.isSkill && shortcutIsScript(path);
        const bool prompt = entry.isSkill || entry.kind == QLatin1String("prompt");
        const bool dedicated = prompt && !entry.meta.agent.isEmpty();

        auto *card = new ShortcutCard;
        card->setObjectName("shortcutCard");
        // What this card resolved to, so tests (and future filters) can read
        // the rendered order without reaching into the label hierarchy.
        card->setProperty("shortcutTitle", entry.title);
        card->setProperty("shortcutUses", entry.uses);
        card->setProperty("shortcutIsSkill", entry.isSkill);
        card->setProperty("shortcutStepCount", int(entry.meta.steps.size()));
        if (!entry.isSkill)
            card->setProperty("shortcutFile", QFileInfo(path).fileName());
        card->setCursor(Qt::PointingHandCursor);
        card->setToolTip(
            script ? QStringLiteral("Run %1").arg(QFileInfo(path).fileName())
                   : dedicated
                         ? QStringLiteral("Start a new %1 agent with %2")
                               .arg(entry.meta.agent,
                                    entry.meta.model.isEmpty()
                                        ? QStringLiteral("its default model")
                                        : entry.meta.model)
                   : prompt ? QStringLiteral("Draft %1 in the composer")
                                  .arg(entry.title)
                            : QStringLiteral("Edit %1")
                                  .arg(QFileInfo(path).fileName()));
        connect(card, &QPushButton::clicked, this,
                [this, path, scope, script, prompt] {
                    if (script)
                        runShortcut(path);
                    else if (prompt)
                        draftShortcutPrompt(path, scope);
                    else
                        openShortcutEditor(path);
                });

        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 10, 12, 10);
        cardLayout->setSpacing(3);

        auto *titleRow = new QHBoxLayout;
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(8);
        auto *icon = new QLabel;
        icon->setPixmap(
            themedOcticon(entry.isSkill ? "sparkle"
                                        : script || dedicated
                                              ? "rocket"
                                              : prompt ? "comment" : "file",
                          Theme::nextOr(Theme::Role::FgMuted,
                                        entry.isSkill ? QColor("#7c3aed")
                                                      : QColor("#2ea043")),
                          16)
                .pixmap(16, 16));
        icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *title = new QLabel(entry.title);
        title->setObjectName(QStringLiteral("shortcutCardTitle"));
        title->setWordWrap(true);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        titleRow->addWidget(icon);
        titleRow->addWidget(title, 1);

        auto *editButton = new QPushButton;
        editButton->setObjectName("ghostButton");
        editButton->setProperty("buttonSize", "sm");
        editButton->setCursor(Qt::PointingHandCursor);
        setOcticon(editButton, "pencil", 14);
        editButton->setToolTip(entry.isSkill ? "Edit this skill"
                                             : "Edit this shortcut");
        connect(editButton, &QPushButton::clicked, this,
                [this, path, isSkill = entry.isSkill] {
                    if (isSkill)
                        openSkillEditor(path);
                    else
                        openShortcutEditor(path);
                });
        auto *deleteButton = new QPushButton;
        deleteButton->setObjectName("ghostButton");
        deleteButton->setProperty("buttonSize", "sm");
        deleteButton->setCursor(Qt::PointingHandCursor);
        setOcticon(deleteButton, "trash", 14);
        deleteButton->setToolTip(entry.isSkill ? "Delete this skill"
                                               : "Delete this shortcut");
        connect(deleteButton, &QPushButton::clicked, this,
                [this, path, isSkill = entry.isSkill] {
                    if (isSkill)
                        deleteSkill(path);
                    else
                        deleteShortcut(path);
                });
        const int queuePosition = script ? shortcutQueuePosition(path) : 0;
        card->setProperty("shortcutQueuePosition", queuePosition);
        if (script && (repoBusy || queuePosition > 0)) {
            auto *queueButton = new QPushButton;
            queueButton->setObjectName("ghostButton");
            queueButton->setProperty("buttonSize", "sm");
            queueButton->setProperty("shortcutQueueButton", true);
            queueButton->setCursor(Qt::PointingHandCursor);
            setOcticon(queueButton, queuePosition > 0 ? "x" : "list-unordered", 14);
            queueButton->setToolTip(
                queuePosition > 0
                    ? QStringLiteral("Remove from the queue")
                    : QStringLiteral("Queue this to run once the running shortcuts finish"));
            connect(queueButton, &QPushButton::clicked, this, [this, path, queuePosition] {
                QTimer::singleShot(0, this, [this, path, queuePosition] {
                    if (queuePosition > 0)
                        unqueueShortcut(path);
                    else
                        queueShortcut(path);
                });
            });
            titleRow->addWidget(queueButton);
        }
        titleRow->addWidget(editButton);
        titleRow->addWidget(deleteButton);
        cardLayout->addLayout(titleRow);

        // Pills sit on their own row: the title needs the full card width, and
        // a two-column grid wraps long names badly once anything shares it.
        auto *metaRow = new QHBoxLayout;
        metaRow->setContentsMargins(0, 0, 0, 0);
        metaRow->setSpacing(6);
        if (entry.uses > 0) {
            auto *runs = new QLabel(QStringLiteral("%1 run%2")
                                        .arg(entry.uses)
                                        .arg(entry.uses == 1
                                                 ? QString()
                                                 : QStringLiteral("s")));
            runs->setObjectName(QStringLiteral("shortcutRunPill"));
            runs->setToolTip(QStringLiteral("Launched %1 time%2 from this page")
                                 .arg(entry.uses)
                                 .arg(entry.uses == 1 ? QString()
                                                      : QStringLiteral("s")));
            runs->setAttribute(Qt::WA_TransparentForMouseEvents);
            metaRow->addWidget(runs);
        }
        if (queuePosition > 0) {
            auto *queued = new QLabel(QStringLiteral("queued #%1").arg(queuePosition));
            queued->setObjectName(QStringLiteral("shortcutRunPill"));
            queued->setToolTip(
                QStringLiteral("Starts once the shortcuts running ahead of it finish"));
            queued->setAttribute(Qt::WA_TransparentForMouseEvents);
            metaRow->addWidget(queued);
        }
        if (entry.isSkill) {
            auto *scopeLabel = new QLabel(
                scope == QLatin1String("repository")
                    ? QStringLiteral("REPO")
                    : scope == QLatin1String("organization")
                          ? QStringLiteral("ORG")
                          : QStringLiteral("APP"));
            scopeLabel->setObjectName(QStringLiteral("skillScopePill"));
            scopeLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            metaRow->addWidget(scopeLabel);
        } else {
            auto *kind = new QLabel(dedicated && !entry.meta.model.isEmpty()
                                        ? entry.meta.model
                                        : entry.kind);
            kind->setObjectName(QStringLiteral("shortcutKindPill"));
            kind->setAttribute(Qt::WA_TransparentForMouseEvents);
            metaRow->addWidget(kind);
        }
        metaRow->addStretch();
        cardLayout->addLayout(metaRow);

        const QString description =
            entry.description.isEmpty() && entry.isSkill
                ? QStringLiteral(
                      "Reusable instructions for ForkMesh special agents and other agents.")
                : entry.description;
        if (!description.isEmpty()) {
            auto *desc = new QLabel(description);
            desc->setObjectName("statusLine");
            desc->setWordWrap(true);
            desc->setAttribute(Qt::WA_TransparentForMouseEvents);
            cardLayout->addWidget(desc);
        }

        if (!entry.meta.steps.isEmpty()) {
            auto *steps = new ShortcutStepList(entry.meta.steps, shortcutTitles,
                                               kMaxCardStepRows);
            steps->onLinkClicked = revealLinkedShortcut;
            steps->setProperty("shortcutPath", path);
            for (auto run = m_shortcutRuns.crbegin(); run != m_shortcutRuns.crend(); ++run) {
                if (run->filePath == path) {
                    steps->setProgress(shortcutStepProgress(*run), run->step);
                    break;
                }
            }
            cardLayout->addSpacing(2);
            cardLayout->addWidget(steps);
        }

        // When it last ran, how often it worked, and how long that took.
        const ShortcutRunStats stats =
            shortcutRunStats(shortcutUsageKey(path, scope));
        QStringList statParts;
        QStringList statTips;
        if (stats.lastRunAtMs > 0) {
            statParts << QStringLiteral("Last run %1")
                             .arg(formatIssueRelativeTime(stats.lastRunAtMs));
            statTips << QStringLiteral("Last run %1")
                            .arg(QDateTime::fromMSecsSinceEpoch(stats.lastRunAtMs)
                                     .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        } else if (entry.uses == 0) {
            statParts << QStringLiteral("Never run");
        }
        const int successRate =
            stats.finished > 0
                ? qRound(stats.succeeded * 100.0 / stats.finished)
                : -1;
        if (successRate >= 0) {
            statParts << QStringLiteral("%1% success").arg(successRate);
            statTips << QStringLiteral("%1 of %2 finished run%3 succeeded")
                            .arg(stats.succeeded)
                            .arg(stats.finished)
                            .arg(stats.finished == 1 ? QString()
                                                     : QStringLiteral("s"));
        } else if (stats.lastRunAtMs > 0 || entry.uses > 0) {
            statParts << QStringLiteral("no finished runs yet");
            statTips << (script || dedicated
                             ? QStringLiteral("Success and duration appear once a "
                                              "run finishes.")
                             : QStringLiteral("Drafting into the composer has no "
                                              "outcome to measure; a prompt with "
                                              "# agent: reports how its agent "
                                              "did."));
        }
        if (stats.lastMs >= 0) {
            statParts << QStringLiteral("took %1").arg(formatDuration(stats.lastMs));
            statTips << QStringLiteral("Last run took %1, %2 on average")
                            .arg(formatDuration(stats.lastMs),
                                 formatDuration(stats.totalMs /
                                                qMax(1, stats.timed)));
        }
        auto *runStats = new QLabel(statParts.join(QStringLiteral(" · ")));
        runStats->setObjectName(QStringLiteral("shortcutRunStats"));
        runStats->setWordWrap(true);
        runStats->setToolTip(statTips.join(QLatin1Char('\n')));
        runStats->setAttribute(Qt::WA_TransparentForMouseEvents);
        cardLayout->addWidget(runStats);
        card->setProperty("shortcutLastRunAtMs", stats.lastRunAtMs);
        card->setProperty("shortcutSuccessRate", successRate);
        card->setProperty("shortcutLastDurationMs", stats.lastMs);
        card->setProperty("shortcutRunStatsText", runStats->text());
        if (QWidget *progress = activeShortcutProgress(path, scope, script, dedicated,
                                                       stats.lastMs, false))
            cardLayout->addWidget(progress);

        if (entry.isSkill) {
            auto *footer = new QLabel(QStringLiteral("Install from Special Agents · %1")
                                          .arg(QFileInfo(path).fileName()));
            footer->setObjectName(QStringLiteral("skillCardFooter"));
            footer->setAttribute(Qt::WA_TransparentForMouseEvents);
            cardLayout->addWidget(footer);
        }

        cardsGrid->addCard(card);
    }
}

void MainWindow::draftShortcutPrompt(QString filePath, QString scope)
{
    const bool isSkill = scope != QLatin1String("shortcut");
    QString prompt = shortcutPromptText(filePath);
    if (prompt.isEmpty()) {
        // Nothing to send: fall back to editing, and don't count that as a run.
        if (isSkill)
            openSkillEditor(filePath);
        else
            openShortcutEditor(filePath);
        return;
    }
    const QString stallLog = stallLogPath();
    prompt.replace(QStringLiteral("{{stallLog}}"),
                   stallLog.isEmpty()
                       ? QStringLiteral("(disabled in this build)")
                       : QDir::toNativeSeparators(stallLog));
    prompt.replace(QStringLiteral("{{appLog}}"),
                   QDir::toNativeSeparators(networkLogPath()));
    const ShortcutMeta meta = shortcutMeta(filePath);
    if (!meta.agent.isEmpty()) {
        QString provider = meta.agent;
        if (provider == QLatin1String("claude"))
            provider = QStringLiteral("claude-code");
        if (!agentIsCodexProvider(provider) &&
            provider != QLatin1String("claude-code")) {
            setRepoDetailNotice(
                QStringLiteral("Shortcut %1 names unsupported dedicated agent “%2”.")
                    .arg(QFileInfo(filePath).fileName(), meta.agent),
                true);
            return;
        }
        if (!agentModelMatchesProvider(provider, meta.model)) {
            setRepoDetailNotice(
                QStringLiteral("Shortcut model %1 does not match agent %2.")
                    .arg(meta.model, provider),
                true);
            return;
        }
        const int sessionId = startAdHocAgentForRepo(
            m_repoDetailIndex, prompt, provider, /*createPr=*/false, meta.model,
            meta.name);
        if (sessionId > 0) {
            // A dedicated skill starts an agent immediately, so preserve that
            // use on the session just like teammate-installed skills. Merely
            // opening or drafting a skill still does not enter the chart.
            if (isSkill) {
                if (AgentSession *session = findAgentSession(sessionId)) {
                    session->skillsUsed = {meta.name};
                    if (m_agentStore)
                        m_agentStore->saveSession(*session);
                }
            }
            trackShortcutAgentRun(sessionId, recordShortcutUsage(filePath, scope));
            loadShortcutsPanel();
            setRepoDetailNotice(
                QStringLiteral("Started dedicated %1 agent for “%2”%3.")
                    .arg(agentProviderName(provider), meta.name,
                         meta.model.isEmpty()
                             ? QString()
                             : QStringLiteral(" on %1").arg(meta.model)));
        }
        return;
    }
    if (!m_issueQuickAdd) {
        if (isSkill)
            openSkillEditor(filePath);
        else
            openShortcutEditor(filePath);
        return;
    }
    recordQuickAddHistory(m_issueQuickAdd->toPlainText());
    m_issueQuickAdd->setPlainText(prompt);
    m_issueQuickAdd->moveCursor(QTextCursor::End);
    m_issueQuickAdd->setFocus();
    recordShortcutUsage(filePath, scope);
    loadShortcutsPanel();
    setRepoDetailNotice(QStringLiteral("Drafted \"%1\" in the composer — review "
                                       "it, then send it to an agent.")
                            .arg(QFileInfo(filePath).fileName()));
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testShortcutMetadata(const QString &filePath) const
{
    const ShortcutMeta meta = shortcutMeta(filePath);
    return {meta.name, meta.description, meta.agent, meta.model,
            shortcutPromptText(filePath)};
}
#endif

QProcess *MainWindow::shortcutProcess(const QString &filePath) const
{
    for (QProcess *process : m_shortcutProcesses)
        if (process->property("shortcutPath").toString() == filePath)
            return process;
    return nullptr;
}

bool MainWindow::shortcutRunning(const QString &filePath) const
{
    return shortcutProcess(filePath) != nullptr;
}

forkmesh::localcontrol::insights::ShortcutRun *MainWindow::shortcutRunById(int id)
{
    for (auto &run : m_shortcutRuns)
        if (run.id == id)
            return &run;
    return nullptr;
}

bool MainWindow::runShortcut(QString filePath, int repoIndex,
                             QMap<QString, QString> environmentOverrides)
{
    if (QProcess *running = shortcutProcess(filePath)) {
        constexpr qint64 kRepeatClickMs = 3000;
        const int runningId = running->property("shortcutRunId").toInt();
        const auto *run = shortcutRunById(runningId);
        if (run && QDateTime::currentMSecsSinceEpoch() - run->startedAtMs < kRepeatClickMs)
            return false;
        setRepoDetailNotice(QStringLiteral("Stopping %1 to run it again.")
                                .arg(QFileInfo(filePath).fileName()));
        stopShortcutRun(runningId);
        m_shortcutRestarts.insert(filePath, repoIndex);
        return true;
    }
    if (repoIndex < 0)
        repoIndex = m_repoDetailIndex;
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return false;
    const RepositoryRecord &record = m_repositories.at(repoIndex);
    const QString workDir = record.localPath;
    const QString name = QFileInfo(filePath).fileName();
    const QString command = QStringLiteral("$ bash %1").arg(filePath);

    const QString usageKey =
        recordShortcutUsage(filePath, QStringLiteral("shortcut"), repoIndex);

    constexpr int kKeptShortcutRuns = 20;
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    const int runId = ++m_shortcutRunCounter;
    forkmesh::localcontrol::insights::ShortcutRun run;
    run.id = runId;
    run.repo = record.owner + QLatin1Char('/') + record.name;
    run.file = name;
    const ShortcutMeta meta = shortcutMeta(filePath);
    run.name = meta.name;
    run.running = true;
    run.startedAtMs = startedAtMs;
    run.expectedMs = shortcutRunStats(usageKey).lastMs;
    run.filePath = filePath;
    run.steps = meta.steps;
    m_shortcutRuns.append(run);
    for (qsizetype i = 0; m_shortcutRuns.size() > kKeptShortcutRuns && i < m_shortcutRuns.size();) {
        if (m_shortcutRuns.at(i).running)
            ++i;
        else
            m_shortcutRuns.removeAt(i);
    }

    auto *process = new QProcess(this);
    m_shortcutProcesses.insert(runId, process);
    process->setProperty("shortcutPath", filePath);
    process->setProperty("shortcutRunId", runId);
    showShortcutRun(runId);
    loadShortcutsPanel();
    appendShortcutOutput(runId, command + QLatin1Char('\n'));
    process->setWorkingDirectory(workDir);
    process->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QMap<QString, QString> variables = ActionStore::variables();
    for (const QString &name : {QStringLiteral("ASC_KEY_ID"), QStringLiteral("ASC_ISSUER_ID")}) {
        const QString value = variables.value(name).trimmed();
        if (!value.isEmpty())
            environment.insert(name, value);
    }
    for (auto it = environmentOverrides.cbegin(); it != environmentOverrides.cend(); ++it)
        environment.insert(it.key(), it.value());
    process->setProcessEnvironment(environment);
    connect(process, &QProcess::readyReadStandardOutput, this, [this, process, runId] {
        appendShortcutOutput(runId, QString::fromUtf8(process->readAllStandardOutput()));
    });
    auto finish = [this, process, runId, filePath, usageKey,
                   startedAtMs](const QString &message, bool ok, bool started) {
        m_shortcutProcesses.remove(runId);
        QString lastLine;
        if (auto *run = shortcutRunById(runId)) {
            lastLine = forkmesh::localcontrol::insights::lastOutputLine(run->output);
            run->running = false;
            run->ok = ok;
            run->stopped = process->property("shortcutStopRequested").toBool();
            run->message = message;
        }
        appendShortcutOutput(runId, message + QLatin1Char('\n'));
        if (runId == m_shortcutShownRun)
            showShortcutRun(runId);
        else
            refreshShortcutRunPicker();
        if (!usageKey.isEmpty()) {
            ShortcutRunStats stats = shortcutRunStats(usageKey);
            stats.lastOutput = lastLine.isEmpty() ? message : lastLine;
            m_shortcutRunStats.insert(usageKey, stats);
        }
        // A run the user stopped says nothing about whether the shortcut works.
        if (!process->property("shortcutStopRequested").toBool()) {
            recordShortcutRunResult(
                usageKey, ok,
                started ? QDateTime::currentMSecsSinceEpoch() - startedAtMs : -1);
            loadShortcutsPanel();
        } else {
            if (!usageKey.isEmpty())
                QSettings().setValue(kShortcutRunStatsSetting,
                                     shortcutRunStatsSettingValue());
            loadShortcutsPanel();
        }
        if (m_localControl)
            m_localControl->broadcast(QStringLiteral("insights.changed"));
        process->deleteLater();
        if (m_shortcutRestarts.contains(filePath)) {
            const int againRepo = m_shortcutRestarts.take(filePath);
            QTimer::singleShot(0, this, [this, filePath, againRepo] { runShortcut(filePath, againRepo); });
        } else if (!m_shortcutQueue.isEmpty()) {
            QTimer::singleShot(0, this, [this] { startNextQueuedShortcuts(); });
        }
    };
    connect(process, &QProcess::finished, this,
            [finish](int code, QProcess::ExitStatus st) {
                const bool ok = st == QProcess::NormalExit && code == 0;
                finish(ok ? QStringLiteral("done (exit 0)")
                          : QStringLiteral("failed (exit %1)").arg(code),
                       ok, true);
            });
    connect(process, &QProcess::errorOccurred, this,
            [finish, process](QProcess::ProcessError) {
                if (process->state() == QProcess::NotRunning &&
                    process->error() == QProcess::FailedToStart)
                    finish(QStringLiteral("failed to start bash"), false, false);
            });
    process->start(QStringLiteral("bash"), {filePath});
    return true;
}

void MainWindow::appendShortcutOutput(int runId, const QString &chunk)
{
    namespace insightctl = forkmesh::localcontrol::insights;
    constexpr int kMaxShortcutOutputChars = 96 * 1024;
    constexpr int kMaxShortcutChunkChars = 16 * 1024;
    auto *run = shortcutRunById(runId);
    if (!run)
        return;
    run->output = insightctl::appendShortcutOutput(run->output, chunk, kMaxShortcutOutputChars);
    if (m_shortcutOutput && runId == m_shortcutShownRun)
        m_shortcutOutput->appendPlainText(chunk.trimmed());
    if (insightctl::trackShortcutStep(*run, chunk)) {
        if (m_shortcutRunStatus && runId == m_shortcutShownRun)
            m_shortcutRunStatus->setText(shortcutRunStatusText(*run));
        if (m_shortcutCardsHost)
            for (QWidget *steps : m_shortcutCardsHost->findChildren<QWidget *>(
                     QStringLiteral("shortcutStepList")))
                if (steps->property("shortcutPath").toString() == run->filePath)
                    static_cast<ShortcutStepList *>(steps)->setProgress(
                        shortcutStepProgress(*run), run->step);
    }
    if (!m_localControl)
        return;
    QJsonObject event = insightctl::shortcutRunRow(*run, false);
    event.insert(QStringLiteral("chunk"), chunk.right(kMaxShortcutChunkChars));
    m_localControl->broadcast(QStringLiteral("insights.shortcutOutput"), event);
}

void MainWindow::showShortcutRun(int runId)
{
    m_shortcutShownRun = runId;
    const auto *run = shortcutRunById(runId);
    if (m_shortcutOutput) {
        m_shortcutOutput->setPlainText(run ? run->output.trimmed() : QString());
        m_shortcutOutput->moveCursor(QTextCursor::End);
    }
    if (m_shortcutRunStatus)
        m_shortcutRunStatus->setText(run ? shortcutRunStatusText(*run) : QString());
    if (m_shortcutStopButton)
        m_shortcutStopButton->setVisible(run && run->running);
    refreshShortcutRunPicker();
    tickShortcutProgress();
}

QWidget *MainWindow::shortcutProgressWidget(qint64 startedAtMs, qint64 expectedMs,
                                            int agentSessionId, bool compact)
{
    auto *holder = new QWidget;
    holder->setObjectName(QStringLiteral("shortcutRunProgressBox"));
    holder->setAttribute(Qt::WA_TransparentForMouseEvents);
    QBoxLayout *layout = compact ? static_cast<QBoxLayout *>(new QHBoxLayout(holder))
                                 : new QVBoxLayout(holder);
    layout->setContentsMargins(compact ? 6 : 0, compact ? 0 : 4, compact ? 6 : 0, 0);
    layout->setSpacing(compact ? 6 : 3);
    auto *bar = new QProgressBar;
    bar->setObjectName(QStringLiteral("shortcutRunProgress"));
    bar->setTextVisible(false);
    bar->setFixedHeight(4);
    if (compact)
        bar->setFixedWidth(56);
    auto *label = new QLabel;
    label->setObjectName(QStringLiteral("shortcutRunEta"));
    layout->addWidget(bar, 0, compact ? Qt::AlignVCenter : Qt::Alignment());
    layout->addWidget(label, 1);
    m_shortcutProgress.append({bar, label, startedAtMs, expectedMs, agentSessionId});
    tickShortcutProgress();
    return holder;
}

QWidget *MainWindow::activeShortcutProgress(const QString &filePath, const QString &scope,
                                            bool script, bool dedicated, qint64 lastMs,
                                            bool compact)
{
    if (script) {
        const QProcess *process = shortcutProcess(filePath);
        const auto *run =
            process ? shortcutRunById(process->property("shortcutRunId").toInt()) : nullptr;
        return run ? shortcutProgressWidget(run->startedAtMs, run->expectedMs, 0, compact)
                   : nullptr;
    }
    if (!dedicated)
        return nullptr;
    const QString key = shortcutUsageKey(filePath, scope);
    for (auto it = m_shortcutAgentRuns.cbegin(); it != m_shortcutAgentRuns.cend(); ++it) {
        const AgentSession *session = it.value() == key ? findAgentSession(it.key()) : nullptr;
        if (session && (session->runInFlight() || session->status == AgentStatus::Queued))
            return shortcutProgressWidget(0, lastMs, it.key(), compact);
    }
    return nullptr;
}

void MainWindow::tickShortcutProgress()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    bool active = false;
    for (qsizetype i = 0; i < m_shortcutProgress.size();) {
        const ShortcutProgress &progress = m_shortcutProgress.at(i);
        if (!progress.bar) {
            m_shortcutProgress.removeAt(i);
            continue;
        }
        qint64 elapsedMs = nowMs - progress.startedAtMs;
        bool waiting = false;
        if (progress.agentSessionId > 0) {
            const AgentSession *session = findAgentSession(progress.agentSessionId);
            if (!session || !(session->runInFlight() ||
                              session->status == AgentStatus::Queued)) {
                progress.bar->parentWidget()->hide();
                m_shortcutProgress.removeAt(i);
                continue;
            }
            waiting = !session->runInFlight();
            elapsedMs = session->totalRunMs(nowMs);
        }
        setShortcutProgress(progress.bar, progress.label, elapsedMs, progress.expectedMs,
                            waiting);
        active = true;
        ++i;
    }
    const auto *run = shortcutRunById(m_shortcutShownRun);
    const bool shownRunning = run && run->running;
    if (m_shortcutRunProgress && m_shortcutRunEta) {
        m_shortcutRunProgress->setVisible(shownRunning);
        m_shortcutRunEta->setVisible(shownRunning);
        if (shownRunning)
            setShortcutProgress(m_shortcutRunProgress, m_shortcutRunEta,
                                nowMs - run->startedAtMs, run->expectedMs, false);
    }
    active = active || shownRunning;
    if (!active) {
        if (m_shortcutProgressTimer)
            m_shortcutProgressTimer->stop();
        return;
    }
    if (!m_shortcutProgressTimer) {
        m_shortcutProgressTimer = new QTimer(this);
        m_shortcutProgressTimer->setInterval(1000);
        connect(m_shortcutProgressTimer, &QTimer::timeout, this,
                &MainWindow::tickShortcutProgress);
    }
    if (!m_shortcutProgressTimer->isActive())
        m_shortcutProgressTimer->start();
}

void MainWindow::refreshShortcutRunPicker()
{
    if (!m_shortcutRunPicker)
        return;
    const QSignalBlocker blocker(m_shortcutRunPicker);
    m_shortcutRunPicker->clear();
    for (auto it = m_shortcutRuns.crbegin(); it != m_shortcutRuns.crend(); ++it)
        m_shortcutRunPicker->addItem(
            QStringLiteral("%1 — %2").arg(it->file, it->running ? QStringLiteral("running")
                                                                 : it->message),
            it->id);
    m_shortcutRunPicker->setCurrentIndex(m_shortcutRunPicker->findData(m_shortcutShownRun));
    m_shortcutRunPicker->setVisible(m_shortcutRuns.size() > 1);
}

void MainWindow::stopShortcut()
{
    stopShortcutRun(m_shortcutShownRun);
}

void MainWindow::stopShortcutRun(int runId)
{
    QPointer<QProcess> process = m_shortcutProcesses.value(runId);
    if (!process)
        return;
    m_shortcutRestarts.remove(process->property("shortcutPath").toString());
    appendShortcutOutput(runId, QStringLiteral("^C stopping…\n"));
    process->setProperty("shortcutStopRequested", true);
#ifndef Q_OS_WIN
    const qint64 pid = process->processId();
    if (pid > 0)
        ::kill(static_cast<pid_t>(pid), SIGINT);
    QTimer::singleShot(1000, this, [process] {
        if (process && process->state() != QProcess::NotRunning)
            process->terminate();
    });
#else
    process->terminate();
#endif
    QTimer::singleShot(4000, this, [process] {
        if (process && process->state() != QProcess::NotRunning)
            process->kill();
    });
}

QString MainWindow::shortcutRepoKey(int repoIndex) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &record = m_repositories.at(repoIndex);
    return record.owner + QLatin1Char('/') + record.name;
}

bool MainWindow::shortcutRepoBusy(const QString &repo) const
{
    for (const auto &run : m_shortcutRuns)
        if (run.running && run.repo.compare(repo, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

int MainWindow::shortcutQueuePosition(const QString &filePath) const
{
    const QString path = QFileInfo(filePath).absoluteFilePath();
    for (qsizetype i = 0; i < m_shortcutQueue.size(); ++i) {
        if (m_shortcutQueue.at(i).path != path)
            continue;
        int position = 0;
        for (qsizetype j = 0; j <= i; ++j)
            position += m_shortcutQueue.at(j).repo.compare(m_shortcutQueue.at(i).repo,
                                                           Qt::CaseInsensitive) == 0
                            ? 1
                            : 0;
        return position;
    }
    return 0;
}

int MainWindow::queueShortcut(QString filePath, int repoIndex)
{
    if (repoIndex < 0)
        repoIndex = m_repoDetailIndex;
    const QString repo = shortcutRepoKey(repoIndex);
    const QFileInfo info(filePath);
    if (repo.isEmpty() || !info.isFile() || !shortcutIsScript(filePath))
        return -1;
    const QString path = info.absoluteFilePath();
    if (const int position = shortcutQueuePosition(path))
        return position;
    if (!shortcutRepoBusy(repo))
        return runShortcut(path, repoIndex) ? 0 : -1;
    m_shortcutQueue.append({path, repo, info.fileName(), shortcutMeta(path).name});
    setRepoDetailNotice(QStringLiteral("%1 runs once the shortcuts running in %2 finish.")
                            .arg(info.fileName(), repo));
    refreshShortcutQueueLabel();
    loadShortcutsPanel();
    if (m_localControl)
        m_localControl->broadcast(QStringLiteral("insights.changed"));
    return shortcutQueuePosition(path);
}

bool MainWindow::unqueueShortcut(const QString &filePath)
{
    const QString path = QFileInfo(filePath).absoluteFilePath();
    const qsizetype removed = m_shortcutQueue.removeIf(
        [&path](const QueuedShortcut &queued) { return queued.path == path; });
    if (removed == 0)
        return false;
    refreshShortcutQueueLabel();
    loadShortcutsPanel();
    if (m_localControl)
        m_localControl->broadcast(QStringLiteral("insights.changed"));
    return true;
}

void MainWindow::clearShortcutQueue()
{
    if (m_shortcutQueue.isEmpty())
        return;
    m_shortcutQueue.clear();
    refreshShortcutQueueLabel();
    loadShortcutsPanel();
    if (m_localControl)
        m_localControl->broadcast(QStringLiteral("insights.changed"));
}

void MainWindow::startNextQueuedShortcuts()
{
    QSet<QString> started;
    for (qsizetype i = 0; i < m_shortcutQueue.size();) {
        const QueuedShortcut queued = m_shortcutQueue.at(i);
        const QString repo = queued.repo.toLower();
        if (started.contains(repo) || shortcutRepoBusy(queued.repo)) {
            ++i;
            continue;
        }
        m_shortcutQueue.removeAt(i);
        int repoIndex = -1;
        for (int r = 0; r < m_repositories.size() && repoIndex < 0; ++r)
            if (shortcutRepoKey(r).compare(queued.repo, Qt::CaseInsensitive) == 0)
                repoIndex = r;
        if (repoIndex < 0 || !QFileInfo(queued.path).isFile())
            continue;
        if (runShortcut(queued.path, repoIndex))
            started.insert(repo);
        i = 0;
    }
    refreshShortcutQueueLabel();
}

void MainWindow::refreshShortcutQueueLabel()
{
    if (!m_shortcutQueueLabel)
        return;
    QStringList names;
    for (const QueuedShortcut &queued : std::as_const(m_shortcutQueue))
        names << queued.file;
    m_shortcutQueueLabel->setText(
        names.isEmpty() ? QString()
                        : QStringLiteral("Up next: %1").arg(names.join(QStringLiteral(", "))));
    m_shortcutQueueLabel->setVisible(!names.isEmpty());
    if (m_shortcutQueueClear)
        m_shortcutQueueClear->setVisible(!names.isEmpty());
}

void MainWindow::openShortcutEditor(const QString &filePath)
{
    const QString dirPath = shortcutsDirPath();
    if (dirPath.isEmpty()) {
        setRepoDetailNotice(
            "This is a read-only mirror; shortcuts can't be edited here.", true);
        return;
    }
    const bool creating = filePath.isEmpty();

    QDialog dialog(this);
    dialog.setWindowTitle(creating ? "New shortcut" : "Edit shortcut");
    dialog.resize(640, 480);
    auto *form = new QVBoxLayout(&dialog);
    auto *nameEdit = new QLineEdit;
    nameEdit->setPlaceholderText("file name, e.g. run-tests.sh (.md for a prompt)");
    if (creating) {
        form->addWidget(nameEdit);
    } else {
        nameEdit->setText(QFileInfo(filePath).fileName());
        auto *nameLabel = new QLabel(QFileInfo(filePath).fileName());
        nameLabel->setObjectName("statusLine");
        form->addWidget(nameLabel);
    }
    auto *contentEdit = new QPlainTextEdit;
    contentEdit->setObjectName("networkLog");
    if (creating) {
        contentEdit->setPlainText(QStringLiteral(
            "#!/usr/bin/env bash\n"
            "# name: My shortcut\n"
            "# description: What this shortcut does.\n"
            "set -euo pipefail\n\n"));
    } else {
        QFile file(filePath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            contentEdit->setPlainText(QString::fromUtf8(file.readAll()));
    }
    form->addWidget(contentEdit, 1);
    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QString targetPath = filePath;
    if (creating) {
        const QString name =
            QFileInfo(nameEdit->text().trimmed()).fileName();
        if (name.isEmpty() || name.startsWith(QLatin1Char('.'))) {
            setRepoDetailNotice("Shortcut needs a plain file name.", true);
            return;
        }
        targetPath = dirPath + QLatin1Char('/') + name;
        if (QFile::exists(targetPath)) {
            setRepoDetailNotice(
                QStringLiteral("%1 already exists.").arg(name), true);
            return;
        }
        QDir().mkpath(dirPath);
    }
    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setRepoDetailNotice(
            QStringLiteral("Couldn't write %1.").arg(targetPath), true);
        return;
    }
    out.write(contentEdit->toPlainText().toUtf8());
    out.close();
    if (shortcutIsScript(targetPath))
        QFile::setPermissions(targetPath, QFile::permissions(targetPath) |
                                              QFileDevice::ExeOwner |
                                              QFileDevice::ExeGroup);
    setRepoDetailNotice(
        QStringLiteral("Saved %1.").arg(QFileInfo(targetPath).fileName()), false);
    loadShortcutsPanel();
}

void MainWindow::deleteShortcut(const QString &filePath)
{
    const QString name = QFileInfo(filePath).fileName();
    if (QMessageBox::question(
            this, "Delete shortcut",
            QStringLiteral("Delete %1? The file is removed from "
                           ".forkmesh/shortcuts/ in the working tree.")
                .arg(name)) != QMessageBox::Yes)
        return;
    if (!QFile::remove(filePath)) {
        setRepoDetailNotice(QStringLiteral("Couldn't delete %1.").arg(name), true);
        return;
    }
    forgetShortcutUsage(filePath, QStringLiteral("shortcut"));
    setRepoDetailNotice(QStringLiteral("Deleted %1.").arg(name), false);
    loadShortcutsPanel();
}

QString MainWindow::skillsDirPath(const QString &scope, int repoIndex) const
{
    if (repoIndex < 0)
        repoIndex = m_repoDetailIndex;
    const QString normalized = scope.trimmed().toLower();
    if (normalized == QLatin1String("app")) {
        return QDir(forkmesh::paths::dataRoot())
            .filePath(QStringLiteral("skills"));
    }
    if (normalized == QLatin1String("organization")) {
        QString owner = QStringLiteral("personal");
        if (repoIndex >= 0 && repoIndex < m_repositories.size())
            owner = m_repositories.at(repoIndex).owner.trimmed();
        owner.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
                      QStringLiteral("-"));
        if (owner.isEmpty() || owner == QLatin1String(".") ||
            owner == QLatin1String(".."))
            owner = QStringLiteral("personal");
        return QDir(forkmesh::paths::dataRoot())
            .filePath(QStringLiteral("organization-skills/%1").arg(owner));
    }
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return {};
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    if (repo.previewOnly || repo.localPath.isEmpty())
        return {};
    return QDir(repo.localPath).filePath(QStringLiteral(".forkmesh/skills"));
}

void MainWindow::openSkillEditor(const QString &filePath)
{
    if (!m_skillsScope)
        return;
    const bool creating = filePath.isEmpty();
    const QString directory = creating
                                  ? skillsDirPath(m_skillsScope->currentData().toString())
                                  : QFileInfo(filePath).absolutePath();
    if (directory.isEmpty()) {
        setRepoDetailNotice(QStringLiteral("This skill scope is not writable."), true);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(creating ? QStringLiteral("New skill")
                                   : QStringLiteral("Edit skill"));
    dialog.resize(680, 570);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(
        QStringLiteral("Skills are reusable operating knowledge. Special agents receive "
                       "the selected skill instructions with every assignment."));
    hint->setObjectName(QStringLiteral("statusLine"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("Release validation"));
    auto *description = new QLineEdit;
    description->setPlaceholderText(
        QStringLiteral("Validates a release against acceptance criteria and evidence."));
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Description"), description);
    layout->addLayout(form);
    auto *instructionsLabel = new QLabel(QStringLiteral("Instructions"));
    instructionsLabel->setObjectName(QStringLiteral("teammateFieldLabel"));
    layout->addWidget(instructionsLabel);
    auto *instructions = new QPlainTextEdit;
    instructions->setPlaceholderText(
        QStringLiteral("When this skill is installed:\n- Inspect…\n- Verify…\n- Report…"));
    layout->addWidget(instructions, 1);
    if (!creating) {
        const ShortcutMeta meta = shortcutMeta(filePath);
        name->setText(meta.name == QFileInfo(filePath).fileName()
                          ? QFileInfo(filePath).completeBaseName()
                          : meta.name);
        description->setText(meta.description);
        instructions->setPlainText(shortcutPromptText(filePath));
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save |
                                        QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString cleanName = name->text().trimmed();
    const QString body = instructions->toPlainText().trimmed();
    if (cleanName.isEmpty() || body.isEmpty()) {
        setRepoDetailNotice(
            QStringLiteral("A skill needs a name and instructions."), true);
        return;
    }
    QString target = filePath;
    if (creating) {
        QString slug = cleanName.toLower();
        slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                     QStringLiteral("-"));
        slug.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
        if (slug.isEmpty())
            slug = QStringLiteral("skill");
        target = QDir(directory).filePath(slug.left(64) + QStringLiteral(".skill"));
        if (QFileInfo::exists(target)) {
            setRepoDetailNotice(
                QStringLiteral("A skill named %1 already exists in this scope.")
                    .arg(QFileInfo(target).fileName()),
                true);
            return;
        }
    }
    if (!QDir().mkpath(directory)) {
        setRepoDetailNotice(QStringLiteral("Could not create the skills folder."),
                            true);
        return;
    }
    QSaveFile output(target);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setRepoDetailNotice(QStringLiteral("Could not write that skill."), true);
        return;
    }
    const QString content = QStringLiteral("# name: %1\n# description: %2\n\n%3\n")
                                .arg(cleanName,
                                     description->text().trimmed(), body);
    output.write(content.toUtf8());
    if (!output.commit()) {
        setRepoDetailNotice(QStringLiteral("Could not finish saving that skill."),
                            true);
        return;
    }
    setRepoDetailNotice(
        QStringLiteral("Saved skill %1 — special agents can install it now.")
            .arg(cleanName),
        false);
    loadShortcutsPanel();
}

void MainWindow::deleteSkill(const QString &filePath)
{
    const QString name = QFileInfo(filePath).fileName();
    const QString canonicalParent = QFileInfo(filePath).absolutePath();
    const QStringList scopes{
        QStringLiteral("repository"),
        QStringLiteral("app"),
        QStringLiteral("organization"),
    };
    QString scope;
    for (const QString &candidate : scopes) {
        if (skillsDirPath(candidate) == canonicalParent) {
            scope = candidate;
            break;
        }
    }
    if (scope.isEmpty()) {
        setRepoDetailNotice(QStringLiteral("That skill is outside a managed scope."),
                            true);
        return;
    }
    if (QMessageBox::question(
            this, QStringLiteral("Delete skill"),
            QStringLiteral("Delete %1? Special agents keep the skill tag, but its "
                           "reusable instructions will no longer be installed.")
                .arg(name)) != QMessageBox::Yes)
        return;
    if (!QFile::remove(filePath)) {
        setRepoDetailNotice(QStringLiteral("Could not delete %1.").arg(name), true);
        return;
    }
    forgetShortcutUsage(filePath, scope);
    setRepoDetailNotice(QStringLiteral("Deleted skill %1.").arg(name), false);
    loadShortcutsPanel();
}

int MainWindow::scrubTargetRepoIndex(const QString &scope) const
{
    int index = m_repoDetailIndex;
    if (!scope.isEmpty()) {
        const qsizetype slash = scope.indexOf(QLatin1Char('/'));
        index = slash < 0 ? -1 : repoIndexFor(scope.left(slash), scope.mid(slash + 1));
    }
    if (index < 0 || index >= m_repositories.size())
        return -1;
    const RepositoryRecord &repo = m_repositories.at(index);
    if (repo.previewOnly || repo.localPath.isEmpty() || !QDir(repo.localPath).exists())
        return -1;
    return index;
}

QString MainWindow::scrubTermShortcutPath(int repoIndex) const
{
    const QString name = QStringLiteral("scrub-term.sh");
    if (repoIndex >= 0 && repoIndex < m_repositories.size()) {
        const QString own = m_repositories.at(repoIndex).localPath +
                            QStringLiteral("/.forkmesh/shortcuts/") + name;
        if (QFileInfo(own).isFile())
            return own;
    }
    const QString bundledDir = forkmesh::paths::cacheRoot() + QStringLiteral("/shortcuts");
    const QString bundled = bundledDir + QLatin1Char('/') + name;
    QFile resource(QStringLiteral(":/shortcuts/") + name);
    if (!resource.open(QIODevice::ReadOnly) || !QDir().mkpath(bundledDir))
        return {};
    const QByteArray script = resource.readAll();
    QFile current(bundled);
    if (current.open(QIODevice::ReadOnly) && current.readAll() == script)
        return bundled;
    current.close();
    QSaveFile out(bundled);
    if (!out.open(QIODevice::WriteOnly) || out.write(script) != script.size() || !out.commit())
        return {};
    return bundled;
}

void MainWindow::scrubSearchTerm(QString term, QString scope)
{
    term = term.trimmed();
    if (term.isEmpty())
        return;
    const int repoIndex = scrubTargetRepoIndex(scope);
    if (repoIndex < 0) {
        flashMessage(scope.isEmpty()
                         ? QStringLiteral("Open a repository with a local checkout to scrub "
                                          "a term from it.")
                         : QStringLiteral("%1 has no local checkout on this node to scrub.")
                               .arg(scope),
                     true);
        return;
    }
    const QString script = scrubTermShortcutPath(repoIndex);
    if (script.isEmpty()) {
        flashMessage(QStringLiteral("The Scrub a term shortcut could not be prepared."), true);
        return;
    }
    if (shortcutProcess(script)) {
        flashMessage(QStringLiteral("A scrub is already running. Wait for it to finish."),
                     true);
        return;
    }
    const RepositoryRecord repo = m_repositories.at(repoIndex);
    const QString repoName = repo.owner + QLatin1Char('/') + repo.name;
    const QString backupDir = forkmesh::paths::dataRoot() + QStringLiteral("/scrub-backups");

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("scrubTermDialog"));
    dialog.setWindowTitle(QStringLiteral("Scrub a term"));
    dialog.setMinimumWidth(520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        QString::fromUtf8(
            "Replace \xE2\x80\x9C%1\xE2\x80\x9D (not case-sensitive) in %2 at %3.\n\n"
            "\xE2\x80\xA2 Every tracked text file is changed and committed.\n"
            "\xE2\x80\xA2 With history on, every commit, message, author and tag on every "
            "branch is rewritten, the reflogs are expired and the old objects are pruned. "
            "Commit ids change; other worktrees keep their old files until refreshed.\n"
            "\xE2\x80\xA2 First, every ref is saved to a backup bundle in %4. The bundle "
            "still holds the term: delete it once you have checked the result.\n"
            "\xE2\x80\xA2 Remotes and other clones keep the old history until you replace "
            "or force-push them.\n\n"
            "Commit or move any uncommitted changes first.")
            .arg(term, repoName, repo.localPath, backupDir));
    intro->setTextFormat(Qt::PlainText);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *replacement = new QLineEdit(QStringLiteral("[redacted]"));
    replacement->setObjectName(QStringLiteral("scrubTermReplacement"));
    form->addRow(QStringLiteral("Replace with"), replacement);
    auto *history = new QCheckBox(QStringLiteral("Also rewrite history on every branch"));
    history->setObjectName(QStringLiteral("scrubTermHistory"));
    history->setChecked(true);
    form->addRow(QString(), history);
    auto *confirm = new QLineEdit;
    confirm->setObjectName(QStringLiteral("scrubTermConfirm"));
    confirm->setPlaceholderText(term);
    form->addRow(QStringLiteral("Type the term to confirm"), confirm);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *run = buttons->addButton(QStringLiteral("Scrub"), QDialogButtonBox::AcceptRole);
    run->setObjectName(QStringLiteral("dangerButton"));
    run->setEnabled(false);
    connect(confirm, &QLineEdit::textChanged, run,
            [run, term](const QString &typed) { run->setEnabled(typed.trimmed() == term); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || confirm->text().trimmed() != term)
        return;

    QMap<QString, QString> environment{
        {QStringLiteral("SCRUB_TERM"), term},
        {QStringLiteral("SCRUB_REPLACEMENT"), replacement->text()},
        {QStringLiteral("SCRUB_HISTORY"),
         history->isChecked() ? QStringLiteral("1") : QStringLiteral("0")},
        {QStringLiteral("SCRUB_BACKUP_DIR"), backupDir}};
    if (repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    if (m_repoDetailTabs && m_shortcutsTabIndex >= 0 &&
        m_repoDetailTabs->button(m_shortcutsTabIndex))
        m_repoDetailTabs->button(m_shortcutsTabIndex)->click();
    if (!runShortcut(script, repoIndex, environment))
        flashMessage(QStringLiteral("Could not start the scrub."), true);
}
