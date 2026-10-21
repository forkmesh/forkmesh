
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "FederatedThreadView.h"
#include "KebabHeaderView.h"
#include "PrivateMirrorRuntime.h"

using namespace forkmesh::ui;


QWidget *MainWindow::buildDiscussionsTab()
{
    auto *page = new QWidget;

    auto *listPane = new QWidget;
    listPane->setMinimumWidth(340);
    auto *heading = new QLabel("Discussions");
    heading->setObjectName("channelTitle");

    m_discussionNewButton = new QPushButton("New discussion");
    m_discussionSyncButton = new QPushButton("Sync inbox");
    for (QPushButton *b : {m_discussionNewButton, m_discussionSyncButton}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_discussionNewButton, "plus", 16);
    setOcticon(m_discussionSyncButton, "sync", 16);
    m_discussionSyncButton->setToolTip(
        "Pull discussion submissions filed by other nodes");

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(6);
    toolbar->addWidget(m_discussionNewButton);
    toolbar->addWidget(m_discussionSyncButton);
    toolbar->addStretch();

    m_discussionSearch = new QLineEdit;
    m_discussionSearch->setObjectName("issueSearch");
    m_discussionSearch->setPlaceholderText("Search discussions...");
    m_discussionSearch->setClearButtonEnabled(true);

    m_discussionCategoryFilter = new QComboBox;
    m_discussionCategoryFilter->setObjectName("issueFilter");
    m_discussionCategoryFilter->addItem("All categories", QString());
    for (const QString &category : DiscussionStore::categories()) {
        m_discussionCategoryFilter->addItem(
            category, DiscussionStore::normalizedCategory(category));
    }

    m_discussionTable = new QTableWidget(0, 7);
    installColumnHeaderMenu(m_discussionTable);
    m_discussionTable->setObjectName("issueTable");
    enableHoverRowHighlight(m_discussionTable);
    m_discussionTable->setHorizontalHeaderLabels(
        {"#", "Title", "Category", "Status", "Comments", "Updated", "Author"});
    m_discussionTable->verticalHeader()->setVisible(false);
    m_discussionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_discussionTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_discussionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_discussionTable->setShowGrid(false);
    m_discussionTable->setWordWrap(false);
    m_discussionTable->setSortingEnabled(true);
    QHeaderView *dh = m_discussionTable->horizontalHeader();
    dh->setHighlightSections(false);
    dh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    dh->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int c = 2; c < 7; ++c)
        dh->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_discussionTable);

    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(18, 18, 12, 18);
    listLayout->setSpacing(8);
    listLayout->addWidget(heading);
    listLayout->addLayout(toolbar);
    listLayout->addWidget(m_discussionSearch);
    listLayout->addWidget(m_discussionCategoryFilter);
    listLayout->addWidget(m_discussionTable, 1);

    auto *detailPane = new QWidget;
    m_discussionTitle = new QLabel("Select a discussion");
    m_discussionTitle->setObjectName("channelTitle");
    m_discussionTitle->setTextFormat(Qt::PlainText);
    m_discussionTitle->setWordWrap(true);
    m_discussionMeta = new QLabel;
    m_discussionMeta->setObjectName("statusLine");
    m_discussionMeta->setTextFormat(Qt::RichText);
    m_discussionMeta->setWordWrap(true);
    m_discussionMeta->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_discussionMeta->setOpenExternalLinks(false);
    connect(m_discussionMeta, &QLabel::linkActivated, this,
            [this](const QString &href) { openBodyReference(href); });
    m_discussionInlineNotice = new QLabel;
    m_discussionInlineNotice->setObjectName("issueInlineNotice");
    m_discussionInlineNotice->setWordWrap(true);
    m_discussionInlineNotice->hide();

    m_discussionCloseButton = new QPushButton("Close discussion");
    m_discussionArchiveButton = new QPushButton("Archive discussion");
    m_discussionDeleteButton = new QPushButton("Delete discussion");
    for (QPushButton *button : {m_discussionCloseButton,
                                m_discussionArchiveButton,
                                m_discussionDeleteButton}) {
        button->setObjectName(button == m_discussionDeleteButton
                                   ? "issueDangerLink"
                                   : "ghostButton");
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_discussionCloseButton, "check-circle", 15);
    setOcticon(m_discussionArchiveButton, "package", 15);
    setOcticon(m_discussionDeleteButton, "trash", 15);
    auto *detailHeader = new QHBoxLayout;
    detailHeader->setContentsMargins(0, 0, 0, 0);
    detailHeader->setSpacing(6);
    detailHeader->addWidget(m_discussionTitle, 1);
    detailHeader->addWidget(m_discussionCloseButton);
    detailHeader->addWidget(m_discussionArchiveButton);
    detailHeader->addWidget(m_discussionDeleteButton);

    m_discussionThreadContainer = new QWidget;
    m_discussionThreadLayout = new QVBoxLayout(m_discussionThreadContainer);
    m_discussionThreadLayout->setContentsMargins(0, 0, 0, 0);
    m_discussionThreadLayout->setSpacing(10);
    m_discussionThreadLayout->addStretch();

    m_discussionThreadScroll = new QScrollArea;
    m_discussionThreadScroll->setWidgetResizable(true);
    m_discussionThreadScroll->setWidget(m_discussionThreadContainer);
    m_discussionThreadScroll->setObjectName("issuePageScroll");
    m_discussionThreadScroll->setFrameShape(QFrame::NoFrame);

    m_discussionComposer = new MarkdownEditor;
    m_discussionComposer->setPlaceholderText("Add to the discussion...");
    m_discussionComposer->setMinimumHeight(100);
    m_discussionCommentButton = new QPushButton("Comment");
    m_discussionCommentButton->setObjectName("ghostButton");
    m_discussionCommentButton->setProperty("buttonSize", "sm");
    m_discussionCommentButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_discussionCommentButton, "comment", 16);
    auto *composerButtons = new QHBoxLayout;
    composerButtons->setContentsMargins(0, 0, 0, 0);
    composerButtons->addWidget(makeVoiceButton(m_discussionComposer), 0, Qt::AlignLeft);
    composerButtons->addStretch();
    composerButtons->addWidget(m_discussionCommentButton);

    auto *detailLayout = new QVBoxLayout(detailPane);
    detailLayout->setContentsMargins(18, 18, 12, 18);
    detailLayout->setSpacing(8);
    detailLayout->addLayout(detailHeader);
    detailLayout->addWidget(m_discussionMeta);
    detailLayout->addWidget(m_discussionInlineNotice);
    detailLayout->addWidget(m_discussionThreadScroll, 1);
    detailLayout->addWidget(makeComposerIdentity(nullptr, QStringLiteral("Commenting")));
    detailLayout->addWidget(m_discussionComposer);
    detailLayout->addLayout(composerButtons);

    auto *summaryPane = new QWidget;
    summaryPane->setMinimumWidth(190);
    summaryPane->setMaximumWidth(260);
    auto *summaryTitle = new QLabel("Categories");
    summaryTitle->setObjectName("sectionLabel");
    m_discussionCategorySummary = new QLabel;
    m_discussionCategorySummary->setObjectName("statusLine");
    m_discussionCategorySummary->setTextFormat(Qt::RichText);
    m_discussionCategorySummary->setWordWrap(true);
    auto *summaryLayout = new QVBoxLayout(summaryPane);
    summaryLayout->setContentsMargins(12, 18, 18, 18);
    summaryLayout->setSpacing(8);
    summaryLayout->addWidget(summaryTitle);
    summaryLayout->addWidget(m_discussionCategorySummary);
    summaryLayout->addStretch();

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(listPane);
    splitter->addWidget(detailPane);
    splitter->addWidget(summaryPane);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 0);
    splitter->setSizes({420, 680, 220});

    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(splitter);

    connect(m_discussionSearch, &QLineEdit::textChanged, this,
            [this] { reloadDiscussions(); });
    connect(m_discussionCategoryFilter,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this] { reloadDiscussions(); });
    connect(m_discussionTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QModelIndexList rows =
            m_discussionTable->selectionModel()->selectedRows();
        if (rows.isEmpty())
            return;
        if (QTableWidgetItem *first =
                m_discussionTable->item(rows.first().row(), 0))
            showDiscussion(first->data(Qt::UserRole).toInt());
    });
    connect(m_discussionNewButton, &QPushButton::clicked, this,
            &MainWindow::createDiscussionDialog);
    connect(m_discussionSyncButton, &QPushButton::clicked, this,
            &MainWindow::syncDiscussionsInbox);
    connect(m_discussionCommentButton, &QPushButton::clicked, this,
            &MainWindow::postDiscussionComment);
    connect(m_discussionCloseButton, &QPushButton::clicked, this, [this] {
        for (const Discussion &discussion : std::as_const(m_currentDiscussions))
            if (discussion.number == m_currentDiscussionNumber) {
                setDiscussionStatus(discussion.status == QLatin1String("closed")
                                        ? QStringLiteral("open")
                                        : QStringLiteral("closed"));
                return;
            }
    });
    connect(m_discussionArchiveButton, &QPushButton::clicked, this, [this] {
        for (const Discussion &discussion : std::as_const(m_currentDiscussions))
            if (discussion.number == m_currentDiscussionNumber) {
                setDiscussionStatus(discussion.status == QLatin1String("archived")
                                        ? QStringLiteral("open")
                                        : QStringLiteral("archived"));
                return;
            }
    });
    connect(m_discussionDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteCurrentDiscussion);
    updateDiscussionActionState();
    return page;
}

QString MainWindow::discussionReadStampKey(const QString &repoKey, int number)
{
    return repoKey + QLatin1Char('#') + QString::number(number);
}

QVariantMap MainWindow::discussionReadStamps()
{
    return QSettings()
        .value(QStringLiteral("discussions/readStamps"))
        .toMap();
}

void MainWindow::recordDiscussionRead(const QString &repoKey,
                                      const Discussion &discussion)
{
    if (repoKey.isEmpty() || discussion.number <= 0)
        return;
    const qint64 latest = discussion.updatedAt();
    if (latest <= 0)
        return;
    QSettings settings;
    QVariantMap stamps =
        settings.value(QStringLiteral("discussions/readStamps")).toMap();
    const QString key = discussionReadStampKey(repoKey, discussion.number);
    if (stamps.value(key).toLongLong() >= latest)
        return; // already read at (or past) this activity
    stamps.insert(key, QVariant::fromValue<qlonglong>(latest));
    settings.setValue(QStringLiteral("discussions/readStamps"), stamps);
}

DiscussionStore MainWindow::discussionStoreForCurrentRepo() const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return DiscussionStore(QString(), QString(), &m_profileIdentity,
                               chatDisplayName());
    const RepositoryRecord &target = m_repositories.at(m_repoDetailIndex);
    const RepositoryRecord &repo = writableRecordFor(target);
    DiscussionStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                          chatDisplayName());
    store.setSigningRepository(target.owner, target.name);
    return store;
}

void MainWindow::reloadDiscussionCountInBackground()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const int repoIndex = m_repoDetailIndex;
    const int generation = ++m_discussionCountLoadGen;
    const DiscussionStore store = discussionStoreForCurrentRepo();
    runOffThread<QList<Discussion>>(
        [store] { return store.loadAll(nullptr); },
        [this, repoIndex, generation](QList<Discussion> loaded) {
            if (generation != m_discussionCountLoadGen ||
                repoIndex != m_repoDetailIndex)
                return; // superseded, or the user moved to another repo
            m_currentDiscussions = std::move(loaded);
            updateRepoDiscussionCount();
        });
}

void MainWindow::reloadDiscussions()
{
    if (!m_discussionTable)
        return;
    ++m_discussionCountLoadGen;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        m_currentDiscussions.clear();
        m_currentDiscussionNumber = -1;
        m_discussionTable->setRowCount(0);
        renderDiscussionThread(Discussion());
        updateDiscussionActionState();
        updateRepoDiscussionCount();
        return;
    }

    QString error;
    m_currentDiscussions = discussionStoreForCurrentRepo().loadAll(&error);
    if (!error.trimmed().isEmpty())
        setDiscussionInlineNotice(error, true);

    QHash<QString, int> categoryCounts;
    for (const Discussion &discussion : std::as_const(m_currentDiscussions)) {
        const QString category =
            DiscussionStore::normalizedCategory(discussion.category);
        categoryCounts[category] += 1;
    }
    if (m_discussionCategorySummary) {
        QStringList lines;
        lines << QStringLiteral("<b>Total</b> %1")
                     .arg(formatCount(m_currentDiscussions.size()));
        for (const QString &category : DiscussionStore::categories()) {
            const QString normalized =
                DiscussionStore::normalizedCategory(category);
            lines << QStringLiteral("<b>%1</b> %2")
                         .arg(category.toHtmlEscaped())
                         .arg(formatCount(categoryCounts.value(normalized)));
        }
        m_discussionCategorySummary->setText(lines.join(QStringLiteral("<br>")));
    }

    const QString search =
        m_discussionSearch ? m_discussionSearch->text().trimmed() : QString();
    const QString categoryFilter =
        m_discussionCategoryFilter
            ? m_discussionCategoryFilter->currentData().toString()
            : QString();
    const int keep = m_currentDiscussionNumber;

    QSignalBlocker block(m_discussionTable);
    TableRepaintGuard repaintGuard(m_discussionTable);
    m_discussionTable->setSortingEnabled(false);
    m_discussionTable->setRowCount(0);
    for (const Discussion &discussion : std::as_const(m_currentDiscussions)) {
        const QString category =
            DiscussionStore::normalizedCategory(discussion.category);
        if (!categoryFilter.isEmpty() &&
            category.compare(categoryFilter, Qt::CaseInsensitive) != 0)
            continue;
        if (!search.isEmpty()) {
            const QString hay = QStringLiteral("#%1 %2 %3 %4 %5")
                                    .arg(discussion.number)
                                    .arg(discussion.title, category,
                                         discussion.authorName,
                                         discussion.author);
            if (!hay.contains(search, Qt::CaseInsensitive))
                continue;
        }

        const int row = m_discussionTable->rowCount();
        m_discussionTable->insertRow(row);
        auto *num = new SortTableWidgetItem;
        num->setData(Qt::DisplayRole, discussion.number);
        num->setData(Qt::UserRole, discussion.number);
        num->setData(kTableSortRole, discussion.number);
        m_discussionTable->setItem(row, 0, num);
        m_discussionTable->setItem(row, 1,
                                   new QTableWidgetItem(discussion.title));
        m_discussionTable->setItem(row, 2, new QTableWidgetItem(category));
        m_discussionTable->setItem(row, 3,
                                   new QTableWidgetItem(discussion.status));
        auto *comments = new SortTableWidgetItem;
        comments->setData(Qt::DisplayRole, discussion.commentCount());
        comments->setData(kTableSortRole, discussion.commentCount());
        m_discussionTable->setItem(row, 4, comments);
        const qint64 updated = discussion.updatedAt();
        auto *updatedItem = new SortTableWidgetItem(
            updated > 0 ? formatIssueRelativeTime(updated)
                        : QStringLiteral("-"));
        updatedItem->setData(kTableSortRole, updated);
        if (updated > 0) {
            updatedItem->setToolTip(
                QDateTime::fromMSecsSinceEpoch(updated).toString(
                    QStringLiteral("yyyy-MM-dd HH:mm")));
        }
        m_discussionTable->setItem(row, 5, updatedItem);
        const QString author =
            discussion.authorName.trimmed().isEmpty()
                ? (discussion.author.isEmpty() ? QStringLiteral("-")
                                               : discussion.author.left(8))
                : discussion.authorName.trimmed();
        auto *authorItem = new QTableWidgetItem(author);
        authorItem->setToolTip(discussion.author);
        m_discussionTable->setItem(row, 6, authorItem);
    }
    m_discussionTable->setSortingEnabled(true);
    m_discussionTable->sortItems(5, Qt::DescendingOrder);
    block.unblock();

    int selRow = -1;
    for (int r = 0; r < m_discussionTable->rowCount(); ++r) {
        QTableWidgetItem *item = m_discussionTable->item(r, 0);
        if (item && item->data(Qt::UserRole).toInt() == keep) {
            selRow = r;
            break;
        }
    }
    if (selRow < 0 && m_discussionTable->rowCount() > 0)
        selRow = 0;
    if (selRow >= 0) {
        m_discussionTable->selectRow(selRow);
        if (QTableWidgetItem *first = m_discussionTable->item(selRow, 0))
            showDiscussion(first->data(Qt::UserRole).toInt());
    } else {
        m_currentDiscussionNumber = -1;
        showDiscussion(-1);
    }
    updateDiscussionActionState();
    updateRepoDiscussionCount();
}

void MainWindow::showDiscussion(int number)
{
    const Discussion *found = nullptr;
    for (const Discussion &discussion : std::as_const(m_currentDiscussions)) {
        if (discussion.number == number) {
            found = &discussion;
            break;
        }
    }
    m_currentDiscussionNumber = found ? number : -1;
    scheduleNavRecord();
    if (!found) {
        if (m_discussionTitle)
            m_discussionTitle->setText("Select a discussion");
        if (m_discussionMeta)
            m_discussionMeta->clear();
        renderDiscussionThread(Discussion());
        updateDiscussionActionState();
        return;
    }

    if (m_discussionTitle) {
        m_discussionTitle->setText(
            QStringLiteral("#%1  %2")
                .arg(found->number)
                .arg(found->title));
    }
    if (m_discussionMeta) {
        const QString who =
            found->authorName.isEmpty() ? found->author.left(10)
                                        : found->authorName;
        m_discussionMeta->setText(
            QStringLiteral("<b>%1</b> - %2 - %3 comment%4 - updated %5 - by %6")
                .arg(DiscussionStore::normalizedCategory(found->category)
                         .toHtmlEscaped())
                .arg(found->status.toHtmlEscaped())
                .arg(found->commentCount())
                .arg(found->commentCount() == 1 ? QString()
                                                : QStringLiteral("s"))
                .arg(formatIssueRelativeTime(found->updatedAt()).toHtmlEscaped())
                .arg(userProfileLinkHtml(found->authorName, who)));
    }
    if (m_discussionComposer) {
        m_discussionComposer->setEnabled(found->status == QLatin1String("open"));
        m_discussionComposer->setMentionCandidates(mentionCandidateNames());
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            const RepositoryRecord &repo =
                writableRecordFor(m_repositories.at(m_repoDetailIndex));
            m_discussionComposer->setPreviewBasePath(
                repo.localPath + "/.forkmesh/discussions/" + QString::number(found->number));
        }
    }
    renderDiscussionThread(*found);
    // The thread is on screen now: stamp it read so the tab badge only keeps
    // counting discussions with activity this machine has not displayed yet.
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        recordDiscussionRead(repo.owner + "/" + repo.name, *found);
    }
    updateRepoDiscussionCount();
    updateDiscussionActionState();
}

void MainWindow::renderDiscussionThread(const Discussion &discussion)
{
    if (!m_discussionThreadLayout)
        return;
    while (QLayoutItem *item = m_discussionThreadLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
    m_discussionThreadLayout->addStretch();
    if (discussion.number <= 0)
        return;

    const DiscussionStore threadStore = discussionStoreForCurrentRepo();
    const bool canModerate = threadStore.canWrite();
    const bool canAnswer = threadStore.mayMarkAnswer(discussion);
    const QString answerId = discussion.answerId();
    const int number = discussion.number;
    for (const DiscussionEvent &ev : discussion.events) {
        if (ev.type != QLatin1String("open") &&
            ev.type != QLatin1String("comment"))
            continue;
        const bool isOpen = ev.type == QLatin1String("open");
        const QString who =
            ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
        const bool isAnswer = !isOpen && !answerId.isEmpty() && ev.id == answerId;
        QString header =
            QStringLiteral("<b>%1</b> %2 %3")
                .arg(userProfileLinkHtml(ev.authorName, who),
                     isOpen ? QStringLiteral("opened")
                            : QStringLiteral("commented"),
                     formatIssueRelativeTime(ev.ts).toHtmlEscaped());
        if (isAnswer)
            header += QStringLiteral(" &nbsp;<b style=\"color:%1\">\u2713 Answer</b>")
                          .arg(Theme::nextOrHex(Theme::Role::Success,
                                                QStringLiteral("#3fb950")));
        QList<QPair<QString, std::function<void()>>> extraActions;
        if (!isOpen && canAnswer && !ev.id.isEmpty()) {
            const QString target = isAnswer ? QString() : ev.id;
            extraActions.append(
                {isAnswer ? QStringLiteral("Clear answer")
                          : QStringLiteral("Mark as answer"),
                 [this, number, target] { markDiscussionAnswer(number, target); }});
        }
        std::function<void()> onDelete;
        if (!isOpen && canModerate && !ev.id.isEmpty()) {
            const QString eventId = ev.id;
            onDelete = [this, number, eventId] {
                deleteDiscussionComment(number, eventId);
            };
        }
        addConversationCard(m_discussionThreadLayout, who, header, ev.body,
                            isOpen ? Theme::nextOrHex(Theme::Role::AccentFg,
                                                      QStringLiteral("#58a6ff"))
                                   : QString(),
                            QString(), ev.author, onDelete, extraActions);
    }
    // Private repositories have no ActivityPub actor, so there is no remote
    // thread to show and the lookup would only leak the repo name in a URL.
    if (m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size() && m_networkAccess &&
        !m_repositories.at(m_repoDetailIndex).isPrivate) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        auto *remoteThread = new FederatedThreadView(
            m_networkAccess, m_discussionThreadContainer);
        const QUrl server(canonicalServerUrl(
            m_activeServer >= 0 && m_activeServer < m_servers.size()
                ? m_servers.at(m_activeServer).url
                : QString()));
        remoteThread->load(server, repo.owner, repo.name,
                           QStringLiteral("discussion"), discussion.number);
        m_discussionThreadLayout->insertWidget(
            qMax(0, m_discussionThreadLayout->count() - 1), remoteThread);
    }
    if (m_discussionThreadScroll) {
        QTimer::singleShot(0, this, [this] {
            if (m_discussionThreadScroll && m_discussionThreadScroll->verticalScrollBar())
                m_discussionThreadScroll->verticalScrollBar()->setValue(
                    m_discussionThreadScroll->verticalScrollBar()->minimum());
        });
    }
}

void MainWindow::updateDiscussionActionState()
{
    const bool haveRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    const DiscussionStore store = discussionStoreForCurrentRepo();
    const bool writable = haveRepo && store.canWrite();
    if (m_discussionNewButton) {
        m_discussionNewButton->setEnabled(haveRepo && (writable || m_networkAccess));
        m_discussionNewButton->setToolTip(
            writable ? QStringLiteral("Create a discussion in this repository")
                     : m_networkAccess
                           ? QStringLiteral("Send a signed discussion to the owner")
                           : QStringLiteral("Network access is unavailable"));
    }
    if (m_discussionSyncButton) {
        m_discussionSyncButton->setEnabled(writable);
        m_discussionSyncButton->setToolTip(
            writable ? QStringLiteral("Pull pending discussion submissions")
                     : QStringLiteral("Only the owning node can sync this inbox"));
    }
    refreshPendingInboxBadges();
    const bool haveDiscussion = m_currentDiscussionNumber > 0;
    QString status = QStringLiteral("open");
    for (const Discussion &discussion : std::as_const(m_currentDiscussions))
        if (discussion.number == m_currentDiscussionNumber) {
            status = discussion.status;
            break;
        }
    const bool canParticipate = haveRepo && (writable || m_networkAccess) &&
                                (!haveDiscussion || status == QLatin1String("open"));
    if (m_discussionCloseButton) {
        m_discussionCloseButton->setEnabled(writable && haveDiscussion &&
                                            status != QLatin1String("archived"));
        m_discussionCloseButton->setText(status == QLatin1String("closed")
                                             ? QStringLiteral("Reopen discussion")
                                             : QStringLiteral("Close discussion"));
    }
    if (m_discussionArchiveButton) {
        m_discussionArchiveButton->setEnabled(writable && haveDiscussion);
        m_discussionArchiveButton->setText(
            status == QLatin1String("archived")
                ? QStringLiteral("Unarchive discussion")
                : QStringLiteral("Archive discussion"));
    }
    if (m_discussionDeleteButton)
        m_discussionDeleteButton->setEnabled(writable && haveDiscussion);
    if (m_discussionComposer) {
        m_discussionComposer->setEnabled(canParticipate);
        m_discussionComposer->setPlaceholderText(
            haveDiscussion
                ? QStringLiteral("Add to the discussion...")
                : QStringLiteral("Start a discussion — just type, the title is "
                                 "taken from your first line..."));
    }
    if (m_discussionCommentButton) {
        m_discussionCommentButton->setEnabled(canParticipate);
        m_discussionCommentButton->setText(
            haveDiscussion ? QStringLiteral("Comment")
                           : QStringLiteral("Start discussion"));
        setOcticon(m_discussionCommentButton, haveDiscussion ? "comment" : "plus",
                   16);
        const QString noun =
            haveDiscussion ? QStringLiteral("comment") : QStringLiteral("discussion");
        m_discussionCommentButton->setToolTip(
            writable ? (haveDiscussion ? QStringLiteral("Add a comment")
                                       : QStringLiteral("Start a discussion"))
                     : m_networkAccess
                           ? QStringLiteral("Send a signed %1 to the owner").arg(noun)
                           : QStringLiteral("Network access is unavailable"));
    }
}

void MainWindow::setDiscussionStatus(const QString &status)
{
    if (m_currentDiscussionNumber <= 0)
        return;
    DiscussionStore store = discussionStoreForCurrentRepo();
    QString error;
    if (!store.setStatus(m_currentDiscussionNumber, status, &error)) {
        setDiscussionInlineNotice(
            error.isEmpty() ? QStringLiteral("Could not update the discussion.")
                            : error,
            true);
        return;
    }
    const int number = m_currentDiscussionNumber;
    reloadDiscussions();
    showDiscussion(number);
    propagateRepoUpdate(m_repoDetailIndex);
    setDiscussionInlineNotice(
        status == QLatin1String("closed")
            ? QStringLiteral("Discussion closed.")
            : status == QLatin1String("archived")
                  ? QStringLiteral("Discussion archived.")
                  : QStringLiteral("Discussion reopened."));
}

void MainWindow::deleteCurrentDiscussion()
{
    if (m_currentDiscussionNumber <= 0)
        return;
    const int number = m_currentDiscussionNumber;
    if (QMessageBox::question(
            this, QStringLiteral("Delete discussion"),
            QStringLiteral("Delete discussion #%1? This cannot be undone.")
                .arg(number),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;
    DiscussionStore store = discussionStoreForCurrentRepo();
    QString error;
    if (!store.deleteDiscussion(number, &error)) {
        setDiscussionInlineNotice(
            error.isEmpty() ? QStringLiteral("Could not delete the discussion.")
                            : error,
            true);
        return;
    }
    m_currentDiscussionNumber = -1;
    reloadDiscussions();
    propagateRepoUpdate(m_repoDetailIndex);
    setDiscussionInlineNotice(QStringLiteral("Discussion deleted."));
}

void MainWindow::createDiscussionDialog()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("New discussion");
    auto *titleEdit = new QLineEdit(&dialog);
    titleEdit->setPlaceholderText("Title");
    auto *categoryCombo = new QComboBox(&dialog);
    categoryCombo->addItems(DiscussionStore::categories());
    auto *bodyEdit = new MarkdownEditor(&dialog);
    bodyEdit->setPlaceholderText("Start the discussion...");
    bodyEdit->setMinimumHeight(220);
    bodyEdit->setMentionCandidates(mentionCandidateNames());
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const RepositoryRecord &writable = writableRecordFor(repo);
    if (!writable.localPath.isEmpty())
        bodyEdit->setPreviewBasePath(writable.localPath);

    auto *form = new QFormLayout;
    form->addRow("Title", titleEdit);
    form->addRow("Category", categoryCombo);
    form->addRow("Body", bodyEdit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(makeComposerIdentity(nullptr, QStringLiteral("Posting")));
    layout->addLayout(form);
    layout->addWidget(buttons);
    dialog.resize(560, 460);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString title = titleEdit->text().trimmed();
    const QString body = bodyEdit->markdown().trimmed();
    const QString category =
        DiscussionStore::normalizedCategory(categoryCombo->currentText());
    if (title.isEmpty()) {
        setDiscussionInlineNotice("A title is required.", true);
        return;
    }
    if (body.isEmpty()) {
        setDiscussionInlineNotice("Write a body for the discussion.", true);
        return;
    }

    DiscussionStore store = discussionStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        const int number = store.createDiscussion(title, body, category, &error);
        if (number < 0) {
            setDiscussionInlineNotice(
                error.isEmpty() ? "Could not create the discussion." : error,
                true);
            return;
        }
        m_currentDiscussionNumber = number;
        reloadDiscussions();
        showDiscussion(number);
        propagateRepoUpdate(m_repoDetailIndex);
        setDiscussionInlineNotice("Discussion created.");
        return;
    }

    DiscussionEvent ev;
    ev.type = QStringLiteral("open");
    ev.title = title;
    ev.category = category;
    ev.body = body;
    ev = store.makeSignedEvent(0, ev);
    submitDiscussionEventToInbox(0, ev, title);
}

QString MainWindow::discussionTitleFromBody(const QString &body)
{
    static const QRegularExpression prefix(
        QStringLiteral("^(#{1,6}\\s+|>\\s*|[-*+]\\s+|\\d+[.)]\\s+)+"));
    QString title;
    const QStringList lines = body.split('\n');
    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        line.remove(prefix);
        line = line.trimmed();
        if (!line.isEmpty()) {
            title = line;
            break;
        }
    }
    if (title.isEmpty())
        title = QStringLiteral("Untitled discussion");

    constexpr int maxLen = 80;
    if (title.size() > maxLen) {
        QString clipped = title.left(maxLen);
        const int space = clipped.lastIndexOf(' ');
        if (space > maxLen / 2)
            clipped.truncate(space);
        title = clipped.trimmed() + QStringLiteral("…");
    }
    return title;
}

void MainWindow::startDiscussionFromComposer()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        !m_discussionComposer)
        return;
    const QString body = m_discussionComposer->markdown().trimmed();
    if (body.isEmpty()) {
        setDiscussionInlineNotice("Type something to start a discussion.", true);
        return;
    }
    const QString title = discussionTitleFromBody(body);
    const QString category =
        DiscussionStore::normalizedCategory(QStringLiteral("Ideas"));

    DiscussionStore store = discussionStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        const int number = store.createDiscussion(title, body, category, &error);
        if (number < 0) {
            setDiscussionInlineNotice(
                error.isEmpty() ? "Could not create the discussion." : error, true);
            return;
        }
        m_discussionComposer->setMarkdown(QString());
        m_currentDiscussionNumber = number;
        reloadDiscussions();
        showDiscussion(number);
        propagateRepoUpdate(m_repoDetailIndex);
        setDiscussionInlineNotice("Discussion created.");
        return;
    }

    DiscussionEvent ev;
    ev.type = QStringLiteral("open");
    ev.title = title;
    ev.category = category;
    ev.body = body;
    ev = store.makeSignedEvent(0, ev);
    submitDiscussionEventToInbox(0, ev, title);
}

void MainWindow::postDiscussionComment()
{
    if (!m_discussionComposer)
        return;
    if (m_currentDiscussionNumber <= 0) {
        startDiscussionFromComposer();
        return;
    }
    for (const Discussion &discussion : std::as_const(m_currentDiscussions))
        if (discussion.number == m_currentDiscussionNumber &&
            discussion.status != QLatin1String("open")) {
            setDiscussionInlineNotice(
                QStringLiteral("This discussion is %1.").arg(discussion.status),
                true);
            return;
        }
    const QString body = m_discussionComposer->markdown().trimmed();
    if (body.isEmpty()) {
        setDiscussionInlineNotice("Write a comment first.", true);
        return;
    }

    DiscussionStore store = discussionStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        if (!store.addComment(m_currentDiscussionNumber, body, &error)) {
            setDiscussionInlineNotice(
                error.isEmpty() ? "Could not add the comment." : error, true);
            return;
        }
        m_discussionComposer->setMarkdown(QString());
        reloadDiscussions();
        showDiscussion(m_currentDiscussionNumber);
        propagateRepoUpdate(m_repoDetailIndex);
        setDiscussionInlineNotice("Comment added.");
        return;
    }

    DiscussionEvent ev;
    ev.type = QStringLiteral("comment");
    ev.body = body;
    ev = store.makeSignedEvent(m_currentDiscussionNumber, ev);
    submitDiscussionEventToInbox(m_currentDiscussionNumber, ev);
}

void MainWindow::deleteDiscussionComment(int number, const QString &eventId)
{
    if (number <= 0 || eventId.isEmpty())
        return;
    if (QMessageBox::question(
            this, QStringLiteral("Delete comment"),
            QStringLiteral("Delete this comment from the discussion? This "
                           "cannot be undone."),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;

    DiscussionStore store = discussionStoreForCurrentRepo();
    QString error;
    if (!store.deleteComment(number, eventId, &error)) {
        setDiscussionInlineNotice(
            error.isEmpty() ? QStringLiteral("Could not delete the comment.")
                            : error,
            true);
        return;
    }
    reloadDiscussions();
    showDiscussion(number);
    propagateRepoUpdate(m_repoDetailIndex);
    setDiscussionInlineNotice(QStringLiteral("Comment deleted."));
}

void MainWindow::markDiscussionAnswer(int number, const QString &commentId)
{
    if (number <= 0)
        return;
    DiscussionStore store = discussionStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        if (!store.markAnswer(number, commentId, &error)) {
            setDiscussionInlineNotice(
                error.isEmpty() ? QStringLiteral("Could not mark the answer.") : error,
                true);
            return;
        }
        reloadDiscussions();
        showDiscussion(number);
        propagateRepoUpdate(m_repoDetailIndex);
        setDiscussionInlineNotice(commentId.isEmpty()
                                      ? QStringLiteral("Answer cleared.")
                                      : QStringLiteral("Marked as the answer."));
        return;
    }
    DiscussionEvent ev;
    ev.type = QStringLiteral("answer");
    ev.target = commentId;
    submitDiscussionEventToInbox(number, store.makeSignedEvent(number, ev));
}

void MainWindow::submitDiscussionEventToInbox(int number, const DiscussionEvent &ev,
                                              const QString &titleIfNew)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (!m_networkAccess) {
        setDiscussionInlineNotice("Network access is unavailable.", true);
        return;
    }
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QUrl inboxUrl = discussionsApiUrl(repo);
    const QString inboxBackoffKey =
        inboxUrl.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    if (m_discussionInboxBackoff.shouldBackOff(
            inboxBackoffKey, QDateTime::currentMSecsSinceEpoch())) {
        setDiscussionInlineNotice(
            "This relay does not expose discussion inbox submissions for this "
            "repo yet. Writable clones can still create local discussions.",
            true);
        return;
    }
    QJsonObject eventJson = ev.toJson();
    eventJson.insert(QStringLiteral("body"), ev.body);
    const QJsonObject payload{{"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", number},
                              {"titleIfNew", titleIfNew},
                              {"event", eventJson}};
    QNetworkRequest request(inboxUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, type = ev.type, inboxBackoffKey] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            m_discussionInboxBackoff.clear(inboxBackoffKey);
            if (type == QLatin1String("comment") && m_discussionComposer)
                m_discussionComposer->setMarkdown(QString());
            setDiscussionInlineNotice(
                type == QLatin1String("open")
                    ? QStringLiteral("Your signed discussion was delivered to %1/%2.")
                          .arg(repo.owner, repo.name)
                : type == QLatin1String("answer")
                    ? QStringLiteral("Your answer choice was delivered to %1/%2.")
                          .arg(repo.owner, repo.name)
                    : QStringLiteral("Your signed comment was delivered to %1/%2.")
                          .arg(repo.owner, repo.name));
        } else {
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::ContentNotFoundError ||
                status == 404) {
                m_discussionInboxBackoff.markUnsupported(
                    inboxBackoffKey, QDateTime::currentMSecsSinceEpoch());
                setDiscussionInlineNotice(
                    "This relay does not expose discussion inbox submissions for "
                    "this repo yet. Writable clones can still create local "
                    "discussions.",
                    true);
                return;
            }
            setDiscussionInlineNotice(
                "Could not send the discussion update: " + reply->errorString(),
                true);
        }
    });
}

QUrl MainWindow::discussionsApiUrl(const RepositoryRecord &repo) const
{
    QUrl url = catalogApiUrl();
    url.setPath("/api/repo/" + repoSegment(repo.owner, QStringLiteral("owner")) +
                "/" + repoSegment(repo.name, QStringLiteral("repository")) +
                "/discussions");
    return url;
}

void MainWindow::syncDiscussionsInbox()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (!m_networkAccess) {
        setDiscussionInlineNotice("Network access is unavailable.", true);
        return;
    }
    showPendingInbox(m_repositories.at(m_repoDetailIndex),
                     QStringLiteral("discussions"));
}

void MainWindow::drainDiscussionsInboxFor(RepositoryRecord repo,
										  bool interactive)
{
    if (!m_networkAccess) {
        if (interactive)
            setDiscussionInlineNotice("Network access is unavailable.", true);
        return;
    }
    // A local-only repository has no relay inbox to drain, and the drain URL
    // would carry its owner and name to the relay. Fail closed before any
    // signing or network work.
    if (!PrivateMirrorRuntime::repositoryRelayInboxAllowed(
            repo.publishToNetwork))
        return;
    const RepositoryRecord writable = writableRecordFor(repo);
	DiscussionStore probe(writable.localPath, writable.mirrorPath,
						  &m_profileIdentity, m_userName);
	if (!probe.canWrite())
        return;

    QUrl url = discussionsApiUrl(repo);
	const QString signer =
		repoSegment(repo.owner, QStringLiteral("owner"));
	if (signer.isEmpty() || !hasOwnerSigningCapability())
        return;
	const QString inboxBackoffKey =
		url.toString(QUrl::RemoveQuery | QUrl::RemoveFragment);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_discussionInboxBackoff.shouldBackOff(inboxBackoffKey, nowMs)) {
        if (interactive)
            setDiscussionInlineNotice(
                "This relay does not expose discussion inbox sync for this repo "
                "yet. Local discussions still work.");
        return;
    }
    if (!interactive && !m_pollBackoff.ready(inboxBackoffKey, nowMs))
        return;
    const QString ts = QString::number(nowMs);
    const QByteArray canonical =
        ("forkmesh-issues-pull-v1\n" + signer + "\n" + ts).toUtf8();
    const QString sig = m_profileIdentity.signData(canonical);
    QUrlQuery query;
    query.addQueryItem("owner", signer);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", sig);
    url.setQuery(query);

    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, interactive, inboxBackoffKey] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply->error() == QNetworkReply::ContentNotFoundError ||
                status == 404) {
                m_discussionInboxBackoff.markUnsupported(
                    inboxBackoffKey, QDateTime::currentMSecsSinceEpoch());
                if (interactive)
                    setDiscussionInlineNotice(
                        "This relay does not expose discussion inbox sync for "
                        "this repo yet. Local discussions still work.");
                return;
            }
			noteInboxDrainFailure(inboxBackoffKey, status);
            if (interactive)
                setDiscussionInlineNotice("Could not reach the inbox: " +
                                              reply->errorString(),
                                          true);
            return;
        }
        m_discussionInboxBackoff.clear(inboxBackoffKey);
        noteInboxDrainSuccess(inboxBackoffKey);
        const QJsonArray pending =
            QJsonDocument::fromJson(reply->readAll())
                .object()
                .value("pending")
                .toArray();
		applyDiscussionsInboxPayload(repo, pending, interactive);
    });
}

void MainWindow::applyDiscussionsInboxPayload(const RepositoryRecord &repo,
                                              const QJsonArray &pending,
											  bool interactive)
{
    if (!hasOwnerSigningCapability())
        return;
    if (pending.isEmpty()) {
        if (interactive)
            setDiscussionInlineNotice("No pending discussion submissions.");
        return;
    }
    const RepositoryRecord writable = writableRecordFor(repo);
    DiscussionStore store(writable.localPath, writable.mirrorPath,
                          &m_profileIdentity, m_userName);
    if (!store.canWrite())
        return;
    int merged = 0;
    int comments = 0;
    int newDiscussions = 0;
    QStringList drainedIds;
    QString lastAuthor;
    QString lastTitle;
    int lastNumber = 0;
    for (const QJsonValue &value : pending) {
        const QJsonObject item = value.toObject();
        const QString inboxId =
            item.value("id").isDouble()
                ? QString::number(
                      static_cast<qint64>(item.value("id").toDouble()))
                : QString();
        const int number = item.value("number").toInt();
        const QJsonObject eventObj = item.value("event").toObject();
        DiscussionEvent ev = DiscussionEvent::fromJson(eventObj);
        if (ev.body.isEmpty())
            ev.body = eventObj.value("body").toString();
        const QString titleIfNew = item.value("titleIfNew").toString();
        QString error;
        const bool ownerApproved = item.value("meta").toObject()
                                       .value("ownerApproved").toBool();
        if (!store.applyRemoteEvent(number, ev, titleIfNew, &error,
                                    ownerApproved))
            continue;
        if (!inboxId.isEmpty())
            drainedIds << inboxId;
        ++merged;
        const QString who =
            ev.authorName.isEmpty() ? ev.author.left(8) : ev.authorName;
        lastAuthor = who;
        lastNumber = number;
        if (ev.type == QLatin1String("open")) {
            ++newDiscussions;
            lastTitle = ev.title.isEmpty() ? titleIfNew : ev.title;
        } else if (ev.type == QLatin1String("comment")) {
            ++comments;
            lastTitle = QStringLiteral("comment on #%1").arg(number);
        } else if (ev.type == QLatin1String("answer")) {
            lastTitle = QStringLiteral("answer on #%1").arg(number);
        }
    }
    if (!drainedIds.isEmpty()) {
        QUrl ackUrl = discussionsApiUrl(repo);
		const QString ackSigner =
			repoSegment(repo.owner, QStringLiteral("owner"));
        QUrlQuery query = signedInboxQuery(ackSigner);
        query.addQueryItem(QStringLiteral("ids"),
                           drainedIds.join(QStringLiteral(",")));
        ackUrl.setQuery(query);
        m_networkAccess->deleteResource(QNetworkRequest(ackUrl));
    }

    const bool onThisRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size() &&
        m_repositories.at(m_repoDetailIndex).owner == repo.owner &&
        m_repositories.at(m_repoDetailIndex).name == repo.name;
    if (onThisRepo)
        reloadDiscussions();
	if (merged > 0) {
        propagateRepoUpdate(repoIndexFor(repo.owner, repo.name));
        scanRepoMentionsFor(writable);
    }
    if (interactive) {
        setDiscussionInlineNotice(
            QStringLiteral("Merged %1 discussion submission(s).").arg(merged));
    } else if (merged > 0) {
        QString body;
        if (newDiscussions > 0) {
            body = newDiscussions == 1
                       ? QStringLiteral("%1 opened a discussion on %2/%3: %4")
                             .arg(lastAuthor, repo.owner, repo.name, lastTitle)
                       : QStringLiteral("%1 new discussions on %2/%3")
                             .arg(newDiscussions)
                             .arg(repo.owner, repo.name);
        } else if (comments > 0) {
            body = comments == 1
                       ? QStringLiteral("%1 commented on %2/%3 discussion #%4")
                             .arg(lastAuthor, repo.owner, repo.name)
                             .arg(lastNumber)
                       : QStringLiteral("%1 new discussion comments on %2/%3")
                             .arg(comments)
                             .arg(repo.owner, repo.name);
        }
        if (!body.isEmpty()) {
            flashPingMessage(body);
            NotificationLink link;
            link.kind = QStringLiteral("discussion");
            link.owner = repo.owner;
            link.name = repo.name;
            link.number = merged == 1 ? lastNumber : -1;
            addNotification(QStringLiteral("Discussion update"), body, false,
                            link);
        }
    }
}

void MainWindow::setDiscussionInlineNotice(const QString &message, bool isError)
{
    showLocalNotice(m_discussionInlineNotice, message, isError);
}
