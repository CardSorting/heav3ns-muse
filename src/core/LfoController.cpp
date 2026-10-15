/*
 * LfoController.cpp - implementation of class controller which handles
 *                      remote-control of AutomatableModels
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail.com>
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

#include "LfoController.h"

#include <QDomElement>
#include <QPointer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>
#include <QDebug>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>

#include "AudioEngine.h"
#include "NativeValueNotification.h"
#include "DataFile.h"
#include "Engine.h"
#include "Oscillator.h"
#include "Song.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "Mixer.h"
#include "PathUtil.h"

namespace lmms
{


LfoController::LfoController( Model * _parent ) :
	Controller( ControllerType::Lfo, _parent, tr( "LFO Controller" ) ),
	m_baseModel(0.5f, 0.f, 1.f, 0.001f, this, tr("Base value")),
	m_speedModel(2.f, 0.01f, 20.f, 0.0001f, 20000.f, this, tr("Oscillator speed")),
	m_amountModel(1.f, -1.f, 1.f, 0.005f, this, tr("Oscillator amount")),
	m_phaseModel( 0.0, 0.0, 360.0, 4.0, this, tr( "Oscillator phase" ) ),
	m_waveModel( static_cast<int>(Oscillator::WaveShape::Sine), 0, Oscillator::NumWaveShapes - 1,
			this, tr( "Oscillator waveform" ) ),
	m_multiplierModel( 0, 0, 2, this, tr( "Frequency Multiplier" ) ),
	m_duration( 1000 ),
	m_phaseOffset( 0 ),
	m_currentPhase( 0 ),
	m_sampleFunction( &Oscillator::sinSample ),
	m_userDefSampleBuffer(std::make_shared<SampleBuffer>())
{
	setSampleExact( true );
	m_waveModel.setValueLabels(Oscillator::waveShapeLabels());
	m_multiplierModel.setValueLabels({{0, tr("1x frequency")}, {1, tr("100x frequency")}, {2, tr("0.01x frequency")}});
	connect(&m_waveModel, &Model::dataChanged, this, [this] {
		try { updateSampleFunction(); }
		catch (...) { if (!NativeValueNotification::report(std::current_exception())) { qWarning("LFO waveform update failed outside a native value notification frame"); } }
	}, Qt::DirectConnection);
	const auto updateTiming = [this] {
		try { updateDuration(); }
		catch (...) { if (!NativeValueNotification::report(std::current_exception())) { qWarning("LFO duration update failed outside a native value notification frame"); } }
	};
	connect(&m_speedModel, &Model::dataChanged, this, updateTiming, Qt::DirectConnection);
	connect(&m_multiplierModel, &Model::dataChanged, this, updateTiming, Qt::DirectConnection);
	const QPointer<AudioEngine> engine(Engine::audioEngine());
	connect(engine.data(), &AudioEngine::sampleRateChanged, this, [this, engine] {
		try { updateDuration(); }
		catch (...)
		{
			// Controller-backed speed reads can fail. Retain the previous
			// duration and return through the native engine after Qt delivery.
			if (engine && !engine->reportDeviceNotificationFailure(std::current_exception()))
			{ qWarning("LFO duration update failed outside a native device notification frame"); }
		}
	});

	connect( Engine::getSong(), SIGNAL(playbackStateChanged()),
			this, SLOT(updatePhase()), Qt::DirectConnection);
	connect(Engine::getSong(), &Song::playbackPositionJumped,
			this, &LfoController::updatePhase, Qt::DirectConnection);

	updateDuration();
}




LfoController::~LfoController()
{
	beginRetirement();
	m_baseModel.disconnect( this );
	m_speedModel.disconnect( this );
	m_amountModel.disconnect( this );
	m_phaseModel.disconnect( this );
	m_waveModel.disconnect( this );
	m_multiplierModel.disconnect( this );
}


void LfoController::updateValueBuffer()
{
	m_phaseOffset = m_phaseModel.value() / 360.0;
	float phase = m_currentPhase + m_phaseOffset;
	float phasePrev = 0.0f;

	// roll phase up until we're in sync with period counter
	m_bufferLastUpdated++;
	if( m_bufferLastUpdated < s_periods )
	{
		const auto diff = s_periods - m_bufferLastUpdated;
		// Wrap before narrowing to float. An unread LFO can otherwise accumulate
		// a phase large enough that its per-sample increment rounds to zero.
		const double elapsedPhase = static_cast<double>(Engine::audioEngine()->framesPerPeriod())
			* static_cast<double>(diff) / static_cast<double>(m_duration);
		phase = static_cast<float>(absFraction(static_cast<double>(phase) + elapsedPhase));
		m_bufferLastUpdated += diff;
	}

	float amount = m_amountModel.value();
	ValueBuffer *amountBuffer = m_amountModel.valueBuffer();
	int amountInc = amountBuffer ? 1 : 0;
	float *amountPtr = amountBuffer ? &(amountBuffer->values()[ 0 ] ) : &amount;
	Oscillator::WaveShape waveshape = static_cast<Oscillator::WaveShape>(m_waveModel.value());

	for( float& f : m_valueBuffer )
	{
		float currentSample = 0;
		switch (waveshape)
		{
		case Oscillator::WaveShape::WhiteNoise:
		{
			if (absFraction(phase) < absFraction(phasePrev))
			{
				// Resample when phase period has completed
				m_heldSample = m_sampleFunction(phase);
			}
			currentSample = m_heldSample;
			break;
		}
		case Oscillator::WaveShape::UserDefined:
		{
			currentSample = Oscillator::userWaveSample(m_userDefSampleBuffer.get(), phase);
			break;
		}
		default:
		{
			if (m_sampleFunction != nullptr)
			{
				currentSample = m_sampleFunction(phase);
			}
		}
	}

		f = std::clamp(m_baseModel.value() + (*amountPtr * currentSample / 2.0f), 0.0f, 1.0f);

		phasePrev = phase;
		phase += 1.0 / m_duration;
		amountPtr += amountInc;
	}

	m_currentPhase = absFraction(phase - m_phaseOffset);
	m_bufferLastUpdated = s_periods;
}

void LfoController::updatePhase()
{
	// Native export starts on its render thread; phase must be ready before DSP.
	const auto changes = Engine::audioEngine()->requestChangesGuard();
	// Long seeks/marker exports need only the fractional oscillator phase.
	// Keep the absolute frame position in double until after wrapping.
	m_currentPhase = static_cast<float>(absFraction(static_cast<double>(Engine::getSong()->getFrames())
		/ static_cast<double>(m_duration)));
	m_bufferLastUpdated = s_periods - 1;
}


void LfoController::updateDuration()
{
	const QPointer<LfoController> receiver(this);
	const QPointer<AudioEngine> engine(Engine::audioEngine());
	if (!engine) { throw std::runtime_error("Native LFO timing requires an audio engine"); }
	const auto rate = engine->outputSampleRate();
	const auto context = [&] {
		if (!receiver || !engine || Engine::audioEngine() != engine.data() || engine->outputSampleRate() != rate)
		{ throw std::runtime_error("Native LFO timing context changed during controller evaluation"); }
	};
	const auto speed = m_speedModel.value(); context();
	const auto multiplier = receiver->m_multiplierModel.value(); context();
	float newDurationF = rate * speed;

	switch(multiplier)
	{
		case 1:
			newDurationF /= 100.0;
			break;

		case 2:
			newDurationF *= 100.0;
			break;

		default:
			break;
	}

	if (!std::isfinite(newDurationF) || newDurationF <= 0.f)
	{ throw std::invalid_argument("Native LFO duration must be finite and positive"); }
	receiver->m_duration = newDurationF;
}

void LfoController::updateSampleFunction()
{
	switch( static_cast<Oscillator::WaveShape>(m_waveModel.value()) )
	{
		case Oscillator::WaveShape::Sine:
		default:
			m_sampleFunction = &Oscillator::sinSample;
			break;
		case Oscillator::WaveShape::Triangle:
			m_sampleFunction = &Oscillator::triangleSample;
			break;
		case Oscillator::WaveShape::Saw:
			m_sampleFunction = &Oscillator::sawSample;
			break;
		case Oscillator::WaveShape::Square:
			m_sampleFunction = &Oscillator::squareSample;
			break;
		case Oscillator::WaveShape::MoogSaw:
			m_sampleFunction = &Oscillator::moogSawSample;
			break;
		case Oscillator::WaveShape::Exponential:
			m_sampleFunction = &Oscillator::expSample;
			break;
		case Oscillator::WaveShape::WhiteNoise:
			m_sampleFunction = &Oscillator::noiseSample;
			break;
		case Oscillator::WaveShape::UserDefined:
			m_sampleFunction = nullptr;
			/*TODO: If C++11 is allowed, should change the type of
			 m_sampleFunction be std::function<sample_t(const float)>
			 and use the line below:
			*/
			//m_sampleFunction = &(m_userDefSampleBuffer->userWaveSample)
			break;
	}
}



// Resource callbacks may retire native owners or replace the project while decoding.
// The guard keeps values/weak actors, never a callback-owned raw buffer reference.
class LfoController::ResourceContext
{
public:
	explicit ResourceContext(LfoController* controller) : m_owner(controller)
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
			|| !controller || controller->thread() != QThread::currentThread() || controller->isRetiring()
			|| !Engine::audioEngine() || !Engine::getSong() || !Engine::mixer())
		{ throw std::logic_error("Native LFO resources require a live controller on the application thread"); }
		m_engine = Engine::audioEngine(); m_song = Engine::getSong(); m_mixer = Engine::mixer();
		m_parentIdentity = controller->parent(); m_parent = controller->parent();
		m_project = m_song->replacementGeneration(); m_routing = m_mixer->replacementGeneration(); m_filename = m_song->projectFileName();
		adopted();
	}
	void actors() const
	{
		if (!m_owner || m_owner->isRetiring() || m_owner->thread() != QThread::currentThread()
			|| !m_engine || Engine::audioEngine() != m_engine.data() || !m_song || Engine::getSong() != m_song.data()
			|| !m_mixer || Engine::mixer() != m_mixer.data() || m_song->replacementGeneration() != m_project
			|| m_mixer->replacementGeneration() != m_routing || m_song->projectFileName() != m_filename
			|| (m_parentIdentity && !m_parent) || m_owner->parent() != m_parentIdentity)
		{ throw std::runtime_error("Native LFO resource context changed"); }
	}
	void check() const
	{ actors(); if (m_owner->m_userDefSampleBuffer != m_buffer || m_owner->m_unresolvedWave != m_missing) { throw std::runtime_error("Native LFO waveform changed during resource work"); } }
	void poll() const
	{ check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } check(); }
	void adopted() { actors(); const auto buffer = m_owner->m_userDefSampleBuffer; const QString missing = m_owner->m_unresolvedWave; m_buffer = buffer; m_missing = missing; actors(); }
private:
	QPointer<LfoController> m_owner; QPointer<QObject> m_parent; QObject* m_parentIdentity = nullptr;
	QPointer<AudioEngine> m_engine; QPointer<Song> m_song; QPointer<Mixer> m_mixer;
	std::uint64_t m_project = 0, m_routing = 0; QString m_filename, m_missing; std::shared_ptr<const SampleBuffer> m_buffer;
};

bool LfoController::setUserWave(const QString& path, QString& error)
{
	const QString requested(path); ResourceContext work(this);
	const auto guard = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; SettingsGuard changing(this);
	const bool result = setUserWaveImpl(requested, error, work); work.check(); return result;
}

bool LfoController::setUserWaveImpl(const QString& path, QString& error, ResourceContext& work)
{
	const QString requested(path); const QPointer<LfoController> owner(this);
	const auto controls = [&] { return std::array<float, 6>{m_baseModel.storedValue(), m_speedModel.storedValue(), m_amountModel.storedValue(), m_phaseModel.storedValue(), m_waveModel.storedValue(), m_multiplierModel.storedValue()}; };
	const auto original = controls();
	const auto ready = [&] { work.check(); if (!owner || controls() != original) { throw std::runtime_error("Native LFO controls changed during waveform loading"); } };
	work.poll(); ready();
	const auto buffer = SampleBuffer::fromFile(requested);
	ready();
	if (!buffer || (!requested.isEmpty() && buffer->empty()))
	{
		error = tr("The LFO waveform could not be decoded: %1").arg(requested);
		return false;
	}
	m_userDefSampleBuffer = buffer; m_unresolvedWave.clear(); work.adopted();
	m_bufferLastUpdated = s_periods - 1;
	error.clear();
	emit dataChanged();
	ready();
	return true;
}

void LfoController::saveSettings(QDomDocument& document, QDomElement& element)
{
	ResourceContext work(this); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; SettingsGuard changing(this);
	const auto buffer = m_userDefSampleBuffer; const QString missing = m_unresolvedWave;
	const auto controls = [&] { return std::array<float, 6>{m_baseModel.storedValue(), m_speedModel.storedValue(), m_amountModel.storedValue(), m_phaseModel.storedValue(), m_waveModel.storedValue(), m_multiplierModel.storedValue()}; };
	const auto values = controls();
	const auto ready = [&] { work.check(); if (controls() != values) { throw std::runtime_error("Native LFO controls changed during serialization"); } };
	saveBaseSettings(document, element); changing.check(); ready();
	m_baseModel.saveSettings(document, element, "base"); changing.check(); ready();
	m_speedModel.saveSettings(document, element, "speed"); changing.check(); ready();
	m_amountModel.saveSettings(document, element, "amount"); changing.check(); ready();
	m_phaseModel.saveSettings(document, element, "phase"); changing.check(); ready();
	m_waveModel.saveSettings(document, element, "wave"); changing.check(); ready();
	m_multiplierModel.saveSettings(document, element, "multiplier"); changing.check(); ready();
	element.setAttribute("userwavefile", missing.isEmpty() ? buffer->audioFile() : missing);
	if (!missing.isEmpty()) { element.setAttribute("userwaveunresolved", 1); }
	else { element.removeAttribute("userwaveunresolved"); buffer->registerResourceSource(document, element, "userwavefile", buffer); ready(); }
}

void LfoController::loadSettings(const QDomElement& element)
{
	const QDomElement settings(element); const QString filename = settings.attribute("userwavefile");
	const bool pending = settings.attribute("userwaveunresolved") == "1";
	ResourceContext work(this); const auto changes = Engine::audioEngine()->requestChangesGuard(); FileResourceIO::EventSuspension events; SettingsGuard changing(this);
	const QPointer<Song> song(Engine::getSong());
	// Select and verify captured accepted data before admitting intentional missing state.
	work.poll(); DataFile::ResourceReferenceScope resource(settings, "userwavefile"); work.check();
	if (filename.contains(QChar::Null) || (pending && filename.isEmpty())) { throw std::invalid_argument("An unresolved LFO waveform requires one valid filename"); }
	QString absolute; bool absent = false;
	if (!filename.isEmpty())
	{
		bool pathError = false; absolute = PathUtil::toAbsolute(filename, &pathError);
		if (pathError || absolute.isEmpty()) { throw std::invalid_argument("Invalid native LFO waveform source path"); }
		if (pending && FileResourceSnapshot::hasRetainedSource(absolute)) { throw std::invalid_argument("An unresolved LFO waveform cannot discard an accepted resource"); }
		absent = !pending && !QFileInfo(FileResourceSnapshot::readableSourcePath(absolute)).exists(); work.check();
	}
	loadBaseSettings(settings); changing.check(); work.check();
	m_baseModel.loadSettings(settings, "base"); changing.check(); work.check();
	m_speedModel.loadSettings(settings, "speed"); changing.check(); work.check();
	m_amountModel.loadSettings(settings, "amount"); changing.check(); work.check();
	m_phaseModel.loadSettings(settings, "phase"); changing.check(); work.check();
	m_waveModel.loadSettings(settings, "wave"); changing.check(); work.check();
	m_multiplierModel.loadSettings(settings, "multiplier"); changing.check(); work.check();
	if (pending || absent)
	{
		m_userDefSampleBuffer = SampleBuffer::emptyBuffer(); m_unresolvedWave = absolute; work.adopted(); m_bufferLastUpdated = s_periods - 1;
		song->collectUnresolvedResource(absolute); work.check();
	}
	else
	{
		QString error; const bool decoded = setUserWaveImpl(filename, error, work); changing.check(); work.check();
		if (!decoded) { song->collectError(error); work.check(); }
	}
	updateSampleFunction(); changing.check(); work.check();
}



QString LfoController::nodeName() const
{
	return( "lfocontroller" );
}



gui::ControllerDialog * LfoController::createDialog( QWidget * _parent )
{
	return new gui::LfoControllerDialog( this, _parent );
}


} // namespace lmms
