/* Native Carla plugin discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_CARLA_PLUGIN_DISCOVERY_H
#define LMMS_CARLA_PLUGIN_DISCOVERY_H

#include <functional>
#include <memory>
#include <QtGlobal>
#include "PluginHost.h"

namespace lmms
{
class CarlaPluginDiscovery
{
public:
	using Resolver = std::function<QFunctionPointer(const char*)>;
	CarlaPluginDiscovery(const QString& libraryPath, const Resolver& resolver);
	~CarlaPluginDiscovery();
	bool available() const;
	QString unavailableReason() const;
	PluginHost::DiscoveryPage start(const PluginHost::DiscoveryQuery& query);
	PluginHost::DiscoveryPage poll(const QString& scan, int offset, int limit);
	PluginHost::DiscoveryPage stop(const QString& scan);
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace lmms
#endif
