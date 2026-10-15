/*
 * AudioFileProcessor.cpp - instrument for using audio files
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

#include "AudioFileProcessor.h"
#include "AudioFileProcessorView.h"
#include <limits>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <cmath>
#include <initializer_list>
#include <QPointer>
#include <QThread>
#include "FileResourceIO.h"
#include "AudioEngine.h"
#include "DataFile.h"

#include "InstrumentTrack.h"
#include "PathUtil.h"
#include "Song.h"

#include "LmmsTypes.h"
#include "plugin_export.h"

#include <QDomElement>


namespace lmms
{

extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT audiofileprocessor_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"AudioFileProcessor",
	QT_TRANSLATE_NOOP( "PluginBrowser",
				"Simple sampler with various settings for "
				"using samples (e.g. drums) in an "
				"instrument-track" ),
	"Tobias Doerffel <tobydox/at/users.sf.net>",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader( "logo" ),
	"wav,ogg,ds,spx,au,voc,aif,aiff,flac,raw"
#ifdef LMMS_HAVE_SNDFILE_MP3
	",mp3"
#endif
	,
	nullptr,
} ;

}

class AudioFileProcessor::Context
{
public:
	explicit Context(AudioFileProcessor* owner, bool strict = false)
		: m_owner(owner), m_strict(strict)
	{
		if (strict && owner->thread() != QThread::currentThread()) { throw std::logic_error("AudioFileProcessor settings require the owner thread"); }
		m_track = owner->instrumentTrack(); m_selected = m_track ? m_track->instrument() : nullptr;
		m_selection = m_selected.data(); m_buffer = owner->m_sample.buffer();
		if (strict)
		{
			m_song = Engine::getSong(); m_container = m_track ? m_track->trackContainer() : nullptr;
			if (!m_song) { throw std::logic_error("AudioFileProcessor requires a native project"); }
			m_generation = m_song->replacementGeneration();
		}
		check();
	}
	void check() const
	{
		if (!m_owner || !m_track || m_track->isRetiring() || !m_owner->isFromTrack(m_track.data())
			|| m_selected.data() != m_selection || m_track->instrument() != m_selection || m_owner->m_sample.buffer() != m_buffer)
		{ throw std::runtime_error("AudioFileProcessor owner, selection or sample changed during native work"); }
		if (m_strict && (!m_song || Engine::getSong() != m_song.data() || m_song->replacementGeneration() != m_generation
			|| !m_container || m_track->trackContainer() != m_container.data() || m_owner->thread() != QThread::currentThread()
			|| m_track->thread() != QThread::currentThread() || std::find(m_container->tracks().begin(), m_container->tracks().end(), m_track.data()) == m_container->tracks().end()))
		{ throw std::runtime_error("AudioFileProcessor project context changed during native work"); }
	}
	void adoptedSample() { if (!m_owner) { check(); } m_buffer = m_owner->m_sample.buffer(); check(); }
protected:
	QPointer<AudioFileProcessor> m_owner;
private:
	QPointer<InstrumentTrack> m_track;
	QPointer<Instrument> m_selected;
	Instrument* m_selection = nullptr;
	std::shared_ptr<const SampleBuffer> m_buffer;
	bool m_strict;
	QPointer<Song> m_song;
	QPointer<TrackContainer> m_container;
	std::uint64_t m_generation = 0;
};

class AudioFileProcessor::SettingsGuard : public AudioFileProcessor::Context
{
public:
	explicit SettingsGuard(AudioFileProcessor* owner) : Context(owner, true)
	{
		if (owner->m_changingState) { throw std::logic_error("AudioFileProcessor settings and file work cannot reenter"); }
		m_changes.emplace(Engine::audioEngine()->requestChangesGuard()); owner->m_changingState = true;
	}
	~SettingsGuard() { if (m_owner) { m_owner->m_changingState = false; } }
	void poll() const { check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } check(); }
private:
	std::optional<AudioEngine::RequestChangesGuard> m_changes;
	FileResourceIO::EventSuspension m_events;
};



AudioFileProcessor::AudioFileProcessor( InstrumentTrack * _instrument_track ) :
	Instrument( _instrument_track, &audiofileprocessor_plugin_descriptor ),
	m_ampModel( 100, 0, 500, 1, this, tr( "Amplify" ) ),
	m_startPointModel( 0, 0, 1, 0.0000001f, this, tr( "Start of sample" ) ),
	m_endPointModel( 1, 0, 1, 0.0000001f, this, tr( "End of sample" ) ),
	m_loopPointModel( 0, 0, 1, 0.0000001f, this, tr( "Loopback point" ) ),
	m_reverseModel( false, this, tr( "Reverse sample" ) ),
	m_loopModel( 0, 0, 2, this, tr( "Loop mode" ) ),
	m_stutterModel( false, this, tr( "Stutter" ) ),
	m_interpolationModel( this, tr( "Interpolation mode" ) ),
	m_nextPlayStartPoint( 0 ),
	m_nextPlayBackwards( false )
{
	m_loopModel.setValueLabels({{static_cast<int>(Sample::Loop::Off), tr("Off")},
		{static_cast<int>(Sample::Loop::On), tr("Loop")}, {static_cast<int>(Sample::Loop::PingPong), tr("Ping-pong")}});
	connect( &m_reverseModel, SIGNAL( dataChanged() ),
				this, SLOT( reverseModelChanged() ), Qt::DirectConnection );
	connect( &m_ampModel, SIGNAL( dataChanged() ),
				this, SLOT( ampModelChanged() ), Qt::DirectConnection );
	connect( &m_startPointModel, SIGNAL( dataChanged() ),
				this, SLOT( startPointChanged() ), Qt::DirectConnection );
	connect( &m_endPointModel, SIGNAL( dataChanged() ),
				this, SLOT( endPointChanged() ), Qt::DirectConnection );
	connect( &m_loopPointModel, SIGNAL( dataChanged() ),
				this, SLOT( loopPointChanged() ), Qt::DirectConnection );
	connect( &m_stutterModel, SIGNAL( dataChanged() ),
				this, SLOT( stutterModelChanged() ), Qt::DirectConnection );

//interpolation modes
	m_interpolationModel.addItem( tr( "None" ) );
	m_interpolationModel.addItem( tr( "Linear" ) );
	m_interpolationModel.addItem( tr( "Sinc" ) );
	m_interpolationModel.setValue( 1 );

	pointChanged();
}




void AudioFileProcessor::playNote( NotePlayHandle * _n,
						SampleFrame* _working_buffer )
{
	const f_cnt_t frames = _n->framesLeftForCurrentPeriod();
	const f_cnt_t offset = _n->noteOffset();

	// Magic key - a frequency < 20 (say, the bottom piano note if using
	// a A4 base tuning) restarts the start point. The note is not actually
	// played.
	if( m_stutterModel.value() == true && _n->frequency() < 20.0 )
	{
		m_nextPlayStartPoint = m_sample.startFrame();
		m_nextPlayBackwards = false;
		return;
	}

	if( !_n->m_pluginData )
	{
		if (m_stutterModel.value() == true && m_nextPlayStartPoint >= static_cast<std::size_t>(m_sample.endFrame()))
		{
			// Restart playing the note if in stutter mode, not in loop mode,
			// and we're at the end of the sample.
			m_nextPlayStartPoint = m_sample.startFrame();
			m_nextPlayBackwards = false;
		}
		// set interpolation mode for libsamplerate
		auto interpolationMode = AudioResampler::Mode::Linear;
		switch( m_interpolationModel.value() )
		{
			case 0:
				interpolationMode = AudioResampler::Mode::ZOH;
				break;
			case 1:
				interpolationMode = AudioResampler::Mode::Linear;
				break;
			case 2:
				interpolationMode = AudioResampler::Mode::SincMedium;
				break;
		}

		_n->m_pluginData = new Sample::PlaybackState(interpolationMode);
		static_cast<Sample::PlaybackState*>(_n->m_pluginData)->setFrameIndex(m_nextPlayStartPoint);
		static_cast<Sample::PlaybackState*>(_n->m_pluginData)->setBackwards(m_nextPlayBackwards);

// debug code
/*		qDebug( "frames %d", m_sample->frames() );
		qDebug( "startframe %d", m_sample->startFrame() );
		qDebug( "nextPlayStartPoint %d", m_nextPlayStartPoint );*/
	}

	if( ! _n->isFinished() )
	{
		if (m_sample.play(_working_buffer + offset,
						static_cast<Sample::PlaybackState*>(_n->m_pluginData),
						frames, static_cast<Sample::Loop>(m_loopModel.value()),
						DefaultBaseFreq / _n->frequency()))
		{
			applyRelease( _working_buffer, _n );
			emit isPlaying(static_cast<Sample::PlaybackState*>(_n->m_pluginData)->frameIndex());
		}
		else
		{
			zeroSampleFrames(_working_buffer, frames + offset);
			emit isPlaying( 0 );
		}
	}
	else
	{
		emit isPlaying( 0 );
	}
	if( m_stutterModel.value() == true )
	{
		m_nextPlayStartPoint = static_cast<Sample::PlaybackState*>(_n->m_pluginData)->frameIndex();
		m_nextPlayBackwards = static_cast<Sample::PlaybackState*>(_n->m_pluginData)->backwards();
	}
}




void AudioFileProcessor::deleteNotePluginData( NotePlayHandle * _n )
{
	delete static_cast<Sample::PlaybackState*>(_n->m_pluginData);
}




void AudioFileProcessor::saveSettings(QDomDocument& doc, QDomElement& elem)
{
	SettingsGuard work(this); work.poll(); const Sample source(m_sample); const QString unresolved(m_unresolvedFile);
	std::vector<std::pair<QPointer<AutomatableModel>, std::shared_ptr<AutomatableModel::ControllerState>>> controls;
	for (auto model : std::initializer_list<AutomatableModel*>{&m_reverseModel, &m_loopModel, &m_ampModel, &m_startPointModel, &m_endPointModel, &m_loopPointModel, &m_stutterModel, &m_interpolationModel}) { controls.push_back({model, model->captureControllerState()}); work.check(); }
	const auto ready = [&] {
		work.check();
		if (m_unresolvedFile != unresolved || m_sample.startFrame() != source.startFrame() || m_sample.endFrame() != source.endFrame()
			|| m_sample.loopStartFrame() != source.loopStartFrame() || m_sample.loopEndFrame() != source.loopEndFrame()
			|| m_sample.amplification() != source.amplification() || m_sample.frequency() != source.frequency() || m_sample.reversed() != source.reversed())
		{ throw std::runtime_error("AudioFileProcessor sample changed during serialization"); }
		for (const auto& control : controls) { if (!control.first || !control.first->controllerStateMatches(*control.second, false, nullptr, false, false)) { throw std::runtime_error("AudioFileProcessor control changed during serialization"); } work.check(); }
	};
	ready(); const QString path = unresolved.isEmpty() ? source.sampleFile() : unresolved; elem.setAttribute("src", path);
	if (!unresolved.isEmpty()) { elem.setAttribute("unresolved", 1); } else { elem.removeAttribute("unresolved"); }
	if (unresolved.isEmpty()) { source.buffer()->registerResourceSource(doc, elem, "src", source.buffer()); ready(); }
	if (path.isEmpty())
	{
		const bool saved = source.buffer()->saveEmbeddedData(doc, elem, "sampledata"); ready();
		if (!saved) { throw std::runtime_error("AudioFileProcessor embedded sample serialization failed"); } elem.setAttribute("sampleRate", source.sampleRate());
	}
	const auto save = [&](auto& model, const char* key) { ready(); model.saveSettings(doc, elem, key); ready(); };
	save(m_reverseModel, "reversed"); save(m_loopModel, "looped"); save(m_ampModel, "amp"); save(m_startPointModel, "sframe"); save(m_endPointModel, "eframe"); save(m_loopPointModel, "lframe"); save(m_stutterModel, "stutter"); save(m_interpolationModel, "interp"); ready();
}

void AudioFileProcessor::loadSettings(const QDomElement& elem)
{
	const QDomElement settings(elem); const QString srcFile = settings.attribute("src"), sampleData = settings.attribute("sampledata");
	SettingsGuard work(this); work.poll(); DataFile::ResourceReferenceScope resource(settings, "src"); work.check();
	if (settings.attribute("unresolved") == "1")
	{
		if (srcFile.isEmpty() || !sampleData.isEmpty()) { throw std::invalid_argument("Unresolved AudioFileProcessor settings require one filename and no embedded data"); }
		bool error = false; const QString absolute = PathUtil::toAbsolute(srcFile, &error);
		if (error || absolute.isEmpty()) { throw std::invalid_argument("Unresolved AudioFileProcessor filename is invalid"); }
		if (FileResourceSnapshot::hasRetainedSource(absolute)) { throw std::invalid_argument("Unresolved AudioFileProcessor settings cannot discard an accepted resource"); }
		m_unresolvedFile = absolute; m_sample = Sample(SampleBuffer::emptyBuffer()); work.adoptedSample();
		Engine::getSong()->collectUnresolvedResource(m_unresolvedFile); work.check();
	}
	else if (!srcFile.isEmpty())
	{
		const QString absolute = PathUtil::toAbsolute(srcFile); const bool exists = QFileInfo(FileResourceSnapshot::readableSourcePath(absolute)).exists(); work.check();
		if (exists) { QString error; if (!loadAudioFileImpl(srcFile, false, error, work)) { Engine::getSong()->collectError(error); work.check(); } }
		else
		{
			if (m_sample.buffer()->empty() && m_sample.sampleFile().isEmpty()) { m_unresolvedFile = absolute; }
			if (!m_unresolvedFile.isEmpty()) { Engine::getSong()->collectUnresolvedResource(m_unresolvedFile); }
			else { Engine::getSong()->collectError(QString("%1: %2").arg(tr("Sample not found"), srcFile)); } work.check();
		}
	}
	else if (!sampleData.isEmpty())
	{
		QString error; bool rateOk = true;
		const int rate = settings.hasAttribute("sampleRate") ? settings.attribute("sampleRate").toInt(&rateOk) : Engine::audioEngine()->outputSampleRate();
		auto buffer = SampleBuffer::fromBase64(sampleData, rateOk ? rate : 0, error); work.check();
		if (buffer) { m_unresolvedFile.clear(); m_sample = Sample(std::move(buffer)); work.adoptedSample(); }
		else { Engine::getSong()->collectError(tr("Invalid embedded AudioFileProcessor sample: %1").arg(error)); work.check(); }
	}
	else { QString error; if (!loadAudioFileImpl({}, false, error, work)) { Engine::getSong()->collectError(error); work.check(); } }
	const auto load = [&](auto& model, const char* key) { work.check(); model.loadSettings(settings, key); work.check(); };
	load(m_loopModel, "looped"); load(m_ampModel, "amp"); load(m_endPointModel, "eframe"); load(m_startPointModel, "sframe");
	load(m_loopPointModel, settings.hasAttribute("lframe") || !settings.firstChildElement("lframe").isNull() ? "lframe" : "sframe");
	load(m_reverseModel, "reversed"); load(m_stutterModel, "stutter");
	if (settings.hasAttribute("interp") || !settings.firstChildElement("interp").isNull()) { load(m_interpolationModel, "interp"); }
	else { m_interpolationModel.setValue(1.0f); work.check(); }
	pointChanged(); work.check(); emit sampleUpdated(); work.check();
}

void AudioFileProcessor::loadFile( const QString & _file )
{
	setAudioFile( _file );
}

std::vector<Plugin::FileResource> AudioFileProcessor::fileResources() const
{
	return {{"sample", tr("Sample"), m_unresolvedFile.isEmpty() ? m_sample.sampleFile() : m_unresolvedFile, QString(descriptor()->supportedFileTypes).split(','), nullptr, 0, true, m_sample.buffer(), !m_unresolvedFile.isEmpty()}};
}

bool AudioFileProcessor::setFileResource(const QString& key, const QString& path, QString& error)
{
	if (key != "sample") { error = tr("Unknown AudioFileProcessor resource: %1").arg(key); return false; }
	return loadAudioFile(path, false, error);
}




QString AudioFileProcessor::nodeName() const
{
	return audiofileprocessor_plugin_descriptor.name;
}




auto AudioFileProcessor::beatLen(NotePlayHandle* note) const -> f_cnt_t
{
	// If we can play indefinitely, use the default beat note duration
	if (static_cast<Sample::Loop>(m_loopModel.value()) != Sample::Loop::Off) { return 0; }

	// Otherwise, use the remaining sample duration
	const auto baseFreq = instrumentTrack()->baseFreq();
	const auto freqFactor = baseFreq / note->frequency()
		* Engine::audioEngine()->outputSampleRate()
		/ Engine::audioEngine()->baseSampleRate();
	const auto sampleRateRatio = static_cast<double>(Engine::audioEngine()->outputSampleRate()) / m_sample.sampleRate();

	const auto startFrame = m_nextPlayStartPoint >= static_cast<std::size_t>(m_sample.endFrame())
		? m_sample.startFrame()
		: m_nextPlayStartPoint;
	const auto duration = m_sample.endFrame() - startFrame;

	return static_cast<f_cnt_t>(std::floor(duration * freqFactor * sampleRateRatio));
}




gui::PluginView* AudioFileProcessor::instantiateView( QWidget * _parent )
{
	return new gui::AudioFileProcessorView( this, _parent );
}

void AudioFileProcessor::setAudioFile(const QString& path, bool rename)
{
	const QString requested(path); const QPointer<Song> owner(Engine::getSong()); QString error;
	if (!loadAudioFile(requested, rename, error) && owner && Engine::getSong() == owner.data()) { owner->collectError(error); }
}

bool AudioFileProcessor::loadAudioFile(const QString& path, bool rename, QString& error)
{
	const QString requested(path);
	try { SettingsGuard work(this); return loadAudioFileImpl(requested, rename, error, work); }
	catch (const std::exception& failure) { error = QString::fromUtf8(failure.what()); }
	catch (...) { error = "AudioFileProcessor file work failed with an unknown native exception"; }
	return false;
}

bool AudioFileProcessor::loadAudioFileImpl(const QString& path, bool rename, QString& error, SettingsGuard& work)
{
	const QString requested(path); work.poll(); const auto buffer = SampleBuffer::fromFile(requested); work.check();
	if (!requested.isEmpty() && (buffer->empty() || buffer->size() > static_cast<std::size_t>(std::numeric_limits<int>::max())))
	{
		if (m_sample.buffer()->empty() && m_sample.sampleFile().isEmpty()) { m_unresolvedFile = requested; }
		error = tr("The instrument sample could not be decoded within native frame bounds: %1").arg(requested); return false;
	}
	if (rename && (instrumentTrack()->name() == QFileInfo(m_sample.sampleFile()).fileName() || m_sample.sampleFile().isEmpty()))
	{ const QString name = PathUtil::cleanName(requested); instrumentTrack()->setName(name); work.check(); }
	m_unresolvedFile.clear(); m_sample = Sample(buffer); work.adoptedSample();
	loopPointChanged(); work.check(); ampModelChanged(); work.check(); reverseModelChanged(); work.check(); emit sampleUpdated(); work.check();
	error.clear(); return true;
}

void AudioFileProcessor::reverseModelChanged()
{
	Context context(this); const bool reversed = m_reverseModel.value(); context.check();
	m_sample.setReversed(reversed); m_nextPlayStartPoint = m_sample.startFrame(); m_nextPlayBackwards = false;
	emit sampleUpdated(); context.check();
}

void AudioFileProcessor::ampModelChanged()
{
	Context context(this); const auto value = m_ampModel.value(); context.check();
	if (!std::isfinite(value)) { throw std::out_of_range("AudioFileProcessor amplification is not finite"); }
	m_sample.setAmplification(value / 100.0f); emit sampleUpdated(); context.check();
}

void AudioFileProcessor::stutterModelChanged()
{
	Context context(this); m_nextPlayStartPoint = m_sample.startFrame(); m_nextPlayBackwards = false;
}

void AudioFileProcessor::startPointChanged()
{
	Context context(this); const auto value = [&](const auto& model) { context.check(); const auto result = model.value(); context.check(); return result; };
	if (value(m_startPointModel) > value(m_endPointModel))
	{ const auto start = value(m_startPointModel), end = value(m_endPointModel); m_endPointModel.setValue(start); context.check(); m_startPointModel.setValue(end); context.check(); }
	if (value(m_loopPointModel) >= value(m_endPointModel)) { const auto end = value(m_endPointModel); m_loopPointModel.setValue(qMax(end - .001f, 0.f)); context.check(); }
	if (value(m_loopPointModel) < value(m_startPointModel)) { const auto start = value(m_startPointModel); m_loopPointModel.setValue(start); context.check(); }
	if (value(m_startPointModel) == value(m_endPointModel)) { const auto end = value(m_endPointModel); m_endPointModel.setValue(qMin(end + .001f, 1.f)); context.check(); }
	pointChanged(); context.check();
}

void AudioFileProcessor::endPointChanged() { startPointChanged(); }

void AudioFileProcessor::loopPointChanged()
{
	Context context(this); const auto value = [&](const auto& model) { context.check(); const auto result = model.value(); context.check(); return result; };
	if (value(m_loopPointModel) >= value(m_endPointModel))
	{
		const auto loop = value(m_loopPointModel); m_endPointModel.setValue(loop + .001f); context.check();
		if (value(m_endPointModel) == 1.f) { m_loopPointModel.setValue(1.f - .001f); context.check(); }
	}
	if (value(m_loopPointModel) < value(m_startPointModel)) { const auto loop = value(m_loopPointModel); m_startPointModel.setValue(loop); context.check(); }
	pointChanged(); context.check();
}

void AudioFileProcessor::pointChanged()
{
	Context context(this); const auto frames = m_sample.sampleSize();
	const auto start = m_startPointModel.value(); context.check(); const auto end = m_endPointModel.value(); context.check(); const auto loop = m_loopPointModel.value(); context.check();
	if (frames > static_cast<std::size_t>(std::numeric_limits<int>::max()) || !std::isfinite(start) || !std::isfinite(end) || !std::isfinite(loop)
		|| start < 0 || start > 1 || end < 0 || end > 1 || loop < 0 || loop > 1)
	{ throw std::out_of_range("AudioFileProcessor sample points exceed native frame bounds"); }
	const auto f_start = static_cast<f_cnt_t>(static_cast<double>(start) * frames), f_end = static_cast<f_cnt_t>(static_cast<double>(end) * frames), f_loop = static_cast<f_cnt_t>(static_cast<double>(loop) * frames);
	m_nextPlayStartPoint = f_start; m_nextPlayBackwards = false; m_sample.setAllPointFrames(f_start, f_end, f_loop, f_end);
	emit dataChanged(); context.check();
}

extern "C"
{

// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main(Model * model, void *)
{
	return new AudioFileProcessor(static_cast<InstrumentTrack *>(model));
}


}


} // namespace lmms
