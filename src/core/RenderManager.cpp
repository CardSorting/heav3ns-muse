/*
 * RenderManager - exporting logic common between the CLI and GUI.
 *
 * Copyright (c) 2015 Ryan Roden-Corrent <ryan/at/rcorre.net>
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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QThread>
#include <array>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

#include "RenderManager.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "Track.h"

#include "PatternStore.h"
#include "Song.h"
#include "RemotePlugin.h"
#include "TrackRenderScope.h"
#include "FileResourceIO.h"


namespace lmms
{
class NativeRenderOutput
{
public:
	NativeRenderOutput(const QString& destination, const QString& extension, const QPointer<AudioEngine>& engine, std::uint64_t reservation)
		: m_destination(QFileInfo(destination).absoluteFilePath()),
		m_engine(engine), m_reservation(reservation),
		m_stage(QFileInfo(m_destination).absoluteDir().filePath(".lmms-export-XXXXXX" + extension))
	{
		if (!m_stage.open()) { throw std::runtime_error("Native export staging could not be opened"); }
		m_stage.close();
	}
	~NativeRenderOutput()
	{
		// A callback can deliberately reuse this filename under a newer device
		// reservation. Retain uncertain leftovers instead of removing its output.
		m_stage.setAutoRemove(m_engine && Engine::audioEngine() == m_engine.data() && m_engine->audioDeviceReservationGeneration() == m_reservation);
	}
	QString path() const { return m_stage.fileName(); }
	bool publish(const std::function<bool()>& current, QString& error)
	{
		if (m_published) { return true; }
		FileResourceIO::Scope work([&] { return !current(); }, true);
		if (!current()) { error = "Native export publication was cancelled or superseded"; return false; }
		QFile source(path()); QSaveFile destination(m_destination); destination.setDirectWriteFallback(false);
		if (!source.open(QIODevice::ReadOnly) || source.size() < 1 || !destination.open(QIODevice::WriteOnly))
		{ error = "Native export publication could not open its source or destination"; return false; }
		if (!FileResourceIO::stream(source, &destination, nullptr, source.size(), error)) { return false; }
		if (!current()) { error = "Native export publication was cancelled or superseded"; return false; }
		if (!destination.commit()) { error = "Native export atomic publication failed"; return false; }
		m_published = true; error.clear(); return true;
	}
private:
	QString m_destination;
	QPointer<AudioEngine> m_engine;
	std::uint64_t m_reservation;
	QTemporaryFile m_stage;
	bool m_published = false;
};
namespace
{
// Preparation owns weak roster/model values across effective getters and asset
// callbacks. It does not retain native container iterators at those boundaries.
class ExportPreparation
{
public:
	ExportPreparation(RenderManager* owner, std::function<bool()> current) : m_owner(owner), m_current(std::move(current))
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()) { throw std::logic_error("Native export preparation requires the application thread"); }
		m_engine = Engine::audioEngine(); m_song = Engine::getSong(); m_patterns = Engine::patternStore();
		if (!m_engine || !m_song || !m_patterns) { throw std::runtime_error("Native export preparation requires initialized roots"); }
		m_changes.emplace(m_engine->requestChangesGuard()); m_project = m_song->replacementGeneration(); m_filename = m_song->projectFileName(); m_rate = m_engine->outputSampleRate();
		check();
		m_rosters[0].owner = m_song.data(); m_rosters[1].owner = m_patterns.data();
		for (auto& roster : m_rosters)
		{
			roster.rows.reserve(roster.owner->tracks().size());
			for (auto track : roster.owner->tracks())
			{
				if (!track || track->thread() != QThread::currentThread()) { throw std::runtime_error("Native export preparation found an absent or foreign track"); }
				roster.rows.push_back({track, track->getMutedModel(), track->getSoloModel(), track->getMutedModel()->storedValue(), track->getSoloModel()->storedValue()});
			}
		}
		check();
	}
	void check() const
	{
		if (!m_owner || m_owner->thread() != QThread::currentThread() || !m_current() || !m_engine || Engine::audioEngine() != m_engine.data() || m_engine->thread() != QThread::currentThread() || m_engine->outputSampleRate() != m_rate
			|| !m_song || Engine::getSong() != m_song.data() || m_song->thread() != QThread::currentThread() || m_song->replacementGeneration() != m_project || m_song->projectFileName() != m_filename
			|| !m_patterns || Engine::patternStore() != m_patterns.data() || m_patterns->thread() != QThread::currentThread() || m_song->isExporting() || m_song->hasPendingExportCleanup() || RemotePlugin::hasActiveRenderContext())
		{ throw std::runtime_error("Native export preparation context changed or was cancelled"); }
	}
	void verify() const
	{
		check();
		for (const auto& roster : m_rosters)
		{
			if (!roster.owner || roster.owner->tracks().size() != roster.rows.size()) { throw std::runtime_error("Native export track membership changed"); }
			for (std::size_t index = 0; index < roster.rows.size(); ++index)
			{
				const auto& row = roster.rows[index]; checkRow(row, roster.owner.data());
				if (roster.owner->tracks()[index] != row.track.data()) { throw std::runtime_error("Native export track order changed"); }
			}
		}
	}
	std::vector<QPointer<Track>> eligible()
	{
		std::vector<QPointer<Track>> result; std::size_t checked = 0;
		for (const auto& roster : m_rosters)
		{
			for (const auto& row : roster.rows)
			{
				if ((checked++ & 127) == 0) { poll(); }
				checkRow(row, roster.owner.data()); const auto type = row.track->type();
				if (type != Track::Type::Instrument && type != Track::Type::Sample) { continue; }
				const bool muted = row.track->isMuted(); check(); checkRow(row, roster.owner.data());
				if (!muted) { result.push_back(row.track); }
			}
		}
		poll(); verify(); return result;
	}
	QString name(const QPointer<Track>& track) const
	{
		poll(); verify(); if (!track) { throw std::runtime_error("Native export track was retired"); }
		bool found = false; for (const auto& roster : m_rosters) { for (const auto& row : roster.rows) { if (row.track == track) { found = true; } } }
		if (!found) { throw std::runtime_error("Native export track is outside the accepted rosters"); }
		const QString& reported = track->name(); verify();
		if (!track) { throw std::runtime_error("Native export track was retired"); }
		const QString owned(reported); poll(); verify(); return owned;
	}
private:
	struct Row { QPointer<Track> track; QPointer<BoolModel> mute, solo; float muted, soloed; };
	struct Roster { QPointer<TrackContainer> owner; std::vector<Row> rows; };
	static void checkRow(const Row& row, const TrackContainer* owner)
	{
		if (!row.track || row.track->thread() != QThread::currentThread() || row.track->trackContainer() != owner || !row.mute || !row.solo
			|| row.mute->storedValue() != row.muted || row.solo->storedValue() != row.soloed)
		{ throw std::runtime_error("Native export track ownership or stored eligibility changed"); }
	}
	void poll() const { check(); QString error; const bool accepted = FileResourceIO::check(error); check(); if (!accepted) { throw std::runtime_error(error.toStdString()); } }
	FileResourceIO::EventSuspension m_events;
	QPointer<RenderManager> m_owner; std::function<bool()> m_current;
	QPointer<AudioEngine> m_engine; QPointer<Song> m_song; QPointer<PatternStore> m_patterns;
	std::uint64_t m_project = 0; QString m_filename; sample_rate_t m_rate = 0;
	std::array<Roster, 2> m_rosters;
	std::optional<AudioEngine::RequestChangesGuard> m_changes;
};

bool ownsReservation(const QPointer<AudioEngine>& engine, std::uint64_t generation)
{
	return engine && Engine::audioEngine() == engine.data() && engine->audioDeviceReservationGeneration() == generation;
}

// A returned native callback may retire the manager while a local renderer is
// still executing. Finish that callee before releasing its context and device.
void recoverOrphanedExport(std::shared_ptr<ProjectRenderer>& renderer, const QPointer<AudioEngine>& engine,
	std::uint64_t reservation, const QString& path, bool opened, bool removeOutput)
{
	if (renderer) { renderer->abortProcessing(); renderer.reset(); }
	FileResourceIO::Suspension recovery;
	if (ownsReservation(engine, reservation)) { engine->restoreAudioDevice(); }
	// Renderer destruction can start a newer reservation through a Qt callback.
	if (opened && removeOutput && ownsReservation(engine, reservation)) { QFile::remove(path); }
}
}

RenderManager::RenderManager(
	const OutputSettings& outputSettings, ProjectRenderer::ExportFileFormat fmt, QString outputPath)
	: m_outputSettings(outputSettings)
	, m_format(fmt)
	, m_outputPath(outputPath)
	, m_rendererFailureMessage(tr("Native audio renderer creation or setup failed."))
{}

RenderManager::~RenderManager()
{
	m_retiring = true; m_cancelled = true;
	const QPointer<AudioEngine> engine(Engine::audioEngine()); const auto reservation = m_deviceReservation;
	const QString path = m_activeOutputPath;
	const bool restore = std::exchange(m_deviceStored, false);
	auto renderer = std::move(m_activeRenderer); auto selection = std::move(m_trackSelection); auto output = std::move(m_outputTransaction);
	const bool removeOutput = renderer && renderer->isReady() && !m_outputFinalized;
	m_tracksToRender.clear(); m_exportTracks.clear();
	if (renderer) { disconnect(renderer.get(), nullptr, this, nullptr); renderer->abortProcessing(); }
	selection.reset(); renderer.reset();
	if (restore && ownsReservation(engine, reservation))
	{
		FileResourceIO::Suspension recovery; const bool restored = engine->restoreAudioDevice();
		if (restored && removeOutput && ownsReservation(engine, reservation)) { QFile::remove(path); }
	}
	output.reset();
}

void RenderManager::abortProcessing()
{
	if (m_retiring) { return; }
	const QPointer<RenderManager> owner(this); const auto renderer = m_activeRenderer;
	m_cancelled = true;
	if (renderer) {
		disconnect(renderer.get(), SIGNAL(finished()), this, nullptr);
		renderer->abortProcessing();
	}
	if (owner && !owner->m_retiring) { owner->clearTrackSelection(); }
}

// Called to render each new track when rendering tracks individually.
void RenderManager::renderNextTrack()
{
	if (m_starting || m_finishing || m_retiring) { return; }
	const QPointer<RenderManager> owner(this); bool proceed = false;
	{
		m_finishing = true; auto finishPhase = qScopeGuard([owner] { if (owner && !owner->m_retiring) { owner->m_finishing = false; } });
		const QPointer<AudioEngine> engine(Engine::audioEngine()); const auto reservation = m_deviceReservation;
		const QString path = m_activeOutputPath; auto renderer = m_activeRenderer; auto output = m_outputTransaction;
		const bool opened = renderer && renderer->isReady(); bool encoded = false, removeOutput = false;
		auto orphanRecovery = qScopeGuard([&] { if (!owner) { recoverOrphanedExport(renderer, engine, reservation, path, opened, removeOutput || !encoded); } });
		QString finalizationError;
		const auto publish = [&]
		{
			const QPointer<Song> song(Engine::getSong()); const auto project = song ? song->replacementGeneration() : 0; const QString filename = song ? song->projectFileName() : QString{};
			const QPointer<ProjectRenderer> expected(renderer.get()); QString error; bool published = false;
			try
			{
				published = output && output->publish([&] {
					return owner && !owner->m_retiring && !owner->m_cancelled && expected && owner->m_activeRenderer.get() == expected.data()
						&& ownsReservation(engine, reservation) && !expected->hasDspFailure() && song && Engine::getSong() == song.data()
						&& song->replacementGeneration() == project && song->projectFileName() == filename;
				}, error);
			}
			catch (...) { error = tr("Native export publication raised an exception."); }
			if (!owner) { return false; }
			if (!published) { owner->m_error = owner->m_cancelled ? tr("Audio export was cancelled.") : error.isEmpty() ? tr("Native export output is unavailable.") : error; owner->m_tracksToRender.clear(); }
			return published;
		};
		if (renderer)
		{
			renderer->wait();
			try { encoded = opened && renderer->finalizeOutput(); }
			catch (...) { finalizationError = tr("Native audio output finalization raised an exception."); }
			if (!owner) { return; }
			owner->m_outputFinalized = encoded;
			if (owner->m_cancelled || !encoded)
			{
				owner->m_error = owner->m_cancelled ? tr("Audio export was cancelled.")
					: owner->m_setupFailed ? owner->m_rendererFailureMessage
					: renderer->wasAborted() ? tr("Audio export was cancelled.")
					: !finalizationError.isEmpty() ? finalizationError
					: renderer->hasPluginFailure() ? tr("A remote plugin failed during export.")
					: renderer->hasDspFailure() ? tr("Native DSP failed during export.")
					: renderer->hasExecutionFailure() ? tr("Native export setup or processing raised an exception.")
					: renderer->hasEncodingFailure() ? tr("Native audio encoding or output writing failed during export.")
					: tr("Audio export could not open or finalize its output.");
				owner->m_tracksToRender.clear(); removeOutput = opened;
			}
		}
		if (owner->m_tracksToRender.empty() || owner->m_cancelled)
		{
			owner->clearTrackSelection();
			if (owner->m_deviceStored)
			{
				const bool owned = ownsReservation(engine, reservation);
				FileResourceIO::Suspension recovery;
				const bool restored = owned && engine->restoreAudioDevice();
				if (!owner) { return; }
				if (restored || !owned) { owner->m_deviceStored = false; }
				if (!owned) { owner->m_error = tr("Native export audio reservation was superseded."); }
				else if (!restored) { owner->m_error += tr(" Native audio device restoration failed; retry is required."); }
			}
			if (owner->m_cancelled) { owner->m_error = tr("Audio export was cancelled."); removeOutput = opened; }
			if (renderer && renderer->hasDspFailure()) { owner->m_error = tr("Native DSP failed during export or device restoration."); }
			if (renderer && encoded && owner->m_error.isEmpty()) { publish(); if (!owner) { return; } }
			owner->m_activeRenderer.reset(); renderer.reset();
			if (!owner) { return; }
			if (owner->m_cancelled) { owner->m_error = tr("Audio export was cancelled."); removeOutput = opened; }
			if (removeOutput && ownsReservation(engine, reservation)) { QFile::remove(path); }
			owner->m_outputTransaction.reset(); output.reset();
			// Completion may synchronously start another job. Do not let this
			// frame's guards change that new job or retry its device reservation.
			orphanRecovery.dismiss(); owner->m_finishing = false; finishPhase.dismiss();
			emit owner->finished(); return;
		}
		if (renderer && encoded && !publish()) { if (!owner) { return; } }
		if (!owner) { return; }
		owner->m_activeRenderer.reset(); renderer.reset();
		if (!owner) { return; }
		owner->m_trackSelection.reset(); proceed = !owner->m_cancelled && !owner->m_tracksToRender.empty();
		owner->m_outputTransaction.reset(); output.reset();
		orphanRecovery.dismiss();
	}
	if (!owner) { return; }
	if (!proceed) { owner->renderNextTrack(); return; }
	{
		// pop the next track from our rendering queue
		const QPointer<Track> renderTrack = m_tracksToRender.back();
		m_tracksToRender.pop_back();
		QString outputPath; bool prepared = false;
		{
			m_starting = true; const auto finishStarting = qScopeGuard([owner] { if (owner && !owner->m_retiring) { owner->m_starting = false; } });
			try
			{
				auto selection = std::make_unique<TrackRenderScope>(renderTrack.data(), TrackRenderScope::Eligibility::PreviouslyAdmitted);
				if (!owner) { return; }
				if (owner->m_cancelled) { throw std::runtime_error("Native track export was cancelled"); }
				owner->m_trackSelection = std::move(selection);
				// Preserve the existing reverse queue numbering and sanitized names.
				outputPath = owner->pathForTrack(renderTrack.data(), owner->m_tracksToRender.size() + 1);
				if (!owner) { return; }
				prepared = true;
			}
			catch (const std::exception& error)
			{
				if (!owner) { return; } owner->m_error = owner->m_cancelled ? tr("Audio export was cancelled.") : QString::fromUtf8(error.what());
			}
			catch (...)
			{
				if (!owner) { return; } owner->m_error = owner->m_cancelled ? tr("Audio export was cancelled.") : tr("Native track export selection or naming failed.");
			}
		}
		if (!owner) { return; }
		if (!prepared) { owner->clearTrackSelection(); owner->renderNextTrack(); return; }
		owner->render(outputPath);
	}
}

// Render the song into individual tracks
void RenderManager::renderTracks()
{
	if (!prepareRender()) { return; }
	const QPointer<RenderManager> owner(this); bool prepared = false;
	{
		m_starting = true; const auto finishStarting = qScopeGuard([owner] { if (owner && !owner->m_retiring) { owner->m_starting = false; } });
		try
		{
			ExportPreparation work(owner.data(), [owner] { return owner && !owner->m_retiring && !owner->m_cancelled; });
			auto eligible = work.eligible(); if (!owner) { return; }
			owner->m_exportTracks = std::move(eligible); owner->m_tracksToRender = owner->m_exportTracks; prepared = true;
		}
		catch (const std::exception& error) { if (!owner) { return; } owner->m_error = owner->m_cancelled ? tr("Audio export was cancelled.") : QString::fromUtf8(error.what()); }
		catch (...) { if (!owner) { return; } owner->m_error = owner->m_cancelled ? tr("Audio export was cancelled.") : tr("Native track export eligibility failed."); }
	}
	if (!owner) { return; }
	if (!prepared) { owner->clearTrackSelection(); }
	owner->renderNextTrack();
}

// Render the song into a single track
void RenderManager::renderProject()
{
	if (!prepareRender()) { return; }
	render( m_outputPath );
}

bool RenderManager::prepareRender()
{
	if (m_retiring || m_starting || m_finishing) { return false; }
	if (m_deviceStored && !m_activeRenderer && (!Engine::audioEngine()->hasStoredAudioDevice() || Engine::audioEngine()->audioDeviceReservationGeneration() != m_deviceReservation)) { m_deviceStored = false; }
	if (m_deviceStored || m_activeRenderer) { return false; }
	m_error.clear(); m_cancelled = false; m_setupFailed = false;
	if (RemotePlugin::hasActiveRenderContext() || Engine::audioEngine()->hasStoredAudioDevice() || TrackRenderScope::hasActiveSelection())
	{
		m_error = tr("Another native export is active.");
		emit finished();
		return false;
	}
	if (!Engine::audioEngine()->storeAudioDevice())
	{
		m_error = tr("Another native export is active."); emit finished(); return false;
	}
	m_deviceStored = true;
	m_deviceReservation = Engine::audioEngine()->audioDeviceReservationGeneration();
	return true;
}

std::shared_ptr<ProjectRenderer> RenderManager::createRenderer(const OutputSettings& settings,
	ProjectRenderer::ExportFileFormat format, const QString& outputPath)
{
	return std::make_shared<ProjectRenderer>(settings, format, outputPath);
}

void RenderManager::render(QString outputPath)
{
	const QPointer<RenderManager> owner(this); const QPointer<AudioEngine> engine(Engine::audioEngine());
	const auto reservation = m_deviceReservation; const auto settings = m_outputSettings; const auto format = m_format;
	const QString failureMessage = m_rendererFailureMessage;
	std::shared_ptr<ProjectRenderer> renderer; std::shared_ptr<NativeRenderOutput> output; QString staging;
	bool opened = false, started = false;
	auto orphanRecovery = qScopeGuard([&] { if (!owner) { recoverOrphanedExport(renderer, engine, reservation, staging, opened, true); } });
	{
		m_starting = true; const auto finishStarting = qScopeGuard([owner] { if (owner && !owner->m_retiring) { owner->m_starting = false; } });
		m_outputFinalized = false; m_setupFailed = false;
		bool failed = false;
		try
		{
			output = std::make_shared<NativeRenderOutput>(outputPath, ProjectRenderer::getFileExtensionFromFormat(format), engine, reservation); staging = output->path();
			if (!owner) { return; } owner->m_activeOutputPath = staging; owner->m_outputTransaction = output;
			renderer = owner->createRenderer(settings, format, staging); opened = renderer && renderer->isReady();
			if (!owner) { return; }
			if (!renderer || !ownsReservation(engine, reservation)) { failed = true; }
			else
			{
				owner->m_activeRenderer = renderer;
				if (owner->m_cancelled) { renderer->abortProcessing(); }
				else if (opened)
				{
					const QPointer<ProjectRenderer> expected(renderer.get());
					connect(renderer.get(), &ProjectRenderer::progressChanged, owner.data(), [owner, expected](int progress) {
						if (owner && expected && owner->m_activeRenderer.get() == expected.data()) { emit owner->progressChanged(progress); }
					}, Qt::QueuedConnection);
					connect(renderer.get(), &QThread::finished, owner.data(), [owner, expected] {
						if (owner && expected && owner->m_activeRenderer.get() == expected.data()) { owner->renderNextTrack(); }
					}, Qt::QueuedConnection);
					started = renderer->startProcessing();
				}
			}
		}
		catch (...) { failed = true; }
		if (!owner) { return; }
		if (failed)
		{
			if (renderer) { disconnect(renderer.get(), SIGNAL(finished()), owner.data(), nullptr); renderer->abortProcessing(); }
			if (!owner) { return; }
			owner->m_activeRenderer = renderer; owner->m_setupFailed = true;
			// This cached QString copy does not construct exception diagnostics
			// while recovering from a failed native allocation.
			owner->m_error = failureMessage; owner->m_tracksToRender.clear(); started = false;
		}
	}
	if (!owner) { return; }
	// The member now owns a returned setup. Release the temporary pin before
	// failure cleanup so an unattached file device can close before removal.
	output.reset(); renderer.reset(); orphanRecovery.dismiss();
	if (!started) { owner->renderNextTrack(); }
}

// Called only after the native processing thread has stopped.
void RenderManager::clearTrackSelection()
{
	m_trackSelection.reset(); m_tracksToRender.clear(); m_exportTracks.clear();
}

// Determine the output path for a track when rendering tracks individually
QString RenderManager::pathForTrack(const Track *track, int num)
{
	const QPointer<RenderManager> owner(this); const QPointer<Track> selected(const_cast<Track*>(track));
	const QString directory = m_outputPath; const QString extension = ProjectRenderer::getFileExtensionFromFormat(m_format);
	ExportPreparation work(owner.data(), [owner] { return owner && !owner->m_retiring && !owner->m_cancelled; });
	QString name = work.name(selected);
	name = name.remove(QRegularExpression(FILENAME_FILTER));
	name = QString( "%1_%2%3" ).arg( num ).arg( name ).arg( extension );
	return QDir(directory).filePath(name);
}

void RenderManager::updateConsoleProgress()
{
	if (m_retiring) { return; }
	const QPointer<RenderManager> owner(this); const auto renderer = m_activeRenderer;
	if (renderer)
	{
		renderer->updateConsoleProgress(); if (!owner) { return; }

		int totalNum = m_exportTracks.size();
		if ( totalNum > 0 )
		{
			// we are rendering multiple tracks, append a track counter to the output
			int trackNum = totalNum - m_tracksToRender.size();
			fprintf( stderr, "(%d/%d)", trackNum, totalNum );
		}
	}
}


} // namespace lmms
