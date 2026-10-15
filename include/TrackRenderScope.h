/* Native per-track export isolation. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_TRACK_RENDER_SCOPE_H
#define LMMS_TRACK_RENDER_SCOPE_H

#include <QPointer>
#include <vector>
#include "lmms_export.h"

namespace lmms
{
class Track;

// Owner-thread scope. Install before starting a native export and release only
// after its processing thread stops. User mute/solo models are never rewritten.
// Admission owns weak track snapshots across cancellable asset callbacks and
// validates final native membership before publishing any processing flags.
class LMMS_EXPORT TrackRenderScope
{
public:
	// Bulk export alone can retain eligibility captured before prior stems ran
	// native mute automation. This does not override the selected track's audio mute.
	enum class Eligibility { RequireUnmuted, PreviouslyAdmitted };
	explicit TrackRenderScope(Track* selected, Eligibility eligibility = Eligibility::RequireUnmuted);
	~TrackRenderScope();
	static bool hasActiveSelection();
	TrackRenderScope(const TrackRenderScope&) = delete;
	TrackRenderScope& operator=(const TrackRenderScope&) = delete;
private:
	QPointer<Track> m_selected;
	std::vector<QPointer<Track>> m_excluded;
};
}
#endif
