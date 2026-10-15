/*
 * Song.cpp - root of the model tree
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

#include "Song.h"
#include "NativeValueNotification.h"
#include <QTextStream>
#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QMessageBox>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>

#include "AutomationTrack.h"
#include "AutomationEditor.h"
#include "ConfigManager.h"
#include "ControllerRackView.h"
#include "ControllerConnection.h"
#include "NativeLinkedControls.h"
#include "NativeTrackSaveSnapshot.h"
#include "FileResourceIO.h"
#include "EnvelopeAndLfoParameters.h"
#include "Mixer.h"
#include "MixerView.h"
#include "GuiApplication.h"
#include "ExportFilter.h"
#include "InstrumentTrack.h"
#include "Instrument.h"
#include "Keymap.h"
#include "NotePlayHandle.h"
#include "MidiClip.h"
#include "PatternEditor.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PianoRoll.h"
#include "ProjectJournal.h"
#include "ProjectNotes.h"
#include "Scale.h"
#include "TuningFile.h"
#include "SongEditor.h"
#include "PeakController.h"
#include "SampleRecordHandle.h"


namespace lmms
{

tick_t TimePos::s_ticksPerBar = DefaultTicksPerBar;

namespace
{
struct ControllerNotificationFrame;
thread_local ControllerNotificationFrame* controllerNotificationFrame = nullptr;
// Stack-owned frames require no Song field and never dereference a Song on
// unwind. Nested delivery, including another Song, restores its predecessor.
struct ControllerNotificationFrame
{
	explicit ControllerNotificationFrame(Song* source)
		: owner(source), previous(controllerNotificationFrame) { controllerNotificationFrame = this; }
	~ControllerNotificationFrame() { controllerNotificationFrame = previous; }
	ControllerNotificationFrame(const ControllerNotificationFrame&) = delete;
	ControllerNotificationFrame& operator=(const ControllerNotificationFrame&) = delete;
	void rethrow() const { if (failure) { std::rethrow_exception(failure); } }
	QPointer<Song> owner;
	ControllerNotificationFrame* previous;
	std::exception_ptr failure;
};
}

class Song::CreationGuard
{
public:
	explicit CreationGuard(Song* song)
		: m_owner(song), m_patterns(Engine::patternStore()), m_mixer(Engine::mixer()), m_hidden(song->m_globalAutomationTrack),
		  m_journal(Engine::projectJournal()), m_loading(song->m_loadingProject), m_journalling(m_journal->isJournalling())
	{
		if (song->thread() != QThread::currentThread() || song->m_creatingProject || song->m_loadingProjectFile || song->m_loadingProject || song->m_clearingProject || song->m_savingProject || song->m_importingProject)
		{ throw std::logic_error("Native project creation cannot overlap a project transition or run on another thread"); }
		checkContext(); m_changes.emplace(Engine::audioEngine()->requestChangesGuard());
		song->m_creatingProject = true; song->m_loadingProject = true; m_journal->setJournalling(false);
	}
	~CreationGuard()
	{
		if (m_owner) { m_owner->m_permitCreationLoad = false; m_owner->m_creatingProject = false; m_owner->m_loadingProject = m_loading; }
		if (Engine::projectJournal() == m_journal) { m_journal->setJournalling(m_journalling); }
	}
	void checkContext() const
	{
		if (!m_owner || Engine::getSong() != m_owner.data() || !m_patterns || Engine::patternStore() != m_patterns.data()
			|| !m_mixer || Engine::mixer() != m_mixer.data() || !m_hidden || m_owner->m_globalAutomationTrack != m_hidden.data()
			|| Engine::projectJournal() != m_journal) { throw std::runtime_error("Native project creation context retired"); }
	}
	void check() const
	{
		checkContext(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } checkContext();
		for (const auto& entry : m_tracks)
		{
			if (!entry.track || !entry.owner || entry.track->trackContainer() != entry.owner.data()
				|| std::find(entry.owner->tracks().begin(), entry.owner->tracks().end(), entry.track.data()) == entry.owner->tracks().end())
			{ throw std::runtime_error("Native created track retired or detached before project completion"); }
			if (auto instrument = dynamic_cast<InstrumentTrack*>(entry.track.data()); instrument && entry.confirmed
				&& (instrument->isRetiring() || entry.instrument.data() != entry.identity || instrument->instrument() != entry.identity))
			{ throw std::runtime_error("Native created instrument changed before project completion"); }
		}
	}
	void watch(Track* track, bool pendingInstrument = false)
	{
		if (!track) { throw std::runtime_error("Native project track could not be created"); }
		auto instrument = dynamic_cast<InstrumentTrack*>(track);
		m_tracks.push_back({track, track->trackContainer(), instrument ? instrument->instrument() : nullptr, instrument ? instrument->instrument() : nullptr, !pendingInstrument});
	}
	void expectInstrument(const QPointer<InstrumentTrack>& track, const QPointer<Instrument>& instrument)
	{
		checkContext();
		if (!track || !instrument || track->instrument() != instrument.data()) { throw std::runtime_error("Native builtin instrument did not survive creation"); }
		for (auto& entry : m_tracks) { if (entry.track.data() == track.data()) { entry.instrument = instrument; entry.identity = instrument.data(); entry.confirmed = true; } }
		check();
	}
	void watchProject()
	{
		for (auto container : {static_cast<TrackContainer*>(m_owner.data()), static_cast<TrackContainer*>(m_patterns.data())})
		{ for (auto track : container->tracks()) { watch(track); } }
	}
	bool loadTemplate(const QString& path)
	{
		const QString requested(path); check(); m_owner->m_loadingProject = false; m_owner->m_permitCreationLoad = true;
		const auto permit = qScopeGuard([owner = m_owner] { if (owner) { owner->m_permitCreationLoad = false; } });
		const bool loaded = m_owner->loadProject(requested); checkContext(); m_owner->m_loadingProject = true;
		if (loaded) { watchProject(); check(); }
		return loaded;
	}
	void finish(bool refreshPatterns)
	{
		check(); NativeProjectSaveSnapshot source(m_owner.data());
		m_owner->m_oldFileName.clear(); m_owner->setProjectFileName(""); check(); source.validate();
		m_owner->m_loadingProject = false; m_owner->updateLength(); check(); source.validate();
		if (refreshPatterns) { m_patterns->updateAfterTrackAdd(); check(); source.validate(); }
		m_owner->setModified(false); check(); source.validate();
		if (m_owner->isModified() || !m_owner->projectFileName().isEmpty()) { throw std::runtime_error("Native new project publication state changed"); }
		m_owner->m_loadOnLaunch = false;
	}
private:
	struct TrackEntry { QPointer<Track> track; QPointer<TrackContainer> owner; QPointer<Instrument> instrument; Instrument* identity; bool confirmed; };
	QPointer<Song> m_owner;
	QPointer<PatternStore> m_patterns;
	QPointer<Mixer> m_mixer;
	QPointer<AutomationTrack> m_hidden;
	ProjectJournal* m_journal;
	bool m_loading, m_journalling;
	std::optional<AudioEngine::RequestChangesGuard> m_changes;
	FileResourceIO::EventSuspension m_events;
	std::vector<TrackEntry> m_tracks;
};



Song::Song() :
	TrackContainer(),
	m_globalAutomationTrack( dynamic_cast<AutomationTrack *>(
				Track::create( Track::Type::HiddenAutomation,
								this ) ) ),
	m_tempoModel( DefaultTempo, MinTempo, MaxTempo, this, tr( "Tempo" ) ),
	m_timeSigModel( this ),
	m_oldTicksPerBar( DefaultTicksPerBar ),
	m_masterVolumeModel( 100, 0, 200, this, tr( "Master volume" ) ),
	m_masterPitchModel( 0, -12, 12, this, tr( "Master pitch" ) ),
	m_nLoadingTrack( 0 ),
	m_fileName(),
	m_oldFileName(),
	m_modified( false ),
	m_loadOnLaunch( true ),
	m_recording( false ),
	m_exporting( false ),
	m_exportLoop( false ),
	m_renderBetweenMarkers( false ),
	m_playing( false ),
	m_paused( false ),
	m_savingProject( false ),
	m_loadingProject( false ),
	m_isCancelled( false ),
	m_playMode( PlayMode::None ),
	m_length( 0 ),
	m_midiClipToPlay( nullptr ),
	m_loopMidiClip( false ),
	m_loopRenderCount(1),
	m_loopRenderRemaining(1),
	m_oldAutomatedModels()
{
	const auto updateTempo = [this] {
		try { setTempo(); }
		catch (...)
		{
			if (!NativeValueNotification::report(std::current_exception()))
			{ qWarning("Song tempo update failed outside a native value notification frame"); }
		}
	};
	connect(&m_tempoModel, &Model::dataChanged, this, updateTempo, Qt::DirectConnection);
	connect(&m_tempoModel, &Model::dataUnchanged, this, updateTempo, Qt::DirectConnection);
	connect(&m_timeSigModel, &Model::dataChanged, this, [this] {
		try { setTimeSignature(); }
		catch (...)
		{
			if (!NativeValueNotification::report(std::current_exception()))
			{ qWarning("Song meter update failed outside a native value notification frame"); }
		}
	}, Qt::DirectConnection);


	const QPointer<AudioEngine> engine(Engine::audioEngine());
	connect(engine.data(), &AudioEngine::sampleRateChanged, this, [this, engine] {
		try { updateFramesPerTick(); }
		catch (...)
		{
			if (engine && !engine->reportDeviceNotificationFailure(std::current_exception()))
			{ qWarning("Song frame timing update failed outside a native device notification frame"); }
		}
	});

	connect(&m_masterVolumeModel, &Model::dataChanged, this, [this] {
		try { masterVolumeChanged(); }
		catch (...)
		{
			if (!NativeValueNotification::report(std::current_exception()))
			{ qWarning("Song master gain update failed outside a native value notification frame"); }
		}
	}, Qt::DirectConnection);
/*	connect( &m_masterPitchModel, SIGNAL(dataChanged()),
			this, SLOT(masterPitchChanged()));*/

	qRegisterMetaType<lmms::Note>( "lmms::Note" );
	setType( Type::Song );

	for (auto& scale : m_scales) {scale = std::make_shared<Scale>();}
	for (auto& keymap : m_keymaps) {keymap = std::make_shared<Keymap>();}

	// Aggregate the `positionJumped` signals from all the timelines into a single `playbackPositionJumped` signal for
	// other objects (sample tracks, LFOs, etc) to use.
	for (auto i = std::size_t{0}; i < static_cast<std::size_t>(PlayMode::Count); ++i)
	{
		const auto onPositionJumped = [this, playMode = static_cast<PlayMode>(i)] {
			// Only emit the signal when the song is actually playing and the active timeline jumps
			// This prevents LFOs from changing phase when the user drags the timeline while paused
			if (m_playing && m_playMode == playMode) { emit playbackPositionJumped(); }
		};

		// Native sample/LFO/VST discontinuity handling must run on the caller's
		// processing thread before the next tick; GUI consumers queue themselves.
		connect(&m_timelines[i], &Timeline::positionJumped, this, onPositionJumped, Qt::DirectConnection);
	}

	// Inform VST plugins if the user moved the play head
	connect(this, &Song::playbackPositionJumped, this, [this](){ m_vstSyncController.setPlaybackJumped(true); }, Qt::DirectConnection);
}




Song::~Song()
{
	m_controllerLifecycleContext.reset();
	m_playing = false;
	delete m_globalAutomationTrack;
}




void Song::masterVolumeChanged()
{
	Engine::audioEngine()->setMasterGain( m_masterVolumeModel.value() / 100.0f );
}




void Song::setTempo()
{
	const QPointer<Song> owner(this); const QPointer<AudioEngine> engine(Engine::audioEngine());
	if (!engine) { throw std::runtime_error("Native tempo update requires an audio engine"); }
	const auto context = [&] {
		if (!owner || Engine::getSong() != owner.data() || !engine || Engine::audioEngine() != engine.data())
		{ throw std::runtime_error("Native tempo update context retired"); }
	};
	bpm_t tempo;
	{
		const auto changes = engine->requestChangesGuard();
		const auto value = checkedTempo(); context();
		tempo = static_cast<bpm_t>(value);
		for (const auto playHandle : engine->playHandles())
		{
			auto nph = dynamic_cast<NotePlayHandle*>(playHandle);
			if (nph && !nph->isReleased())
			{
				nph->lock(); const auto unlock = qScopeGuard([nph] { nph->unlock(); });
				nph->resize(tempo);
			}
		}
	}

	Engine::updateFramesPerTick(); context();

	m_vstSyncController.setTempo( tempo );

	NativeValueNotification notification;
	emit tempoChanged( tempo );
	notification.rethrow();
}




void Song::setTimeSignature()
{
	const QPointer<Song> owner(this); const QPointer<AudioEngine> engine(Engine::audioEngine());
	if (!engine) { throw std::runtime_error("Native meter update requires an audio engine"); }
	std::uint64_t project = 0, generation = 0;
	const auto context = [&] {
		if (!owner || Engine::getSong() != owner.data() || !engine || Engine::audioEngine() != engine.data()
			|| owner->m_replacementGeneration != project || owner->m_timeSignatureGeneration.load(std::memory_order_relaxed) != generation)
		{ throw std::runtime_error("Native meter update context retired, replaced or superseded"); }
	};
	tick_t ticks = 0; int oldTicks = 0;
	{
		const auto changes = engine->requestChangesGuard();
		project = m_replacementGeneration;
		const auto previous = m_timeSignatureGeneration.load(std::memory_order_relaxed);
		if (previous == std::numeric_limits<std::uint64_t>::max())
		{ throw std::overflow_error("Native meter notification generation exhausted"); }
		generation = previous + 1;
		// Native model ownership serializes writers; notification checks after
		// releasing our local guard can observe a newer DSP/control-thread frame.
		m_timeSignatureGeneration.store(generation, std::memory_order_relaxed);
		// Read once: a controller getter can fail or change between successive reads.
		const int numerator = m_timeSigModel.getNumerator(); context();
		const int denominator = m_timeSigModel.getDenominator(); context();
		if (numerator < m_timeSigModel.numeratorModel().minValue() || numerator > m_timeSigModel.numeratorModel().maxValue()
			|| denominator < m_timeSigModel.denominatorModel().minValue() || denominator > m_timeSigModel.denominatorModel().maxValue())
		{ throw std::out_of_range("Native time signature exceeds the song meter range"); }
		ticks = TimePos::ticksPerBar(TimeSig(numerator, denominator));
		oldTicks = m_oldTicksPerBar;
		TimePos::setTicksPerBar(ticks); m_oldTicksPerBar = ticks;
		m_vstSyncController.setTimeSignature(numerator, denominator);
	}
	context();

	// Receivers report locally; propagate only after Qt returns. A nested update
	// owns its newer clock and must not be overwritten by this older frame.
	NativeValueNotification notification;
	emit timeSignatureChanged(oldTicks, ticks); notification.rethrow(); context();
	emit dataChanged(); notification.rethrow(); context();
}




void Song::savePlayStartPosition()
{
	m_playbackFrames = 0;
	++m_playbackGeneration;
	getTimeline().setPlayStartPosition(getPlayPos());
}




void Song::processNextBuffer()
{
	// If nothing is playing, there is nothing to do
	if (!m_playing) { return; }
	const auto generation = m_playbackGeneration;
	const auto mode = m_playMode;
	QPointer<PatternTrack> selectedPattern;
	const auto samePlayback = [&] {
		return m_playing && !m_paused && m_playMode == mode && m_playbackGeneration == generation
			&& (mode != PlayMode::Pattern || (selectedPattern
				&& PatternTrack::findPatternTrack(Engine::patternStore()->currentPattern()) == selectedPattern.data()
				&& std::find(tracks().begin(), tracks().end(), selectedPattern.data()) != tracks().end()));
	};

	// At the beginning of the song, we have to reset the LFOs
	if (m_playMode == PlayMode::Song && getPlayPos() == 0)
	{
		EnvelopeAndLfoParameters::instances()->reset();
	}

	TrackList trackList;
	int clipNum = -1; // The number of the clip that will be played

	// Determine the list of tracks to play and the clip number
	switch (m_playMode)
	{
		case PlayMode::Song:
			trackList = tracks();
			break;

		case PlayMode::Pattern:
			if (Engine::patternStore()->numOfPatterns() > 0)
			{
				clipNum = Engine::patternStore()->currentPattern();
				selectedPattern = PatternTrack::findPatternTrack(clipNum);
				if (selectedPattern) { trackList.push_back(selectedPattern.data()); }
			}
			break;

		case PlayMode::MidiClip:
			if (m_midiClipToPlay)
			{
				clipNum = m_midiClipToPlay->getTrack()->getClipNum(m_midiClipToPlay);
				trackList.push_back(m_midiClipToPlay->getTrack());
			}
			break;

		default:
			return;
	}

	std::vector<QPointer<Track>> playbackTracks;
	playbackTracks.reserve(trackList.size());
	for (auto track : trackList) { if (track) { playbackTracks.emplace_back(track); } }
	auto& timeline = getTimeline();

	// If the playback position is outside of the range [begin, end), move it to
	// begin and inform interested parties.
	// Returns true if the playback position was moved, else false.
	const auto enforceLoop = [this](const TimePos& begin, const TimePos& end)
	{
		if (getPlayPos() < begin || getPlayPos() >= end)
		{
			getTimeline().setTicks(begin.getTicks());
			return true;
		}
		return false;
	};

	const auto loopEnabled = !m_exporting && timeline.loopEnabled();

	// Ensure playback begins within the loop if it is enabled
	if (loopEnabled) { enforceLoop(timeline.loopBegin(), timeline.loopEnd()); }

	const auto framesPerTick = Engine::framesPerTick();
	const auto framesPerPeriod = Engine::audioEngine()->framesPerPeriod();

	f_cnt_t frameOffsetInPeriod = 0;

	while (frameOffsetInPeriod < framesPerPeriod)
	{
		if (!samePlayback()) { return; }
		auto frameOffsetInTick = timeline.frameOffset();

		// If a whole tick has elapsed, update the frame and tick count, and check any loops
		if (frameOffsetInTick >= framesPerTick)
		{
			// Transfer any whole ticks from the frame count to the tick count
			const auto elapsedTicks = static_cast<int>(frameOffsetInTick / framesPerTick);
			frameOffsetInTick -= elapsedTicks * framesPerTick;
			const auto expectedTick = static_cast<qint64>(timeline.ticks()) + elapsedTicks;
			if (expectedTick < std::numeric_limits<tick_t>::min() || expectedTick > std::numeric_limits<tick_t>::max()) { return; }
			timeline.incrementTicks(elapsedTicks);
			if (!samePlayback() || getPlayPos().getTicks() != expectedTick) { return; }
			timeline.setFrameOffset(frameOffsetInTick);

			// If we are playing a pattern track, or a MIDI clip with no loop enabled,
			// loop back to the beginning when we reach the end
			if (m_playMode == PlayMode::Pattern)
			{
				enforceLoop(TimePos{0}, TimePos{Engine::patternStore()->lengthOfCurrentPattern(), 0});
			}
			else if (m_playMode == PlayMode::MidiClip && m_loopMidiClip && !loopEnabled)
			{
				enforceLoop(-m_midiClipToPlay->startTimeOffset(), m_midiClipToPlay->length() - m_midiClipToPlay->startTimeOffset());
			}

			// Handle loop points, and inform VST plugins of the loop status
			const bool repeatExport = m_exporting && m_loopRenderRemaining > 1;
			if (loopEnabled || (repeatExport && getPlayPos() >= timeline.loopBegin()))
			{
				m_vstSyncController.startCycle(
					timeline.loopBegin().getTicks(), timeline.loopEnd().getTicks());

				// Loop if necessary, and decrement the remaining loops if we did
				if (enforceLoop(timeline.loopBegin(), timeline.loopEnd())
					&& repeatExport)
				{
					m_loopRenderRemaining--;
				}
			}
			else
			{
				m_vstSyncController.stopCycle();
			}
		}

		if (!samePlayback()) { return; }
		const f_cnt_t framesUntilNextPeriod = framesPerPeriod - frameOffsetInPeriod;
		const auto framesUntilNextTick = static_cast<f_cnt_t>(std::ceil(framesPerTick - frameOffsetInTick));

		// We want to proceed to the next buffer or tick, whichever is closer
		const auto framesToPlay = std::min(framesUntilNextPeriod, framesUntilNextTick);

		if (frameOffsetInPeriod == 0)
		{
			// First frame of buffer: update VST sync position.
			// This must be done after we've corrected the frame/tick count,
			// but before actually playing any frames.
			m_vstSyncController.setAbsolutePosition(getPlayPos().getTicks()
				+ timeline.frameOffset() / static_cast<double>(framesPerTick));
			m_vstSyncController.update();
		}

		if (static_cast<f_cnt_t>(frameOffsetInTick) == 0)
		{
			// First frame of tick: process automation and play tracks
			const auto position = getPlayPos();
			if (!processAutomations(playbackTracks, position, framesToPlay)) { return; }
			const auto sameTick = [&] { return samePlayback() && getPlayPos() == position; };
			if (!sameTick()) { return; }
			processMetronome(frameOffsetInPeriod);
			if (!sameTick()) { return; }

			for (const auto& track : playbackTracks)
			{
				if (!track) { continue; }
				const auto owner = track->trackContainer();
				if (!owner || std::find(owner->tracks().begin(), owner->tracks().end(), track.data()) == owner->tracks().end()) { continue; }
				track->play(getPlayPos(), framesToPlay, frameOffsetInPeriod, clipNum);
				if (!sameTick()) { return; }
			}
		}

		// Update frame counters
		frameOffsetInPeriod += framesToPlay;
		frameOffsetInTick += framesToPlay;
		timeline.setFrameOffset(frameOffsetInTick);
	}

	// Reset the jumped state after processing this buffer, since presumably it has now been handled.
	m_vstSyncController.setPlaybackJumped(false);
	m_playbackFrames += framesPerPeriod;
}


bool Song::processAutomations(const std::vector<QPointer<Track>>& tracklist, TimePos timeStart, f_cnt_t)
{
	const auto generation = m_playbackGeneration;
	const auto mode = m_playMode;
	const auto position = getPlayPos();
	QPointer<PatternTrack> selectedPattern;
	const auto current = [&] {
		return m_playing && !m_paused && !m_loadingProject && m_playMode == mode
			&& m_playbackGeneration == generation && getPlayPos() == position
			&& (mode != PlayMode::Pattern || (selectedPattern
				&& PatternTrack::findPatternTrack(Engine::patternStore()->currentPattern()) == selectedPattern.data()
				&& std::find(tracks().begin(), tracks().end(), selectedPattern.data()) != tracks().end()));
	};
	AutomatedValueMap values;

	QSet<const AutomatableModel*> recordedModels;

	TrackContainer* container = this;
	int clipNum = -1;

	switch (m_playMode)
	{
	case PlayMode::Song:
		break;
	case PlayMode::Pattern:
	{
		if (tracklist.empty()) { return true; }
		auto patternTrack = dynamic_cast<PatternTrack*>(tracklist.at(0).data());
		if (!patternTrack) { return false; }
		selectedPattern = patternTrack;
		container = Engine::patternStore();
		clipNum = patternTrack->patternIndex();
	}
		break;
	default:
		return true;
	}
	if (!current()) { return false; }

	values = container->automatedValuesAt(timeStart, clipNum);
	// The native map preserves curve precedence and values. Guard its targets
	// before recording or controller callbacks can retire another model.
	struct Target { QPointer<AutomatableModel> model; float value; };
	std::vector<Target> targets;
	targets.reserve(values.size());
	for (auto it = values.cbegin(); it != values.cend(); ++it)
	{
		if (it.key()) { targets.push_back({it.key(), it.value()}); }
	}
	const TrackList& recordingTracks = container->tracks();

	Track::clipVector clips;
	for (Track* track : recordingTracks)
	{
		if (track->type() == Track::Type::Automation && !track->isMuted()) {
			if (clipNum >= 0)
			{
				if (clipNum < track->numOfClips() && track->getClips()[clipNum]) { clips.push_back(track->getClips()[clipNum]); }
			}
			else { track->getClipsInRange(clips, 0, timeStart); }
		}
	}

	// Snapshot recording clips before model/curve callbacks can retire another
	// clip. This does not pin a track or permit arbitrary deletion during DSP.
	std::vector<QPointer<AutomationClip>> recordingClips;
	for (const auto clip : clips)
	{
		if (const auto curve = dynamic_cast<AutomationClip*>(clip)) { recordingClips.emplace_back(curve); }
	}
	// Process recording
	for (const auto& p : recordingClips)
	{
		if (!p || p->isMuted()) { continue; }
		TimePos relTime = clipNum >= 0 ? timeStart : TimePos(timeStart - p->startPosition());
		if (p->isRecording() && relTime >= 0 && relTime < p->length() && !p->objects().empty())
		{
			const auto recordedModel = p->objects().front();
			if (!recordedModel || !recordedModel->supportsAutomation()) { continue; }
			recordedModel->setUseControllerValue(true);
			if (!current()) { return false; }
			if (!p || !recordedModel) { continue; }
			// The automation system really needs to be reworked.
			// For whatever reason, the values in an automation clip are stored in un-un-scaled format, so if you
			// are automating a log knob, when you draw an curve, the values being stored are not the actual values the
			// knob will take, but instead the unscaled version of the unscaled numbers. The tooltip shows the number you expect, but if you double-click,
			// you can see that the true values are stored by their inverse scaled value....which is wrong, since they weren't scaled in the first place...?
			// Anyhow, in the meantime before we redo the automation system, when recording automations, we have to get the inverseScaledValue
			// and store that so that when playing it back, it scales the value correctly.
			p->recordValue(relTime, recordedModel->inverseScaledValue(recordedModel->value<float>()));
			if (!current()) { return false; }

			if (recordedModel) { recordedModels << recordedModel.data(); }
		}
	}

	// Checks if an automated model stopped being automated by automation clip
	// so we can move the control back to any connected controller again
	const auto previous = m_oldAutomatedModels;
	for (const auto& model : previous)
	{
		if (model && !values.contains(model.data()))
		{
			model->setUseControllerValue(true);
			if (!current()) { return false; }
		}
	}
	// Retain ownership before applying values so a returned callback exception
	// still permits native stop to restore any surviving controller targets.
	std::vector<QPointer<AutomatableModel>> automatedModels;
	automatedModels.reserve(targets.size());
	for (const auto& target : targets)
	{
		if (target.model) { automatedModels.push_back(target.model); }
	}
	m_oldAutomatedModels.swap(automatedModels);

	// Apply values
	for (const auto& target : targets)
	{
		const auto& model = target.model;
		if (!model) { continue; }
		bool isRecording = recordedModels.contains(model.data());
		model->setUseControllerValue(isRecording);
		if (!current()) { return false; }
		if (!model) { continue; }

		if (!isRecording)
		{
			/* TODO
			 * Remove scaleValue() from here when automation editor's
			 * Y axis can be set to logarithmic, and automation clips store
			 * the actual values, and not the invertedScaledValue.
			 */
			model->setValue(model->scaledValue(target.value), true);
			if (!current()) { return false; }
		}
	}
	return current();
}

void Song::processMetronome(size_t bufferOffset)
{
	const auto currentPlayMode = playMode();
	const auto supported = currentPlayMode == PlayMode::MidiClip
		|| currentPlayMode == PlayMode::Song
		|| currentPlayMode == PlayMode::Pattern;

	if (!supported || m_exporting) { return; }
	m_metronome.processTick(currentTick(), ticksPerBar(), m_timeSigModel.getNumerator(), bufferOffset);
}

void Song::setModified(bool value)
{
	if( !m_loadingProject && m_modified != value)
	{
		m_modified = value;
		emit modified();
	}
}

bool Song::isExportDone() const
{
	return !isExporting() || getPlayPos() >= m_exportSongEnd;
}

int Song::getExportProgress() const
{
	qint64 pos = getPlayPos().getTicks();

	if (pos >= m_exportSongEnd)
	{
		return 100;
	}
	else if (pos < m_exportSongBegin)
	{
		return 0;
	}
	else if (pos >= m_exportLoopEnd)
	{
		pos = (m_exportLoopBegin-m_exportSongBegin) + qint64(m_exportLoopEnd - m_exportLoopBegin) *
			m_loopRenderCount + (pos - m_exportLoopEnd);
	}
	else if ( pos >= m_exportLoopBegin )
	{
		pos = (m_exportLoopBegin-m_exportSongBegin) + (qint64(m_exportLoopEnd - m_exportLoopBegin) *
			(m_loopRenderCount - m_loopRenderRemaining)) + (pos - m_exportLoopBegin);
	}
	else
	{
		pos = (pos - m_exportSongBegin);
	}

	return m_exportEffectiveLength > 0 ? static_cast<int>(static_cast<double>(pos) / m_exportEffectiveLength * 100.0) : 0;
}

void Song::playSong()
{
	m_recording = false;

	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = PlayMode::Song;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePlayStartPosition();

	emit playbackStateChanged();
}




void Song::record()
{
	m_recording = true;
	// TODO: Implement
}




void Song::playAndRecord()
{
	playSong();
	m_recording = true;
}




void Song::playPattern()
{
	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = PlayMode::Pattern;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePlayStartPosition();

	emit playbackStateChanged();
}




void Song::playMidiClip( const MidiClip* midiClipToPlay, bool loop )
{
	if( isStopped() == false )
	{
		stop();
	}

	QObject::disconnect(m_playingMidiClipDestroyed);
	m_midiClipToPlay = midiClipToPlay;
	m_loopMidiClip = loop;

	if( m_midiClipToPlay != nullptr )
	{
		m_playMode = PlayMode::MidiClip;
		m_playing = true;
		m_paused = false;
		m_playingMidiClipDestroyed = connect(midiClipToPlay, &MidiClip::destroyedMidiClip, this,
			[this](MidiClip* destroyed) {
				const auto guard = Engine::audioEngine()->requestChangesGuard();
				if (m_midiClipToPlay != destroyed) { return; }
				if (m_playMode == PlayMode::MidiClip) { stop(); }
				m_midiClipToPlay = nullptr;
			}, Qt::DirectConnection);
	}

	m_vstSyncController.setPlaybackState(m_playing);
	savePlayStartPosition();

	emit playbackStateChanged();
}




void Song::updateLength()
{
	if (m_loadingProject) { return; }

	m_length = 0;
	m_tracksMutex.lockForRead();
	for (auto track : tracks())
	{
		// A bulk stem was admitted before earlier stems ran their automation.
		// Its current mute value still controls audio, but must not erase its
		// arranged extent before its own automation gets a chance to run.
		if (m_exporting && track->isMuted() && !track->isRenderSelected())
		{
			continue;
		}

		const bar_t cur = track->length();
		if( cur > m_length )
		{
			m_length = cur;
		}
	}
	m_tracksMutex.unlock();

	emit lengthChanged( m_length );
}




void Song::togglePause()
{
	const auto transportGuard = Engine::audioEngine()->requestChangesGuard();
	// Pause/unpause only works when something is actually playing
	if (m_playMode == PlayMode::None)
	{
		return;
	}

	if( m_paused == true )
	{
		m_playing = true;
		m_paused = false;
	}
	else
	{
		m_playing = false;
		m_paused = true;
		Engine::audioEngine()->clear();
		releaseMidiOutput();
	}

	m_vstSyncController.setPlaybackState( m_playing );

	emit playbackStateChanged();
}




bool Song::releaseMidiOutput()
{
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	m_midiOutputCleanupError.clear();
	int failed = 0;
	for (const auto owner : {static_cast<TrackContainer*>(this), static_cast<TrackContainer*>(Engine::patternStore())})
	{
		if (!owner) { continue; }
		for (const auto track : owner->tracks())
		{
			const auto instrument = dynamic_cast<InstrumentTrack*>(track); if (!instrument) { continue; }
			QString error;
			try { instrument->midiPort()->releaseOutput(); }
			catch (const std::exception& exception)
			{
				error = QString::fromUtf8(exception.what()).left(1024);
				if (error.isEmpty()) { error = QStringLiteral("Native MIDI output cleanup failed without a message"); }
			}
			catch (...) { error = QStringLiteral("Unknown native MIDI output cleanup failure"); }
			if (!error.isEmpty())
			{
				if (++failed == 1) { m_midiOutputCleanupError = error; }
			}
		}
	}
	if (failed > 1) { m_midiOutputCleanupError += QStringLiteral(" (%1 output ports failed)").arg(failed); }
	return failed == 0;
}

void Song::stop()
{
	const auto recordingGuard = Engine::audioEngine()->requestChangesGuard();
	Engine::audioEngine()->finishSampleRecordings();
	QString recordingError;
	if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread()
		&& !SampleRecordHandle::finalizePendingRecordings(recordingError)) { collectError(recordingError); }
	// do not stop/reset things again if we're stopped already
	if( m_playMode == PlayMode::None )
	{
		m_recording = false;
		releaseMidiOutput();
		return;
	}

	using gui::TimeLineWidget;

	// recordingGuard already owns native changes and releases them on exceptions.

	auto& timeline = getTimeline();
	m_paused = false;
	m_recording = false;
	m_playing = false;

	switch (timeline.stopBehaviour())
	{
		case Timeline::StopBehaviour::BackToZero:
			if (m_playMode == PlayMode::MidiClip)
			{
				timeline.setTicks(std::max(0, -m_midiClipToPlay->startTimeOffset()));
			}
			else
			{
				timeline.setTicks(0);
			}
			break;

		case Timeline::StopBehaviour::BackToStart:
			if (timeline.playStartPosition() >= 0)
			{
				timeline.setTicks(timeline.playStartPosition().getTicks());

				timeline.setPlayStartPosition(-1);
			}
			break;

		case Timeline::StopBehaviour::KeepPosition:
			break;
	}

	getTimeline(PlayMode::None).setTicks(getPlayPos().getTicks());
	exportTransitionCheckpoint(ExportTransitionPoint::StopAfterTimeline);

	m_vstSyncController.setPlaybackState( m_exporting );
	m_vstSyncController.setAbsolutePosition(
		getPlayPos().getTicks()
		+ timeline.frameOffset()
		/ (double) Engine::framesPerTick() );

	// remove all note-play-handles that are active
	Engine::audioEngine()->clear();

	// Moves the control of the models that were processed on the last frame
	// back to their controllers.
	const auto automatedModels = m_oldAutomatedModels;
	for (const auto& model : automatedModels)
	{
		if (model) { model->setUseControllerValue(true); }
	}
	m_oldAutomatedModels.clear();
	m_playMode = PlayMode::None;

	releaseMidiOutput();

	emit stopped();
	emit playbackStateChanged();
}




Song::ExportRange Song::exportRange(bool exportLoop, bool betweenMarkers, int loopCount) const
{
	if (loopCount < 1) { throw std::invalid_argument("Native export loop count must be positive"); }
	const auto& timeline = getTimeline(PlayMode::Song);
	const qint64 loopBegin = timeline.loopBegin().getTicks(), loopEnd = timeline.loopEnd().getTicks();
	if ((betweenMarkers || loopCount > 1) && (loopBegin < 0 || loopEnd <= loopBegin))
	{ throw std::invalid_argument("Native export requires ordered nonnegative loop markers"); }
	qint64 begin = 0, end = 0, repeatBegin = 0, repeatEnd = 0;
	if (betweenMarkers)
	{
		begin = repeatBegin = loopBegin; end = repeatEnd = loopEnd;
	}
	else
	{
		end = qint64(m_length) * TimePos::ticksPerBar();
		if (loopCount > 1) { end = std::max(end, loopEnd); }
		if (!exportLoop) { end += TimePos::ticksPerBar(); }
		if (loopBegin >= 0 && loopBegin < end && loopEnd >= loopBegin && loopEnd <= end)
		{ repeatBegin = loopBegin; repeatEnd = loopEnd; }
	}
	if (end < begin || end > std::numeric_limits<tick_t>::max())
	{ throw std::invalid_argument("Native export range exceeds native tick bounds"); }
	const auto effective = (end - begin) + (repeatEnd - repeatBegin) * (loopCount - 1);
	return {TimePos(static_cast<tick_t>(begin)), TimePos(static_cast<tick_t>(end)),
		TimePos(static_cast<tick_t>(repeatBegin)), TimePos(static_cast<tick_t>(repeatEnd)), effective};
}

class Song::ExportTransitionGuard
{
public:
	explicit ExportTransitionGuard(Song* owner) : m_song(owner), m_engine(Engine::audioEngine())
	{
		if (!m_engine) { throw std::logic_error("Native export transitions require an initialized audio engine"); }
		const auto changes = m_engine->requestChangesGuard();
		if (owner->m_exportTransitionActive) { throw std::logic_error("Native export transition cannot reenter"); }
		owner->m_exportTransitionActive = true;
	}
	~ExportTransitionGuard()
	{
		if (m_song && m_engine && Engine::audioEngine() == m_engine.data())
		{ const auto changes = m_engine->requestChangesGuard(); m_song->m_exportTransitionActive = false; }
	}
private:
	QPointer<Song> m_song;
	QPointer<AudioEngine> m_engine;
};

void Song::exportTransitionCheckpoint(ExportTransitionPoint point)
{
	if (!m_exportTransitionProbe) { return; }
	const auto probe = m_exportTransitionProbe; probe(point);
}

void Song::startExport()
{
	ExportTransitionGuard transition(this);
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		if (m_exportCleanupPending) { throw std::logic_error("Pending native export cleanup requires audio.device.restore before another export"); }
		if (m_exporting) { throw std::logic_error("A native song export is already active"); }
	}
	ExportRange range;
	try
	{
		stop();
		// Native export length excludes muted tracks and clips.
		{
			const auto guard = Engine::audioEngine()->requestChangesGuard();
			m_exporting = true; Engine::audioEngine()->updateMidiInputAdmission();
		}
		updateLength(); range = exportRange(m_exportLoop, m_renderBetweenMarkers, m_loopRenderCount);
		if (range.effectiveTicks < 1) { throw std::invalid_argument("Native export range is empty"); }
		m_exportSongBegin = range.begin; m_exportSongEnd = range.end;
		m_exportLoopBegin = range.loopBegin; m_exportLoopEnd = range.loopEnd;
		m_exportEffectiveLength = range.effectiveTicks;
		m_loopRenderRemaining = m_loopRenderCount;
		getTimeline(PlayMode::Song).setTicks(range.begin.getTicks());
		exportTransitionCheckpoint(ExportTransitionPoint::StartAfterTimeline);
		playSong();
		exportTransitionCheckpoint(ExportTransitionPoint::StartAfterPlayback);
		m_vstSyncController.setPlaybackState(true);
	}
	catch (...)
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_exporting = false; m_exportCleanupPending = true; Engine::audioEngine()->updateMidiInputAdmission(); throw;
	}
}




void Song::stopExport()
{
	ExportTransitionGuard transition(this);
	try
	{
		stop();
		exportTransitionCheckpoint(ExportTransitionPoint::StopAfterPlayback);
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_vstSyncController.setPlaybackState(m_playing);
		m_exporting = false; m_exportCleanupPending = false; Engine::audioEngine()->updateMidiInputAdmission();
	}
	catch (...)
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_exportCleanupPending = true; Engine::audioEngine()->updateMidiInputAdmission(); throw;
	}
}




void Song::insertBar()
{
	m_tracksMutex.lockForRead();
	for (Track* track: tracks())
	{
		// FIXME journal batch of tracks instead of each track individually
		if (track->numOfClips() > 0) { track->addJournalCheckPoint(); }
		track->insertBar(getPlayPos(PlayMode::Song));
	}
	m_tracksMutex.unlock();
}




void Song::removeBar()
{
	m_tracksMutex.lockForRead();
	for (Track* track: tracks())
	{
		// FIXME journal batch of tracks instead of each track individually
		if (track->numOfClips() > 0) { track->addJournalCheckPoint(); }
		track->removeBar(getPlayPos(PlayMode::Song));
	}
	m_tracksMutex.unlock();
}




void Song::addPatternTrack()
{
	Track * t = Track::create(Track::Type::Pattern, this);
	Engine::patternStore()->setCurrentPattern(dynamic_cast<PatternTrack*>(t)->patternIndex());
}




void Song::addSampleTrack()
{
	( void )Track::create( Track::Type::Sample, this );
}




void Song::addAutomationTrack()
{
	( void )Track::create( Track::Type::Automation, this );
}




int Song::checkedTempo() const
{
	const auto value = m_tempoModel.value();
	if (value < MinTempo || value > MaxTempo) { throw std::out_of_range("Native tempo exceeds the song range"); }
	return value;
}

bpm_t Song::getTempo()
{
	return static_cast<bpm_t>(checkedTempo());
}


AutomatedValueMap Song::automatedValuesAt(TimePos time, int clipNum) const
{
	auto trackList = TrackList{m_globalAutomationTrack};
	trackList.insert(trackList.end(), tracks().begin(), tracks().end());
	return TrackContainer::automatedValuesFromTracks(trackList, time, clipNum);
}




void Song::clearProject()
{
	if (m_savingProject) { throw std::logic_error("Native project cannot be cleared during serialization"); }
	if (m_loadingProjectFile) { throw std::logic_error("Native project cannot be cleared during restoration"); }
	if (m_creatingProject) { throw std::logic_error("Native project cannot be cleared during creation"); }
	if (m_importingProject) { throw std::logic_error("Native project cannot be cleared during import"); }
	clearProjectImpl();
}

void Song::clearProjectImpl()
{
	if (m_clearingProject || thread() != QThread::currentThread()) { throw std::logic_error("Native project clear cannot reenter or run on another thread"); }
	FileResourceIO::EventSuspension events;
	const QPointer<Song> owner(this);
	const QPointer<PatternStore> patterns(Engine::patternStore());
	const QPointer<Mixer> mixer(Engine::mixer());
	const QPointer<AutomationTrack> hidden(m_globalAutomationTrack);
	auto journal = Engine::projectJournal();
	m_clearingProject = true;
	const auto clearing = qScopeGuard([owner] { if (owner) { owner->m_clearingProject = false; } });
	const auto ready = [&] {
		if (!owner || !patterns || Engine::patternStore() != patterns.data() || !mixer || Engine::mixer() != mixer.data()
			|| Engine::projectJournal() != journal || !hidden || owner->m_globalAutomationTrack != hidden.data()) { throw std::runtime_error("Native project clear context retired"); }
	};
	using gui::getGUI;
	advanceReplacementGeneration();
	m_preservedProjectData.clear();
	const bool wasJournalling = Engine::projectJournal()->isJournalling();
	const auto journalStateRestore = qScopeGuard([journal, wasJournalling] { if (Engine::projectJournal() == journal) { journal->setJournalling(wasJournalling); } });

	Engine::projectJournal()->setJournalling( false );

	if (!isStopped() || isRecording())
	{
		stop();
		ready();
	}

	for (auto i = std::size_t{0}; i < PlayModeCount; i++)
	{
		setPlayPos( 0, ( PlayMode )i );
		ready();
	}


	std::optional<AudioEngine::RequestChangesGuard> modelChange(Engine::audioEngine()->requestChangesGuard());

	NativeLinkedControls::unlinkProjectModels();
	ready();
	if( getGUI() != nullptr && getGUI()->patternEditor() )
	{
		getGUI()->patternEditor()->m_editor->clearAllTracks();
		ready();
	}
	if( getGUI() != nullptr && getGUI()->songEditor() )
	{
		getGUI()->songEditor()->m_editor->clearAllTracks();
		ready();
	}
	if( getGUI() != nullptr && getGUI()->mixerView() )
	{
		getGUI()->mixerView()->clear();
		ready();
	}
	if (getGUI() != nullptr) { QCoreApplication::sendPostedEvents(); ready(); }
	Engine::patternStore()->clearAllTracks();
	ready();
	clearAllTracks();
	ready();

	Engine::mixer()->clear();
	ready();
	for (auto& scale : m_scales) { std::atomic_store(&scale, std::make_shared<Scale>()); }
	for (auto& keymap : m_keymaps) { std::atomic_store(&keymap, std::make_shared<Keymap>()); }
	emit scaleListChanged(-1);
	ready();
	emit keymapListChanged(-1);
	ready();

	if( getGUI() != nullptr && getGUI()->automationEditor() )
	{
		getGUI()->automationEditor()->setCurrentClip( nullptr );
		ready();
	}

	if( getGUI() != nullptr && getGUI()->pianoRoll() )
	{
		getGUI()->pianoRoll()->reset();
		ready();
	}

	m_tempoModel.reset();
	ready();
	m_masterVolumeModel.reset();
	ready();
	m_masterPitchModel.reset();
	ready();
	m_timeSigModel.reset();
	ready();
	getTimeline(PlayMode::Song).setLoopPoints(TimePos(0), TimePos(DefaultTicksPerBar));
	ready();
	getTimeline(PlayMode::Song).setLoopEnabled(false);
	ready();

	// Forget the previous native automation ownership after project replacement.
	m_oldAutomatedModels.clear();
	// Hidden curves can target any project model. Remove stale targets and pending
	// journal IDs before a new project reuses model IDs.
	while (hidden && !hidden->getClips().empty()) { delete hidden->getClips().back(); ready(); }
	if (!hidden) { throw std::runtime_error("Native hidden automation track retired during project clear"); }

	AutomationClip::globalAutomationClip( &m_tempoModel )->clear();
	ready();
	AutomationClip::globalAutomationClip( &m_masterVolumeModel )->
									clear();
	ready();
	AutomationClip::globalAutomationClip( &m_masterPitchModel )->
									clear();
	ready();

	modelChange.reset();

	if( getGUI() != nullptr && getGUI()->getProjectNotes() )
	{
		getGUI()->getProjectNotes()->clear();
		ready();
	}

	removeAllControllers();
	ready();

	emit dataChanged();
	ready();

	Engine::projectJournal()->clearJournal();

	Engine::projectJournal()->setJournalling(wasJournalling);
}




// create new file
void Song::createEmptyProject()
{
	CreationGuard creation(this); creation.check(); clearErrors(); clearProjectImpl(); creation.check(); creation.finish(false);
}

void Song::createNewProject()
{
	CreationGuard creation(this); creation.check();
	QString defaultTemplate = ConfigManager::inst()->userTemplateDir() + "default.mpt";
	if (!QFile::exists(defaultTemplate)) { defaultTemplate = ConfigManager::inst()->factoryProjectsDir() + "templates/default.mpt"; }
	creation.check();
	if (QFile::exists(defaultTemplate))
	{
		if (!creation.loadTemplate(defaultTemplate)) { collectError(tr("The native default project template could not be loaded")); return; }
		creation.finish(false); return;
	}
	clearErrors(); clearProjectImpl(); creation.check();
	m_oldFileName.clear(); setProjectFileName(""); creation.check();
	const auto builtin = [&](TrackContainer* container, const QString& name) {
		const QPointer<InstrumentTrack> track(dynamic_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, container)));
		creation.watch(track.data(), true); creation.check();
		const QPointer<Instrument> instrument(track->loadInstrument(name)); creation.checkContext();
		if (!track || !instrument || track->instrument() != instrument.data() || !instrument->descriptor()
			|| QString::fromUtf8(instrument->descriptor()->name) != name)
		{ collectError(tr("Native default instrument %1 is unavailable").arg(name)); return false; }
		creation.expectInstrument(track, instrument); return true;
	};
	if (!builtin(this, "tripleoscillator")) { return; }
	if (!builtin(Engine::patternStore(), "kicker")) { return; }
	creation.watch(Track::create(Track::Type::Sample, this)); creation.check();
	creation.watch(Track::create(Track::Type::Pattern, this)); creation.check();
	creation.watch(Track::create(Track::Type::Automation, this)); creation.check();
	m_tempoModel.setInitValue(DefaultTempo); creation.check();
	m_timeSigModel.reset(); creation.check();
	m_masterVolumeModel.setInitValue(100); creation.check();
	m_masterPitchModel.setInitValue(0); creation.check();
	creation.finish(true);
}

void Song::createNewProjectFromTemplate(const QString& templ)
{
	const QString requested(templ); CreationGuard creation(this);
	if (!creation.loadTemplate(requested)) { collectError(tr("The native project template could not be loaded")); return; }
	creation.finish(false);
}


// load given song
bool Song::loadProject( const QString & fileName )
{
	if (m_savingProject) { return false; }
	if (m_importingProject) { return false; }
	if (m_creatingProject && !m_permitCreationLoad) { return false; }
	m_permitCreationLoad = false;
	if (m_loadingProjectFile || m_loadingProject || m_clearingProject || thread() != QThread::currentThread()) { return false; }
	const QString requestedFile(fileName);
	const QPointer<Song> owner(this);
	const QPointer<PatternStore> patterns(Engine::patternStore());
	const QPointer<Mixer> mixer(Engine::mixer());
	const QPointer<AutomationTrack> hidden(m_globalAutomationTrack);
	auto journal = Engine::projectJournal();
	const bool wasLoading = m_loadingProject;
	const bool wasJournalling = journal->isJournalling();
	FileResourceIO::EventSuspension events;
	const auto ready = [&] {
		if (!owner || Engine::getSong() != owner.data() || !patterns || Engine::patternStore() != patterns.data()
			|| !mixer || Engine::mixer() != mixer.data() || !hidden || owner->m_globalAutomationTrack != hidden.data()
			|| Engine::projectJournal() != journal) { throw std::runtime_error("Native project load context retired"); }
	};
	const auto check = [&] { ready(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready(); };
	m_loadingProjectFile = true; m_loadingProject = true;
	const auto finish = [&] {
		if (owner) { owner->m_loadingProject = wasLoading; owner->m_loadingProjectFile = false; }
		if (Engine::projectJournal() == journal) { journal->setJournalling(wasJournalling); }
	};
	const auto loadStateRestore = qScopeGuard(finish);
	using gui::getGUI;

	QDomNode node;

	Engine::projectJournal()->setJournalling( false );
	check();
	DataFile dataFile(requestedFile);
	check();

	bool cantLoadProject = false;
	// if file could not be opened, head-node is null and we create
	// new project
	if (dataFile.head().isNull() || (dataFile.type() != DataFile::Type::SongProject && dataFile.type() != DataFile::Type::SongProjectTemplate))
	{
		cantLoadProject = true;
	}
	else
	{
		// We check if plugins contain local paths to prevent malicious code being
		// added to project bundles and loaded with "local:" paths
		if (dataFile.hasLocalPlugins())
		{
			cantLoadProject = true;

			if (getGUI() != nullptr)
			{
				QMessageBox::critical(nullptr, tr("Aborting project load"),
					tr("Project file contains local paths to plugins, which could be used to "
						"run malicious code."));
				ready();
			}
			else
			{
				QTextStream(stderr) << tr("Can't load project: "
					"Project file contains local paths to plugins.")
					<< Qt::endl;
			}
		}
	}

	if (cantLoadProject)
	{
		finish();
		if (m_loadOnLaunch && !m_creatingProject)
		{
			createNewProject();
		}
		return false;
	}

	advanceReplacementGeneration();
	m_oldFileName = m_fileName;
	setProjectFileName(requestedFile);
	check();
	m_oldFileName = m_fileName;

	clearProjectImpl();
	check();
	Engine::projectJournal()->setJournalling(false);
	m_preservedProjectData.appendChild(m_preservedProjectData.importNode(dataFile.content(), true));

	clearErrors();

	std::optional<AudioEngine::RequestChangesGuard> modelChange(Engine::audioEngine()->requestChangesGuard());

	// get the header information from the DOM
	NativeLinkedControls::RestoreScope linkedControls(dataFile);
	m_tempoModel.loadSettings( dataFile.head(), "bpm" );
	check();
	m_timeSigModel.loadSettings( dataFile.head(), "timesig" );
	check();
	m_masterVolumeModel.loadSettings( dataFile.head(), "mastervol" );
	check();
	m_masterPitchModel.loadSettings( dataFile.head(), "masterpitch" );
	check();

	getTimeline(PlayMode::Song).setLoopEnabled(false);
	check();

	//Backward compatibility for LMMS <= 0.4.15
	PeakController::initGetControllerBySetting();

	// Load mixer first to be able to set the correct range for mixer channels
	node = dataFile.content().firstChildElement( Engine::mixer()->nodeName() );
	if( !node.isNull() )
	{
		Engine::mixer()->restoreState( node.toElement() );
		check();
		if( getGUI() != nullptr )
		{
			// refresh MixerView
			getGUI()->mixerView()->refreshDisplay();
			ready();
		}
	}

	node = dataFile.content().firstChild();

	QDomNodeList tclist=dataFile.content().elementsByTagName("trackcontainer");
	m_nLoadingTrack=0;
	for( int i=0,n=tclist.count(); i<n; ++i )
	{
		check();
		QDomNode nd=tclist.at(i).firstChild();
		while(!nd.isNull())
		{
			if( nd.isElement() && nd.nodeName() == "track" )
			{
				++m_nLoadingTrack;
			}
			check();
			nd=nd.nextSibling(); // Comments and extension elements must also advance.
		}
	}

	while( !node.isNull() && !isCancelled() )
	{
		check();
		if( node.isElement() )
		{
			if( node.nodeName() == "trackcontainer" )
			{
				( (JournallingObject *)( this ) )->restoreState( node.toElement() );
			}
			else if (node.nodeName() == "track" && static_cast<Track::Type>(node.toElement().attribute("type").toInt()) == Track::Type::HiddenAutomation)
			{
				m_globalAutomationTrack->restoreState(node.toElement());
			}
			else if( node.nodeName() == "controllers" )
			{
				restoreControllerStates( node.toElement() );
			}
			else if (node.nodeName() == "scales")
			{
				restoreScaleStates(node.toElement());
			}
			else if (node.nodeName() == "keymaps")
			{
				restoreKeymapStates(node.toElement());
			}
			else if (node.nodeName() == getTimeline(PlayMode::Song).nodeName())
			{
				getTimeline(PlayMode::Song).restoreState(node.toElement());
			}
			else if( getGUI() != nullptr )
			{
				if( node.nodeName() == getGUI()->getControllerRackView()->nodeName() )
				{
					getGUI()->getControllerRackView()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == getGUI()->pianoRoll()->nodeName() )
				{
					getGUI()->pianoRoll()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == getGUI()->automationEditor()->m_editor->nodeName() )
				{
					getGUI()->automationEditor()->m_editor->restoreState( node.toElement() );
				}
				else if( node.nodeName() == getGUI()->getProjectNotes()->nodeName() )
				{
					 getGUI()->getProjectNotes()->SerializingObject::restoreState( node.toElement() );
				}
			}
		}
		check();
		node = node.nextSibling();
	}

	// quirk for fixing projects with broken positions of Clips inside pattern tracks
	Engine::patternStore()->fixIncorrectPositions();
	check();
	if (dataFile.head().hasAttribute("currentpattern"))
	{
		bool valid = false;
		const int selected = dataFile.head().attribute("currentpattern").toInt(&valid);
		const int count = Engine::patternStore()->numOfPatterns();
		Engine::patternStore()->setCurrentPattern(valid && selected >= 0 && selected < count ? selected : 0);
		check();
	}

	// Connect controller links to their controllers
	// now that everything is loaded
	ControllerConnection::finalizeConnections();
	check();
	QString linkedError;
	if (!linkedControls.finish(dataFile.content(), linkedError)) { collectError("Native linked controls did not restore: " + linkedError); }
	check();

	// Remove dummy controllers that was added for correct connections
	m_controllers.erase(std::remove_if(m_controllers.begin(), m_controllers.end(),
		[](Controller* c){return c->type() == Controller::ControllerType::Dummy;}),
		m_controllers.end());

	// resolve all IDs so that autoModels are automated
	AutomationClip::resolveAllIDs();
	check();


	modelChange.reset();
	ready();

	ConfigManager::inst()->addRecentlyOpenedProject(requestedFile);
	ready();

	Engine::projectJournal()->setJournalling(wasJournalling);

	emit projectLoaded();
	check();

	if( isCancelled() )
	{
		m_isCancelled = false;
		finish();
		if (!m_creatingProject) { createNewProject(); }
		return false;
	}

	if ( hasErrors())
	{
		if ( getGUI() != nullptr )
		{
			QMessageBox::warning( nullptr, tr("LMMS Error report"), errorSummary(),
							QMessageBox::Ok );
			ready();
		}
		else
		{
			QTextStream(stderr) << Engine::getSong()->errorSummary() << Qt::endl;
		}
	}

	m_loadingProject = false;
	updateLength();
	ready();
	setModified(false);
	ready();
	m_loadOnLaunch = false;
	return true;
}


// only save current song as filename and do nothing else
bool Song::saveProjectFile(const QString & filename, bool withResources, bool absoluteLocalResources,
	QJsonArray* checkpointAssets)
{
	using gui::getGUI;
	if ((checkpointAssets && withResources) || m_savingProject || m_loadingProjectFile || m_clearingProject || m_creatingProject || m_importingProject || thread() != QThread::currentThread()) { return false; }
	const auto modelGuard = Engine::audioEngine()->requestChangesGuard();
	FileResourceIO::EventSuspension events;
	const QPointer<Song> owner(this);
	const QPointer<PatternStore> patterns(Engine::patternStore());
	const QPointer<Mixer> mixer(Engine::mixer());
	const QPointer<Track> hidden(m_globalAutomationTrack);
	const bool oldCheckpoint = m_savingCheckpoint;
	m_savingProject = true; m_savingCheckpoint = checkpointAssets != nullptr;
	const auto saving = qScopeGuard([owner, oldCheckpoint] { if (owner) { owner->m_savingCheckpoint = oldCheckpoint; owner->m_savingProject = false; } });
	const auto ready = [&] {
		if (!owner || Engine::getSong() != owner.data() || !patterns || Engine::patternStore() != patterns.data()
			|| !mixer || Engine::mixer() != mixer.data() || !hidden || owner->m_globalAutomationTrack != hidden.data()) { throw std::runtime_error("Native project save owner retired"); }
	};
	try
	{
	QString recordingError;
	const bool recordingsReady = SampleRecordHandle::finalizePendingRecordings(recordingError); ready();
	if (!recordingsReady) { collectError(recordingError); return false; }
	NativeProjectSaveSnapshot source(this);

	// ensure property order in file is consistent across saves
#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
	qSetGlobalQHashSeed(0);
#else
	QHashSeed::setDeterministicGlobalSeed();
#endif
	const auto hashSeedReset = qScopeGuard([] {
#if QT_VERSION < QT_VERSION_CHECK(6, 6, 0)
		qSetGlobalQHashSeed(-1);
#else
		QHashSeed::resetRandomGlobalSeed();
#endif
	});

	DataFile dataFile( DataFile::Type::SongProject );
	NativeLinkedControls::SaveScope linkedControls(dataFile);
	DataFile::ResourceSourceScope resourceSources(dataFile, withResources || checkpointAssets);
	m_tempoModel.saveSettings( dataFile, dataFile.head(), "bpm" );
	ready();
	m_timeSigModel.saveSettings( dataFile, dataFile.head(), "timesig" );
	ready();
	m_masterVolumeModel.saveSettings( dataFile, dataFile.head(), "mastervol" );
	ready();
	m_masterPitchModel.saveSettings( dataFile, dataFile.head(), "masterpitch" );
	ready();
	dataFile.head().setAttribute("currentpattern", Engine::patternStore()->currentPattern());

	saveState( dataFile, dataFile.content() );
	ready();
	// Use the native hidden-track representation, alongside the regular tracks.
	m_globalAutomationTrack->saveState(dataFile, dataFile.content());
	ready();

	Engine::mixer()->saveState( dataFile, dataFile.content() );
	ready();
	if( getGUI() != nullptr )
	{
		getGUI()->getControllerRackView()->saveState( dataFile, dataFile.content() );
		ready();
		getGUI()->pianoRoll()->saveState( dataFile, dataFile.content() );
		ready();
		getGUI()->automationEditor()->m_editor->saveState( dataFile, dataFile.content() );
		ready();
		getGUI()->getProjectNotes()->SerializingObject::saveState( dataFile, dataFile.content() );
		ready();
	}
	else
	{
		// Views cannot serialize their state in a QCoreApplication. Retain the
		// original nodes, including project notes and future extension metadata.
		for (auto node = m_preservedProjectData.documentElement().firstChild();
			!node.isNull(); node = node.nextSibling())
		{
			const auto name = node.nodeName();
			const bool globalTrack = name == "track" && static_cast<Track::Type>(node.toElement().attribute("type").toInt()) == Track::Type::HiddenAutomation;
			if (!globalTrack && name != "trackcontainer" && name != Engine::mixer()->nodeName()
				&& name != "controllers" && name != "scales" && name != "keymaps" && name != "nativeLinkedControls"
				&& name != getTimeline(PlayMode::Song).nodeName())
			{
				dataFile.content().appendChild(dataFile.importNode(node, true));
			}
		}
	}
	getTimeline(PlayMode::Song).saveState(dataFile, dataFile.content());
	ready();

	saveControllerStates( dataFile, dataFile.content() );
	ready();

	saveScaleStates(dataFile, dataFile.content());
	ready();
	saveKeymapStates(dataFile, dataFile.content());
	ready();
	QString linkedError;
	if (!linkedControls.finish(dataFile.content(), linkedError))
	{ DataFile::recordSerializationError(dataFile, linkedError); collectError("Native linked controls could not be saved: " + linkedError); }
	ready(); source.validate();

	const bool pathsResolved = dataFile.serializationError().isEmpty()
		&& (!absoluteLocalResources || dataFile.makeLocalResourcePathsAbsolute());
	ready();
	const bool assetsPreserved = !checkpointAssets || (pathsResolved
		&& dataFile.copyResources(QFileInfo(filename).absoluteDir().filePath("assets"), checkpointAssets));
	ready();
	bool writeStatus = pathsResolved && assetsPreserved && dataFile.writeFile(filename, withResources, [&] { ready(); source.validate(); });

	return writeStatus;
	}
	catch (const std::exception& error) { if (owner) { owner->collectError(QString("Native project save failed: ") + QString::fromUtf8(error.what())); } return false; }
	catch (...) { if (owner) { owner->collectError("Native project save failed with a nonstandard serializer exception"); } return false; }
}



// Save the current song
bool Song::guiSaveProject()
{
	return guiSaveProjectAs(m_fileName);
}




// Save the current song with the given filename
bool Song::guiSaveProjectAs(const QString & filename)
{
	const QPointer<Song> owner(this);
	DataFile dataFile(DataFile::Type::SongProject);
	QString fileNameWithExtension = dataFile.nameWithExtension(filename);

	bool withResources = m_saveOptions.saveAsProjectBundle.value();

	bool const saveResult = saveProjectFile(fileNameWithExtension, withResources);
	if (!owner) { return false; }

	// After saving, restore default save options.
	m_saveOptions.setDefaultOptions();

	// If we saved a bundle, we keep the project on the original
	// file and still keep it as modified
	if (saveResult && !withResources)
	{
		setModified(false);
		if (!owner) { return false; }
		setProjectFileName(fileNameWithExtension);
	}

	return saveResult;
}




void Song::saveControllerStates( QDomDocument & doc, QDomElement & element )
{
	const QPointer<Song> owner(this);
	std::vector<QPointer<Controller>> roster;
	for (auto controller : m_controllers) { roster.emplace_back(controller); }
	const auto ready = [&] {
		if (!owner || owner->m_controllers.size() != roster.size()) { throw std::runtime_error("Native controller rack changed during serialization"); }
		for (std::size_t index = 0; index < roster.size(); ++index)
		{ if (!roster[index] || owner->m_controllers[index] != roster[index].data()) { throw std::runtime_error("Native controller identity changed during serialization"); } }
	};
	// save settings of controllers
	QDomElement controllersNode = doc.createElement( "controllers" );
	element.appendChild( controllersNode );
	for (const auto& controller : roster)
	{
		ready();
		controller->saveState(doc, controllersNode);
		ready();
	}
}




void Song::restoreControllerStates( const QDomElement & element )
{
	const QPointer<Song> owner(this);
	Song* const ownerIdentity = this;
	std::vector<QPointer<Controller>> restored;
	for (auto controller : m_controllers) { restored.emplace_back(controller); }
	const auto ready = [&] {
		if (!owner) { throw std::runtime_error("Native project retired during controller restoration"); }
		for (const auto& controller : restored)
		{ if (!controller || !owner->containsController(controller.data())) { throw std::runtime_error("Native restored rack controller retired or detached"); } }
	};
	// Only element nodes occupy persisted controller indices. Comments and
	// formatting are not unavailable controllers and must not shift bindings.
	QDomElement node = element.firstChildElement();
	while( !node.isNull() && !isCancelled() )
	{
		ready(); QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } ready();
		const QPointer<Controller> c(Controller::create(node.toElement(), this));
		const bool owned = c && (c->type() == Controller::ControllerType::Lfo || c->type() == Controller::ControllerType::Midi);
		const auto cleanup = qScopeGuard([&] { if (owned && c && c->parent() == ownerIdentity && c->connectionCount() == 0 && (!owner || !owner->containsController(c.data()))) { delete c.data(); } });
		ready();
		if (c && c->type() != Controller::ControllerType::Dummy)
		{
			addController(c.data());
			if (!owner || !c || !owner->containsController(c.data())) { throw std::runtime_error("Native controller registration did not survive project restoration"); }
			restored.push_back(c);
		}
		else
		{
			// Every unavailable element occupies its own index, even though the
			// native dummy controller is shared and addController deduplicates it.
			m_controllers.push_back(Controller::create(Controller::ControllerType::Dummy, this));
			collectError(tr("Unavailable project controller"));
		}
		ready();

		node = node.nextSiblingElement();
	}
}



void Song::removeAllControllers()
{
	const QPointer<Song> owner(this);
	while (owner && !owner->m_controllers.empty())
	{
		removeController(m_controllers.at(0));
	}

	if (owner) { owner->m_controllers.clear(); }
}



void Song::saveScaleStates(QDomDocument &doc, QDomElement &element)
{
	const QPointer<Song> owner(this);
	std::vector<std::shared_ptr<Scale>> scales;
	for (unsigned int index = 0; index < MaxScaleCount; ++index) { scales.push_back(std::atomic_load(&m_scales[index])); }
	QDomElement scalesNode = doc.createElement("scales");
	element.appendChild(scalesNode);

	for (const auto& scale : scales)
	{
		scale->saveState(doc, scalesNode);
		if (!owner) { throw std::runtime_error("Native project retired during scale serialization"); }
	}
}


void Song::restoreScaleStates(const QDomElement &element)
{
	QDomElement node = element.firstChildElement("scale");

	for (auto i = std::size_t{0}; i < MaxScaleCount && !node.isNull() && !isCancelled(); i++)
	{
		auto scale = std::make_shared<Scale>();
		scale->restoreState(node);
		QString error;
		if (TuningFile::validateScale(*scale, error)) { std::atomic_store(&m_scales[i], scale); }
		else { collectError(tr("Invalid native scale %1: %2").arg(i).arg(error)); }
		node = node.nextSiblingElement("scale");
	}
	emit scaleListChanged(-1);
}


void Song::saveKeymapStates(QDomDocument &doc, QDomElement &element)
{
	const QPointer<Song> owner(this);
	std::vector<std::shared_ptr<Keymap>> keymaps;
	for (unsigned int index = 0; index < MaxKeymapCount; ++index) { keymaps.push_back(std::atomic_load(&m_keymaps[index])); }
	QDomElement keymapsNode = doc.createElement("keymaps");
	element.appendChild(keymapsNode);

	for (const auto& keymap : keymaps)
	{
		keymap->saveState(doc, keymapsNode);
		if (!owner) { throw std::runtime_error("Native project retired during keymap serialization"); }
	}
}


void Song::restoreKeymapStates(const QDomElement &element)
{
	QDomElement node = element.firstChildElement("keymap");

	for (auto i = std::size_t{0}; i < MaxKeymapCount && !node.isNull() && !isCancelled(); i++)
	{
		auto keymap = std::make_shared<Keymap>();
		keymap->restoreState(node);
		QString error;
		if (TuningFile::validateKeymap(*keymap, error)) { std::atomic_store(&m_keymaps[i], keymap); }
		else { collectError(tr("Invalid native keymap %1: %2").arg(i).arg(error)); }
		node = node.nextSiblingElement("keymap");
	}
	emit keymapListChanged(-1);
}


bool Song::exportProjectMidi(const std::filesystem::path& filePath) const
{
	// instantiate midi export plugin
	TrackContainer::TrackList const & tracks = this->tracks();
	TrackContainer::TrackList const & patternStoreTracks = Engine::patternStore()->tracks();

	std::unique_ptr<Plugin> plugin(Plugin::instantiate("midiexport", nullptr, nullptr));
	auto exf = dynamic_cast<ExportFilter*>(plugin.get());
	if (exf)
	{
		return exf->tryExport(tracks, patternStoreTracks, getTempo(), m_masterPitchModel.value(), filePath);
	}
	else
	{
		qDebug() << "failed to load midi export filter!";
	}
	return false;
}



void Song::updateFramesPerTick()
{
	Engine::updateFramesPerTick();
}




void Song::setModified()
{
	setModified(true);
}

void Song::setProjectFileName(QString const & projectFileName)
{
	if (m_fileName != projectFileName)
	{
		m_fileName = projectFileName;
		emit projectFileNameChanged();
	}
}

void Song::advanceReplacementGeneration()
{
	if (m_replacementGeneration == std::numeric_limits<std::uint64_t>::max()) { throw std::length_error("Native project replacement generation exhausted"); }
	++m_replacementGeneration;
}




bool Song::reportControllerNotificationFailure(std::exception_ptr failure) noexcept
{
	if (QThread::currentThread() != thread() || !failure) { return false; }
	for (auto frame = controllerNotificationFrame; frame; frame = frame->previous)
	{
		if (frame->owner.data() != this) { continue; }
		if (!frame->failure) { frame->failure = std::move(failure); }
		return true;
	}
	return false;
}

void Song::notifyControllerChange(ControllerNotification kind, Controller* controller)
{
	ControllerNotificationFrame frame(this);
	if (kind == ControllerNotification::Added) { emit controllerAdded(controller); }
	else if (kind == ControllerNotification::Removed) { emit controllerRemoved(controller); }
	else { emit controllerRosterChanged(); }
	frame.rethrow();
}

void Song::addController( Controller * controller )
{
	if (!m_controllerLifecycleContext || !controller || containsController(controller)) { return; }
	const auto engine = Engine::audioEngine();
	const auto changes = engine ? engine->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	const QPointer<Song> owner(this); const QPointer<Controller> target(controller);
	if (!target || target->isRetiring() || std::find(m_controllerRemovals.begin(), m_controllerRemovals.end(), target) != m_controllerRemovals.end()) { return; }
	m_controllers.push_back(controller);
	// Direct native retirement must not leave a dangling entry. Use identity
	// only after QObject destruction and a weak owner through its teardown.
	const bool sharedDummy = target->type() == Controller::ControllerType::Dummy
		&& target.data() == Controller::create(Controller::ControllerType::Dummy, nullptr);
	if (!sharedDummy)
	{
		QObject::connect(target.data(), &QObject::destroyed, m_controllerLifecycleContext.get(), [owner, controller] {
			if (!owner) { return; }
			try
			{
				owner->m_controllers.erase(std::remove(owner->m_controllers.begin(), owner->m_controllers.end(), controller), owner->m_controllers.end());
				owner->notifyControllerChange(ControllerNotification::Roster);
				if (owner) { owner->setModified(); }
			}
			catch (...)
			{
				// QObject destruction must finish inside Qt. An explicit removal
				// retains this error in its outer native destruction frame.
				if (!owner || !owner->reportControllerNotificationFailure(std::current_exception()))
				{ qWarning("Native controller roster notification failed during destruction"); }
			}
		}, Qt::DirectConnection);
	}
	if (!owner || !target || !owner->containsController(target.data())) { return; }
	owner->notifyControllerChange(ControllerNotification::Added, target.data());
	if (!owner || !target || !owner->containsController(target.data())) { return; }
	owner->notifyControllerChange(ControllerNotification::Roster);
	if (owner) { owner->setModified(); }
}

bool Song::containsController(const Controller* controller) const
{
	return controller && std::find(m_controllers.begin(), m_controllers.end(), controller) != m_controllers.end();
}




void Song::removeController( Controller * controller )
{
	if (!controller) { return; }
	const auto engine = Engine::audioEngine();
	const auto changes = engine ? engine->requestChangesGuard() : AudioEngine::RequestChangesGuard{};
	const auto found = std::find(m_controllers.begin(), m_controllers.end(), controller);
	if (found == m_controllers.end()) { return; }
	const QPointer<Song> owner(this); const QPointer<Controller> target(controller);
	if (!target) { return; }
	const bool alreadyRetiring = target->isRetiring();
	m_controllerRemovals.push_back(target);
	m_controllers.erase(found);
	const auto finish = qScopeGuard([owner, target] {
		if (owner) { std::erase_if(owner->m_controllerRemovals, [&](const auto& entry) { return !entry || entry == target; }); }
	});
	// Unavailable project slots use a process-wide dummy singleton. Removing
	// one slot must never destroy that shared fallback controller.
	const bool sharedDummy = !alreadyRetiring && target->type() == Controller::ControllerType::Dummy
		&& target.data() == Controller::create(Controller::ControllerType::Dummy, nullptr);
	const auto retire = [&] { if (!sharedDummy && !alreadyRetiring) { delete target.data(); } };
	const auto cleanup = qScopeGuard(retire);
	// An independently running destructor owns completion. Its registered
	// QObject hook will publish the final roster after all early detach slots.
	if (alreadyRetiring) { return; }
	if (!sharedDummy) { target->beginRetirement(); }
	if (owner && target) { owner->notifyControllerChange(ControllerNotification::Removed, target.data()); }
	if (owner) { owner->notifyControllerChange(ControllerNotification::Roster); }
	ControllerNotificationFrame destruction(this);
	retire();
	if (owner) { owner->setModified(); }
	destruction.rethrow();
}




void Song::clearErrors()
{
	if (!m_errors.isEmpty() || m_hasFatalErrors) { ++m_errorGeneration; }
	m_errors.clear();
	m_hasFatalErrors = false;
}



void Song::collectError( const QString error )
{
	const bool becameFatal = !m_hasFatalErrors; m_hasFatalErrors = true;
	++m_fatalErrorGeneration;
	auto& count = m_errors[error];
	// Error repetition can continue for the lifetime of an autonomous session.
	// Preserve the existing signed count format without overflowing it.
	if (count < std::numeric_limits<int>::max()) { ++count; ++m_errorGeneration; }
	else if (becameFatal) { ++m_errorGeneration; }
}

void Song::collectUnresolvedResource(const QString& path)
{
	if (path.isEmpty()) { collectError(tr("An unresolved native sample has no filename")); return; }
	auto& count = m_errors[tr("Unresolved sample (silent until relinked): %1").arg(path)];
	if (count < std::numeric_limits<int>::max()) { ++count; ++m_errorGeneration; }
}



bool Song::hasErrors()
{
	return !(m_errors.isEmpty());
}



QString Song::errorSummary()
{
	QString errors;

	auto i = m_errors.constBegin();
	while (i != m_errors.constEnd())
	{
		errors.append( i.key() );
		if( i.value() > 1 )
		{
			errors.append( tr(" (repeated %1 times)").arg( i.value() ) );
		}
		errors.append("\n");
		++i;
	}

	errors.prepend( "\n\n" );
	errors.prepend( tr( "The following errors occurred while loading: " ) );

	return errors;
}

bool Song::isSavingProject() const {
	return m_savingProject;
}


std::shared_ptr<const Scale> Song::getScale(unsigned int index) const
{
	if (index >= MaxScaleCount) {index = 0;}

	return std::atomic_load(&m_scales[index]);
}


std::shared_ptr<const Keymap> Song::getKeymap(unsigned int index) const
{
	if (index >= MaxKeymapCount) {index = 0;}

	return std::atomic_load(&m_keymaps[index]);
}


void Song::setScale(unsigned int index, std::shared_ptr<Scale> newScale)
{
	QString error;
	if (index >= MaxScaleCount || !newScale || !TuningFile::validateScale(*newScale, error))
	{
		collectError(error.isEmpty() ? tr("Invalid native scale slot or object") : error); return;
	}
	auto guard = Engine::audioEngine()->requestChangesGuard();
	std::atomic_store(&m_scales[index], newScale);
	emit scaleListChanged(index);
	setModified();
}


void Song::setKeymap(unsigned int index, std::shared_ptr<Keymap> newMap)
{
	QString error;
	if (index >= MaxKeymapCount || !newMap || !TuningFile::validateKeymap(*newMap, error))
	{
		collectError(error.isEmpty() ? tr("Invalid native keymap slot or object") : error); return;
	}
	auto guard = Engine::audioEngine()->requestChangesGuard();
	std::atomic_store(&m_keymaps[index], newMap);
	emit keymapListChanged(index);
	setModified();
}
} // namespace lmms
