#include "DjBrowserView.h"
#include "DjModel.h"
#include "DjWidgets.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDrag>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::djstudio {

namespace {

const char songMime[] = "application/x-forkmesh-dj-song";

QString treeStyle()
{
    return QStringLiteral(
        "QTreeWidget{background:#111214;color:#e6e8eb;border:1px solid #3c3e43;font-size:12px;outline:0;"
        "alternate-background-color:#17181b;}"
        "QTreeWidget::item{height:24px;}"
        "QTreeWidget::item:selected{background:#1d4f78;color:#ffffff;}"
        "QTreeWidget::item:hover{background:#23262b;}"
        "QHeaderView::section{background:#2a2c30;color:#8c9199;border:0;border-right:1px solid #3c3e43;"
        "padding:3px 6px;font-size:11px;font-weight:bold;}"
        "QScrollBar:vertical{background:#111214;width:10px;}QScrollBar::handle:vertical{background:#3c3e43;border-radius:4px;min-height:24px;}"
        "QScrollBar::add-line,QScrollBar::sub-line{height:0;}");
}

QString lengthText(double seconds) { return seconds > 0 ? studio::clock(seconds) : QString(); }

}

DjBrowserView::DjBrowserView(DjModel *model, QWidget *parent) : QWidget(parent), m_model(model)
{
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setHandleWidth(3);
    split->setStyleSheet(QStringLiteral("QSplitter::handle{background:#15161a;}"));
    outer->addWidget(split);

    m_sources = new QTreeWidget(split);
    m_sources->setHeaderHidden(true);
    m_sources->setStyleSheet(treeStyle());
    m_sources->setRootIsDecorated(true);
    m_sources->setIndentation(14);
    m_sources->setContextMenuPolicy(Qt::CustomContextMenu);
    m_sources->setMinimumWidth(180);
    connect(m_sources, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_building || !item)
            return;
        const QString key = item->data(0, Qt::UserRole).toString();
        if (key.isEmpty())
            return;
        m_source = key;
        if (key == QLatin1String("similar")) {
            const int live = bestDeck() == 0 ? 1 : 0;
            const QString id = m_model->deck(live).loaded() ? m_model->deck(live).videoId : m_model->deck(1 - live).videoId;
            if (!id.isEmpty())
                m_model->findSimilar(id);
        }
        refreshList();
    });
    connect(m_sources, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        sourceMenu(m_sources->itemAt(point), m_sources->viewport()->mapToGlobal(point));
    });

    auto *center = new QWidget(split);
    auto *column = new QVBoxLayout(center);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);
    auto *bar = new QHBoxLayout;
    bar->setSpacing(4);
    m_search = new QLineEdit(center);
    m_search->setPlaceholderText(QStringLiteral("Search YouTube (Enter), filter this list, or paste a YouTube link"));
    m_search->setClearButtonEnabled(true);
    m_search->setStyleSheet(QStringLiteral("QLineEdit{background:#111214;color:#e6e8eb;border:1px solid #3c3e43;border-radius:3px;"
                                           "padding:3px 6px;font-size:12px;}QLineEdit:focus{border-color:#23a6ff;}"));
    connect(m_search, &QLineEdit::textChanged, this, [this]() {
        if (m_source != QLatin1String("search"))
            refreshList();
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this]() { runSearch(); });
    m_count = new QLabel(center);
    m_count->setStyleSheet(QStringLiteral("color:#8c9199;font-size:11px;"));
    auto *importButton = new DjButton(QStringLiteral("IMPORT"), center);
    importButton->setFixedSize(54, 22);
    importButton->setFontSize(9);
    importButton->setToolTip(QStringLiteral("Import playlists: a ForkMesh playlist file, an M3U list or any text with YouTube links"));
    importButton->clicked = [this]() {
        if (importRequested)
            importRequested();
    };
    importButton->rightClicked = [this]() {
        if (pasteRequested)
            pasteRequested();
    };
    auto *exportButton = new DjButton(QStringLiteral("EXPORT"), center);
    exportButton->setFixedSize(54, 22);
    exportButton->setFontSize(9);
    exportButton->setToolTip(QStringLiteral("Export every playlist and tag"));
    exportButton->clicked = [this]() {
        if (exportRequested)
            exportRequested(DjFocus::everything());
    };
    bar->addWidget(m_search, 1);
    bar->addWidget(m_count);
    bar->addWidget(importButton);
    bar->addWidget(exportButton);
    column->addLayout(bar);
    m_list = new QTreeWidget(center);
    m_list->setStyleSheet(treeStyle());
    m_list->setRootIsDecorated(false);
    m_list->setAlternatingRowColors(true);
    m_list->setUniformRowHeights(true);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setIconSize(QSize(32, 20));
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setHeaderLabels({QStringLiteral("#"), QStringLiteral("Title"), QStringLiteral("Artist"), QStringLiteral("Plays"),
                             QStringLiteral("Length"), QStringLiteral("Info")});
    m_list->header()->setStretchLastSection(true);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_list->setColumnWidth(0, 64);
    m_list->setColumnWidth(1, 320);
    m_list->setColumnWidth(2, 170);
    m_list->setColumnWidth(3, 50);
    m_list->setColumnWidth(4, 56);
    m_list->viewport()->installEventFilter(this);
    m_list->installEventFilter(this);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) { activate(rowFor(item)); });
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            showInfo(rowFor(item));
    });
    connect(m_list, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint &point) { rowMenu(m_list->viewport()->mapToGlobal(point)); });
    column->addWidget(m_list, 1);
    auto *suggest = new QHBoxLayout;
    suggest->setSpacing(4);
    auto *previous = new DjButton(QStringLiteral("◀"), center);
    previous->setFixedSize(22, 20);
    previous->setColor(QColor(0xff, 0x3d, 0x9a));
    previous->clicked = [this]() {
        if (!m_suggestions.isEmpty())
            m_suggestionIndex = (m_suggestionIndex + m_suggestions.size() - 1) % m_suggestions.size();
        refreshSuggestion();
    };
    m_suggestion = new QLabel(center);
    m_suggestion->setStyleSheet(QStringLiteral("color:#e6e8eb;font-size:12px;"));
    m_suggestion->setCursor(Qt::PointingHandCursor);
    m_suggestion->setToolTip(QStringLiteral("Click to queue this suggestion"));
    m_suggestion->installEventFilter(this);
    auto *next = new DjButton(QStringLiteral("▶"), center);
    next->setFixedSize(22, 20);
    next->setColor(QColor(0xff, 0x3d, 0x9a));
    next->clicked = [this]() {
        if (!m_suggestions.isEmpty())
            m_suggestionIndex = (m_suggestionIndex + 1) % m_suggestions.size();
        refreshSuggestion();
    };
    suggest->addWidget(previous);
    suggest->addWidget(m_suggestion, 1);
    suggest->addWidget(next);
    column->addLayout(suggest);

    auto *info = new QWidget(split);
    info->setMinimumWidth(190);
    auto *infoColumn = new QVBoxLayout(info);
    infoColumn->setContentsMargins(8, 6, 8, 6);
    infoColumn->setSpacing(4);
    m_infoArt = new QLabel(info);
    m_infoArt->setFixedSize(160, 90);
    m_infoArt->setStyleSheet(QStringLiteral("background:#0c0d0f;border:1px solid #3c3e43;"));
    m_infoArt->setAlignment(Qt::AlignCenter);
    m_infoTitle = new QLabel(info);
    m_infoTitle->setWordWrap(true);
    m_infoTitle->setStyleSheet(QStringLiteral("color:#e6e8eb;font-size:13px;font-weight:bold;"));
    m_infoArtist = new QLabel(info);
    m_infoArtist->setWordWrap(true);
    m_infoArtist->setStyleSheet(QStringLiteral("color:#23a6ff;font-size:12px;font-weight:bold;"));
    m_infoDetails = new QLabel(info);
    m_infoDetails->setWordWrap(true);
    m_infoDetails->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_infoDetails->setStyleSheet(QStringLiteral("color:#8c9199;font-size:11px;"));
    m_infoDetails->setTextFormat(Qt::PlainText);
    auto *infoButtons = new QHBoxLayout;
    infoButtons->setSpacing(3);
    for (int deck = 0; deck < 4; ++deck) {
        auto *load = new DjButton(QString::number(deck + 1), info);
        load->setFixedSize(26, 22);
        load->setColor(theme::deck(deck));
        load->setLit(true);
        load->setToolTip(QStringLiteral("Load on deck %1").arg(deck + 1));
        load->clicked = [this, deck]() {
            for (const Row &row : m_rows) {
                if (row.id == m_infoId) {
                    activate(row, deck);
                    return;
                }
            }
            if (studio::isVideoId(m_infoId))
                m_model->load(deck, m_infoId);
        };
        infoButtons->addWidget(load);
        m_loadButtons.append(load);
    }
    auto *queue = new DjButton(QStringLiteral("QUEUE"), info);
    queue->setFixedHeight(22);
    queue->setFontSize(9);
    queue->clicked = [this]() {
        if (!studio::isVideoId(m_infoId))
            return;
        const DjTrack track = m_model->song(m_infoId);
        m_model->enqueue(track.id, track.title, track.author);
    };
    infoButtons->addWidget(queue);
    infoColumn->addWidget(m_infoArt);
    infoColumn->addWidget(m_infoTitle);
    infoColumn->addWidget(m_infoArtist);
    infoColumn->addLayout(infoButtons);
    infoColumn->addWidget(m_infoDetails, 1);

    split->addWidget(m_sources);
    split->addWidget(center);
    split->addWidget(info);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes({200, 760, 210});

    refreshSources();
    refreshList();
}

void DjBrowserView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), theme::panel());
}

QString DjBrowserView::source() const { return m_source; }

void DjBrowserView::showSource(const QString &key)
{
    m_source = key;
    refreshSources();
    refreshList();
}

void DjBrowserView::setQuery(const QString &query)
{
    if (m_search->text() != query)
        m_search->setText(query);
}

void DjBrowserView::focusSearch()
{
    m_search->setFocus();
    m_search->selectAll();
}

int DjBrowserView::bestDeck() const
{
    const int count = m_model->deckCount();
    const int incoming = m_model->crossfader() <= 0 ? 1 : 0;
    if (!m_model->deck(incoming).playing)
        return incoming;
    for (int i = 0; i < count; ++i) {
        if (!m_model->deck(i).playing)
            return i;
    }
    return -1;
}

void DjBrowserView::refreshSources()
{
    m_building = true;
    m_sources->clear();
    QTreeWidgetItem *selected = nullptr;
    auto add = [&](QTreeWidgetItem *parent, const QString &text, const QString &key, const QString &glyph) {
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_sources);
        item->setText(0, glyph.isEmpty() ? text : glyph + QStringLiteral("  ") + text);
        item->setData(0, Qt::UserRole, key);
        if (key == m_source)
            selected = item;
        return item;
    };
    add(nullptr, QStringLiteral("YouTube Search"), QStringLiteral("search"), QStringLiteral("⌕"));
    add(nullptr, QStringLiteral("Queue (%1)").arg(m_model->shownQueue().size()), QStringLiteral("queue"), QStringLiteral("☰"));
    add(nullptr, QStringLiteral("Recently Played"), QStringLiteral("history"), QStringLiteral("◷"));
    add(nullptr, QStringLiteral("Most Played"), QStringLiteral("most"), QStringLiteral("★"));
    add(nullptr, QStringLiteral("All Songs"), QStringLiteral("all"), QStringLiteral("♫"));
    add(nullptr, QStringLiteral("Like the Playing Song"), QStringLiteral("similar"), QStringLiteral("✦"));
    const DjCrate &crate = m_model->crate();
    auto *playlists = add(nullptr, QStringLiteral("Playlists"), QString(), QStringLiteral("▤"));
    playlists->setData(0, Qt::UserRole + 1, QStringLiteral("playlists"));
    for (const DjPlaylist &playlist : crate.playlists)
        add(playlists, QStringLiteral("%1 (%2)").arg(playlist.name).arg(playlist.songs.size()), DjFocus::playlist(playlist.id).key(), QString());
    auto *tags = add(nullptr, QStringLiteral("Tags"), QString(), QStringLiteral("#"));
    tags->setData(0, Qt::UserRole + 1, QStringLiteral("tags"));
    for (const DjTagCount &tag : crate.tagCounts())
        add(tags, QStringLiteral("#%1 (%2)").arg(tag.name).arg(tag.count), DjFocus::tag(tag.name).key(), QString());
    auto *smartRoot = add(nullptr, QStringLiteral("Smart Playlists"), QString(), QStringLiteral("⚙"));
    smartRoot->setData(0, Qt::UserRole + 1, QStringLiteral("smart"));
    for (DjSmartKind kind : smart::kinds()) {
        QTreeWidgetItem *group = nullptr;
        for (const DjSmartList &list : crate.smart) {
            if (list.kind != kind)
                continue;
            if (!group)
                group = add(smartRoot, smart::kindTitle(kind), QString(), QString());
            add(group, QStringLiteral("%1 (%2)").arg(list.name).arg(list.songs.size()), DjFocus::smart(list.id).key(), QString());
        }
    }
    add(nullptr, QStringLiteral("Sampler"), QStringLiteral("sampler"), QStringLiteral("◉"));
    m_sources->expandAll();
    if (selected)
        m_sources->setCurrentItem(selected);
    m_building = false;
}

QList<DjBrowserView::Row> DjBrowserView::rows() const
{
    QList<Row> out;
    const DjCrate &crate = m_model->crate();
    auto track = [this](const DjTrack &t, const QString &info = QString()) {
        Row row;
        row.id = t.id;
        row.title = t.shownTitle();
        row.author = t.author;
        row.plays = m_model->plays(t.id);
        QStringList bits;
        if (!info.isEmpty())
            bits << info;
        if (!t.tags.isEmpty())
            bits << QStringLiteral("#") + t.tags.join(QStringLiteral(" #"));
        if (t.noShuffle)
            bits << QStringLiteral("Not shuffled");
        row.info = bits.join(QStringLiteral("  "));
        return row;
    };
    if (m_source == QLatin1String("search")) {
        for (const DjSearchResult &result : m_model->searchResults()) {
            Row row = track(m_model->song(result.id));
            row.title = result.title;
            row.author = result.author;
            row.seconds = result.seconds;
            out.append(row);
        }
    } else if (m_source == QLatin1String("queue")) {
        const QDateTime today = QDateTime::currentDateTime();
        for (const DjQueueItem &item : m_model->shownQueue()) {
            Row row = track(m_model->song(item.videoId));
            if (!item.title.isEmpty())
                row.title = item.title;
            if (!item.author.isEmpty())
                row.author = item.author;
            row.queueId = item.id;
            QString when;
            if (item.addedAt > 0) {
                const QDateTime at = QDateTime::fromMSecsSinceEpoch(item.addedAt);
                when = at.date() == today.date() ? at.toString(QStringLiteral("HH:mm")) : at.toString(QStringLiteral("MMM d, HH:mm"));
            }
            row.info = item.addedBy.isEmpty() ? (when.isEmpty() ? QString() : QStringLiteral("Added %1").arg(when))
                                              : QStringLiteral("Added by %1 %2").arg(item.addedBy, when);
            out.append(row);
        }
    } else if (m_source == QLatin1String("history")) {
        for (const DjHistoryItem &item : m_model->history()) {
            Row row = track(m_model->song(item.id));
            if (!item.title.isEmpty())
                row.title = item.title;
            if (!item.author.isEmpty())
                row.author = item.author;
            out.append(row);
        }
    } else if (m_source == QLatin1String("most")) {
        for (const DjPlayedSong &song : studio::mostPlayed(m_model->library()))
            out.append(track(m_model->song(song.id)));
    } else if (m_source == QLatin1String("all")) {
        for (const DjTrack &t : m_model->librarySongs())
            out.append(track(t));
    } else if (m_source == QLatin1String("similar")) {
        QSet<QString> seen;
        const QString seed = m_model->similarSeed();
        if (!seed.isEmpty()) {
            for (const DjSimilarSong &similar : m_model->similarSongs(seed)) {
                seen.insert(similar.track.id);
                out.append(track(similar.track, QStringLiteral("In your library: ") + similar.reasons.join(QStringLiteral(", "))));
            }
            for (const DjSearchResult &result : m_model->similarResults()) {
                if (seen.contains(result.id))
                    continue;
                Row row = track(m_model->song(result.id), QStringLiteral("Suggested on YouTube"));
                row.title = result.title;
                row.author = result.author;
                row.seconds = result.seconds;
                out.append(row);
            }
        }
    } else if (m_source == QLatin1String("sampler")) {
        for (const DjSoundEffect &effect : soundEffects()) {
            Row row;
            row.title = effect.name;
            row.author = QStringLiteral("Sound effect");
            row.seconds = effect.seconds;
            row.effect = effect.id;
            row.info = QStringLiteral("Double-click to play");
            out.append(row);
        }
        for (DjDrumPad pad : drumPads()) {
            Row row;
            row.title = drumPadLabel(pad);
            row.author = QStringLiteral("Drum pad");
            row.effect = drumPadKey(pad);
            row.info = QStringLiteral("Double-click to play");
            out.append(row);
        }
    } else {
        const DjFocus focus = DjFocus::fromKey(m_source);
        if (!focus.isEverything()) {
            for (const DjTrack &t : crate.songs(focus))
                out.append(track(t));
        }
    }
    const QString filter = m_source == QLatin1String("search") ? QString() : m_search->text().trimmed().toLower();
    if (!filter.isEmpty() && studio::videoId(filter).isEmpty()) {
        QList<Row> kept;
        for (const Row &row : out) {
            const QString haystack = (row.title + QLatin1Char(' ') + row.author + QLatin1Char(' ') + row.info).toLower();
            bool all = true;
            for (const QString &word : filter.split(QLatin1Char(' '), Qt::SkipEmptyParts))
                all = all && haystack.contains(word);
            if (all)
                kept.append(row);
        }
        out = kept;
    }
    return out;
}

void DjBrowserView::refreshList()
{
    for (int i = 0; i < m_loadButtons.size(); ++i)
        m_loadButtons.at(i)->setVisible(i < m_model->deckCount());
    const QString current = m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole).toString() : QString();
    m_rows = rows();
    m_list->clear();
    QTreeWidgetItem *restore = nullptr;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, QString::number(i + 1));
        item->setText(1, row.title);
        item->setText(2, row.author);
        item->setText(3, row.plays > 0 ? QString::number(row.plays) : QString());
        item->setText(4, lengthText(row.seconds));
        item->setText(5, row.info);
        item->setData(0, Qt::UserRole, row.id.isEmpty() ? row.effect : row.id);
        item->setData(0, Qt::UserRole + 1, i);
        item->setToolTip(1, row.title);
        item->setForeground(2, QColor(0x23, 0xa6, 0xff));
        item->setForeground(5, theme::dim());
        if (!row.id.isEmpty()) {
            const QImage art = m_model->artwork(row.id);
            if (!art.isNull() && art.width() > 1)
                item->setIcon(0, QIcon(QPixmap::fromImage(art.scaled(32, 20, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation))));
        }
        if (!current.isEmpty() && item->data(0, Qt::UserRole).toString() == current && !restore)
            restore = item;
    }
    if (restore)
        m_list->setCurrentItem(restore);
    QString status;
    if (m_source == QLatin1String("search")) {
        status = m_model->searching() ? QStringLiteral("Searching YouTube…")
                 : !m_model->searchProblem().isEmpty() ? m_model->searchProblem()
                                                       : QStringLiteral("%1 videos").arg(m_rows.size());
    } else if (m_source == QLatin1String("similar") && m_model->similarLoading()) {
        status = QStringLiteral("Finding songs like this…");
    } else if (m_source == QLatin1String("similar") && m_rows.isEmpty()) {
        status = m_model->similarProblem().isEmpty() ? QStringLiteral("Play a song to see songs like it") : m_model->similarProblem();
    } else {
        status = m_rows.size() == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(m_rows.size());
    }
    m_count->setText(status);
    refreshSuggestion();
}

void DjBrowserView::refreshIcons()
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        if (!item->icon(0).isNull())
            continue;
        const int index = item->data(0, Qt::UserRole + 1).toInt();
        if (index < 0 || index >= m_rows.size() || m_rows.at(index).id.isEmpty())
            continue;
        const QImage art = m_model->artwork(m_rows.at(index).id);
        if (!art.isNull() && art.width() > 1)
            item->setIcon(0, QIcon(QPixmap::fromImage(art.scaled(32, 20, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation))));
    }
    if (!m_infoId.isEmpty() && m_infoArt->pixmap().isNull()) {
        const QImage art = m_model->artwork(m_infoId);
        if (!art.isNull() && art.width() > 1)
            m_infoArt->setPixmap(QPixmap::fromImage(art.scaled(90, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }
}

DjBrowserView::Row DjBrowserView::rowFor(QTreeWidgetItem *item) const
{
    if (!item)
        return {};
    const int index = item->data(0, Qt::UserRole + 1).toInt();
    return index >= 0 && index < m_rows.size() ? m_rows.at(index) : Row();
}

void DjBrowserView::activate(const Row &row, int deck)
{
    if (!row.effect.isEmpty()) {
        m_model->playPad(row.effect);
        return;
    }
    if (!studio::isVideoId(row.id))
        return;
    const int target = deck >= 0 ? deck : bestDeck();
    if (target < 0) {
        m_model->enqueue(row.id, row.title, row.author);
        return;
    }
    if (!row.queueId.isEmpty()) {
        for (const DjQueueItem &item : m_model->shownQueue()) {
            if (item.id == row.queueId) {
                m_model->playFromQueue(item, target);
                return;
            }
        }
    }
    m_model->load(target, row.id, row.title, row.author);
}

void DjBrowserView::runSearch()
{
    const QString text = m_search->text().trimmed();
    if (text.isEmpty())
        return;
    const QString id = studio::videoId(text);
    if (!id.isEmpty()) {
        const int target = bestDeck();
        if (target >= 0)
            m_model->load(target, id);
        else
            m_model->enqueue(id, QString(), QString());
        m_search->clear();
        return;
    }
    m_source = QStringLiteral("search");
    refreshSources();
    m_model->search(text);
    refreshList();
}

void DjBrowserView::showInfo(const Row &row)
{
    m_infoId = row.id;
    m_infoTitle->setText(row.title);
    m_infoArtist->setText(row.author);
    m_infoArt->setPixmap(QPixmap());
    if (row.id.isEmpty()) {
        m_infoDetails->setText(row.info);
        return;
    }
    const QImage art = m_model->artwork(row.id);
    if (!art.isNull() && art.width() > 1)
        m_infoArt->setPixmap(QPixmap::fromImage(art.scaled(90, 90, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    const DjTrack track = m_model->song(row.id);
    QStringList lines;
    const int plays = m_model->plays(row.id);
    lines << (plays > 0 ? QStringLiteral("Played %1").arg(studio::playsLabel(plays)) : QStringLiteral("Not played yet"));
    const double last = m_model->lastPlayed(row.id);
    if (last > 0)
        lines << QStringLiteral("Last played: %1").arg(QDateTime::fromSecsSinceEpoch(qint64(last)).toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    if (row.seconds > 0)
        lines << QStringLiteral("Length: %1").arg(studio::clock(row.seconds));
    if (!track.tags.isEmpty())
        lines << QStringLiteral("Tags: #%1").arg(track.tags.join(QStringLiteral(" #")));
    QStringList in;
    for (const DjPlaylist &playlist : m_model->crate().playlists) {
        if (playlist.songs.contains(row.id))
            in << playlist.name;
    }
    if (!in.isEmpty())
        lines << QStringLiteral("Playlists: %1").arg(in.join(QStringLiteral(", ")));
    if (track.noShuffle)
        lines << QStringLiteral("Left out of shuffle");
    const QString genre = smart::categories(track).join(QStringLiteral(", "));
    if (!genre.isEmpty())
        lines << QStringLiteral("Smart lists: %1").arg(genre);
    if (!row.info.isEmpty() && row.queueId.size())
        lines << row.info;
    lines << QString() << QStringLiteral("youtube.com/watch?v=%1").arg(row.id);
    m_infoDetails->setText(lines.join(QLatin1Char('\n')));
}

void DjBrowserView::refreshSuggestion()
{
    int live = -1;
    double loudest = -1;
    for (int i = 0; i < m_model->deckCount(); ++i) {
        if (m_model->deck(i).playing && m_model->audibleGain(i) > loudest) {
            loudest = m_model->audibleGain(i);
            live = i;
        }
    }
    const QString seed = live >= 0 ? m_model->deck(live).videoId : QString();
    if (seed != m_suggestionSeed || m_suggestions.isEmpty()) {
        m_suggestionSeed = seed;
        m_suggestions.clear();
        m_suggestionIndex = 0;
        if (const auto pick = m_model->autoDjNext()) {
            Row row;
            row.id = pick->id;
            row.title = pick->title;
            row.author = pick->author;
            row.info = QStringLiteral("AUTO DJ");
            m_suggestions.append(row);
        }
        if (!seed.isEmpty()) {
            for (const DjSimilarSong &similar : m_model->similarSongs(seed).mid(0, 8)) {
                Row row;
                row.id = similar.track.id;
                row.title = similar.track.shownTitle();
                row.author = similar.track.author;
                row.info = similar.reasons.join(QStringLiteral(", "));
                m_suggestions.append(row);
            }
        }
    }
    if (m_suggestions.isEmpty()) {
        m_suggestion->setText(QStringLiteral("<span style='color:#ff3d9a'>Suggestions</span> appear here while a song plays."));
        return;
    }
    m_suggestionIndex = std::clamp(m_suggestionIndex, 0, int(m_suggestions.size()) - 1);
    const Row &row = m_suggestions.at(m_suggestionIndex);
    m_suggestion->setText(QStringLiteral("<span style='color:#ff3d9a'>Up next idea:</span> %1%2 <span style='color:#8c9199'>(%3) — click to queue</span>")
                              .arg(row.title.toHtmlEscaped(),
                                   row.author.isEmpty() ? QString() : QStringLiteral(" · ") + row.author.toHtmlEscaped(),
                                   row.info.toHtmlEscaped()));
}

void DjBrowserView::startDrag(QTreeWidgetItem *item)
{
    const Row row = rowFor(item);
    if (!studio::isVideoId(row.id))
        return;
    auto *mime = new QMimeData;
    mime->setData(QString::fromLatin1(songMime),
                  QJsonDocument(QJsonObject{{QStringLiteral("id"), row.id},
                                            {QStringLiteral("title"), row.title},
                                            {QStringLiteral("author"), row.author},
                                            {QStringLiteral("queue"), row.queueId}})
                      .toJson(QJsonDocument::Compact));
    mime->setText(studio::watchUrl(row.id).toString());
    auto *drag = new QDrag(m_list);
    drag->setMimeData(mime);
    const QImage art = m_model->artwork(row.id);
    if (!art.isNull() && art.width() > 1)
        drag->setPixmap(QPixmap::fromImage(art.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag->exec(Qt::CopyAction);
}

bool DjBrowserView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_suggestion && event->type() == QEvent::MouseButtonRelease) {
        if (!m_suggestions.isEmpty()) {
            const Row row = m_suggestions.at(m_suggestionIndex);
            m_model->enqueue(row.id, row.title, row.author);
            m_suggestions.removeAt(m_suggestionIndex);
            refreshSuggestion();
        }
        return true;
    }
    if (watched == m_list->viewport()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton)
                m_dragStart = mouse->position().toPoint();
        } else if (event->type() == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if ((mouse->buttons() & Qt::LeftButton) &&
                (mouse->position().toPoint() - m_dragStart).manhattanLength() > QApplication::startDragDistance()) {
                if (QTreeWidgetItem *item = m_list->itemAt(m_dragStart)) {
                    startDrag(item);
                    return true;
                }
            }
        }
    }
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            activate(rowFor(m_list->currentItem()));
            return true;
        }
        if (shortcut && !key->text().isEmpty() && key->text().at(0).isPrint() && shortcut(key))
            return true;
        if (key->key() == Qt::Key_Delete && m_source == QLatin1String("queue")) {
            const Row row = rowFor(m_list->currentItem());
            for (const DjQueueItem &item : m_model->shownQueue()) {
                if (item.id == row.queueId)
                    m_model->removeFromQueue(item);
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DjBrowserView::rowMenu(const QPoint &global)
{
    QTreeWidgetItem *item = m_list->currentItem();
    const Row row = rowFor(item);
    if (!row.effect.isEmpty()) {
        m_model->playPad(row.effect);
        return;
    }
    if (!studio::isVideoId(row.id))
        return;
    QMenu menu(this);
    for (int deck = 0; deck < m_model->deckCount(); ++deck)
        menu.addAction(QStringLiteral("Load on Deck %1").arg(deck + 1), this, [this, row, deck]() { activate(row, deck); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Add to the Queue"), this, [this, row]() { m_model->enqueue(row.id, row.title, row.author); });
    if (!row.queueId.isEmpty()) {
        menu.addAction(QStringLiteral("Remove from the Queue"), this, [this, row]() {
            for (const DjQueueItem &q : m_model->shownQueue()) {
                if (q.id == row.queueId)
                    m_model->removeFromQueue(q);
            }
        });
        if (!m_model->inJam()) {
            const int index = item->data(0, Qt::UserRole + 1).toInt();
            menu.addAction(QStringLiteral("Move Up"), this, [this, index]() { m_model->moveQueueItem(index, index - 1); });
            menu.addAction(QStringLiteral("Move Down"), this, [this, index]() { m_model->moveQueueItem(index, index + 1); });
            menu.addAction(QStringLiteral("Clear the Queue"), this, [this]() { m_model->clearQueue(); });
        }
    }
    QMenu *add = menu.addMenu(QStringLiteral("Add to Playlist"));
    for (const DjPlaylist &playlist : m_model->crate().playlists) {
        QAction *action = add->addAction(playlist.name, this, [this, row, id = playlist.id]() {
            m_model->crate().keep(row.id, row.title, row.author);
            m_model->toggleInPlaylist(row.id, id);
        });
        action->setCheckable(true);
        action->setChecked(playlist.songs.contains(row.id));
    }
    add->addSeparator();
    add->addAction(QStringLiteral("New Playlist…"), this, [this, row]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("New Playlist"), QStringLiteral("Name"), QLineEdit::Normal, QString(), &ok);
        if (ok && !name.trimmed().isEmpty())
            m_model->newPlaylist(name, {row.id});
    });
    menu.addAction(QStringLiteral("Tags and Playlists…"), this, [this, row]() {
        if (songEditRequested)
            songEditRequested(row.id);
    });
    const DjFocus focus = DjFocus::fromKey(m_source);
    if (focus.kind == DjFocus::Kind::Playlist)
        menu.addAction(QStringLiteral("Remove from This Playlist"), this, [this, row, focus]() { m_model->removeFromPlaylist(row.id, focus.value); });
    if (focus.kind == DjFocus::Kind::Tag)
        menu.addAction(QStringLiteral("Remove #%1").arg(focus.value), this, [this, row, focus]() { m_model->toggleTag(focus.value, row.id); });
    QAction *shuffle = menu.addAction(QStringLiteral("Don't Shuffle"), this, [this, row]() { m_model->toggleShuffle(row.id); });
    shuffle->setCheckable(true);
    shuffle->setChecked(!m_model->crate().shuffles(row.id));
    menu.addAction(QStringLiteral("Find Songs Like This"), this, [this, row]() {
        m_model->findSimilar(row.id);
        m_source = QStringLiteral("similar");
        refreshSources();
        refreshList();
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Open on YouTube"), this, [row]() { QDesktopServices::openUrl(studio::watchUrl(row.id)); });
    menu.addAction(QStringLiteral("Copy Link"), this, [row]() { QApplication::clipboard()->setText(studio::watchUrl(row.id).toString()); });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Delete from Library"), this, [this, row]() {
        if (QMessageBox::question(this, QStringLiteral("Delete from Library"),
                                  QStringLiteral("Delete \"%1\" from the library, every playlist, its tags and its play count?").arg(row.title)) ==
            QMessageBox::Yes)
            m_model->removeSong(row.id);
    });
    menu.exec(global);
}

void DjBrowserView::sourceMenu(QTreeWidgetItem *item, const QPoint &global)
{
    QMenu menu(this);
    const QString key = item ? item->data(0, Qt::UserRole).toString() : QString();
    const QString group = item ? item->data(0, Qt::UserRole + 1).toString() : QString();
    const DjFocus focus = DjFocus::fromKey(key);
    const bool list = !focus.isEverything();
    if (group == QLatin1String("playlists") || focus.kind == DjFocus::Kind::Playlist || !item) {
        menu.addAction(QStringLiteral("New Playlist…"), this, [this]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("New Playlist"), QStringLiteral("Name"), QLineEdit::Normal, QString(), &ok);
            if (ok && !name.trimmed().isEmpty())
                m_model->newPlaylist(name);
        });
    }
    if (list) {
        menu.addAction(QStringLiteral("Play with AUTOMIX"), this, [this, focus]() { m_model->focusAutomix(focus, true); });
        menu.addAction(QStringLiteral("Add All to the Queue"), this, [this, focus]() { m_model->queueAll(focus, false); });
        menu.addAction(QStringLiteral("Shuffle into the Queue"), this, [this, focus]() { m_model->queueAll(focus, true); });
        menu.addSeparator();
    }
    if (focus.kind == DjFocus::Kind::Playlist) {
        menu.addAction(QStringLiteral("Rename…"), this, [this, focus]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Rename Playlist"), QStringLiteral("Name"), QLineEdit::Normal,
                                                       m_model->crate().label(focus), &ok);
            if (ok && !name.trimmed().isEmpty())
                m_model->renamePlaylist(focus.value, name);
        });
        menu.addAction(QStringLiteral("Add a YouTube Link…"), this, [this, focus]() {
            bool ok = false;
            const QString link = QInputDialog::getText(this, QStringLiteral("Add a Song"), QStringLiteral("YouTube link"), QLineEdit::Normal, QString(), &ok);
            const QString id = studio::videoId(link);
            if (ok && !id.isEmpty() && !m_model->inPlaylist(id, focus.value))
                m_model->toggleInPlaylist(id, focus.value);
        });
        menu.addAction(QStringLiteral("Delete Playlist"), this, [this, focus]() {
            if (QMessageBox::question(this, QStringLiteral("Delete Playlist"),
                                      QStringLiteral("Delete %1? Its songs stay in the library.").arg(m_model->crate().label(focus))) == QMessageBox::Yes)
                m_model->deletePlaylist(focus.value);
        });
    }
    if (focus.kind == DjFocus::Kind::Tag) {
        menu.addAction(QStringLiteral("Rename Tag…"), this, [this, focus]() {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Rename Tag"), QStringLiteral("Tag"), QLineEdit::Normal, focus.value, &ok);
            if (ok && !name.trimmed().isEmpty())
                m_model->renameTag(focus.value, name);
        });
        menu.addAction(QStringLiteral("Delete Tag"), this, [this, focus]() { m_model->deleteTag(focus.value); });
    }
    if (focus.kind == DjFocus::Kind::Smart)
        menu.addAction(QStringLiteral("Save as Playlist"), this, [this, focus]() { m_model->saveSmart(focus.value); });
    if (list) {
        menu.addAction(QStringLiteral("Export…"), this, [this, focus]() {
            if (exportRequested)
                exportRequested(focus);
        });
    }
    if (key == QLatin1String("history") || key == QLatin1String("most")) {
        menu.addAction(QStringLiteral("Clear Recently Played and Play Counts"), this, [this]() {
            if (QMessageBox::question(this, QStringLiteral("Clear"), QStringLiteral("Clear Recently Played and every play count? Playlists and tags stay.")) ==
                QMessageBox::Yes)
                m_model->clearHistory();
        });
    }
    if (key == QLatin1String("queue")) {
        menu.addAction(QStringLiteral("Clear the Queue"), this, [this]() { m_model->clearQueue(); });
    }
    menu.addSeparator();
    menu.addAction(QStringLiteral("Import from a File…"), this, [this]() {
        if (importRequested)
            importRequested();
    });
    menu.addAction(QStringLiteral("Paste Songs from the Clipboard"), this, [this]() {
        if (pasteRequested)
            pasteRequested();
    });
    menu.addAction(QStringLiteral("Export Everything…"), this, [this]() {
        if (exportRequested)
            exportRequested(DjFocus::everything());
    });
    if (!menu.isEmpty())
        menu.exec(global);
}

}
