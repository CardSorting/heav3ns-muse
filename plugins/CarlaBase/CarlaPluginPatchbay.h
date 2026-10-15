/* Native internal Carla graph. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_CARLA_PLUGIN_PATCHBAY_H
#define LMMS_CARLA_PLUGIN_PATCHBAY_H

#include <functional>
#include <memory>
#include <QtGlobal>
#include "PluginHost.h"

namespace lmms
{

// Construct while the native engine is empty and inactive. The owner suspends
// processing for each application-thread call, calls prepareForEngineCleanup,
// and retains this object until native cleanup has stopped its runners. Direct
// destruction without that sequence requires a quiescent native engine.
class CarlaPluginPatchbay
{
public:
	using Resolver = std::function<QFunctionPointer(const char*)>;
	CarlaPluginPatchbay(void* host, bool patchbay, const Resolver& resolver);
	~CarlaPluginPatchbay();
	// Before native cleanup: stop observation without replacing a callback slot
	// read by native runner threads. Keep this object alive until cleanup returns.
	void prepareForEngineCleanup();
	bool available() const;
	QString unavailableReason() const;
	PluginHost::PatchbayGraph inspect();
	PluginHost::PatchbayGraph connect(const PluginHost::PatchbayRoute& route);
	PluginHost::PatchbayGraph disconnect(const PluginHost::PatchbayRoute& route);
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace lmms
#endif
