/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2015  Vladimir Golovnev <glassez@yandex.ru>
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

#include "torrenthandleimpl.h"

#include <algorithm>
#include <memory>
#include <type_traits>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

#include <libtorrent/address.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/time.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <QBitArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QUrl>

#include "base/global.h"
#include "base/logger.h"
#include "base/preferences.h"
#include "base/profile.h"
#include "base/utils/fs.h"
#include "base/utils/string.h"
#include "common.h"
#include "downloadpriority.h"
#include "ltunderlyingtype.h"
#include "peeraddress.h"
#include "peerinfo.h"
#include "session.h"
#include "trackerentry.h"

const QString QB_EXT {QStringLiteral(".!qB")};

using namespace BitTorrent;

namespace libtorrent
{
    namespace aux
    {
        template <typename T, typename Tag>
        uint qHash(const strong_typedef<T, Tag> &key, const uint seed)
        {
            return ::qHash((std::hash<strong_typedef<T, Tag>> {})(key), seed);
        }
    }
}

namespace
{
    std::vector<lt::download_priority_t> toLTDownloadPriorities(const QVector<DownloadPriority> &priorities)
    {
        std::vector<lt::download_priority_t> out;
        out.reserve(priorities.size());

        std::transform(priorities.cbegin(), priorities.cend()
                       , std::back_inserter(out), [](BitTorrent::DownloadPriority priority)
        {
            return static_cast<lt::download_priority_t>(
                        static_cast<LTUnderlyingType<lt::download_priority_t>>(priority));
        });
        return out;
    }

    using ListType = lt::entry::list_type;

    ListType setToEntryList(const QSet<QString> &input)
    {
        ListType entryList;
        for (const QString &setValue : input)
            entryList.emplace_back(setValue.toStdString());
        return entryList;
    }
}

// TorrentHandleImpl

TorrentHandleImpl::TorrentHandleImpl(Session *session, const lt::torrent_handle &nativeHandle,
                                     const LoadTorrentParams &params)
    : QObject(session)
    , m_session(session)
    , m_nativeHandle(nativeHandle)
    , m_name(params.name)
    , m_savePath(Utils::Fs::toNativePath(params.savePath))
    , m_category(params.category)
    , m_tags(params.tags)
    , m_ratioLimit(params.ratioLimit)
    , m_seedingTimeLimit(params.seedingTimeLimit)
    , m_hasSeedStatus(params.hasSeedStatus)
    , m_hasRootFolder(params.hasRootFolder)
    , m_useAutoTMM(params.savePath.isEmpty())
    , m_ltAddTorrentParams(params.ltAddTorrentParams)
{
    if (m_useAutoTMM)
        m_savePath = Utils::Fs::toNativePath(m_session->categorySavePath(m_category));

    updateStatus();
    m_hash = InfoHash(m_nativeStatus.info_hash);

    // NB: the following two if statements are present because we don't want
    // to set either sequential download or first/last piece priority to false
    // if their respective flags in data are false when a torrent is being
    // resumed. This is because, in that circumstance, this constructor is
    // called with those flags set to false, even if the torrent was set to
    // download sequentially or have first/last piece priority enabled when
    // its resume data was saved. These two settings are restored later. But
    // if we set them to false now, both will erroneously not be restored.
    if (!params.restored || params.sequential)
        setSequentialDownload(params.sequential);
    if (!params.restored || params.firstLastPiecePriority)
        setFirstLastPiecePriority(params.firstLastPiecePriority);

    if (!params.restored && hasMetadata()) {
        if (filesCount() == 1)
            m_hasRootFolder = false;
    }

    // TODO: Remove the following upgrade code in v.4.4
    // == BEGIN UPGRADE CODE ==
    const QString spath = actualStorageLocation();
    for (int i = 0; i < filesCount(); ++i) {
        const QString filepath = filePath(i);
        // Move "unwanted" files back to their original folder
        const QString parentRelPath = Utils::Fs::branchPath(filepath);
        if (QDir(parentRelPath).dirName() == ".unwanted") {
            const QString oldName = Utils::Fs::fileName(filepath);
            const QString newRelPath = Utils::Fs::branchPath(parentRelPath);
            if (newRelPath.isEmpty())
                renameFile(i, oldName);
            else
                renameFile(i, QDir(newRelPath).filePath(oldName));

            // Remove .unwanted directory if empty
            qDebug() << "Attempting to remove \".unwanted\" folder at " << QDir(spath + '/' + newRelPath).absoluteFilePath(".unwanted");
            QDir(spath + '/' + newRelPath).rmdir(".unwanted");
        }
    }
    // == END UPGRADE CODE ==
}

TorrentHandleImpl::~TorrentHandleImpl() {}

bool TorrentHandleImpl::isValid() const
{
    return m_nativeHandle.is_valid();
}

InfoHash TorrentHandleImpl::hash() const
{
    return m_hash;
}

QString TorrentHandleImpl::name() const
{
    QString name = m_name;
    if (!name.isEmpty()) return name;

    name = QString::fromStdString(m_nativeStatus.name);
    if (!name.isEmpty()) return name;

    if (hasMetadata()) {
        name = QString::fromStdString(m_torrentInfo.nativeInfo()->orig_files().name());
        if (!name.isEmpty()) return name;
    }

    return m_hash;
}

QDateTime TorrentHandleImpl::creationDate() const
{
    return m_torrentInfo.creationDate();
}

QString TorrentHandleImpl::creator() const
{
    return m_torrentInfo.creator();
}

QString TorrentHandleImpl::comment() const
{
    return m_torrentInfo.comment();
}

bool TorrentHandleImpl::isPrivate() const
{
    return m_torrentInfo.isPrivate();
}

qlonglong TorrentHandleImpl::totalSize() const
{
    return m_torrentInfo.totalSize();
}

// size without the "don't download" files
qlonglong TorrentHandleImpl::wantedSize() const
{
    return m_nativeStatus.total_wanted;
}

qlonglong TorrentHandleImpl::completedSize() const
{
    return m_nativeStatus.total_wanted_done;
}

qlonglong TorrentHandleImpl::incompletedSize() const
{
    return (m_nativeStatus.total_wanted - m_nativeStatus.total_wanted_done);
}

qlonglong TorrentHandleImpl::pieceLength() const
{
    return m_torrentInfo.pieceLength();
}

qlonglong TorrentHandleImpl::wastedSize() const
{
    return (m_nativeStatus.total_failed_bytes + m_nativeStatus.total_redundant_bytes);
}

QString TorrentHandleImpl::currentTracker() const
{
    return QString::fromStdString(m_nativeStatus.current_tracker);
}

QString TorrentHandleImpl::savePath(bool actual) const
{
    if (actual)
        return Utils::Fs::toUniformPath(actualStorageLocation());
    else
        return Utils::Fs::toUniformPath(m_savePath);
}

QString TorrentHandleImpl::rootPath(bool actual) const
{
    if ((filesCount() > 1) && !hasRootFolder())
        return {};

    const QString firstFilePath = filePath(0);
    const int slashIndex = firstFilePath.indexOf('/');
    if (slashIndex >= 0)
        return QDir(savePath(actual)).absoluteFilePath(firstFilePath.left(slashIndex));
    else
        return QDir(savePath(actual)).absoluteFilePath(firstFilePath);
}

QString TorrentHandleImpl::contentPath(const bool actual) const
{
    if (filesCount() == 1)
        return QDir(savePath(actual)).absoluteFilePath(filePath(0));

    if (hasRootFolder())
        return rootPath(actual);

    return savePath(actual);
}

bool TorrentHandleImpl::isAutoTMMEnabled() const
{
    return m_useAutoTMM;
}

void TorrentHandleImpl::setAutoTMMEnabled(bool enabled)
{
    if (m_useAutoTMM == enabled) return;

    m_useAutoTMM = enabled;
    m_session->handleTorrentSavingModeChanged(this);

    if (m_useAutoTMM)
        move_impl(m_session->categorySavePath(m_category), MoveStorageMode::Overwrite);
}

bool TorrentHandleImpl::hasRootFolder() const
{
    return m_hasRootFolder;
}

QString TorrentHandleImpl::actualStorageLocation() const
{
    return QString::fromStdString(m_nativeStatus.save_path);
}

bool TorrentHandleImpl::isAutoManaged() const
{
    return static_cast<bool>(m_nativeStatus.flags & lt::torrent_flags::auto_managed);
}

void TorrentHandleImpl::setAutoManaged(const bool enable)
{
    if (enable)
        m_nativeHandle.set_flags(lt::torrent_flags::auto_managed);
    else
        m_nativeHandle.unset_flags(lt::torrent_flags::auto_managed);
}

QVector<TrackerEntry> TorrentHandleImpl::trackers() const
{
    const std::vector<lt::announce_entry> nativeTrackers = m_nativeHandle.trackers();

    QVector<TrackerEntry> entries;
    entries.reserve(nativeTrackers.size());

    for (const lt::announce_entry &tracker : nativeTrackers)
        entries << tracker;

    return entries;
}

QHash<QString, TrackerInfo> TorrentHandleImpl::trackerInfos() const
{
    return m_trackerInfos;
}

void TorrentHandleImpl::addTrackers(const QVector<TrackerEntry> &trackers)
{
    QSet<TrackerEntry> currentTrackers;
    for (const lt::announce_entry &entry : m_nativeHandle.trackers())
        currentTrackers << entry;

    QVector<TrackerEntry> newTrackers;
    newTrackers.reserve(trackers.size());

    for (const TrackerEntry &tracker : trackers) {
        if (!currentTrackers.contains(tracker)) {
            m_nativeHandle.add_tracker(tracker.nativeEntry());
            newTrackers << tracker;
        }
    }

    if (!newTrackers.isEmpty())
        m_session->handleTorrentTrackersAdded(this, newTrackers);
}

void TorrentHandleImpl::replaceTrackers(const QVector<TrackerEntry> &trackers)
{
    QVector<TrackerEntry> currentTrackers = this->trackers();

    QVector<TrackerEntry> newTrackers;
    newTrackers.reserve(trackers.size());

    std::vector<lt::announce_entry> nativeTrackers;
    nativeTrackers.reserve(trackers.size());

    for (const TrackerEntry &tracker : trackers) {
        nativeTrackers.emplace_back(tracker.nativeEntry());

        if (!currentTrackers.removeOne(tracker))
            newTrackers << tracker;
    }

    m_nativeHandle.replace_trackers(nativeTrackers);

    if (newTrackers.isEmpty() && currentTrackers.isEmpty()) {
        // when existing tracker reorders
        m_session->handleTorrentTrackersChanged(this);
    }
    else {
        if (!currentTrackers.isEmpty())
            m_session->handleTorrentTrackersRemoved(this, currentTrackers);

        if (!newTrackers.isEmpty())
            m_session->handleTorrentTrackersAdded(this, newTrackers);

        // Clear the peer list if it's a private torrent since
        // we do not want to keep connecting with peers from old tracker.
        if (isPrivate())
            clearPeers();
    }
}

QVector<QUrl> TorrentHandleImpl::urlSeeds() const
{
    const std::set<std::string> currentSeeds = m_nativeHandle.url_seeds();

    QVector<QUrl> urlSeeds;
    urlSeeds.reserve(currentSeeds.size());

    for (const std::string &urlSeed : currentSeeds)
        urlSeeds.append(QUrl(urlSeed.c_str()));

    return urlSeeds;
}

void TorrentHandleImpl::addUrlSeeds(const QVector<QUrl> &urlSeeds)
{
    const std::set<std::string> currentSeeds = m_nativeHandle.url_seeds();

    QVector<QUrl> addedUrlSeeds;
    addedUrlSeeds.reserve(urlSeeds.size());

    for (const QUrl &url : urlSeeds) {
        const std::string nativeUrl = url.toString().toStdString();
        if (currentSeeds.find(nativeUrl) == currentSeeds.end()) {
            m_nativeHandle.add_url_seed(nativeUrl);
            addedUrlSeeds << url;
        }
    }

    if (!addedUrlSeeds.isEmpty())
        m_session->handleTorrentUrlSeedsAdded(this, addedUrlSeeds);
}

void TorrentHandleImpl::removeUrlSeeds(const QVector<QUrl> &urlSeeds)
{
    const std::set<std::string> currentSeeds = m_nativeHandle.url_seeds();

    QVector<QUrl> removedUrlSeeds;
    removedUrlSeeds.reserve(urlSeeds.size());

    for (const QUrl &url : urlSeeds) {
        const std::string nativeUrl = url.toString().toStdString();
        if (currentSeeds.find(nativeUrl) != currentSeeds.end()) {
            m_nativeHandle.remove_url_seed(nativeUrl);
            removedUrlSeeds << url;
        }
    }

    if (!removedUrlSeeds.isEmpty())
        m_session->handleTorrentUrlSeedsRemoved(this, removedUrlSeeds);
}

void TorrentHandleImpl::clearPeers()
{
#if (LIBTORRENT_VERSION_NUM >= 10207)
    m_nativeHandle.clear_peers();
#endif
}

bool TorrentHandleImpl::connectPeer(const PeerAddress &peerAddress)
{
    lt::error_code ec;
    const lt::address addr = lt::make_address(peerAddress.ip.toString().toStdString(), ec);
    if (ec) return false;

    const lt::tcp::endpoint endpoint(addr, peerAddress.port);
    try {
        m_nativeHandle.connect_peer(endpoint);
    }
    catch (const lt::system_error &err) {
        LogMsg(tr("Failed to add peer \"%1\" to torrent \"%2\". Reason: %3")
            .arg(peerAddress.toString(), name(), QString::fromLocal8Bit(err.what())), Log::WARNING);
        return false;
    }

    LogMsg(tr("Peer \"%1\" is added to torrent \"%2\"").arg(peerAddress.toString(), name()));
    return true;
}

bool TorrentHandleImpl::needSaveResumeData() const
{
    return m_nativeHandle.need_save_resume_data();
}

void TorrentHandleImpl::saveResumeData()
{
    m_nativeHandle.save_resume_data();
    m_session->handleTorrentSaveResumeDataRequested(this);
}

int TorrentHandleImpl::filesCount() const
{
    return m_torrentInfo.filesCount();
}

int TorrentHandleImpl::piecesCount() const
{
    return m_torrentInfo.piecesCount();
}

int TorrentHandleImpl::piecesHave() const
{
    return m_nativeStatus.num_pieces;
}

qreal TorrentHandleImpl::progress() const
{
    if (!isChecking()) {
        if (!m_nativeStatus.total_wanted)
            return 0.;

        if (m_nativeStatus.total_wanted_done == m_nativeStatus.total_wanted)
            return 1.;

        const qreal progress = static_cast<qreal>(m_nativeStatus.total_wanted_done) / m_nativeStatus.total_wanted;
        Q_ASSERT((progress >= 0.f) && (progress <= 1.f));
        return progress;
    }

    return m_nativeStatus.progress;
}

QString TorrentHandleImpl::category() const
{
    return m_category;
}

bool TorrentHandleImpl::belongsToCategory(const QString &category) const
{
    if (m_category.isEmpty()) return category.isEmpty();
    if (!Session::isValidCategoryName(category)) return false;

    if (m_category == category) return true;

    if (m_session->isSubcategoriesEnabled() && m_category.startsWith(category + '/'))
        return true;

    return false;
}

QSet<QString> TorrentHandleImpl::tags() const
{
    return m_tags;
}

bool TorrentHandleImpl::hasTag(const QString &tag) const
{
    return m_tags.contains(tag);
}

bool TorrentHandleImpl::addTag(const QString &tag)
{
    if (!Session::isValidTag(tag))
        return false;

    if (!hasTag(tag)) {
        if (!m_session->hasTag(tag))
            if (!m_session->addTag(tag))
                return false;
        m_tags.insert(tag);
        m_session->handleTorrentTagAdded(this, tag);
        return true;
    }
    return false;
}

bool TorrentHandleImpl::removeTag(const QString &tag)
{
    if (m_tags.remove(tag)) {
        m_session->handleTorrentTagRemoved(this, tag);
        return true;
    }
    return false;
}

void TorrentHandleImpl::removeAllTags()
{
    for (const QString &tag : asConst(tags()))
        removeTag(tag);
}

QDateTime TorrentHandleImpl::addedTime() const
{
    return QDateTime::fromSecsSinceEpoch(m_nativeStatus.added_time);
}

qreal TorrentHandleImpl::ratioLimit() const
{
    return m_ratioLimit;
}

int TorrentHandleImpl::seedingTimeLimit() const
{
    return m_seedingTimeLimit;
}

QString TorrentHandleImpl::filePath(int index) const
{
    return m_torrentInfo.filePath(index);
}

QString TorrentHandleImpl::fileName(int index) const
{
    if (!hasMetadata()) return {};
    return Utils::Fs::fileName(filePath(index));
}

qlonglong TorrentHandleImpl::fileSize(int index) const
{
    return m_torrentInfo.fileSize(index);
}

// Return a list of absolute paths corresponding
// to all files in a torrent
QStringList TorrentHandleImpl::absoluteFilePaths() const
{
    if (!hasMetadata()) return {};

    const QDir saveDir(savePath(true));
    QStringList res;
    for (int i = 0; i < filesCount(); ++i)
        res << Utils::Fs::expandPathAbs(saveDir.absoluteFilePath(filePath(i)));
    return res;
}

QVector<DownloadPriority> TorrentHandleImpl::filePriorities() const
{
    const std::vector<lt::download_priority_t> fp = m_nativeHandle.get_file_priorities();

    QVector<DownloadPriority> ret;
    std::transform(fp.cbegin(), fp.cend(), std::back_inserter(ret), [](lt::download_priority_t priority)
    {
        return static_cast<DownloadPriority>(static_cast<LTUnderlyingType<lt::download_priority_t>>(priority));
    });
    return ret;
}

TorrentInfo TorrentHandleImpl::info() const
{
    return m_torrentInfo;
}

bool TorrentHandleImpl::isPaused() const
{
    return ((m_nativeStatus.flags & lt::torrent_flags::paused)
            && !isAutoManaged());
}

bool TorrentHandleImpl::isResumed() const
{
    return !isPaused();
}

bool TorrentHandleImpl::isQueued() const
{
    return ((m_nativeStatus.flags & lt::torrent_flags::paused)
            && isAutoManaged());
}

bool TorrentHandleImpl::isChecking() const
{
    return ((m_nativeStatus.state == lt::torrent_status::checking_files)
            || (m_nativeStatus.state == lt::torrent_status::checking_resume_data));
}

bool TorrentHandleImpl::isDownloading() const
{
    return m_state == TorrentState::Downloading
            || m_state == TorrentState::DownloadingMetadata
            || m_state == TorrentState::StalledDownloading
            || m_state == TorrentState::CheckingDownloading
            || m_state == TorrentState::PausedDownloading
            || m_state == TorrentState::QueuedDownloading
            || m_state == TorrentState::ForcedDownloading;
}

bool TorrentHandleImpl::isUploading() const
{
    return m_state == TorrentState::Uploading
            || m_state == TorrentState::StalledUploading
            || m_state == TorrentState::CheckingUploading
            || m_state == TorrentState::QueuedUploading
            || m_state == TorrentState::ForcedUploading;
}

bool TorrentHandleImpl::isCompleted() const
{
    return m_state == TorrentState::Uploading
            || m_state == TorrentState::StalledUploading
            || m_state == TorrentState::CheckingUploading
            || m_state == TorrentState::PausedUploading
            || m_state == TorrentState::QueuedUploading
            || m_state == TorrentState::ForcedUploading;
}

bool TorrentHandleImpl::isActive() const
{
    if (m_state == TorrentState::StalledDownloading)
        return (uploadPayloadRate() > 0);

    return m_state == TorrentState::DownloadingMetadata
            || m_state == TorrentState::Downloading
            || m_state == TorrentState::ForcedDownloading
            || m_state == TorrentState::Uploading
            || m_state == TorrentState::ForcedUploading
            || m_state == TorrentState::Moving;
}

bool TorrentHandleImpl::isInactive() const
{
    return !isActive();
}

bool TorrentHandleImpl::isErrored() const
{
    return m_state == TorrentState::MissingFiles
            || m_state == TorrentState::Error;
}

bool TorrentHandleImpl::isSeed() const
{
    // Affected by bug http://code.rasterbar.com/libtorrent/ticket/402
    //bool result;
    //result = m_nativeHandle.is_seed());
    //return result;
    // May suffer from approximation problems
    //return (progress() == 1.);
    // This looks safe
    return ((m_nativeStatus.state == lt::torrent_status::finished)
            || (m_nativeStatus.state == lt::torrent_status::seeding));
}

bool TorrentHandleImpl::isForced() const
{
    return (!(m_nativeStatus.flags & lt::torrent_flags::paused)
            && !isAutoManaged());
}

bool TorrentHandleImpl::isSequentialDownload() const
{
    return static_cast<bool>(m_nativeStatus.flags & lt::torrent_flags::sequential_download);
}

bool TorrentHandleImpl::hasFirstLastPiecePriority() const
{
    if (!hasMetadata())
        return m_needsToSetFirstLastPiecePriority;

    const std::vector<lt::download_priority_t> filePriorities = nativeHandle().get_file_priorities();
    for (int i = 0; i < static_cast<int>(filePriorities.size()); ++i) {
        if (filePriorities[i] <= lt::download_priority_t {0})
            continue;

        const TorrentInfo::PieceRange extremities = info().filePieces(i);
        const lt::download_priority_t firstPiecePrio = nativeHandle().piece_priority(lt::piece_index_t {extremities.first()});
        const lt::download_priority_t lastPiecePrio = nativeHandle().piece_priority(lt::piece_index_t {extremities.last()});
        return ((firstPiecePrio == lt::download_priority_t {7}) && (lastPiecePrio == lt::download_priority_t {7}));
    }

    return false;
}

TorrentState TorrentHandleImpl::state() const
{
    return m_state;
}

void TorrentHandleImpl::updateState()
{
    if (m_nativeStatus.state == lt::torrent_status::checking_resume_data) {
        m_state = TorrentState::CheckingResumeData;
    }
    else if (m_nativeStatus.state == lt::torrent_status::checking_files) {
        m_state = m_hasSeedStatus ? TorrentState::CheckingUploading : TorrentState::CheckingDownloading;
    }
    else if (m_nativeStatus.state == lt::torrent_status::allocating) {
        m_state = TorrentState::Allocating;
    }
    else if (isMoveInProgress()) {
        m_state = TorrentState::Moving;
    }
    else if (hasError()) {
        m_state = TorrentState::Error;
    }
    else if (hasMissingFiles()) {
        m_state = TorrentState::MissingFiles;
    }
    else if (isPaused()) {
        m_state = isSeed() ? TorrentState::PausedUploading : TorrentState::PausedDownloading;
    }
    else if (m_session->isQueueingSystemEnabled() && isQueued() && !isChecking()) {
        m_state = isSeed() ? TorrentState::QueuedUploading : TorrentState::QueuedDownloading;
    }
    else {
        switch (m_nativeStatus.state) {
        case lt::torrent_status::finished:
        case lt::torrent_status::seeding:
            if (isForced())
                m_state = TorrentState::ForcedUploading;
            else
                m_state = m_nativeStatus.upload_payload_rate > 0 ? TorrentState::Uploading : TorrentState::StalledUploading;
            break;
        case lt::torrent_status::downloading_metadata:
            m_state = TorrentState::DownloadingMetadata;
            break;
        case lt::torrent_status::downloading:
            if (isForced())
                m_state = TorrentState::ForcedDownloading;
            else
                m_state = m_nativeStatus.download_payload_rate > 0 ? TorrentState::Downloading : TorrentState::StalledDownloading;
            break;
        default:
            qWarning("Unrecognized torrent status, should not happen!!! status was %d", m_nativeStatus.state);
            m_state = TorrentState::Unknown;
        }
    }
}

bool TorrentHandleImpl::hasMetadata() const
{
    return m_nativeStatus.has_metadata;
}

bool TorrentHandleImpl::hasMissingFiles() const
{
    return m_hasMissingFiles;
}

bool TorrentHandleImpl::hasError() const
{
    return static_cast<bool>(m_nativeStatus.errc);
}

bool TorrentHandleImpl::hasFilteredPieces() const
{
    const std::vector<lt::download_priority_t> pp = m_nativeHandle.get_piece_priorities();
    return std::any_of(pp.cbegin(), pp.cend(), [](const lt::download_priority_t priority)
    {
        return (priority == lt::download_priority_t {0});
    });
}

int TorrentHandleImpl::queuePosition() const
{
    if (m_nativeStatus.queue_position < lt::queue_position_t {0}) return 0;

    return static_cast<int>(m_nativeStatus.queue_position) + 1;
}

QString TorrentHandleImpl::error() const
{
    return QString::fromStdString(m_nativeStatus.errc.message());
}

qlonglong TorrentHandleImpl::totalDownload() const
{
    return m_nativeStatus.all_time_download;
}

qlonglong TorrentHandleImpl::totalUpload() const
{
    return m_nativeStatus.all_time_upload;
}

qlonglong TorrentHandleImpl::activeTime() const
{
    return lt::total_seconds(m_nativeStatus.active_duration);
}

qlonglong TorrentHandleImpl::finishedTime() const
{
    return lt::total_seconds(m_nativeStatus.finished_duration);
}

qlonglong TorrentHandleImpl::seedingTime() const
{
    return lt::total_seconds(m_nativeStatus.seeding_duration);
}

qlonglong TorrentHandleImpl::eta() const
{
    if (isPaused()) return MAX_ETA;

    const SpeedSampleAvg speedAverage = m_speedMonitor.average();

    if (isSeed()) {
        const qreal maxRatioValue = maxRatio();
        const int maxSeedingTimeValue = maxSeedingTime();
        if ((maxRatioValue < 0) && (maxSeedingTimeValue < 0)) return MAX_ETA;

        qlonglong ratioEta = MAX_ETA;

        if ((speedAverage.upload > 0) && (maxRatioValue >= 0)) {

            qlonglong realDL = totalDownload();
            if (realDL <= 0)
                realDL = wantedSize();

            ratioEta = ((realDL * maxRatioValue) - totalUpload()) / speedAverage.upload;
        }

        qlonglong seedingTimeEta = MAX_ETA;

        if (maxSeedingTimeValue >= 0) {
            seedingTimeEta = (maxSeedingTimeValue * 60) - seedingTime();
            if (seedingTimeEta < 0)
                seedingTimeEta = 0;
        }

        return qMin(ratioEta, seedingTimeEta);
    }

    if (!speedAverage.download) return MAX_ETA;

    return (wantedSize() - completedSize()) / speedAverage.download;
}

QVector<qreal> TorrentHandleImpl::filesProgress() const
{
    std::vector<int64_t> fp;
    m_nativeHandle.file_progress(fp, lt::torrent_handle::piece_granularity);

    const int count = static_cast<int>(fp.size());
    QVector<qreal> result;
    result.reserve(count);
    for (int i = 0; i < count; ++i) {
        const qlonglong size = fileSize(i);
        if ((size <= 0) || (fp[i] == size))
            result << 1;
        else
            result << (fp[i] / static_cast<qreal>(size));
    }

    return result;
}

int TorrentHandleImpl::seedsCount() const
{
    return m_nativeStatus.num_seeds;
}

int TorrentHandleImpl::peersCount() const
{
    return m_nativeStatus.num_peers;
}

int TorrentHandleImpl::leechsCount() const
{
    return (m_nativeStatus.num_peers - m_nativeStatus.num_seeds);
}

int TorrentHandleImpl::totalSeedsCount() const
{
    return (m_nativeStatus.num_complete > 0) ? m_nativeStatus.num_complete : m_nativeStatus.list_seeds;
}

int TorrentHandleImpl::totalPeersCount() const
{
    const int peers = m_nativeStatus.num_complete + m_nativeStatus.num_incomplete;
    return (peers > 0) ? peers : m_nativeStatus.list_peers;
}

int TorrentHandleImpl::totalLeechersCount() const
{
    return (m_nativeStatus.num_incomplete > 0) ? m_nativeStatus.num_incomplete : (m_nativeStatus.list_peers - m_nativeStatus.list_seeds);
}

int TorrentHandleImpl::completeCount() const
{
    // additional info: https://github.com/qbittorrent/qBittorrent/pull/5300#issuecomment-267783646
    return m_nativeStatus.num_complete;
}

int TorrentHandleImpl::incompleteCount() const
{
    // additional info: https://github.com/qbittorrent/qBittorrent/pull/5300#issuecomment-267783646
    return m_nativeStatus.num_incomplete;
}

QDateTime TorrentHandleImpl::lastSeenComplete() const
{
    if (m_nativeStatus.last_seen_complete > 0)
        return QDateTime::fromSecsSinceEpoch(m_nativeStatus.last_seen_complete);
    else
        return {};
}

QDateTime TorrentHandleImpl::completedTime() const
{
    if (m_nativeStatus.completed_time > 0)
        return QDateTime::fromSecsSinceEpoch(m_nativeStatus.completed_time);
    else
        return {};
}

qlonglong TorrentHandleImpl::timeSinceUpload() const
{
    if (m_nativeStatus.last_upload.time_since_epoch().count() == 0)
        return -1;
    return lt::total_seconds(lt::clock_type::now() - m_nativeStatus.last_upload);
}

qlonglong TorrentHandleImpl::timeSinceDownload() const
{
    if (m_nativeStatus.last_download.time_since_epoch().count() == 0)
        return -1;
    return lt::total_seconds(lt::clock_type::now() - m_nativeStatus.last_download);
}

qlonglong TorrentHandleImpl::timeSinceActivity() const
{
    const qlonglong upTime = timeSinceUpload();
    const qlonglong downTime = timeSinceDownload();
    return ((upTime < 0) != (downTime < 0))
        ? std::max(upTime, downTime)
        : std::min(upTime, downTime);
}

int TorrentHandleImpl::downloadLimit() const
{
    return m_nativeHandle.download_limit();
}

int TorrentHandleImpl::uploadLimit() const
{
    return m_nativeHandle.upload_limit();
}

bool TorrentHandleImpl::superSeeding() const
{
    return static_cast<bool>(m_nativeStatus.flags & lt::torrent_flags::super_seeding);
}

QVector<PeerInfo> TorrentHandleImpl::peers() const
{
    std::vector<lt::peer_info> nativePeers;
    m_nativeHandle.get_peer_info(nativePeers);

    QVector<PeerInfo> peers;
    peers.reserve(nativePeers.size());
    for (const lt::peer_info &peer : nativePeers)
        peers << PeerInfo(this, peer);
    return peers;
}

QBitArray TorrentHandleImpl::pieces() const
{
    QBitArray result(m_nativeStatus.pieces.size());
    for (int i = 0; i < result.size(); ++i) {
        if (m_nativeStatus.pieces[lt::piece_index_t {i}])
            result.setBit(i, true);
    }
    return result;
}

QBitArray TorrentHandleImpl::downloadingPieces() const
{
    QBitArray result(piecesCount());

    std::vector<lt::partial_piece_info> queue;
    m_nativeHandle.get_download_queue(queue);

    for (const lt::partial_piece_info &info : queue)
        result.setBit(static_cast<LTUnderlyingType<lt::piece_index_t>>(info.piece_index));

    return result;
}

QVector<int> TorrentHandleImpl::pieceAvailability() const
{
    std::vector<int> avail;
    m_nativeHandle.piece_availability(avail);

    return Vector::fromStdVector(avail);
}

qreal TorrentHandleImpl::distributedCopies() const
{
    return m_nativeStatus.distributed_copies;
}

qreal TorrentHandleImpl::maxRatio() const
{
    if (m_ratioLimit == USE_GLOBAL_RATIO)
        return m_session->globalMaxRatio();

    return m_ratioLimit;
}

int TorrentHandleImpl::maxSeedingTime() const
{
    if (m_seedingTimeLimit == USE_GLOBAL_SEEDING_TIME)
        return m_session->globalMaxSeedingMinutes();

    return m_seedingTimeLimit;
}

qreal TorrentHandleImpl::realRatio() const
{
    const int64_t upload = m_nativeStatus.all_time_upload;
    // special case for a seeder who lost its stats, also assume nobody will import a 99% done torrent
    const int64_t download = (m_nativeStatus.all_time_download < (m_nativeStatus.total_done * 0.01))
        ? m_nativeStatus.total_done
        : m_nativeStatus.all_time_download;

    if (download == 0)
        return (upload == 0) ? 0 : MAX_RATIO;

    const qreal ratio = upload / static_cast<qreal>(download);
    Q_ASSERT(ratio >= 0);
    return (ratio > MAX_RATIO) ? MAX_RATIO : ratio;
}

int TorrentHandleImpl::uploadPayloadRate() const
{
    return m_nativeStatus.upload_payload_rate;
}

int TorrentHandleImpl::downloadPayloadRate() const
{
    return m_nativeStatus.download_payload_rate;
}

qlonglong TorrentHandleImpl::totalPayloadUpload() const
{
    return m_nativeStatus.total_payload_upload;
}

qlonglong TorrentHandleImpl::totalPayloadDownload() const
{
    return m_nativeStatus.total_payload_download;
}

int TorrentHandleImpl::connectionsCount() const
{
    return m_nativeStatus.num_connections;
}

int TorrentHandleImpl::connectionsLimit() const
{
    return m_nativeStatus.connections_limit;
}

qlonglong TorrentHandleImpl::nextAnnounce() const
{
    return lt::total_seconds(m_nativeStatus.next_announce);
}

void TorrentHandleImpl::setName(const QString &name)
{
    if (m_name != name) {
        m_name = name;
        m_session->handleTorrentNameChanged(this);
    }
}

bool TorrentHandleImpl::setCategory(const QString &category)
{
    if (m_category != category) {
        if (!category.isEmpty() && !m_session->categories().contains(category))
            return false;

        const QString oldCategory = m_category;
        m_category = category;
        m_session->handleTorrentCategoryChanged(this, oldCategory);

        if (m_useAutoTMM) {
            if (!m_session->isDisableAutoTMMWhenCategoryChanged())
                move_impl(m_session->categorySavePath(m_category), MoveStorageMode::Overwrite);
            else
                setAutoTMMEnabled(false);
        }
    }

    return true;
}

void TorrentHandleImpl::move(QString path)
{
    if (m_useAutoTMM) {
        m_useAutoTMM = false;
        m_session->handleTorrentSavingModeChanged(this);
    }

    path = Utils::Fs::toUniformPath(path.trimmed());
    if (path.isEmpty())
        path = m_session->defaultSavePath();
    if (!path.endsWith('/'))
        path += '/';

    move_impl(path, MoveStorageMode::KeepExistingFiles);
}

void TorrentHandleImpl::move_impl(QString path, const MoveStorageMode mode)
{
    if (path == savePath()) return;
    path = Utils::Fs::toNativePath(path);

    if (!useTempPath()) {
        moveStorage(path, mode);
    }
    else {
        m_savePath = path;
        m_session->handleTorrentSavePathChanged(this);
    }
}

void TorrentHandleImpl::forceReannounce(int index)
{
    m_nativeHandle.force_reannounce(0, index);
}

void TorrentHandleImpl::forceDHTAnnounce()
{
    m_nativeHandle.force_dht_announce();
}

void TorrentHandleImpl::forceRecheck()
{
    if (!hasMetadata()) return;

    m_nativeHandle.force_recheck();
    m_unchecked = false;
}

void TorrentHandleImpl::setSequentialDownload(const bool enable)
{
    if (enable) {
        m_nativeHandle.set_flags(lt::torrent_flags::sequential_download);
        m_nativeStatus.flags |= lt::torrent_flags::sequential_download;  // prevent return cached value
    }
    else {
        m_nativeHandle.unset_flags(lt::torrent_flags::sequential_download);
        m_nativeStatus.flags &= ~lt::torrent_flags::sequential_download;  // prevent return cached value
    }

    saveResumeData();
}

void TorrentHandleImpl::setFirstLastPiecePriority(const bool enabled)
{
    setFirstLastPiecePriorityImpl(enabled);
}

void TorrentHandleImpl::setFirstLastPiecePriorityImpl(const bool enabled, const QVector<DownloadPriority> &updatedFilePrio)
{
    // Download first and last pieces first for every file in the torrent

    if (!hasMetadata()) {
        m_needsToSetFirstLastPiecePriority = enabled;
        return;
    }

    const std::vector<lt::download_priority_t> filePriorities = !updatedFilePrio.isEmpty() ? toLTDownloadPriorities(updatedFilePrio)
                                                                           : nativeHandle().get_file_priorities();
    std::vector<lt::download_priority_t> piecePriorities = nativeHandle().get_piece_priorities();

    // Updating file priorities is an async operation in libtorrent, when we just updated it and immediately query it
    // we might get the old/wrong values, so we rely on `updatedFilePrio` in this case.
    for (int index = 0; index < static_cast<int>(filePriorities.size()); ++index) {
        const lt::download_priority_t filePrio = filePriorities[index];
        if (filePrio <= lt::download_priority_t {0})
            continue;

        // Determine the priority to set
        const lt::download_priority_t newPrio = enabled ? lt::download_priority_t {7} : filePrio;
        const TorrentInfo::PieceRange extremities = info().filePieces(index);

        // worst case: AVI index = 1% of total file size (at the end of the file)
        const int nNumPieces = std::ceil(fileSize(index) * 0.01 / pieceLength());
        for (int i = 0; i < nNumPieces; ++i) {
            piecePriorities[extremities.first() + i] = newPrio;
            piecePriorities[extremities.last() - i] = newPrio;
        }
    }

    m_nativeHandle.prioritize_pieces(piecePriorities);

    LogMsg(tr("Download first and last piece first: %1, torrent: '%2'")
        .arg((enabled ? tr("On") : tr("Off")), name()));

    saveResumeData();
}

void TorrentHandleImpl::pause()
{
    if (isPaused()) return;

    setAutoManaged(false);
    m_nativeHandle.pause();

    // Libtorrent doesn't emit a torrent_paused_alert when the
    // torrent is queued (no I/O)
    // We test on the cached m_nativeStatus
    if (isQueued())
        m_session->handleTorrentPaused(this);
}

void TorrentHandleImpl::resume(bool forced)
{
    resume_impl(forced);
}

void TorrentHandleImpl::resume_impl(bool forced)
{
    if (hasError())
        m_nativeHandle.clear_error();

    if (m_hasMissingFiles) {
        m_hasMissingFiles = false;
        m_nativeHandle.force_recheck();
    }

    setAutoManaged(!forced);
    if (forced)
        m_nativeHandle.resume();
}

void TorrentHandleImpl::moveStorage(const QString &newPath, const MoveStorageMode mode)
{
    if (m_session->addMoveTorrentStorageJob(this, newPath, mode)) {
        m_storageIsMoving = true;
        updateStatus();
    }
}

void TorrentHandleImpl::renameFile(const int index, const QString &name)
{
    m_oldPath[lt::file_index_t {index}].push_back(filePath(index));
    ++m_renameCount;
    m_nativeHandle.rename_file(lt::file_index_t {index}, Utils::Fs::toNativePath(name).toStdString());
}

void TorrentHandleImpl::handleStateUpdate(const lt::torrent_status &nativeStatus)
{
    updateStatus(nativeStatus);
}

void TorrentHandleImpl::handleMoveStorageJobFinished(const bool hasOutstandingJob)
{
    m_storageIsMoving = hasOutstandingJob;

    updateStatus();
    const QString newPath = QString::fromStdString(m_nativeStatus.save_path);
    if (!useTempPath() && (newPath != m_savePath)) {
        m_savePath = newPath;
        m_session->handleTorrentSavePathChanged(this);
    }

    saveResumeData();

    while ((m_renameCount == 0) && !m_moveFinishedTriggers.isEmpty())
        m_moveFinishedTriggers.takeFirst()();
}

void TorrentHandleImpl::handleTrackerReplyAlert(const lt::tracker_reply_alert *p)
{
    const QString trackerUrl(p->tracker_url());
    qDebug("Received a tracker reply from %s (Num_peers = %d)", qUtf8Printable(trackerUrl), p->num_peers);
    // Connection was successful now. Remove possible old errors
    m_trackerInfos[trackerUrl] = {{}, p->num_peers};

    m_session->handleTorrentTrackerReply(this, trackerUrl);
}

void TorrentHandleImpl::handleTrackerWarningAlert(const lt::tracker_warning_alert *p)
{
    const QString trackerUrl = p->tracker_url();
    const QString message = p->warning_message();

    // Connection was successful now but there is a warning message
    m_trackerInfos[trackerUrl].lastMessage = message; // Store warning message

    m_session->handleTorrentTrackerWarning(this, trackerUrl);
}

void TorrentHandleImpl::handleTrackerErrorAlert(const lt::tracker_error_alert *p)
{
    const QString trackerUrl = p->tracker_url();
    const QString message = p->error_message();

    m_trackerInfos[trackerUrl].lastMessage = message;

    // Starting with libtorrent 1.2.x each tracker has multiple local endpoints from which
    // an announce is attempted. Some endpoints might succeed while others might fail.
    // Emit the signal only if all endpoints have failed.
    const QVector<TrackerEntry> trackerList = trackers();
    const auto iter = std::find_if(trackerList.cbegin(), trackerList.cend(), [&trackerUrl](const TrackerEntry &entry)
    {
        return (entry.url() == trackerUrl);
    });
    if ((iter != trackerList.cend()) && (iter->status() == TrackerEntry::NotWorking))
        m_session->handleTorrentTrackerError(this, trackerUrl);
}

void TorrentHandleImpl::handleTorrentCheckedAlert(const lt::torrent_checked_alert *p)
{
    Q_UNUSED(p);
    qDebug("\"%s\" have just finished checking", qUtf8Printable(name()));

    if (m_fastresumeDataRejected && !m_hasMissingFiles) {
        saveResumeData();
        m_fastresumeDataRejected = false;
    }

    updateStatus();

    if (!m_hasMissingFiles) {
        if ((progress() < 1.0) && (wantedSize() > 0))
            m_hasSeedStatus = false;
        else if (progress() == 1.0)
            m_hasSeedStatus = true;

        adjustActualSavePath();
        manageIncompleteFiles();
    }

    m_session->handleTorrentChecked(this);
}

void TorrentHandleImpl::handleTorrentFinishedAlert(const lt::torrent_finished_alert *p)
{
    Q_UNUSED(p);
    qDebug("Got a torrent finished alert for \"%s\"", qUtf8Printable(name()));
    qDebug("Torrent has seed status: %s", m_hasSeedStatus ? "yes" : "no");
    m_hasMissingFiles = false;
    if (m_hasSeedStatus) return;

    updateStatus();
    m_hasSeedStatus = true;

    adjustActualSavePath();
    manageIncompleteFiles();

    const bool recheckTorrentsOnCompletion = Preferences::instance()->recheckTorrentsOnCompletion();
    if (isMoveInProgress() || (m_renameCount > 0)) {
        if (recheckTorrentsOnCompletion)
            m_moveFinishedTriggers.append([this]() { forceRecheck(); });
        m_moveFinishedTriggers.append([this]() { m_session->handleTorrentFinished(this); });
    }
    else {
        if (recheckTorrentsOnCompletion && m_unchecked)
            forceRecheck();
        m_session->handleTorrentFinished(this);
    }
}

void TorrentHandleImpl::handleTorrentPausedAlert(const lt::torrent_paused_alert *p)
{
    Q_UNUSED(p);

    updateStatus();
    m_speedMonitor.reset();

    m_session->handleTorrentPaused(this);
}

void TorrentHandleImpl::handleTorrentResumedAlert(const lt::torrent_resumed_alert *p)
{
    Q_UNUSED(p);

    m_session->handleTorrentResumed(this);
}

void TorrentHandleImpl::handleSaveResumeDataAlert(const lt::save_resume_data_alert *p)
{
    if (p && !m_hasMissingFiles) {
        // Update recent resume data
        m_ltAddTorrentParams = p->params;
    }

    updateStatus();

    m_ltAddTorrentParams.added_time = addedTime().toSecsSinceEpoch();
    m_ltAddTorrentParams.save_path = Profile::instance()->toPortablePath(
                QString::fromStdString(m_ltAddTorrentParams.save_path)).toStdString();
    if (!m_hasMissingFiles) {
        m_ltAddTorrentParams.flags = m_nativeStatus.flags;
        if (m_nativeStatus.flags & lt::torrent_flags::stop_when_ready) {
            // We need to redefine these values when torrent starting/rechecking
            // in "paused" state since native values can be logically wrong
            // (torrent can be not paused and auto_managed when it is checking).
            m_ltAddTorrentParams.flags |= lt::torrent_flags::paused;
            m_ltAddTorrentParams.flags &= ~lt::torrent_flags::auto_managed;
            m_ltAddTorrentParams.flags &= ~lt::torrent_flags::stop_when_ready;
        }
    }

    auto resumeDataPtr = std::make_shared<lt::entry>(lt::write_resume_data(m_ltAddTorrentParams));
    lt::entry &resumeData = *resumeDataPtr;

    // TODO: The following code is deprecated. Remove after several releases in 4.3.x.
    // === BEGIN DEPRECATED CODE === //
    const bool useDummyResumeData = !p;
    if (useDummyResumeData) {
        resumeData["qBt-magnetUri"] = createMagnetURI().toStdString();
        // Both firstLastPiecePriority and sequential need to be stored in the
        // resume data if there is no metadata, otherwise they won't be
        // restored if qBittorrent quits before the metadata are retrieved:
        resumeData["qBt-firstLastPiecePriority"] = hasFirstLastPiecePriority();
        resumeData["qBt-sequential"] = isSequentialDownload();

        resumeData["qBt-addedTime"] = addedTime().toSecsSinceEpoch();
    }
    // === END DEPRECATED CODE === //

    resumeData["qBt-savePath"] = m_useAutoTMM ? "" : Profile::instance()->toPortablePath(m_savePath).toStdString();
    resumeData["qBt-ratioLimit"] = static_cast<int>(m_ratioLimit * 1000);
    resumeData["qBt-seedingTimeLimit"] = m_seedingTimeLimit;
    resumeData["qBt-category"] = m_category.toStdString();
    resumeData["qBt-tags"] = setToEntryList(m_tags);
    resumeData["qBt-name"] = m_name.toStdString();
    resumeData["qBt-seedStatus"] = m_hasSeedStatus;
    resumeData["qBt-hasRootFolder"] = m_hasRootFolder;

    m_session->handleTorrentResumeDataReady(this, resumeDataPtr);
}

void TorrentHandleImpl::handleSaveResumeDataFailedAlert(const lt::save_resume_data_failed_alert *p)
{
    Q_UNUSED(p);

    // if torrent has no metadata libtorrent doesn't generate "fastresume" data
    // so we should save dummy "fastresume" data containing the values used to
    // load torrent and qBittorrent own resume data
    handleSaveResumeDataAlert(nullptr);
}

void TorrentHandleImpl::handleFastResumeRejectedAlert(const lt::fastresume_rejected_alert *p)
{
    m_fastresumeDataRejected = true;

    if (p->error.value() == lt::errors::mismatching_file_size) {
        // Mismatching file size (files were probably moved)
        m_hasMissingFiles = true;
        LogMsg(tr("File sizes mismatch for torrent '%1', pausing it.").arg(name()), Log::CRITICAL);
    }
    else {
        LogMsg(tr("Fast resume data was rejected for torrent '%1'. Reason: %2. Checking again...")
            .arg(name(), QString::fromStdString(p->message())), Log::WARNING);
    }
}

void TorrentHandleImpl::handleFileRenamedAlert(const lt::file_renamed_alert *p)
{
    // We don't really need to call updateStatus() in this place.
    // All we need to do is make sure we have a valid instance of the TorrentInfo object.
    m_torrentInfo = TorrentInfo {m_nativeHandle.torrent_file()};

    // remove empty leftover folders
    // for example renaming "a/b/c" to "d/b/c", then folders "a/b" and "a" will
    // be removed if they are empty
    const QString oldFilePath = m_oldPath[p->index].takeFirst();
    const QString newFilePath = Utils::Fs::toUniformPath(p->new_name());

    if (m_oldPath[p->index].isEmpty())
        m_oldPath.remove(p->index);

    QVector<QStringRef> oldPathParts = oldFilePath.splitRef('/', QString::SkipEmptyParts);
    oldPathParts.removeLast();  // drop file name part
    QVector<QStringRef> newPathParts = newFilePath.splitRef('/', QString::SkipEmptyParts);
    newPathParts.removeLast();  // drop file name part

#if defined(Q_OS_WIN)
    const Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive;
#endif

    int pathIdx = 0;
    while ((pathIdx < oldPathParts.size()) && (pathIdx < newPathParts.size())) {
        if (oldPathParts[pathIdx].compare(newPathParts[pathIdx], caseSensitivity) != 0)
            break;
        ++pathIdx;
    }

    for (int i = (oldPathParts.size() - 1); i >= pathIdx; --i) {
        QDir().rmdir(savePath() + Utils::String::join(oldPathParts, QLatin1String("/")));
        oldPathParts.removeLast();
    }

    --m_renameCount;
    while (!isMoveInProgress() && (m_renameCount == 0) && !m_moveFinishedTriggers.isEmpty())
        m_moveFinishedTriggers.takeFirst()();

    if (isPaused() && (m_renameCount == 0))
        saveResumeData();  // otherwise the new path will not be saved
}

void TorrentHandleImpl::handleFileRenameFailedAlert(const lt::file_rename_failed_alert *p)
{
    LogMsg(tr("File rename failed. Torrent: \"%1\", file: \"%2\", reason: \"%3\"")
        .arg(name(), filePath(static_cast<LTUnderlyingType<lt::file_index_t>>(p->index))
             , QString::fromLocal8Bit(p->error.message().c_str())), Log::WARNING);

    m_oldPath[p->index].removeFirst();
    if (m_oldPath[p->index].isEmpty())
        m_oldPath.remove(p->index);

    --m_renameCount;
    while (!isMoveInProgress() && (m_renameCount == 0) && !m_moveFinishedTriggers.isEmpty())
        m_moveFinishedTriggers.takeFirst()();

    if (isPaused() && (m_renameCount == 0))
        saveResumeData();  // otherwise the new path will not be saved
}

void TorrentHandleImpl::handleFileCompletedAlert(const lt::file_completed_alert *p)
{
    // We don't really need to call updateStatus() in this place.
    // All we need to do is make sure we have a valid instance of the TorrentInfo object.
    m_torrentInfo = TorrentInfo {m_nativeHandle.torrent_file()};

    qDebug("A file completed download in torrent \"%s\"", qUtf8Printable(name()));
    if (m_session->isAppendExtensionEnabled()) {
        QString name = filePath(static_cast<LTUnderlyingType<lt::file_index_t>>(p->index));
        if (name.endsWith(QB_EXT)) {
            const QString oldName = name;
            name.chop(QB_EXT.size());
            qDebug("Renaming %s to %s", qUtf8Printable(oldName), qUtf8Printable(name));
            renameFile(static_cast<LTUnderlyingType<lt::file_index_t>>(p->index), name);
        }
    }
}

void TorrentHandleImpl::handleMetadataReceivedAlert(const lt::metadata_received_alert *p)
{
    Q_UNUSED(p);
    qDebug("Metadata received for torrent %s.", qUtf8Printable(name()));
    updateStatus();
    if (m_session->isAppendExtensionEnabled())
        manageIncompleteFiles();
    if (!m_hasRootFolder)
        m_torrentInfo.stripRootFolder();
    if (filesCount() == 1)
        m_hasRootFolder = false;
    m_session->handleTorrentMetadataReceived(this);

    if (isPaused()) {
        // XXX: Unfortunately libtorrent-rasterbar does not send a torrent_paused_alert
        // and the torrent can be paused when metadata is received
        m_speedMonitor.reset();
        m_session->handleTorrentPaused(this);
    }

    // If first/last piece priority was specified when adding this torrent, we can set it
    // now that we have metadata:
    if (m_needsToSetFirstLastPiecePriority) {
        setFirstLastPiecePriority(true);
        m_needsToSetFirstLastPiecePriority = false;
    }
}

void TorrentHandleImpl::handlePerformanceAlert(const lt::performance_alert *p) const
{
    LogMsg((tr("Performance alert: ") + QString::fromStdString(p->message()))
           , Log::INFO);
}

void TorrentHandleImpl::handleTempPathChanged()
{
    adjustActualSavePath();
}

void TorrentHandleImpl::handleCategorySavePathChanged()
{
    if (m_useAutoTMM)
        move_impl(m_session->categorySavePath(m_category), MoveStorageMode::Overwrite);
}

void TorrentHandleImpl::handleAppendExtensionToggled()
{
    if (!hasMetadata()) return;

    manageIncompleteFiles();
}

void TorrentHandleImpl::handleAlert(const lt::alert *a)
{
    switch (a->type()) {
    case lt::file_renamed_alert::alert_type:
        handleFileRenamedAlert(static_cast<const lt::file_renamed_alert*>(a));
        break;
    case lt::file_rename_failed_alert::alert_type:
        handleFileRenameFailedAlert(static_cast<const lt::file_rename_failed_alert*>(a));
        break;
    case lt::file_completed_alert::alert_type:
        handleFileCompletedAlert(static_cast<const lt::file_completed_alert*>(a));
        break;
    case lt::torrent_finished_alert::alert_type:
        handleTorrentFinishedAlert(static_cast<const lt::torrent_finished_alert*>(a));
        break;
    case lt::save_resume_data_alert::alert_type:
        handleSaveResumeDataAlert(static_cast<const lt::save_resume_data_alert*>(a));
        break;
    case lt::save_resume_data_failed_alert::alert_type:
        handleSaveResumeDataFailedAlert(static_cast<const lt::save_resume_data_failed_alert*>(a));
        break;
    case lt::torrent_paused_alert::alert_type:
        handleTorrentPausedAlert(static_cast<const lt::torrent_paused_alert*>(a));
        break;
    case lt::torrent_resumed_alert::alert_type:
        handleTorrentResumedAlert(static_cast<const lt::torrent_resumed_alert*>(a));
        break;
    case lt::tracker_error_alert::alert_type:
        handleTrackerErrorAlert(static_cast<const lt::tracker_error_alert*>(a));
        break;
    case lt::tracker_reply_alert::alert_type:
        handleTrackerReplyAlert(static_cast<const lt::tracker_reply_alert*>(a));
        break;
    case lt::tracker_warning_alert::alert_type:
        handleTrackerWarningAlert(static_cast<const lt::tracker_warning_alert*>(a));
        break;
    case lt::metadata_received_alert::alert_type:
        handleMetadataReceivedAlert(static_cast<const lt::metadata_received_alert*>(a));
        break;
    case lt::fastresume_rejected_alert::alert_type:
        handleFastResumeRejectedAlert(static_cast<const lt::fastresume_rejected_alert*>(a));
        break;
    case lt::torrent_checked_alert::alert_type:
        handleTorrentCheckedAlert(static_cast<const lt::torrent_checked_alert*>(a));
        break;
    case lt::performance_alert::alert_type:
        handlePerformanceAlert(static_cast<const lt::performance_alert*>(a));
        break;
    }
}

void TorrentHandleImpl::manageIncompleteFiles()
{
    const bool isAppendExtensionEnabled = m_session->isAppendExtensionEnabled();
    const QVector<qreal> fp = filesProgress();
    if (fp.size() != filesCount()) {
        qDebug() << "skip manageIncompleteFiles because of invalid torrent meta-data or empty file-progress";
        return;
    }

    for (int i = 0; i < filesCount(); ++i) {
        QString name = filePath(i);
        if (isAppendExtensionEnabled && (fileSize(i) > 0) && (fp[i] < 1)) {
            if (!name.endsWith(QB_EXT)) {
                const QString newName = name + QB_EXT;
                qDebug() << "Renaming" << name << "to" << newName;
                renameFile(i, newName);
            }
        }
        else {
            if (name.endsWith(QB_EXT)) {
                const QString oldName = name;
                name.chop(QB_EXT.size());
                qDebug() << "Renaming" << oldName << "to" << name;
                renameFile(i, name);
            }
        }
    }
}

void TorrentHandleImpl::adjustActualSavePath()
{
    if (!isMoveInProgress())
        adjustActualSavePath_impl();
    else
        m_moveFinishedTriggers.append([this]() { adjustActualSavePath_impl(); });
}

void TorrentHandleImpl::adjustActualSavePath_impl()
{
    const bool needUseTempDir = useTempPath();
    const QDir tempDir {m_session->torrentTempPath(info())};
    const QDir currentDir {actualStorageLocation()};
    const QDir targetDir {needUseTempDir ? tempDir : QDir {savePath()}};

    if (targetDir == currentDir) return;

    if (!needUseTempDir) {
        if ((currentDir == tempDir) && (currentDir != QDir {m_session->tempPath()})) {
            // torrent without root folder still has it in its temporary save path
            // so its temp path isn't equal to temp path root
            const QString currentDirPath = currentDir.absolutePath();
            m_moveFinishedTriggers.append([currentDirPath]
            {
                qDebug() << "Removing torrent temp folder:" << currentDirPath;
                Utils::Fs::smartRemoveEmptyFolderTree(currentDirPath);
            });
        }
    }

    moveStorage(Utils::Fs::toNativePath(targetDir.absolutePath()), MoveStorageMode::Overwrite);
}

lt::torrent_handle TorrentHandleImpl::nativeHandle() const
{
    return m_nativeHandle;
}

void TorrentHandleImpl::updateTorrentInfo()
{
    if (!hasMetadata()) return;

    m_torrentInfo = TorrentInfo(m_nativeStatus.torrent_file.lock());
}

bool TorrentHandleImpl::isMoveInProgress() const
{
    return m_storageIsMoving;
}

bool TorrentHandleImpl::useTempPath() const
{
    return m_session->isTempPathEnabled() && !(isSeed() || m_hasSeedStatus);
}

void TorrentHandleImpl::updateStatus()
{
    updateStatus(m_nativeHandle.status());
}

void TorrentHandleImpl::updateStatus(const lt::torrent_status &nativeStatus)
{
    m_nativeStatus = nativeStatus;

    updateState();
    updateTorrentInfo();

    // NOTE: Don't change the order of these conditionals!
    // Otherwise it will not work properly since torrent can be CheckingDownloading.
    if (isChecking())
        m_unchecked = false;
    else if (isDownloading())
        m_unchecked = true;

    m_speedMonitor.addSample({nativeStatus.download_payload_rate
        , nativeStatus.upload_payload_rate});
}

void TorrentHandleImpl::setRatioLimit(qreal limit)
{
    if (limit < USE_GLOBAL_RATIO)
        limit = NO_RATIO_LIMIT;
    else if (limit > MAX_RATIO)
        limit = MAX_RATIO;

    if (m_ratioLimit != limit) {
        m_ratioLimit = limit;
        m_session->handleTorrentShareLimitChanged(this);
    }
}

void TorrentHandleImpl::setSeedingTimeLimit(int limit)
{
    if (limit < USE_GLOBAL_SEEDING_TIME)
        limit = NO_SEEDING_TIME_LIMIT;
    else if (limit > MAX_SEEDING_TIME)
        limit = MAX_SEEDING_TIME;

    if (m_seedingTimeLimit != limit) {
        m_seedingTimeLimit = limit;
        m_session->handleTorrentShareLimitChanged(this);
    }
}

void TorrentHandleImpl::setUploadLimit(const int limit)
{
    m_nativeHandle.set_upload_limit(limit);
}

void TorrentHandleImpl::setDownloadLimit(const int limit)
{
    m_nativeHandle.set_download_limit(limit);
}

void TorrentHandleImpl::setSuperSeeding(const bool enable)
{
    if (enable)
        m_nativeHandle.set_flags(lt::torrent_flags::super_seeding);
    else
        m_nativeHandle.unset_flags(lt::torrent_flags::super_seeding);
}

void TorrentHandleImpl::flushCache() const
{
    m_nativeHandle.flush_cache();
}

QString TorrentHandleImpl::createMagnetURI() const
{
    return QString::fromStdString(lt::make_magnet_uri(m_nativeHandle));
}

void TorrentHandleImpl::prioritizeFiles(const QVector<DownloadPriority> &priorities)
{
    if (!hasMetadata()) return;
    if (priorities.size() != filesCount()) return;

    // Save first/last piece first option state
    const bool firstLastPieceFirst = hasFirstLastPiecePriority();

    // Reset 'm_hasSeedStatus' if needed in order to react again to
    // 'torrent_finished_alert' and eg show tray notifications
    const QVector<qreal> progress = filesProgress();
    const QVector<DownloadPriority> oldPriorities = filePriorities();
    for (int i = 0; i < oldPriorities.size(); ++i) {
        if ((oldPriorities[i] == DownloadPriority::Ignored)
            && (priorities[i] > DownloadPriority::Ignored)
            && (progress[i] < 1.0)) {
            m_hasSeedStatus = false;
            break;
        }
    }

    qDebug() << Q_FUNC_INFO << "Changing files priorities...";
    m_nativeHandle.prioritize_files(toLTDownloadPriorities(priorities));

    // Restore first/last piece first option if necessary
    if (firstLastPieceFirst)
        setFirstLastPiecePriorityImpl(true, priorities);
}

QVector<qreal> TorrentHandleImpl::availableFileFractions() const
{
    const int filesCount = this->filesCount();
    if (filesCount <= 0) return {};

    const QVector<int> piecesAvailability = pieceAvailability();
    // libtorrent returns empty array for seeding only torrents
    if (piecesAvailability.empty()) return QVector<qreal>(filesCount, -1);

    QVector<qreal> res;
    res.reserve(filesCount);
    const TorrentInfo info = this->info();
    for (int i = 0; i < filesCount; ++i) {
        const TorrentInfo::PieceRange filePieces = info.filePieces(i);

        int availablePieces = 0;
        for (const int piece : filePieces)
            availablePieces += (piecesAvailability[piece] > 0) ? 1 : 0;

        const qreal availability = filePieces.isEmpty()
            ? 1  // the file has no pieces, so it is available by default
            : static_cast<qreal>(availablePieces) / filePieces.size();
        res.push_back(availability);
    }
    return res;
}
