/* Read-only native track serialization source checks. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_TRACK_SAVE_SNAPSHOT_H
#define LMMS_NATIVE_TRACK_SAVE_SNAPSHOT_H
#include <memory>
#include "lmms_export.h"
namespace lmms
{
class Track;
class Song;
// Project-wide admission and publication checks for native identities and exposed parameters.
// Opaque plugin bytes and non-model clip contents are not immutable snapshots.
class LMMS_EXPORT NativeProjectSaveSnapshot
{
public:
	// Caller retains native audio/model ownership through validation and file publication.
	explicit NativeProjectSaveSnapshot(Song* song);
	~NativeProjectSaveSnapshot();
	void validate() const;
private:
	struct State;
	std::unique_ptr<State> m_state;
};
class LMMS_EXPORT NativeTrackSaveSnapshot
{
public:
	// Native audio/model ownership must span capture, serialization and checks.
	explicit NativeTrackSaveSnapshot(Track* track, bool includeClips);
	~NativeTrackSaveSnapshot();
	NativeTrackSaveSnapshot(const NativeTrackSaveSnapshot&) = delete;
	NativeTrackSaveSnapshot& operator=(const NativeTrackSaveSnapshot&) = delete;
	void validate(bool complete = true) const;
private:
	struct State;
	std::unique_ptr<State> m_state;
};
}
#endif
