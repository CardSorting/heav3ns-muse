/*
 * MidiSndio.cpp - base-class that implements sndio midi support
 *
 * Copyright (c) 2010-2016 jackmsr@openbsd.net
 * Copyright (c) 2016-2017 David Carlier <devnexen@gmail.com>
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

#include "MidiSndio.h"
#ifdef LMMS_HAVE_SNDIO
#include "ConfigManager.h"
#include "MidiIoWakeup.h"
#include "RawMidiApi.h"
#include "RawMidiOutput.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <cerrno>
#include <stdexcept>
#include <utility>

namespace lmms
{
namespace { const detail::SndioMidiApi nativeApi; }

MidiSndio::MidiSndio() : MidiSndio(nativeApi) { }

MidiSndio::MidiSndio(const detail::SndioMidiApi& api)
	: m_api(api), m_wakeup(std::make_unique<detail::MidiIoWakeup>()), m_outputQueue(std::make_unique<detail::RawMidiOutput>())
{
	bool prepared = false;
	const auto cleanup = qScopeGuard([&] { if (!prepared) { closeResources(); } });
	try
	{
		const auto device = probeDevice().toLatin1();
		m_hdl = m_api.open(device.isEmpty() ? MIO_PORTANY : device.constData(), MIO_IN | MIO_OUT, 1);
		if (!m_hdl) { inputFailed(-ENODEV); return; }
		const auto count = m_api.descriptorCount(m_hdl);
		if (count <= 0 || count > MaximumDescriptors) { inputFailed(-EINVAL); return; }
		const auto filled = m_api.descriptors(m_hdl, m_descriptors.data(), POLLIN | POLLOUT);
		if (filled <= 0 || filled > count || m_api.eof(m_hdl)) { inputFailed(-EIO); return; }
		if (!m_wakeup->open()) { inputFailed(-errno); return; }
		m_active = true; start(QThread::LowPriority); prepared = true;
	}
	catch (...) { inputFailed(-EIO); }
}

MidiSndio::~MidiSndio()
{
	try { m_outputQueue->cancelAll(); m_wakeup->signal(); drainOutput({}); }
	catch (...) { qWarning("Native raw MIDI output cleanup was incomplete during shutdown"); }
	m_quit = true; m_active = false; m_wakeup->signal();
	wait(); // Join without a native-handle lock; input is nonblocking.
	closeResources();
}

void MidiSndio::closeResources() noexcept
{
	const auto lock = std::lock_guard{m_ioMutex};
	if (const auto handle = std::exchange(m_hdl, nullptr)) { m_api.close(handle); }
	m_wakeup->close();
}

bool MidiSndio::isAvailable() const
{ return m_active.load(std::memory_order_acquire) && !m_quit.load(std::memory_order_acquire) && isRunning(); }

MidiClient::InputBackendStatus MidiSndio::inputBackendStatus() const
{ return {true, isAvailable(), m_nativeError.load(std::memory_order_acquire)}; }

void MidiSndio::inputFailed(int error) noexcept
{
	m_nativeError.store(error ? error : -EIO, std::memory_order_release);
	try { m_outputQueue->fail(error); } catch (...) { }
	m_active = false; m_quit = true; m_wakeup->signal();
	failInputEvents(InputDeliveryFailure::BackendInput);
}

QString MidiSndio::probeDevice()
{ return ConfigManager::inst()->value("MidiSndio", "device"); }

void MidiSndio::sendByte(unsigned char)
{ throw std::logic_error("Native raw MIDI output requires a complete packet"); }

MidiClient::OutputBackendStatus MidiSndio::outputBackendStatus() const
{
	auto status = m_outputQueue->status(isAvailable()); status.abandonmentSupported = true;
	status.abandonmentEligible = m_quit.load(std::memory_order_acquire) && !isRunning() && m_outputQueue->terminal();
	return status;
}

void MidiSndio::addPort(MidiPort* port)
{
	m_outputQueue->add(port);
	try { MidiClient::addPort(port); } catch (...) { m_outputQueue->remove(port); throw; }
}

void MidiSndio::removePort(MidiPort* port)
{ m_outputQueue->remove(port); m_wakeup->signal(); MidiClient::removePort(port); }

void MidiSndio::cancelOutputEvents(const MidiPort* port)
{ m_outputQueue->cancel(port); m_wakeup->signal(); }

void MidiSndio::processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort* port)
{
	if (!isAvailable()) { throw std::runtime_error("Native raw MIDI output is unavailable"); }
	try { m_outputQueue->enqueue(event, port); }
	catch (...) { m_wakeup->signal(); throw; }
	m_wakeup->signal();
}

void MidiSndio::finishPortTransferOutput(const std::function<bool()>& cancelled)
{ drainOutput(cancelled); }

void MidiSndio::drainOutput(const std::function<bool()>& cancelled)
{
	QElapsedTimer deadline; deadline.start(); m_wakeup->signal();
	while (true)
	{
		if (cancelled && cancelled()) { throw std::runtime_error("Native raw MIDI cleanup was cancelled"); }
		if (m_outputQueue->drained()) { return; }
		if (!isAvailable()) { throw std::runtime_error("Native raw MIDI output failed before cleanup was submitted"); }
		if (deadline.elapsed() >= 250) { throw std::runtime_error("Native raw MIDI output cleanup timed out"); }
		QThread::msleep(1);
	}
}

int MidiSndio::writeOutput(const std::uint8_t* bytes, std::size_t size)
{
	const auto lock = std::lock_guard{m_ioMutex};
	if (!m_hdl || m_quit) { return -ENODEV; }
	const auto written = m_api.write(m_hdl, bytes, size);
	if (m_api.eof(m_hdl) || written > size) { return -EIO; }
	return static_cast<int>(written);
}

void MidiSndio::run()
{
	const auto stopped = qScopeGuard([&] { m_active = false; });
	try
	{
		std::array<unsigned char, 256> bytes{};
		while (!m_quit.load(std::memory_order_acquire))
		{
			int count = 0; bool failed = false;
			const int wanted = POLLIN | (m_outputQueue->wantsWrite() ? POLLOUT : 0);
			{
				const auto lock = std::lock_guard{m_ioMutex}; const auto capacity = m_api.descriptorCount(m_hdl);
				if (capacity <= 0 || capacity > MaximumDescriptors) { failed = true; }
				else { count = m_api.descriptors(m_hdl, m_descriptors.data(), wanted); failed = count <= 0 || count > capacity || m_api.eof(m_hdl); }
			}
			if (failed) { inputFailed(-EIO); return; }
			m_descriptors[count] = {m_wakeup->descriptor(), POLLIN, 0};
			const auto ready = m_api.poll(m_descriptors.data(), count + 1, 1000);
			if (m_quit.load(std::memory_order_acquire)) { return; }
			if (ready < 0) { if (errno == EINTR) { continue; } inputFailed(-errno); return; }
			if (!ready) { continue; }
			if (m_descriptors[count].revents & (POLLERR | POLLHUP | POLLNVAL)) { inputFailed(-EIO); return; }
			if (m_descriptors[count].revents & POLLIN) { m_wakeup->drain(); }
			int events; std::size_t received = 0;
			{
				const auto lock = std::lock_guard{m_ioMutex}; events = m_api.events(m_hdl, m_descriptors.data());
				failed = m_api.eof(m_hdl) || (events & (POLLERR | POLLHUP | POLLNVAL));
				if (!failed && (events & POLLIN)) { received = m_api.read(m_hdl, bytes.data(), bytes.size()); failed = m_api.eof(m_hdl) || received > bytes.size(); }
			}
			if (failed) { inputFailed(-EIO); return; }
			if (events & POLLOUT)
			{
				const auto error = m_outputQueue->service([this](const auto* data, auto size) { return writeOutput(data, size); });
				if (error < 0) { inputFailed(error); return; }
				if (error > 0) { QThread::msleep(1); }
			}
			// Zero with no EOF is ordinary nonblocking backpressure, not a
			// disconnected reader. Never parse a short/failed native buffer.
			for (std::size_t index = 0; index < received && !m_quit.load(std::memory_order_acquire); ++index) { parseData(bytes[index]); }
		}
	}
	catch (...) { inputFailed(-EIO); }
}
} // namespace lmms
#endif // LMMS_HAVE_SNDIO
