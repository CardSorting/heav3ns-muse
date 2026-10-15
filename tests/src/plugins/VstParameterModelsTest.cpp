/* Native VST model ownership and legacy XML. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include "AudioBuffer.h"
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "MidiController.h"
#include "MixHelpers.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "VstParameterModels.h"

using namespace lmms;

namespace
{
QMap<QString, QString> dump(float first = .25f, float second = .75f)
{
	return {{"param0", "0:Gain: " + QString::number(first, 'g', 9)},
		{"param1", "1:Filter:cutoff:" + QString::number(second, 'g', 9)}};
}
const Plugin::Descriptor fixtureDescriptor{"vst-model-fixture", "VST model fixture", "Native model dispatch fixture", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
// This deterministic writer is not a VST compatibility test. It verifies the
// production model's discovery/automation path and the native EffectChain DSP.
class ParameterEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	float gain = .25f;
	int writes = 0;
	VstParameterModels parameters{&m_controls, [this](int index, float value) { if (index == 0) { gain = value; } ++writes; }};
	explicit ParameterEffect(EffectChain* chain) : Effect(&fixtureDescriptor, chain, nullptr) {}
	EffectControls* controls() override { return &m_controls; }
	void refreshParameterModels() override { parameters.synchronize(dump(gain)); }
protected:
	ProcessStatus processImpl(SampleFrame* buffer, f_cnt_t frames) override
	{
		for (f_cnt_t index = 0; index < frames; ++index) { buffer[index][0] *= gain; buffer[index][1] *= gain; }
		return ProcessStatus::Continue;
	}
};
}

class VstParameterModelsTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
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

	void namesColonsLocalesAndNumericIndexOrder()
	{
		QMap<QString, QString> records;
		for (int i = 0; i < 12; ++i) { records["param" + QString::number(i)] = QString::number(i) + ":Filter:cutoff:0,125"; }
		const auto values = VstParameterModels::decode(records);
		QCOMPARE(values.size(), std::size_t{12});
		for (int i = 0; i < 12; ++i) { QCOMPARE(values[i].index, i); QCOMPARE(values[i].name, QString("Filter:cutoff")); QCOMPARE(values[i].value, .125f); }
		QVERIFY(VstParameterModels::decode({}).empty());
		QCOMPARE(VstParameterModels::decode({{"param0", ".625"}}, true).at(0).value, .625f);
		QVERIFY_EXCEPTION_THROWN(VstParameterModels::decode({{"param0", ".625"}}), std::runtime_error);
	}

	void invalidRepliesDoNotPublishAnyValues_data()
	{
		QTest::addColumn<QString>("record");
		QTest::newRow("empty") << QString();
		QTest::newRow("missing-name") << QString("1:0.5");
		QTest::newRow("wrong-index") << QString("0:Cutoff:0.5");
		QTest::newRow("invalid-index") << QString("bad:Cutoff:0.5");
		QTest::newRow("negative-index") << QString("-1:Cutoff:0.5");
		QTest::newRow("overflow-index") << QString("99999999999999:Cutoff:0.5");
		QTest::newRow("invalid-value") << QString("1:Cutoff:oops");
		QTest::newRow("nan") << QString("1:Cutoff:nan");
		QTest::newRow("infinite") << QString("1:Cutoff:inf");
		QTest::newRow("negative-value") << QString("1:Cutoff:-0.1");
		QTest::newRow("excess-value") << QString("1:Cutoff:1.1");
	}
	void invalidRepliesDoNotPublishAnyValues()
	{
		QFETCH(QString, record); int writes = 0;
		VstParameterModels parameters(nullptr, [&](int, float) { ++writes; }); parameters.synchronize(dump());
		auto first = parameters.at(0); const auto id = first->id(); auto malformed = dump(.5f); malformed["param1"] = record;
		QVERIFY_EXCEPTION_THROWN(parameters.synchronize(malformed), std::runtime_error);
		QCOMPARE(parameters.at(0), first); QCOMPARE(first->id(), id); QCOMPARE(first->value(), .25f); QCOMPARE(writes, 0);
		parameters.synchronize(dump(.625f)); QCOMPARE(first->value(), .625f);
	}

	void modelsDriveNativeWriterAndKeepIdentityWithoutAnEditor()
	{
		std::vector<std::pair<int, float>> writes;
		Model owner(nullptr); VstParameterModels parameters(&owner, [&](int i, float v) { writes.emplace_back(i, v); });
		parameters.synchronize(dump()); auto model = parameters.at(1); const auto id = model->id();
		QCOMPARE(owner.findChildren<AutomatableModel*>().size(), 2); QVERIFY(writes.empty());
		model->setDisplayName("A label that is not an index"); model->setValue(.3125f);
		QCOMPARE(writes.size(), std::size_t{1}); QCOMPARE(writes[0].first, 1); QCOMPARE(writes[0].second, .3125f);
		parameters.synchronize(dump(.125f, .875f));
		QCOMPARE(parameters.at(1), model); QCOMPARE(model->id(), id); QCOMPARE(model->value(), .875f);
		QCOMPARE(writes.size(), std::size_t{1}); QCOMPARE(owner.findChildren<AutomatableModel*>().size(), 2);
		model->setValue(.5f, true); QCOMPARE(writes.size(), std::size_t{2}); QCOMPARE(writes.back().second, .5f);
		QVERIFY_EXCEPTION_THROWN(parameters.at(-1), std::out_of_range); QVERIFY_EXCEPTION_THROWN(parameters.at(2), std::out_of_range);
	}

	void layoutChangesFailAndExplicitClearRetiresOldTargets()
	{
		Model owner(nullptr); VstParameterModels parameters(&owner, [](int, float) {}); parameters.synchronize(dump());
		QPointer<FloatModel> old = parameters.at(0); auto changed = dump(); changed.remove("param1");
		QVERIFY_EXCEPTION_THROWN(parameters.synchronize(changed), std::runtime_error); QVERIFY(old); QCOMPARE(parameters.size(), 2);
		changed = {{"param0", "0:Gain:0.5"}, {"param2", "2:Gap:0.5"}};
		QVERIFY_EXCEPTION_THROWN(parameters.synchronize(changed), std::runtime_error); QCOMPARE(old->value(), .25f);
		parameters.clear(); QVERIFY(old.isNull()); QCOMPARE(owner.findChildren<AutomatableModel*>().size(), 0);
		parameters.synchronize({}); QCOMPARE(parameters.size(), 0);
		QVERIFY_EXCEPTION_THROWN(parameters.synchronize(dump()), std::runtime_error);
		parameters.clear(); parameters.synchronize(dump()); QCOMPARE(parameters.size(), 2);
	}

	void synchronizationPreservesAutomationControllersAndLinkedModels()
	{
		int writes = 0; VstParameterModels parameters(nullptr, [&](int, float) { ++writes; }); parameters.synchronize(dump());
		auto first = parameters.at(0); auto second = parameters.at(1);
		AutomationClip::globalAutomationClip(first); QVERIFY(AutomationClip::isConnected(first));
		second->setControllerConnection(new ControllerConnection(nullptr)); const auto controlled = second->value(); const auto before = writes;
		parameters.synchronize(dump(.875f, .125f)); QCOMPARE(first->value(), .25f); QCOMPARE(second->value(), controlled); QCOMPARE(writes, before);
		delete second->controllerConnection(); FloatModel linked(.375f, 0.f, 1.f, .01f); second->linkToModel(&linked);
		const auto linkedValue = second->value(); parameters.synchronize(dump(.5f, .875f)); QCOMPARE(second->value(), linkedValue); QCOMPARE(linked.value(), linkedValue);
	}

	void legacyXmlPreservesDumpAutomationIdsAndRepeatedRestore()
	{
		int writes = 0; VstParameterModels parameters(nullptr, [&](int, float) { ++writes; }); parameters.synchronize(dump());
		auto first = parameters.at(0); auto second = parameters.at(1); const auto firstId = first->id();
		AutomationClip::globalAutomationClip(first);
		second->setControllerConnection(new ControllerConnection(new MidiController(nullptr))); QPointer<ControllerConnection> old = second->controllerConnection();
		QDomDocument document; auto settings = document.createElement("vestige"); document.appendChild(settings);
		settings.setAttribute("param0", "0:Gain:0.25"); settings.setAttribute("param1", "1:Filter:cutoff:0.75");
		parameters.saveSettings(document, settings);
		QCOMPARE(settings.attribute("param1"), QString("1:Filter:cutoff:0.75"));
		QVERIFY(!settings.firstChildElement("connection").firstChildElement("param1").isNull());
		QVERIFY(!settings.firstChildElement("param0").isNull());
		const auto before = writes;
		for (int i = 0; i < 3; ++i)
		{
			parameters.restore(dump(), settings); QCOMPARE(parameters.at(0), first); QCOMPARE(parameters.at(1), second); QCOMPARE(first->id(), firstId);
			QVERIFY(second->controllerConnection()); QCOMPARE(second->controllerConnection()->getController()->type(), Controller::ControllerType::Midi);
		}
		QVERIFY(old.isNull()); QCOMPARE(writes, before); QCOMPARE(parameters.findChildren<AutomatableModel*>().size(), 2);
		QCOMPARE(first->value(), .25f); QCOMPARE(second->initValue(), .75f);
	}

	void invalidAutomationMetadataFailsBeforeChangingOtherModels()
	{
		VstParameterModels parameters(nullptr, [](int, float) {}); parameters.synchronize(dump());
		QDomDocument document; auto settings = document.createElement("controls"); document.appendChild(settings);
		auto bad = document.createElement("param1"); bad.setAttribute("id", parameters.at(1)->id()); bad.setAttribute("value", "nan"); settings.appendChild(bad);
		QVERIFY_EXCEPTION_THROWN(parameters.restore(dump(.875f), settings), std::runtime_error); QCOMPARE(parameters.at(0)->value(), .25f);
	}

	void ownerThreadAndSynchronousReentryAreRefused()
	{
		VstParameterModels parameters(nullptr, [](int, float) {}); parameters.synchronize(dump());
		bool refused = false; std::thread worker([&] { try { parameters.synchronize(dump()); } catch (const std::runtime_error&) { refused = true; } }); worker.join(); QVERIFY(refused);
		int reentries = 0;
		connect(parameters.at(0), &FloatModel::dataChanged, &parameters, [&] {
			try { parameters.checkOwner(); } catch (const std::runtime_error&) { ++reentries; }
		});
		parameters.synchronize(dump(.5f)); QCOMPARE(reentries, 1);
		parameters.synchronize(dump(.625f)); QCOMPARE(reentries, 2);
		bool checkpointRefused = false;
		FileResourceIO::Scope checkpoint([&] {
			try { parameters.checkOwner(); } catch (const std::runtime_error&) { checkpointRefused = true; }
			return false;
		});
		parameters.synchronize(dump(.625f)); QVERIFY(checkpointRefused);
	}

	void cancelledReplyDoesNotChangeExistingModels()
	{
		VstParameterModels parameters(nullptr, [](int, float) {}); parameters.synchronize(dump());
		{ FileResourceIO::Scope cancelled([] { return true; }); QVERIFY_EXCEPTION_THROWN(parameters.synchronize(dump(.875f)), std::runtime_error); }
		QCOMPARE(parameters.at(0)->value(), .25f); parameters.synchronize(dump(.5f)); QCOMPARE(parameters.at(0)->value(), .5f);
	}

	void studioDiscoveryAndEditsUseNativeOwnershipAndDsp_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("mixer") << QString("mixer");
		QTest::newRow("song-instrument") << QString("instrument"); QTest::newRow("song-sample") << QString("sample");
		QTest::newRow("pattern-instrument") << QString("pattern-instrument"); QTest::newRow("pattern-sample") << QString("pattern-sample");
	}
	void studioDiscoveryAndEditsUseNativeOwnershipAndDsp()
	{
		QFETCH(QString, kind); QJsonObject selector; EffectChain* chain = nullptr;
		if (kind == "mixer") { selector = {{"channel", 0}}; chain = &Engine::mixer()->mixerChannel(0)->m_fxChain; }
		else
		{
			const bool pattern = kind.startsWith("pattern-"); const bool sample = kind.endsWith("sample");
			auto track = Track::create(sample ? Track::Type::Sample : Track::Type::Instrument, pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : Engine::getSong());
			selector = {{"container", pattern ? "pattern" : "song"}, {"track", 0}};
			chain = sample ? static_cast<SampleTrack*>(track)->audioBusHandle()->effects() : static_cast<InstrumentTrack*>(track)->audioBusHandle()->effects();
		}
		auto first = new ParameterEffect(chain); auto second = new ParameterEffect(chain); QVERIFY(chain->appendEffect(first)); QVERIFY(chain->appendEffect(second));
		const auto inventory = StudioCapabilities::execute("effect.inspect", selector); QVERIFY(inventory.value("ok").toBool());
		const auto effects = inventory.value("result").toObject().value("effects").toArray(); QCOMPARE(effects.size(), 2);
		const auto id = second->parameters.at(0)->id(); bool discovered = false;
		for (const auto& item : effects.at(1).toObject().value("parameters").toArray())
		{
			const auto parameter = item.toObject(); if (parameter.value("id").toInt() == static_cast<int>(id)) { discovered = true; QVERIFY(parameter.value("name").toString().contains("Gain")); }
		}
		QVERIFY(discovered); QCOMPARE(first->writes, 0); QCOMPARE(second->writes, 0);
		QVERIFY(StudioCapabilities::execute("parameter.set", {{"parameter", static_cast<int>(id)}, {"value", .5}}).value("ok").toBool());
		QCOMPARE(first->gain, .25f); QCOMPARE(second->gain, .5f); QCOMPARE(second->writes, 1);
		AudioBuffer buffer(Engine::audioEngine()->framesPerPeriod()); buffer.allocateInterleavedBuffer();
		for (int side = 0; side < 2; ++side)
		{
			std::fill_n(buffer.group(0).buffer(side), buffer.frames(), side ? -.5f : .5f); buffer.assumeNonSilent(side);
		}
		toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer());
		QVERIFY(chain->processAudioBuffer(buffer));
		for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[0], .0625f); QCOMPARE(frame[1], -.0625f); }
		QVERIFY(StudioCapabilities::execute("effect.inspect", selector).value("ok").toBool()); QCOMPARE(second->parameters.at(0)->id(), id);
	}
};

QTEST_GUILESS_MAIN(VstParameterModelsTest)
#include "VstParameterModelsTest.moc"
