/*
 * Song.h - class song - the root of the model-tree
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

#ifndef LMMS_SONG_H
#define LMMS_SONG_H

#include <array>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <vector>

#include <QString>
#include <QHash>  // IWYU pragma: keep
#include <QJsonArray>
#include <QPointer>

#include "AudioEngine.h"
#include "Controller.h"
#include "Metronome.h"
#include "lmms_constants.h"
#include "MeterModel.h"
#include "Timeline.h"
#include "TrackContainer.h"
#include "VstSyncController.h"

#include <filesystem> // QTBUG-73263

namespace lmms
{

class AutomationTrack;
class Keymap;
class MidiClip;
class Scale;

namespace gui
{
class SongEditor;
class ControllerRackView;
}


const bpm_t MinTempo = 10;
const bpm_t DefaultTempo = 140;
const bpm_t MaxTempo = 999;
const tick_t MaxSongLength = 9999 * DefaultTicksPerBar;


class LMMS_EXPORT Song : public TrackContainer
{
	Q_OBJECT
	mapPropertyFromModel( int,getTempo,setTempo,m_tempoModel );
	mapPropertyFromModel( int,masterPitch,setMasterPitch,m_masterPitchModel );
	mapPropertyFromModel( int,masterVolume,setMasterVolume, m_masterVolumeModel );
public:
	enum class PlayMode
	{
		None,
		Song,
		Pattern,
		MidiClip,
		AutomationClip,
		Count
	} ;
	constexpr static auto PlayModeCount = static_cast<std::size_t>(PlayMode::Count);

	struct SaveOptions {
		/**
		 * Should we discard MIDI ControllerConnections from project files?
		 */
		BoolModel discardMIDIConnections{false};
		/**
		 * Should we save the project as a project bundle? (with resources)
		 */
		BoolModel saveAsProjectBundle{false};

		void setDefaultOptions() {
			discardMIDIConnections.setValue(false);
			saveAsProjectBundle.setValue(false);
		}
	};

	void clearErrors();
	void collectError( const QString error );
	bool hasErrors();
	QString errorSummary();
	// Retained, silent sample references remain visible in native diagnostics but
	// do not invalidate a checkpoint. Every ordinary collectError stays fatal.
	void collectUnresolvedResource(const QString& path);
	bool hasFatalErrors() const { return m_hasFatalErrors; }
	std::uint64_t fatalErrorGeneration() const { return m_fatalErrorGeneration; }
	// Read under native model ownership; entries and offsets can change whenever
	// the generation changes, independently of persisted project revisions.
	const QHash<QString, int>& nativeErrors() const { return m_errors; }
	std::uint64_t errorGeneration() const { return m_errorGeneration; }
	bool releaseMidiOutput();
	QString midiOutputCleanupError() const { return m_midiOutputCleanupError; }
	// Read under the audio engine model guard. Counts completed transport buffers
	// across loops, independent of an external MIDI device's timestamp origin.
	// Callback-interrupted buffers can have effects before being abandoned.
	f_cnt_t playbackFrames() const { return m_playbackFrames; }
	std::uint64_t playbackGeneration() const { return m_playbackGeneration; }

	void processNextBuffer();

	inline int getLoadingTrackCount() const
	{
		return m_nLoadingTrack;
	}

	inline int getMilliseconds() const
	{
		return getMilliseconds(m_playMode);
	}

	//! Returns the elapsed milliseconds since the start of the song
	//! This function attempts to give the correct value even despite mid-song tempo changes
	inline int getMilliseconds(PlayMode playMode) const
	{
		return 1000 * getTimeline(playMode).getElapsedSeconds();
	}

	inline int getBars() const
	{
		return currentBar();
	}

	inline int ticksPerBar() const
	{
		return TimePos::ticksPerBar(m_timeSigModel);
	}

	// Returns the beat position inside the bar, 0-based
	inline int getBeat() const
	{
		return getPlayPos().getBeatWithinBar(m_timeSigModel);
	}
	// the remainder after bar and beat are removed
	inline int getBeatTicks() const
	{
		return getPlayPos().getTickWithinBeat(m_timeSigModel);
	}
	inline int getTicks() const
	{
		return currentTick();
	}
	inline f_cnt_t getFrames() const
	{
		return currentFrame();
	}
	inline bool isPaused() const
	{
		return m_paused;
	}

	inline bool isPlaying() const
	{
		return m_playing == true && m_exporting == false;
	}

	inline bool isStopped() const
	{
		return m_playing == false && m_paused == false;
	}

	inline bool isExporting() const
	{
		return m_exporting;
	}
	bool hasPendingExportCleanup() const { return m_exportCleanupPending; }

	inline void setExportLoop( bool exportLoop )
	{
		m_exportLoop = exportLoop;
	}
	bool exportLoop() const { return m_exportLoop; }

	inline bool isRecording() const
	{
		return m_recording;
	}

	inline void setLoopRenderCount(int count)
	{
		if (count < 1)
			m_loopRenderCount = 1;
		else
			m_loopRenderCount = count;
		m_loopRenderRemaining = m_loopRenderCount;
	}

	inline int getLoopRenderCount() const
	{
		return m_loopRenderCount;
	}

	bool isExportDone() const;
	int getExportProgress() const;

	inline void setRenderBetweenMarkers( bool renderBetweenMarkers )
	{
		m_renderBetweenMarkers = renderBetweenMarkers;
	}
	bool renderBetweenMarkers() const { return m_renderBetweenMarkers; }

	struct ExportRange
	{
		TimePos begin, end, loopBegin, loopEnd;
		qint64 effectiveTicks = 0;
	};
	// Native export semantics shared by the renderer and headless preflight.
	ExportRange exportRange(bool exportLoop, bool betweenMarkers, int loopCount) const;
	ExportRange lastExportRange() const { return {m_exportSongBegin, m_exportSongEnd, m_exportLoopBegin, m_exportLoopEnd, m_exportEffectiveLength}; }

	inline PlayMode playMode() const
	{
		return m_playMode;
	}
	const MidiClip* playingMidiClip() const { return m_playMode == PlayMode::MidiClip ? m_midiClipToPlay : nullptr; }
	bool midiClipLoopEnabled() const { return m_loopMidiClip; }

	const TimePos& getPlayPos(PlayMode pm) const
	{
		return getTimeline(pm).pos();
	}
	const TimePos& getPlayPos() const
	{
		return getPlayPos(m_playMode);
	}

	void setPlayPos(tick_t ticks, PlayMode playMode)
	{
		getTimeline(playMode).setTicks(ticks);
	}
	void setPlayPos(tick_t ticks)
	{
		setPlayPos(ticks, m_playMode);
	}

	auto getTimeline(PlayMode mode) -> Timeline& { return m_timelines[static_cast<std::size_t>(mode)]; }
	auto getTimeline(PlayMode mode) const -> const Timeline& { return m_timelines[static_cast<std::size_t>(mode)]; }
	auto getTimeline() -> Timeline& { return getTimeline(m_playMode); }
	auto getTimeline() const -> const Timeline& { return getTimeline(m_playMode); }

	void updateLength();
	bar_t length() const
	{
		return m_length;
	}


	bpm_t getTempo();

	AutomationTrack * globalAutomationTrack()
	{
		return m_globalAutomationTrack;
	}

	//TODO: Add Q_DECL_OVERRIDE when Qt4 is dropped
	AutomatedValueMap automatedValuesAt(TimePos time, int clipNum = -1) const override;

	// file management
	void createNewProject();
	void createEmptyProject();
	void createNewProjectFromTemplate( const QString & templ );
	// Returns false on rejection/cancellation without reporting success to headless callers.
	bool loadProject( const QString & filename );
	bool guiSaveProject();
	bool guiSaveProjectAs(const QString & filename);
	bool saveProjectFile(const QString & filename, bool withResources = false,
		bool absoluteLocalResources = false, QJsonArray* checkpointAssets = nullptr);
	std::uint64_t replacementGeneration() const noexcept { return m_replacementGeneration; }

	const QString & projectFileName() const
	{
		return m_fileName;
	}

	bool isLoadingProject() const
	{
		return m_loadingProject;
	}

	void loadingCancelled()
	{
		m_isCancelled = true;
		Engine::audioEngine()->clearNewPlayHandles();
	}

	bool isCancelled()
	{
		return m_isCancelled;
	}

	bool isModified() const
	{
		return m_modified;
	}

	QString nodeName() const override
	{
		return "song";
	}

	virtual bool fixedClips() const
	{
		return false;
	}

	void addController( Controller * c );
	void removeController( Controller * c );
	//! Identity-only roster query; does not dereference the supplied pointer.
	bool containsController(const Controller* controller) const;
	// Synchronous owner-thread lifecycle receivers catch locally and report
	// before returning to Qt. Accepted failures return through the native caller.
	bool reportControllerNotificationFailure(std::exception_ptr failure) noexcept;


	const ControllerVector & controllers() const
	{
		return m_controllers;
	}


	MeterModel & getTimeSigModel()
	{
		return m_timeSigModel;
	}

	IntModel& tempoModel()
	{
		return m_tempoModel;
	}

	bool exportProjectMidi(const std::filesystem::path& filePath) const;

	inline void setLoadOnLaunch(bool value) { m_loadOnLaunch = value; }
	SaveOptions &getSaveOptions() {
		return m_saveOptions;
	}

	bool isSavingProject() const;
	// Sharing/export preferences cannot discard state from a recovery checkpoint.
	bool shouldDiscardMidiConnections() const
	{
		return m_savingProject && !m_savingCheckpoint && m_saveOptions.discardMIDIConnections.value();
	}

	std::shared_ptr<const Scale> getScale(unsigned int index) const;
	std::shared_ptr<const Keymap> getKeymap(unsigned int index) const;
	void setScale(unsigned int index, std::shared_ptr<Scale> newScale);
	void setKeymap(unsigned int index, std::shared_ptr<Keymap> newMap);

	const std::string& syncKey() const noexcept { return m_vstSyncController.sharedMemoryKey(); }

	Metronome& metronome() { return m_metronome; }

public slots:
	void playSong();
	void record();
	void playAndRecord();
	void playPattern();
	void playMidiClip( const lmms::MidiClip * midiClipToPlay, bool loop = true );
	void togglePause();
	void stop();

	void startExport();
	void stopExport();


	void setModified();

	void clearProject();

	void addPatternTrack();


private slots:
	void insertBar();
	void removeBar();
	void addSampleTrack();
	void addAutomationTrack();

	void setTempo();
	void setTimeSignature();

	void masterVolumeChanged();

	void savePlayStartPosition();

	void updateFramesPerTick();



private:
	class CreationGuard;
	friend class ImportFilter;
	Song();
	Song( const Song & );
	~Song() override;


	inline bar_t currentBar() const
	{
		return getPlayPos(m_playMode).getBar();
	}

	inline tick_t currentTick() const
	{
		return getPlayPos(m_playMode).getTicks();
	}

	inline f_cnt_t currentFrame() const
	{
		return getTimeline(m_playMode).ticks() * Engine::framesPerTick() + getTimeline(m_playMode).frameOffset();
	}

	void saveControllerStates( QDomDocument & doc, QDomElement & element );
	void clearProjectImpl();
	void advanceReplacementGeneration();
	void restoreControllerStates( const QDomElement & element );

	void removeAllControllers();

	void saveScaleStates(QDomDocument &doc, QDomElement &element);
	void restoreScaleStates(const QDomElement &element);

	void saveKeymapStates(QDomDocument &doc, QDomElement &element);
	void restoreKeymapStates(const QDomElement &element);

	// False means transport context is unavailable or changed; abandon this tick.
	bool processAutomations(const std::vector<QPointer<Track>>& tracks, TimePos timeStart, f_cnt_t frames);
	void processMetronome(size_t bufferOffset);

	void setModified(bool value);

	void setProjectFileName(QString const & projectFileName);

	AutomationTrack * m_globalAutomationTrack;

	IntModel m_tempoModel;
	MeterModel m_timeSigModel;
	int m_oldTicksPerBar;
	IntModel m_masterVolumeModel;
	IntModel m_masterPitchModel;

	ControllerVector m_controllers;
	enum class ControllerNotification { Added, Removed, Roster };
	void notifyControllerChange(ControllerNotification kind, Controller* controller = nullptr);
	std::vector<QPointer<Controller>> m_controllerRemovals;
	// Disconnect roster hooks before Song fields and base-owned tracks die.
	std::unique_ptr<QObject> m_controllerLifecycleContext = std::make_unique<QObject>();

	int m_nLoadingTrack;

	QString m_fileName;
	// Retain presentation and extension data when a project is edited without views.
	QDomDocument m_preservedProjectData;
	QString m_oldFileName;
	bool m_modified;
	bool m_loadOnLaunch;

	volatile bool m_recording;
	volatile bool m_exporting;
	bool m_exportCleanupPending = false;
	class ExportTransitionGuard;
	bool m_exportTransitionActive = false;
	enum class ExportTransitionPoint { StartAfterTimeline, StartAfterPlayback, StopAfterTimeline, StopAfterPlayback };
	void exportTransitionCheckpoint(ExportTransitionPoint point);
	std::function<void(ExportTransitionPoint)> m_exportTransitionProbe;
	volatile bool m_exportLoop;
	volatile bool m_renderBetweenMarkers;
	volatile bool m_playing;
	volatile bool m_paused;

	bool m_savingProject;
	bool m_savingCheckpoint = false;
	bool m_loadingProject;
	bool m_loadingProjectFile = false;
	bool m_clearingProject = false;
	bool m_creatingProject = false;
	bool m_importingProject = false;
	bool m_permitCreationLoad = false;
	std::uint64_t m_replacementGeneration = 0;
	bool m_isCancelled;

	SaveOptions m_saveOptions;

	QHash<QString, int> m_errors;
	bool m_hasFatalErrors = false;
	std::uint64_t m_fatalErrorGeneration = 0;
	std::uint64_t m_errorGeneration = 0;
	QString m_midiOutputCleanupError;
	f_cnt_t m_playbackFrames = 0;
	std::uint64_t m_playbackGeneration = 0;

	std::array<Timeline, PlayModeCount> m_timelines;

	PlayMode m_playMode;
	bar_t m_length;

	const MidiClip* m_midiClipToPlay;
	QMetaObject::Connection m_playingMidiClipDestroyed;
	bool m_loopMidiClip;

	VstSyncController m_vstSyncController;

	int m_loopRenderCount;
	int m_loopRenderRemaining;
	TimePos m_exportSongBegin;
	TimePos m_exportLoopBegin;
	TimePos m_exportLoopEnd;
	TimePos m_exportSongEnd;
	qint64 m_exportEffectiveLength = 0;

	std::shared_ptr<Scale> m_scales[MaxScaleCount];
	std::shared_ptr<Keymap> m_keymaps[MaxKeymapCount];

	std::vector<QPointer<AutomatableModel>> m_oldAutomatedModels;

	Metronome m_metronome;

	friend class Engine;
	friend class SongAutomationOwnershipTestAccess;
	friend class StudioControllerLifecycleTestAccess;
	friend class NativeSongTestAccess;
	friend class SongExportTransitionTestAccess;
	friend class gui::SongEditor;
	friend class gui::ControllerRackView;

signals:
	void projectLoaded();
	void playbackStateChanged();
	void playbackPositionJumped();
	void lengthChanged( int bars );
	void tempoChanged( lmms::bpm_t newBPM );
	void timeSignatureChanged( int oldTicksPerBar, int ticksPerBar );
	void controllerAdded( lmms::Controller * );
	void controllerRemoved( lmms::Controller * );
	void controllerRosterChanged();
	void stopped();
	void modified();
	void projectFileNameChanged();
	void scaleListChanged(int index);
	void keymapListChanged(int index);
} ;


} // namespace lmms

#endif // LMMS_SONG_H
