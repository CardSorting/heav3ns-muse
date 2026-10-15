/* Native graph capability ownership and DSP checks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <limits>
#include "AudioBuffer.h"
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "GraphModel.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "MixHelpers.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"

using namespace lmms;

namespace
{
const Plugin::Descriptor fixtureDescriptor{"graph-fixture", "Graph fixture", "Native graph tests", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
class GraphEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	explicit GraphEffect(EffectChain* chain) : Effect(&fixtureDescriptor, chain, nullptr) {}
	graphModel wave{-1, 1, 4, this};
	graphModel quantized{0, 15, 4, this, false, 1};
	int edits = 0;
	QString lastEdited;
	EffectControls* controls() override { return &m_controls; }
	std::vector<GraphResource> graphResources() override
	{
		return {{"wave", "Wave", "amplitude", &wave}, {"quantized", "Quantized", "4-bit amplitude", &quantized}, {"preview", "Preview", "amplitude", &wave, false}};
	}
	void graphEdited(const QString& key) override { ++edits; lastEdited = key; }
protected:
	ProcessStatus processImpl(SampleFrame* buffer, f_cnt_t frames) override
	{
		for (f_cnt_t frame = 0; frame < frames; ++frame) { buffer[frame][0] *= wave.samples()[0]; buffer[frame][1] *= wave.samples()[0]; }
		return ProcessStatus::Continue;
	}
};
QJsonObject call(const QString& operation, const QJsonObject& arguments) { return StudioCapabilities::execute(operation, arguments); }
QJsonObject graphInfo(const QJsonObject& selector) { return call("graph.inspect", selector).value("result").toObject().value("graph").toObject(); }
}

class StudioGraphTest : public QObject
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

	void graphDiscoveryAndEditsSelectTheExactNativeEffect_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("mixer") << QString("mixer");
		QTest::newRow("song-instrument") << QString("instrument"); QTest::newRow("song-sample") << QString("sample");
		QTest::newRow("pattern-instrument") << QString("pattern-instrument"); QTest::newRow("pattern-sample") << QString("pattern-sample");
	}
	void graphDiscoveryAndEditsSelectTheExactNativeEffect()
	{
		QFETCH(QString, kind); QJsonObject selector; const auto chain = createChain(kind, selector);
		auto first = new GraphEffect(chain); auto second = new GraphEffect(chain); QVERIFY(chain->appendEffect(first)); QVERIFY(chain->appendEffect(second));
		const auto effects = call("effect.inspect", selector).value("result").toObject().value("effects").toArray();
		QCOMPARE(effects.size(), 2); QCOMPARE(effects.at(0).toObject().value("graphs").toArray().size(), 3);
		selector.insert("effect", 1); const auto discovery = call("graph.list", selector); QVERIFY(discovery.value("ok").toBool());
		const auto graphs = discovery.value("result").toObject().value("graphs").toArray(); QCOMPARE(graphs.size(), 3);
		const auto wave = graphs.at(0).toObject(); QCOMPARE(wave.value("key").toString(), QString("wave")); QCOMPARE(wave.value("length").toInt(), 4);
		QVERIFY(wave.value("fileImport").toBool()); QVERIFY(!wave.contains("values"));
		QVERIFY(!graphs.at(1).toObject().value("fileImport").toBool()); QCOMPARE(graphs.at(1).toObject().value("step").toInt(), 1);
		selector.insert("graph", "wave"); auto edit = selector; edit.insert("values", QJsonArray{.25, .5, .75, 1});
		QVERIFY(call("graph.set", edit).value("ok").toBool()); QCOMPARE(second->wave.samples()[0], .25f); QCOMPARE(first->wave.samples()[0], 0.f);
		QCOMPARE(graphInfo(selector).value("values").toArray(), (QJsonArray{.25, .5, .75, 1})); QCOMPARE(second->edits, 1); QCOMPARE(second->lastEdited, QString("wave"));
	}

	void invalidSamplesCannotPartiallyPublishOrInvokePluginHooks_data()
	{
		QTest::addColumn<QJsonValue>("values");
		QTest::newRow("object") << QJsonValue(QJsonObject{}); QTest::newRow("short") << QJsonValue(QJsonArray{.5, .5});
		QTest::newRow("long") << QJsonValue(QJsonArray{.5, .5, .5, .5, .5});
		QTest::newRow("late-string") << QJsonValue(QJsonArray{.5, .5, .5, "bad"});
		QTest::newRow("late-null") << QJsonValue(QJsonArray{.5, .5, .5, QJsonValue::Null});
		QTest::newRow("late-out-of-range") << QJsonValue(QJsonArray{.5, .5, .5, 1.001});
	}
	void invalidSamplesCannotPartiallyPublishOrInvokePluginHooks()
	{
		QFETCH(QJsonValue, values); auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new GraphEffect(&chain); QVERIFY(chain.appendEffect(effect));
		QVERIFY(!call("graph.set", {{"channel", 0}, {"effect", 0}, {"graph", "wave"}, {"values", values}}).value("ok").toBool());
		QCOMPARE(effect->edits, 0); for (int index = 0; index < 4; ++index) { QCOMPARE(effect->wave.samples()[index], 0.f); }
	}

	void quantizationAndNativeTransformsHaveDiscoverableSemantics()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new GraphEffect(&chain); QVERIFY(chain.appendEffect(effect));
		QJsonObject selector{{"channel", 0}, {"effect", 0}, {"graph", "quantized"}}; auto edit = selector; edit.insert("values", QJsonArray{.9, 5.9, 10.9, 15});
		QVERIFY(call("graph.set", edit).value("ok").toBool()); QCOMPARE(graphInfo(selector).value("values").toArray(), (QJsonArray{0, 5, 10, 15}));
		auto transform = selector; transform.insert("action", "invert"); QVERIFY(call("graph.transform", transform).value("ok").toBool());
		QCOMPARE(graphInfo(selector).value("values").toArray(), (QJsonArray{15, 10, 5, 0}));
		transform.insert("action", "sine"); QVERIFY(!call("graph.transform", transform).value("ok").toBool());
		transform.insert("action", "shift"); QVERIFY(!call("graph.transform", transform).value("ok").toBool());
		transform.insert("degrees", 180); QVERIFY(call("graph.transform", transform).value("ok").toBool());
		QCOMPARE(graphInfo(selector).value("values").toArray(), (QJsonArray{5, 0, 15, 10}));
		transform.insert("action", "clear"); QVERIFY(!call("graph.transform", transform).value("ok").toBool());
		QVERIFY(!call("graph.load", {{"channel", 0}, {"effect", 0}, {"graph", "quantized"}, {"path", "unused.wav"}}).value("ok").toBool());
	}

	void readOnlyGraphsAndMissingPluginsRefuseMutations()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new GraphEffect(&chain); QVERIFY(chain.appendEffect(effect));
		QVERIFY(graphInfo({{"channel", 0}, {"effect", 0}, {"graph", "preview"}}).contains("values"));
		QVERIFY(!call("graph.set", {{"channel", 0}, {"effect", 0}, {"graph", "preview"}, {"values", QJsonArray{0, 0, 0, 0}}}).value("ok").toBool());
		QCOMPARE(effect->edits, 0); QDomDocument document; auto node = document.createElement("effect"); node.setAttribute("name", "missing-graph-plugin");
		QVERIFY(chain.appendEffect(new DummyEffect(&chain, node))); QVERIFY(!call("graph.list", {{"channel", 0}, {"effect", 1}}).value("ok").toBool());
		const auto effects = call("effect.inspect", {{"channel", 0}}).value("result").toObject().value("effects").toArray();
		QVERIFY(effects.at(1).toObject().value("graphs").toArray().isEmpty());
	}

	void invalidSelectorsDoNotGuessAnOwner_data()
	{
		QTest::addColumn<QJsonObject>("selector");
		QTest::newRow("none") << QJsonObject{}; QTest::newRow("channel-without-effect") << QJsonObject{{"channel", 0}};
		QTest::newRow("effect-alone") << QJsonObject{{"effect", 0}};
		QTest::newRow("mixed-owner") << QJsonObject{{"channel", 0}, {"track", 0}, {"effect", 0}};
		QTest::newRow("mixed-container") << QJsonObject{{"channel", 0}, {"container", "song"}, {"effect", 0}};
		QTest::newRow("bad-index") << QJsonObject{{"channel", 0}, {"effect", 5}};
		QTest::newRow("fractional-index") << QJsonObject{{"channel", 0}, {"effect", .5}};
	}
	void invalidSelectorsDoNotGuessAnOwner()
	{
		QFETCH(QJsonObject, selector); auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new GraphEffect(&chain); QVERIFY(chain.appendEffect(effect));
		QVERIFY(!call("graph.list", selector).value("ok").toBool()); QCOMPARE(effect->edits, 0);
	}

	void nativeGraphEditsReachTheActiveEffectDsp()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new GraphEffect(&chain); QVERIFY(chain.appendEffect(effect));
		QVERIFY(call("graph.set", {{"channel", 0}, {"effect", 0}, {"graph", "wave"}, {"values", QJsonArray{.5, .5, .5, .5}}}).value("ok").toBool());
		AudioBuffer buffer(Engine::audioEngine()->framesPerPeriod()); buffer.allocateInterleavedBuffer();
		for (int side = 0; side < 2; ++side) { std::fill_n(buffer.group(0).buffer(side), buffer.frames(), side ? -.5f : .25f); buffer.assumeNonSilent(side); }
		toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer()); QVERIFY(chain.processAudioBuffer(buffer));
		for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[0], .125f); QCOMPARE(frame[1], -.25f); }
	}

	void overflowingNativeTransformIsRejectedBeforePublication()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new GraphEffect(&chain); QVERIFY(chain.appendEffect(effect));
		const float large = std::numeric_limits<float>::max(); const float values[]{large, large, large, large}; effect->wave.setSamples(values);
		QVERIFY(!call("graph.transform", {{"channel", 0}, {"effect", 0}, {"graph", "wave"}, {"action", "smooth"}}).value("ok").toBool());
		QCOMPARE(effect->edits, 0); for (int index = 0; index < 4; ++index) { QCOMPARE(effect->wave.samples()[index], large); }
	}
};

QTEST_GUILESS_MAIN(StudioGraphTest)
#include "StudioGraphTest.moc"
