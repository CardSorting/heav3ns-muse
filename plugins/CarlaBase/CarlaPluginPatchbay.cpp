/* Native internal Carla graph. SPDX-License-Identifier: GPL-2.0-or-later */
#include "CarlaPluginPatchbay.h"
#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
#include <CarlaHost.h>
#endif

namespace lmms
{
namespace
{
[[noreturn]] void patchbayError(const QString& message) { throw std::runtime_error(message.toStdString()); }
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
using namespace CARLA_BACKEND_NAMESPACE;
using Endpoint = PluginHost::PatchbayEndpoint;
using Route = PluginHost::PatchbayRoute;
using Graph = PluginHost::PatchbayGraph;
using Port = PluginHost::PatchbayPort;
auto endpointKey(const Endpoint& endpoint) { return std::pair{endpoint.group, endpoint.port}; }
auto routeKey(const Route& route)
{
	return std::tuple{route.source.group, route.source.port, route.destination.group, route.destination.port};
}
quint32 connectionNumber(const QString& text)
{
	if (text.isEmpty() || std::any_of(text.begin(), text.end(), [](QChar c) { return c < QChar('0') || c > QChar('9'); }))
	{
		patchbayError("Carla returned malformed patchbay connection endpoints");
	}
	bool valid = false; const auto value = text.toULongLong(&valid);
	if (!valid || value > std::numeric_limits<quint32>::max()) { patchbayError("Carla patchbay endpoint exceeds its native range"); }
	return static_cast<quint32>(value);
}
void validateGraph(const Graph& graph)
{
	if (graph.clients.empty()) { patchbayError("Carla did not emit an internal patchbay graph"); }
	std::set<quint32> clients, connections;
	std::map<std::pair<quint32, quint32>, const Port*> ports;
	std::set<decltype(routeKey(Route{}))> routes;
	for (const auto& client : graph.clients)
	{
		if (client.plugin < -1 || !clients.insert(client.id).second) { patchbayError("Carla returned invalid or duplicate patchbay clients"); }
	}
	for (const auto& port : graph.ports)
	{
		if (!clients.count(port.endpoint.group) || !ports.emplace(endpointKey(port.endpoint), &port).second)
		{
			patchbayError("Carla returned duplicate ports or a port without a patchbay client");
		}
	}
	for (const auto& connection : graph.connections)
	{
		const auto source = ports.find(endpointKey(connection.route.source));
		const auto destination = ports.find(endpointKey(connection.route.destination));
		if (!connections.insert(connection.id).second || !routes.insert(routeKey(connection.route)).second
			|| source == ports.end() || destination == ports.end()
			|| source->second->input || !destination->second->input || source->second->signal != destination->second->signal)
		{
			patchbayError("Carla returned an invalid or duplicate patchbay connection");
		}
	}
}
void validateRoute(const Graph& graph, const Route& route)
{
	const auto find = [&](const Endpoint& endpoint)
	{
		return std::find_if(graph.ports.begin(), graph.ports.end(), [&](const auto& port) { return port.endpoint == endpoint; });
	};
	const auto source = find(route.source), destination = find(route.destination);
	if (source == graph.ports.end() || destination == graph.ports.end()) { patchbayError("Patchbay endpoint does not exist; inspect the current graph"); }
	if (source->input || !destination->input) { patchbayError("Patchbay routes must run from an output to an input"); }
	if (source->signal != destination->signal) { patchbayError("Patchbay endpoints must carry the same signal type"); }
}
auto findConnection(const Graph& graph, const Route& route)
{
	return std::find_if(graph.connections.begin(), graph.connections.end(), [&](const auto& connection) { return connection.route == route; });
}
#endif
}

struct CarlaPluginPatchbay::Impl
{
	QString failure = "This Carla build does not expose native patchbay routing";
	const std::thread::id owner = std::this_thread::get_id();
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	CarlaHostHandle host = nullptr;
#define PATCHBAY_FUNCTION(name) decltype(&carla_##name) name = nullptr
	PATCHBAY_FUNCTION(set_engine_callback);
	PATCHBAY_FUNCTION(patchbay_refresh);
	PATCHBAY_FUNCTION(patchbay_connect);
	PATCHBAY_FUNCTION(patchbay_disconnect);
	PATCHBAY_FUNCTION(get_last_error);
#undef PATCHBAY_FUNCTION
	Graph graph;
	std::atomic<bool> collecting{false}, callbackFailed{false};
	bool attached = false;
	void detach()
	{
		collecting.store(false);
		if (attached) { set_engine_callback(host, nullptr, nullptr); attached = false; }
	}
	~Impl() { try { detach(); } catch (...) {} }
	static void callback(void* context, EngineCallbackOpcode action, uint id,
		int value1, int value2, int value3, float, const char* text) noexcept
	{
		if (!context) { return; }
		auto& self = *static_cast<Impl*>(context);
		if (!self.collecting.load()) { return; }
		// Native runner threads may report unrelated parameter/transport events.
		// They do not participate in this graph snapshot.
		if (action < ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED || action > ENGINE_CALLBACK_PATCHBAY_CONNECTION_REMOVED) { return; }
		if (self.owner != std::this_thread::get_id()) { self.callbackFailed.store(true); return; }
		if (self.callbackFailed.load()) { return; }
		try
		{
			// Only refresh's synchronous add records form a snapshot. Other native
			// host/UI notifications still follow CarlaEngineNative's own path.
			if (action != ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED && action != ENGINE_CALLBACK_PATCHBAY_PORT_ADDED
				&& action != ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED)
			{
				if (action >= ENGINE_CALLBACK_PATCHBAY_CLIENT_REMOVED && action <= ENGINE_CALLBACK_PATCHBAY_CONNECTION_REMOVED)
				{
					patchbayError("Carla changed its graph during a patchbay refresh");
				}
				return;
			}
			if (self.graph.clients.size() + self.graph.ports.size() + self.graph.connections.size() >= 65536)
			{
				patchbayError("Carla patchbay snapshot exceeds 65536 records");
			}
			if (!text) { patchbayError("Carla omitted patchbay metadata"); }
			const auto copied = QString::fromUtf8(text);
			if (action == ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED) { self.graph.clients.push_back({id, value2, value1, copied}); }
			else if (action == ENGINE_CALLBACK_PATCHBAY_PORT_ADDED)
			{
				if (value1 < 0 || value2 < 0 || value3 < 0) { patchbayError("Carla returned invalid patchbay port metadata"); }
				const auto type = value2 & (PATCHBAY_PORT_TYPE_AUDIO | PATCHBAY_PORT_TYPE_CV | PATCHBAY_PORT_TYPE_MIDI);
				Port::Signal signal;
				if (type == PATCHBAY_PORT_TYPE_AUDIO) { signal = Port::Signal::Audio; }
				else if (type == PATCHBAY_PORT_TYPE_MIDI) { signal = Port::Signal::Midi; }
				else if (type == PATCHBAY_PORT_TYPE_CV) { signal = Port::Signal::Cv; }
				else { patchbayError("Carla returned an unsupported patchbay signal type"); }
				self.graph.ports.push_back({{id, static_cast<quint32>(value1)}, value3, static_cast<quint32>(value2),
					(value2 & PATCHBAY_PORT_IS_INPUT) != 0, signal, copied});
			}
			else
			{
				const auto parts = copied.split(':');
				if (parts.size() != 4) { patchbayError("Carla returned malformed patchbay connection metadata"); }
				self.graph.connections.push_back({id, {{connectionNumber(parts[0]), connectionNumber(parts[1])},
					{connectionNumber(parts[2]), connectionNumber(parts[3])}}});
			}
		}
		catch (...) { self.callbackFailed.store(true); } // Never unwind through the native callback ABI.
	}
	[[noreturn]] void failed(const QString& operation) const
	{
		const auto message = get_last_error(host);
		patchbayError(operation + ": " + (message && *message ? QString::fromUtf8(message) : QString("native Carla rejected the operation")));
	}
	Graph inspect()
	{
		if (!failure.isEmpty()) { patchbayError(failure); }
		if (owner != std::this_thread::get_id()) { patchbayError("Carla patchbay access requires its owning application thread"); }
		if (collecting.load()) { patchbayError("A Carla patchbay refresh is already active"); }
		graph = {}; callbackFailed.store(false);
		try
		{
			collecting.store(true);
			const auto refreshed = patchbay_refresh(host, false);
			collecting.store(false);
			if (!refreshed) { failed("Could not refresh the internal Carla patchbay"); }
			if (callbackFailed.load()) { patchbayError("Carla patchbay refresh returned malformed, oversized or asynchronous metadata"); }
			validateGraph(graph);
			return std::exchange(graph, {});
		}
		catch (...)
		{
			collecting.store(false);
			graph = {}; throw;
		}
	}
#endif
};

CarlaPluginPatchbay::CarlaPluginPatchbay(void* host, bool patchbay, const Resolver& resolve) : m_impl(std::make_unique<Impl>())
{
	if (!patchbay) { m_impl->failure = "Internal graph routing requires a Carla Patchbay instrument; Carla Rack uses its native serial chain"; return; }
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	try
	{
		if (!host) { m_impl->failure = "The native Carla host handle is unavailable"; return; }
		m_impl->host = static_cast<CarlaHostHandle>(host);
#define RESOLVE(name) \
		m_impl->name = reinterpret_cast<decltype(m_impl->name)>(resolve("carla_" #name)); \
		if (!m_impl->name) { m_impl->failure = "Carla library lacks carla_" #name; return; }
		RESOLVE(set_engine_callback)
		RESOLVE(patchbay_refresh)
		RESOLVE(patchbay_connect)
		RESOLVE(patchbay_disconnect)
		RESOLVE(get_last_error)
#undef RESOLVE
		// Install once while the owning native engine is empty and inactive.
		// Carla's runner reads this slot without a lock; never replace it while
		// hosted plugins can emit events. Only the atomic collection flag changes.
		m_impl->attached = true;
		m_impl->set_engine_callback(m_impl->host, &Impl::callback, m_impl.get());
		m_impl->failure.clear();
	}
	catch (const std::exception& error)
	{
		m_impl->failure = "Carla patchbay attachment failed: " + QString::fromUtf8(error.what());
		try { m_impl->detach(); } catch (...) {}
	}
	catch (...)
	{
		m_impl->failure = "Carla patchbay attachment failed with a native exception";
		try { m_impl->detach(); } catch (...) {}
	}
#else
	Q_UNUSED(host) Q_UNUSED(resolve)
#endif
}
CarlaPluginPatchbay::~CarlaPluginPatchbay() = default;
void CarlaPluginPatchbay::prepareForEngineCleanup()
{
	m_impl->failure = "The native Carla engine is closing";
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	m_impl->collecting.store(false);
	// The engine retains the inert callback until its destructor stops/joins
	// native runners. The owner retains this Impl until that destructor returns.
	m_impl->attached = false;
	m_impl->host = nullptr;
#endif
}
bool CarlaPluginPatchbay::available() const { return m_impl->failure.isEmpty(); }
QString CarlaPluginPatchbay::unavailableReason() const { return m_impl->failure; }
PluginHost::PatchbayGraph CarlaPluginPatchbay::inspect()
{
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	return m_impl->inspect();
#else
	patchbayError(m_impl->failure);
#endif
}
PluginHost::PatchbayGraph CarlaPluginPatchbay::connect(const PluginHost::PatchbayRoute& route)
{
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	const auto before = inspect(); validateRoute(before, route);
	if (findConnection(before, route) != before.connections.end()) { patchbayError("The patchbay route already exists"); }
	if (!m_impl->patchbay_connect(m_impl->host, false, route.source.group, route.source.port, route.destination.group, route.destination.port))
	{
		m_impl->failed("Could not connect the Carla patchbay route");
	}
	auto after = inspect();
	if (findConnection(after, route) == after.connections.end()) { patchbayError("Carla accepted the route but did not retain it in its graph"); }
	return after;
#else
	Q_UNUSED(route) patchbayError(m_impl->failure);
#endif
}
PluginHost::PatchbayGraph CarlaPluginPatchbay::disconnect(const PluginHost::PatchbayRoute& route)
{
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	const auto before = inspect(); validateRoute(before, route);
	const auto connection = findConnection(before, route);
	if (connection == before.connections.end()) { patchbayError("The patchbay route does not exist"); }
	// Refresh may renumber connection IDs. Resolve from endpoints immediately
	// before the edit instead of accepting a previously observed connection ID.
	if (!m_impl->patchbay_disconnect(m_impl->host, false, connection->id)) { m_impl->failed("Could not disconnect the Carla patchbay route"); }
	auto after = inspect();
	if (findConnection(after, route) != after.connections.end()) { patchbayError("Carla accepted disconnection but retained the route in its graph"); }
	return after;
#else
	Q_UNUSED(route) patchbayError(m_impl->failure);
#endif
}

} // namespace lmms
