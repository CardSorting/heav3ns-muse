/* Native plugin program dispatch. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QTemporaryDir>
#include <algorithm>
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
#include "StudioParameterTestUtils.h"

using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"program-fixture", "Program fixture", "Native dispatch fixture", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
class ProgramEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	explicit ProgramEffect(EffectChain* chain) : Effect(&fixtureDescriptor, chain, nullptr) {}
	int selected = 0, reads = 0, edits = 0;
	bool malformed = false, refuse = false, lie = false;
	FloatModel value{.25f, 0.f, 1.f, .01f, this, "Native program gain"};
	EffectControls* controls() override { return &m_controls; }
	bool supportsNativePrograms() const override { return true; }
	NativeProgramPage nativePrograms(int offset, int limit) override
	{
		++reads; NativeProgramPage page{700, selected, offset, {}};
		const int length = offset >= 700 ? 0 : std::min(limit, 700 - offset);
		for (int relative = 0; relative < length; ++relative) { const int i = offset + relative; page.programs.push_back({i, i == 512 ? "A|B: Native" : QString{}, i == 512}); }
		if (malformed) { page.current = 700; }
		return page;
	}
	bool selectNativeProgram(int index, QString& error) override
	{
		if (refuse || index < 0 || index >= 700) { error = "Native program is unavailable"; return false; }
		++edits; if (!lie) { selected = index; value.setValue(.5f); } return true;
	}
protected:
	ProcessStatus processImpl(SampleFrame* frames, f_cnt_t count) override
	{
		for (f_cnt_t i = 0; i < count; ++i) { frames[i][0] *= value.value(); frames[i][1] *= value.value(); }
		return ProcessStatus::Continue;
	}
};
QJsonObject call(const QString& operation, const QJsonObject& args) { return test::executeWithAllParameters(operation, args); }
}

class StudioPluginProgramTest : public QObject
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

	void programsSelectTheRequestedOwnerAndNativeDsp_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("mixer") << QString("mixer");
		QTest::newRow("song-instrument") << QString("instrument"); QTest::newRow("song-sample") << QString("sample");
		QTest::newRow("pattern-instrument") << QString("pattern-instrument"); QTest::newRow("pattern-sample") << QString("pattern-sample");
	}
	void programsSelectTheRequestedOwnerAndNativeDsp()
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
		auto first = new ProgramEffect(chain); auto second = new ProgramEffect(chain); QVERIFY(chain->appendEffect(first)); QVERIFY(chain->appendEffect(second));
		const auto inventory = call("effect.inspect", selector).value("result").toObject().value("effects").toArray();
		QVERIFY(inventory.at(1).toObject().value("nativeProgramsSupported").toBool()); selector.insert("effect", 1);
		const bool modified = Engine::getSong()->isModified(); const auto id = second->value.id();
		auto pageArgs = selector; pageArgs.insert("offset", 510); pageArgs.insert("limit", 4);
		const auto response = call("plugin.program.list", pageArgs); QVERIFY(response.value("ok").toBool());
		const auto page = response.value("result").toObject(); QCOMPARE(page.value("total").toInt(), 700); QCOMPARE(page.value("current").toInt(), 0);
		const auto entries = page.value("programs").toArray(); QCOMPARE(entries.size(), 4); QVERIFY(page.value("hasMore").toBool());
		QVERIFY(!entries.at(0).toObject().value("nameAvailable").toBool()); QCOMPARE(entries.at(2).toObject().value("name").toString(), QString("A|B: Native"));
		QCOMPARE(first->reads, 0); QCOMPARE(second->edits, 0); QCOMPARE(second->value.value(), .25f); QCOMPARE(Engine::getSong()->isModified(), modified);
		selector.insert("program", 512); const auto selected = call("plugin.program.select", selector); QVERIFY(selected.value("ok").toBool());
		QCOMPARE(selected.value("result").toObject().value("current").toInt(), 512); QCOMPARE(second->value.id(), id); QCOMPARE(first->edits, 0); QCOMPARE(second->edits, 1);
		AudioBuffer buffer(Engine::audioEngine()->framesPerPeriod()); buffer.allocateInterleavedBuffer();
		for (int side = 0; side < 2; ++side) { std::fill_n(buffer.group(0).buffer(side), buffer.frames(), side ? -.5f : .5f); buffer.assumeNonSilent(side); }
		toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer()); QVERIFY(chain->processAudioBuffer(buffer));
		for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[0], .0625f); QCOMPARE(frame[1], -.0625f); }
	}

	void invalidArgumentsFailBeforeNativeEntry_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("args");
		QTest::newRow("negative-offset") << QString("plugin.program.list") << QJsonObject{{"offset", -1}};
		QTest::newRow("excess-limit") << QString("plugin.program.list") << QJsonObject{{"limit", 1025}};
		QTest::newRow("mixed-owner") << QString("plugin.program.list") << QJsonObject{{"track", 0}};
		QTest::newRow("fraction-program") << QString("plugin.program.select") << QJsonObject{{"program", .5}};
		QTest::newRow("negative-program") << QString("plugin.program.select") << QJsonObject{{"program", -1}};
		QTest::newRow("missing-program") << QString("plugin.program.select") << QJsonObject{};
	}
	void invalidArgumentsFailBeforeNativeEntry()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, args); auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain;
		auto effect = new ProgramEffect(&chain); QVERIFY(chain.appendEffect(effect)); args.insert("channel", 0); args.insert("effect", 0);
		QVERIFY(!call(operation, args).value("ok").toBool()); QCOMPARE(effect->reads, 0); QCOMPARE(effect->edits, 0);
	}

	void nativeFailuresAndIncorrectReadbackAreNotSuccess()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ProgramEffect(&chain); QVERIFY(chain.appendEffect(effect));
		const QJsonObject selector{{"channel", 0}, {"effect", 0}}; auto select = selector; select.insert("program", 512);
		effect->refuse = true; QVERIFY(!call("plugin.program.select", select).value("ok").toBool()); QCOMPARE(effect->edits, 0);
		effect->refuse = false; effect->lie = true; QVERIFY(!call("plugin.program.select", select).value("ok").toBool());
		effect->malformed = true; QVERIFY(!call("plugin.program.list", selector).value("ok").toBool());
		QDomDocument document; auto node = document.createElement("effect"); node.setAttribute("name", "missing-program-plugin"); QVERIFY(chain.appendEffect(new DummyEffect(&chain, node)));
		QVERIFY(!call("plugin.program.list", {{"channel", 0}, {"effect", 1}}).value("ok").toBool());
	}
};

QTEST_GUILESS_MAIN(StudioPluginProgramTest)
#include "StudioPluginProgramTest.moc"
