/* Native per-track export isolation. SPDX-License-Identifier: GPL-2.0-or-later */
#include "TrackRenderScope.h"

#include <atomic>
#include <array>
#include <stdexcept>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QThread>
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "PatternStore.h"
#include "RemotePlugin.h"
#include "Song.h"
#include "Track.h"

namespace lmms
{
namespace { std::atomic_bool active{false}; }

bool TrackRenderScope::hasActiveSelection() { return active.load(); }

TrackRenderScope::TrackRenderScope(Track* selected, Eligibility eligibility)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
	{ throw std::invalid_argument("Track export selection requires the application thread"); }
	const QPointer<Song> song(Engine::getSong()); const QPointer<PatternStore> patterns(Engine::patternStore());
	const QPointer<AudioEngine> engine(Engine::audioEngine()); const QPointer<Track> chosen(selected);
	if (!song || !patterns || !engine || !chosen)
	{ throw std::invalid_argument("Track export requires an initialized native song and selected track"); }
	if (song->isExporting() || song->hasPendingExportCleanup() || RemotePlugin::hasActiveRenderContext())
	{ throw std::runtime_error("Track export selection cannot change during native rendering"); }
	if (active.exchange(true)) { throw std::runtime_error("Another native track export selection is active"); }
	bool installed = false;
	const auto releaseOnFailure = qScopeGuard([&] { if (!installed) { active = false; } });
	FileResourceIO::EventSuspension events;
	const auto guard = engine->requestChangesGuard();
	const auto generation = song->replacementGeneration(); const QString filename = song->projectFileName(); const auto rate = engine->outputSampleRate();
	const QPointer<BoolModel> mute(chosen->getMutedModel()), solo(chosen->getSoloModel());
	const auto muted = mute->storedValue(), soloed = solo->storedValue();
	struct OwnedTracks { QPointer<TrackContainer> owner; std::vector<QPointer<Track>> tracks; };
	std::array<OwnedTracks, 2> owners{{{song.data(), {}}, {patterns.data(), {}}}};
	// No callback runs while borrowing a native container iterator. Subsequent
	// polling visits only owned weak identities, never an invalidated live list.
	for (auto& entry : owners)
	{
		entry.tracks.reserve(entry.owner->tracks().size());
		for (auto track : entry.owner->tracks()) { entry.tracks.emplace_back(track); }
	}
	const auto checkActors = [&]
	{
		if (!engine || Engine::audioEngine() != engine.data() || engine->thread() != QThread::currentThread() || engine->outputSampleRate() != rate
			|| !song || Engine::getSong() != song.data() || song->thread() != QThread::currentThread() || song->replacementGeneration() != generation || song->projectFileName() != filename
			|| !patterns || Engine::patternStore() != patterns.data() || patterns->thread() != QThread::currentThread()
			|| !chosen || chosen->thread() != QThread::currentThread() || !mute || !solo || mute->storedValue() != muted || solo->storedValue() != soloed
			|| song->isExporting() || song->hasPendingExportCleanup() || RemotePlugin::hasActiveRenderContext())
		{ throw std::runtime_error("Native track export admission context changed"); }
	};
	const auto poll = [&]
	{
		checkActors(); QString error; const bool accepted = FileResourceIO::check(error); checkActors();
		if (!accepted) { throw std::runtime_error(error.toStdString()); }
	};
	checkActors();
	if (chosen->type() != Track::Type::Instrument && chosen->type() != Track::Type::Sample)
	{ throw std::invalid_argument("Individual native export selects an instrument or sample track"); }
	if (eligibility == Eligibility::RequireUnmuted)
	{
		const bool unavailable = chosen->isMutedByModel(); checkActors();
		if (unavailable) { throw std::invalid_argument("Selected native export track is muted; change its native mute/solo state explicitly first"); }
	}
	bool found = false; std::size_t checked = 0;
	for (const auto& entry : owners)
	{
		for (const auto& track : entry.tracks)
		{
			if ((checked++ & 127) == 0) { poll(); }
			if (!track || track->thread() != QThread::currentThread() || track->trackContainer() != entry.owner.data())
			{ throw std::runtime_error("Native track export membership changed"); }
			if (track == chosen) { found = true; continue; }
			if (track->type() == Track::Type::Instrument || track->type() == Track::Type::Sample) { m_excluded.emplace_back(track); }
		}
	}
	if (!found) { throw std::invalid_argument("Selected native export track is outside the song and pattern store"); }
	poll();
	// Validate complete final membership once, keeping admission linear rather
	// than rescanning the entire graph at each cancellation checkpoint.
	for (const auto& entry : owners)
	{
		if (!entry.owner || entry.owner->tracks().size() != entry.tracks.size()) { throw std::runtime_error("Native track export membership changed"); }
		for (std::size_t index = 0; index < entry.tracks.size(); ++index)
		{
			const auto& track = entry.tracks[index];
			if (!track || track->thread() != QThread::currentThread() || track->trackContainer() != entry.owner.data() || entry.owner->tracks()[index] != track.data())
			{ throw std::runtime_error("Native track export membership changed"); }
		}
	}
	m_selected = chosen;
	// Finish all allocation/admission before publishing any processing flags.
	for (const auto& track : m_excluded) { track->m_renderExcluded.store(true, std::memory_order_release); }
	m_selected->m_renderSelected.store(true, std::memory_order_release);
	installed = true;
}

TrackRenderScope::~TrackRenderScope()
{
	if (m_selected) { m_selected->m_renderSelected.store(false, std::memory_order_release); }
	for (const auto& track : m_excluded) { if (track) { track->m_renderExcluded.store(false, std::memory_order_release); } }
	active = false;
}
}
