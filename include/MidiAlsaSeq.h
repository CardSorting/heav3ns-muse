/*
 * MidiAlsaSeq.h - ALSA-sequencer-client
 *
 * Copyright (c) 2005-2013 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_MIDI_ALSA_SEQ_H
#define LMMS_MIDI_ALSA_SEQ_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_ALSA
#include <alsa/asoundlib.h>

#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>
#include <atomic>


#include "MidiClient.h"




namespace lmms
{


class MidiAlsaSeq : public QThread, public MidiClient
{
	Q_OBJECT
public:
	MidiAlsaSeq();
	~MidiAlsaSeq() override;
	bool hasQueuedInputDelivery() const override { return true; }
	InputBackendStatus inputBackendStatus() const override;
	OutputClockStatus outputClockStatus() const override;

	static QString probeDevice();


	inline static QString name()
	{
		return QT_TRANSLATE_NOOP( "MidiSetupWidget",
			"ALSA-Sequencer (Advanced Linux Sound "
							"Architecture)" );
	}

	inline static QString configSection()
	{
		return "Midialsaseq";
	}



	void processOutEvent( const MidiEvent & _me,
						const TimePos & _time,
						const MidiPort * _port ) override;
	void cancelOutputEvents(const MidiPort* port) override;

	void applyPortMode( MidiPort * _port ) override;
	void applyPortName( MidiPort * _port ) override;

	void removePort( MidiPort * _port ) override;


	// list seq-ports from ALSA
	QStringList readablePorts() const override
	{
		const QMutexLocker locker(&m_portListMutex);
		return m_readablePorts;
	}

	QStringList writablePorts() const override
	{
		const QMutexLocker locker(&m_portListMutex);
		return m_writablePorts;
	}

	// return name of port which specified MIDI event came from
	QString sourcePortName( const MidiEvent & ) const override;

	// (un)subscribe given MidiPort to/from destination-port
	void subscribeReadablePort( MidiPort * _port,
						const QString & _dest,
						bool _subscribe = true ) override;
	void subscribeWritablePort( MidiPort * _port,
						const QString & _dest,
						bool _subscribe = true ) override;
	void connectRPChanged( QObject * _receiver,
							const char * _member ) override
	{
		connect( this, SIGNAL( readablePortsChanged() ),
							_receiver, _member );
	}

	void connectWPChanged( QObject * _receiver,
							const char * _member ) override
	{
		connect( this, SIGNAL( writablePortsChanged() ),
							_receiver, _member );
	}


private slots:
	void changeQueueTempo( lmms::bpm_t _bpm );
	void updatePortList();


private:
	friend class MidiAlsaSeqTestAccess;
	explicit MidiAlsaSeq(bool connectSequencer);
	bool initialize();
	void closeResources();
	void recordError(int code);
	void validatePortForTransfer(const MidiPort* port) const override;
	void checkOutputResult(int code);
	int synchronizeQueueTempoLocked(bpm_t tempo);
	// m_seqMutex also serializes output submission, cancellation and native ID reuse.
	void cancelOutputEventsLocked(int id, quint16& activeChannels);
	void inputFailed(int code, bool terminal) noexcept;
	// Caller holds m_seqMutex through destination lookup and delivery registration.
	void processInputEventLocked(const snd_seq_event_t& event, const QString& sourceName);
	void subscribePort(MidiPort* port, const QString& destination, bool input, bool subscribe);
	void publishPortLists(const QStringList& readable, const QStringList& writable);
	void run() override;

#ifdef LMMS_HAVE_ALSA
	mutable QMutex m_seqMutex;
	mutable QMutex m_portListMutex;
	snd_seq_t * m_seqHandle;
	struct Ports
	{
		Ports() { p[0] = -1; p[1] = -1; }
		int & operator[]( const int _i ) { return p[_i]; }
		int operator[](const int i) const { return p[i]; }
		// A submitted release may still be queued. Retain channels until cancellation
		// and immediate cleanup both succeed, independently of MidiPort's note count.
		quint16 activeOutputChannels = 0;
		private: int p[2];
	} ;
	QMap<MidiPort *, Ports> m_portIDs;
	struct OutputApi
	{
		decltype(&snd_seq_event_output_direct) send = &snd_seq_event_output_direct;
		decltype(&snd_seq_remove_events) remove = &snd_seq_remove_events;
		decltype(&snd_seq_get_queue_tempo) getTempo = &snd_seq_get_queue_tempo;
		decltype(&snd_seq_set_queue_tempo) setTempo = &snd_seq_set_queue_tempo;
	} m_outputApi;
#endif

	int m_queueID;
	bpm_t m_queueTempo = 0;
	unsigned int m_queueMicrosecondsPerQuarterNote = 0;
	int m_queueTempoError = 0;
	int m_clientID = -1;
	std::atomic<int> m_nativeError{0};
	QMetaObject::Connection m_tempoConnection;

	std::atomic<bool> m_quit;

	QTimer m_portListUpdateTimer;
	QStringList m_readablePorts;
	QStringList m_writablePorts;

	int m_pipe[2]{-1, -1};


signals:
	void readablePortsChanged();
	void writablePortsChanged();

} ;


} // namespace lmms

#endif // LMMS_HAVE_ALSA

#endif // LMMS_MIDI_ALSA_SEQ_H
