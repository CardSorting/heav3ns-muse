/* Native patchbay callback/route tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include "../../../plugins/CarlaBase/CarlaPluginPatchbay.h"
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
#include <CarlaHost.h>
using namespace CARLA_BACKEND_NAMESPACE;
#endif

using namespace lmms;
namespace
{
struct NativeGraph
{
	QByteArray missing;
	int attaches = 0, detaches = 0, refreshes = 0, connects = 0, disconnects = 0;
	int refreshFailure = 0, editFailure = 0;
	bool externalRequested = false, setCallbackThrows = false;
	int workerCallback = 0;
	quint32 disconnectedId = 0;
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	struct Event { EngineCallbackOpcode action; uint id; int first, second, third; QByteArray text; bool nullText = false; };
	std::vector<Event> events{
		{ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED, 1, PATCHBAY_ICON_HARDWARE, -1, 0, "LMMS input"},
		{ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED, 2, PATCHBAY_ICON_PLUGIN, 0, 0, "Hosted synth"},
		{ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED, 3, PATCHBAY_ICON_HARDWARE, -1, 0, "LMMS output"}
	};
	std::vector<PluginHost::PatchbayConnection> connections{{1, {{1, 1}, {2, 1}}}};
	EngineCallbackFunc callback = nullptr;
	void* context = nullptr;
	NativeGraph()
	{
		const int types[] = {PATCHBAY_PORT_TYPE_AUDIO, PATCHBAY_PORT_TYPE_MIDI, PATCHBAY_PORT_TYPE_CV};
		for (int i = 0; i < 3; ++i)
		{
			events.push_back({ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, 1, i + 1, types[i], 0, "Input source " + QByteArray::number(i)});
			events.push_back({ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, 2, i + 1, static_cast<int>(types[i] | PATCHBAY_PORT_IS_INPUT), 0, "Plugin input " + QByteArray::number(i)});
			events.push_back({ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, 2, i + 4, types[i], 9, "Plugin output " + QByteArray::number(i)});
			events.push_back({ENGINE_CALLBACK_PATCHBAY_PORT_ADDED, 3, i + 1, static_cast<int>(types[i] | PATCHBAY_PORT_IS_INPUT), 0, "Output sink " + QByteArray::number(i)});
		}
	}
#endif
};
NativeGraph native;
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
void setCallback(CarlaHostHandle, EngineCallbackFunc callback, void* context)
{
	if (callback) { ++native.attaches; } else { ++native.detaches; }
	native.callback = callback; native.context = context;
	if (callback && native.setCallbackThrows) { throw std::runtime_error("callback attachment failed after changing the slot"); }
}
void emitEvent(const NativeGraph::Event& event)
{
	QByteArray borrowed(event.text.constData(), event.text.size()); // Independent storage, overwritten after the callback.
	native.callback(native.context, event.action, event.id, event.first, event.second, event.third, 0,
		event.nullText ? nullptr : borrowed.constData());
	borrowed.fill('X');
}
bool refresh(CarlaHostHandle, bool external)
{
	++native.refreshes; native.externalRequested |= external;
	for (const auto& event : native.events) { emitEvent(event); }
	uint next = static_cast<uint>(native.refreshes * 100);
	for (auto& connection : native.connections)
	{
		connection.id = ++next;
		const auto& route = connection.route;
		const auto endpoints = QByteArray::number(route.source.group) + ':' + QByteArray::number(route.source.port)
			+ ':' + QByteArray::number(route.destination.group) + ':' + QByteArray::number(route.destination.port);
		emitEvent({ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, connection.id, 0, 0, 0, endpoints});
	}
	if (native.workerCallback)
	{
		std::thread worker([]
		{
			emitEvent({native.workerCallback == 1 ? ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED : ENGINE_CALLBACK_PARAMETER_VALUE_CHANGED,
				20, 0, -1, 0, "asynchronous notification"});
		});
		worker.join();
	}
	if (native.refreshFailure == 2) { throw std::runtime_error("native refresh exception"); }
	return native.refreshFailure != 1;
}
bool connect(CarlaHostHandle, bool external, uint sourceGroup, uint sourcePort, uint destinationGroup, uint destinationPort)
{
	++native.connects; native.externalRequested |= external;
	if (native.editFailure == 1) { return false; }
	if (native.editFailure == 2) { return true; }
	native.connections.push_back({0, {{sourceGroup, sourcePort}, {destinationGroup, destinationPort}}});
	if (native.editFailure == 3) { throw std::runtime_error("connect failed after changing the native graph"); }
	if (native.editFailure == 4) { native.refreshFailure = 1; }
	return true;
}
bool disconnect(CarlaHostHandle, bool external, uint id)
{
	++native.disconnects; native.externalRequested |= external; native.disconnectedId = id;
	if (native.editFailure == 1) { return false; }
	if (native.editFailure == 2) { return true; }
	const auto found = std::find_if(native.connections.begin(), native.connections.end(), [id](const auto& connection) { return connection.id == id; });
	if (found == native.connections.end()) { return false; }
	native.connections.erase(found);
	if (native.editFailure == 3) { throw std::runtime_error("disconnect failed after changing the native graph"); }
	if (native.editFailure == 4) { native.refreshFailure = 1; }
	return true;
}
const char* lastError(CarlaHostHandle) { return "native patchbay failure"; }
#endif
CarlaPluginPatchbay::Resolver resolver()
{
	return [](const char* name) -> QFunctionPointer
	{
		if (native.missing == name) { return nullptr; }
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
#define FUNCTION(symbol, function) \
		static_assert(std::is_same_v<decltype(&function), decltype(&carla_##symbol)>); \
		if (QByteArray(name) == "carla_" #symbol) { return reinterpret_cast<QFunctionPointer>(&function); }
		FUNCTION(set_engine_callback, setCallback)
		FUNCTION(patchbay_refresh, refresh)
		FUNCTION(patchbay_connect, connect)
		FUNCTION(patchbay_disconnect, disconnect)
		FUNCTION(get_last_error, lastError)
#undef FUNCTION
#endif
		return nullptr;
	};
}
}

class CarlaPluginPatchbayTest : public QObject
{
	Q_OBJECT
private slots:
	void init() { native = NativeGraph{}; }
	void unavailableGraphNeverInstallsACallback_data()
	{
		QTest::addColumn<QByteArray>("missing");
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
		for (const char* name : {"carla_set_engine_callback", "carla_patchbay_refresh", "carla_patchbay_connect", "carla_patchbay_disconnect", "carla_get_last_error"})
		{
			QTest::newRow(name) << QByteArray(name);
		}
#else
		QTest::newRow("headers-unavailable") << QByteArray{};
#endif
	}
	void unavailableGraphNeverInstallsACallback()
	{
		QFETCH(QByteArray, missing); native.missing = missing;
		CarlaPluginPatchbay graph(&native, true, resolver()); QVERIFY(!graph.available()); QVERIFY(!graph.unavailableReason().isEmpty());
		QVERIFY_EXCEPTION_THROWN(graph.inspect(), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(graph.connect({{1, 1}, {2, 1}}), std::runtime_error);
		QVERIFY_EXCEPTION_THROWN(graph.disconnect({{1, 1}, {2, 1}}), std::runtime_error);
		QCOMPARE(native.attaches, 0); QCOMPARE(native.refreshes, 0);
	}
	void rackAndMissingHostNeverInstallACallback_data()
	{
		QTest::addColumn<bool>("rack"); QTest::newRow("rack") << true; QTest::newRow("no-host") << false;
	}
	void rackAndMissingHostNeverInstallACallback()
	{
		QFETCH(bool, rack); CarlaPluginPatchbay graph(rack ? &native : nullptr, !rack, resolver());
		QVERIFY(!graph.available()); QVERIFY(!graph.unavailableReason().isEmpty());
		QVERIFY_EXCEPTION_THROWN(graph.inspect(), std::runtime_error); QCOMPARE(native.attaches, 0);
	}
#ifdef LMMS_CARLA_NATIVE_PATCHBAY_API
	void refreshCopiesMetadataAndIgnoresUnrelatedCallbacks()
	{
		native.workerCallback = 2;
		native.events.push_back({ENGINE_CALLBACK_PATCHBAY_CLIENT_POSITION_CHANGED, 2, 3, 4, 5, {}, true});
		native.events.push_back({ENGINE_CALLBACK_PARAMETER_VALUE_CHANGED, 0, 1, 0, 0, {}, true});
		CarlaPluginPatchbay graph(&native, true, resolver()); QVERIFY(graph.available());
		const auto result = graph.inspect(); QCOMPARE(result.clients.size(), std::size_t{3}); QCOMPARE(result.ports.size(), std::size_t{12});
		QCOMPARE(result.clients[1].name, QString("Hosted synth")); QCOMPARE(result.clients[1].plugin, 0);
		QCOMPARE(result.clients[0].plugin, -1); QCOMPARE(result.ports[2].name, QString("Plugin output 0"));
		QCOMPARE(result.ports[2].portGroup, 9); QVERIFY(!result.ports[2].input); QVERIFY(result.ports[1].input);
		QVERIFY(result.ports[0].signal == PluginHost::PatchbayPort::Signal::Audio);
		QVERIFY(result.ports[4].signal == PluginHost::PatchbayPort::Signal::Midi);
		QVERIFY(result.ports[8].signal == PluginHost::PatchbayPort::Signal::Cv);
		QCOMPARE(result.connections.size(), std::size_t{1}); QCOMPARE(result.connections[0].id, quint32{101});
		QCOMPARE(native.attaches, 1); QCOMPARE(native.detaches, 0); QVERIFY(native.callback); QVERIFY(native.context);
		QVERIFY(!native.externalRequested);
		const auto again = graph.inspect(); QCOMPARE(again.connections[0].id, quint32{201});
		QCOMPARE(result.connections[0].id, quint32{101}); QCOMPARE(native.attaches, 1); QCOMPARE(native.detaches, 0);
	}
	void routesUseEndpointIdentityAcrossRenumbering_data()
	{
		QTest::addColumn<int>("signal");
		QTest::newRow("audio") << 0; QTest::newRow("midi") << 1; QTest::newRow("cv") << 2;
	}
	void routesUseEndpointIdentityAcrossRenumbering()
	{
		QFETCH(int, signal); CarlaPluginPatchbay graph(&native, true, resolver());
		const PluginHost::PatchbayRoute route{{2, static_cast<quint32>(signal + 4)}, {3, static_cast<quint32>(signal + 1)}};
		const auto added = graph.connect(route); QCOMPARE(added.connections.size(), std::size_t{2}); QVERIFY(added.connections[1].route == route);
		const auto previousId = added.connections[1].id;
		const auto removed = graph.disconnect(route); QCOMPARE(removed.connections.size(), std::size_t{1});
		QVERIFY(native.disconnectedId != previousId); QCOMPARE(native.disconnectedId, quint32{302});
		QCOMPARE(native.connects, 1); QCOMPARE(native.disconnects, 1); QCOMPARE(native.refreshes, 4);
		QCOMPARE(native.attaches, 1); QCOMPARE(native.detaches, 0); QVERIFY(native.callback); QVERIFY(!native.externalRequested);
	}
	void unsignedClientIdsSurviveInspectionAndRouteEdits()
	{
		const auto id = std::numeric_limits<quint32>::max();
		for (auto& event : native.events) { if (event.id == 3) { event.id = id; } }
		CarlaPluginPatchbay graph(&native, true, resolver());
		const auto added = graph.connect({{2, 4}, {id, 1}}); QCOMPARE(added.connections[1].route.destination.group, id);
		QCOMPARE(graph.disconnect({{2, 4}, {id, 1}}).connections.size(), std::size_t{1});
	}
	void invalidRoutesNeverReachNativeEdits_data()
	{
		QTest::addColumn<int>("scenario");
		const char* names[] = {"unknown-client", "unknown-port", "input-source", "output-destination", "mixed-signal", "duplicate", "absent"};
		for (int i = 0; i < 7; ++i) { QTest::newRow(names[i]) << i; }
	}
	void invalidRoutesNeverReachNativeEdits()
	{
		QFETCH(int, scenario); CarlaPluginPatchbay graph(&native, true, resolver());
		PluginHost::PatchbayRoute route{{2, 4}, {3, 1}};
		if (scenario == 0) { route.source.group = 20; }
		if (scenario == 1) { route.source.port = 40; }
		if (scenario == 2) { route.source = {2, 1}; }
		if (scenario == 3) { route.destination = {2, 4}; }
		if (scenario == 4) { route.destination = {3, 2}; }
		if (scenario == 5) { route = {{1, 1}, {2, 1}}; }
		if (scenario == 6) { QVERIFY_EXCEPTION_THROWN(graph.disconnect(route), std::runtime_error); }
		else { QVERIFY_EXCEPTION_THROWN(graph.connect(route), std::runtime_error); }
		QCOMPARE(native.connects, 0); QCOMPARE(native.disconnects, 0); QVERIFY(native.callback);
		QCOMPARE(native.connections.size(), std::size_t{1});
	}
	void malformedRefreshNeverPublishesAPartialGraph_data()
	{
		QTest::addColumn<int>("scenario");
		const char* names[] = {"empty", "null-name", "invalid-plugin", "duplicate-client", "negative-port", "unknown-type",
			"multiple-types", "negative-port-group", "dangling-port", "duplicate-port", "malformed-connection", "overflow-endpoint",
			"negative-endpoint", "dangling-connection", "duplicate-route", "duplicate-connection-id", "wrong-direction",
			"mixed-connection", "concurrent-callback", "graph-changed", "oversized", "negative-hints", "nondecimal-endpoint"};
		for (int i = 0; i < 23; ++i) { QTest::newRow(names[i]) << i; }
	}
	void malformedRefreshNeverPublishesAPartialGraph()
	{
		QFETCH(int, scenario);
		if (scenario == 0) { native.events.clear(); native.connections.clear(); }
		if (scenario == 1) { native.events[0].nullText = true; }
		if (scenario == 2) { native.events[0].second = -2; }
		if (scenario == 3) { native.events.push_back(native.events[0]); }
		if (scenario == 4) { native.events[3].first = -1; }
		if (scenario == 5) { native.events[3].second = 0; }
		if (scenario == 6) { native.events[3].second = PATCHBAY_PORT_TYPE_AUDIO | PATCHBAY_PORT_TYPE_CV; }
		if (scenario == 7) { native.events[3].third = -1; }
		if (scenario == 8) { native.events[3].id = 88; }
		if (scenario == 9) { native.events.push_back(native.events[3]); }
		if (scenario >= 10 && scenario <= 12)
		{
			const QByteArray metadata = scenario == 10 ? "1:1:2" : scenario == 11 ? "4294967296:1:2:1" : "-1:1:2:1";
			native.events.push_back({ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, 999, 0, 0, 0, metadata});
		}
		if (scenario == 13) { native.connections[0].route.destination.port = 90; }
		if (scenario == 14) { native.connections.push_back(native.connections[0]); }
		if (scenario == 15) { native.events.push_back({ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, 101, 0, 0, 0, "2:4:3:1"}); }
		if (scenario == 16) { native.connections[0].route.source = {2, 1}; }
		if (scenario == 17) { native.connections[0].route.destination = {2, 2}; }
		if (scenario == 18) { native.workerCallback = 1; }
		if (scenario == 19) { native.events.push_back({ENGINE_CALLBACK_PATCHBAY_CLIENT_REMOVED, 1, 0, 0, 0, {}}); }
		if (scenario == 20)
		{
			for (uint i = 10; i < 65546; ++i) { native.events.push_back({ENGINE_CALLBACK_PATCHBAY_CLIENT_ADDED, i, 0, -1, 0, "client"}); }
		}
		if (scenario == 21) { native.events[3].second = -1; }
		if (scenario == 22) { native.events.push_back({ENGINE_CALLBACK_PATCHBAY_CONNECTION_ADDED, 999, 0, 0, 0, "+1:1:2:1"}); }
		CarlaPluginPatchbay graph(&native, true, resolver());
		QVERIFY_EXCEPTION_THROWN(graph.inspect(), std::runtime_error); QVERIFY(native.callback);
		QCOMPARE(native.attaches, 1); QCOMPARE(native.detaches, 0); QCOMPARE(native.connects, 0); QCOMPARE(native.disconnects, 0);
		// The next complete native snapshot can recover without replacing the bridge.
		const auto callback = native.callback; auto* context = native.context;
		native = NativeGraph{}; native.callback = callback; native.context = context;
		emitEvent({ENGINE_CALLBACK_PATCHBAY_CLIENT_REMOVED, 1, 0, 0, 0, {}}); // Inactive collection ignores late events.
		QCOMPARE(graph.inspect().clients.size(), std::size_t{3}); QVERIFY(native.callback);
	}
	void nativeFailuresEndCollectionAndDoNotClaimSuccess_data()
	{
		QTest::addColumn<int>("scenario");
		const char* names[] = {"refresh-rejected", "refresh-exception", "attach-exception", "connect-rejected", "connect-ignored",
			"connect-partial-exception", "connect-readback-failed", "disconnect-rejected", "disconnect-ignored",
			"disconnect-partial-exception", "disconnect-readback-failed"};
		for (int i = 0; i < 11; ++i) { QTest::newRow(names[i]) << i; }
	}
	void nativeFailuresEndCollectionAndDoNotClaimSuccess()
	{
		QFETCH(int, scenario);
		if (scenario <= 1) { native.refreshFailure = scenario + 1; }
		if (scenario == 2) { native.setCallbackThrows = true; }
		if (scenario >= 3) { native.editFailure = (scenario - 3) % 4 + 1; }
		CarlaPluginPatchbay graph(&native, true, resolver());
		if (scenario < 3) { QVERIFY_EXCEPTION_THROWN(graph.inspect(), std::runtime_error); }
		else if (scenario < 7) { QVERIFY_EXCEPTION_THROWN(graph.connect({{2, 4}, {3, 1}}), std::runtime_error); }
		else { QVERIFY_EXCEPTION_THROWN(graph.disconnect({{1, 1}, {2, 1}}), std::runtime_error); }
		QCOMPARE(native.attaches, 1); QCOMPARE(native.detaches, scenario == 2 ? 1 : 0);
		QCOMPARE(native.callback != nullptr, scenario != 2); QCOMPARE(native.context != nullptr, scenario != 2);
		// Native partial edits remain explicit failures; the enclosing studio
		// transaction owns checkpoint rollback, not this ABI adapter.
		if (scenario == 5 || scenario == 6) { QCOMPARE(native.connections.size(), std::size_t{2}); }
		if (scenario == 9 || scenario == 10) { QVERIFY(native.connections.empty()); }
	}
	void workerRequestsNeverTouchNativeGraphState()
	{
		CarlaPluginPatchbay graph(&native, true, resolver()); bool rejected = false;
		std::thread worker([&] { try { graph.inspect(); } catch (const std::runtime_error&) { rejected = true; } }); worker.join();
		QVERIFY(rejected); QCOMPARE(native.attaches, 1); QCOMPARE(native.refreshes, 0);
		QCOMPARE(graph.inspect().clients.size(), std::size_t{3});
	}
	void observerStaysAliveUntilNativeEngineCleanupFinishes()
	{
		{
			CarlaPluginPatchbay graph(&native, true, resolver()); graph.inspect();
			const auto callback = native.callback; auto* context = native.context;
			graph.prepareForEngineCleanup(); QVERIFY(!graph.available());
			QVERIFY_EXCEPTION_THROWN(graph.inspect(), std::runtime_error);
			QVERIFY(native.callback == callback); QCOMPARE(native.context, context);
			std::thread finishingRunner([&]
			{
				callback(context, ENGINE_CALLBACK_PATCHBAY_CLIENT_REMOVED, 2, 0, 0, 0, 0, nullptr);
				callback(context, ENGINE_CALLBACK_PARAMETER_VALUE_CHANGED, 0, 1, 0, 0, .5f, nullptr);
			});
			finishingRunner.join();
			// Simulate native destruction after it has joined its own runner.
			native.callback = nullptr; native.context = nullptr;
		}
		QCOMPARE(native.attaches, 1); QCOMPARE(native.detaches, 0);
	}
#endif
};

QTEST_GUILESS_MAIN(CarlaPluginPatchbayTest)
#include "CarlaPluginPatchbayTest.moc"
