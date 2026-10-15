/* Native studio interruption bridge. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_SIGNALS_H
#define LMMS_STUDIO_SIGNALS_H

#include <memory>
#include <QString>
#include "lmms_export.h"

namespace lmms
{
// Own this on the application thread for the complete studio/engine lifetime.
// Handlers only latch an atomic flag; callers perform shutdown on their thread.
class LMMS_EXPORT StudioSignals
{
public:
	StudioSignals();
	~StudioSignals();
	StudioSignals(const StudioSignals&) = delete;
	StudioSignals& operator=(const StudioSignals&) = delete;
	bool install(QString& error);
	bool requested() const noexcept;
private:
	struct State;
	std::unique_ptr<State> m_state;
};
}
#endif
