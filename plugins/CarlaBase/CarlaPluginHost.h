/* Existing Carla engine management. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_CARLA_PLUGIN_HOST_H
#define LMMS_CARLA_PLUGIN_HOST_H

#include <functional>
#include <memory>
#include <QtGlobal>
#include <CarlaNative.h>
#include "PluginHost.h"

namespace lmms
{

class CarlaPluginHost final : public PluginHost
{
public:
	using Resolver = std::function<QFunctionPointer(const char*)>;
	// Native flattened slots before the edit map to slots afterward; -1 removes
	// the old target. The owner executes the edit synchronously under its guard.
	struct LayoutChange { bool known = false; std::vector<int> oldToNew; };
	using LayoutEdit = std::function<void(const LayoutChange&, const std::function<void()>&)>;
	// A resolver allows the same native ABI to be exercised without loading a
	// third-party engine in contract tests. Production resolves the Carla library.
	CarlaPluginHost(const NativePluginDescriptor* descriptor, NativePluginHandle instrument,
		const QString& library, const Resolver& resolver = {}, const LayoutEdit& layoutEdit = {}, bool patchbay = false);
	~CarlaPluginHost() override;
	// Release the wrapper/scanner before engine cleanup, retaining callback
	// storage and its library until the native engine has stopped its runners.
	void prepareForEngineCleanup();
	bool available() const override;
	QString unavailableReason() const override;
	QStringList formats() const override;
	std::vector<Entry> plugins() const override;
	int add(const Specification& plugin) override;
	void remove(int index) override;
	void rename(int index, const QString& name) override;
	void move(int first, int second) override;
	void setActive(int index, bool active) override;
	bool parametersAvailable() const override;
	QString parametersUnavailableReason() const override;
	int parameterCount(int plugin) const override;
	Parameter parameter(int plugin, int index) const override;
	std::vector<Choice> parameterChoices(int plugin, int index, int offset, int limit) const override;
	Parameter setParameter(int plugin, int index, double value) override;
	bool discoveryAvailable() const override;
	QString discoveryUnavailableReason() const override;
	DiscoveryPage startDiscovery(const DiscoveryQuery& query) override;
	DiscoveryPage pollDiscovery(const QString& scan, int offset, int limit) override;
	DiscoveryPage stopDiscovery(const QString& scan) override;
	bool patchbayAvailable() const override;
	QString patchbayUnavailableReason() const override;
	PatchbayGraph patchbay() override;
	PatchbayGraph connectPatchbay(const PatchbayRoute& route) override;
	PatchbayGraph disconnectPatchbay(const PatchbayRoute& route) override;
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace lmms
#endif
