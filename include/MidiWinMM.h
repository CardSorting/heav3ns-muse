/*
 * MidiWinMM.h - WinMM MIDI client
 *
 * Copyright (c) 2008-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_MIDI_WINMM_H
#define LMMS_MIDI_WINMM_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_WINMM
#include <windows.h>
#include <mmsystem.h>

#include "MidiClient.h"
#include "MidiPort.h"
#include <atomic>
#include <QSet>


class QLineEdit;


namespace lmms
{

namespace detail { struct WinMmMidiApi; }

class MidiWinMM : public QObject, public MidiClient
{
	Q_OBJECT
public:
	MidiWinMM();
	virtual ~MidiWinMM();
	bool hasQueuedInputDelivery() const override { return true; }
	InputBackendStatus inputBackendStatus() const override;
	OutputBackendStatus outputBackendStatus() const override;
	void retainFailedOutputCleanup(const MidiPort* port, std::uint16_t channels) noexcept override;
	bool isAvailable() const;
	static NativeRetirementStatus retiredHandleStatus();

	inline static QString probeDevice()
	{
		return QString(); // no midi device name
	}


	inline static QString name()
	{
		return QT_TRANSLATE_NOOP( "MidiSetupWidget", "WinMM MIDI" );
	}

	inline static QString configSection()
	{
		return QString(); // no configuration settings
	}



	virtual void processOutEvent( const MidiEvent & _me,
						const TimePos & _time,
						const MidiPort * _port );

	virtual void applyPortMode( MidiPort * _port );
	virtual void removePort( MidiPort * _port );


	// list devices as ports
	virtual QStringList readablePorts() const
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		return m_inputDevices.values();
	}

	virtual QStringList writablePorts() const
	{
		const auto lock = std::lock_guard{m_devicesMutex};
		QStringList names; for (const auto& output : m_outputDevices) { names.append(output.name); } return names;
	}

	// return name of port which specified MIDI event came from
	virtual QString sourcePortName( const MidiEvent & ) const;

	// (un)subscribe given MidiPort to/from destination-port
	virtual void subscribeReadablePort( MidiPort * _port,
						const QString & _dest,
						bool _subscribe = true );
	virtual void subscribeWritablePort( MidiPort * _port,
						const QString & _dest,
						bool _subscribe = true );
	virtual void connectRPChanged( QObject * _receiver,
							const char * _member )
	{
		connect( this, SIGNAL( readablePortsChanged() ),
							_receiver, _member );
	}

	virtual void connectWPChanged( QObject * _receiver,
							const char * _member )
	{
		connect( this, SIGNAL( writablePortsChanged() ),
							_receiver, _member );
	}

	virtual bool isRaw() const
	{
		return false;
	}


private:// slots:
	void updateDeviceList();


private:
	friend class MidiWinMMTestAccess;
	friend class WinMmMidiIoTestAccess;
	explicit MidiWinMM(bool openPhysicalDevices);
	MidiWinMM(const detail::WinMmMidiApi& api, bool openPhysicalDevices);
	bool readyForPortTransfer() const override;
	void finishPortTransferOutput(const std::function<bool()>& cancelled) override;
	// Caller holds output lifetime, never the device-table mutex, during native calls.
	MMRESULT submitShortMessage(HMIDIOUT handle, DWORD message) noexcept;
	MMRESULT flushOutputCleanup(HMIDIOUT handle, const std::function<bool()>& cancelled);
	void flushAllOutputCleanup(const std::function<bool()>& cancelled);
	void openDevices();
	void closeDevices();
	DWORD_PTR registerInput(HMIDIIN handle);
	void removeInputToken(DWORD_PTR token);
	void recordError(MMRESULT result) noexcept;
	void recordOutputError(MMRESULT result) noexcept;
	void handleInputFailure(HMIDIIN handle, bool closed);

	static void WINAPI CALLBACK inputCallback( HMIDIIN _hm, UINT _msg,
						DWORD_PTR _inst,
						DWORD_PTR _param1,
							DWORD_PTR _param2 ) noexcept;
	void handleInputEvent( HMIDIIN _hm, DWORD _ev );

	const detail::WinMmMidiApi& m_api;
	std::uint64_t m_nativeOwnerId = 0;
	QMap<HMIDIIN, QString> m_inputDevices;
	mutable std::mutex m_devicesMutex;
	std::mutex m_outputMutex;
	struct OutputDevice
	{
		QString name;
		MMRESULT result = MMSYSERR_NOERROR;
		std::uint16_t cleanupChannels = 0; // Owns no retired MidiPort pointer.
		// Possible activations since the last complete channel cleanup. Ordinary
		// note releases cannot prove that another source/pedal on the channel ended.
		std::uint16_t usedChannels = 0;
	};
	QMap<HMIDIOUT, OutputDevice> m_outputDevices;
	bool m_closing = false; // Device-table mutex; remains true after close until open.
	QSet<HMIDIIN> m_startedInputs;
	std::atomic<int> m_nativeError{0};
	std::atomic<int> m_lastOutputError{0};

	// subscriptions
	using SubMap = QMap<QString, MidiPortList>;
	SubMap m_inputSubs;
	SubMap m_outputSubs;


signals:
	void readablePortsChanged();
	void writablePortsChanged();

} ;


} // namespace lmms

#endif // LMMS_HAVE_WINMM

#endif // LMMS_MIDI_WINMM_H
