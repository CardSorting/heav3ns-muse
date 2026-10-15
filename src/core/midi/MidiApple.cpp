/*
 * MidiApple.cpp - Apple MIDI client
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2015 Maurizio Lo Bosco (rageboge on github)
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

#include "MidiApple.h"

#ifdef LMMS_BUILD_APPLE

#include "MidiEventToByteSeq.h"
#include <QScopeGuard>
#include <QSet>
#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace lmms
{
namespace
{
// Process lifetime: the framework may still call a retired opaque refCon.
// Tokens are never dereferenced/reused; only live client registrations remain in this map.
struct CallbackRegistry
{
	std::mutex mutex;
	std::map<std::uintptr_t, MidiApple*> clients;
	std::uintptr_t nextToken = 1;
};
CallbackRegistry& callbackRegistry()
{
	static auto* registry = new CallbackRegistry;
	return *registry;
}
std::uintptr_t allocateTokenLocked(CallbackRegistry& registry)
{
	if (registry.nextToken == std::numeric_limits<std::uintptr_t>::max()) { return 0; }
	return registry.nextToken++;
}
std::uintptr_t sourceToken()
{
	auto& registry = callbackRegistry();
	const auto lock = std::lock_guard{registry.mutex};
	return allocateTokenLocked(registry);
}

// Apple advises against disposing the last client if an application may create clients later.
// One process-owned client survives backend reconstruction; individual input/output ports retire.
struct ProcessClient
{
	std::mutex mutex;
	MIDIClientRef reference = 0;
};
OSStatus processClient(MIDINotifyProc notify, MIDIClientRef& reference)
{
	static auto* service = new ProcessClient;
	const auto lock = std::lock_guard{service->mutex};
	if (!service->reference)
	{
		MIDIClientRef created = 0;
		const auto status = MIDIClientCreate(CFSTR("LMMS MIDI"), notify, nullptr, &created);
		if (status != noErr) { return status; }
		service->reference = created;
	}
	reference = service->reference;
	return noErr;
}

QString objectName(MIDIObjectRef object)
{
	CFStringRef name = nullptr;
	if (!object || MIDIObjectGetStringProperty(object, kMIDIPropertyName, &name) != noErr || !name) { return {}; }
	const auto release = qScopeGuard([name] { CFRelease(name); });
	const auto length = CFStringGetLength(name);
	const auto maximum = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
	if (maximum < 0 || maximum > std::numeric_limits<int>::max()) { return {}; }
	QByteArray bytes(static_cast<int>(maximum), '\0');
	CFIndex used = 0;
	if (CFStringGetBytes(name, CFRangeMake(0, length), kCFStringEncodingUTF8, 0, false,
		reinterpret_cast<UInt8*>(bytes.data()), maximum, &used) != length) { return {}; }
	return QString::fromUtf8(bytes.constData(), static_cast<int>(used));
}

MIDIDeviceRef endpointDevice(MIDIEndpointRef endpoint)
{
	MIDIEntityRef entity = 0;
	MIDIDeviceRef device = 0;
	if (MIDIEndpointGetEntity(endpoint, &entity) == noErr && entity
		&& MIDIEntityGetDevice(entity, &device) == noErr) { return device; }
	return 0;
}

QString endpointName(MIDIEndpointRef endpoint, MIDIDeviceRef device)
{
	const auto name = objectName(endpoint);
	const auto deviceName = objectName(device);
	if (!deviceName.isEmpty()) { return deviceName + ":" + name; }
	return name.isEmpty() ? QString("Endpoint %1").arg(endpoint) : name;
}

bool isOffline(MIDIEndpointRef endpoint)
{
	SInt32 offline = 0;
	return MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyOffline, &offline) == noErr && offline != 0;
}
}

class MidiApple::SourceParser final : public MidiClientRaw
{
public:
	SourceParser(MidiApple& owner, QString source) : name(std::move(source)), m_owner(owner) { }
	void feed(unsigned char byte) { parseData(byte); }
	QString name;
private:
	void sendByte(unsigned char) override { }
	void processParsedEvent(const MidiEvent& event) override { m_owner.processSourceEventLocked(name, event); }
	MidiApple& m_owner;
};

struct MidiApple::SourceInfo
{
	SourceInfo(MidiApple& owner, MIDIEndpointRef endpoint, std::uintptr_t generation, const QString& name, SInt32 unique, MIDIDeviceRef parent) :
		endpoint(endpoint), token(generation), uniqueID(unique), device(parent), parser(std::make_unique<SourceParser>(owner, name)) { }
	MIDIEndpointRef endpoint;
	std::uintptr_t token;
	SInt32 uniqueID;
	MIDIDeviceRef device;
	std::unique_ptr<SourceParser> parser;
};

MidiApple::MidiApple() : MidiApple(true) { }

MidiApple::MidiApple(bool initializeNative)
{
	auto& registry = callbackRegistry();
	{
		const auto lock = std::lock_guard{registry.mutex};
		m_callbackToken = allocateTokenLocked(registry);
		if (m_callbackToken) { registry.clients.emplace(m_callbackToken, this); }
	}
	if (!m_callbackToken) { recordError(kMIDIUnknownError); return; }
	if (!initializeNative) { return; }
	try { initialize(); }
	catch (...) { recordError(kMIDIUnknownError); closeDevices(); }
}

MidiApple::~MidiApple() { closeDevices(); }

void MidiApple::recordError(OSStatus status) noexcept
{
	if (status == noErr) { return; }
	int expected = 0;
	m_nativeError.compare_exchange_strong(expected, static_cast<int>(status), std::memory_order_relaxed);
}

void MidiApple::initialize()
{
	MIDIClientRef client = 0;
	const auto status = processClient(&MidiApple::NotifyCallback, client);
	recordError(status);
	if (status != noErr) { return; }
	MIDIPortRef input = 0, output = 0;
	const auto inputStatus = MIDIInputPortCreate(client, CFSTR("LMMS input"), &MidiApple::ReadCallback,
		reinterpret_cast<void*>(m_callbackToken), &input);
	recordError(inputStatus);
	if (inputStatus != noErr) { input = 0; }
	const auto outputStatus = MIDIOutputPortCreate(client, CFSTR("LMMS output"), &output);
	recordError(outputStatus);
	if (outputStatus != noErr) { output = 0; }
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		m_client = client; m_inputPort = input; m_outputPort = output;
	}
	updateDeviceList();
}

MidiClient::InputBackendStatus MidiApple::inputBackendStatus() const
{
	const auto lock = std::lock_guard{m_devicesMutex};
	return {true, !m_closing && m_client && m_inputPort, m_nativeError.load(std::memory_order_relaxed)};
}

bool MidiApple::isAvailable() const
{
	const auto lock = std::lock_guard{m_devicesMutex};
	return !m_closing && m_client && (m_inputPort || m_outputPort);
}

QStringList MidiApple::readablePorts() const
{
	const auto lock = std::lock_guard{m_devicesMutex};
	return m_inputDevices.keys();
}
QStringList MidiApple::writablePorts() const
{
	const auto lock = std::lock_guard{m_devicesMutex};
	return m_outputDevices.keys();
}

QMap<QString, MIDIEndpointRef> MidiApple::endpointNames(const QList<EndpointInfo>& endpoints)
{
	QMap<QString, int> counts;
	for (const auto& endpoint : endpoints) { ++counts[endpoint.name]; }
	auto ordered = endpoints;
	std::sort(ordered.begin(), ordered.end(), [](const auto& first, const auto& second) {
		if (first.uniqueID != second.uniqueID) { return first.uniqueID < second.uniqueID; }
		return first.endpoint < second.endpoint;
	});
	QMap<QString, MIDIEndpointRef> names;
	// Reserve legacy unique names before adding duplicate suffixes that could collide with literal names.
	for (const auto& endpoint : ordered)
	{
		if (counts.value(endpoint.name) == 1) { names.insert(endpoint.name, endpoint.endpoint); }
	}
	for (const auto& endpoint : ordered)
	{
		if (counts.value(endpoint.name) == 1) { continue; }
		auto name = endpoint.name + QString(" [id:%1]").arg(endpoint.uniqueID ? endpoint.uniqueID : static_cast<qint64>(endpoint.endpoint));
		while (names.contains(name)) { name += QString(" [ref:%1]").arg(endpoint.endpoint); }
		names.insert(name, endpoint.endpoint);
	}
	return names;
}

void MidiApple::updateDeviceList()
{
	bool inputChanged = false, outputChanged = false;
	{
		const auto apiLock = std::lock_guard{m_apiMutex};
		MIDIPortRef input;
		{
			const auto lock = std::lock_guard{m_devicesMutex};
			if (m_closing || !m_client) { return; }
			input = m_inputPort;
		}
		struct Catalog { QMap<QString, MIDIEndpointRef> names; QMap<MIDIEndpointRef, EndpointInfo> info; };
		const auto enumerate = [](bool sources) {
			Catalog result; QList<EndpointInfo> descriptions;
			const auto count = sources ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
			for (ItemCount index = 0; index < count; ++index)
			{
				const auto endpoint = sources ? MIDIGetSource(index) : MIDIGetDestination(index);
				if (!endpoint || isOffline(endpoint)) { continue; }
				SInt32 unique = 0;
				MIDIObjectGetIntegerProperty(endpoint, kMIDIPropertyUniqueID, &unique);
				const auto device = endpointDevice(endpoint);
				const EndpointInfo info{endpoint, endpointName(endpoint, device), unique, device};
				descriptions.append(info); result.info.insert(endpoint, info);
			}
			result.names = endpointNames(descriptions); return result;
		};
		const auto inputs = input ? enumerate(true) : Catalog{};
		const auto outputs = enumerate(false);
		QSet<MIDIEndpointRef> desired;
		for (auto it = inputs.names.cbegin(); it != inputs.names.cend(); ++it) { desired.insert(it.value()); }
		SourceMap removed;
		{
			const auto lock = std::lock_guard{m_devicesMutex};
			for (auto it = m_sources.begin(); it != m_sources.end();)
			{
				if (desired.contains(it.key()) && it.value()->uniqueID == inputs.info.value(it.key()).uniqueID) { ++it; continue; }
				retireSourceLocked(it.value()); removed.insert(it.key(), it.value()); it = m_sources.erase(it);
			}
		}
		// No callback/table mutex is held across calls that may enter CoreMIDI.
		for (auto it = removed.cbegin(); it != removed.cend(); ++it) { recordError(MIDIPortDisconnectSource(input, it.key())); }
		for (auto it = inputs.names.cbegin(); it != inputs.names.cend(); ++it)
		{
			bool exists;
			{
				const auto lock = std::lock_guard{m_devicesMutex};
				const auto found = m_sources.find(it.value());
				exists = found != m_sources.end();
				if (exists) { found.value()->device = inputs.info.value(it.value()).device; }
				if (exists && found.value()->parser->name != it.key())
				{
					renameSourceLocked(found.value(), it.key());
				}
			}
			if (exists) { continue; }
			const auto info = inputs.info.value(it.value());
			const auto token = registerSource(it.value(), it.key(), info.uniqueID, info.device);
			if (!token) { continue; }
			const auto connected = MIDIPortConnectSource(input, it.value(), reinterpret_cast<void*>(token));
			recordError(connected);
			if (connected != noErr)
			{
				const auto lock = std::lock_guard{m_devicesMutex};
				const auto source = m_sources.value(it.value());
				if (source) { retireSourceLocked(source); m_sources.remove(it.value()); }
			}
		}
		QMap<QString, MIDIEndpointRef> connectedInputs;
		{
			const auto lock = std::lock_guard{m_devicesMutex};
			for (auto it = m_sources.cbegin(); it != m_sources.cend(); ++it) { connectedInputs.insert(it.value()->parser->name, it.key()); }
			inputChanged = m_inputDevices != connectedInputs; outputChanged = m_outputDevices != outputs.names;
			m_inputDevices = connectedInputs; m_outputDevices = outputs.names;
			m_outputEndpointDevices.clear();
			for (auto it = outputs.info.cbegin(); it != outputs.info.cend(); ++it) { m_outputEndpointDevices.insert(it.key(), it.value().device); }
		}
	}
	if (inputChanged) { emit readablePortsChanged(); }
	if (outputChanged) { emit writablePortsChanged(); }
}

std::uintptr_t MidiApple::registerSource(MIDIEndpointRef endpoint, const QString& name, SInt32 uniqueID, MIDIDeviceRef device)
{
	const auto token = sourceToken();
	if (!token) { recordError(kMIDIUnknownError); return 0; }
	auto source = std::make_shared<SourceInfo>(*this, endpoint, token, name, uniqueID, device);
	const auto lock = std::lock_guard{m_devicesMutex};
	if (m_closing) { return 0; }
	const auto previous = m_sources.value(endpoint);
	if (previous) { retireSourceLocked(previous); }
	m_sources.insert(endpoint, source); m_sourceTokens.insert(token, source);
	return token;
}

void MidiApple::renameSourceLocked(const std::shared_ptr<SourceInfo>& source, const QString& name)
{
	faultSourceSubscribersLocked(source->parser->name);
	source->parser->name = name;
}

void MidiApple::retireSourceLocked(const std::shared_ptr<SourceInfo>& source)
{
	m_sourceTokens.remove(source->token);
	faultSourceSubscribersLocked(source->parser->name);
}

void MidiApple::faultSourceSubscribersLocked(const QString& sourceName)
{
	for (MidiPort* port : m_inputSubs.value(sourceName))
	{
		cancelInputEvents(port, sourceName);
		failInputEvents(InputDeliveryFailure::BackendInput, port);
	}
}

void MidiApple::closeDevices()
{
	const auto apiLock = std::lock_guard{m_apiMutex};
	auto& registry = callbackRegistry();
	{
		// A callback holds this mutex until all its accesses to the client finish.
		const auto callbackLock = std::lock_guard{registry.mutex};
		registry.clients.erase(m_callbackToken);
	}
	MIDIPortRef input, output;
	QList<MIDIEndpointRef> sources;
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		m_closing = true; m_updatePending = false;
		input = m_inputPort; output = m_outputPort;
		m_inputPort = 0; m_outputPort = 0; m_client = 0;
		sources = m_sources.keys();
		m_sources.clear(); m_sourceTokens.clear(); m_inputDevices.clear(); m_outputDevices.clear(); m_outputEndpointDevices.clear();
		m_inputSubs.clear(); m_outputSubs.clear();
	}
	if (input)
	{
		for (const auto source : sources) { recordError(MIDIPortDisconnectSource(input, source)); }
		recordError(MIDIPortDispose(input));
	}
	if (output) { recordError(MIDIPortDispose(output)); }
}

bool MidiApple::createMidiPacketList(const MidiEvent& event, MIDIPacketList& packets)
{
	std::uint8_t bytes[3]{};
	const auto length = writeToByteSeq(event, bytes, sizeof(bytes));
	if (!length) { return false; }
	const auto packet = MIDIPacketListInit(&packets);
	return MIDIPacketListAdd(&packets, sizeof(packets), packet, 0, length, bytes) != nullptr;
}

void MidiApple::processOutEvent(const MidiEvent& event, const TimePos&, const MidiPort* port)
{
	MIDIPacketList packets{};
	if (!createMidiPacketList(event, packets)) { return; }
	const auto apiLock = std::lock_guard{m_apiMutex};
	MIDIPortRef output;
	QList<MIDIEndpointRef> destinations;
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		if (m_closing || !m_outputPort) { return; }
		output = m_outputPort;
		for (auto it = m_outputSubs.cbegin(); it != m_outputSubs.cend(); ++it)
		{
			if (std::find(it.value().cbegin(), it.value().cend(), port) == it.value().cend()) { continue; }
			const auto endpoint = m_outputDevices.value(it.key());
			if (endpoint && !destinations.contains(endpoint)) { destinations.append(endpoint); }
		}
	}
	for (const auto endpoint : destinations) { recordError(MIDISend(output, endpoint, &packets)); }
}

void MidiApple::applyPortMode(MidiPort* port)
{
	const auto lock = std::lock_guard{m_devicesMutex};
	if (!port->isInputEnabled())
	{
		for (auto it = m_inputSubs.begin(); it != m_inputSubs.end(); ++it) { it.value().removeAll(port); }
	}
	if (!port->isOutputEnabled())
	{
		for (auto it = m_outputSubs.begin(); it != m_outputSubs.end(); ++it) { it.value().removeAll(port); }
	}
}

void MidiApple::removePort(MidiPort* port)
{
	const auto lock = std::lock_guard{m_devicesMutex};
	for (auto it = m_inputSubs.begin(); it != m_inputSubs.end(); ++it) { it.value().removeAll(port); }
	for (auto it = m_outputSubs.begin(); it != m_outputSubs.end(); ++it) { it.value().removeAll(port); }
	MidiClient::removePort(port);
}

void MidiApple::subscribeReadablePort(MidiPort* port, const QString& destination, bool subscribe)
{
	if (subscribe && !port->isInputEnabled()) { return; }
	const auto lock = std::lock_guard{m_devicesMutex};
	auto& ports = m_inputSubs[destination]; ports.removeAll(port);
	if (subscribe && !m_closing) { ports.push_back(port); }
	if (ports.isEmpty()) { m_inputSubs.remove(destination); }
}
void MidiApple::subscribeWritablePort(MidiPort* port, const QString& destination, bool subscribe)
{
	if (subscribe && !port->isOutputEnabled()) { return; }
	const auto lock = std::lock_guard{m_devicesMutex};
	auto& ports = m_outputSubs[destination]; ports.removeAll(port);
	if (subscribe && !m_closing) { ports.push_back(port); }
	if (ports.isEmpty()) { m_outputSubs.remove(destination); }
}

void MidiApple::processSourceEventLocked(const QString& sourceName, const MidiEvent& event)
{
	for (MidiPort* port : m_inputSubs.value(sourceName)) { dispatchInputEvent(event, TimePos{}, port, sourceName); }
}

void MidiApple::handlePacketsLocked(const MIDIPacketList* packets, std::uintptr_t token)
{
	const auto source = m_sourceTokens.value(token);
	if (m_closing || !packets || !source) { return; }
	const MIDIPacket* packet = &packets->packet[0];
	// CoreMIDI supplies a valid variable-length allocation. An empty packet has no byte to inspect.
	for (UInt32 index = 0; index < packets->numPackets; ++index)
	{
		for (UInt16 byte = 0; byte < packet->length; ++byte) { source->parser->feed(packet->data[byte]); }
		if (index + 1 < packets->numPackets) { packet = MIDIPacketNext(packet); }
	}
}

void MidiApple::ReadCallback(const MIDIPacketList* packets, void* context, void* source) noexcept
{
	try
	{
		auto& registry = callbackRegistry();
		const auto callbackLock = std::lock_guard{registry.mutex};
		const auto found = registry.clients.find(reinterpret_cast<std::uintptr_t>(context));
		if (found == registry.clients.end()) { return; }
		auto client = found->second;
		const auto lock = std::lock_guard{client->m_devicesMutex};
		try { client->handlePacketsLocked(packets, reinterpret_cast<std::uintptr_t>(source)); }
		catch (...) { client->recordError(kMIDIUnknownError); client->failInputEvents(); }
	}
	catch (...) { } // Do not unwind through the framework's C callback.
}

void MidiApple::scheduleDeviceUpdateLocked()
{
	if (m_closing || m_updatePending) { return; }
	m_updatePending = true;
	if (!QMetaObject::invokeMethod(this, [this] {
		{
			const auto lock = std::lock_guard{m_devicesMutex};
			if (m_closing) { return; }
			m_updatePending = false;
		}
		try { updateDeviceList(); }
		catch (...) { recordError(kMIDIUnknownError); failInputEvents(); }
	}, Qt::QueuedConnection))
	{
		m_updatePending = false; recordError(kMIDIUnknownError);
	}
}

void MidiApple::NotifyCallback(const MIDINotification* notification, void*) noexcept
{
	if (!notification || notification->messageSize < sizeof(MIDINotification)) { return; }
	try
	{
		auto& registry = callbackRegistry();
		const auto callbackLock = std::lock_guard{registry.mutex};
		for (const auto& registration : registry.clients)
		{
			auto client = registration.second;
			const auto lock = std::lock_guard{client->m_devicesMutex};
			if (client->m_closing) { continue; }
			if (notification->messageID == kMIDIMsgIOError && notification->messageSize >= sizeof(MIDIIOErrorNotification))
			{
				const auto error = reinterpret_cast<const MIDIIOErrorNotification*>(notification);
				bool affected = false;
				for (auto it = client->m_sources.cbegin(); it != client->m_sources.cend(); ++it)
				{
					const auto& source = it.value();
					if (error->driverDevice && source->device != error->driverDevice) { continue; }
					for (MidiPort* port : client->m_inputSubs.value(source->parser->name))
					{
						affected = true; client->failInputEvents(InputDeliveryFailure::BackendInput, port);
					}
				}
				for (auto it = client->m_outputSubs.cbegin(); it != client->m_outputSubs.cend(); ++it)
				{
					if (!it.value().isEmpty() && (!error->driverDevice || client->m_outputEndpointDevices.value(client->m_outputDevices.value(it.key())) == error->driverDevice)) { affected = true; }
				}
				if (affected) { client->recordError(error->errorCode); }
			}
			try { client->scheduleDeviceUpdateLocked(); }
			catch (...) { client->m_updatePending = false; client->recordError(kMIDIUnknownError); }
		}
	}
	catch (...) { }
}

} // namespace lmms
#endif // LMMS_BUILD_APPLE
