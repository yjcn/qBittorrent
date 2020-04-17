/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2020  Prince Gupta <jagannatharjun11@gmail.com>
 * Copyright (C) 2019  sledgehammer999 <hammered999@gmail.com>
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

#include "logmodel.h"

#include <QApplication>
#include <QDateTime>
#include <QColor>
#include <QPalette>

#include "base/global.h"

namespace
{
    const int MAX_VISIBLE_MESSAGES = 20000;
}

BaseLogModel::Message::Message(const QString &time, const QString &message, const QColor &foreground, const Log::MsgType type)
    : m_time(time)
    , m_message(message)
    , m_foreground(foreground)
    , m_type(type)
{
}

QVariant BaseLogModel::Message::time() const
{
    return m_time;
}

QVariant BaseLogModel::Message::message() const
{
    return m_message;
}

QVariant BaseLogModel::Message::foreground() const
{
    return m_foreground;
}

QVariant BaseLogModel::Message::type() const
{
    return m_type;
}

BaseLogModel::BaseLogModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_messages(MAX_VISIBLE_MESSAGES)
{
}

int BaseLogModel::rowCount(const QModelIndex &) const
{
    return m_messages.size();
}

int BaseLogModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant BaseLogModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid())
        return {};

    const int messageIndex = index.row();
    if ((messageIndex < 0) || (messageIndex >= static_cast<int>(m_messages.size())))
        return {};

    const Message &message = m_messages[messageIndex];
    switch (role) {
    case TimeRole:
        return message.time();
    case MessageRole:
        return message.message();
    case ForegroundRole:
        return message.foreground();
    case TypeRole:
        return message.type();
    default:
        return {};
    }
}

void BaseLogModel::addNewMessage(const BaseLogModel::Message &message)
{
    // if row is inserted on filled up buffer, the size will not change
    // but because of calling of beginInsertRows function we'll have ghost rows.
    if (m_messages.size() == MAX_VISIBLE_MESSAGES) {
        const int lastMessage = m_messages.size() - 1;
        beginRemoveRows(QModelIndex(), lastMessage, lastMessage);
        m_messages.pop_back();
        endRemoveRows();
    }

    beginInsertRows(QModelIndex(), 0, 0);
    m_messages.push_front(message);
    endInsertRows();
}

void BaseLogModel::reset()
{
    beginResetModel();
    m_messages.clear();
    endResetModel();
}

LogMessageModel::LogMessageModel(QObject *parent)
    : BaseLogModel(parent)
{
    for (const Log::Msg &msg : asConst(Logger::instance()->getMessages()))
        handleNewMessage(msg);
    connect(Logger::instance(), &Logger::newLogMessage, this, &LogMessageModel::handleNewMessage);
}

void LogMessageModel::handleNewMessage(const Log::Msg &message)
{
    const QString time = QDateTime::fromMSecsSinceEpoch(message.timestamp).toString(Qt::SystemLocaleShortDate);
    const QString messageText = message.message;

    QColor foreground;
    switch (message.type) {
    // The RGB QColor constructor is used for performance
    case Log::NORMAL:
        foreground = QApplication::palette().color(QPalette::WindowText);
        break;
    case Log::INFO:
        foreground = QColor(0, 0, 255); // blue
        break;
    case Log::WARNING:
        foreground = QColor(255, 165, 0);  // orange
        break;
    case Log::CRITICAL:
        foreground = QColor(255, 0, 0);  // red
        break;
    default:
        Q_ASSERT(false);
        break;
    }

    addNewMessage({time, messageText, foreground, message.type});
}

LogPeerModel::LogPeerModel(QObject *parent)
    : BaseLogModel(parent)
{
    for (const Log::Peer &peer : asConst(Logger::instance()->getPeers()))
        handleNewMessage(peer);
    connect(Logger::instance(), &Logger::newLogPeer, this, &LogPeerModel::handleNewMessage);
}

void LogPeerModel::handleNewMessage(const Log::Peer &peer)
{
    const QString time = QDateTime::fromMSecsSinceEpoch(peer.timestamp).toString(Qt::SystemLocaleShortDate);
    const QString message = peer.blocked
            ? tr("%1 was blocked due to %2", "0.0.0.0 was blocked due to reason").arg(peer.ip, peer.reason)
            : tr("%1 was banned", "0.0.0.0 was banned").arg(peer.ip);
    const QColor foreground = Qt::red;

    addNewMessage({time, message, foreground, Log::NORMAL});
}
