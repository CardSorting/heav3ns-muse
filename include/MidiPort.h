/*
 * MidiPort.h - abstraction of MIDI ports which are part of LMMS' MIDI
 *              sequencing system
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

#ifndef LMMS_MIDI_PORT_H
#define LMMS_MIDI_PORT_H

#include <QString>
#include <QList>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <array>
#include <mutex>
#include <memory>
#include <utility>
#include <QPointer>

#include "Midi.h"
#include "TimePos.h"
#include "AutomatableModel.h"

namespace lmms
{

class MidiClient;
class MidiEvent;
class MidiEventProcessor;

namespace gui
{

class MidiPortMenu;
class ControllerConnectionDialog;
class InstrumentMidiIOView;

}


// class for abstraction of MIDI-port
class MidiPort : public Model, public SerializingObject
{
	Q_OBJECT
	mapPropertyFromModel(int,inputChannel,setInputChannel,m_inputChannelModel);
	mapPropertyFromModel(int,outputChannel,setOutputChannel,m_outputChannelModel);
	mapPropertyFromModel(int,inputController,setInputController,m_inputControllerModel);
	mapPropertyFromModel(int,outputController,setOutputController,m_outputControllerModel);
	mapPropertyFromModel(int,fixedInputVelocity,setFixedInputVelocity,m_fixedInputVelocityModel);
	mapPropertyFromModel(int,fixedOutputVelocity,setFixedOutputVelocity,m_fixedOutputVelocityModel);
	mapPropertyFromModel(int,fixedOutputNote,setFixedOutputNote,m_fixedOutputNoteModel);
	mapPropertyFromModel(int,outputProgram,setOutputProgram,m_outputProgramModel);
	mapPropertyFromModel(int,baseVelocity,setBaseVelocity,m_baseVelocityModel);
	mapPropertyFromModel(bool,isReadable,setReadable,m_readableModel);
	mapPropertyFromModel(bool,isWritable,setWritable,m_writableModel);
public:
	using Map = QMap<QString, bool>;
	enum class OutputSource { Track, Instrument };
	static constexpr unsigned int OutputNoteLimit = 4096;

	enum class Mode
	{
		Disabled,	// don't route any MIDI-events (default)
		Input,		// from MIDI-client to MIDI-event-processor
		Output,		// from MIDI-event-processor to MIDI-client
		Duplex		// both directions
	} ;

	MidiPort( const QString& name,
			MidiClient* client,
			MidiEventProcessor* eventProcessor,
			Model* parent = nullptr,
			Mode mode = Mode::Disabled );
	~MidiPort() override;

	void setName( const QString& name );

	Mode mode() const
	{
		return m_mode;
	}

	void setMode( Mode mode );

	bool isInputEnabled() const
	{
		return mode() == Mode::Input || mode() == Mode::Duplex;
	}

	bool isOutputEnabled() const
	{
		return mode() == Mode::Output || mode() == Mode::Duplex;
	}

	int realOutputChannel() const
	{
		// There's a possibility of outputChannel being 0 ("--"), which is used to keep all
		// midi channels when forwarding. In that case, realOutputChannel will return the
		// default channel 1 (whose value is 0).
		return outputChannel() ? outputChannel() - 1 : 0;
	}

	void processInEvent( const MidiEvent& event, const TimePos& time = TimePos() );
	void processOutEvent(const MidiEvent& event, const TimePos& time = TimePos(), OutputSource source = OutputSource::Track);
	OutputSource outputSource() const;
	bool acceptsOutput(OutputSource source) const;
	void setOutputSource(OutputSource source);
	// Release only notes/pedals submitted by this port before retiring routing.
	// Native backends provide no hardware-delivery acknowledgment.
	void releaseOutput();
	unsigned int trackedOutputNotes() const;
	void detachInputProcessor();
	bool inputDeliveryFailed();
	bool resetInputDelivery(QString& error, bool& changed);


	void saveSettings( QDomDocument& doc, QDomElement& thisElement ) override;
	void loadSettings( const QDomElement& thisElement ) override;

	QString nodeName() const override
	{
		return "midiport";
	}

	void subscribeReadablePort( const QString& port, bool subscribe = true );
	void subscribeWritablePort( const QString& port, bool subscribe = true );
	QStringList requestedReadablePorts() const;
	QStringList requestedWritablePorts() const;
	MidiClient* client() const { return m_midiClient; }

	const Map& readablePorts() const
	{
		return m_readablePorts;
	}

	const Map& writablePorts() const
	{
		return m_writablePorts;
	}

	void invalidateCilent();

	gui::MidiPortMenu* m_readablePortsMenu;
	gui::MidiPortMenu* m_writablePortsMenu;


public slots:
	void updateMidiPortMode();


private slots:
	void updateReadablePorts();
	void updateWritablePorts();
	void updateOutputProgram();


private:
	void setModeImpl(Mode mode);
	// A returned backend callback may retire the port before its lock unwinds.
	class OutputLock
	{
	public:
		OutputLock() = default;
		~OutputLock() = default;
		OutputLock(const OutputLock&) = delete;
		OutputLock& operator=(const OutputLock&) = delete;
		explicit OutputLock(std::shared_ptr<std::recursive_mutex> mutex) : m_mutex(std::move(mutex)), m_lock(*m_mutex) { }
		OutputLock(OutputLock&&) noexcept = default;
		OutputLock& operator=(OutputLock&& other) noexcept
		{
			if (this != &other) { m_lock = std::unique_lock<std::recursive_mutex>{}; m_mutex = std::move(other.m_mutex); m_lock = std::move(other.m_lock); }
			return *this;
		}
		bool owns_lock() const noexcept { return m_lock.owns_lock(); }
		void unlock() { m_lock.unlock(); }
	private:
		std::shared_ptr<std::recursive_mutex> m_mutex;
		std::unique_lock<std::recursive_mutex> m_lock;
	};
	OutputLock lockOutput() const { return OutputLock(m_outputMutex); }
	class Transition
	{
	public:
		Transition(MidiPort* port, bool MidiPort::* flag, bool release = false);
		~Transition();
		Transition(const Transition&) = delete;
		Transition& operator=(const Transition&) = delete;
		void check() const;
	private:
		QPointer<MidiPort> m_owner;
		MidiClient* m_client;
		bool MidiPort::* m_flag;
	};
	MidiClient* m_midiClient;
	MidiEventProcessor* m_midiEventProcessor;
	bool m_clientTransfer = false; // serialized with output; owner stages routing
	bool m_retiring = false;
	bool m_serializingSettings = false, m_changingMode = false, m_updatingMode = false;
	bool m_changingName = false, m_changingSource = false, m_releasingOutput = false;
	bool m_changingReadable = false, m_changingWritable = false;

	Mode m_mode;
	OutputSource m_outputSource = OutputSource::Track;
	const std::shared_ptr<std::recursive_mutex> m_outputMutex = std::make_shared<std::recursive_mutex>();
	std::array<std::array<quint16, MidiMaxKey + 1>, MidiChannelCount> m_outputNotes{};
	std::array<std::array<bool, 2>, MidiChannelCount> m_outputPedals{};
	unsigned int m_trackedOutputNotes = 0;

	IntModel m_inputChannelModel;
	IntModel m_outputChannelModel;
	IntModel m_inputControllerModel;
	IntModel m_outputControllerModel;
	IntModel m_fixedInputVelocityModel;
	IntModel m_fixedOutputVelocityModel;
	IntModel m_fixedOutputNoteModel;
	IntModel m_outputProgramModel;
	IntModel m_baseVelocityModel;
	BoolModel m_readableModel;
	BoolModel m_writableModel;

	Map m_readablePorts;
	Map m_writablePorts;
	QSet<QString> m_requestedReadablePorts;
	QSet<QString> m_requestedWritablePorts;


	friend class gui::ControllerConnectionDialog;
	friend class gui::InstrumentMidiIOView;
	friend class MidiPortTestAccess;
	friend class MidiClient;


signals:
	void outputSourceChanged();
	void readablePortsChanged();
	void writablePortsChanged();
	void modeChanged();

} ;

using MidiPortList = QList<MidiPort*>;

} // namespace lmms

#endif // LMMS_MIDI_PORT_H
