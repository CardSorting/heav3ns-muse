/* Native hosted-plugin management. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_PLUGIN_HOST_H
#define LMMS_PLUGIN_HOST_H

#include <QString>
#include <QStringList>
#include <vector>
#include "lmms_export.h"

namespace lmms
{

// The owning instrument supplies this interface to its existing native engine.
// Call on the application thread with audio processing suspended. A failed
// mutation may need StudioRuntime's native checkpoint rollback.
class LMMS_EXPORT PluginHost
{
public:
	struct Entry
	{
		int index;
		QString format, name, label, path, maker, uniqueId;
		bool active;
	};
	struct Specification
	{
		QString format, path, name, label;
		QString binary = "native";
		qint64 uniqueId = 0;
	};
	struct Parameter
	{
		enum class Kind { Number, Integer, Boolean };
		int index = 0, nativeIndex = 0, scalePointCount = 0;
		QString name, symbol, unit, comment, group, text;
		Kind kind = Kind::Number;
		double value = 0, defaultValue = 0, minimum = 0, maximum = 0;
		double step = 0, smallStep = 0, largeStep = 0;
		bool enabled = false, output = false, writable = false, automatable = false;
		bool logarithmic = false, saved = true;
	};
	struct Choice { double value; QString label; };
	struct DiscoveryQuery
	{
		QString format, path, discoveryTool;
		QString binary = "native";
		int maximumPlugins = 4096, timeoutMs = 60000;
	};
	struct DiscoveredPlugin
	{
		Specification plugin;
		QString maker, hash;
		int category = 0;
		bool instrument = false;
		quint32 audioInputs = 0, audioOutputs = 0, midiInputs = 0, midiOutputs = 0;
		quint32 cvInputs = 0, cvOutputs = 0, parameterInputs = 0, parameterOutputs = 0;
	};
	struct DiscoveryPage
	{
		QString scan, state, error;
		int offset = 0, total = 0;
		bool hasMore = false;
		qint64 elapsedMs = 0;
		std::vector<DiscoveredPlugin> plugins;
	};
	struct PatchbayEndpoint
	{
		quint32 group = 0, port = 0;
		bool operator==(const PatchbayEndpoint&) const = default;
	};
	struct PatchbayRoute
	{
		PatchbayEndpoint source, destination;
		bool operator==(const PatchbayRoute&) const = default;
	};
	struct PatchbayClient { quint32 id; int plugin, icon; QString name; };
	struct PatchbayPort
	{
		enum class Signal { Audio, Midi, Cv };
		PatchbayEndpoint endpoint;
		int portGroup = 0;
		quint32 hints = 0;
		bool input = false;
		Signal signal = Signal::Audio;
		QString name;
	};
	struct PatchbayConnection { quint32 id; PatchbayRoute route; };
	struct PatchbayGraph
	{
		std::vector<PatchbayClient> clients;
		std::vector<PatchbayPort> ports;
		std::vector<PatchbayConnection> connections;
	};
	PluginHost();
	virtual ~PluginHost();
	virtual bool available() const = 0;
	virtual QString unavailableReason() const = 0;
	virtual QStringList formats() const = 0;
	virtual std::vector<Entry> plugins() const = 0;
	virtual int add(const Specification& plugin) = 0;
	virtual void remove(int index) = 0;
	virtual void rename(int index, const QString& name) = 0;
	virtual void move(int first, int second) = 0;
	virtual void setActive(int index, bool active) = 0;
	virtual bool parametersAvailable() const = 0;
	virtual QString parametersUnavailableReason() const = 0;
	virtual int parameterCount(int plugin) const = 0;
	virtual Parameter parameter(int plugin, int index) const = 0;
	virtual std::vector<Choice> parameterChoices(int plugin, int index, int offset, int limit) const = 0;
	// Return native readback. A void native setter may quantize or ignore a write.
	virtual Parameter setParameter(int plugin, int index, double value) = 0;
	virtual bool discoveryAvailable() const = 0;
	virtual QString discoveryUnavailableReason() const = 0;
	virtual DiscoveryPage startDiscovery(const DiscoveryQuery& query) = 0;
	virtual DiscoveryPage pollDiscovery(const QString& scan, int offset, int limit) = 0;
	virtual DiscoveryPage stopDiscovery(const QString& scan) = 0;
	virtual bool patchbayAvailable() const = 0;
	virtual QString patchbayUnavailableReason() const = 0;
	// Native refresh can remove illegal connections. Treat inspection as a
	// project mutation too, so the runtime checkpoints it before entering Carla.
	virtual PatchbayGraph patchbay() = 0;
	virtual PatchbayGraph connectPatchbay(const PatchbayRoute& route) = 0;
	virtual PatchbayGraph disconnectPatchbay(const PatchbayRoute& route) = 0;
};

} // namespace lmms
#endif
