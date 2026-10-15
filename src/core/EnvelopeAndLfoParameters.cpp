/*
 * EnvelopeAndLfoParameters.cpp - class EnvelopeAndLfoParameters
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

#include "EnvelopeAndLfoParameters.h"
#include <QPointer>
#include <QCoreApplication>
#include <QThread>
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include <QDomElement>
#include <QFileInfo>

#include "AudioEngine.h"
#include "Engine.h"
#include "DataFile.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "TrackContainer.h"
#include "Oscillator.h"
#include "PathUtil.h"
#include "Song.h"

namespace lmms
{

// how long should be each envelope-segment maximal (e.g. attack)?
extern const float SECS_PER_ENV_SEGMENT = 5.0f;
// how long should be one LFO-oscillation maximal?
extern const float SECS_PER_LFO_OSCILLATION = 20.0f;
// minimum number of frames for ENV/LFO stages that mustn't be '0'
const f_cnt_t minimumFrames = 1;


EnvelopeAndLfoParameters::LfoInstances * EnvelopeAndLfoParameters::s_lfoInstances = nullptr;


void EnvelopeAndLfoParameters::LfoInstances::trigger()
{
	QMutexLocker m( &m_lfoListMutex );
	for (const auto& lfo : m_lfos)
	{
		lfo->m_lfoFrame += Engine::audioEngine()->framesPerPeriod();
		lfo->m_bad_lfoShapeData = true;
	}
}




void EnvelopeAndLfoParameters::LfoInstances::reset()
{
	QMutexLocker m( &m_lfoListMutex );
	for (const auto& lfo : m_lfos)
	{
		lfo->m_lfoFrame = 0;
		lfo->m_bad_lfoShapeData = true;
	}
}




void EnvelopeAndLfoParameters::LfoInstances::add( EnvelopeAndLfoParameters * lfo )
{
	QMutexLocker m( &m_lfoListMutex );
	m_lfos.append( lfo );
}




void EnvelopeAndLfoParameters::LfoInstances::remove( EnvelopeAndLfoParameters * lfo )
{
	QMutexLocker m( &m_lfoListMutex );
	m_lfos.removeAll( lfo );
}




EnvelopeAndLfoParameters::EnvelopeAndLfoParameters(
					float _value_for_zero_amount,
							Model * _parent ) :
	Model( _parent ),
	m_used( false ),
	m_predelayModel(0.f, 0.f, 2.f, 0.001f, this, tr("Env pre-delay")),
	m_attackModel(0.f, 0.f, 2.f, 0.001f, this, tr("Env attack")),
	m_holdModel(0.5f, 0.f, 2.f, 0.001f, this, tr("Env hold")),
	m_decayModel(0.5f, 0.f, 2.f, 0.001f, this, tr("Env decay")),
	m_sustainModel(0.5f, 0.f, 1.f, 0.001f, this, tr("Env sustain")),
	m_releaseModel(0.1f, 0.f, 2.f, 0.001f, this, tr("Env release")),
	m_amountModel(0.f, -1.f, 1.f, 0.005f, this, tr("Env mod amount")),
	m_valueForZeroAmount( _value_for_zero_amount ),
	m_pahdFrames( 0 ),
	m_rFrames( 0 ),
	m_pahdEnv( nullptr ),
	m_rEnv( nullptr ),
	m_pahdBufSize( 0 ),
	m_rBufSize( 0 ),
	m_lfoPredelayModel(0.f, 0.f, 1.f, 0.001f, this, tr("LFO pre-delay")),
	m_lfoAttackModel(0.f, 0.f, 1.f, 0.001f, this, tr("LFO attack")),
	m_lfoSpeedModel(0.1f, 0.001f, 1.f, 0.0001f,
				SECS_PER_LFO_OSCILLATION * 1000.f, this,
							tr("LFO frequency")),
	m_lfoAmountModel(0.f, -1.f, 1.f, 0.005f, this, tr("LFO mod amount")),
	m_lfoWaveModel( static_cast<int>(LfoShape::SineWave), 0, NumLfoShapes, this, tr( "LFO wave shape" ) ),
	m_x100Model( false, this, tr( "LFO frequency x 100" ) ),
	m_controlEnvAmountModel( false, this, tr( "Modulate env amount" ) ),
	m_lfoFrame( 0 ),
	m_lfoAmountIsZero( false ),
	m_lfoShapeData(nullptr)
{
	m_amountModel.setCenterValue( 0 );
	m_lfoAmountModel.setCenterValue( 0 );

	if( s_lfoInstances == nullptr )
	{
		s_lfoInstances = new LfoInstances();
	}

	instances()->add( this );

	connect( &m_predelayModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_attackModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_holdModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_decayModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_sustainModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_releaseModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_amountModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );

	connect( &m_lfoPredelayModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_lfoAttackModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_lfoSpeedModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_lfoAmountModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_lfoWaveModel, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );
	connect( &m_x100Model, SIGNAL(dataChanged()),
			this, SLOT(updateSampleVars()), Qt::DirectConnection );

	connect( Engine::audioEngine(), SIGNAL(sampleRateChanged()),
				this, SLOT(updateSampleVars()));


	m_lfoShapeData =
		new sample_t[Engine::audioEngine()->framesPerPeriod()];

	updateSampleVars();
}




EnvelopeAndLfoParameters::~EnvelopeAndLfoParameters()
{
	m_predelayModel.disconnect( this );
	m_attackModel.disconnect( this );
	m_holdModel.disconnect( this );
	m_decayModel.disconnect( this );
	m_sustainModel.disconnect( this );
	m_releaseModel.disconnect( this );
	m_amountModel.disconnect( this );
	m_lfoPredelayModel.disconnect( this );
	m_lfoAttackModel.disconnect( this );
	m_lfoSpeedModel.disconnect( this );
	m_lfoAmountModel.disconnect( this );
	m_lfoWaveModel.disconnect( this );
	m_x100Model.disconnect( this );

	delete[] m_pahdEnv;
	delete[] m_rEnv;
	delete[] m_lfoShapeData;

	instances()->remove( this );

	if( instances()->isEmpty() )
	{
		delete instances();
		s_lfoInstances = nullptr;
	}
}




inline sample_t EnvelopeAndLfoParameters::lfoShapeSample( f_cnt_t _frame_offset )
{
	f_cnt_t frame = ( m_lfoFrame + _frame_offset ) % m_lfoOscillationFrames;
	const float phase = frame / static_cast<float>(
						m_lfoOscillationFrames );
	sample_t shape_sample;
	switch( static_cast<LfoShape>(m_lfoWaveModel.value())  )
	{
		case LfoShape::TriangleWave:
			shape_sample = Oscillator::triangleSample( phase );
			break;
		case LfoShape::SquareWave:
			shape_sample = Oscillator::squareSample( phase );
			break;
		case LfoShape::SawWave:
			shape_sample = Oscillator::sawSample( phase );
			break;
		case LfoShape::UserDefinedWave:
			shape_sample = Oscillator::userWaveSample(m_userWave.get(), phase);
			break;
		case LfoShape::RandomWave:
			if( frame == 0 )
			{
				m_random = Oscillator::noiseSample( 0.0f );
			}
			shape_sample = m_random;
			break;
		case LfoShape::SineWave:
		default:
			shape_sample = Oscillator::sinSample( phase );
			break;
	}
	return shape_sample * m_lfoAmount;
}




void EnvelopeAndLfoParameters::updateLfoShapeData()
{
	const f_cnt_t frames = Engine::audioEngine()->framesPerPeriod();
	for( f_cnt_t offset = 0; offset < frames; ++offset )
	{
		m_lfoShapeData[offset] = lfoShapeSample( offset );
	}
	m_bad_lfoShapeData = false;
}




inline void EnvelopeAndLfoParameters::fillLfoLevel( float * _buf,
							f_cnt_t _frame,
							const f_cnt_t _frames )
{
	if( m_lfoAmountIsZero || _frame <= m_lfoPredelayFrames )
	{
		for( f_cnt_t offset = 0; offset < _frames; ++offset )
		{
			*_buf++ = 0.0f;
		}
		return;
	}
	_frame -= m_lfoPredelayFrames;

	if( m_bad_lfoShapeData )
	{
		updateLfoShapeData();
	}

	f_cnt_t offset = 0;
	const float lafI = 1.0f / std::max(minimumFrames, m_lfoAttackFrames);
	for( ; offset < _frames && _frame < m_lfoAttackFrames; ++offset,
								++_frame )
	{
		*_buf++ = m_lfoShapeData[offset] * _frame * lafI;
	}
	for( ; offset < _frames; ++offset )
	{
		*_buf++ = m_lfoShapeData[offset];
	}
}




void EnvelopeAndLfoParameters::fillLevel( float * _buf, f_cnt_t _frame,
						const f_cnt_t _release_begin,
						const f_cnt_t _frames )
{
	QMutexLocker m(&m_paramMutex);

	fillLfoLevel( _buf, _frame, _frames );

	for( f_cnt_t offset = 0; offset < _frames; ++offset, ++_buf, ++_frame )
	{
		float env_level;
		if( _frame < _release_begin )
		{
			if( _frame < m_pahdFrames )
			{
				env_level = m_pahdEnv[_frame];
			}
			else
			{
				env_level = m_sustainLevel;
			}
		}
		else if( ( _frame - _release_begin ) < m_rFrames )
		{
			env_level = m_rEnv[_frame - _release_begin] *
				( ( _release_begin < m_pahdFrames ) ?
				m_pahdEnv[_release_begin] : m_sustainLevel );
		}
		else
		{
			env_level = 0.0f;
		}

		// at this point, *_buf is LFO level
		*_buf = m_controlEnvAmountModel.value() ?
			env_level * ( 0.5f + *_buf ) :
			env_level + *_buf;
	}
}




class EnvelopeAndLfoParameters::ResourceGuard
{
public:
	explicit ResourceGuard(EnvelopeAndLfoParameters* owner, bool freeze = true) : m_owner(owner), m_freeze(freeze)
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() || !owner || owner->thread() != QThread::currentThread()
			|| !Engine::audioEngine() || !Engine::getSong() || !Engine::mixer()) { throw std::logic_error("Envelope/LFO resource work requires the native application thread"); }
		if (owner->m_changingResource) { throw std::logic_error("Envelope/LFO settings and resource work cannot reenter"); }
		m_engine = Engine::audioEngine(); m_song = Engine::getSong(); m_mixer = Engine::mixer(); m_project = m_song->replacementGeneration(); m_routing = m_mixer->replacementGeneration(); m_filename = m_song->projectFileName();
		for (auto parent = owner->parent(); parent; parent = parent->parent()) { m_parents.emplace_back(parent); if (auto track = qobject_cast<InstrumentTrack*>(parent)) { m_track = track; } }
		m_container = m_track ? m_track->trackContainer() : nullptr; m_values = values(owner); adopted();
		m_changes.emplace(m_engine->requestChangesGuard()); owner->m_changingResource = true;
	}
	~ResourceGuard() { if (m_owner) { m_owner->m_changingResource = false; } }
	void actors() const
	{
		if (!m_owner || m_owner->thread() != QThread::currentThread() || !m_engine || Engine::audioEngine() != m_engine.data() || !m_song || Engine::getSong() != m_song.data()
			|| !m_mixer || Engine::mixer() != m_mixer.data() || m_song->replacementGeneration() != m_project || m_mixer->replacementGeneration() != m_routing || m_song->projectFileName() != m_filename)
		{ throw std::runtime_error("Native envelope/LFO resource context changed"); }
		QObject* child = m_owner.data();
		for (const auto& parent : m_parents) { if (!parent || child->parent() != parent.data()) { throw std::runtime_error("Native envelope/LFO parent changed"); } child = parent.data(); }
		if (child->parent() || (m_track && (m_track->isRetiring() || !m_container || m_track->trackContainer() != m_container.data()
			|| std::find(m_container->tracks().begin(), m_container->tracks().end(), m_track.data()) == m_container->tracks().end()))) { throw std::runtime_error("Native envelope/LFO track changed"); }
	}
	void check() const
	{ actors(); if (m_owner->m_userWave != m_buffer || m_owner->m_unresolvedWave != m_missing || (m_freeze && values(m_owner.data()) != m_values)) { throw std::runtime_error("Native envelope/LFO resource or controls changed"); } }
	void poll() const { check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } check(); }
	void adopted()
	{ actors(); const auto buffer = m_owner->m_userWave; const QString missing = m_owner->m_unresolvedWave; m_buffer = buffer; m_missing = missing; check(); }
	void freezeControls() { check(); m_values = values(m_owner.data()); m_freeze = true; }
	void selectUserWave() { check(); m_values[7] = static_cast<int>(LfoShape::UserDefinedWave); m_owner->m_lfoWaveModel.setValue(static_cast<int>(LfoShape::UserDefinedWave)); check(); }
private:
	static std::array<float, 14> values(const EnvelopeAndLfoParameters* p)
	{ return {p->m_predelayModel.storedValue(), p->m_attackModel.storedValue(), p->m_holdModel.storedValue(), p->m_decayModel.storedValue(), p->m_sustainModel.storedValue(), p->m_releaseModel.storedValue(), p->m_amountModel.storedValue(), p->m_lfoWaveModel.storedValue(), p->m_lfoPredelayModel.storedValue(), p->m_lfoAttackModel.storedValue(), p->m_lfoSpeedModel.storedValue(), p->m_lfoAmountModel.storedValue(), p->m_x100Model.storedValue(), p->m_controlEnvAmountModel.storedValue()}; }
	QPointer<EnvelopeAndLfoParameters> m_owner; std::vector<QPointer<QObject>> m_parents; QPointer<InstrumentTrack> m_track; QPointer<TrackContainer> m_container;
	QPointer<AudioEngine> m_engine; QPointer<Song> m_song; QPointer<Mixer> m_mixer; std::uint64_t m_project = 0, m_routing = 0; QString m_filename, m_missing;
	std::shared_ptr<const SampleBuffer> m_buffer; std::array<float, 14> m_values; bool m_freeze;
	std::optional<AudioEngine::RequestChangesGuard> m_changes; FileResourceIO::EventSuspension m_events;
};

bool EnvelopeAndLfoParameters::setLfoUserWave(const QString& path, QString& error, bool select)
{
	const QString requested(path); ResourceGuard work(this);
	if (!setLfoUserWaveImpl(requested, error, select, work)) { return false; }
	Engine::getSong()->setModified(); work.check(); return true;
}

bool EnvelopeAndLfoParameters::setLfoUserWaveImpl(const QString& path, QString& error, bool select, ResourceGuard& work)
{
	const QString requested(path); if (requested.contains(QChar::Null)) { error = tr("Envelope/LFO waveform filename contains a null character"); return false; }
	work.poll(); const auto wave = SampleBuffer::fromFile(requested); work.check();
	if (!wave || (!requested.isEmpty() && wave->empty())) { error = tr("Envelope/LFO waveform could not be decoded: %1").arg(requested); return false; }
	m_userWave = wave; m_unresolvedWave.clear(); m_bad_lfoShapeData = true; work.adopted();
	if (select) { work.selectUserWave(); }
	error.clear(); emit dataChanged(); work.check(); return true;
}

void EnvelopeAndLfoParameters::saveSettings(QDomDocument& document, QDomElement& element)
{
	ResourceGuard work(this); const auto wave = m_userWave; const QString missing = m_unresolvedWave;
	m_predelayModel.saveSettings(document, element, "pdel"); work.check();
	m_attackModel.saveSettings(document, element, "att"); work.check();
	m_holdModel.saveSettings(document, element, "hold"); work.check();
	m_decayModel.saveSettings(document, element, "dec"); work.check();
	m_sustainModel.saveSettings(document, element, "sustain"); work.check();
	m_releaseModel.saveSettings(document, element, "rel"); work.check();
	m_amountModel.saveSettings(document, element, "amt"); work.check();
	m_lfoWaveModel.saveSettings(document, element, "lshp"); work.check();
	m_lfoPredelayModel.saveSettings(document, element, "lpdel"); work.check();
	m_lfoAttackModel.saveSettings(document, element, "latt"); work.check();
	m_lfoSpeedModel.saveSettings(document, element, "lspd"); work.check();
	m_lfoAmountModel.saveSettings(document, element, "lamt"); work.check();
	m_x100Model.saveSettings(document, element, "x100"); work.check();
	m_controlEnvAmountModel.saveSettings(document, element, "ctlenvamt"); work.check();
	element.setAttribute("userwavefile", missing.isEmpty() ? wave->audioFile() : missing);
	if (!missing.isEmpty()) { element.setAttribute("userwaveunresolved", 1); }
	else { element.removeAttribute("userwaveunresolved"); wave->registerResourceSource(document, element, "userwavefile", wave); work.check(); }
}

void EnvelopeAndLfoParameters::loadSettings(const QDomElement& element)
{
	const QDomElement settings(element); const QString filename = settings.attribute("userwavefile"); const bool pending = settings.attribute("userwaveunresolved") == "1";
	ResourceGuard work(this, false); work.poll(); DataFile::ResourceReferenceScope resource(settings, "userwavefile"); work.check();
	if (filename.contains(QChar::Null) || (pending && filename.isEmpty())) { throw std::invalid_argument("An unresolved envelope/LFO waveform requires one valid filename"); }
	QString absolute; bool absent = false;
	if (!filename.isEmpty())
	{
		bool pathError = false; absolute = PathUtil::toAbsolute(filename, &pathError);
		if (pathError || absolute.isEmpty()) { throw std::invalid_argument("Invalid envelope/LFO waveform source path"); }
		if (pending && FileResourceSnapshot::hasRetainedSource(absolute)) { throw std::invalid_argument("An unresolved envelope/LFO waveform cannot discard an accepted source"); }
		absent = !pending && !QFileInfo(FileResourceSnapshot::readableSourcePath(absolute)).exists(); work.check();
	}
	m_predelayModel.loadSettings(settings, "pdel"); work.check();
	m_attackModel.loadSettings(settings, "att"); work.check();
	m_holdModel.loadSettings(settings, "hold"); work.check();
	m_decayModel.loadSettings(settings, "dec"); work.check();
	m_sustainModel.loadSettings(settings, "sustain"); work.check();
	m_releaseModel.loadSettings(settings, "rel"); work.check();
	m_amountModel.loadSettings(settings, "amt"); work.check();
	m_lfoWaveModel.loadSettings(settings, "lshp"); work.check();
	m_lfoPredelayModel.loadSettings(settings, "lpdel"); work.check();
	m_lfoAttackModel.loadSettings(settings, "latt"); work.check();
	m_lfoSpeedModel.loadSettings(settings, "lspd"); work.check();
	m_lfoAmountModel.loadSettings(settings, "lamt"); work.check();
	m_x100Model.loadSettings(settings, "x100"); work.check();
	m_controlEnvAmountModel.loadSettings(settings, "ctlenvamt"); work.check();
	// Native 4.15 projects used reversed sustain.
	if (settings.hasAttribute("sus")) { m_sustainModel.loadSettings(settings, "sus"); work.check(); m_sustainModel.setValue(1.0 - m_sustainModel.value()); work.check(); }
	work.freezeControls();
	if (pending || absent)
	{
		m_userWave = SampleBuffer::emptyBuffer(); m_unresolvedWave = absolute; m_bad_lfoShapeData = true; work.adopted();
		Engine::getSong()->collectUnresolvedResource(absolute); work.check();
	}
	else { QString error; if (!setLfoUserWaveImpl(filename, error, false, work)) { Engine::getSong()->collectError(error); work.check(); } }
	updateSampleVars(); work.check();
}


void EnvelopeAndLfoParameters::updateSampleVars()
{
	QMutexLocker m(&m_paramMutex);

	const float frames_per_env_seg = SECS_PER_ENV_SEGMENT *
				Engine::audioEngine()->outputSampleRate();

	// TODO: Remove the expKnobVals, time should be linear
	const auto predelay_frames = static_cast<f_cnt_t>(frames_per_env_seg * expKnobVal(m_predelayModel.value()));

	const f_cnt_t attack_frames = std::max(minimumFrames,
					static_cast<f_cnt_t>(frames_per_env_seg *
					expKnobVal(m_attackModel.value())));

	const auto hold_frames = static_cast<f_cnt_t>(frames_per_env_seg * expKnobVal(m_holdModel.value()));

	const f_cnt_t decay_frames = std::max(minimumFrames,
					static_cast<f_cnt_t>(frames_per_env_seg *
					expKnobVal(m_decayModel.value() *
					(1 - m_sustainModel.value()))));

	m_sustainLevel = m_sustainModel.value();
	m_amount = m_amountModel.value();
	if( m_amount >= 0 )
	{
		m_amountAdd = ( 1.0f - m_amount ) * m_valueForZeroAmount;
	}
	else
	{
		m_amountAdd = m_valueForZeroAmount;
	}

	m_pahdFrames = predelay_frames + attack_frames + hold_frames +
								decay_frames;
	m_rFrames = static_cast<f_cnt_t>( frames_per_env_seg *
					expKnobVal( m_releaseModel.value() ) );
	m_rFrames = std::max(minimumFrames, m_rFrames);

	if( static_cast<int>( floorf( m_amount * 1000.0f ) ) == 0 )
	{
		m_rFrames = minimumFrames;
	}

	// if the buffers are too small, make bigger ones - so we only alloc new memory when necessary
	if( m_pahdBufSize < m_pahdFrames )
	{
		sample_t * tmp = m_pahdEnv;
		m_pahdEnv = new sample_t[m_pahdFrames];
		delete[] tmp;
		m_pahdBufSize = m_pahdFrames;
	}
	if( m_rBufSize < m_rFrames )
	{
		sample_t * tmp = m_rEnv;
		m_rEnv = new sample_t[m_rFrames];
		delete[] tmp;
		m_rBufSize = m_rFrames;
	}

	const float aa = m_amountAdd;
	for( f_cnt_t i = 0; i < predelay_frames; ++i )
	{
		m_pahdEnv[i] = aa;
	}

	f_cnt_t add = predelay_frames;

	const float afI = ( 1.0f / attack_frames ) * m_amount;
	for( f_cnt_t i = 0; i < attack_frames; ++i )
	{
		m_pahdEnv[add+i] = i * afI + aa;
	}

	add += attack_frames;
	const float amsum = m_amount + m_amountAdd;
	for( f_cnt_t i = 0; i < hold_frames; ++i )
	{
		m_pahdEnv[add + i] = amsum;
	}

	add += hold_frames;
	const float dfI = ( 1.0 / decay_frames ) * ( m_sustainLevel -1 ) * m_amount;
	for( f_cnt_t i = 0; i < decay_frames; ++i )
	{
/*
		m_pahdEnv[add + i] = ( m_sustainLevel + ( 1.0f -
						(float)i / decay_frames ) *
						( 1.0f - m_sustainLevel ) ) *
							m_amount + m_amountAdd;
*/
		m_pahdEnv[add + i] = amsum + i*dfI;
	}

	const float rfI = ( 1.0f / m_rFrames ) * m_amount;
	for( f_cnt_t i = 0; i < m_rFrames; ++i )
	{
		m_rEnv[i] = (float)( m_rFrames - i ) * rfI;
	}

	// save this calculation in real-time-part
	m_sustainLevel = m_sustainLevel * m_amount + m_amountAdd;


	const float frames_per_lfo_oscillation = SECS_PER_LFO_OSCILLATION *
				Engine::audioEngine()->outputSampleRate();
	m_lfoPredelayFrames = static_cast<f_cnt_t>( frames_per_lfo_oscillation *
				expKnobVal( m_lfoPredelayModel.value() ) );
	m_lfoAttackFrames = static_cast<f_cnt_t>( frames_per_lfo_oscillation *
				expKnobVal( m_lfoAttackModel.value() ) );
	m_lfoOscillationFrames = static_cast<f_cnt_t>(
						frames_per_lfo_oscillation *
						m_lfoSpeedModel.value() );
	if( m_x100Model.value() )
	{
		m_lfoOscillationFrames /= 100;
	}
	m_lfoAmount = m_lfoAmountModel.value() * 0.5f;

	m_used = true;
	if( static_cast<int>( floorf( m_lfoAmount * 1000.0f ) ) == 0 )
	{
		m_lfoAmountIsZero = true;
		if( static_cast<int>( floorf( m_amount * 1000.0f ) ) == 0 )
		{
			m_used = false;
		}
	}
	else
	{
		m_lfoAmountIsZero = false;
	}

	m_bad_lfoShapeData = true;

	m.unlock();
	emit dataChanged();

}







} // namespace lmms
