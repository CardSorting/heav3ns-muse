/*
 * MidiAlsaRaw.cpp - MIDI client for RawMIDI via ALSA
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "MidiAlsaRaw.h"
#ifdef LMMS_HAVE_ALSA
#include "ConfigManager.h"
#include "MidiIoWakeup.h"
#include "RawMidiApi.h"
#include "RawMidiOutput.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace lmms
{
namespace { const detail::AlsaRawMidiApi nativeApi; }

MidiAlsaRaw::MidiAlsaRaw() : MidiAlsaRaw(nativeApi) { }

MidiAlsaRaw::MidiAlsaRaw(const detail::AlsaRawMidiApi& api)
	: m_api(api), m_wakeup(std::make_unique<detail::MidiIoWakeup>()), m_outputQueue(std::make_unique<detail::RawMidiOutput>())
{
	bool prepared = false;
	const auto cleanup = qScopeGuard([&] { if (!prepared) { closeResources(); } });
	try
	{
		const auto opened = m_api.open(&m_input, &m_output, probeDevice().toLatin1().constData(), SND_RAWMIDI_NONBLOCK);
		if (opened < 0) { inputFailed(opened); return; }
		if (!m_input || !m_output) { inputFailed(-ENODEV); return; }
		for (const auto handle : {m_input, m_output})
		{
			const auto count = m_api.descriptorCount(handle);
			if (count <= 0 || count > MaximumDescriptors) { inputFailed(count < 0 ? count : -EINVAL); return; }
			const auto filled = m_api.descriptors(handle, m_descriptors.data(), count);
			if (filled != count) { inputFailed(filled < 0 ? filled : -EINVAL); return; }
		}
		if (const auto error = inspectInputLoss(); error < 0) { inputFailed(error); return; }
		if (!m_wakeup->open()) { inputFailed(-errno); return; }
		m_active = true; start(QThread::LowPriority); prepared = true;
	}
	catch (...) { inputFailed(-EIO); }
}

MidiAlsaRaw::~MidiAlsaRaw()
{
	try { m_outputQueue->cancelAll(); m_wakeup->signal(); drainOutput({}); }
	catch (...) { qWarning("Native raw MIDI output cleanup was incomplete during shutdown"); }
	m_quit = true; m_active = false; m_wakeup->signal();
	wait(); // Native reads are nonblocking; do not close a handle under its reader.
	closeResources();
}

void MidiAlsaRaw::closeResources() noexcept
{
	const auto lock = std::lock_guard{m_ioMutex};
	if (const auto input = std::exchange(m_input, nullptr)) { m_api.close(input); }
	if (const auto output = std::exchange(m_output, nullptr)) { m_api.close(output); }
	m_wakeup->close();
}

bool MidiAlsaRaw::isAvailable() const
{ return m_active.load(std::memory_order_acquire) && !m_quit.load(std::memory_order_acquire) && isRunning(); }

MidiClient::InputBackendStatus MidiAlsaRaw::inputBackendStatus() const
{
	return {true, isAvailable(), m_nativeError.load(std::memory_order_acquire),
		m_overrunObserved.load(std::memory_order_acquire), m_reportedOverrunBytes.load(std::memory_order_acquire)};
}

void MidiAlsaRaw::inputFailed(int error) noexcept
{
	m_nativeError.store(error ? error : -EIO, std::memory_order_release);
	try { m_outputQueue->fail(error); } catch (...) { }
	m_active = false; m_quit = true; m_wakeup->signal();
	failInputEvents(InputDeliveryFailure::BackendInput);
}

QString MidiAlsaRaw::probeDevice()
{
	const auto device = ConfigManager::inst()->value("MidiAlsaRaw", "device");
	if (!device.isEmpty()) { return device; }
	const auto environment = getenv("MIDIDEV");
	return environment ? QString::fromLocal8Bit(environment) : QString("default");
}

void MidiAlsaRaw::sendByte(unsigned char)
{ throw std::logic_error("Native raw MIDI output requires a complete packet"); }

MidiClient::OutputBackendStatus MidiAlsaRaw::outputBackendStatus() const
{
	auto status = m_outputQueue->status(isAvailable()); status.abandonmentSupported = true;
	status.abandonmentEligible = m_quit.load(std::memory_order_acquire) && !isRunning() && m_outputQueue->terminal();
	return status;
}

void MidiAlsaRaw::addPort(MidiPort* port)
{
	m_outputQueue->add(port);
	try { MidiClient::addPort(port); } catch (...) { m_outputQueue->remove(port); throw; }
}

void MidiAlsaRaw::removePort(MidiPort* port)
{ m_outputQueue->remove(port); m_wakeup->signal(); MidiClient::removePort(port); }

void MidiAlsaRaw::cancelOutputEvents(const MidiPort* port)
{ m_outputQueue->cancel(port); m_wakeup->signal(); }

void MidiAlsaRaw::processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort* port)
{
	if (!isAvailable()) { throw std::runtime_error("Native raw MIDI output is unavailable"); }
	try { m_outputQueue->enqueue(event, port); }
	catch (...) { m_wakeup->signal(); throw; }
	m_wakeup->signal();
}

void MidiAlsaRaw::finishPortTransferOutput(const std::function<bool()>& cancelled)
{ drainOutput(cancelled); }

void MidiAlsaRaw::drainOutput(const std::function<bool()>& cancelled)
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

int MidiAlsaRaw::writeOutput(const std::uint8_t* bytes, std::size_t size)
{
	const auto lock = std::lock_guard{m_ioMutex};
	if (!m_output || m_quit) { return -ENODEV; }
	const auto written = m_api.write(m_output, bytes, size);
	return written > static_cast<ssize_t>(size) ? -EIO : static_cast<int>(written);
}

int MidiAlsaRaw::inspectInputLoss()
{
	// ALSA's native status sample consumes its overrun count. Do not subtract a
	// previous sample or ask the model owner to query this stream independently.
	snd_rawmidi_status_t* status; snd_rawmidi_status_alloca(&status);
	const auto lock = std::lock_guard{m_ioMutex};
	bool observed = false;
	const auto invalidate = qScopeGuard([&] { if (!observed) { m_overrunObserved = false; } });
	int error = -EINTR;
	for (unsigned int attempt = 0; attempt < 4 && error == -EINTR; ++attempt) { error = m_api.status(m_input, status); }
	if (error != 0) { return error < 0 ? error : -EIO; }
	const auto lost = m_api.statusXruns(status);
	m_reportedOverrunBytes.store(static_cast<std::uint64_t>(lost), std::memory_order_release);
	m_overrunObserved.store(true, std::memory_order_release);
	observed = true;
	return lost ? -EOVERFLOW : 0;
}

void MidiAlsaRaw::run()
{
	const auto stopped = qScopeGuard([&] { m_active = false; });
	try
	{
		std::array<unsigned char, 256> bytes{};
		while (!m_quit.load(std::memory_order_acquire))
		{
			int inputCount = 0, outputCount = 0, preparationError = 0;
			const bool outputPending = m_outputQueue->wantsWrite();
			{
				const auto lock = std::lock_guard{m_ioMutex};
				const auto prepare = [&](snd_rawmidi_t* handle, int offset, int& count) {
					count = m_api.descriptorCount(handle);
					if (count <= 0 || count > MaximumDescriptors) { preparationError = count < 0 ? count : -EINVAL; return; }
					const auto filled = m_api.descriptors(handle, m_descriptors.data() + offset, count);
					if (filled != count) { preparationError = filled < 0 ? filled : -EINVAL; }
				};
				prepare(m_input, 0, inputCount);
				if (!preparationError && outputPending) { prepare(m_output, inputCount, outputCount); }
			}
			if (preparationError) { inputFailed(preparationError); return; }
			const auto count = inputCount + outputCount;
			m_descriptors[count] = {m_wakeup->descriptor(), POLLIN, 0};
			const auto ready = m_api.poll(m_descriptors.data(), count + 1, 1000);
			if (m_quit.load(std::memory_order_acquire)) { return; }
			if (ready < 0) { if (errno == EINTR) { continue; } inputFailed(-errno); return; }
			if (!ready)
			{
				if (const auto error = inspectInputLoss(); error < 0) { inputFailed(error); return; }
				continue;
			}
			if (m_descriptors[count].revents & (POLLERR | POLLHUP | POLLNVAL)) { inputFailed(-EIO); return; }
			if (m_descriptors[count].revents & POLLIN) { m_wakeup->drain(); }
			unsigned short events = 0, outputEvents = 0; int eventError;
			{
				const auto lock = std::lock_guard{m_ioMutex};
				eventError = m_api.events(m_input, m_descriptors.data(), inputCount, &events);
				if (eventError >= 0 && outputCount) { eventError = m_api.events(m_output, m_descriptors.data() + inputCount, outputCount, &outputEvents); }
			}
			if (eventError < 0) { inputFailed(eventError); return; }
			if ((events | outputEvents) & (POLLERR | POLLHUP | POLLNVAL)) { inputFailed(-EIO); return; }
			if (events & POLLIN)
			{ if (const auto error = inspectInputLoss(); error < 0) { inputFailed(error); return; } }
			if (outputEvents & POLLOUT)
			{
				const auto error = m_outputQueue->service([this](const auto* data, auto size) { return writeOutput(data, size); });
				if (error < 0) { inputFailed(error); return; }
				if (error > 0) { QThread::msleep(1); } // A stale writable indication must not spin.
			}
			if (!(events & POLLIN)) { continue; }
			// One bounded chunk per poll keeps continuous input interruptible.
			ssize_t received;
			{ const auto lock = std::lock_guard{m_ioMutex}; received = m_api.read(m_input, bytes.data(), bytes.size()); }
			if (received == -EAGAIN || received == -EINTR || received == 0) { continue; }
			if (received < 0 || received > static_cast<ssize_t>(bytes.size())) { inputFailed(received < 0 ? static_cast<int>(received) : -EOVERFLOW); return; }
			// A native ring overflow can occur during read without failing read.
			// Never decode that uncertain chunk or continue its running status.
			if (const auto error = inspectInputLoss(); error < 0) { inputFailed(error); return; }
			for (ssize_t index = 0; index < received && !m_quit.load(std::memory_order_acquire); ++index) { parseData(bytes[index]); }
		}
	}
	catch (...) { inputFailed(-EIO); }
}
} // namespace lmms
#endif // LMMS_HAVE_ALSA
