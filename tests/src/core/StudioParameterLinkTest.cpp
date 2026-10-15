/* Native agent parameter ring mutation and complete project recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "NativeReplayTestAccess.h"
#include "ValueBuffer.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject link(AutomatableModel* target, AutomatableModel* peer) { return {{"parameter", static_cast<int>(target->id())}, {"peer", static_cast<int>(peer->id())}}; }
QJsonObject detach(AutomatableModel* target) { return {{"parameter", static_cast<int>(target->id())}}; }
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args, bool catalog = false)
{
	if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; }
	auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}};
}
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-link-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject() : QJsonObject{}; }
float tail(AutomatableModel* model) { const auto buffer = model->valueBuffer(); return buffer ? buffer->values()[buffer->length() - 1] : model->value<float>(); }
}
class AdmissionModel : public FloatModel
{
public:
	AdmissionModel(Model* parent) : FloatModel(42, 1, 100, 0, parent) { }
	bool writable = true, input = true;
	bool isWritable() const override { return writable; }
	bool supportsAutomation() const override { return input && writable; }
};

class StudioParameterLinkTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void nativeGroupsMergeAndDetachWithoutLosingPeerValues()
	{
		FloatModel a(42, 0, 100, 0, Engine::getSong()), b(70, 0, 100, 0, Engine::getSong()), c(42, 0, 100, 0, Engine::getSong()), d(70, 0, 100, 0, Engine::getSong());
		QVERIFY(a.linkToModel(&c)); QVERIFY(b.linkToModel(&d)); auto result = StudioCapabilities::execute("parameter.link", link(&a, &b)); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		QCOMPARE(a.countLinks(), std::size_t{3}); QCOMPARE(a.storedValue(), 70.f); QCOMPARE(c.storedValue(), 70.f); QCOMPARE(d.storedValue(), 70.f);
		QSignalSpy changed(&a, &AutomatableModel::dataChanged); result = StudioCapabilities::execute("parameter.link", link(&a, &d)); QVERIFY(result.value("ok").toBool()); QVERIFY(!result.value("result").toObject().value("changed").toBool()); QCOMPARE(changed.size(), 0);
		result = StudioCapabilities::execute("parameter.unlink", detach(&a)); QVERIFY(result.value("ok").toBool()); QVERIFY(!a.isLinked()); QVERIFY(b.isLinkedToModel(&c)); QCOMPARE(b.countLinks(), std::size_t{2}); QCOMPARE(a.storedValue(), 70.f);
		a.setValue(25); QCOMPARE(c.storedValue(), 70.f); result = StudioCapabilities::execute("parameter.unlink", detach(&a)); QVERIFY(result.value("ok").toBool()); QVERIFY(!result.value("result").toObject().value("changed").toBool());
	}

	void inheritedMidiBuffersUpdateInTheCurrentPeriod_data()
	{ QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true; }
	void inheritedMidiBuffersUpdateInTheCurrentPeriod()
	{
		QFETCH(bool, logarithmic); FloatModel source(42, 1, 100, 0, Engine::getSong()), target(42, 1, 100, 0, Engine::getSong()); source.setScaleLogarithmic(logarithmic); target.setScaleLogarithmic(logarithmic);
		auto midi = new MidiController(nullptr); midi->midiPort()->setInputController(74); midi->processInEvent(MidiEvent(MidiControlChange, 0, 74, 63), {}); source.setControllerConnection(new ControllerConnection(midi));
		Controller::triggerFrameCounter(); midi->currentValue(0); Controller::triggerFrameCounter(); midi->currentValue(0); const float expected = tail(&source); const auto period = Controller::runningPeriods(); QVERIFY(!target.valueBuffer());
		QVERIFY(StudioCapabilities::execute("parameter.link", link(&target, &source)).value("ok").toBool()); QCOMPARE(tail(&target), expected); QCOMPARE(target.storedValue(), 42.f);
		QVERIFY(StudioCapabilities::execute("parameter.unlink", detach(&target)).value("ok").toBool()); QVERIFY(!target.valueBuffer()); QCOMPARE(target.value(), 42.f); QCOMPARE(tail(&source), expected); QCOMPARE(Controller::runningPeriods(), period);
	}

	void rawUnitLinkValuesFitEachMemberWithoutNormalizedConversion()
	{
		FloatModel target(42, 40, 60, 0, Engine::getSong()), member(42, 0, 100, 0, Engine::getSong()), source(10, 0, 100, 0, Engine::getSong()); QVERIFY(target.linkToModel(&member));
		const auto result = StudioCapabilities::execute("parameter.link", link(&target, &source)); QVERIFY(result.value("ok").toBool()); QCOMPARE(target.storedValue(), 40.f); QCOMPARE(member.storedValue(), 10.f); QCOMPARE(source.storedValue(), 10.f);
		QCOMPARE(result.value("result").toObject().value("manualValue").toDouble(), 40.); QVERIFY(target.isLinkedToModel(&source));
	}

	void directIndirectAndGroupFeedbackRejectBeforeValueCopy_data()
	{ QTest::addColumn<int>("mode"); QTest::newRow("direct") << 0; QTest::newRow("indirect") << 1; QTest::newRow("group-binding") << 2; }
	void directIndirectAndGroupFeedbackRejectBeforeValueCopy()
	{
		QFETCH(int, mode); auto first = new LfoController(Engine::getSong()), second = new LfoController(Engine::getSong()); Engine::getSong()->addController(first); Engine::getSong()->addController(second);
		FloatModel target(42, 1, 100, 0, Engine::getSong()), member(42, 1, 100, 0, Engine::getSong()); auto child = first->findChildren<AutomatableModel*>().first(); AutomatableModel* candidate = child;
		if (mode == 1) { child->setControllerConnection(new ControllerConnection(second)); candidate = second->findChildren<AutomatableModel*>().first(); }
		if (mode == 2) { QVERIFY(target.linkToModel(&member)); member.setControllerConnection(new ControllerConnection(first)); } else { target.setControllerConnection(new ControllerConnection(first)); }
		const auto links = target.countLinks(); QSignalSpy changed(&target, &AutomatableModel::dataChanged); const auto result = StudioCapabilities::execute("parameter.link", link(&target, candidate));
		QVERIFY(!result.value("ok").toBool()); QCOMPARE(target.storedValue(), 42.f); QCOMPARE(target.countLinks(), links); QCOMPARE(changed.size(), 0);
	}

	void everyMergedMemberMustBeACurrentWritableSupportedInput_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("target-readonly") << 0; QTest::newRow("peer-readonly") << 1; QTest::newRow("member-readonly") << 2; QTest::newRow("unsupported-input") << 3; QTest::newRow("unsupported-scale") << 4; QTest::newRow("external-member") << 5; }
	void everyMergedMemberMustBeACurrentWritableSupportedInput()
	{
		QFETCH(int, failure); AdmissionModel target(Engine::getSong()), peer(Engine::getSong()), member(failure == 5 ? nullptr : Engine::getSong());
		if (failure == 0) { target.writable = false; } if (failure == 1) { peer.writable = false; } if (failure == 2 || failure == 5) { QVERIFY(target.linkToModel(&member)); }
		if (failure == 2) { member.writable = false; } if (failure == 3) { peer.input = false; } if (failure == 4) { peer.setScaleType(AutomatableModel::ScaleType::Decibel); }
		const auto links = target.countLinks(); const auto result = StudioCapabilities::execute("parameter.link", link(&target, &peer)); QVERIFY(!result.value("ok").toBool()); QCOMPARE(target.countLinks(), links); QCOMPARE(target.storedValue(), 42.f);
		if (failure == 2) { QVERIFY(!StudioCapabilities::execute("parameter.set", {{"parameter", static_cast<int>(target.id())}, {"value", 70}}).value("ok").toBool()); QCOMPARE(member.storedValue(), 42.f); }
		if (failure == 5) { QVERIFY(StudioCapabilities::execute("parameter.unlink", detach(&target)).value("ok").toBool()); QVERIFY(!member.isLinked()); }
	}

	void malformedAndHistoricalHandlesCannotChangeGroups_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("missing-peer") << 0; QTest::newRow("missing-parameter") << 1; QTest::newRow("negative") << 2; QTest::newRow("fractional") << 3;
		QTest::newRow("string") << 4; QTest::newRow("unknown") << 5; QTest::newRow("self") << 6; QTest::newRow("saved-alias") << 7; QTest::newRow("unknown-field") << 8; QTest::newRow("overflow") << 9;
	}
	void malformedAndHistoricalHandlesCannotChangeGroups()
	{
		QFETCH(int, failure); auto track = new InstrumentTrack(Engine::getSong()); auto args = link(track->volumeModel(), track->panningModel());
		if (failure == 0) { args.remove("peer"); } if (failure == 1) { args.remove("parameter"); } if (failure == 2) { args.insert("peer", -1); } if (failure == 3) { args.insert("peer", .5); }
		if (failure == 4) { args.insert("peer", "0"); } if (failure == 5) { args.insert("peer", 2147483647); } if (failure == 6) { args.insert("peer", args.value("parameter")); }
		if (failure == 7) { args.insert("peer", static_cast<int>(ProjectJournal::idToSave(track->panningModel()->id()))); } if (failure == 8) { args.insert("other", 1); } if (failure == 9) { args.insert("peer", 2147483648.); }
		QVERIFY(!StudioCapabilities::execute("parameter.link", args).value("ok").toBool()); QVERIFY(!track->volumeModel()->isLinked()); QVERIFY(!track->panningModel()->isLinked());
	}

	void callbackFeedbackFailureRestoresNativeProjectThroughRuntime()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); auto track = new InstrumentTrack(Engine::getSong()); auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller);
		auto child = controller->findChildren<AutomatableModel*>().first(); const float stored = track->volumeModel()->storedValue(); const auto args = link(track->volumeModel(), child);
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); const QPointer<AutomatableModel> old(track->volumeModel());
		const auto connection = QObject::connect(track->volumeModel(), &AutomatableModel::dataChanged, this, [track, controller] { if (!track->volumeModel()->controllerConnection()) { track->volumeModel()->setControllerConnection(new ControllerConnection(controller)); } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto result = runtime.process(request("feedback", "parameter.link", args)); QVERIFY(!result.value("ok").toBool());
		QVERIFY(!old); track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QCOMPARE(track->volumeModel()->storedValue(), stored); QVERIFY(!track->volumeModel()->isLinked()); QVERIFY(!track->volumeModel()->controllerConnection()); QVERIFY(!runtime.stopping()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void callbackPolicyChangesCannotBeReportedAsSuccessfulLinks()
	{
		AdmissionModel target(Engine::getSong()), peer(Engine::getSong()); peer.setValue(70);
		const auto connection = QObject::connect(&target, &AutomatableModel::dataChanged, this, [&] { peer.writable = false; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!StudioCapabilities::execute("parameter.link", link(&target, &peer)).value("ok").toBool());
		// Direct native calls retain setter/callback side effects on rejection;
		// complete runtime checkpoints, not this admission check, supply rollback.
		QCOMPARE(target.storedValue(), 70.f); QVERIFY(target.isLinkedToModel(&peer));
	}

	void projectHistoryReopeningAndReceiptReplayPreserveLinkedTopology()
	{
		QTemporaryDir directory; QString error; QJsonObject linkRequest, receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); auto track = new InstrumentTrack(Engine::getSong()); track->panningModel()->setValue(25);
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); linkRequest = request("link", "parameter.link", link(track->volumeModel(), track->panningModel())); receipt = runtime.process(linkRequest); QVERIFY(receipt.value("ok").toBool());
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QVERIFY(!track->volumeModel()->isLinked());
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QVERIFY(track->volumeModel()->isLinkedToModel(track->panningModel())); QCOMPARE(track->volumeModel()->storedValue(), 25.f);
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(linkRequest), receipt); auto track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QVERIFY(track->volumeModel()->isLinkedToModel(track->panningModel()));
		QVERIFY(reopened.process(request("unlink", "parameter.unlink", detach(track->volumeModel()))).value("ok").toBool()); QCOMPARE(track->volumeModel()->storedValue(), 25.f); QVERIFY(!track->volumeModel()->isLinked());
		QVERIFY(reopened.process(request("undo-unlink", "session.undo", {{"summary", true}})).value("ok").toBool()); track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QVERIFY(track->volumeModel()->isLinkedToModel(track->panningModel()));
	}

	void callbackAddressReuseCannotAliasTheDiscoveredMemberIdentity()
	{
		AdmissionModel target(Engine::getSong()), source(Engine::getSong()); source.setValue(70); alignas(AdmissionModel) unsigned char storage[sizeof(AdmissionModel)];
		auto member = std::construct_at(reinterpret_cast<AdmissionModel*>(storage), Engine::getSong()); const QPointer<AutomatableModel> original(member);
		const auto cleanup = qScopeGuard([&] { if (member) { std::destroy_at(member); } }); bool replaced = false;
		const auto connection = QObject::connect(&target, &AutomatableModel::dataChanged, this, [&] {
			if (replaced) { return; } replaced = true; std::destroy_at(member); member = nullptr;
			member = std::construct_at(reinterpret_cast<AdmissionModel*>(storage), Engine::getSong()); QVERIFY(target.linkToModel(member));
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY(!StudioCapabilities::execute("parameter.link", link(&target, &source)).value("ok").toBool()); QVERIFY(replaced); QVERIFY(!original); QVERIFY(target.isLinkedToModel(member));
	}

	void unsupportedSerializerRejectsRuntimePublicationAndRestoresTheProject()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); auto track = new InstrumentTrack(Engine::getSong());
		FloatModel omitted(25, 0, 100, 0, Engine::getSong()); const auto stored = track->volumeModel()->storedValue(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		const auto result = runtime.process(request("unsupported", "parameter.link", link(track->volumeModel(), &omitted))); QVERIFY(!result.value("ok").toBool());
		track = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QCOMPARE(track->volumeModel()->storedValue(), stored); QVERIFY(!track->volumeModel()->isLinked()); QVERIFY(!omitted.isLinked()); QVERIFY(!runtime.stopping()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void queuedPeerHandlesTrackRetirementIndependently_data()
	{ QTest::addColumn<bool>("peer"); QTest::newRow("peer") << true; QTest::newRow("parameter") << false; }
	void queuedPeerHandlesTrackRetirementIndependently()
	{
		QFETCH(bool, peer); auto first = new InstrumentTrack(Engine::getSong()), second = new InstrumentTrack(Engine::getSong()); const auto args = link(first->volumeModel(), second->volumeModel());
		const auto before = StudioCapabilities::selection("parameter.link", args); delete (peer ? second : first); QVERIFY(before.changed(StudioCapabilities::selection("parameter.link", args)));
	}

	void scriptedNativeAgentsLinkAndUnlinkFromTheirPlans_data()
	{ QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
	void scriptedNativeAgentsLinkAndUnlinkFromTheirPlans()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; InstrumentTrack* track = nullptr;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Manage linked controls"}, {"steps", QJsonArray{"Link and detach native controls"}}, {"decisions", QJsonArray{"Keep native stored values"}}}),
				call("link", "parameter.link", link(track->volumeModel(), track->panningModel()), catalog), call("unlink", "parameter.unlink", detach(track->volumeModel()), catalog),
				call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native links edited"}, {"evidence", QJsonArray{"link", "unlink"}}})});
		});
		QVERIFY(runtime.open(error)); track = new InstrumentTrack(Engine::getSong()); track->panningModel()->setValue(25); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Link and detach the native controls."}})).value("ok").toBool());
		for (int index = 0; index < 5; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(track->volumeModel()->storedValue(), 25.f); QVERIFY(!track->volumeModel()->isLinked()); QCOMPARE(job(directory).value("operations").toInt(), 4);
	}

	void cancellationBeforeMutationDoesNotDispatchEventsOrChangeValues()
	{
		auto track = new InstrumentTrack(Engine::getSong()); const float stored = track->volumeModel()->storedValue(); bool dispatched = false; QTimer::singleShot(0, this, [&] { dispatched = true; });
		QVERIFY(!StudioCapabilities::execute("parameter.link", link(track->volumeModel(), track->panningModel()), [] { return true; }).value("ok").toBool()); QVERIFY(!dispatched); QVERIFY(!track->volumeModel()->isLinked()); QCOMPARE(track->volumeModel()->storedValue(), stored);
		QCoreApplication::processEvents(); QVERIFY(dispatched);
	}

	void schemasAndCatalogExposeBothMutationsWithinDeclarationBudget()
	{
		for (const auto op : {QString("parameter.link"), QString("parameter.unlink")})
		{
			QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == op) { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool());
			QJsonObject args{{"op", op}, {"args", QJsonObject{{"parameter", 1}}}}; QCOMPARE(StudioAgent::operation("studio_execute", args), op);
		}
		for (const auto protocol : {QString("chat"), QString("responses")})
		{
			const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); QSet<QString> names;
			for (const auto& value : tools) { const auto entry = value.toObject(); names.insert(protocol == "chat" ? entry.value("function").toObject().value("name").toString() : entry.value("name").toString()); }
			QVERIFY(names.contains("parameter_link")); QVERIFY(names.contains("parameter_unlink")); QVERIFY(names.contains("studio_execute"));
		}
	}
};
QTEST_GUILESS_MAIN(StudioParameterLinkTest)
#include "StudioParameterLinkTest.moc"
