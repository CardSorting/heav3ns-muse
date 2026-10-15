/* Native expression dispatch, ownership and publication. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include "AudioBuffer.h"
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
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

using namespace lmms;

namespace
{
const Plugin::Descriptor fixtureDescriptor{"expression-fixture", "Native expression fixture", "Studio dispatch fixture", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
class ExpressionEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	explicit ExpressionEffect(EffectChain* chain) : Effect(&fixtureDescriptor, chain, nullptr) {}
	QString source = "1";
	float gain = 1;
	int validations = 0;
	int edits = 0;
	EffectControls* controls() override { return &m_controls; }
	std::vector<ExpressionResource> expressionResources() const override
	{
		return {{"output", "Output", source, "output", {"scalar"}, "A fixture scalar; empty source is silence"},
			{"fixed", "Fixed", "1", "output", {}, "Read only", false}};
	}
	ExpressionResult validateExpression(const QString&, const QString& text) override
	{
		++validations; ExpressionResult result; bool valid = text.isEmpty(); const float value = text.isEmpty() ? 0.f : text.toFloat(&valid);
		result.valid = valid && std::isfinite(value);
		if (result.valid) { result.preview.assign(4, value); result.sampleRate = 4; }
		else { result.error = "Native fixture parse error"; }
		return result;
	}
	bool setExpression(const QString& key, const QString& text, ExpressionResult& result) override
	{
		result = validateExpression(key, text); if (!result.valid) { return false; }
		source = text; gain = result.preview[0]; ++edits; return true;
	}
protected:
	ProcessStatus processImpl(SampleFrame* buffer, f_cnt_t frames) override
	{
		for (f_cnt_t index = 0; index < frames; ++index) { buffer[index][0] *= gain; buffer[index][1] *= gain; }
		return ProcessStatus::Continue;
	}
};
QJsonObject call(const QString& operation, const QJsonObject& arguments) { return StudioCapabilities::execute(operation, arguments); }
}

class StudioExpressionTest : public QObject
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

	void discoveryValidationAndEditsUseTheSelectedNativeOwner_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("mixer") << QString("mixer");
		QTest::newRow("song-instrument") << QString("instrument"); QTest::newRow("song-sample") << QString("sample");
		QTest::newRow("pattern-instrument") << QString("pattern-instrument"); QTest::newRow("pattern-sample") << QString("pattern-sample");
	}
	void discoveryValidationAndEditsUseTheSelectedNativeOwner()
	{
		QFETCH(QString, kind); QJsonObject selector; const auto chain = createChain(kind, selector);
		auto first = new ExpressionEffect(chain); auto second = new ExpressionEffect(chain); QVERIFY(chain->appendEffect(first)); QVERIFY(chain->appendEffect(second));
		const auto effects = call("effect.inspect", selector).value("result").toObject().value("effects").toArray();
		QCOMPARE(effects.at(0).toObject().value("expressions").toArray().size(), 2);
		selector.insert("effect", 1); const auto discovery = call("expression.list", selector); QVERIFY(discovery.value("ok").toBool());
		const auto expression = discovery.value("result").toObject().value("expressions").toArray().at(0).toObject();
		QCOMPARE(expression.value("key").toString(), QString("output")); QCOMPARE(expression.value("source").toString(), QString("1"));
		QVERIFY(expression.value("symbols").toArray().contains("scalar")); QVERIFY(expression.value("writable").toBool());
		selector.insert("expression", "output"); selector.insert("source", ".5");
		const bool modified = Engine::getSong()->isModified(); const auto validation = call("expression.validate", selector);
		QVERIFY(validation.value("ok").toBool()); QVERIFY(validation.value("result").toObject().value("valid").toBool());
		QVERIFY(!validation.value("result").toObject().contains("preview")); QCOMPARE(second->source, QString("1")); QCOMPARE(Engine::getSong()->isModified(), modified);
		selector.insert("preview", true); const auto result = call("expression.set", selector); QVERIFY(result.value("ok").toBool());
		QCOMPARE(result.value("result").toObject().value("preview").toArray(), (QJsonArray{.5, .5, .5, .5}));
		QCOMPARE(result.value("result").toObject().value("expression").toObject().value("source").toString(), QString(".5"));
		QCOMPARE(first->edits, 0); QCOMPARE(second->edits, 1); QCOMPARE(first->gain, 1.f); QCOMPARE(second->gain, .5f);
	}

	void invalidFormulaIsDiagnosticWithoutPublishingAndSetIsRefused()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ExpressionEffect(&chain); QVERIFY(chain.appendEffect(effect));
		const QJsonObject args{{"channel", 0}, {"effect", 0}, {"expression", "output"}, {"source", "broken("}, {"preview", true}};
		const auto validation = call("expression.validate", args); QVERIFY(validation.value("ok").toBool());
		QVERIFY(!validation.value("result").toObject().value("valid").toBool());
		QCOMPARE(validation.value("result").toObject().value("error").toString(), QString("Native fixture parse error"));
		QVERIFY(!call("expression.set", args).value("ok").toBool()); QCOMPARE(effect->edits, 0); QCOMPARE(effect->source, QString("1"));
	}

	void readonlyMissingAndUnknownExpressionsAreNotEditable()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ExpressionEffect(&chain); QVERIFY(chain.appendEffect(effect));
		for (const auto key : {"fixed", "unknown"})
		{
			QVERIFY(!call("expression.set", {{"channel", 0}, {"effect", 0}, {"expression", key}, {"source", "2"}}).value("ok").toBool());
		}
		QCOMPARE(effect->validations, 0); QDomDocument document; auto node = document.createElement("effect"); node.setAttribute("name", "missing-expression-plugin");
		QVERIFY(chain.appendEffect(new DummyEffect(&chain, node))); QVERIFY(!call("expression.list", {{"channel", 0}, {"effect", 1}}).value("ok").toBool());
		const auto effects = call("effect.inspect", {{"channel", 0}}).value("result").toObject().value("effects").toArray();
		QVERIFY(effects.at(1).toObject().value("expressions").toArray().isEmpty());
	}

	void malformedSelectorsAndSourceTypesFailBeforePluginEntry_data()
	{
		QTest::addColumn<QJsonObject>("args");
		QTest::newRow("channel-alone") << QJsonObject{{"channel", 0}, {"expression", "output"}, {"source", "2"}};
		QTest::newRow("mixed-owner") << QJsonObject{{"channel", 0}, {"track", 0}, {"effect", 0}, {"expression", "output"}, {"source", "2"}};
		QTest::newRow("numeric-source") << QJsonObject{{"channel", 0}, {"effect", 0}, {"expression", "output"}, {"source", 2}};
		QTest::newRow("missing-source") << QJsonObject{{"channel", 0}, {"effect", 0}, {"expression", "output"}};
		QTest::newRow("numeric-preview") << QJsonObject{{"channel", 0}, {"effect", 0}, {"expression", "output"}, {"source", "2"}, {"preview", 1}};
	}
	void malformedSelectorsAndSourceTypesFailBeforePluginEntry()
	{
		QFETCH(QJsonObject, args); auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ExpressionEffect(&chain); QVERIFY(chain.appendEffect(effect));
		QVERIFY(!call("expression.set", args).value("ok").toBool()); QCOMPARE(effect->validations, 0); QCOMPARE(effect->edits, 0);
	}

	void acceptedNativeExpressionReachesDspAndEmptySourceCanSilenceIt()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ExpressionEffect(&chain); QVERIFY(chain.appendEffect(effect));
		for (const auto source : {QString(".5"), QString{}})
		{
			QVERIFY(call("expression.set", {{"channel", 0}, {"effect", 0}, {"expression", "output"}, {"source", source}}).value("ok").toBool());
			AudioBuffer buffer(Engine::audioEngine()->framesPerPeriod()); buffer.allocateInterleavedBuffer();
			for (int side = 0; side < 2; ++side) { std::fill_n(buffer.group(0).buffer(side), buffer.frames(), .25f); buffer.assumeNonSilent(side); }
			toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer()); QVERIFY(chain.processAudioBuffer(buffer));
			for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[0], source.isEmpty() ? 0.f : .125f); QCOMPARE(frame[1], frame[0]); }
		}
	}
};

QTEST_GUILESS_MAIN(StudioExpressionTest)
#include "StudioExpressionTest.moc"
