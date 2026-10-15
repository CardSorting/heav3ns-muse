/*
 * MidiApple.h - Apple raw MIDI client
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

#ifndef LMMS_MIDI_APPLE_H
#define LMMS_MIDI_APPLE_H

#include "lmmsconfig.h"
#ifdef LMMS_BUILD_APPLE

#include "MidiClient.h"
#include "MidiPort.h"
#include <CoreMIDI/CoreMIDI.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace lmms
{

class MidiApple : public QObject, public MidiClient
{
	Q_OBJECT
public:
	MidiApple();
	~MidiApple() override;

	static QString probeDevice() { return {}; }
	static QString name() { return QT_TRANSLATE_NOOP("MidiSetupWidget", "Apple MIDI"); }
	static QString configSection() { return {}; }

	void processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort* port) override;
	void applyPortMode(MidiPort* port) override;
	void removePort(MidiPort* port) override;
	QStringList readablePorts() const override;
	QStringList writablePorts() const override;
	QString sourcePortName(const MidiEvent& event) const override { return MidiClient::sourcePortName(event); }
	void subscribeReadablePort(MidiPort* port, const QString& destination, bool subscribe = true) override;
	void subscribeWritablePort(MidiPort* port, const QString& destination, bool subscribe = true) override;
	void connectRPChanged(QObject* receiver, const char* member) override
	{
		connect(this, SIGNAL(readablePortsChanged()), receiver, member);
	}
	void connectWPChanged(QObject* receiver, const char* member) override
	{
		connect(this, SIGNAL(writablePortsChanged()), receiver, member);
	}
	bool hasQueuedInputDelivery() const override { return true; }
	InputBackendStatus inputBackendStatus() const override;
	bool isAvailable() const;

private:
	friend class MidiAppleTestAccess;
	explicit MidiApple(bool initializeNative);
	class SourceParser;
	struct SourceInfo;
	struct EndpointInfo
	{
		MIDIEndpointRef endpoint = 0;
		QString name;
		SInt32 uniqueID = 0;
		MIDIDeviceRef device = 0;
	};
	using SourceMap = QMap<MIDIEndpointRef, std::shared_ptr<SourceInfo>>;
	using SubMap = QMap<QString, MidiPortList>;

	static void NotifyCallback(const MIDINotification* notification, void* context) noexcept;
	static void ReadCallback(const MIDIPacketList* packets, void* clientToken, void* sourceToken) noexcept;
	static QMap<QString, MIDIEndpointRef> endpointNames(const QList<EndpointInfo>& endpoints);
	static bool createMidiPacketList(const MidiEvent& event, MIDIPacketList& packets);

	void initialize();
	void closeDevices();
	void updateDeviceList();
	void scheduleDeviceUpdateLocked();
	void handlePacketsLocked(const MIDIPacketList* packets, std::uintptr_t sourceToken);
	void processSourceEventLocked(const QString& sourceName, const MidiEvent& event);
	void retireSourceLocked(const std::shared_ptr<SourceInfo>& source);
	void faultSourceSubscribersLocked(const QString& sourceName);
	std::uintptr_t registerSource(MIDIEndpointRef endpoint, const QString& name, SInt32 uniqueID = 0, MIDIDeviceRef device = 0);
	void renameSourceLocked(const std::shared_ptr<SourceInfo>& source, const QString& name);
	void recordError(OSStatus status) noexcept;

	// Callback lookup holds the registry before this mutex. CoreMIDI API calls hold neither.
	mutable std::mutex m_devicesMutex;
	std::mutex m_apiMutex;
	std::uintptr_t m_callbackToken = 0;
	MIDIClientRef m_client = 0; // The native client belongs to the process service.
	MIDIPortRef m_inputPort = 0;
	MIDIPortRef m_outputPort = 0;
	bool m_closing = false;
	bool m_updatePending = false;
	std::atomic<int> m_nativeError{0};
	QMap<QString, MIDIEndpointRef> m_inputDevices;
	QMap<QString, MIDIEndpointRef> m_outputDevices;
	QMap<MIDIEndpointRef, MIDIDeviceRef> m_outputEndpointDevices;
	SourceMap m_sources;
	QMap<std::uintptr_t, std::shared_ptr<SourceInfo>> m_sourceTokens;
	SubMap m_inputSubs;
	SubMap m_outputSubs;

signals:
	void readablePortsChanged();
	void writablePortsChanged();
};

} // namespace lmms
#endif // LMMS_BUILD_APPLE
#endif // LMMS_MIDI_APPLE_H
