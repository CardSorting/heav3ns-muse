/*
 * SampleClip.cpp
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

#include "SampleClip.h"
#include "NativeValueNotification.h"

#include <QDomElement>
#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QScopedValueRollback>
#include <algorithm>
#include <stdexcept>
#include <exception>
#include <cmath>
#include <limits>

#include "PatternStore.h"
#include "PathUtil.h"
#include "SampleClipView.h"
#include "SampleTrack.h"
#include "SampleRecordingBuffer.h"
#include "DataFile.h"
#include "FileResourceIO.h"
#include "Song.h"

namespace lmms
{

class SampleClip::ResourceGuard
{
public:
	explicit ResourceGuard(SampleClip* clip) : m_clip(clip)
	{
		if (clip->thread() != QThread::currentThread()) { throw std::logic_error("Sample resources require the clip owner thread"); }
		if (clip->m_changingResource) { throw std::logic_error("Sample resource work cannot reenter"); }
		m_track = clip->getTrack(); m_container = m_track ? m_track->trackContainer() : nullptr;
		m_song = Engine::getSong(); m_generation = m_song ? m_song->replacementGeneration() : 0; m_buffer = clip->m_sample.buffer();
		check(); m_changes.emplace(Engine::audioEngine()->requestChangesGuard()); clip->m_changingResource = true;
	}
	~ResourceGuard() { if (m_clip) { m_clip->m_changingResource = false; } }
	void check() const
	{
		if (!m_clip || !m_track || !m_container || !m_song || Engine::getSong() != m_song.data()
			|| m_song->replacementGeneration() != m_generation || m_clip->getTrack() != m_track.data() || m_track->trackContainer() != m_container.data()
			|| m_clip->m_sample.buffer() != m_buffer || m_clip->thread() != QThread::currentThread()
			|| std::find(m_track->getClips().begin(), m_track->getClips().end(), m_clip.data()) == m_track->getClips().end()
			|| std::find(m_container->tracks().begin(), m_container->tracks().end(), m_track.data()) == m_container->tracks().end())
		{ throw std::runtime_error("Native sample clip context or buffer changed during resource work"); }
	}
	void adopted() { if (!m_clip) { check(); } m_buffer = m_clip->m_sample.buffer(); check(); }
	void poll() const { check(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } check(); }
private:
	QPointer<SampleClip> m_clip;
	QPointer<Track> m_track;
	QPointer<TrackContainer> m_container;
	QPointer<Song> m_song;
	std::uint64_t m_generation = 0;
	std::shared_ptr<const SampleBuffer> m_buffer;
	std::optional<AudioEngine::RequestChangesGuard> m_changes;
	FileResourceIO::EventSuspension m_events;
};

SampleClip::SampleClip(Track* _track, Sample sample, bool isPlaying):
	Clip(_track),
	m_sample(std::move(sample)),
	m_isPlaying(false),
	m_startFrameOffset(0)
{
	saveJournallingState( false );
	changeLength(DefaultTicksPerBar * (static_cast<float>(Engine::getSong()->getTimeSigModel().getNumerator()) / Engine::getSong()->getTimeSigModel().getDenominator()));
	restoreJournallingState();

	// we need to receive bpm-change-events, because then we have to
	// change length of this Clip
	connect(Engine::getSong(), &Song::tempoChanged, this, [this] {
		try { tempoChanged(); }
		catch (...) { if (!NativeValueNotification::report(std::current_exception())) { qWarning("Sample clip tempo update failed outside a native value notification frame"); } }
	}, Qt::DirectConnection);
	connect(Engine::getSong(), &Song::timeSignatureChanged, this, [this] {
		try { updateLength(); }
		catch (...) { if (!NativeValueNotification::report(std::current_exception())) { qWarning("Sample clip meter update failed outside a native value notification frame"); } }
	});

	//playbutton clicked or space key / on Export Song set isPlaying to false
	connect( Engine::getSong(), SIGNAL(playbackStateChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about loops and jumps
	connect(Engine::getSong(), &Song::playbackPositionJumped,
			this, &SampleClip::playbackPositionChanged, Qt::DirectConnection);
	//care about mute Clips
	connect( this, SIGNAL(dataChanged()), this, SLOT(playbackPositionChanged()));
	//care about mute track
	connect( getTrack()->getMutedModel(), SIGNAL(dataChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about Clip position
	connect( this, SIGNAL(positionChanged()), this, SLOT(updateTrackClips()));

	updateTrackClips();
}

SampleClip::SampleClip(Track* track)
	: SampleClip(track, Sample(), false)
{
}

SampleClip::SampleClip(const SampleClip& orig) : SampleClip(orig, orig.getTrack()) { }

SampleClip::SampleClip(const SampleClip& orig, Track* destination) :
	Clip(orig, destination),
	m_sample(orig.m_sample),
	m_unresolved(orig.m_unresolved),
	m_isPlaying(false),
	m_startFrameOffset(orig.m_startFrameOffset)
{
	// we need to receive bpm-change-events, because then we have to
	// change length of this Clip
	connect(Engine::getSong(), &Song::tempoChanged, this, [this] {
		try { tempoChanged(); }
		catch (...) { if (!NativeValueNotification::report(std::current_exception())) { qWarning("Sample clip tempo update failed outside a native value notification frame"); } }
	}, Qt::DirectConnection);
	connect(Engine::getSong(), &Song::timeSignatureChanged, this, [this] {
		try { updateLength(); }
		catch (...) { if (!NativeValueNotification::report(std::current_exception())) { qWarning("Sample clip meter update failed outside a native value notification frame"); } }
	});

	//playbutton clicked or space key / on Export Song set isPlaying to false
	connect( Engine::getSong(), SIGNAL(playbackStateChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about loops and jumps
	connect(Engine::getSong(), &Song::playbackPositionJumped,
			this, &SampleClip::playbackPositionChanged, Qt::DirectConnection);
	//care about mute Clips
	connect( this, SIGNAL(dataChanged()), this, SLOT(playbackPositionChanged()));
	//care about mute track
	connect( getTrack()->getMutedModel(), SIGNAL(dataChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about Clip position
	connect( this, SIGNAL(positionChanged()), this, SLOT(updateTrackClips()));

	updateTrackClips();
}




SampleClip::~SampleClip()
{
	auto sampletrack = dynamic_cast<SampleTrack*>(getTrack());
	if ( sampletrack )
	{
		sampletrack->updateClips();
	}
}




void SampleClip::changeLength( const TimePos & _length )
{
	Clip::changeLength(std::max(static_cast<int>(_length), 1));
}



const QString& SampleClip::sampleFile() const
{
	return m_unresolved ? m_unresolved->path : m_sample.sampleFile();
}

bool SampleClip::hasSampleFileLoaded(const QString & filename) const
{
	return !m_unresolved && m_sample.sampleFile() == filename;
}

int SampleClip::sourceSampleRate() const { return m_unresolved ? m_unresolved->rate : m_sample.sampleRate(); }
std::optional<std::pair<int, int>> SampleClip::sourceSampleWindow() const
{ if (m_unresolved) { return m_unresolved->window; } return std::make_pair(m_sample.startFrame(), m_sample.endFrame()); }

bool SampleClip::beginRecordingTake(bool reserved)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_finalizingRecording || reserved != m_recordingReserved
		|| (reserved && (m_activeRecordingTakes || hasPendingRecording()))) { return false; }
	++m_activeRecordingTakes;
	return true;
}

bool SampleClip::reserveRecordingCompletion(QString& error)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (QThread::currentThread() != thread() || m_recordingReserved || hasActiveRecording()
		|| hasPendingRecording() || isFinalizingRecording())
	{
		error = tr("Native clip recording completion is already in use"); return false;
	}
	m_recordingReserved = true; error.clear(); return true;
}

bool SampleClip::finishReservedRecording(f_cnt_t expectedFrames, sample_rate_t expectedRate, QString& error)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (QThread::currentThread() != thread() || !m_recordingReserved || hasActiveRecording()
		|| isFinalizingRecording() || m_completedRecordings.size() != 1 || !m_recordingCompletionError.isEmpty())
	{
		error = m_recordingCompletionError.isEmpty() ? tr("Native recording did not complete exactly one take") : m_recordingCompletionError; return false;
	}
	const auto& recording = m_completedRecordings.first();
	if (!recording || recording->failed() || recording->framesRecorded() != expectedFrames || recording->sampleRate() != expectedRate)
	{
		error = recording && recording->failed() ? recording->error() : tr("Native recording frame count or sample rate did not match the request"); return false;
	}
	m_recordingReserved = false;
	return finalizeRecordings(error);
}

void SampleClip::discardReservedRecording()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (!m_recordingReserved || hasActiveRecording() || isFinalizingRecording()) { return; }
	m_completedRecordings.clear(); m_recordingCompletionError.clear(); m_recordingReserved = false;
}

void SampleClip::stageRecordingCompletion(std::shared_ptr<SampleRecordingBuffer> recording, const QString& error)
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_activeRecordingTakes > 0) { --m_activeRecordingTakes; }
	try
	{
		if (recording) { m_completedRecordings.push_back(std::move(recording)); }
		else if (m_recordingCompletionError.isEmpty())
		{
			m_recordingCompletionError = error.isEmpty() ? tr("Native recording completion failed") : error;
		}
	}
	catch (const std::exception& failure) { m_recordingCompletionError = QString::fromUtf8(failure.what()); }
	if (m_recordingReserved) { return; }
	const QPointer<SampleClip> destination(this);
	const auto complete = [destination] {
		if (!destination) { return; }
		QString error; destination->finalizeRecordings(error);
	};
	if (QThread::currentThread() == thread()) { complete(); }
	else { QMetaObject::invokeMethod(this, complete, Qt::QueuedConnection); }
}

bool SampleClip::finalizeRecordings(QString& error)
{
	if (QThread::currentThread() != thread()) { error = tr("Native recording must finalize on its clip thread"); return false; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	if (m_recordingReserved) { error = tr("Native recording transaction has not committed"); return false; }
	if (m_activeRecordingTakes != 0) { error = tr("Finish the active native recording before saving"); return false; }
	if (m_finalizingRecording) { error = tr("Native recording completion is still publishing"); return false; }
	QScopedValueRollback<bool> finalizing(m_finalizingRecording, true);
	QString firstError = std::move(m_recordingCompletionError); m_recordingCompletionError.clear();
	bool completed = !firstError.isEmpty();
	while (!m_completedRecordings.isEmpty())
	{
		completed = true; const auto recording = m_completedRecordings.takeFirst();
		try
		{
			QString failure; const auto buffer = recording->finalize(failure);
			if (buffer) { setSampleBuffer(buffer); }
			else if (firstError.isEmpty()) { firstError = failure; }
		}
		catch (const std::exception& failure) { if (firstError.isEmpty()) { firstError = QString::fromUtf8(failure.what()); } }
	}
	if (completed && m_activeRecordingTakes == 0) { setRecord(false); }
	if (!firstError.isEmpty()) { Engine::getSong()->collectError(firstError); error = firstError; return false; }
	if (m_activeRecordingTakes != 0) { error = tr("A new native recording started during completion"); return false; }
	error.clear(); return true;
}

void SampleClip::setSampleBuffer(std::shared_ptr<const SampleBuffer> sb)
{
	if (!sb) { throw std::invalid_argument("A native sample buffer is required"); }
	ResourceGuard work(this); m_sample = Sample(std::move(sb)); m_unresolved.reset(); work.adopted();
	updateLength(); work.check(); emit sampleChanged(); work.check(); emit dataChanged(); work.check();
	Engine::getSong()->setModified(); work.check();
}

void SampleClip::setSampleFile(const QString& sf)
{
	const QString requested(sf); const QPointer<Song> song(Engine::getSong()); QString error;
	if (!loadSampleFile(requested, error, sampleUnresolved()) && song && Engine::getSong() == song.data()) { song->collectError(error); }
}

bool SampleClip::loadSampleFile(const QString& path, QString& error, bool preserveSettings)
{
	const QString requested(path);
	try
	{
		ResourceGuard work(this); requireIdleForCopy(); work.poll();
		const Sample original(m_sample); const auto missing = m_unresolved; const auto position = startPosition(), duration = length(), offset = startTimeOffset(); const auto automatic = getAutoResize(); const QString savedName(name());
		const auto window = sourceSampleWindow(); const auto gain = m_sample.amplification(); const bool reversed = m_sample.reversed();
		const auto buffer = SampleBuffer::fromFile(requested); work.check();
		if (m_unresolved != missing || startPosition() != position || length() != duration || startTimeOffset() != offset || getAutoResize() != automatic || name() != savedName
			|| m_sample.startFrame() != original.startFrame() || m_sample.endFrame() != original.endFrame() || m_sample.amplification() != gain || m_sample.reversed() != reversed)
		{ throw std::runtime_error("Native sample clip changed during decoding"); }
		if (!requested.isEmpty() && (!buffer || buffer->empty() || buffer->size() > static_cast<std::size_t>(std::numeric_limits<int>::max())))
		{ error = tr("Sample could not be decoded within native frame bounds: %1").arg(requested); return false; }
		if (preserveSettings && !requested.isEmpty() && window && static_cast<std::size_t>(window->second) > buffer->size())
		{ error = tr("Replacement sample cannot satisfy retained source frame bounds"); return false; }
		Sample replacement(buffer);
		if (preserveSettings && !requested.isEmpty())
		{ if (window) { replacement.setStartFrame(window->first); replacement.setEndFrame(window->second); } replacement.setAmplification(gain); replacement.setReversed(reversed); }
		const Sample accepted(replacement);
		if (!preserveSettings || requested.isEmpty()) { setStartTimeOffset(0); work.check(); }
		m_sample = std::move(replacement); m_unresolved.reset(); work.adopted();
		const auto acceptedState = [&] { work.check(); if (m_unresolved || m_sample.startFrame() != accepted.startFrame() || m_sample.endFrame() != accepted.endFrame() || m_sample.amplification() != accepted.amplification() || m_sample.reversed() != accepted.reversed()) { throw std::runtime_error("Native sample content changed during publication"); } };
		if (!preserveSettings || requested.isEmpty()) { updateLength(); acceptedState(); }
		const auto publishedPosition = startPosition(), publishedDuration = length(), publishedOffset = startTimeOffset(); const auto publishedAutomatic = getAutoResize(); const QString publishedName(name());
		const auto ready = [&] { acceptedState(); if (startPosition() != publishedPosition || length() != publishedDuration || startTimeOffset() != publishedOffset || getAutoResize() != publishedAutomatic || name() != publishedName) { throw std::runtime_error("Native sample placement changed during publication"); } };
		emit sampleChanged(); ready(); emit dataChanged(); ready(); Engine::getSong()->setModified(); ready();
		error.clear(); return true;
	}
	catch (const std::exception& failure) { error = QString::fromUtf8(failure.what()); }
	catch (...) { error = "Native sample resource work failed with a nonstandard exception"; }
	return false;
}




void SampleClip::toggleRecord()
{
	m_recordModel.setValue( !m_recordModel.value() );
	emit dataChanged();
}




void SampleClip::playbackPositionChanged()
{
	Engine::audioEngine()->removePlayHandlesOfTypes(getTrack(), PlayHandle::Type::SamplePlayHandle, true);
	auto st = dynamic_cast<SampleTrack*>(getTrack());
	st->setPlayingClips( false );
}




void SampleClip::updateTrackClips()
{
	auto sampletrack = dynamic_cast<SampleTrack*>(getTrack());
	if( sampletrack)
	{
		sampletrack->updateClips();
	}
}




bool SampleClip::isPlaying() const
{
	return m_isPlaying;
}




void SampleClip::setIsPlaying(bool isPlaying)
{
	m_isPlaying = isPlaying;
}




void SampleClip::updateLength()
{
	const QPointer<SampleClip> owner(this);
	if (m_unresolved) { emit sampleChanged(); return; }
	// If the clip has already been manually resized, don't automatically resize it.
	if (getAutoResize())
	{
		if (getTrack()->trackContainer() == Engine::patternStore())
		{
			changeLength(TimePos::ticksPerBar() * Engine::patternStore()->lengthOfPattern(getTrack()->getClipNum(this)));
			return;
		}
		changeLength(sampleLength());
		if (!owner) { return; }
		setStartTimeOffset(0);
	}

	if (owner) { emit sampleChanged(); }
}


void SampleClip::tempoChanged()
{
	const QPointer<SampleClip> owner(this);
	Clip::setStartTimeOffset(static_cast<int>(std::clamp(std::round(m_startFrameOffset / Engine::framesPerTick()),
		static_cast<double>(std::numeric_limits<int>::min()), static_cast<double>(std::numeric_limits<int>::max()))));
	if (!owner) { return; }
	updateLength();
	if (owner) { emit sampleChanged(); }
}

void SampleClip::setStartTimeOffset(const TimePos& startTimeOffset)
{
	m_startFrameOffset = static_cast<double>(startTimeOffset.getTicks()) * Engine::framesPerTick();
	Clip::setStartTimeOffset(startTimeOffset);
}


TimePos SampleClip::sampleLength() const
{
	if (m_unresolved)
	{
		if (!m_unresolved->window || m_unresolved->rate <= 0) { return 0; }
		const auto ticks = (m_unresolved->window->second - m_unresolved->window->first) / static_cast<double>(Engine::framesPerTick(m_unresolved->rate));
		return static_cast<int>(std::clamp(ticks, 0., static_cast<double>(std::numeric_limits<int>::max())));
	}
	const double ticks = std::max(0, m_sample.endFrame() - m_sample.startFrame())
		/ static_cast<double>(Engine::framesPerTick(m_sample.sampleRate()));
	return static_cast<int>(std::min(ticks, static_cast<double>(std::numeric_limits<int>::max())));
}




bool SampleClip::setSampleWindow(int startFrame, int endFrame)
{
	if (startFrame < 0 || startFrame >= endFrame || static_cast<std::size_t>(endFrame) > m_sample.sampleSize())
	{
		return false;
	}
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	m_sample.setStartFrame(startFrame);
	m_sample.setEndFrame(endFrame);
	updateLength();
	emit dataChanged();
	return true;
}




std::optional<Sample> SampleClip::playbackSample(const TimePos& relativePosition) const
{
	if (m_unresolved) { return std::nullopt; }
	if (relativePosition < 0 || relativePosition >= length()) { return std::nullopt; }
	const double ticks = static_cast<double>(relativePosition.getTicks()) - startTimeOffset().getTicks();
	if (ticks < 0 || m_sample.startFrame() < 0 || m_sample.endFrame() <= m_sample.startFrame()
		|| static_cast<std::size_t>(m_sample.endFrame()) > m_sample.sampleSize()) { return std::nullopt; }
	const double framesPerTick = Engine::framesPerTick(m_sample.sampleRate());
	const double start = m_sample.startFrame() + framesPerTick * ticks;
	const double end = std::min(static_cast<double>(m_sample.endFrame()), m_sample.startFrame()
		+ framesPerTick * (static_cast<double>(length().getTicks()) - startTimeOffset().getTicks()));
	if (start >= end) { return std::nullopt; }
	auto playback = m_sample;
	playback.setStartFrame(static_cast<int>(start));
	playback.setEndFrame(static_cast<int>(end));
	if (playback.startFrame() >= playback.endFrame()) { return std::nullopt; }
	return playback;
}

void SampleClip::reverseSample()
{
	if (m_unresolved) { return; }
	if (m_sample.sampleSize() > static_cast<std::size_t>(std::numeric_limits<int>::max())
		|| m_sample.startFrame() < 0 || m_sample.endFrame() < m_sample.startFrame()
		|| static_cast<std::size_t>(m_sample.endFrame()) > m_sample.sampleSize()) { return; }
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	const auto size = static_cast<int>(m_sample.sampleSize());
	const auto start = m_sample.startFrame();
	m_sample.setStartFrame(size - m_sample.endFrame());
	m_sample.setEndFrame(size - start);
	m_sample.setReversed(!m_sample.reversed());
	const auto offset = static_cast<qint64>(length().getTicks()) - startTimeOffset().getTicks() - sampleLength().getTicks();
	setStartTimeOffset(static_cast<int>(std::clamp(offset,
		static_cast<qint64>(std::numeric_limits<int>::min()), static_cast<qint64>(std::numeric_limits<int>::max()))));
	emit wasReversed();
	emit sampleChanged();
	emit dataChanged();
	Engine::getSong()->setModified();
}




void SampleClip::saveSettings(QDomDocument& doc, QDomElement& element)
{
	if (thread() != QThread::currentThread()) { throw std::logic_error("Native sample serialization requires the clip owner thread"); }
	const QPointer<SampleClip> owner(this);
	const auto recordingGuard = Engine::audioEngine()->requestChangesGuard();
	if (hasReservedRecording() || hasActiveRecording() || hasPendingRecording() || isFinalizingRecording())
	{
		QString error; const bool finalized = finalizeRecordings(error);
		if (!owner) { throw std::runtime_error("Native sample clip retired during recording completion"); }
		if (!finalized) { DataFile::recordSerializationError(doc, error); return; }
	}
	ResourceGuard work(this); work.poll(); const Sample sample(m_sample); const auto missing = m_unresolved;
	const QString filename(sampleFile()), savedName(name()); const auto position = startPosition(), duration = length(), offset = startTimeOffset();
	const auto savedColor = color(); const bool automatic = getAutoResize(); const auto window = sourceSampleWindow(); const auto rate = sourceSampleRate();
	const auto mute = mutedModel()->captureControllerState(), solo = soloModel()->captureControllerState();
	const auto ready = [&] {
		work.check();
		if (m_unresolved != missing || name() != savedName || startPosition() != position || length() != duration || startTimeOffset() != offset
			|| color() != savedColor || getAutoResize() != automatic || m_sample.startFrame() != sample.startFrame() || m_sample.endFrame() != sample.endFrame()
			|| m_sample.amplification() != sample.amplification() || m_sample.reversed() != sample.reversed()
			|| !mutedModel()->controllerStateMatches(*mute, false, nullptr, false, false) || !soloModel()->controllerStateMatches(*solo, false, nullptr, false, false))
		{ throw std::runtime_error("Native sample clip changed during serialization"); }
	};
	ready(); element.setAttribute("pos", element.parentNode().nodeName() == "clipboard" ? -1 : position.getTicks());
	element.setAttribute("len", duration.getTicks()); element.setAttribute("name", savedName); saveControlSettings(doc, element); ready();
	element.setAttribute("src", filename);
	if (missing) { element.setAttribute("unresolved", 1); }
	else { element.removeAttribute("unresolved"); sample.buffer()->registerResourceSource(doc, element, "src", sample.buffer()); ready(); }
	element.setAttribute("off", offset.getTicks()); element.setAttribute("autoresize", QString::number(automatic));
	if (filename.isEmpty()) { const bool saved = sample.buffer()->saveEmbeddedData(doc, element, "data"); ready(); if (!saved) { return; } }
	else { element.removeAttribute("data"); }
	if (rate > 0) { element.setAttribute("sample_rate", rate); } else { element.removeAttribute("sample_rate"); }
	if (savedColor) { element.setAttribute("color", savedColor->name()); } else { element.removeAttribute("color"); }
	if (sample.reversed()) { element.setAttribute("reversed", "true"); } else { element.removeAttribute("reversed"); }
	if (window) { element.setAttribute("sample_start", window->first); element.setAttribute("sample_end", window->second); }
	else { element.removeAttribute("sample_start"); element.removeAttribute("sample_end"); }
	element.setAttribute("sample_gain", QString::number(sample.amplification(), 'g', std::numeric_limits<float>::max_digits10)); ready();
}

void SampleClip::loadSettings(const QDomElement& element)
{
	const QDomElement settings(element); const QString filename = settings.attribute("src"), embedded = settings.attribute("data");
	const bool explicitMissing = settings.attribute("unresolved") == "1";
	ResourceGuard work(this); requireIdleForCopy(); work.poll(); DataFile::ResourceReferenceScope resource(settings, "src"); work.check();
	if (explicitMissing && (filename.isEmpty() || !embedded.isEmpty())) { throw std::invalid_argument("An unresolved sample clip requires one filename and no embedded audio"); }
	Sample sample; std::optional<MissingSource> missing;
	if (!filename.isEmpty())
	{
		bool pathError = false; const QString absolute = PathUtil::toAbsolute(filename, &pathError);
		if (pathError || absolute.isEmpty()) { throw std::invalid_argument("Invalid native sample clip source path"); }
		if (explicitMissing && FileResourceSnapshot::hasRetainedSource(absolute)) { throw std::invalid_argument("An unresolved sample clip cannot discard an accepted resource"); }
		if (explicitMissing || !QFileInfo(FileResourceSnapshot::readableSourcePath(absolute)).exists())
		{ missing = MissingSource{absolute, 0, {}}; }
		else
		{
			const auto buffer = SampleBuffer::fromFile(filename); work.check();
			if (!buffer || buffer->empty() || buffer->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
			{ Engine::getSong()->collectError(tr("Sample could not be decoded: %1").arg(filename)); }
			else { sample = Sample(buffer); }
		}
	}
	if (!explicitMissing && sample.sampleFile().isEmpty() && settings.hasAttribute("data") && (!missing || !embedded.isEmpty()))
	{
		bool valid = false; auto rate = settings.attribute("sample_rate").toInt(&valid);
		if (!valid || rate <= 0)
		{ if (settings.hasAttribute("sample_rate")) { Engine::getSong()->collectError(tr("Invalid embedded sample rate")); } rate = Engine::audioEngine()->outputSampleRate(); }
		QString error; auto buffer = SampleBuffer::fromBase64(embedded, rate, error); work.check();
		if (buffer) { sample = Sample(std::move(buffer)); missing.reset(); } else { Engine::getSong()->collectError(tr("Invalid embedded sample: %1").arg(error)); }
	}
	if (missing && settings.hasAttribute("sample_rate"))
	{
		bool valid = false; const auto rate = settings.attribute("sample_rate").toInt(&valid);
		if (valid && rate > 0) { missing->rate = rate; } else { Engine::getSong()->collectError(tr("Invalid retained sample rate")); }
	}
	const auto reversed = settings.attribute("reversed"); sample.setReversed(reversed == "true" || reversed == "1");
	if (settings.hasAttribute("sample_start") || settings.hasAttribute("sample_end"))
	{
		bool startValid = false, endValid = false; const auto start = settings.attribute("sample_start", "0").toInt(&startValid);
		const auto end = settings.attribute("sample_end", QString::number(sample.endFrame())).toInt(&endValid);
		if (startValid && endValid && start >= 0 && end >= start && (missing || static_cast<std::size_t>(end) <= sample.sampleSize()))
		{ if (missing) { missing->window = std::make_pair(start, end); } else { sample.setStartFrame(start); sample.setEndFrame(end); } }
		else { Engine::getSong()->collectError(tr("Invalid sample source frame bounds")); }
	}
	if (settings.hasAttribute("sample_gain"))
	{
		bool valid = false; const auto gain = settings.attribute("sample_gain").toFloat(&valid);
		if (valid && std::isfinite(gain)) { sample.setAmplification(gain); } else { Engine::getSong()->collectError(tr("Invalid sample gain")); }
	}
	work.check(); m_sample = std::move(sample); m_unresolved = std::move(missing); work.adopted();
	if (m_unresolved) { Engine::getSong()->collectUnresolvedResource(m_unresolved->path); work.check(); }
	setName(settings.attribute("name")); work.check();
	if (settings.attribute("pos").toInt() >= 0) { movePosition(settings.attribute("pos").toInt()); work.check(); }
	changeLength(settings.attribute("len").toInt()); work.check(); loadControlSettings(settings); work.check();
	setStartTimeOffset(settings.attribute("off").toInt()); work.check(); setAutoResize(settings.attribute("autoresize", "1").toInt());
	if (settings.hasAttribute("color")) { setColor(QColor{settings.attribute("color")}); work.check(); }
	emit wasReversed(); work.check(); emit sampleChanged(); work.check(); emit dataChanged(); work.check();
}

gui::ClipView * SampleClip::createView( gui::TrackView * _tv )
{
	return new gui::SampleClipView( this, _tv );
}


} // namespace lmms
