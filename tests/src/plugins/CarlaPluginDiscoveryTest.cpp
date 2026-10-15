/* Native discovery lifecycle tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include "../../../plugins/CarlaBase/CarlaPluginDiscovery.h"
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
#include <CarlaUtils.h>

_CarlaPluginDiscoveryMetadata::_CarlaPluginDiscoveryMetadata() noexcept
	: name(nullptr), maker(nullptr), category(static_cast<PluginCategory>(0)), hints(0) {}
_CarlaPluginDiscoveryIO::_CarlaPluginDiscoveryIO() noexcept
	: audioIns(0), audioOuts(0), cvIns(0), cvOuts(0), midiIns(0), midiOuts(0), parameterIns(0), parameterOuts(0) {}
_CarlaPluginDiscoveryInfo::_CarlaPluginDiscoveryInfo() noexcept
	: btype(static_cast<BinaryType>(0)), ptype(static_cast<PluginType>(0)), filename(nullptr), label(nullptr), uniqueId(0) {}
#endif

using namespace lmms;
namespace
{
struct NativeScan
{
	int starts = 0, polls = 0, stops = 0, mode = 0;
	QByteArray missing, path, tool;
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	CarlaPluginDiscoveryCallback callback = nullptr;
	void* context = nullptr;
	BinaryType binary = CARLA_BACKEND_NAMESPACE::BINARY_NATIVE;
	PluginType format = CARLA_BACKEND_NAMESPACE::PLUGIN_VST2;
#endif
};
NativeScan native;
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
void emitPlugin(int number)
{
	static CarlaPluginDiscoveryInfo info;
	static QByteArray name, filename, label;
	name = "Native " + QByteArray::number(number);
	filename = native.path + "/instrument.dll"; label = "synth";
	if (native.format == CARLA_BACKEND_NAMESPACE::PLUGIN_LV2) { filename = "bundle.lv2"; label = "urn:lmms:discovery"; }
	if (native.format == CARLA_BACKEND_NAMESPACE::PLUGIN_INTERNAL) { filename.clear(); }
	if (native.format == CARLA_BACKEND_NAMESPACE::PLUGIN_SFZ) { label = native.path + "/instrument.sfz"; filename.clear(); }
	if (native.format == CARLA_BACKEND_NAMESPACE::PLUGIN_JSFX) { label = "effects/instrument.jsfx"; filename.clear(); }
	info.btype = native.binary; info.ptype = native.mode == 3 ? static_cast<PluginType>(-1) : native.format;
	info.filename = filename.constData(); info.label = label.constData(); info.metadata.name = name.constData();
	info.metadata.maker = "Native maker"; info.metadata.hints = CARLA_BACKEND_NAMESPACE::PLUGIN_IS_SYNTH;
	info.metadata.category = CARLA_BACKEND_NAMESPACE::PLUGIN_CATEGORY_SYNTH;
	info.uniqueId = std::numeric_limits<uint64_t>::max();
	info.io.audioOuts = 2; info.io.midiIns = 1; info.io.parameterIns = std::numeric_limits<uint32_t>::max();
	native.callback(native.context, &info, "binary-hash");
	name.fill('X'); label.fill('Y'); filename.fill('Z'); // Native callback storage is borrowed.
}
CarlaPluginDiscoveryHandle startScan(const char* tool, BinaryType binary, PluginType format, const char* path,
	CarlaPluginDiscoveryCallback callback, CarlaPluginCheckCacheCallback, void* context)
{
	++native.starts; native.tool = tool; native.path = path ? path : "";
	native.binary = binary; native.format = format; native.callback = callback; native.context = context;
	if (native.mode == 1) { throw std::runtime_error("native startup failure"); }
	if (native.mode == 7) { return nullptr; }
	return &native;
}
bool idleScan(CarlaPluginDiscoveryHandle)
{
	++native.polls;
	if (native.mode == 2) { throw std::runtime_error("native polling failure"); }
	native.callback(native.context, nullptr, "empty-binary");
	emitPlugin(2 * native.polls - 1); emitPlugin(2 * native.polls);
	return native.polls < 2;
}
void stopScan(CarlaPluginDiscoveryHandle)
{
	++native.stops;
	if (native.mode == 6) { throw std::runtime_error("native cleanup failure"); }
}
#endif
CarlaPluginDiscovery::Resolver resolver()
{
	return [](const char* name) -> QFunctionPointer
	{
		if (native.missing == name) { return nullptr; }
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
#define FUNCTION(symbol, function) \
		static_assert(std::is_same_v<decltype(&function), decltype(&carla_plugin_discovery_##symbol)>); \
		if (QByteArray(name) == "carla_plugin_discovery_" #symbol) { return reinterpret_cast<QFunctionPointer>(&function); }
		FUNCTION(start, startScan)
		FUNCTION(idle, idleScan)
		FUNCTION(stop, stopScan)
#undef FUNCTION
#endif
		return nullptr;
	};
}
}

class CarlaPluginDiscoveryTest : public QObject
{
	Q_OBJECT
	QTemporaryDir directory;
	PluginHost::DiscoveryQuery query() const
	{
		PluginHost::DiscoveryQuery result; result.format = "vst2"; result.path = directory.path();
		result.discoveryTool = directory.filePath("discovery-tool"); return result;
	}
private slots:
	void initTestCase()
	{
		QVERIFY(directory.isValid()); QFile tool(directory.filePath("discovery-tool"));
		QVERIFY(tool.open(QIODevice::WriteOnly)); QVERIFY(tool.write("fixture") > 0);
	}
	void init() { native = NativeScan{}; }
	void missingApiReportsAvailabilityWithoutStarting_data()
	{
		QTest::addColumn<QByteArray>("missing");
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
		for (const char* symbol : {"carla_plugin_discovery_start", "carla_plugin_discovery_idle", "carla_plugin_discovery_stop"})
		{
			QTest::newRow(symbol) << QByteArray(symbol);
		}
#else
		QTest::newRow("headers-unavailable") << QByteArray{};
#endif
	}
	void missingApiReportsAvailabilityWithoutStarting()
	{
		QFETCH(QByteArray, missing); native.missing = missing;
		CarlaPluginDiscovery discovery({}, resolver()); QVERIFY(!discovery.available());
		QVERIFY(!discovery.unavailableReason().isEmpty()); QVERIFY_EXCEPTION_THROWN(discovery.start(query()), std::runtime_error);
		QCOMPARE(native.starts, 0); QCOMPARE(native.stops, 0);
	}
#ifdef LMMS_CARLA_NATIVE_DISCOVERY_API
	void nativeCallbacksAreCopiedAndPagedUntilCompletion()
	{
		CarlaPluginDiscovery discovery({}, resolver()); QVERIFY(discovery.available());
		const auto started = discovery.start(query()); QCOMPARE(started.state, QString("running")); QVERIFY(!started.scan.isEmpty());
		const auto first = discovery.poll(started.scan, 0, 1);
		QCOMPARE(first.total, 2); QVERIFY(first.hasMore); QCOMPARE(first.plugins.size(), std::size_t{1});
		QCOMPARE(first.plugins[0].plugin.name, QString("Native 1")); QCOMPARE(first.plugins[0].plugin.label, QString("synth"));
		QCOMPARE(first.plugins[0].plugin.uniqueId, qint64{-1}); QCOMPARE(first.plugins[0].parameterInputs, std::numeric_limits<quint32>::max());
		QVERIFY(first.plugins[0].instrument); QCOMPARE(first.plugins[0].hash, QString("binary-hash"));
		const auto last = discovery.poll(started.scan, 1, 1024);
		QCOMPARE(last.state, QString("finished")); QCOMPARE(last.total, 4); QVERIFY(!last.hasMore);
		QCOMPARE(last.plugins.front().plugin.name, QString("Native 2")); QCOMPARE(last.plugins.back().plugin.name, QString("Native 4"));
		QCOMPARE(native.stops, 1);
		QVERIFY(discovery.poll(started.scan, std::numeric_limits<int>::max(), 1).plugins.empty());
		QCOMPARE(native.polls, 2); QCOMPARE(native.stops, 1);
	}
	void stopAndDestructionReleaseTheNativeHandle_data()
	{
		QTest::addColumn<bool>("explicitStop"); QTest::newRow("stop") << true; QTest::newRow("destruction") << false;
	}
	void stopAndDestructionReleaseTheNativeHandle()
	{
		QFETCH(bool, explicitStop);
		{
			CarlaPluginDiscovery discovery({}, resolver()); const auto started = discovery.start(query());
			discovery.poll(started.scan, 0, 128);
			if (explicitStop)
			{
				const auto stopped = discovery.stop(started.scan); QCOMPARE(stopped.state, QString("stopped")); QCOMPARE(stopped.total, 2);
				QCOMPARE(discovery.poll(started.scan, 0, 128).total, 2); QCOMPARE(native.polls, 1); QCOMPARE(native.stops, 1);
			}
		}
		QCOMPARE(native.stops, 1);
	}
	void invalidInputsNeverStartNativeDiscovery_data()
	{
		QTest::addColumn<int>("invalid"); for (int i = 0; i < 11; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
	}
	void invalidInputsNeverStartNativeDiscovery()
	{
		QFETCH(int, invalid); auto request = query();
		switch (invalid)
		{
		case 0: request.format = "unknown"; break;
		case 1: request.binary = "unknown"; break;
		case 2: request.format = "internal"; request.binary = CARLA_BACKEND_NAMESPACE::BINARY_NATIVE == CARLA_BACKEND_NAMESPACE::BINARY_WIN32 ? "win64" : "win32"; break;
		case 3: request.path.clear(); break;
		case 4: request.discoveryTool = directory.filePath("absent"); break;
		case 5: request.path = request.discoveryTool; break;
		case 6: request.path = QString(QChar::Null); break;
		case 7: request.maximumPlugins = 0; break;
		case 8: request.maximumPlugins = 65537; break;
		case 9: request.timeoutMs = 0; break;
		case 10: request.timeoutMs = 600001; break;
		}
		CarlaPluginDiscovery discovery({}, resolver()); QVERIFY_EXCEPTION_THROWN(discovery.start(request), std::runtime_error);
		QCOMPARE(native.starts, 0); QCOMPARE(native.stops, 0);
	}
	void terminalOutcomesPreservePartialResultsAndReleaseHandles_data()
	{
		QTest::addColumn<int>("mode"); QTest::addColumn<QString>("state");
		QTest::newRow("startup-exception") << 1 << QString("failed");
		QTest::newRow("poll-exception") << 2 << QString("failed");
		QTest::newRow("invalid-callback") << 3 << QString("failed");
		QTest::newRow("result-limit") << 4 << QString("limited");
		QTest::newRow("timeout") << 5 << QString("timedOut");
		QTest::newRow("cleanup-exception") << 6 << QString("failed");
		QTest::newRow("no-native-handle") << 7 << QString("noHandle");
	}
	void terminalOutcomesPreservePartialResultsAndReleaseHandles()
	{
		QFETCH(int, mode); QFETCH(QString, state); native.mode = mode; auto request = query();
		if (mode == 4) { request.maximumPlugins = 1; } if (mode == 5) { request.timeoutMs = 1; }
		CarlaPluginDiscovery discovery({}, resolver()); auto result = discovery.start(request);
		if (mode == 5) { QTest::qWait(5); }
		if (mode == 6) { result = discovery.stop(result.scan); }
		else { result = discovery.poll(result.scan, 0, 128); }
		QCOMPARE(result.state, state); QVERIFY(!result.error.isEmpty());
		QCOMPARE(native.stops, mode == 1 || mode == 7 ? 0 : 1);
		if (mode == 4) { QCOMPARE(result.total, 1); QCOMPARE(result.plugins[0].plugin.name, QString("Native 1")); }
		const auto polls = native.polls; discovery.poll(result.scan, 0, 128); QCOMPARE(native.polls, polls);
	}
	void staleIdsAndInvalidPaginationDoNotAdvanceScans_data()
	{
		QTest::addColumn<int>("invalid"); for (int i = 0; i < 4; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; }
	}
	void staleIdsAndInvalidPaginationDoNotAdvanceScans()
	{
		QFETCH(int, invalid); CarlaPluginDiscovery discovery({}, resolver()); const auto started = discovery.start(query());
		QVERIFY_EXCEPTION_THROWN(discovery.poll(invalid == 0 ? "other-scan" : started.scan,
			invalid == 1 ? -1 : 0, invalid == 2 ? 0 : invalid == 3 ? 1025 : 128), std::runtime_error);
		QCOMPARE(native.polls, 0); QCOMPARE(native.stops, 0);
		QVERIFY_EXCEPTION_THROWN(discovery.stop("other-scan"), std::runtime_error); QCOMPARE(native.stops, 0);
	}
	void aRunningScanCannotBeReplacedAndCompletedIdsBecomeStale()
	{
		CarlaPluginDiscovery discovery({}, resolver()); const auto first = discovery.start(query());
		QVERIFY_EXCEPTION_THROWN(discovery.start(query()), std::runtime_error); QCOMPARE(native.starts, 1);
		discovery.stop(first.scan); const auto second = discovery.start(query());
		QVERIFY(first.scan != second.scan); QCOMPARE(native.starts, 2);
		QVERIFY_EXCEPTION_THROWN(discovery.poll(first.scan, 0, 128), std::runtime_error);
	}
	void discoveryProducesFormatSpecificLoadArguments_data()
	{
		QTest::addColumn<QString>("format");
		for (const char* format : {"internal", "lv2", "sfz", "jsfx", "vst2"}) { QTest::newRow(format) << QString(format); }
	}
	void discoveryProducesFormatSpecificLoadArguments()
	{
		QFETCH(QString, format); auto request = query(); request.format = format;
		if (format != "vst2") { request.discoveryTool.clear(); }
		CarlaPluginDiscovery discovery({}, resolver()); auto result = discovery.start(request); result = discovery.poll(result.scan, 0, 128);
		QCOMPARE(result.total, 2); const auto& plugin = result.plugins[0].plugin; QCOMPARE(plugin.format, format);
		if (format == "lv2") { QVERIFY(plugin.path.isEmpty()); QCOMPARE(plugin.label, QString("urn:lmms:discovery")); }
		else if (format == "internal") { QVERIFY(plugin.path.isEmpty()); QCOMPARE(plugin.label, QString("synth")); }
		else if (format == "sfz") { QCOMPARE(QDir::cleanPath(plugin.path), directory.filePath("instrument.sfz")); }
		else if (format == "jsfx") { QCOMPARE(QDir::cleanPath(plugin.path), directory.filePath("effects/instrument.jsfx")); }
		else { QCOMPARE(QDir::cleanPath(plugin.path), directory.filePath("instrument.dll")); }
	}
#endif
};
QTEST_GUILESS_MAIN(CarlaPluginDiscoveryTest)
#include "CarlaPluginDiscoveryTest.moc"
