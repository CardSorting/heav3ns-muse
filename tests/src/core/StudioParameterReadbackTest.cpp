/* Live parameter diagnostics and durable native controller repair.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "LfoController.h"
#include "Mixer.h"
#include "ModelValueNotificationTestUtils.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class Input final : public Controller
{
public:
	Input() : Controller(ControllerType::Lfo, nullptr, "Native readback fixture") { }
	float next = .25f; int fault = 0, reads = 0;
	float currentValue(int) override
	{
		++reads;
		if (fault == 1) { throw std::runtime_error(std::string(4096, 'x')); }
		if (fault == 2) { throw 337; }
		return next;
	}
};
class AdvertisedInteger final : public FloatModel
{
public:
	using FloatModel::FloatModel;
	ValueType valueType() const override { return ValueType::Integer; }
};
void makeBad(Input& input, int kind)
{
	input.fault = kind >= 5 ? kind - 4 : 0;
	input.next = kind == 0 ? std::numeric_limits<float>::quiet_NaN() : kind == 1 ? std::numeric_limits<float>::infinity()
		: kind == 2 ? -std::numeric_limits<float>::infinity() : kind == 3 ? 0x1p40f : 70.f;
}
QString state(int kind) { return kind < 3 ? "nonfinite" : kind == 3 ? "unrepresentable" : kind == 4 ? "outside-range" : "unavailable"; }
QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
{ return {{"id", id}, {"op", operation}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
QJsonObject inspect(AutomatableModel* model)
{ return StudioCapabilities::execute("parameter.inspect", {{"parameter", int(model->id())}, {"choices", false}}); }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{
		{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
		{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"},
			{"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
void arrange()
{
	std::vector<SampleFrame> pcm(44100);
	for (std::size_t index = 0; index < pcm.size(); ++index)
	{
		pcm[index][0] = .4f * std::sin(6.283185307179586 * 440 * index / 44100);
		pcm[index][1] = .2f * std::sin(6.283185307179586 * 660 * index / 44100);
	}
	auto song = Engine::getSong(); song->setMasterVolume(100); auto track = new SampleTrack(song);
	auto clip = static_cast<SampleClip*>(track->createClip(TimePos())); clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100)); clip->changeLength(TimePos(192));
	auto lfo = new LfoController(song); song->addController(lfo);
	Engine::mixer()->mixerChannel(0)->m_volumeModel.setControllerConnection(new ControllerConnection(lfo));
}
bool decoded(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return false; } SF_INFO info{};
	std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close);
	if (!audio || info.channels != 2 || info.samplerate != 44100 || info.frames < 20000 || info.frames > 441000) { return false; }
	std::vector<float> pcm(static_cast<std::size_t>(info.frames) * 2);
	if (sf_readf_float(audio.get(), pcm.data(), info.frames) != info.frames || sf_error(audio.get())) { return false; }
	double squares[2]{};
	for (sf_count_t frame = 0; frame < info.frames; ++frame) for (int channel = 0; channel < 2; ++channel)
	{
		const auto value = pcm[static_cast<std::size_t>(frame) * 2 + channel]; if (!std::isfinite(value)) { return false; } squares[channel] += double(value) * value;
	}
	const auto left = std::sqrt(squares[0] / info.frames), right = std::sqrt(squares[1] / info.frames);
	return left > .01 && right > .005 && std::abs(left / right - 2.) < .12;
}
}
class StudioParameterReadbackTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	void render(StudioRuntime& runtime, const QString& id, const QString& path)
	{
		const auto response = runtime.process(request(id, "audio.render", {{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}}));
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(decoded(path));
	}
	void replay(StudioRuntime& runtime, const QJsonObject& command, const QJsonObject& receipt)
	{ auto response = runtime.process(command); QVERIFY(response.take("replayed").toBool()); QCOMPARE(response, receipt); }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); QVERIFY(Engine::audioEngine()->resetDspFailure()); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); QVERIFY(Engine::audioEngine()->resetDspFailure()); }

	void healthyTypedAndAdvertisedValuesKeepNativeSemantics_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("integer") << 0; QTest::newRow("boolean") << 1; QTest::newRow("advertised-integer-float") << 2; }
	void healthyTypedAndAdvertisedValuesKeepNativeSemantics()
	{
		QFETCH(int, kind); Input input; auto song = Engine::getSong(); std::unique_ptr<AutomatableModel> model;
		if (kind == 0) { model = std::make_unique<IntModel>(42, 0, 100, song); }
		else if (kind == 1) { model = std::make_unique<BoolModel>(false, song); }
		else { model = std::make_unique<AdvertisedInteger>(42, 0, 100, 0, song); }
		model->setControllerConnection(new ControllerConnection(&input)); input.next = kind == 1 ? .25f : .9775f;
		const auto observed = model->value<float>(), stored = model->storedValue(); const auto response = inspect(model.get());
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject();
		QCOMPARE(result.value("value").toDouble(), double(observed)); const auto readback = result.value("readback").toObject();
		QCOMPARE(readback.value("state").toString(), QString("valid"));
		if (kind == 0) { QCOMPARE(readback.value("effectiveValue").toInt(), 97); }
		else if (kind == 1) { QVERIFY(readback.value("effectiveValue").isBool()); QVERIFY(readback.value("effectiveValue").toBool()); }
		else { QCOMPARE(readback.value("effectiveValue").toDouble(), double(observed)); QVERIFY(observed > 97.f); }
		QCOMPARE(model->storedValue(), stored); QVERIFY(model->controllerConnection());
	}

	void invalidTempoRemainsInspectableAndExplicitlyDisconnectable_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("nan") << 0; QTest::newRow("infinity") << 1; QTest::newRow("negative-infinity") << 2;
		QTest::newRow("unrepresentable") << 3; QTest::newRow("outside-song-range") << 4;
		QTest::newRow("standard-getter-error-bounded") << 5; QTest::newRow("nonstandard-getter-error") << 6;
	}
	void invalidTempoRemainsInspectableAndExplicitlyDisconnectable()
	{
		QFETCH(int, kind); Input input; auto song = Engine::getSong(); song->setTempo(91); auto& model = song->tempoModel();
		model.setControllerConnection(new ControllerConnection(&input)); makeBad(input, kind); const auto frames = Engine::framesPerTick();
		const auto cleanupBinding = qScopeGuard([&] { input.fault = 0; input.next = .25f; bool changed = false; QString error; model.disconnectController(changed, error); });
		const auto inspected = inspect(&model); QVERIFY2(inspected.value("ok").toBool(), qPrintable(inspected.value("error").toString()));
		const auto details = inspected.value("result").toObject(); const auto readback = details.value("readback").toObject();
		QCOMPARE(readback.value("state").toString(), state(kind)); QCOMPARE(details.value("manualValue").toInt(), 91); QVERIFY(details.contains("controller"));
		if (kind == 3 || kind == 4) { QVERIFY(details.value("value").isDouble()); } else { QVERIFY(details.value("value").isNull()); }
		if (kind == 5) { QCOMPARE(readback.value("error").toString().size(), 1024); }
		if (kind == 6) { QVERIFY(readback.value("error").toString().contains("nonstandard")); }
		for (const auto section : {"summary", "full"})
		{
			const auto response = StudioCapabilities::execute("project.inspect", {{"section", section}});
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject();
			QVERIFY(result.value("tempo").isNull()); const auto tempo = result.value("tempoReadback").toObject();
			QCOMPARE(tempo.value("parameter").toInt(), int(model.id())); QCOMPARE(tempo.value("state").toString(), state(kind));
			QVERIFY(tempo.value("directControllerConnected").toBool());
		}
		QCOMPARE(model.storedValue(), 91.f); QCOMPARE(Engine::framesPerTick(), frames); const int reads = input.reads;
		const auto response = StudioCapabilities::execute("controller.disconnect", {{"parameter", int(model.id())}});
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(response.value("result").toObject().value("changed").toBool());
		QCOMPARE(input.reads, reads); QVERIFY(!model.controllerConnection()); QCOMPARE(song->getTempo(), bpm_t(91));
		song->setTempo(97); QCOMPARE(StudioCapabilities::inspectSummary().value("tempo").toInt(), 97);
	}

	void parameterPagesRetainHealthyControlsBesideGetterFailures()
	{
		Input input; auto song = Engine::getSong(); FloatModel healthy(42, 0, 100, 0, song);
		song->tempoModel().setControllerConnection(new ControllerConnection(&input)); input.fault = 1; bool foundBad = false, foundHealthy = false; int offset = 0;
		const auto cleanupBinding = qScopeGuard([&] { input.fault = 0; bool changed = false; QString error; song->tempoModel().disconnectController(changed, error); });
		for (int pages = 0; pages < 100; ++pages)
		{
			const auto response = StudioCapabilities::execute("parameter.list", {{"offset", offset}, {"limit", 16}, {"choices", false}});
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject();
			for (const auto value : result.value("parameters").toArray())
			{
				const auto item = value.toObject(); if (item.value("id").toInt() == int(song->tempoModel().id()))
				{ foundBad = true; QCOMPARE(item.value("readback").toObject().value("state").toString(), QString("unavailable")); }
				if (item.value("id").toInt() == int(healthy.id())) { foundHealthy = true; QCOMPARE(item.value("value").toInt(), 42); }
			}
			if (!result.value("hasMore").toBool()) { break; }
			const auto next = result.value("nextOffset").toInt(); QVERIFY(next > offset); offset = next;
		}
		QVERIFY(foundBad); QVERIFY(foundHealthy);
	}

	void inheritedControllerRequiresDiscoveringAndDisconnectingItsPeer()
	{
		Input input; auto song = Engine::getSong(); auto& model = song->tempoModel(); FloatModel peer(91, 10, 999, 0, song);
		QVERIFY(model.linkToModel(&peer)); peer.setControllerConnection(new ControllerConnection(&input)); makeBad(input, 0);
		const auto cleanupBinding = qScopeGuard([&] { input.next = .25f; bool changed = false; QString error; peer.disconnectController(changed, error); });
		const auto tempo = StudioCapabilities::inspectSummary().value("tempoReadback").toObject();
		QCOMPARE(tempo.value("state").toString(), QString("nonfinite")); QVERIFY(!tempo.value("directControllerConnected").toBool());
		const auto inspected = inspect(&model); QVERIFY(inspected.value("ok").toBool()); const auto details = inspected.value("result").toObject();
		QCOMPARE(details.value("linkedParameters").toArray().size(), 1); QCOMPARE(details.value("linkedParameters").toArray().at(0).toObject().value("id").toInt(), int(peer.id()));
		const auto noop = StudioCapabilities::execute("controller.disconnect", {{"parameter", int(model.id())}});
		QVERIFY(noop.value("ok").toBool()); QVERIFY(!noop.value("result").toObject().value("changed").toBool());
		QVERIFY(StudioCapabilities::inspectSummary().value("tempo").isNull()); const int reads = input.reads;
		const auto repair = StudioCapabilities::execute("controller.disconnect", {{"parameter", int(peer.id())}});
		QVERIFY2(repair.value("ok").toBool(), qPrintable(repair.value("error").toString())); QCOMPARE(input.reads, reads);
		song->setTempo(97); QCOMPARE(song->getTempo(), bpm_t(97));
	}

	void cancellationAfterAReadStillRejectsTheWholeInspection_data()
	{ QTest::addColumn<bool>("throwing"); QTest::newRow("nonfinite") << false; QTest::newRow("throwing-getter") << true; }
	void cancellationAfterAReadStillRejectsTheWholeInspection()
	{
		QFETCH(bool, throwing); Input input; auto& model = Engine::getSong()->tempoModel(); model.setControllerConnection(new ControllerConnection(&input));
		const auto cleanupBinding = qScopeGuard([&] { input.fault = 0; input.next = .25f; bool changed = false; QString error; model.disconnectController(changed, error); });
		makeBad(input, throwing ? 5 : 0); input.reads = 0; bool eventRan = false; QObject timerOwner;
		QTimer::singleShot(0, &timerOwner, [&] { eventRan = true; });
		FileResourceIO::Scope work([&] { return input.reads > 0; }, true);
		const auto response = inspect(&model); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
		QVERIFY(response.value("error").toString().contains("cancelled")); QVERIFY(!eventRan); QVERIFY(model.controllerConnection());
	}

	void nativeAgentReceivesDiagnosticsAndRepairsThroughItsToolLoop_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void nativeAgentReceivesDiagnosticsAndRepairsThroughItsToolLoop()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0; bool observedBad = false, observedRepair = false;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			const auto input = body.value(protocol == "chat" ? "messages" : "input").toArray(); QJsonObject summary, lastOutput;
			for (const auto value : input)
			{
				const auto item = value.toObject(); const auto text = item.value("content").toString();
				if (text.startsWith("Native runtime observation (data):\n"))
				{ summary = QJsonDocument::fromJson(text.mid(text.indexOf('\n') + 1).toUtf8()).object().value("projectSummary").toObject(); }
				if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
				{ lastOutput = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); }
			}
			if (++round == 1)
			{
				const auto tempo = summary.value("tempoReadback").toObject(); observedBad = summary.value("tempo").isNull()
					&& tempo.value("state").toString() == "unavailable" && tempo.value("directControllerConnected").toBool();
				return decision(protocol, "discover", "parameter_inspect", {{"parameter", tempo.value("parameter")}});
			}
			if (round == 2)
			{
				const auto target = lastOutput.value("result").toObject();
				if (!lastOutput.value("ok").toBool() || target.value("readback").toObject().value("state").toString() != "unavailable")
				{ throw std::runtime_error("Native agent did not discover the faulty parameter"); }
				return decision(protocol, "repair", "controller_disconnect", {{"parameter", target.value("id")}});
			}
			observedRepair = lastOutput.value("ok").toBool() && lastOutput.value("result").toObject().value("changed").toBool()
				&& summary.value("tempo").toInt() == 91 && summary.value("tempoReadback").toObject().value("state").toString() == "valid";
			return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Repaired the native tempo binding."}, {"evidence", QJsonArray{"discover", "repair"}}});
		});
		QVERIFY2(runtime.open(error), qPrintable(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool());
		Input input; auto& model = Engine::getSong()->tempoModel(); model.setControllerConnection(new ControllerConnection(&input)); makeBad(input, 5);
		const auto cleanupBinding = qScopeGuard([&] { input.fault = 0; bool changed = false; QString cleanupError; model.disconnectController(changed, cleanupError); });
		QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
			{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "parameter-repair-fixture"}}}})).value("ok").toBool());
		QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Inspect the failed tempo controller and disconnect its binding."}})).value("ok").toBool());
		for (int step = 0; step < 6; ++step) { runtime.advanceAgent(); }
		QVERIFY(observedBad); QVERIFY(observedRepair); QCOMPARE(round, 3); QVERIFY(!model.controllerConnection()); QCOMPARE(Engine::getSong()->getTempo(), bpm_t(91));
		QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete")); QVERIFY(!runtime.stopping());
	}

	void runtimeRepairCommitsReplaysAndRendersAfterReopen_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("nan") << 0; QTest::newRow("outside-range") << 4; QTest::newRow("throwing-getter") << 5; }
	void runtimeRepairCommitsReplaysAndRendersAfterReopen()
	{
		QFETCH(int, kind); QTemporaryDir directory, output; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); arrange();
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool()); const auto before = manifest(directory);
			// The durable baseline contains real native content. A process-local
			// faulty source is attached afterward, then removed before checkpoint.
			Input input; auto& model = Engine::getSong()->tempoModel(); model.setControllerConnection(new ControllerConnection(&input)); makeBad(input, kind);
			const auto cleanupBinding = qScopeGuard([&] { input.fault = 0; input.next = .25f; bool changed = false; QString error; model.disconnectController(changed, error); });
			const QPointer<ControllerConnection> old(model.controllerConnection());
			const auto observed = runtime.process(request("diagnose", "project.inspect", {{"section", "summary"}}));
			QVERIFY(observed.value("ok").toBool()); const auto tempo = observed.value("result").toObject().value("tempoReadback").toObject();
			QCOMPARE(tempo.value("state").toString(), state(kind)); const int reads = input.reads;
			command = request("repair", "controller.disconnect", {{"parameter", tempo.value("parameter")}}); receipt = runtime.process(command);
			QVERIFY2(receipt.value("ok").toBool(), qPrintable(receipt.value("error").toString())); QVERIFY(receipt.value("result").toObject().value("changed").toBool());
			QVERIFY(!old); QCOMPARE(input.reads, reads); QCOMPARE(Engine::getSong()->getTempo(), bpm_t(91));
			const auto after = manifest(directory); QCOMPARE(after.value("history").toArray().size(), before.value("history").toArray().size() + 1);
			QCOMPARE(after.value("cursor").toInt(), before.value("cursor").toInt() + 1); QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
			replay(runtime, command, receipt); QVERIFY(runtime.process(request("revise", "song.set", {{"tempo", 97}})).value("ok").toBool());
			render(runtime, "render-repaired", output.filePath("repaired.wav"));
		}
		StudioRuntime reopened(directory.path()); QString error; QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->getTempo(), bpm_t(97));
		QVERIFY(!Engine::getSong()->tempoModel().controllerConnection()); QCOMPARE(Engine::getSong()->controllers().size(), std::size_t(1));
		QVERIFY(dynamic_cast<LfoController*>(Engine::getSong()->controllers().front())); replay(reopened, command, receipt);
		render(reopened, "render-reopened", output.filePath("reopened.wav"));
	}

	void failedRepairRestoresTheCommittedProjectAndFailedReceipt_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void failedRepairRestoresTheCommittedProjectAndFailedReceipt()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory, output; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); arrange();
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool()); const auto before = manifest(directory);
			Input input; auto& model = Engine::getSong()->tempoModel(); model.setControllerConnection(new ControllerConnection(&input)); makeBad(input, 0);
			const auto cleanupBinding = qScopeGuard([&] { input.next = .25f; bool changed = false; QString error; model.disconnectController(changed, error); });
			const QPointer<ControllerConnection> old(model.controllerConnection()); command = request("failed-repair", "controller.disconnect", {{"parameter", int(model.id())}});
			bool injected = false;
			ModelValueNotificationProbe fault(&model, [&] {
				if (injected) { return; } injected = true;
				if (nonstandard) { throw 339; } throw std::runtime_error("Native disconnect repair notification fixture");
			}, true);
			receipt = runtime.process(command); fault.reset(); QVERIFY(injected); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!receipt.contains("result")); QVERIFY(!runtime.stopping());
			QVERIFY(!old); QVERIFY(!Engine::getSong()->tempoModel().controllerConnection()); QCOMPARE(Engine::getSong()->getTempo(), bpm_t(91));
			const auto after = manifest(directory); QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
			QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1); replay(runtime, command, receipt);
			QVERIFY(runtime.process(request("revise", "song.set", {{"tempo", 97}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QString error; QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt);
		QCOMPARE(Engine::getSong()->getTempo(), bpm_t(97)); render(reopened, "render-recovered", output.filePath("recovered.wav"));
	}
};
QTEST_GUILESS_MAIN(StudioParameterReadbackTest)
#include "StudioParameterReadbackTest.moc"
