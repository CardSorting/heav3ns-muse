/*
 * RemotePluginBase.cpp - base class providing RPC like mechanisms
 *
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "RemotePluginBase.h"

#ifndef BUILD_REMOTE_PLUGIN_CLIENT
#include <QCoreApplication>
#include <QThread>
#endif


namespace lmms
{


#ifdef SYNC_WITH_SHM_FIFO
RemotePluginBase::RemotePluginBase(shmFifo * _in, shmFifo * _out) :
	m_in(_in),
	m_out(_out)
#else
RemotePluginBase::RemotePluginBase() :
	m_socket(-1),
	m_invalid(false)
#endif
{
	// make sure, we're using common ways to print/scan
	// floats to/from strings (',' vs. '.' for decimal point etc.)
	setlocale(LC_NUMERIC, "C");
#ifndef SYNC_WITH_SHM_FIFO
	pthread_mutex_init(&m_receiveMutex, nullptr);
	pthread_mutex_init(&m_sendMutex, nullptr);
#endif
}




RemotePluginBase::~RemotePluginBase()
{
#ifdef SYNC_WITH_SHM_FIFO
	delete m_in;
	delete m_out;
#else
	pthread_mutex_destroy(&m_receiveMutex);
	pthread_mutex_destroy(&m_sendMutex);
#endif
}




int RemotePluginBase::sendMessage(const message & _m)
{
	if (isInvalid()) { return 0; }
	std::size_t bytes = 2 * sizeof(int32_t);
	if (_m.data.size() > (maximumMessageBytes - bytes) / sizeof(int32_t)) { invalidate(); return 0; }
	for (const auto& str : _m.data)
	{
		if (bytes > maximumMessageBytes - sizeof(int32_t)
			|| str.size() > maximumMessageBytes - bytes - sizeof(int32_t))
		{
			invalidate();
			return 0;
		}
		bytes += sizeof(int32_t) + str.size();
	}
	// Publish a complete FIFO message under one lock. Waiting for free capacity
	// while holding the outer writer lock prevents the reader from draining it.
	std::vector<char> packet(bytes);
	std::size_t position = 0;
	const auto appendInt = [&](int32_t value)
	{
		std::memcpy(packet.data() + position, &value, sizeof(value));
		position += sizeof(value);
	};
	appendInt(_m.id);
	appendInt(static_cast<int32_t>(_m.data.size()));
	for (const auto& str : _m.data)
	{
		appendInt(static_cast<int32_t>(str.size()));
		std::memcpy(packet.data() + position, str.data(), str.size());
		position += str.size();
	}
#ifdef SYNC_WITH_SHM_FIFO
	m_out->write(packet.data(), static_cast<int>(packet.size()));
	if (!isInvalid()) { m_out->messageSent(); }
#else
	pthread_mutex_lock(&m_sendMutex);
	write(packet.data(), static_cast<int>(packet.size()));
	pthread_mutex_unlock(&m_sendMutex);
#endif

	return isInvalid() ? 0 : static_cast<int>(packet.size());
}




RemotePluginBase::message RemotePluginBase::receiveMessage()
{
#ifdef SYNC_WITH_SHM_FIFO
	if (!m_in->waitForMessage() || !m_in->lock()) { return {}; }
	struct ReceiveUnlock { shmFifo* fifo; ~ReceiveUnlock() { fifo->unlock(); } } unlock{m_in};
	const auto readBytes = [&](void* target, int bytes) { m_in->read(target, bytes); };
#else
	pthread_mutex_lock(&m_receiveMutex);
	struct ReceiveUnlock { pthread_mutex_t* mutex; ~ReceiveUnlock() { pthread_mutex_unlock(mutex); } } unlock{&m_receiveMutex};
	const auto readBytes = [&](void* target, int bytes) { read(target, bytes); };
#endif
	message m;
	const auto nextInt = [&]
	{
		int32_t value = 0;
		readBytes(&value, sizeof(value));
		return value;
	};
	m.id = nextInt();
	const auto fields = nextInt();
	std::size_t remaining = maximumMessageBytes - 2 * sizeof(int32_t);
	if (fields < 0 || static_cast<std::size_t>(fields) > remaining / sizeof(int32_t)) { invalidate(); }
	for (int32_t i = 0; i < fields && !isInvalid(); ++i)
	{
		if (remaining < sizeof(int32_t)) { invalidate(); break; }
		const auto length = nextInt();
		remaining -= sizeof(int32_t);
		if (length < 0 || static_cast<std::size_t>(length) > remaining) { invalidate(); break; }
		std::string field(static_cast<std::size_t>(length), '\0');
		readBytes(field.data(), length);
		remaining -= length;
		m.data.push_back(std::move(field));
	}
	return isInvalid() ? message{} : m;
}




RemotePluginBase::message RemotePluginBase::waitForMessage(
							const message & _wm,
							bool _busy_waiting)
{
#ifndef BUILD_REMOTE_PLUGIN_CLIENT
	if (_busy_waiting)
	{
		// No point processing events outside of the main thread
		_busy_waiting = QCoreApplication::instance() && QThread::currentThread() ==
					QCoreApplication::instance()->thread();
	}

	struct WaitDepthCounter
	{
		WaitDepthCounter(std::atomic_int& depth, bool busy) :
			m_depth(depth),
			m_busy(busy)
		{
			if (m_busy) { ++m_depth; }
		}

		~WaitDepthCounter()
		{
			if (m_busy) { --m_depth; }
		}

		std::atomic_int& m_depth;
		bool m_busy;
	};

	WaitDepthCounter wdc(waitDepthCounter(), _busy_waiting);
#endif
	while (!isInvalid())
	{
#ifndef BUILD_REMOTE_PLUGIN_CLIENT
		if (_busy_waiting && !messagesLeft())
		{
			// FIXME: Can hang sometimes (due to too many messages?)
			QCoreApplication::processEvents(
				QEventLoop::ExcludeUserInputEvents, 50);
			continue;
		}
#endif
		message m = receiveMessage();
		processMessage(m);
		if (m.id == _wm.id)
		{
			return m;
		}
		else if (m.id == IdUndefined)
		{
			return m;
		}
	}

	return message();
}

} // namespace lmms
