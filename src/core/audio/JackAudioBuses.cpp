/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "JackAudioBuses.h"
#ifdef LMMS_HAVE_JACK
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <QScopeGuard>
#include <QThread>
#include "AudioBusOutput.h"
#include "AudioJack.h"
#include "FileResourceIO.h"
#include "JackGraph.h"

namespace lmms::detail
{
static_assert(std::atomic<JackBusSnapshot*>::is_always_lock_free && std::atomic<std::uint32_t>::is_always_lock_free);
namespace
{
void checkCancellation(const std::function<bool()>& cancelled)
{
	if (cancelled && cancelled()) { throw std::runtime_error("JACK track port update was cancelled"); }
}
}

JackAudioBuses::~JackAudioBuses() { publish({}); }

JackAudioBuses::Read::Read(JackAudioBuses& buses) noexcept : m_buses(buses)
{
	do
	{
		m_snapshot = m_buses.m_published.load();
		m_buses.m_reader.store(m_snapshot);
	} while (m_snapshot != m_buses.m_published.load());
}

JackAudioBuses::Read::~Read()
{
	if (m_snapshot && m_confirmed) { m_buses.m_observed.store(m_snapshot->generation, std::memory_order_release); }
	m_buses.m_reader.store(nullptr);
}

JackAudioBuses::Record* JackAudioBuses::find(const std::shared_ptr<const AudioBusOutput>& source)
{
	const auto found = m_records.find(source.get());
	return found == m_records.end() ? nullptr : found->second.get();
}

void JackAudioBuses::add(const std::shared_ptr<const AudioBusOutput>& source, const QString& label)
{
	if (!source || source->retired()) { throw std::invalid_argument("Cannot publish a retired native audio bus"); }
	if (const auto record = find(source))
	{
		record->wanted = true; record->label = label; record->retry.invalidate(); return;
	}
	if (m_nextId == std::numeric_limits<std::uint64_t>::max()) { throw std::overflow_error("JACK track port identities exhausted"); }
	auto record = std::make_unique<Record>(); record->id = m_nextId++; record->source = source; record->label = label;
	m_records.emplace(source.get(), std::move(record));
}

void JackAudioBuses::remove(const std::shared_ptr<const AudioBusOutput>& source)
{
	if (const auto record = find(source)) { record->wanted = false; record->retry.invalidate(); }
}

void JackAudioBuses::rename(const std::shared_ptr<const AudioBusOutput>& source, const QString& label)
{
	// Stable numeric native names preserve graph edges through track renames.
	// The current track label is returned separately by native inspection.
	if (const auto record = find(source)) { record->label = label; }
}

bool JackAudioBuses::pending(const Record& record) const
{
	const bool wanted = record.wanted && !record.source->retired();
	return wanted ? (!record.ports[0] || !record.ports[1] || record.muted)
		: (record.ports[0] || record.ports[1]);
}

std::unique_ptr<JackBusSnapshot> JackAudioBuses::snapshot(Record* adding, Record* omitting)
{
	if (m_generation == std::numeric_limits<std::uint32_t>::max())
	{ throw std::overflow_error("JACK track snapshot generations exhausted; recreate the audio device"); }
	auto next = std::make_unique<JackBusSnapshot>(); next->generation = ++m_generation;
	next->entries.reserve(std::min(m_records.size(), PortLimit));
	next->destinations.reserve(std::min(m_records.size(), PortLimit));
	for (const auto& item : m_records)
	{
		const auto& record = *item.second;
		if (&record == omitting || (&record != adding && (!record.ports[0] || !record.ports[1]))) { continue; }
		if (next->entries.size() >= PortLimit) { throw std::runtime_error("JACK supports at most 256 published native stereo buses in this process"); }
		next->entries.push_back({record.id, record.ports});
		next->destinations.push_back({record.wanted && !record.source->retired() ? record.source : nullptr, {}});
	}
	next->buffers.resize(next->entries.size());
	return next;
}

void JackAudioBuses::publish(std::unique_ptr<JackBusSnapshot> next)
{
	const auto previous = std::move(m_snapshot); m_snapshot = std::move(next);
	m_published.store(m_snapshot.get());
	if (previous)
	{
		while (m_reader.load() == previous.get()) { QThread::msleep(1); }
	}
	// The former snapshot's shared sources and scratch are destroyed here,
	// never by the native callback and never while that callback uses them.
	for (auto& item : m_records)
	{
		auto& record = *item.second;
		record.muted = (record.ports[0] && record.ports[1]) && (!record.wanted || record.source->retired());
	}
}

void JackAudioBuses::restoreSnapshot() { publish(snapshot()); }

bool JackAudioBuses::prepare(JackBusSnapshot* state, jack_nframes_t frames)
{
	if (!state) { return true; }
	bool valid = true;
	for (std::size_t index = 0; index < state->entries.size(); ++index)
	{
		for (unsigned int channel = 0; channel < 2; ++channel)
		{
			auto buffer = static_cast<float*>(m_owner.m_api.portBuffer(state->entries[index].ports[channel], frames));
			state->buffers[index][channel] = buffer;
			if (buffer) { std::fill_n(buffer, frames, 0.f); } else { valid = false; }
		}
		state->destinations[index].output = {state->buffers[index].data(), 2, frames};
	}
	return valid;
}

bool JackAudioBuses::silence(JackBusSnapshot* state, jack_nframes_t frames) noexcept
{
	if (!state) { return true; }
	bool valid = true;
	for (const auto& entry : state->entries)
	{
		for (const auto port : entry.ports)
		{
			try
			{
				if (const auto buffer = static_cast<float*>(m_owner.m_api.portBuffer(port, frames))) { std::fill_n(buffer, frames, 0.f); }
				else { valid = false; }
			}
			catch (...) { valid = false; }
		}
	}
	return valid;
}

void JackAudioBuses::retireUncertainClient()
{
	// An unregister error leaves native ownership uncertain. Retain ownership
	// until the complete client is closed; automatic recovery must not replay it.
	m_owner.m_processingRequested = false;
	m_owner.m_serverDisconnected.store(true, std::memory_order_release);
	m_owner.reportProcessingFailure(AudioDevice::ProcessingFailure::Disconnected, -EIO);
	m_owner.closeJackClient();
}

void JackAudioBuses::registerPorts(Record& record, const std::function<bool()>& cancelled)
{
	auto next = snapshot(&record); // All allocations precede native registration.
	const auto slot = std::find_if(next->entries.begin(), next->entries.end(), [&](const auto& entry) { return entry.id == record.id; });
	std::array<jack_port_t*, 2> ports{}; bool committed = false;
	const auto cleanup = qScopeGuard([&]
	{
		if (committed) { return; }
		bool uncertain = false;
		for (const auto port : ports)
		{
			if (!port || !m_owner.m_client || m_owner.serverDisconnected()) { continue; }
			try { if (m_owner.m_api.unregisterPort(m_owner.m_client, port)) { uncertain = true; } }
			catch (...) { uncertain = true; }
		}
		if (uncertain) { retireUncertainClient(); }
	});
	for (unsigned int channel = 0; channel < 2; ++channel)
	{
		checkCancellation(cancelled);
		const auto name = (QString("track %1 ").arg(static_cast<qulonglong>(record.id)) + (channel ? "R" : "L")).toUtf8();
		ports[channel] = m_owner.m_api.registerPort(m_owner.m_client, name.constData(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
		if (!ports[channel]) { record.nativeError = -ENOSPC; throw std::runtime_error("JACK could not register both track audio ports"); }
		if (m_owner.serverDisconnected()) { throw std::runtime_error("JACK disconnected while registering track audio ports"); }
	}
	checkCancellation(cancelled);
	slot->ports = ports; record.ports = ports;
	publish(std::move(next)); committed = true;
}

void JackAudioBuses::unregisterPorts(Record& record, const std::function<bool()>& cancelled)
{
	// Keep the ports in a silent snapshot until a native callback has cleared
	// them. A stalled server leaves that snapshot owned and retirement pending.
	publish(snapshot()); const auto generation = m_snapshot->generation;
	if (m_owner.m_active)
	{
		QElapsedTimer wait; wait.start();
		while (m_observed.load(std::memory_order_acquire) != generation)
		{
			checkCancellation(cancelled);
			if (m_owner.serverDisconnected()) { throw std::runtime_error("JACK disconnected before silent track port retirement"); }
			if (wait.elapsed() >= 250) { record.nativeError = -ETIMEDOUT; throw std::runtime_error("JACK track port retirement is waiting for a silent native callback"); }
			QThread::msleep(1);
		}
	}
	auto next = snapshot(nullptr, &record);
	checkCancellation(cancelled);
	// Cancellation is admitted through this point. After disconnect begins,
	// finish owned-handle retirement; do not leave partially removed ports live.
	for (const auto port : record.ports)
	{
		if (!port) { continue; }
		const auto before = m_owner.m_api.portConnected(port);
		if (before < 0) { record.nativeError = before; throw std::runtime_error("JACK track audio connection observation failed"); }
		if (before)
		{
			const auto code = m_owner.m_api.disconnectPort(m_owner.m_client, port);
			if (code) { record.nativeError = code; throw std::runtime_error("JACK could not disconnect a retiring track audio port"); }
		}
		const auto connected = m_owner.m_api.portConnected(port);
		if (connected != 0) { record.nativeError = connected < 0 ? connected : -EIO; throw std::runtime_error("JACK did not confirm track audio port disconnection"); }
		if (m_owner.serverDisconnected()) { throw std::runtime_error("JACK disconnected during track audio port retirement"); }
	}
	publish(std::move(next)); // Join all buffer users before unregistering either handle.
	for (auto& port : record.ports)
	{
		if (!port) { continue; }
		try
		{
			const auto code = m_owner.m_api.unregisterPort(m_owner.m_client, port);
			if (code) { record.nativeError = code; throw std::runtime_error("JACK could not unregister a retired track audio port"); }
			port = nullptr;
		}
		catch (...) { retireUncertainClient(); throw; }
	}
	record.muted = false;
}

void JackAudioBuses::reconcile(Record& record, const std::function<bool()>& cancelled)
{
	record.retry.start(); record.nativeError = 0; record.error.clear();
	try
	{
		checkCancellation(cancelled);
		if (!m_owner.m_client || m_owner.serverDisconnected()) { record.nativeError = -ENOTCONN; throw std::runtime_error("JACK track port graph is unavailable"); }
		if (record.source->retired()) { record.wanted = false; }
		if (record.wanted)
		{
			if (!record.ports[0] || !record.ports[1]) { registerPorts(record, cancelled); }
			else if (record.muted) { publish(snapshot()); }
		}
		else if (record.ports[0] || record.ports[1]) { unregisterPorts(record, cancelled); }
	}
	catch (const std::exception& error)
	{
		if (!record.nativeError) { record.nativeError = -EIO; }
		record.error = QString::fromUtf8(error.what()).left(1024); throw;
	}
	catch (...)
	{
		if (!record.nativeError) { record.nativeError = -EIO; }
		record.error = "JACK track port update failed"; throw;
	}
}

void JackAudioBuses::synchronize(const std::shared_ptr<const AudioBusOutput>& source, const std::function<bool()>& cancelled)
{
	if (const auto record = find(source)) { reconcile(*record, cancelled); }
}

void JackAudioBuses::prune()
{
	for (auto it = m_records.begin(); it != m_records.end(); )
	{
		const auto& record = *it->second;
		if (record.source->retired() && !record.ports[0] && !record.ports[1]) { it = m_records.erase(it); }
		else { ++it; }
	}
}

void JackAudioBuses::poll()
{
	if (m_owner.m_graphBusy || m_owner.m_recovering || m_owner.serverDisconnected()
		|| m_owner.audioEngine()->audioDev() != &m_owner || !m_owner.audioEngine()->canChangeBackendGraph()) { return; }
	FileResourceIO::EventSuspension events; m_owner.m_graphBusy = true;
	const auto idle = qScopeGuard([&] { m_owner.m_graphBusy = false; });
	int attempted = 0;
	for (auto& item : m_records)
	{
		auto& record = *item.second;
		if (!pending(record) || (record.retry.isValid() && record.retry.elapsed() < 1000)) { continue; }
		try { reconcile(record, {}); } catch (...) { }
		if (++attempted >= 8 || m_owner.serverDisconnected()) { break; }
	}
	prune();
}

void JackAudioBuses::prepareClient()
{
	// Used only before initial activation or after owned client recreation.
	FileResourceIO::EventSuspension events; m_owner.m_graphBusy = true;
	const auto idle = qScopeGuard([&] { m_owner.m_graphBusy = false; });
	for (auto& item : m_records)
	{
		auto& record = *item.second;
		if (!record.wanted || record.source->retired()) { continue; }
		try { reconcile(record, {}); } catch (...) { }
		if (m_owner.serverDisconnected()) { break; }
	}
	prune();
}

void JackAudioBuses::detachClient()
{
	publish({});
	for (auto& item : m_records)
	{
		auto& record = *item.second; record.ports = {}; record.muted = false; record.retry.invalidate();
	}
}

std::vector<jack_port_t*> JackAudioBuses::routePorts(const std::shared_ptr<const AudioBusOutput>& source)
{
	const auto record = find(source);
	if (!record || !record->wanted || record->source->retired() || record->muted || !record->ports[0] || !record->ports[1])
	{ throw std::runtime_error("Native track audio ports are not published; configure and inspect the bus before routing"); }
	return {record->ports[0], record->ports[1]};
}

AudioDevice::BusGraph JackAudioBuses::inspect(const std::shared_ptr<const AudioBusOutput>& source, bool enabled)
{
	AudioDevice::BusGraph result; result.enabled = enabled;
	std::vector<jack_port_t*> ports;
	if (const auto record = find(source))
	{
		result.id = QString::number(static_cast<qulonglong>(record->id)); result.label = record->label.left(1024); result.labelTruncated = record->label.size() > 1024;
		result.pending = pending(*record); result.nativeErrorCode = record->nativeError; result.error = record->error;
		result.published = record->ports[0] && record->ports[1] && m_owner.m_client && !m_owner.serverDisconnected();
		if (result.published) { ports.assign(record->ports.begin(), record->ports.end()); }
	}
	else { result.pending = enabled; }
	result.graph = m_owner.inspectGraphPorts(false, ports, false);
	return result;
}
}
#endif
