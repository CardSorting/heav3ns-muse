/* Native effect/send discovery and editable control ownership.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <memory>
#include <vector>
#include "AudioBuffer.h"
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "MixHelpers.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioParameterTestUtils.h"
#include "StudioRuntime.h"

using namespace lmms;

namespace
{
const Plugin::Descriptor fixtureDescriptor{"effect-controls-fixture", "Repeated effect", "Native control ownership fixture", "LMMS tests",
	1, Plugin::Type::Effect, nullptr, "", nullptr};
class ProbeEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	ProbeEffect(EffectChain* chain, const EffectKey* key = nullptr) : Effect(&fixtureDescriptor, chain, key)
	{
		mode.setValueLabels({{0, "First"}, {1, "Second"}});
	}
	std::unique_ptr<FloatModel> gain = std::make_unique<FloatModel>(1.f, 0.f, 2.f, .01f, this, "Gain");
	IntModel mode{0, 0, 1, this, "Mode"};
	bool replaceOnRefresh = false;
	EffectControls* controls() override { return &m_controls; }
	void refreshParameterModels() override
	{
		if (replaceOnRefresh) { replaceOnRefresh = false; gain = std::make_unique<FloatModel>(.75f, 0.f, 2.f, .01f, this, "Gain"); }
	}
protected:
	ProcessStatus processImpl(SampleFrame* buffer, f_cnt_t frames) override
	{
		for (f_cnt_t frame = 0; frame < frames; ++frame) { buffer[frame][0] *= gain->value(); buffer[frame][1] *= gain->value(); }
		return ProcessStatus::Continue;
	}
};
QJsonObject call(const QString& op, const QJsonObject& args = {}) { return test::executeWithAllParameters(op, args); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject metadata(const QJsonArray& parameters, const AutomatableModel* model)
{
	for (const auto item : parameters) { if (item.toObject().value("id").toInt() == static_cast<int>(model->id())) { return item.toObject(); } }
	return {};
}
QJsonObject parameterInfo(AutomatableModel* model)
{
	return call("parameter.inspect", {{"parameter", static_cast<int>(model->id())}}).value("result").toObject();
}
QJsonObject manifest(const QString& session)
{
	QFile file(session + "/session.json"); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
void fill(AudioBuffer& buffer, float left, float right)
{
	for (int side = 0; side < 2; ++side)
	{
		std::fill_n(buffer.group(0).buffer(side), buffer.frames(), side ? right : left); buffer.assumeNonSilent(side);
	}
	toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer());
}
}

class StudioEffectControlsTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	EffectChain* createChain(const QString& kind, QJsonObject& selector)
	{
		if (kind == "mixer") { selector = {{"channel", 0}}; return &Engine::mixer()->mixerChannel(0)->m_fxChain; }
		const bool pattern = kind.startsWith("pattern-"); const bool sample = kind.endsWith("sample");
		const auto track = Track::create(sample ? Track::Type::Sample : Track::Type::Instrument,
			pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : Engine::getSong());
		selector = {{"container", pattern ? "pattern" : "song"}, {"track", 0}};
		return sample ? static_cast<SampleTrack*>(track)->audioBusHandle()->effects() : static_cast<InstrumentTrack*>(track)->audioBusHandle()->effects();
	}
	void chainRows()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("mixer") << QString("mixer");
		QTest::newRow("song-instrument") << QString("instrument"); QTest::newRow("song-sample") << QString("sample");
		QTest::newRow("pattern-instrument") << QString("pattern-instrument"); QTest::newRow("pattern-sample") << QString("pattern-sample");
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->clearProject(); }

	void nativeChainControlsAndRepeatedEffectsAreDiscoverable_data() { chainRows(); }
	void nativeChainControlsAndRepeatedEffectsAreDiscoverable()
	{
		QFETCH(QString, kind); QJsonObject selector; const auto chain = createChain(kind, selector);
		EffectKey key(&fixtureDescriptor, "Variant", {{"label", "variant-a"}}); auto first = new ProbeEffect(chain, &key); auto second = new ProbeEffect(chain, &key);
		QVERIFY(chain->appendEffect(first)); QVERIFY(chain->appendEffect(second)); second->gain->setValue(.5f);
		const auto response = call("effect.inspect", selector); QVERIFY(response.value("ok").toBool()); const auto result = response.value("result").toObject();
		QVERIFY(result.value("enabled").toBool()); const auto bypass = result.value("enabledParameter").toObject();
		QCOMPARE(bypass.value("id").toInt(), static_cast<int>(chain->enabledModel()->id())); QCOMPARE(bypass.value("kind").toString(), QString("boolean"));
		auto chainLocation = selector; chainLocation.insert("scope", "effectChain"); QCOMPARE(bypass.value("location").toObject(), chainLocation);
		const auto effects = result.value("effects").toArray(); QCOMPARE(effects.size(), 2);
		QSet<int> ids;
		for (int index = 0; index < 2; ++index)
		{
			const auto native = index ? second : first; const auto item = effects.at(index).toObject();
			QCOMPARE(item.value("plugin").toString(), QString("effect-controls-fixture")); QVERIFY(!item.value("missing").toBool());
			QCOMPARE(item.value("attributes").toObject().value("label").toString(), QString("variant-a"));
			const auto parameters = item.value("parameters").toArray(); QVERIFY(!metadata(parameters, native->gain.get()).isEmpty());
			QCOMPARE(metadata(parameters, &native->mode).value("choices").toArray().size(), 2);
			auto location = selector; location.insert("effect", index); location.insert("scope", "effect");
			for (const auto value : parameters)
			{
				const auto parameter = value.toObject(); QCOMPARE(parameter.value("location").toObject(), location);
				QVERIFY(!ids.contains(parameter.value("id").toInt())); ids.insert(parameter.value("id").toInt());
				QVERIFY(call("parameter.inspect", {{"parameter", parameter.value("id")}}).value("ok").toBool());
			}
			auto filter = selector; filter.insert("effect", index); filter.insert("choices", false);
			const auto filtered = call("parameter.list", filter).value("result").toObject().value("parameters").toArray(); QCOMPARE(filtered.size(), parameters.size());
			for (const auto parameter : filtered) { QCOMPARE(parameter.toObject().value("location").toObject(), location); QVERIFY(!parameter.toObject().contains("choices")); }
		}
		const auto all = call("parameter.list", selector).value("result").toObject().value("parameters").toArray(); QVERIFY(!metadata(all, chain->enabledModel()).isEmpty());
		auto selected = selector; selected.insert("effect", 1); selected.insert("details", false);
		const auto one = call("effect.inspect", selected).value("result").toObject().value("effects").toArray(); QCOMPARE(one.size(), 1);
		QCOMPARE(one.at(0).toObject().value("index").toInt(), 1); QVERIFY(!one.at(0).toObject().contains("parameters"));
		const auto project = StudioCapabilities::inspect(); const auto records = project.value(kind == "mixer" ? "channels" : kind.startsWith("pattern-") ? "patternTracks" : "tracks").toArray();
		QCOMPARE(records.at(0).toObject().value("effects").toArray().size(), 2); QVERIFY(records.at(0).toObject().value("effectsEnabled").toBool());
		if (kind != "mixer") { QCOMPARE(records.at(0).toObject().value("mixerChannel").toInt(-1), 0); }
		if (kind.endsWith("instrument"))
		{
			const auto instrument = call("instrument.inspect", selector).value("result").toObject().value("parameters").toArray();
			QVERIFY(!metadata(instrument, chain->enabledModel()).isEmpty()); QVERIFY(!metadata(instrument, second->gain.get()).isEmpty());
		}
	}

	void bypassHandleChangesTheActualNativeProcessor()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ProbeEffect(&chain); effect->gain->setValue(2.f); QVERIFY(chain.appendEffect(effect));
		const auto discovered = call("effect.inspect", {{"channel", 0}}).value("result").toObject().value("enabledParameter").toObject();
		const auto id = discovered.value("id"); AudioBuffer buffer(Engine::audioEngine()->framesPerPeriod()); buffer.allocateInterleavedBuffer();
		QVERIFY(call("parameter.set", {{"parameter", id}, {"value", 0}}).value("ok").toBool()); fill(buffer, .125f, -.25f);
		QVERIFY(!chain.processAudioBuffer(buffer)); for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[0], .125f); QCOMPARE(frame[1], -.25f); }
		QVERIFY(call("parameter.set", {{"parameter", id}, {"value", 1}}).value("ok").toBool()); fill(buffer, .125f, -.25f);
		QVERIFY(chain.processAudioBuffer(buffer)); for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[0], .25f); QCOMPARE(frame[1], -.5f); }
		QVERIFY(!call("parameter.set", {{"parameter", id}, {"value", .5}}).value("ok").toBool()); QVERIFY(chain.enabledModel()->value());
		QVERIFY(chain.clear()); QCOMPARE(parameterInfo(chain.enabledModel()).value("value").toInt(), 0);
	}

	void mixerSendControlsAreDiscoverableAndDriveNativeAudio()
	{
		auto mixer = Engine::mixer(); mixer->createChannel(); mixer->createChannel(); mixer->deleteChannelSend(1, 0);
		const auto route = mixer->createChannelSend(1, 2, .25f); QVERIFY(route);
		const auto listed = call("parameter.list", {{"from", 1}, {"to", 2}}).value("result").toObject().value("parameters").toArray(); QCOMPARE(listed.size(), 1);
		const auto entry = listed.at(0).toObject(); QCOMPARE(entry.value("id").toInt(), static_cast<int>(route->amount()->id()));
		const QJsonObject location{{"scope", "mixerRoute"}, {"from", 1}, {"to", 2}}; QCOMPARE(entry.value("location").toObject(), location);
		QVERIFY(call("parameter.set", {{"parameter", entry.value("id")}, {"value", .5}}).value("ok").toBool()); QCOMPARE(route->amount()->value(), .5f);
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod());
		for (int period = 0; period < 2; ++period)
		{
			// The first period performs the native smoothing ramp to the new send.
			AutomatableModel::incrementPeriodCounter(); std::fill(output.begin(), output.end(), SampleFrame{0.f, 0.f});
			fill(mixer->mixerChannel(1)->m_buffer, .25f, -.125f); mixer->masterMix(output.data());
		}
		for (const auto& frame : output) { QCOMPARE(frame[0], .125f); QCOMPARE(frame[1], -.0625f); }
		QVERIFY(!call("parameter.set", {{"parameter", entry.value("id")}, {"value", 1.25}}).value("ok").toBool()); QCOMPARE(route->amount()->value(), .5f);
		QVERIFY(mixer->moveChannel(1, 2)); const auto moved = call("parameter.inspect", {{"parameter", entry.value("id")}}).value("result").toObject();
		QCOMPARE(moved.value("location").toObject().value("from").toInt(), 2); QCOMPARE(moved.value("location").toObject().value("to").toInt(), 1);
		mixer->deleteChannelSend(2, 1); QVERIFY(!call("parameter.inspect", {{"parameter", entry.value("id")}}).value("ok").toBool());
	}

	void effectAndChannelMovesUpdateLocationsWithoutChangingHandles()
	{
		auto mixer = Engine::mixer(); mixer->createChannel(); mixer->createChannel(); auto& chain = mixer->mixerChannel(1)->m_fxChain;
		auto first = new ProbeEffect(&chain); QVERIFY(chain.appendEffect(first)); QVERIFY(chain.appendEffect(new ProbeEffect(&chain))); const auto id = first->gain->id();
		QVERIFY(chain.moveEffect(0, 1)); QVERIFY(mixer->moveChannel(1, 2));
		const auto entry = call("parameter.inspect", {{"parameter", static_cast<int>(id)}}).value("result").toObject();
		const QJsonObject location{{"channel", 2}, {"effect", 1}, {"scope", "effect"}}; QCOMPARE(entry.value("location").toObject(), location);
		const auto controls = call("parameter.list", {{"channel", 2}, {"effect", 1}}).value("result").toObject().value("parameters").toArray(); QVERIFY(!metadata(controls, first->gain.get()).isEmpty());
		const auto channelControls = call("parameter.list", {{"channel", 2}}).value("result").toObject().value("parameters").toArray();
		QVERIFY(!metadata(channelControls, &mixer->mixerChannel(2)->m_volumeModel).isEmpty()); QVERIFY(!metadata(channelControls, chain.enabledModel()).isEmpty());
	}

	void missingAndDetachedEffectsDoNotAdvertiseNonpersistentControls()
	{
		QJsonObject selector; const auto chain = createChain("instrument", selector); QDomDocument document; auto node = document.createElement("effect"); document.appendChild(node);
		node.setAttribute("name", "absent-original-plugin"); auto key = document.createElement("key"); node.appendChild(key);
		auto attribute = document.createElement("attribute"); attribute.setAttribute("name", "file"); attribute.setAttribute("value", "preserved-original-library"); key.appendChild(attribute);
		auto missing = new DummyEffect(chain, node); QVERIFY(chain->appendEffect(missing)); auto detached = new ProbeEffect(chain); QVERIFY(chain->appendEffect(detached));
		const auto detachedId = static_cast<int>(detached->gain->id()); QVERIFY(chain->removeEffect(detached)); QCOMPARE(detached->parent(), static_cast<QObject*>(chain));
		const auto item = call("effect.inspect", selector).value("result").toObject().value("effects").toArray().at(0).toObject();
		QVERIFY(item.value("missing").toBool()); QCOMPARE(item.value("plugin").toString(), QString("absent-original-plugin"));
		QCOMPARE(item.value("attributes").toObject().value("file").toString(), QString("preserved-original-library"));
		QVERIFY(item.value("parameters").toArray().isEmpty()); QVERIFY(!item.contains("wet")); QVERIFY(!item.contains("enabled"));
		QVERIFY(!call("parameter.inspect", {{"parameter", detachedId}}).value("ok").toBool());
		QVERIFY(!call("parameter.inspect", {{"parameter", static_cast<int>(missing->autoQuitModel()->id())}}).value("ok").toBool());
		const auto instrument = call("instrument.inspect", selector).value("result").toObject().value("parameters").toArray();
		QVERIFY(metadata(instrument, detached->gain.get()).isEmpty()); QVERIFY(metadata(instrument, missing->autoQuitModel()).isEmpty());
		QCOMPARE(StudioCapabilities::inspect().value("tracks").toArray().at(0).toObject().value("effects").toArray().at(0).toString(), QString("absent-original-plugin"));
		delete detached;
	}

	void pluginRefreshFinishesBeforeModelHandlesAreCollected_data()
	{
		QTest::addColumn<QString>("operation"); QTest::newRow("parameter-list") << QString("parameter.list");
		QTest::newRow("effect-inspect") << QString("effect.inspect"); QTest::newRow("instrument-inspect") << QString("instrument.inspect");
	}
	void pluginRefreshFinishesBeforeModelHandlesAreCollected()
	{
		QFETCH(QString, operation); QJsonObject selector; const auto chain = createChain("instrument", selector);
		auto effect = new ProbeEffect(chain); QVERIFY(chain->appendEffect(effect)); QPointer<FloatModel> old = effect->gain.get(); const auto id = old->id(); effect->replaceOnRefresh = true;
		const auto response = call(operation, selector); QVERIFY(response.value("ok").toBool()); QVERIFY(old.isNull());
		const auto result = response.value("result").toObject();
		const auto parameters = operation == "effect.inspect" ? result.value("effects").toArray().at(0).toObject().value("parameters").toArray() : result.value("parameters").toArray();
		QVERIFY(!metadata(parameters, effect->gain.get()).isEmpty()); for (const auto item : parameters) { QVERIFY(item.toObject().value("id").toInt() != static_cast<int>(id)); }
	}

	void chainAndRouteHandlesUseNativeControllerConnections()
	{
		auto mixer = Engine::mixer(); mixer->createChannel(); auto& chain = mixer->mixerChannel(1)->m_fxChain;
		const auto route = mixer->channelSendModel(1, 0); QVERIFY(route); route->setValue(.75f);
		QVERIFY(call("controller.add", {{"type", "lfo"}}).value("ok").toBool());
		for (const auto model : {static_cast<AutomatableModel*>(chain.enabledModel()), static_cast<AutomatableModel*>(route)})
		{
			const auto stored = model->storedValue(); const auto id = static_cast<int>(model->id());
			QVERIFY(call("controller.connect", {{"controller", 0}, {"parameter", id}}).value("ok").toBool()); QVERIFY(model->controllerConnection());
			const auto entry = call("parameter.inspect", {{"parameter", id}}).value("result").toObject();
			QCOMPARE(entry.value("controller").toObject().value("index").toInt(-1), 0);
			QVERIFY(call("controller.disconnect", {{"parameter", id}}).value("ok").toBool()); QVERIFY(!model->controllerConnection()); QCOMPARE(model->storedValue(), stored);
		}
	}

	void ambiguousChainSelectorsNeverReachAnEffectEdit()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; QVERIFY(chain.appendEffect(new ProbeEffect(&chain)));
		const auto before = chain.effects();
		for (const auto operation : {"effect.inspect", "effect.add", "effect.remove", "effect.move", "effect.clear"})
		{
			QJsonObject args{{"channel", 0}, {"track", 0}};
			if (QString(operation) == "effect.add") { args.insert("plugin", "amplifier"); }
			if (QString(operation) == "effect.remove" || QString(operation) == "effect.move") { args.insert("effect", 0); }
			if (QString(operation) == "effect.move") { args.insert("to", 0); }
			const auto response = call(operation, args); QVERIFY(!response.value("ok").toBool());
			QVERIFY(response.value("error").toString().contains("not both")); QVERIFY(chain.effects() == before);
		}
	}

	void ambiguousAndInvalidControlFiltersAreRefused_data()
	{
		QTest::addColumn<QJsonObject>("args");
		QTest::newRow("channel-track") << QJsonObject{{"channel", 0}, {"track", 0}};
		QTest::newRow("channel-container") << QJsonObject{{"channel", 0}, {"container", "song"}};
		QTest::newRow("channel-controller") << QJsonObject{{"channel", 0}, {"controller", 0}};
		QTest::newRow("track-route") << QJsonObject{{"track", 0}, {"from", 1}};
		QTest::newRow("effect-alone") << QJsonObject{{"effect", 0}};
		QTest::newRow("effect-container") << QJsonObject{{"effect", 0}, {"container", "song"}};
		QTest::newRow("effect-controller") << QJsonObject{{"effect", 0}, {"controller", 0}};
		QTest::newRow("invalid-channel") << QJsonObject{{"channel", 8}};
		QTest::newRow("invalid-effect") << QJsonObject{{"channel", 0}, {"effect", -1}};
		QTest::newRow("fractional-effect") << QJsonObject{{"channel", 0}, {"effect", .5}};
		QTest::newRow("invalid-route") << QJsonObject{{"from", -1}};
	}
	void ambiguousAndInvalidControlFiltersAreRefused()
	{
		QFETCH(QJsonObject, args); QJsonObject selector; const auto chain = createChain("instrument", selector); QVERIFY(chain->appendEffect(new ProbeEffect(chain)));
		const auto before = StudioCapabilities::inspect(); QVERIFY(!call("parameter.list", args).value("ok").toBool()); QCOMPARE(StudioCapabilities::inspect(), before);
	}

	void bypassAndSendAutomationSurviveNativeHistoryAndReopening_data() { chainRows(); }
	void bypassAndSendAutomationSurviveNativeHistoryAndReopening()
	{
		QFETCH(QString, kind); QTemporaryDir directory; const auto session = directory.filePath("session"); QString error; QJsonObject selector;
		{
			StudioRuntime runtime(session); QVERIFY2(runtime.open(error), qPrintable(error)); const auto chain = createChain(kind, selector);
			QDomDocument document; auto node = document.createElement("effect"); node.setAttribute("name", "unavailable-preserved-effect"); QVERIFY(chain->appendEffect(new DummyEffect(chain, node)));
			Engine::mixer()->createChannel();
			QVERIFY(runtime.process(request("seed", "mixer.edit", {{"channel", 1}, {"name", "Automated send"}})).value("ok").toBool());
			const auto enabled = call("effect.inspect", selector).value("result").toObject().value("enabledParameter").toObject().value("id");
			const auto change = request("disable", "parameter.set", {{"parameter", enabled}, {"value", 0}});
			QVERIFY(runtime.process(change).value("ok").toBool()); QVERIFY(!call("effect.inspect", selector).value("result").toObject().value("enabled").toBool());
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); QVERIFY(call("effect.inspect", selector).value("result").toObject().value("enabled").toBool());
			QVERIFY(runtime.process(change).value("replayed").toBool()); QVERIFY(call("effect.inspect", selector).value("result").toObject().value("enabled").toBool());
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool());
			const auto fresh = call("effect.inspect", selector).value("result").toObject().value("enabledParameter").toObject().value("id");
			const auto send = call("parameter.list", {{"from", 1}, {"to", 0}}).value("result").toObject().value("parameters").toArray().at(0).toObject().value("id");
			QVERIFY(runtime.process(request("send-level", "parameter.set", {{"parameter", send}, {"value", .375}})).value("ok").toBool());
			QVERIFY(runtime.process(request("bypass-automation", "automation.global.point", {{"parameter", fresh}, {"position", 0}, {"value", 1}})).value("ok").toBool());
			QVERIFY(runtime.process(request("send-automation", "automation.global.point", {{"parameter", send}, {"position", 0}, {"value", .25}})).value("ok").toBool());
			const auto beforeReads = manifest(session); QVERIFY(runtime.process(request("read-chain", "effect.inspect", selector)).value("ok").toBool());
			QVERIFY(runtime.process(request("read-controls", "parameter.list", {{"from", 1}})).value("ok").toBool());
			const auto afterReads = manifest(session); QCOMPARE(afterReads.value("history"), beforeReads.value("history")); QCOMPARE(afterReads.value("cursor"), beforeReads.value("cursor"));
			QCOMPARE(afterReads.value("revision"), beforeReads.value("revision"));
			QVERIFY(runtime.process(request("save", "project.save", {{"path", directory.filePath("controls.mmp")}})).value("ok").toBool());
			QVERIFY(runtime.process(request("load", "project.load", {{"path", directory.filePath("controls.mmp")}})).value("ok").toBool());
		}
		Engine::getSong()->clearProject(); StudioRuntime reopened(session); QVERIFY2(reopened.open(error), qPrintable(error));
		const auto chainState = call("effect.inspect", selector).value("result").toObject(); QVERIFY(!chainState.value("enabled").toBool());
		const auto enabled = chainState.value("enabledParameter").toObject().value("id");
		const auto sends = call("parameter.list", {{"from", 1}, {"to", 0}}).value("result").toObject().value("parameters").toArray(); QCOMPARE(sends.size(), 1);
		QCOMPARE(sends.at(0).toObject().value("manualValue").toDouble(), .375);
		for (const auto id : {enabled, sends.at(0).toObject().value("id")})
		{
			const auto automation = call("automation.global.inspect", {{"parameter", id}, {"position", 0}}); QVERIFY(automation.value("ok").toBool());
			const auto result = automation.value("result").toObject(); QVERIFY(result.value("hasAutomation").toBool());
			QCOMPARE(result.value("targets").toArray().size(), 1); QCOMPARE(result.value("targets").toArray().at(0).toObject().value("parameter"), id);
			QCOMPARE(result.value("value").toDouble(), id == enabled ? 1. : .25);
		}
	}
};

QTEST_GUILESS_MAIN(StudioEffectControlsTest)
#include "StudioEffectControlsTest.moc"
