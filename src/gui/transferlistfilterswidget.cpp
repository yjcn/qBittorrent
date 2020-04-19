/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#include "transferlistfilterswidget.h"

#include <QCheckBox>
#include <QIcon>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QScrollArea>
#include <QStyleOptionButton>
#include <QUrl>
#include <QVBoxLayout>

#include "base/bittorrent/infohash.h"
#include "base/bittorrent/session.h"
#include "base/bittorrent/torrenthandle.h"
#include "base/bittorrent/trackerentry.h"
#include "base/global.h"
#include "base/logger.h"
#include "base/net/downloadmanager.h"
#include "base/preferences.h"
#include "base/torrentfilter.h"
#include "base/utils/fs.h"
#include "base/utils/string.h"
#include "categoryfilterwidget.h"
#include "tagfilterwidget.h"
#include "transferlistwidget.h"
#include "uithememanager.h"
#include "utils.h"

namespace
{
    enum TRACKER_FILTER_ROW
    {
        ALL_ROW,
        TRACKERLESS_ROW,
        ERROR_ROW,
        WARNING_ROW
    };

    QString getScheme(const QString &tracker)
    {
        const QUrl url {tracker};
        QString scheme = url.scheme();
        if (scheme.isEmpty())
            scheme = "http";
        return scheme;
    }

    class ArrowCheckBox final : public QCheckBox
    {
    public:
        using QCheckBox::QCheckBox;

    private:
        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);

            QStyleOptionViewItem indicatorOption;
            indicatorOption.initFrom(this);
            indicatorOption.rect = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &indicatorOption, this);
            indicatorOption.state |= (QStyle::State_Children
                                      | (isChecked() ? QStyle::State_Open : QStyle::State_None));
            style()->drawPrimitive(QStyle::PE_IndicatorBranch, &indicatorOption, &painter, this);

            QStyleOptionButton labelOption;
            initStyleOption(&labelOption);
            labelOption.rect = style()->subElementRect(QStyle::SE_CheckBoxContents, &labelOption, this);
            style()->drawControl(QStyle::CE_CheckBoxLabel, &labelOption, &painter, this);
        }
    };
}

BaseFilterWidget::BaseFilterWidget(QWidget *parent, TransferListWidget *transferList)
    : QListWidget(parent)
    , transferList(transferList)
{
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setUniformItemSizes(true);
    setSpacing(0);

    setIconSize(Utils::Gui::smallIconSize());

#if defined(Q_OS_MACOS)
    setAttribute(Qt::WA_MacShowFocusRect, false);
#endif

    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &BaseFilterWidget::customContextMenuRequested, this, &BaseFilterWidget::showMenu);
    connect(this, &BaseFilterWidget::currentRowChanged, this, &BaseFilterWidget::applyFilter);

    connect(BitTorrent::Session::instance(), &BitTorrent::Session::torrentAdded
            , this, &BaseFilterWidget::handleNewTorrent);
    connect(BitTorrent::Session::instance(), &BitTorrent::Session::torrentAboutToBeRemoved
            , this, &BaseFilterWidget::torrentAboutToBeDeleted);
}

QSize BaseFilterWidget::sizeHint() const
{
    return {
        // Width should be exactly the width of the content
        sizeHintForColumn(0),
        // Height should be exactly the height of the content
        static_cast<int>((sizeHintForRow(0) + 2 * spacing()) * (count() + 0.5)),
    };
}

QSize BaseFilterWidget::minimumSizeHint() const
{
    QSize size = sizeHint();
    size.setWidth(6);
    return size;
}

void BaseFilterWidget::toggleFilter(bool checked)
{
    setVisible(checked);
    if (checked)
        applyFilter(currentRow());
    else
        applyFilter(ALL_ROW);
}

StatusFilterWidget::StatusFilterWidget(QWidget *parent, TransferListWidget *transferList)
    : BaseFilterWidget(parent, transferList)
{
    connect(BitTorrent::Session::instance(), &BitTorrent::Session::torrentAdded
            , this, &StatusFilterWidget::updateTorrentNumbers);
    connect(BitTorrent::Session::instance(), &BitTorrent::Session::torrentsUpdated
            , this, &StatusFilterWidget::updateTorrentNumbers);
    connect(BitTorrent::Session::instance(), &BitTorrent::Session::torrentAboutToBeRemoved
            , this, &StatusFilterWidget::updateTorrentNumbers);

    // Add status filters
    auto *all = new QListWidgetItem(this);
    all->setData(Qt::DisplayRole, tr("All (0)", "this is for the status filter"));
    all->setData(Qt::DecorationRole, QIcon(":/icons/skin/filterall.svg"));
    auto *downloading = new QListWidgetItem(this);
    downloading->setData(Qt::DisplayRole, tr("Downloading (0)"));
    downloading->setData(Qt::DecorationRole, QIcon(":/icons/skin/downloading.svg"));
    auto *seeding = new QListWidgetItem(this);
    seeding->setData(Qt::DisplayRole, tr("Seeding (0)"));
    seeding->setData(Qt::DecorationRole, QIcon(":/icons/skin/uploading.svg"));
    auto *completed = new QListWidgetItem(this);
    completed->setData(Qt::DisplayRole, tr("Completed (0)"));
    completed->setData(Qt::DecorationRole, QIcon(":/icons/skin/completed.svg"));
    auto *resumed = new QListWidgetItem(this);
    resumed->setData(Qt::DisplayRole, tr("Resumed (0)"));
    resumed->setData(Qt::DecorationRole, QIcon(":/icons/skin/resumed.svg"));
    auto *paused = new QListWidgetItem(this);
    paused->setData(Qt::DisplayRole, tr("Paused (0)"));
    paused->setData(Qt::DecorationRole, QIcon(":/icons/skin/paused.svg"));
    auto *active = new QListWidgetItem(this);
    active->setData(Qt::DisplayRole, tr("Active (0)"));
    active->setData(Qt::DecorationRole, QIcon(":/icons/skin/filteractive.svg"));
    auto *inactive = new QListWidgetItem(this);
    inactive->setData(Qt::DisplayRole, tr("Inactive (0)"));
    inactive->setData(Qt::DecorationRole, QIcon(":/icons/skin/filterinactive.svg"));
    auto *stalled = new QListWidgetItem(this);
    stalled->setData(Qt::DisplayRole, tr("Stalled (0)"));
    stalled->setData(Qt::DecorationRole, QIcon(":/icons/skin/filterstalled.svg"));
    auto *stalledUploading = new QListWidgetItem(this);
    stalledUploading->setData(Qt::DisplayRole, tr("Stalled Uploading (0)"));
    stalledUploading->setData(Qt::DecorationRole, QIcon(":/icons/skin/stalledUP.svg"));
    auto *stalledDownloading = new QListWidgetItem(this);
    stalledDownloading->setData(Qt::DisplayRole, tr("Stalled Downloading (0)"));
    stalledDownloading->setData(Qt::DecorationRole, QIcon(":/icons/skin/stalledDL.svg"));
    auto *errored = new QListWidgetItem(this);
    errored->setData(Qt::DisplayRole, tr("Errored (0)"));
    errored->setData(Qt::DecorationRole, QIcon(":/icons/skin/error.svg"));

    const Preferences *const pref = Preferences::instance();
    setCurrentRow(pref->getTransSelFilter(), QItemSelectionModel::SelectCurrent);
    toggleFilter(pref->getStatusFilterState());
}

StatusFilterWidget::~StatusFilterWidget()
{
    Preferences::instance()->setTransSelFilter(currentRow());
}

void StatusFilterWidget::updateTorrentNumbers()
{
    int nbDownloading = 0;
    int nbSeeding = 0;
    int nbCompleted = 0;
    int nbResumed = 0;
    int nbPaused = 0;
    int nbActive = 0;
    int nbInactive = 0;
    int nbStalled = 0;
    int nbStalledUploading = 0;
    int nbStalledDownloading = 0;
    int nbErrored = 0;

    const QVector<BitTorrent::TorrentHandle *> torrents = BitTorrent::Session::instance()->torrents();
    for (const BitTorrent::TorrentHandle *torrent : torrents) {
        if (torrent->isDownloading())
            ++nbDownloading;
        if (torrent->isUploading())
            ++nbSeeding;
        if (torrent->isCompleted())
            ++nbCompleted;
        if (torrent->isResumed())
            ++nbResumed;
        if (torrent->isPaused())
            ++nbPaused;
        if (torrent->isActive())
            ++nbActive;
        if (torrent->isInactive())
            ++nbInactive;
        if (torrent->state() ==  BitTorrent::TorrentState::StalledUploading)
            ++nbStalledUploading;
        if (torrent->state() ==  BitTorrent::TorrentState::StalledDownloading)
            ++nbStalledDownloading;
        if (torrent->isErrored())
            ++nbErrored;
    }

    nbStalled = nbStalledUploading + nbStalledDownloading;

    item(TorrentFilter::All)->setData(Qt::DisplayRole, tr("All (%1)").arg(torrents.count()));
    item(TorrentFilter::Downloading)->setData(Qt::DisplayRole, tr("Downloading (%1)").arg(nbDownloading));
    item(TorrentFilter::Seeding)->setData(Qt::DisplayRole, tr("Seeding (%1)").arg(nbSeeding));
    item(TorrentFilter::Completed)->setData(Qt::DisplayRole, tr("Completed (%1)").arg(nbCompleted));
    item(TorrentFilter::Resumed)->setData(Qt::DisplayRole, tr("Resumed (%1)").arg(nbResumed));
    item(TorrentFilter::Paused)->setData(Qt::DisplayRole, tr("Paused (%1)").arg(nbPaused));
    item(TorrentFilter::Active)->setData(Qt::DisplayRole, tr("Active (%1)").arg(nbActive));
    item(TorrentFilter::Inactive)->setData(Qt::DisplayRole, tr("Inactive (%1)").arg(nbInactive));
    item(TorrentFilter::Stalled)->setData(Qt::DisplayRole, tr("Stalled (%1)").arg(nbStalled));
    item(TorrentFilter::StalledUploading)->setData(Qt::DisplayRole, tr("Stalled Uploading (%1)").arg(nbStalledUploading));
    item(TorrentFilter::StalledDownloading)->setData(Qt::DisplayRole, tr("Stalled Downloading (%1)").arg(nbStalledDownloading));
    item(TorrentFilter::Errored)->setData(Qt::DisplayRole, tr("Errored (%1)").arg(nbErrored));
}

void StatusFilterWidget::showMenu(const QPoint &) {}

void StatusFilterWidget::applyFilter(int row)
{
    transferList->applyStatusFilter(row);
}

void StatusFilterWidget::handleNewTorrent(BitTorrent::TorrentHandle *const) {}

void StatusFilterWidget::torrentAboutToBeDeleted(BitTorrent::TorrentHandle *const) {}

TrackerFiltersList::TrackerFiltersList(QWidget *parent, TransferListWidget *transferList, const bool downloadFavicon)
    : BaseFilterWidget(parent, transferList)
    , m_totalTorrents(0)
    , m_downloadTrackerFavicon(downloadFavicon)
{
    auto *allTrackers = new QListWidgetItem(this);
    allTrackers->setData(Qt::DisplayRole, tr("All (0)", "this is for the tracker filter"));
    allTrackers->setData(Qt::DecorationRole, UIThemeManager::instance()->getIcon("network-server"));
    auto *noTracker = new QListWidgetItem(this);
    noTracker->setData(Qt::DisplayRole, tr("Trackerless (0)"));
    noTracker->setData(Qt::DecorationRole, UIThemeManager::instance()->getIcon("network-server"));
    auto *errorTracker = new QListWidgetItem(this);
    errorTracker->setData(Qt::DisplayRole, tr("Error (0)"));
    errorTracker->setData(Qt::DecorationRole, style()->standardIcon(QStyle::SP_MessageBoxCritical));
    auto *warningTracker = new QListWidgetItem(this);
    warningTracker->setData(Qt::DisplayRole, tr("Warning (0)"));
    warningTracker->setData(Qt::DecorationRole, style()->standardIcon(QStyle::SP_MessageBoxWarning));
    m_trackers.insert("", QStringList());

    setCurrentRow(0, QItemSelectionModel::SelectCurrent);
    toggleFilter(Preferences::instance()->getTrackerFilterState());
}

TrackerFiltersList::~TrackerFiltersList()
{
    for (const QString &iconPath : asConst(m_iconPaths))
        Utils::Fs::forceRemove(iconPath);
}

void TrackerFiltersList::addItem(const QString &tracker, const QString &hash)
{
    QStringList tmp;
    QListWidgetItem *trackerItem = nullptr;
    QString host = getHost(tracker);
    bool exists = m_trackers.contains(host);

    if (exists) {
        tmp = m_trackers.value(host);
        if (tmp.contains(hash))
            return;

        if (host != "") {
            trackerItem = item(rowFromTracker(host));
        }
        else {
            trackerItem = item(TRACKERLESS_ROW);
        }
    }
    else {
        trackerItem = new QListWidgetItem();
        trackerItem->setData(Qt::DecorationRole, UIThemeManager::instance()->getIcon("network-server"));

        const QString scheme = getScheme(tracker);
        downloadFavicon(QString::fromLatin1("%1://%2/favicon.ico").arg((scheme.startsWith("http") ? scheme : "http"), host));
    }
    if (!trackerItem) return;

    tmp.append(hash);
    m_trackers.insert(host, tmp);
    if (host == "") {
        trackerItem->setText(tr("Trackerless (%1)").arg(tmp.size()));
        if (currentRow() == TRACKERLESS_ROW)
            applyFilter(TRACKERLESS_ROW);
        return;
    }

    trackerItem->setText(QString::fromLatin1("%1 (%2)").arg(host, QString::number(tmp.size())));
    if (exists) {
        if (currentRow() == rowFromTracker(host))
            applyFilter(currentRow());
        return;
    }

    Q_ASSERT(count() >= 4);
    int insPos = count();
    for (int i = 4; i < count(); ++i) {
        if (Utils::String::naturalLessThan<Qt::CaseSensitive>(host, item(i)->text())) {
            insPos = i;
            break;
        }
    }
    QListWidget::insertItem(insPos, trackerItem);
    updateGeometry();
}

void TrackerFiltersList::removeItem(const QString &tracker, const QString &hash)
{
    QString host = getHost(tracker);
    QListWidgetItem *trackerItem = nullptr;
    QStringList tmp = m_trackers.value(host);
    int row = 0;

    if (tmp.empty())
        return;
    tmp.removeAll(hash);

    if (!host.isEmpty()) {
        // Remove from 'Error' and 'Warning' view
        trackerSuccess(hash, tracker);
        row = rowFromTracker(host);
        trackerItem = item(row);
        if (tmp.empty()) {
            if (currentRow() == row)
                setCurrentRow(0, QItemSelectionModel::SelectCurrent);
            delete trackerItem;
            m_trackers.remove(host);
            updateGeometry();
            return;
        }
        if (trackerItem)
            trackerItem->setText(QString::fromLatin1("%1 (%2)").arg(host, QString::number(tmp.size())));
    }
    else {
        row = 1;
        trackerItem = item(TRACKERLESS_ROW);
        trackerItem->setText(tr("Trackerless (%1)").arg(tmp.size()));
    }

    m_trackers.insert(host, tmp);
    if (currentRow() == row)
        applyFilter(row);
}

void TrackerFiltersList::changeTrackerless(bool trackerless, const QString &hash)
{
    if (trackerless)
        addItem("", hash);
    else
        removeItem("", hash);
}

void TrackerFiltersList::setDownloadTrackerFavicon(bool value)
{
    if (value == m_downloadTrackerFavicon) return;
    m_downloadTrackerFavicon = value;

    if (m_downloadTrackerFavicon) {
        for (auto i = m_trackers.cbegin(); i != m_trackers.cend(); ++i) {
            const QString &tracker = i.key();
            if (!tracker.isEmpty()) {
                const QString scheme = getScheme(tracker);
                downloadFavicon(QString("%1://%2/favicon.ico")
                                .arg((scheme.startsWith("http") ? scheme : "http"), getHost(tracker)));
             }
        }
    }
}

void TrackerFiltersList::trackerSuccess(const QString &hash, const QString &tracker)
{
    QStringList errored = m_errors.value(hash);
    QStringList warned = m_warnings.value(hash);

    if (errored.contains(tracker)) {
        errored.removeAll(tracker);
        if (errored.empty()) {
            m_errors.remove(hash);
            item(ERROR_ROW)->setText(tr("Error (%1)").arg(m_errors.size()));
            if (currentRow() == ERROR_ROW)
                applyFilter(ERROR_ROW);
        }
        else {
            m_errors.insert(hash, errored);
        }
    }

    if (warned.contains(tracker)) {
        warned.removeAll(tracker);
        if (warned.empty()) {
            m_warnings.remove(hash);
            item(WARNING_ROW)->setText(tr("Warning (%1)").arg(m_warnings.size()));
            if (currentRow() == WARNING_ROW)
                applyFilter(WARNING_ROW);
        }
        else {
            m_warnings.insert(hash, warned);
        }
    }
}

void TrackerFiltersList::trackerError(const QString &hash, const QString &tracker)
{
    QStringList trackers = m_errors.value(hash);

    if (trackers.contains(tracker))
        return;

    trackers.append(tracker);
    m_errors.insert(hash, trackers);
    item(ERROR_ROW)->setText(tr("Error (%1)").arg(m_errors.size()));

    if (currentRow() == ERROR_ROW)
        applyFilter(ERROR_ROW);
}

void TrackerFiltersList::trackerWarning(const QString &hash, const QString &tracker)
{
    QStringList trackers = m_warnings.value(hash);

    if (trackers.contains(tracker))
        return;

    trackers.append(tracker);
    m_warnings.insert(hash, trackers);
    item(WARNING_ROW)->setText(tr("Warning (%1)").arg(m_warnings.size()));

    if (currentRow() == WARNING_ROW)
        applyFilter(WARNING_ROW);
}

void TrackerFiltersList::downloadFavicon(const QString &url)
{
    if (!m_downloadTrackerFavicon) return;
    Net::DownloadManager::instance()->download(
                Net::DownloadRequest(url).saveToFile(true)
                , this, &TrackerFiltersList::handleFavicoDownloadFinished);
}

void TrackerFiltersList::handleFavicoDownloadFinished(const Net::DownloadResult &result)
{
    if (result.status != Net::DownloadStatus::Success) {
        if (result.url.endsWith(".ico", Qt::CaseInsensitive))
            downloadFavicon(result.url.left(result.url.size() - 4) + ".png");
        return;
    }

    const QString host = getHost(result.url);

    if (!m_trackers.contains(host)) {
        Utils::Fs::forceRemove(result.filePath);
        return;
    }

    QListWidgetItem *trackerItem = item(rowFromTracker(host));
    if (!trackerItem) return;

    QIcon icon(result.filePath);
    //Detect a non-decodable icon
    QList<QSize> sizes = icon.availableSizes();
    bool invalid = (sizes.isEmpty() || icon.pixmap(sizes.first()).isNull());
    if (invalid) {
        if (result.url.endsWith(".ico", Qt::CaseInsensitive))
            downloadFavicon(result.url.left(result.url.size() - 4) + ".png");
        Utils::Fs::forceRemove(result.filePath);
    }
    else {
        trackerItem->setData(Qt::DecorationRole, QIcon(result.filePath));
        m_iconPaths.append(result.filePath);
    }
}

void TrackerFiltersList::showMenu(const QPoint &)
{
    QMenu *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const QAction *startAct = menu->addAction(UIThemeManager::instance()->getIcon("media-playback-start"), tr("Resume torrents"));
    connect(startAct, &QAction::triggered, transferList, &TransferListWidget::startVisibleTorrents);

    const QAction *pauseAct = menu->addAction(UIThemeManager::instance()->getIcon("media-playback-pause"), tr("Pause torrents"));
    connect(pauseAct, &QAction::triggered, transferList, &TransferListWidget::pauseVisibleTorrents);

    const QAction *deleteTorrentsAct = menu->addAction(UIThemeManager::instance()->getIcon("edit-delete"), tr("Delete torrents"));
    connect(deleteTorrentsAct, &QAction::triggered, transferList, &TransferListWidget::deleteVisibleTorrents);

    menu->popup(QCursor::pos());
}

void TrackerFiltersList::applyFilter(const int row)
{
    if (row == ALL_ROW)
        transferList->applyTrackerFilterAll();
    else if (isVisible())
        transferList->applyTrackerFilter(getHashes(row));
}

void TrackerFiltersList::handleNewTorrent(BitTorrent::TorrentHandle *const torrent)
{
    QString hash = torrent->hash();
    const QVector<BitTorrent::TrackerEntry> trackers = torrent->trackers();
    for (const BitTorrent::TrackerEntry &tracker : trackers)
        addItem(tracker.url(), hash);

    //Check for trackerless torrent
    if (trackers.isEmpty())
        addItem("", hash);

    item(ALL_ROW)->setText(tr("All (%1)", "this is for the tracker filter").arg(++m_totalTorrents));
}

void TrackerFiltersList::torrentAboutToBeDeleted(BitTorrent::TorrentHandle *const torrent)
{
    QString hash = torrent->hash();
    const QVector<BitTorrent::TrackerEntry> trackers = torrent->trackers();
    for (const BitTorrent::TrackerEntry &tracker : trackers)
        removeItem(tracker.url(), hash);

    //Check for trackerless torrent
    if (trackers.isEmpty())
        removeItem("", hash);

    item(ALL_ROW)->setText(tr("All (%1)", "this is for the tracker filter").arg(--m_totalTorrents));
}

QString TrackerFiltersList::trackerFromRow(int row) const
{
    Q_ASSERT(row > 1);
    const QString tracker = item(row)->text();
    QStringList parts = tracker.split(' ');
    Q_ASSERT(parts.size() >= 2);
    parts.removeLast(); // Remove trailing number
    return parts.join(' ');
}

int TrackerFiltersList::rowFromTracker(const QString &tracker) const
{
    Q_ASSERT(!tracker.isEmpty());
    for (int i = 4; i<count(); ++i)
        if (tracker == trackerFromRow(i)) return i;
    return -1;
}

QString TrackerFiltersList::getHost(const QString &tracker) const
{
    QUrl url(tracker);
    QString longHost = url.host();
    QString tld = url.topLevelDomain();
    // We get empty tld when it is invalid or an IPv4/IPv6 address,
    // so just return the full host
    if (tld.isEmpty())
        return longHost;
    // We want the domain + tld. Subdomains should be disregarded
    int index = longHost.lastIndexOf('.', -(tld.size() + 1));
    if (index == -1)
        return longHost;
    return longHost.mid(index + 1);
}

QStringList TrackerFiltersList::getHashes(const int row) const
{
    switch (row) {
    case TRACKERLESS_ROW:
        return m_trackers.value("");
    case ERROR_ROW:
        return m_errors.keys();
    case WARNING_ROW:
        return m_warnings.keys();
    default:
        return m_trackers.value(trackerFromRow(row));
    }
}

TransferListFiltersWidget::TransferListFiltersWidget(QWidget *parent, TransferListWidget *transferList, const bool downloadFavicon)
    : QFrame(parent)
    , m_transferList(transferList)
{
    Preferences *const pref = Preferences::instance();

    // Construct lists
    auto *vLayout = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    QFrame *frame = new QFrame(scroll);
    auto *frameLayout = new QVBoxLayout(frame);
    QFont font;
    font.setBold(true);
    font.setCapitalization(QFont::AllUppercase);

    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    setStyleSheet("QFrame {background: transparent;}");
    scroll->setStyleSheet("QFrame {border: none;}");
    vLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setContentsMargins(0, 2, 0, 0);
    frameLayout->setSpacing(2);
    frameLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    frame->setLayout(frameLayout);
    scroll->setWidget(frame);
    vLayout->addWidget(scroll);
    setLayout(vLayout);

    QCheckBox *statusLabel = new ArrowCheckBox(tr("Status"), this);
    statusLabel->setChecked(pref->getStatusFilterState());
    statusLabel->setFont(font);
    frameLayout->addWidget(statusLabel);

    auto *statusFilters = new StatusFilterWidget(this, transferList);
    frameLayout->addWidget(statusFilters);

    QCheckBox *categoryLabel = new ArrowCheckBox(tr("Categories"), this);
    categoryLabel->setChecked(pref->getCategoryFilterState());
    categoryLabel->setFont(font);
    connect(categoryLabel, &QCheckBox::toggled, this
            , &TransferListFiltersWidget::onCategoryFilterStateChanged);
    frameLayout->addWidget(categoryLabel);

    m_categoryFilterWidget = new CategoryFilterWidget(this);
    connect(m_categoryFilterWidget, &CategoryFilterWidget::actionDeleteTorrentsTriggered
            , transferList, &TransferListWidget::deleteVisibleTorrents);
    connect(m_categoryFilterWidget, &CategoryFilterWidget::actionPauseTorrentsTriggered
            , transferList, &TransferListWidget::pauseVisibleTorrents);
    connect(m_categoryFilterWidget, &CategoryFilterWidget::actionResumeTorrentsTriggered
            , transferList, &TransferListWidget::startVisibleTorrents);
    connect(m_categoryFilterWidget, &CategoryFilterWidget::categoryChanged
            , transferList, &TransferListWidget::applyCategoryFilter);
    toggleCategoryFilter(pref->getCategoryFilterState());
    frameLayout->addWidget(m_categoryFilterWidget);

    QCheckBox *tagsLabel = new ArrowCheckBox(tr("Tags"), this);
    tagsLabel->setChecked(pref->getTagFilterState());
    tagsLabel->setFont(font);
    connect(tagsLabel, &QCheckBox::toggled, this, &TransferListFiltersWidget::onTagFilterStateChanged);
    frameLayout->addWidget(tagsLabel);

    m_tagFilterWidget = new TagFilterWidget(this);
    connect(m_tagFilterWidget, &TagFilterWidget::actionDeleteTorrentsTriggered
            , transferList, &TransferListWidget::deleteVisibleTorrents);
    connect(m_tagFilterWidget, &TagFilterWidget::actionPauseTorrentsTriggered
            , transferList, &TransferListWidget::pauseVisibleTorrents);
    connect(m_tagFilterWidget, &TagFilterWidget::actionResumeTorrentsTriggered
            , transferList, &TransferListWidget::startVisibleTorrents);
    connect(m_tagFilterWidget, &TagFilterWidget::tagChanged
            , transferList, &TransferListWidget::applyTagFilter);
    toggleTagFilter(pref->getTagFilterState());
    frameLayout->addWidget(m_tagFilterWidget);

    QCheckBox *trackerLabel = new ArrowCheckBox(tr("Trackers"), this);
    trackerLabel->setChecked(pref->getTrackerFilterState());
    trackerLabel->setFont(font);
    frameLayout->addWidget(trackerLabel);

    m_trackerFilters = new TrackerFiltersList(this, transferList, downloadFavicon);
    frameLayout->addWidget(m_trackerFilters);

    connect(statusLabel, &QCheckBox::toggled, statusFilters, &StatusFilterWidget::toggleFilter);
    connect(statusLabel, &QCheckBox::toggled, pref, &Preferences::setStatusFilterState);
    connect(trackerLabel, &QCheckBox::toggled, m_trackerFilters, &TrackerFiltersList::toggleFilter);
    connect(trackerLabel, &QCheckBox::toggled, pref, &Preferences::setTrackerFilterState);

    connect(this, qOverload<const QString &, const QString &>(&TransferListFiltersWidget::trackerSuccess)
            , m_trackerFilters, &TrackerFiltersList::trackerSuccess);
    connect(this, qOverload<const QString &, const QString &>(&TransferListFiltersWidget::trackerError)
            , m_trackerFilters, &TrackerFiltersList::trackerError);
    connect(this, qOverload<const QString &, const QString &>(&TransferListFiltersWidget::trackerWarning)
            , m_trackerFilters, &TrackerFiltersList::trackerWarning);
}

void TransferListFiltersWidget::setDownloadTrackerFavicon(bool value)
{
    m_trackerFilters->setDownloadTrackerFavicon(value);
}

void TransferListFiltersWidget::addTrackers(BitTorrent::TorrentHandle *const torrent, const QVector<BitTorrent::TrackerEntry> &trackers)
{
    for (const BitTorrent::TrackerEntry &tracker : trackers)
        m_trackerFilters->addItem(tracker.url(), torrent->hash());
}

void TransferListFiltersWidget::removeTrackers(BitTorrent::TorrentHandle *const torrent, const QVector<BitTorrent::TrackerEntry> &trackers)
{
    for (const BitTorrent::TrackerEntry &tracker : trackers)
        m_trackerFilters->removeItem(tracker.url(), torrent->hash());
}

void TransferListFiltersWidget::changeTrackerless(BitTorrent::TorrentHandle *const torrent, bool trackerless)
{
    m_trackerFilters->changeTrackerless(trackerless, torrent->hash());
}

void TransferListFiltersWidget::trackerSuccess(BitTorrent::TorrentHandle *const torrent, const QString &tracker)
{
    emit trackerSuccess(torrent->hash(), tracker);
}

void TransferListFiltersWidget::trackerWarning(BitTorrent::TorrentHandle *const torrent, const QString &tracker)
{
    emit trackerWarning(torrent->hash(), tracker);
}

void TransferListFiltersWidget::trackerError(BitTorrent::TorrentHandle *const torrent, const QString &tracker)
{
    emit trackerError(torrent->hash(), tracker);
}

void TransferListFiltersWidget::onCategoryFilterStateChanged(bool enabled)
{
    toggleCategoryFilter(enabled);
    Preferences::instance()->setCategoryFilterState(enabled);
}

void TransferListFiltersWidget::toggleCategoryFilter(bool enabled)
{
    m_categoryFilterWidget->setVisible(enabled);
    m_transferList->applyCategoryFilter(enabled ? m_categoryFilterWidget->currentCategory() : QString());
}

void TransferListFiltersWidget::onTagFilterStateChanged(bool enabled)
{
    toggleTagFilter(enabled);
    Preferences::instance()->setTagFilterState(enabled);
}

void TransferListFiltersWidget::toggleTagFilter(bool enabled)
{
    m_tagFilterWidget->setVisible(enabled);
    m_transferList->applyTagFilter(enabled ? m_tagFilterWidget->currentTag() : QString());
}
