/* Native plugin preset dispatch. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
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
#include "VstPresetAccess.h"

using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"preset-fixture", "Preset fixture", "Native dispatch fixture", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
class PresetEffect final : public Effect
{
	DummyEffectControls m_controls{this};
	std::string m_name = "Fixture";
public:
	explicit PresetEffect(EffectChain* chain) : Effect(&fixtureDescriptor, chain, nullptr) {}
	int reads = 0, writes = 0, saves = 0;
	bool refuse = false;
	FloatModel value{.25f, 0.f, 1.f, .01f, this, "Native preset gain"};
	EffectControls* controls() override { return &m_controls; }
	bool supportsNativePresets() const override { return true; }
	VstPresetAccess access()
	{
		VstPresetAccess target;
		target.pluginId = 0xfedcba98; target.pluginVersion = 9; target.parameterCount = 1; target.programCount = 1;
		target.current = [] { return 0; }; target.select = [](int) {};
		target.name = [&] { return m_name; }; target.setName = [&](const std::string& name) { m_name = name; };
		target.parameter = [&](int) { return value.value(); };
		target.setParameter = [&](int, float amount) { ++writes; value.setValue(amount); };
		return target;
	}
	static NativePresetInfo info(const VstPreset& preset, const QByteArray& bytes)
	{
		NativePresetInfo result;
		result.format = preset.bank() ? "fxb" : "fxp"; result.encoding = "parameters";
		result.name = QString::fromStdString(preset.name); result.pluginId = "fedcba98";
		result.pluginVersion = 9; result.formatVersion = preset.version;
		result.programCount = 1; result.parameterCount = preset.bank() ? -1 : 1;
		result.currentProgram = preset.currentProgram.value_or(-1); result.bytes = bytes.size();
		result.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); return result;
	}
	NativePresetInfo read(const QString& path, bool apply)
	{
		++reads; if (refuse) { throw std::runtime_error("Native fixture refused preset"); }
		QFile file(path); if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error("Cannot read fixture preset"); }
		const auto bytes = file.readAll();
		const auto preset = decodeVstPreset(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.constData()), bytes.size()));
		validateVstPresetTarget(preset, access()); if (apply) { applyVstPreset(preset, access()); }
		return info(preset, bytes);
	}
	NativePresetInfo inspectNativePreset(const QString& path) override { return read(path, false); }
	NativePresetInfo loadNativePreset(const QString& path) override { return read(path, true); }
	NativePresetInfo saveNativePreset(const QString& path, bool bank) override
	{
		++saves; if (refuse) { throw std::runtime_error("Native fixture refused export"); }
		const auto preset = captureVstPreset(access(), bank); const auto data = encodeVstPreset(preset);
		const QByteArray bytes(reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()));
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
		{ throw std::runtime_error("Cannot publish fixture preset"); }
		return info(preset, bytes);
	}
protected:
	ProcessStatus processImpl(SampleFrame* frames, f_cnt_t count) override
	{
		for (f_cnt_t index = 0; index < count; ++index) { frames[index][0] *= value.value(); frames[index][1] *= value.value(); }
		return ProcessStatus::Continue;
	}
};
QJsonObject call(const QString& operation, const QJsonObject& args) { return test::executeWithAllParameters(operation, args); }
}

class StudioPluginPresetTest : public QObject
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

	void exactOwnerImportExportAndDsp_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("mixer") << QString("mixer");
		QTest::newRow("song-instrument") << QString("instrument"); QTest::newRow("song-sample") << QString("sample");
		QTest::newRow("pattern-instrument") << QString("pattern-instrument"); QTest::newRow("pattern-sample") << QString("pattern-sample");
	}
	void exactOwnerImportExportAndDsp()
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
		auto first = new PresetEffect(chain); auto second = new PresetEffect(chain); QVERIFY(chain->appendEffect(first)); QVERIFY(chain->appendEffect(second));
		const auto inventory = call("effect.inspect", selector).value("result").toObject().value("effects").toArray();
		QVERIFY(inventory.at(1).toObject().value("nativePresetsSupported").toBool()); selector.insert("effect", 1);
		QTemporaryDir files; QVERIFY(files.isValid()); const auto inputPath = files.filePath("different-extension.data");
		VstPreset preset; preset.pluginId = 0xfedcba98; preset.pluginVersion = 9; preset.name = "New sound"; preset.parameters = {.5f};
		const auto inputBytes = encodeVstPreset(preset); QFile input(inputPath); QVERIFY(input.open(QIODevice::WriteOnly));
		QCOMPARE(input.write(reinterpret_cast<const char*>(inputBytes.data()), inputBytes.size()), static_cast<qint64>(inputBytes.size())); input.close();
		selector.insert("path", inputPath); const auto id = second->value.id(); const bool modified = Engine::getSong()->isModified();
		auto inspected = call("plugin.preset.inspect", selector); QVERIFY(inspected.value("ok").toBool());
		QCOMPARE(inspected.value("result").toObject().value("format").toString(), QString("fxp"));
		QCOMPARE(first->reads, 0); QCOMPARE(second->writes, 0); QCOMPARE(second->value.value(), .25f); QCOMPARE(Engine::getSong()->isModified(), modified);
		const auto loaded = call("plugin.preset.load", selector); QVERIFY(loaded.value("ok").toBool());
		QCOMPARE(second->value.value(), .5f); QCOMPARE(second->value.id(), id); QCOMPARE(first->value.value(), .25f);
		QCOMPARE(inspected.value("result").toObject().value("sha256"), loaded.value("result").toObject().value("sha256"));
		const auto outputPath = files.filePath("explicit-format.data"); selector.insert("path", outputPath); selector.insert("format", "fxb");
		const auto saved = call("plugin.preset.save", selector); QVERIFY(saved.value("ok").toBool()); QCOMPARE(first->saves, 0); QCOMPARE(second->saves, 1);
		QCOMPARE(second->value.value(), .5f); QCOMPARE(second->value.id(), id);
		selector.remove("format"); const auto exported = call("plugin.preset.inspect", selector); QVERIFY(exported.value("ok").toBool());
		QCOMPARE(exported.value("result").toObject().value("format").toString(), QString("fxb"));
		QCOMPARE(saved.value("result").toObject().value("sha256"), exported.value("result").toObject().value("sha256"));
		AudioBuffer buffer(Engine::audioEngine()->framesPerPeriod()); buffer.allocateInterleavedBuffer();
		for (int side = 0; side < 2; ++side) { std::fill_n(buffer.group(0).buffer(side), buffer.frames(), side ? -.5f : .5f); buffer.assumeNonSilent(side); }
		toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer()); QVERIFY(chain->processAudioBuffer(buffer));
		for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[0], .0625f); QCOMPARE(frame[1], -.0625f); }
	}

	void invalidRequestsNeverEnterNativeState_data()
	{
		QTest::addColumn<QJsonObject>("args");
		QTest::newRow("missing-format") << QJsonObject{{"path", "output.fxp"}};
		QTest::newRow("wrong-format") << QJsonObject{{"path", "output.fxp"}, {"format", "xpf"}};
		QTest::newRow("wrong-path-type") << QJsonObject{{"path", 3}, {"format", "fxp"}};
		QTest::newRow("empty-path") << QJsonObject{{"path", ""}, {"format", "fxp"}};
		QTest::newRow("mixed-owner") << QJsonObject{{"path", "output.fxp"}, {"format", "fxp"}, {"track", 0}};
		QTest::newRow("nul-path") << QJsonObject{{"path", QString("bad") + QChar(0)}, {"format", "fxp"}};
	}
	void invalidRequestsNeverEnterNativeState()
	{
		QFETCH(QJsonObject, args); auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain;
		auto effect = new PresetEffect(&chain); QVERIFY(chain.appendEffect(effect)); args.insert("channel", 0); args.insert("effect", 0);
		QVERIFY(!call("plugin.preset.save", args).value("ok").toBool()); QCOMPARE(effect->reads, 0); QCOMPARE(effect->writes, 0); QCOMPARE(effect->saves, 0);
	}

	void failedImportsAndUnsupportedPluginsAreReported()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new PresetEffect(&chain); QVERIFY(chain.appendEffect(effect));
		QTemporaryDir files; QVERIFY(files.isValid()); const auto path = files.filePath("broken.fxp");
		QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("CcnK"), qint64{4}); file.close();
		const QJsonObject selector{{"channel", 0}, {"effect", 0}, {"path", path}};
		QVERIFY(!call("plugin.preset.load", selector).value("ok").toBool()); QCOMPARE(effect->writes, 0); QCOMPARE(effect->value.value(), .25f);
		effect->refuse = true; auto save = selector; save.insert("format", "fxp");
		QVERIFY(!call("plugin.preset.save", save).value("ok").toBool()); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("CcnK")); file.close();
		QDomDocument document; const auto node = document.createElement("effect"); QVERIFY(chain.appendEffect(new DummyEffect(&chain, node)));
		QVERIFY(!call("plugin.preset.inspect", {{"channel", 0}, {"effect", 1}, {"path", path}}).value("ok").toBool());
		QVERIFY(!StudioCapabilities::mutatesProject("plugin.preset.inspect"));
		QVERIFY(StudioCapabilities::mutatesProject("plugin.preset.load")); QVERIFY(StudioCapabilities::mutatesProject("plugin.preset.save"));
	}
};

QTEST_GUILESS_MAIN(StudioPluginPresetTest)
#include "StudioPluginPresetTest.moc"
