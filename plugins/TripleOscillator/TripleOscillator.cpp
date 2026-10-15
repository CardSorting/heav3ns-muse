/*
 * TripleOscillator.cpp - powerful instrument with three oscillators
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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


#include <QDomElement>
#include <QFileInfo>
#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>

#include "TripleOscillator.h"
#include "AudioEngine.h"
#include "AutomatableButton.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "Mixer.h"
#include "TrackContainer.h"
#include "FileDialog.h"
#include "InstrumentTrack.h"
#include "Knob.h"
#include "NotePlayHandle.h"
#include "Oscillator.h"
#include "PathUtil.h"
#include "PixmapButton.h"
#include "SampleBuffer.h"
#include "Song.h"
#include "embed.h"
#include "plugin_export.h"

namespace lmms
{


extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT tripleoscillator_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"TripleOscillator",
	QT_TRANSLATE_NOOP( "PluginBrowser",
				"Three powerful oscillators you can modulate "
				"in several ways" ),
	"Tobias Doerffel <tobydox/at/users.sf.net>",
	0x0110,
	Plugin::Type::Instrument,
	new PluginPixmapLoader( "logo" ),
	nullptr,
	nullptr,
} ;

}



OscillatorObject::OscillatorObject( Model * _parent, int _idx ) :
	Model( _parent ),
	m_volumeModel( DefaultVolume / NUM_OF_OSCILLATORS, MinVolume,
			MaxVolume, 1.0f, this, tr( "Osc %1 volume" ).arg( _idx+1 ) ),
	m_panModel( DefaultPanning, PanningLeft, PanningRight, 1.0f, this,
			tr( "Osc %1 panning" ).arg( _idx+1 ) ),
	m_coarseModel( -_idx*KeysPerOctave,
			-2 * KeysPerOctave, 2 * KeysPerOctave, 1.0f, this,
			tr( "Osc %1 coarse detuning" ).arg( _idx+1 ) ),
	m_fineLeftModel( 0.0f, -100.0f, 100.0f, 1.0f, this,
			tr( "Osc %1 fine detuning left" ).arg( _idx+1 ) ),
	m_fineRightModel( 0.0f, -100.0f, 100.0f, 1.0f, this,
			tr( "Osc %1 fine detuning right" ).arg( _idx + 1 ) ),
	m_phaseOffsetModel( 0.0f, 0.0f, 360.0f, 1.0f, this,
			tr( "Osc %1 phase-offset" ).arg( _idx+1 ) ),
	m_stereoPhaseDetuningModel( 0.0f, 0.0f, 360.0f, 1.0f, this,
			tr( "Osc %1 stereo phase-detuning" ).arg( _idx+1 ) ),
	m_waveShapeModel( static_cast<int>(Oscillator::WaveShape::Sine), 0,
			Oscillator::NumWaveShapes-1, this,
			tr( "Osc %1 wave shape" ).arg( _idx+1 ) ),
	m_modulationAlgoModel( static_cast<int>(Oscillator::ModulationAlgo::SignalMix), 0,
				Oscillator::NumModulationAlgos-1, this,
				tr( "Modulation type %1" ).arg( _idx+1 ) ),
	m_useWaveTableModel(true, this, tr("Osc %1 anti-aliasing").arg(_idx + 1)),

	m_sampleBuffer( new SampleBuffer ),
	m_volumeLeft( 0.0f ),
	m_volumeRight( 0.0f ),
	m_detuningLeft( 0.0f ),
	m_detuningRight( 0.0f ),
	m_phaseOffsetLeft( 0.0f ),
	m_phaseOffsetRight( 0.0f ),
	m_useWaveTable( true )
{
	// Connect knobs with Oscillators' inputs
	m_waveShapeModel.setValueLabels(Oscillator::waveShapeLabels());
	m_modulationAlgoModel.setValueLabels(Oscillator::modulationAlgoLabels());
	connect( &m_volumeModel, SIGNAL( dataChanged() ),
					this, SLOT( updateVolume() ), Qt::DirectConnection );
	connect( &m_panModel, SIGNAL( dataChanged() ),
					this, SLOT( updateVolume() ), Qt::DirectConnection );
	updateVolume();

	connect( &m_coarseModel, SIGNAL( dataChanged() ),
				this, SLOT( updateDetuningLeft() ), Qt::DirectConnection );
	connect( &m_coarseModel, SIGNAL( dataChanged() ),
				this, SLOT( updateDetuningRight() ), Qt::DirectConnection );
	connect( &m_fineLeftModel, SIGNAL( dataChanged() ),
				this, SLOT( updateDetuningLeft() ), Qt::DirectConnection );
	connect( &m_fineRightModel, SIGNAL( dataChanged() ),
				this, SLOT( updateDetuningRight() ), Qt::DirectConnection );
	updateDetuningLeft();
	updateDetuningRight();

	connect( &m_phaseOffsetModel, SIGNAL( dataChanged() ),
			this, SLOT( updatePhaseOffsetLeft() ), Qt::DirectConnection );
	connect( &m_phaseOffsetModel, SIGNAL( dataChanged() ),
			this, SLOT( updatePhaseOffsetRight() ), Qt::DirectConnection );
	connect( &m_stereoPhaseDetuningModel, SIGNAL( dataChanged() ),
			this, SLOT( updatePhaseOffsetLeft() ), Qt::DirectConnection );
	connect ( &m_useWaveTableModel, SIGNAL(dataChanged()),
			this, SLOT( updateUseWaveTable()));

	updatePhaseOffsetLeft();
	updatePhaseOffsetRight();

}

// One native instrument owns the three resource transitions. All rosters/backing
// are observed through weak actors and retained values after returned work.
class TripleOscillator::ResourceGuard
{
public:
	explicit ResourceGuard(TripleOscillator* owner, bool freezeControls = true) : m_owner(owner), m_freeze(freezeControls)
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| !owner || owner->thread() != QThread::currentThread() || !Engine::audioEngine() || !Engine::getSong() || !Engine::mixer())
		{ throw std::logic_error("TripleOscillator resource work requires the native application thread"); }
		if (owner->m_changingResources) { throw std::logic_error("TripleOscillator resources/settings cannot reenter"); }
		m_track = owner->instrumentTrack(); m_selected = m_track ? m_track->instrument() : nullptr; m_selection = m_selected.data();
		m_container = m_track ? m_track->trackContainer() : nullptr; m_engine = Engine::audioEngine(); m_song = Engine::getSong(); m_mixer = Engine::mixer();
		m_project = m_song->replacementGeneration(); m_routing = m_mixer->replacementGeneration(); m_filename = m_song->projectFileName();
		for (int i = 0; i < NUM_OF_OSCILLATORS; ++i)
		{
			m_oscillators[i] = owner->m_osc[i]; if (!m_oscillators[i]) { throw std::logic_error("TripleOscillator has no native oscillator"); }
			adopted(i); m_values[i] = values(m_oscillators[i].data());
		}
		m_captured = true; check(); m_changes.emplace(m_engine->requestChangesGuard()); owner->m_changingResources = true;
	}
	~ResourceGuard() { if (m_owner) { m_owner->m_changingResources = false; } }
	void actors() const
	{
		if (!m_owner || m_owner->thread() != QThread::currentThread() || !m_track || m_track->isRetiring() || m_track->thread() != QThread::currentThread()
			|| !m_owner->isFromTrack(m_track.data()) || m_selected.data() != m_selection || m_track->instrument() != m_selection
			|| !m_container || m_track->trackContainer() != m_container.data() || std::find(m_container->tracks().begin(), m_container->tracks().end(), m_track.data()) == m_container->tracks().end()
			|| !m_engine || Engine::audioEngine() != m_engine.data() || !m_song || Engine::getSong() != m_song.data() || !m_mixer || Engine::mixer() != m_mixer.data()
			|| m_song->replacementGeneration() != m_project || m_mixer->replacementGeneration() != m_routing || m_song->projectFileName() != m_filename)
		{ throw std::runtime_error("TripleOscillator native resource context changed"); }
		for (int i = 0; i < NUM_OF_OSCILLATORS; ++i)
		{ if (!m_oscillators[i] || m_owner->m_osc[i] != m_oscillators[i].data() || m_oscillators[i]->parent() != m_owner.data() || m_oscillators[i]->thread() != QThread::currentThread()) { throw std::runtime_error("Native oscillator retired or changed owner"); } }
	}
	void check() const
	{
		actors(); for (int i = 0; i < NUM_OF_OSCILLATORS; ++i)
		{ const auto oscillator = m_oscillators[i].data(); if (oscillator->m_sampleBuffer != m_buffers[i] || oscillator->m_userAntiAliasWaveTable != m_tables[i] || oscillator->m_unresolvedWave != m_missing[i] || (m_freeze && values(oscillator) != m_values[i])) { throw std::runtime_error("Native oscillator state changed during resource work"); } }
	}
	void poll() const { check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } check(); }
	void adopted(int index)
	{
		// Constructor fills the roster incrementally; no virtual/cooperative work here.
		const auto oscillator = m_oscillators[index].data(); const auto buffer = oscillator->m_sampleBuffer; const auto table = oscillator->m_userAntiAliasWaveTable; const QString missing = oscillator->m_unresolvedWave;
		m_buffers[index] = buffer; m_tables[index] = table; m_missing[index] = missing; if (m_captured) { check(); }
	}
	void freezeControls() { check(); for (int i = 0; i < NUM_OF_OSCILLATORS; ++i) { m_values[i] = values(m_oscillators[i].data()); } m_freeze = true; }
	void thawControls() { check(); m_freeze = false; }
	int index(const OscillatorObject* oscillator) const
	{ actors(); for (int i = 0; i < NUM_OF_OSCILLATORS; ++i) { if (m_oscillators[i].data() == oscillator) { return i; } } throw std::invalid_argument("Unknown native oscillator"); }
private:
	static std::array<float, 10> values(const OscillatorObject* o)
	{ return {o->m_volumeModel.storedValue(), o->m_panModel.storedValue(), o->m_coarseModel.storedValue(), o->m_fineLeftModel.storedValue(), o->m_fineRightModel.storedValue(), o->m_phaseOffsetModel.storedValue(), o->m_stereoPhaseDetuningModel.storedValue(), o->m_waveShapeModel.storedValue(), o->m_modulationAlgoModel.storedValue(), o->m_useWaveTableModel.storedValue()}; }
	QPointer<TripleOscillator> m_owner; QPointer<InstrumentTrack> m_track; QPointer<Instrument> m_selected; Instrument* m_selection = nullptr; QPointer<TrackContainer> m_container;
	QPointer<AudioEngine> m_engine; QPointer<Song> m_song; QPointer<Mixer> m_mixer; std::uint64_t m_project = 0, m_routing = 0; QString m_filename;
	std::array<QPointer<OscillatorObject>, NUM_OF_OSCILLATORS> m_oscillators;
	std::array<std::shared_ptr<const SampleBuffer>, NUM_OF_OSCILLATORS> m_buffers;
	std::array<std::shared_ptr<const OscillatorConstants::waveform_t>, NUM_OF_OSCILLATORS> m_tables;
	std::array<QString, NUM_OF_OSCILLATORS> m_missing; std::array<std::array<float, 10>, NUM_OF_OSCILLATORS> m_values{}; bool m_freeze, m_captured = false;
	std::optional<AudioEngine::RequestChangesGuard> m_changes; FileResourceIO::EventSuspension m_events;
};

void OscillatorObject::oscUserDefWaveDblClick()
{
	const QPointer<OscillatorObject> owner(this); const QPointer<TripleOscillator> instrument(dynamic_cast<TripleOscillator*>(parent()));
	const QPointer<Song> song(Engine::getSong()); const auto generation = song ? song->replacementGeneration() : 0;
	const QPointer<InstrumentTrack> track(instrument ? instrument->instrumentTrack() : nullptr);
	const auto path = gui::FileDialog::openWaveformFile();
	if (path.isEmpty() || !owner || !instrument || owner->parent() != instrument.data() || !track || track->isRetiring() || track->instrument() != instrument.data() || !song || Engine::getSong() != song.data() || song->replacementGeneration() != generation) { return; }
	try { QString error; if (!owner->setUserWave(path, error) && song && Engine::getSong() == song.data() && song->replacementGeneration() == generation) { song->collectError(error); } }
	catch (const std::exception& error) { if (song && Engine::getSong() == song.data() && song->replacementGeneration() == generation) { song->collectError(QString::fromUtf8(error.what())); } }
	catch (...) { if (song && Engine::getSong() == song.data() && song->replacementGeneration() == generation) { song->collectError("Native oscillator waveform loading failed"); } }
}

bool OscillatorObject::setUserWave(const QString& path, QString& error)
{
	const QString requested(path); auto instrument = dynamic_cast<TripleOscillator*>(parent());
	if (!instrument) { throw std::logic_error("A native TripleOscillator must own the waveform"); }
	TripleOscillator::ResourceGuard work(instrument); return instrument->setUserWaveImpl(this, requested, error, work);
}

bool TripleOscillator::setUserWaveImpl(OscillatorObject* oscillator, const QString& path, QString& error, ResourceGuard& work)
{
	const QString requested(path); const int index = work.index(oscillator); work.poll();
	const auto buffer = SampleBuffer::fromFile(requested); work.check();
	if (!buffer || (!requested.isEmpty() && buffer->empty()))
	{ error = tr("The oscillator waveform could not be decoded: %1").arg(requested); return false; }
	std::shared_ptr<const OscillatorConstants::waveform_t> table;
	work.poll(); if (!buffer->empty()) { table = Oscillator::generateAntiAliasUserWaveTable(buffer.get()); } work.poll();
	oscillator->m_sampleBuffer = buffer; oscillator->m_userAntiAliasWaveTable = std::move(table); oscillator->m_unresolvedWave.clear(); work.adopted(index);
	error.clear(); emit oscillator->dataChanged(); work.check(); return true;
}


void OscillatorObject::updateVolume()
{
	if( m_panModel.value() >= 0.0f )
	{
		const float panningFactorLeft = 1.0f - m_panModel.value()
							/ (float)PanningRight;
		m_volumeLeft = panningFactorLeft * m_volumeModel.value() /
									100.0f;
		m_volumeRight = m_volumeModel.value() / 100.0f;
	}
	else
	{
		m_volumeLeft = m_volumeModel.value() / 100.0f;
		const float panningFactorRight = 1.0f + m_panModel.value()
							/ (float)PanningRight;
		m_volumeRight = panningFactorRight * m_volumeModel.value() /
									100.0f;
	}
}




void OscillatorObject::updateDetuningLeft()
{
	m_detuningLeft = std::exp2((m_coarseModel.value() * 100.0f + m_fineLeftModel.value()) / 1200.0f)
		/ Engine::audioEngine()->outputSampleRate();
}




void OscillatorObject::updateDetuningRight()
{
	m_detuningRight = std::exp2((m_coarseModel.value() * 100.0f + m_fineRightModel.value()) / 1200.0f)
		/ Engine::audioEngine()->outputSampleRate();
}




void OscillatorObject::updatePhaseOffsetLeft()
{
	m_phaseOffsetLeft = ( m_phaseOffsetModel.value() +
				m_stereoPhaseDetuningModel.value() ) / 360.0f;
}




void OscillatorObject::updatePhaseOffsetRight()
{
	m_phaseOffsetRight = m_phaseOffsetModel.value() / 360.0f;
}

void OscillatorObject::updateUseWaveTable()
{
	m_useWaveTable = m_useWaveTableModel.value();
}




TripleOscillator::TripleOscillator( InstrumentTrack * _instrument_track ) :
	Instrument( _instrument_track, &tripleoscillator_plugin_descriptor )
{
	for( int i = 0; i < NUM_OF_OSCILLATORS; ++i )
	{
		m_osc[i] = new OscillatorObject( this, i );

	}

	connect( Engine::audioEngine(), SIGNAL( sampleRateChanged() ),
			this, SLOT( updateAllDetuning() ) );
}




void TripleOscillator::saveSettings(QDomDocument& document, QDomElement& element)
{
	ResourceGuard work(this);
	for (int i = 0; i < NUM_OF_OSCILLATORS; ++i)
	{
		work.poll(); const auto oscillator = m_osc[i]; const auto buffer = oscillator->m_sampleBuffer; const QString missing = oscillator->m_unresolvedWave;
		oscillator->m_volumeModel.saveSettings(document, element, "vol" + QString::number(i)); work.check();
		oscillator->m_panModel.saveSettings(document, element, "pan" + QString::number(i)); work.check();
		oscillator->m_coarseModel.saveSettings(document, element, "coarse" + QString::number(i)); work.check();
		oscillator->m_fineLeftModel.saveSettings(document, element, "finel" + QString::number(i)); work.check();
		oscillator->m_fineRightModel.saveSettings(document, element, "finer" + QString::number(i)); work.check();
		oscillator->m_phaseOffsetModel.saveSettings(document, element, "phoffset" + QString::number(i)); work.check();
		oscillator->m_stereoPhaseDetuningModel.saveSettings(document, element, "stphdetun" + QString::number(i)); work.check();
		oscillator->m_waveShapeModel.saveSettings(document, element, "wavetype" + QString::number(i)); work.check();
		oscillator->m_modulationAlgoModel.saveSettings(document, element, "modalgo" + QString::number(i + 1)); work.check();
		oscillator->m_useWaveTableModel.saveSettings(document, element, "useWaveTable" + QString::number(i + 1)); work.check();
		const auto attribute = "userwavefile" + QString::number(i), marker = "userwaveunresolved" + QString::number(i);
		element.setAttribute(attribute, missing.isEmpty() ? buffer->audioFile() : missing);
		if (!missing.isEmpty()) { element.setAttribute(marker, 1); }
		else { element.removeAttribute(marker); buffer->registerResourceSource(document, element, attribute, buffer); work.check(); }
	}
}

void TripleOscillator::loadSettings(const QDomElement& element)
{
	const QDomElement settings(element); ResourceGuard work(this, false);
	for (int i = 0; i < NUM_OF_OSCILLATORS; ++i)
	{
		work.poll(); const auto oscillator = m_osc[i]; const auto attribute = "userwavefile" + QString::number(i);
		const QString filename = settings.attribute(attribute); const bool pending = settings.attribute("userwaveunresolved" + QString::number(i)) == "1";
		DataFile::ResourceReferenceScope resource(settings, attribute); work.check();
		if (filename.contains(QChar::Null) || (pending && filename.isEmpty())) { throw std::invalid_argument("An unresolved oscillator wave requires one valid filename"); }
		QString absolute; bool absent = false;
		if (!filename.isEmpty())
		{
			bool pathError = false; absolute = PathUtil::toAbsolute(filename, &pathError);
			if (pathError || absolute.isEmpty()) { throw std::invalid_argument("Invalid native oscillator waveform source path"); }
			if (pending && FileResourceSnapshot::hasRetainedSource(absolute)) { throw std::invalid_argument("An unresolved oscillator waveform cannot discard an accepted resource"); }
			absent = !pending && !QFileInfo(FileResourceSnapshot::readableSourcePath(absolute)).exists(); work.check();
		}
		oscillator->m_volumeModel.loadSettings(settings, "vol" + QString::number(i)); work.check();
		oscillator->m_panModel.loadSettings(settings, "pan" + QString::number(i)); work.check();
		oscillator->m_coarseModel.loadSettings(settings, "coarse" + QString::number(i)); work.check();
		oscillator->m_fineLeftModel.loadSettings(settings, "finel" + QString::number(i)); work.check();
		oscillator->m_fineRightModel.loadSettings(settings, "finer" + QString::number(i)); work.check();
		oscillator->m_phaseOffsetModel.loadSettings(settings, "phoffset" + QString::number(i)); work.check();
		oscillator->m_stereoPhaseDetuningModel.loadSettings(settings, "stphdetun" + QString::number(i)); work.check();
		oscillator->m_waveShapeModel.loadSettings(settings, "wavetype" + QString::number(i)); work.check();
		oscillator->m_modulationAlgoModel.loadSettings(settings, "modalgo" + QString::number(i + 1)); work.check();
		oscillator->m_useWaveTableModel.loadSettings(settings, "useWaveTable" + QString::number(i + 1)); work.check();
		work.freezeControls();
		if (pending || absent)
		{
			oscillator->m_sampleBuffer = SampleBuffer::emptyBuffer(); oscillator->m_userAntiAliasWaveTable.reset(); oscillator->m_unresolvedWave = absolute; work.adopted(i);
			Engine::getSong()->collectUnresolvedResource(absolute); work.check();
		}
		else { QString error; if (!setUserWaveImpl(oscillator, filename, error, work)) { Engine::getSong()->collectError(error); work.check(); } }
		work.thawControls();
	}
}

std::vector<Plugin::FileResource> TripleOscillator::fileResources() const
{
	std::vector<FileResource> result;
	for (int i = 0; i < NUM_OF_OSCILLATORS; ++i)
	{
		result.push_back({QString("oscillator%1").arg(i + 1), tr("Oscillator %1 waveform").arg(i + 1),
			m_osc[i]->userWaveFile(), {}, &m_osc[i]->m_waveShapeModel,
			static_cast<float>(Oscillator::WaveShape::UserDefined), true, m_osc[i]->m_sampleBuffer, m_osc[i]->userWaveUnresolved()});
	}
	return result;
}

bool TripleOscillator::setFileResource(const QString& key, const QString& path, QString& error)
{
	const QString requestedKey(key), requestedPath(path); ResourceGuard work(this);
	for (int i = 0; i < NUM_OF_OSCILLATORS; ++i)
	{ if (requestedKey == QString("oscillator%1").arg(i + 1)) { return setUserWaveImpl(m_osc[i], requestedPath, error, work); } }
	error = tr("Unknown TripleOscillator resource: %1").arg(requestedKey); return false;
}

QString TripleOscillator::nodeName() const
{
	return( tripleoscillator_plugin_descriptor.name );
}




void TripleOscillator::playNote( NotePlayHandle * _n,
						SampleFrame* _working_buffer )
{
	if (!_n->m_pluginData)
	{
		auto oscs_l = std::array<Oscillator*, NUM_OF_OSCILLATORS>{};
		auto oscs_r = std::array<Oscillator*, NUM_OF_OSCILLATORS>{};

		for( int i = NUM_OF_OSCILLATORS - 1; i >= 0; --i )
		{

			// the last oscs needs no sub-oscs...
			if( i == NUM_OF_OSCILLATORS - 1 )
			{
				oscs_l[i] = new Oscillator(
						&m_osc[i]->m_waveShapeModel,
						&m_osc[i]->m_modulationAlgoModel,
						_n->frequency(),
						m_osc[i]->m_detuningLeft,
						m_osc[i]->m_phaseOffsetLeft,
						m_osc[i]->m_volumeLeft );
				oscs_l[i]->setUseWaveTable(m_osc[i]->m_useWaveTable);
				oscs_r[i] = new Oscillator(
						&m_osc[i]->m_waveShapeModel,
						&m_osc[i]->m_modulationAlgoModel,
						_n->frequency(),
						m_osc[i]->m_detuningRight,
						m_osc[i]->m_phaseOffsetRight,
						m_osc[i]->m_volumeRight );
				oscs_r[i]->setUseWaveTable(m_osc[i]->m_useWaveTable);
			}
			else
			{
				oscs_l[i] = new Oscillator(
						&m_osc[i]->m_waveShapeModel,
						&m_osc[i]->m_modulationAlgoModel,
						_n->frequency(),
						m_osc[i]->m_detuningLeft,
						m_osc[i]->m_phaseOffsetLeft,
						m_osc[i]->m_volumeLeft,
						oscs_l[i + 1] );
				oscs_l[i]->setUseWaveTable(m_osc[i]->m_useWaveTable);
				oscs_r[i] = new Oscillator(
						&m_osc[i]->m_waveShapeModel,
						&m_osc[i]->m_modulationAlgoModel,
						_n->frequency(),
						m_osc[i]->m_detuningRight,
						m_osc[i]->m_phaseOffsetRight,
						m_osc[i]->m_volumeRight,
						oscs_r[i + 1] );
				oscs_r[i]->setUseWaveTable(m_osc[i]->m_useWaveTable);
			}

			oscs_l[i]->setUserWave( m_osc[i]->m_sampleBuffer );
			oscs_r[i]->setUserWave( m_osc[i]->m_sampleBuffer );
			oscs_l[i]->setUserAntiAliasWaveTable(m_osc[i]->m_userAntiAliasWaveTable);
			oscs_r[i]->setUserAntiAliasWaveTable(m_osc[i]->m_userAntiAliasWaveTable);
		}

		_n->m_pluginData = new oscPtr;
		static_cast<oscPtr *>( _n->m_pluginData )->oscLeft = oscs_l[0];
		static_cast< oscPtr *>( _n->m_pluginData )->oscRight =
								oscs_r[0];
	}

	Oscillator * osc_l = static_cast<oscPtr *>( _n->m_pluginData )->oscLeft;
	Oscillator * osc_r = static_cast<oscPtr *>( _n->m_pluginData )->oscRight;

	const f_cnt_t frames = _n->framesLeftForCurrentPeriod();
	const f_cnt_t offset = _n->noteOffset();

	osc_l->update( _working_buffer + offset, frames, 0 );
	osc_r->update( _working_buffer + offset, frames, 1 );

	applyFadeIn(_working_buffer, _n);
	applyRelease( _working_buffer, _n );
}




void TripleOscillator::deleteNotePluginData( NotePlayHandle * _n )
{
	delete static_cast<Oscillator *>( static_cast<oscPtr *>(
						_n->m_pluginData )->oscLeft );
	delete static_cast<Oscillator *>( static_cast<oscPtr *>(
						_n->m_pluginData )->oscRight );
	delete static_cast<oscPtr *>( _n->m_pluginData );
}




gui::PluginView* TripleOscillator::instantiateView( QWidget * _parent )
{
	return new gui::TripleOscillatorView( this, _parent );
}




void TripleOscillator::updateAllDetuning()
{
	for (const auto& osc : m_osc)
	{
		osc->updateDetuningLeft();
		osc->updateDetuningRight();
	}
}



namespace gui
{


class TripleOscKnob : public Knob
{
public:
	TripleOscKnob( QWidget * _parent ) :
			Knob( KnobType::Styled, _parent )
	{
		setFixedSize( 28, 35 );
	}
};

// 82, 109


TripleOscillatorView::TripleOscillatorView( Instrument * _instrument,
							QWidget * _parent ) :
	InstrumentViewFixedSize( _instrument, _parent )
{
	setAutoFillBackground( true );
	QPalette pal;
	pal.setBrush( backgroundRole(),
				PLUGIN_NAME::getIconPixmap( "artwork" ) );
	setPalette( pal );

	const int mod_x = 66;
	const int mod1_y = 58;
	const int mod2_y = 75;
	const int osc_y = 109;
	const int osc_h = 52;

	// TODO: clean rewrite using layouts and all that...
	auto pm_osc1_btn = new PixmapButton(this, nullptr);
	pm_osc1_btn->move( mod_x, mod1_y );
	pm_osc1_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"pm_active" ) );
	pm_osc1_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"pm_inactive" ) );
	pm_osc1_btn->setToolTip(tr("Modulate phase of oscillator 1 by oscillator 2"));

	auto am_osc1_btn = new PixmapButton(this, nullptr);
	am_osc1_btn->move( mod_x + 35, mod1_y );
	am_osc1_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"am_active" ) );
	am_osc1_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"am_inactive" ) );
	am_osc1_btn->setToolTip(tr("Modulate amplitude of oscillator 1 by oscillator 2"));

	auto mix_osc1_btn = new PixmapButton(this, nullptr);
	mix_osc1_btn->move( mod_x + 70, mod1_y );
	mix_osc1_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"mix_active" ) );
	mix_osc1_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"mix_inactive" ) );
	mix_osc1_btn->setToolTip(tr("Mix output of oscillators 1 & 2"));

	auto sync_osc1_btn = new PixmapButton(this, nullptr);
	sync_osc1_btn->move( mod_x + 105, mod1_y );
	sync_osc1_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"sync_active" ) );
	sync_osc1_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"sync_inactive" ) );
	sync_osc1_btn->setToolTip(tr("Synchronize oscillator 1 with "
							"oscillator 2" ) );

	auto fm_osc1_btn = new PixmapButton(this, nullptr);
	fm_osc1_btn->move( mod_x + 140, mod1_y );
	fm_osc1_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"fm_active" ) );
	fm_osc1_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"fm_inactive" ) );
	fm_osc1_btn->setToolTip(tr("Modulate frequency of oscillator 1 by oscillator 2"));

	m_mod1BtnGrp = new AutomatableButtonGroup( this );
	m_mod1BtnGrp->addButton( pm_osc1_btn );
	m_mod1BtnGrp->addButton( am_osc1_btn );
	m_mod1BtnGrp->addButton( mix_osc1_btn );
	m_mod1BtnGrp->addButton( sync_osc1_btn );
	m_mod1BtnGrp->addButton( fm_osc1_btn );

	auto pm_osc2_btn = new PixmapButton(this, nullptr);
	pm_osc2_btn->move( mod_x, mod2_y );
	pm_osc2_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"pm_active" ) );
	pm_osc2_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"pm_inactive" ) );
	pm_osc2_btn->setToolTip(tr("Modulate phase of oscillator 2 by oscillator 3"));

	auto am_osc2_btn = new PixmapButton(this, nullptr);
	am_osc2_btn->move( mod_x + 35, mod2_y );
	am_osc2_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"am_active" ) );
	am_osc2_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"am_inactive" ) );
	am_osc2_btn->setToolTip(tr("Modulate amplitude of oscillator 2 by oscillator 3"));

	auto mix_osc2_btn = new PixmapButton(this, nullptr);
	mix_osc2_btn->move( mod_x + 70, mod2_y );
	mix_osc2_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"mix_active" ) );
	mix_osc2_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"mix_inactive" ) );
	mix_osc2_btn->setToolTip(tr("Mix output of oscillators 2 & 3"));

	auto sync_osc2_btn = new PixmapButton(this, nullptr);
	sync_osc2_btn->move( mod_x + 105, mod2_y );
	sync_osc2_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"sync_active" ) );
	sync_osc2_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"sync_inactive" ) );
	sync_osc2_btn->setToolTip(tr("Synchronize oscillator 2 with oscillator 3"));

	auto fm_osc2_btn = new PixmapButton(this, nullptr);
	fm_osc2_btn->move( mod_x + 140, mod2_y );
	fm_osc2_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"fm_active" ) );
	fm_osc2_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"fm_inactive" ) );
	fm_osc2_btn->setToolTip(tr("Modulate frequency of oscillator 2 by oscillator 3"));

	m_mod2BtnGrp = new AutomatableButtonGroup( this );

	m_mod2BtnGrp->addButton( pm_osc2_btn );
	m_mod2BtnGrp->addButton( am_osc2_btn );
	m_mod2BtnGrp->addButton( mix_osc2_btn );
	m_mod2BtnGrp->addButton( sync_osc2_btn );
	m_mod2BtnGrp->addButton( fm_osc2_btn );


	for( int i = 0; i < NUM_OF_OSCILLATORS; ++i )
	{
		int knob_y = osc_y + i * osc_h;

		// setup volume-knob
		auto vk = new VolumeKnob(KnobType::Styled, this);
		vk->setFixedSize( 28, 35 );
		vk->move( 6, knob_y );
		vk->setHintText( tr( "Osc %1 volume:" ).arg(
							 i+1 ), "%" );

		// setup panning-knob
		Knob * pk = new TripleOscKnob( this );
		pk->move( 35, knob_y );
		pk->setHintText( tr("Osc %1 panning:").arg( i + 1 ), "" );

		// setup coarse-knob
		Knob * ck = new TripleOscKnob( this );
		ck->move( 82, knob_y );
		ck->setHintText( tr( "Osc %1 coarse detuning:" ).arg( i + 1 )
						 , " " + tr( "semitones" ) );

		// setup knob for left fine-detuning
		Knob * flk = new TripleOscKnob( this );
		flk->move( 111, knob_y );
		flk->setHintText( tr( "Osc %1 fine detuning left:" ).
						  arg( i + 1 ),
							" " + tr( "cents" ) );

		// setup knob for right fine-detuning
		Knob * frk = new TripleOscKnob( this );
		frk->move( 140, knob_y );
		frk->setHintText( tr( "Osc %1 fine detuning right:" ).
						  arg( i + 1 ),
							" " + tr( "cents" ) );

		// setup phase-offset-knob
		Knob * pok = new TripleOscKnob( this );
		pok->move( 188, knob_y );
		pok->setHintText( tr( "Osc %1 phase-offset:" ).
						  arg( i + 1 ),
							" " + tr( "degrees" ) );

		// setup stereo-phase-detuning-knob
		Knob * spdk = new TripleOscKnob( this );
		spdk->move( 217, knob_y );
		spdk->setHintText( tr("Osc %1 stereo phase-detuning:" ).
						arg( i + 1 ),
							" " + tr( "degrees" ) );

		int btn_y = 96 + i * osc_h;

		auto sin_wave_btn = new PixmapButton(this, nullptr);
		sin_wave_btn->move( 128, btn_y );
		sin_wave_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"sin_shape_active" ) );
		sin_wave_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"sin_shape_inactive" ) );
		sin_wave_btn->setToolTip(
				tr( "Sine wave" ) );

		auto triangle_wave_btn = new PixmapButton(this, nullptr);
		triangle_wave_btn->move( 143, btn_y );
		triangle_wave_btn->setActiveGraphic(
			PLUGIN_NAME::getIconPixmap( "triangle_shape_active" ) );
		triangle_wave_btn->setInactiveGraphic(
			PLUGIN_NAME::getIconPixmap( "triangle_shape_inactive" ) );
		triangle_wave_btn->setToolTip(
				tr( "Triangle wave") );

		auto saw_wave_btn = new PixmapButton(this, nullptr);
		saw_wave_btn->move( 158, btn_y );
		saw_wave_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"saw_shape_active" ) );
		saw_wave_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"saw_shape_inactive" ) );
		saw_wave_btn->setToolTip(
				tr( "Saw wave" ) );

		auto sqr_wave_btn = new PixmapButton(this, nullptr);
		sqr_wave_btn->move( 173, btn_y );
		sqr_wave_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
						"square_shape_active" ) );
		sqr_wave_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
						"square_shape_inactive" ) );
		sqr_wave_btn->setToolTip(
				tr( "Square wave" ) );

		auto moog_saw_wave_btn = new PixmapButton(this, nullptr);
		moog_saw_wave_btn->move( 188, btn_y );
		moog_saw_wave_btn->setActiveGraphic(
			PLUGIN_NAME::getIconPixmap( "moog_saw_shape_active" ) );
		moog_saw_wave_btn->setInactiveGraphic(
			PLUGIN_NAME::getIconPixmap( "moog_saw_shape_inactive" ) );
		moog_saw_wave_btn->setToolTip(
				tr( "Moog-like saw wave" ) );

		auto exp_wave_btn = new PixmapButton(this, nullptr);
		exp_wave_btn->move( 203, btn_y );
		exp_wave_btn->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"exp_shape_active" ) );
		exp_wave_btn->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"exp_shape_inactive" ) );
		exp_wave_btn->setToolTip(
				tr( "Exponential wave" ) );

		auto white_noise_btn = new PixmapButton(this, nullptr);
		white_noise_btn->move( 218, btn_y );
		white_noise_btn->setActiveGraphic(
			PLUGIN_NAME::getIconPixmap( "white_noise_shape_active" ) );
		white_noise_btn->setInactiveGraphic(
			PLUGIN_NAME::getIconPixmap( "white_noise_shape_inactive" ) );
		white_noise_btn->setToolTip(
				tr( "White noise" ) );

		auto uwb = new PixmapButton(this, nullptr);
		uwb->move( 233, btn_y );
		uwb->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"usr_shape_active" ) );
		uwb->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"usr_shape_inactive" ) );
		uwb->setToolTip(tr("User-defined wave"));

		auto uwt = new PixmapButton(this, nullptr);
		uwt->move( 110, btn_y );
		uwt->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"wavetable_active" ) );
		uwt->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"wavetable_inactive" ) );
		uwt->setCheckable(true);
		uwt->setToolTip(tr("Use alias-free wavetable oscillators."));

		auto wsbg = new AutomatableButtonGroup(this);

		wsbg->addButton( sin_wave_btn );
		wsbg->addButton( triangle_wave_btn );
		wsbg->addButton( saw_wave_btn );
		wsbg->addButton( sqr_wave_btn );
		wsbg->addButton( moog_saw_wave_btn );
		wsbg->addButton( exp_wave_btn );
		wsbg->addButton( white_noise_btn );
		wsbg->addButton( uwb );


		m_oscKnobs[i] = OscillatorKnobs( vk, pk, ck, flk, frk, pok,
							spdk, uwb, wsbg, uwt );
	}
}




void TripleOscillatorView::modelChanged()
{
	auto t = castModel<TripleOscillator>();
	m_mod1BtnGrp->setModel( &t->m_osc[0]->m_modulationAlgoModel );
	m_mod2BtnGrp->setModel( &t->m_osc[1]->m_modulationAlgoModel );

	for( int i = 0; i < NUM_OF_OSCILLATORS; ++i )
	{
		m_oscKnobs[i].m_volKnob->setModel(
					&t->m_osc[i]->m_volumeModel );
		m_oscKnobs[i].m_panKnob->setModel(
					&t->m_osc[i]->m_panModel );
		m_oscKnobs[i].m_coarseKnob->setModel(
					&t->m_osc[i]->m_coarseModel );
		m_oscKnobs[i].m_fineLeftKnob->setModel(
					&t->m_osc[i]->m_fineLeftModel );
		m_oscKnobs[i].m_fineRightKnob->setModel(
					&t->m_osc[i]->m_fineRightModel );
		m_oscKnobs[i].m_phaseOffsetKnob->setModel(
					&t->m_osc[i]->m_phaseOffsetModel );
		m_oscKnobs[i].m_stereoPhaseDetuningKnob->setModel(
				&t->m_osc[i]->m_stereoPhaseDetuningModel );
		m_oscKnobs[i].m_waveShapeBtnGrp->setModel(
					&t->m_osc[i]->m_waveShapeModel );
		m_oscKnobs[i].m_multiBandWaveTableButton->setModel(
					&t->m_osc[i]->m_useWaveTableModel );

		m_oscKnobs[i].m_userWaveButton->setToolTip(t->m_osc[i]->userWaveFile());
		connect( m_oscKnobs[i].m_userWaveButton,
						SIGNAL( doubleClicked() ),
				t->m_osc[i], SLOT( oscUserDefWaveDblClick() ) );
	}
}


} // namespace gui


extern "C"
{

// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main( Model* model, void * )
{
	return new TripleOscillator( static_cast<InstrumentTrack *>( model ) );
}

}



} // namespace lmms
