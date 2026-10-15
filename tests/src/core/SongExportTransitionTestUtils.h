/* Native export fault injection after Qt calls return. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_TEST_SONG_EXPORT_TRANSITION_UTILS_H
#define LMMS_TEST_SONG_EXPORT_TRANSITION_UTILS_H
#include <QPointer>
#include <functional>
#include <utility>
#include "Song.h"
namespace lmms
{
class SongExportTransitionTestAccess
{
public:
	using Point = Song::ExportTransitionPoint;
	using Probe = std::function<void(Point)>;
	static Probe exchange(Song* song, Probe probe) { return std::exchange(song->m_exportTransitionProbe, std::move(probe)); }
	static bool active(Song* song) { return song->m_exportTransitionActive; }
};
using SongExportPoint = SongExportTransitionTestAccess::Point;
// Test-only owner-thread scopes; native renderer entry can invoke the probe on
// its rendering thread. Probe state must obey that actual thread ownership.
class SongExportTransitionProbe
{
public:
	SongExportTransitionProbe(Song* song, SongExportTransitionTestAccess::Probe probe)
		: m_song(song), m_previous(SongExportTransitionTestAccess::exchange(song, std::move(probe))) { }
	~SongExportTransitionProbe() { reset(); }
	SongExportTransitionProbe(const SongExportTransitionProbe&) = delete;
	SongExportTransitionProbe& operator=(const SongExportTransitionProbe&) = delete;
	void reset() { if (m_song) { SongExportTransitionTestAccess::exchange(m_song.data(), std::move(m_previous)); m_song.clear(); } }
private:
	QPointer<Song> m_song;
	SongExportTransitionTestAccess::Probe m_previous;
};
}
#endif
