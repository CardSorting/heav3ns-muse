/* Native linked control groups, current-buffer coherence and live inspection.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "LfoController.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "ValueBuffer.h"

using namespace lmms;
namespace
{
MidiController* midi(int value = 63)
{
	auto controller = new MidiController(nullptr); controller->midiPort()->setInputController(74);
	controller->processInEvent(MidiEvent(MidiControlChange, 0, 74, value), {});
	Controller::triggerFrameCounter(); controller->currentValue(0); Controller::triggerFrameCounter(); controller->currentValue(0); return controller;
}
void bind(FloatModel& model, int value = 63) { model.setControllerConnection(new ControllerConnection(midi(value))); }
float tail(AutomatableModel& model) { const auto buffer = model.valueBuffer(); return buffer ? buffer->values()[buffer->length() - 1] : model.value<float>(); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject inspect(const AutomatableModel& model, QJsonObject args = {})
{ args.insert("parameter", static_cast<int>(model.id())); args.insert("choices", false); return StudioCapabilities::execute("parameter.inspect", args); }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{
		{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
		{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"},
			{"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}

class LinkedParameterTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void joiningPrimedGroupsImmediatelyUsesInheritedNativeSamples_data()
	{ QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true; }
	void joiningPrimedGroupsImmediatelyUsesInheritedNativeSamples()
	{
		QFETCH(bool, logarithmic); FloatModel target(42, 1, 100, 0, nullptr), source(42, 1, 100, 0, nullptr);
		target.setScaleLogarithmic(logarithmic); source.setScaleLogarithmic(logarithmic); bind(source);
		QVERIFY(!target.valueBuffer()); const float expected = tail(source); const auto period = Controller::runningPeriods();
		QVERIFY(target.linkToModel(&source)); QVERIFY(target.valueBuffer()); QCOMPARE(tail(target), expected); QCOMPARE(target.countLinks(), std::size_t{1});
		QCOMPARE(Controller::runningPeriods(), period); QVERIFY(target.linkToModel(&source)); QCOMPARE(target.countLinks(), std::size_t{1}); QVERIFY(!target.linkToModel(nullptr));
	}

	void splitInvalidatesBothRemainingAndDetachedNativeGroups()
	{
		FloatModel a(42, 1, 100, 0, nullptr), b(42, 1, 100, 0, nullptr), source(42, 1, 100, 0, nullptr), c(42, 1, 100, 0, nullptr);
		QVERIFY(a.linkToModel(&b)); bind(source); QVERIFY(source.linkToModel(&c)); QVERIFY(a.linkToModel(&source));
		const float expected = tail(source); for (auto model : {&a, &b, &c}) { QCOMPARE(tail(*model), expected); }
		const auto period = Controller::runningPeriods(); source.unlink(); QCOMPARE(source.countLinks(), std::size_t{0}); QCOMPARE(a.countLinks(), std::size_t{2});
		for (auto model : {&a, &b, &c}) { QVERIFY(!model->valueBuffer()); QCOMPARE(model->value(), 42.f); }
		QCOMPARE(tail(source), expected); QCOMPARE(Controller::runningPeriods(), period);
	}

	void rewireAndOwnedControllerRetirementRefreshEveryFollower()
	{
		FloatModel source(42, 1, 100, 0, nullptr), a(42, 1, 100, 0, nullptr), b(42, 1, 100, 0, nullptr);
		bind(source); QVERIFY(a.linkToModel(&source)); QVERIFY(b.linkToModel(&source)); const float previous = tail(a); QCOMPARE(tail(b), previous);
		auto replacement = new ControllerConnection(midi(127)); const auto period = Controller::runningPeriods();
		{ const std::unique_ptr<ControllerConnection> old(source.controllerConnection()); source.setControllerConnection(replacement); }
		QCOMPARE(tail(a), 100.f); QCOMPARE(tail(b), 100.f); QCOMPARE(Controller::runningPeriods(), period);
		const QPointer<Controller> retired(replacement->getController()); delete replacement; QVERIFY(!retired); QVERIFY(!source.controllerConnection());
		for (auto model : {&source, &a, &b}) { QVERIFY(!model->valueBuffer()); QCOMPARE(model->value(), 42.f); }
	}

	void ownershipToggleInvalidatesInheritedSamplesInTheSamePeriod()
	{
		FloatModel source(42, 1, 100, 0, nullptr), target(42, 1, 100, 0, nullptr); bind(source); QVERIFY(target.linkToModel(&source));
		const float previous = tail(target); const auto period = Controller::runningPeriods(); source.setUseControllerValue(false);
		QVERIFY(!target.valueBuffer()); QCOMPARE(target.value(), 42.f); source.setUseControllerValue(true); QCOMPARE(tail(target), previous); QCOMPARE(Controller::runningPeriods(), period);
	}

	void newControllerFeedbackIsRejectedBeforeCopyingValues_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("direct") << QString("direct");
		QTest::newRow("indirect") << QString("indirect"); QTest::newRow("group-member-binding") << QString("group");
	}
	void newControllerFeedbackIsRejectedBeforeCopyingValues()
	{
		QFETCH(QString, kind); LfoController first(nullptr), second(nullptr); FloatModel target(42, 0, 100, 0, nullptr), peer(42, 0, 100, 0, nullptr);
		auto child = first.findChildren<AutomatableModel*>().front(); AutomatableModel* candidate = child;
		if (kind == "indirect") { child->setControllerConnection(new ControllerConnection(&second)); candidate = second.findChildren<AutomatableModel*>().front(); }
		if (kind == "group") { QVERIFY(target.linkToModel(&peer)); peer.setControllerConnection(new ControllerConnection(&first)); }
		else { target.setControllerConnection(new ControllerConnection(&first)); }
		const auto links = target.countLinks(); const float stored = target.storedValue(); QSignalSpy changed(&target, &AutomatableModel::dataChanged);
		QVERIFY(!target.linkToModel(candidate)); QCOMPARE(target.countLinks(), links); QCOMPARE(target.storedValue(), stored); QCOMPARE(changed.size(), 0);
		QVERIFY(!target.isLinkedToModel(candidate));
	}

	void callbackIntroducedFeedbackIsRecheckedBeforeMergingRings()
	{
		LfoController controller(nullptr); FloatModel target(42, 0, 100, 0, nullptr); auto child = controller.findChildren<AutomatableModel*>().front();
		const auto connection = connect(&target, &AutomatableModel::dataChanged, this, [&] {
			if (!target.controllerConnection()) { target.setControllerConnection(new ControllerConnection(&controller)); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!target.linkToModel(child)); QVERIFY(target.controllerConnection()); QCOMPARE(target.countLinks(), std::size_t{0}); QVERIFY(!child->isLinkedToModel(&target));
	}

	void retiredOtherModelCannotBeMergedAfterTheValueCallback()
	{
		FloatModel target(42, 0, 100, 0, nullptr); auto source = std::make_unique<FloatModel>(25, 0, 100, 0, nullptr); const QPointer<AutomatableModel> retired(source.get());
		const auto connection = connect(&target, &AutomatableModel::dataChanged, this, [&] { source.reset(); }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!target.linkToModel(source.get())); QVERIFY(!retired); QCOMPARE(target.countLinks(), std::size_t{0}); QCOMPARE(target.storedValue(), 25.f);
	}

	void groupChangesDoNotInvalidateUnrelatedInterpolation()
	{
		FloatModel source(42, 1, 100, 0, nullptr), target(42, 1, 100, 0, nullptr), unrelated(42, 1, 100, 0, nullptr); bind(source);
		unrelated.setValue(75); const auto cached = unrelated.valueBuffer(); QVERIFY(cached); const float first = cached->values()[0], last = cached->values()[cached->length() - 1];
		QVERIFY(target.linkToModel(&source)); tail(target); source.setUseControllerValue(false); source.setUseControllerValue(true); target.unlink();
		delete source.controllerConnection(); QCOMPARE(unrelated.valueBuffer(), cached); QCOMPARE(cached->values()[0], first); QCOMPARE(cached->values()[cached->length() - 1], last);
	}

	void pagedLivePeersExcludeSelfAndKeepNativeHistoryUnchanged()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error));
		FloatModel root(42, 1, 100, 0, Engine::getSong(), "Linked root"); std::vector<std::unique_ptr<FloatModel>> peers;
		for (int index = 0; index < 1050; ++index) { peers.push_back(std::make_unique<FloatModel>(42, 1, 100, 0, Engine::getSong())); QVERIFY(root.linkToModel(peers.back().get())); }
		const auto before = manifest(directory); int offset = 0; QSet<int> seen;
		while (offset < 1050)
		{
			const auto query = request("links-" + QString::number(offset), "parameter.inspect", {{"parameter", static_cast<int>(root.id())}, {"choices", false}, {"linkedOffset", offset}, {"linkedLimit", 128}});
			const auto response = runtime.process(query); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			const auto result = response.value("result").toObject(); QCOMPARE(result.value("linkedPersistence").toString(), QString("checked-native-project")); QCOMPARE(result.value("linkedParameterCount").toInt(), 1050);
			const auto items = result.value("linkedParameters").toArray(); QVERIFY(!items.isEmpty()); QVERIFY(items.size() <= 128); QVERIFY(QJsonDocument(items).toJson(QJsonDocument::Compact).size() <= 512 * 1024);
			for (const auto& value : items)
			{
				const auto item = value.toObject(); const int id = item.value("id").toInt(); QCOMPARE(id, static_cast<int>(peers.at(offset++)->id()));
				QVERIFY(id != static_cast<int>(root.id())); QVERIFY(!seen.contains(id)); seen.insert(id); QVERIFY(item.value("inspectable").toBool());
			}
			QCOMPARE(result.value("linkedPage").toObject().value("nextOffset").toInt(), offset); const auto replay = runtime.process(query); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("result"), response.value("result"));
		}
		QCOMPARE(seen.size(), 1050); QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		const auto empty = inspect(root, {{"linkedOffset", 2147483647}}).value("result").toObject(); QVERIFY(empty.value("linkedParameters").toArray().isEmpty()); QVERIFY(!empty.value("linkedPage").toObject().value("hasMore").toBool());
	}

	void invalidPeerPagesAreReadOnly_data()
	{
		QTest::addColumn<QJsonObject>("args"); QTest::newRow("negative-offset") << QJsonObject{{"linkedOffset", -1}};
		QTest::newRow("fractional-offset") << QJsonObject{{"linkedOffset", .5}}; QTest::newRow("zero-limit") << QJsonObject{{"linkedLimit", 0}};
		QTest::newRow("excessive-limit") << QJsonObject{{"linkedLimit", 1025}}; QTest::newRow("fractional-limit") << QJsonObject{{"linkedLimit", 2.5}};
	}
	void invalidPeerPagesAreReadOnly()
	{
		QFETCH(QJsonObject, args); FloatModel root(42, 1, 100, 0, Engine::getSong()), peer(42, 1, 100, 0, Engine::getSong()); QVERIFY(root.linkToModel(&peer));
		const auto before = root.linkedModels(); const auto response = inspect(root, args); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(root.linkedModels() == before);
	}

	void liveExternalPeerDoesNotPretendToBeAProjectHandle()
	{
		FloatModel root(42, 1, 100, 0, Engine::getSong()), external(42, 1, 100, 0, nullptr); QVERIFY(root.linkToModel(&external));
		const auto response = inspect(root); QVERIFY(response.value("ok").toBool()); const auto peer = response.value("result").toObject().value("linkedParameters").toArray().at(0).toObject();
		QCOMPARE(peer.value("id").toInt(), static_cast<int>(external.id())); QVERIFY(!peer.value("inspectable").toBool()); QVERIFY(!inspect(external).value("ok").toBool());
	}

	void cancelledGroupInspectionDoesNotDispatchEventsOrPublishPartialData()
	{
		FloatModel root(42, 1, 100, 0, Engine::getSong()); std::vector<std::unique_ptr<FloatModel>> peers;
		for (int index = 0; index < 100; ++index) { peers.push_back(std::make_unique<FloatModel>(42, 1, 100, 0, Engine::getSong())); QVERIFY(root.linkToModel(peers.back().get())); }
		QObject owner; bool eventRan = false; QTimer::singleShot(0, &owner, [&] { eventRan = true; }); int checks = 0;
		{ FileResourceIO::Scope work([&] { return ++checks >= 8; }, true); const auto response = inspect(root); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(!eventRan); }
		QCOMPARE(root.countLinks(), std::size_t{100}); QCoreApplication::processEvents(); QVERIFY(eventRan); QVERIFY(inspect(root).value("ok").toBool());
	}

	void nativeAgentDiscoversAndReadsRuntimeLinkedPeers_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void nativeAgentDiscoversAndReadsRuntimeLinkedPeers()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0, selected = -1; QSet<int> seen;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			QJsonObject output;
			for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
			{
				const auto item = value.toObject(); if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
				{ output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); }
			}
			if (++round == 1) { return decision(protocol, "parameters", "parameter_list", {{"choices", false}}); }
			if (!output.value("ok").toBool()) { throw std::runtime_error("Native linked parameter inspection failed"); }
			if (round == 2)
			{
				for (const auto& value : output.value("result").toObject().value("parameters").toArray())
				{ const auto item = value.toObject(); if (item.value("name").toString().endsWith("Linked root") && item.value("linkedParametersPresent").toBool()) { selected = item.value("id").toInt(); } }
				return decision(protocol, "links-first", "parameter_inspect", {{"parameter", selected}, {"choices", false}, {"linkedLimit", 1}});
			}
			const auto result = output.value("result").toObject(); if (result.value("linkedPersistence").toString() != "checked-native-project") { throw std::runtime_error("Native link persistence scope was omitted"); }
			seen.insert(result.value("linkedParameters").toArray().at(0).toObject().value("id").toInt());
			if (round == 3) { return decision(protocol, "links-next", "parameter_inspect", {{"parameter", selected}, {"choices", false}, {"linkedOffset", result.value("linkedPage").toObject().value("nextOffset")}, {"linkedLimit", 1}}); }
			return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Inspected linked controls; serializer coverage still requires verification."}, {"evidence", QJsonArray{"parameters", "links-first", "links-next"}}});
		});
		QVERIFY(runtime.open(error)); FloatModel root(42, 1, 100, 0, Engine::getSong(), "Linked root"), a(42, 1, 100, 0, Engine::getSong()), b(42, 1, 100, 0, Engine::getSong());
		QVERIFY(root.linkToModel(&a)); QVERIFY(root.linkToModel(&b));
		QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "linked-control-fixture"}}}})).value("ok").toBool());
		QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Inspect the existing linked controls and report their current relationships."}})).value("ok").toBool());
		for (int step = 0; step < 8; ++step) { runtime.advanceAgent(); }
		QCOMPARE(selected, static_cast<int>(root.id())); QCOMPARE(seen, (QSet<int>{static_cast<int>(a.id()), static_cast<int>(b.id())}));
		QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete")); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(LinkedParameterTest)
#include "LinkedParameterTest.moc"
