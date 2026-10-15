/* Defined integer conversion and native controller/DSP recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "ProjectRenderer.h"
#include "RemotePlugin.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
using namespace lmms;
namespace
{
class Input final : public Controller
{
public:
	Input() : Controller(ControllerType::Lfo, nullptr, "Native conversion input fixture") { }
	std::atomic<float> next{.25f};
	float currentValue(int) override { return next; }
	void publish() { notifyValueChange(); }
};
float invalid(int kind)
{
	if (kind == 0) { return std::numeric_limits<float>::quiet_NaN(); }
	if (kind == 1) { return std::numeric_limits<float>::infinity(); }
	return 0x1p40f;
}
void convert(int destination, float value)
{
	switch (destination)
	{
	case 0: (void)AutomatableModel::castValue<std::int8_t>(value); break;
	case 1: (void)AutomatableModel::castValue<std::uint8_t>(value); break;
	case 2: (void)AutomatableModel::castValue<std::int16_t>(value); break;
	case 3: (void)AutomatableModel::castValue<std::uint16_t>(value); break;
	case 4: (void)AutomatableModel::castValue<std::int32_t>(value); break;
	case 5: (void)AutomatableModel::castValue<std::uint32_t>(value); break;
	case 6: (void)AutomatableModel::castValue<std::int64_t>(value); break;
	case 7: (void)AutomatableModel::castValue<std::uint64_t>(value); break;
	case 8: (void)AutomatableModel::castValue<bool>(value); break;
	case 9: (void)AutomatableModel::castValue<true>(value); break;
	}
}
void arrange(Input& input)
{
	auto song = Engine::getSong(); song->setTempo(240); song->setMasterVolume(100); std::vector<SampleFrame> pcm(44100);
	for (std::size_t index = 0; index < pcm.size(); ++index)
	{
		pcm[index][0] = .4f * std::sin(6.283185307179586 * 440 * index / 44100);
		pcm[index][1] = .2f * std::sin(6.283185307179586 * 660 * index / 44100);
	}
	auto track = new SampleTrack(song); auto clip = static_cast<SampleClip*>(track->createClip(TimePos()));
	clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100)); clip->changeLength(TimePos(192));
	auto lfo = new LfoController(song); song->addController(lfo); lfo->waveModel().setControllerConnection(new ControllerConnection(&input));
	Engine::mixer()->mixerChannel(0)->m_volumeModel.setControllerConnection(new ControllerConnection(lfo));
}
bool silent(std::span<const SampleFrame> output)
{ return std::all_of(output.begin(), output.end(), [](const auto& frame) { return frame[0] == 0 && frame[1] == 0; }); }
QJsonObject render(const QString& path)
{ return StudioCapabilities::execute("audio.render", {{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}}); }
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
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
class NativeParameterConversionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); QVERIFY(Engine::audioEngine()->resetDspFailure()); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); QVERIFY(Engine::audioEngine()->resetDspFailure()); }

	void signedValuesKeepDefinedTruncationAndExactFloatBoundaries_data()
	{
		QTest::addColumn<int>("width"); QTest::addColumn<float>("value"); QTest::addColumn<qint64>("expected");
		QTest::newRow("zero") << 8 << 0.f << qint64(0); QTest::newRow("negative-zero") << 8 << -0.f << qint64(0);
		QTest::newRow("lower-fraction") << 8 << -128.75f << qint64(-128); QTest::newRow("upper-fraction") << 8 << 127.75f << qint64(127);
		QTest::newRow("negative-fraction") << 8 << -.75f << qint64(0);
		QTest::newRow("short-min-fraction") << 16 << -32768.75f << qint64(-32768); QTest::newRow("short-max-fraction") << 16 << 32767.75f << qint64(32767);
		QTest::newRow("int32-last-float") << 32 << 0x1.fffffep30f << qint64(2147483520ll);
		QTest::newRow("int32-min") << 32 << -0x1p31f << qint64(-2147483648ll);
		QTest::newRow("int64-last-float") << 64 << 0x1.fffffep62f << qint64(9223371487098961920ll);
	}
	void signedValuesKeepDefinedTruncationAndExactFloatBoundaries()
	{
		QFETCH(int, width); QFETCH(float, value); QFETCH(qint64, expected); qint64 actual = 0;
		if (width == 8) { actual = AutomatableModel::castValue<std::int8_t>(value); }
		if (width == 16) { actual = AutomatableModel::castValue<std::int16_t>(value); }
		if (width == 32) { actual = AutomatableModel::castValue<std::int32_t>(value); }
		if (width == 64) { actual = AutomatableModel::castValue<std::int64_t>(value); }
		QCOMPARE(actual, expected); QCOMPARE(AutomatableModel::castValue<std::int64_t>(-0x1p63f), std::numeric_limits<std::int64_t>::lowest());
	}

	void unsignedValuesKeepDefinedTruncation_data()
	{
		QTest::addColumn<int>("width"); QTest::addColumn<float>("value"); QTest::addColumn<quint64>("expected");
		QTest::newRow("zero") << 8 << 0.f << quint64(0); QTest::newRow("negative-fraction") << 8 << -.75f << quint64(0);
		QTest::newRow("byte-max-fraction") << 8 << 255.75f << quint64(255); QTest::newRow("short-max-fraction") << 16 << 65535.5f << quint64(65535);
		QTest::newRow("uint32-last-float") << 32 << 0x1.fffffep31f << quint64(4294967040ull);
		QTest::newRow("uint64-last-float") << 64 << 0x1.fffffep63f << quint64(18446742974197923840ull);
		QTest::newRow("uint32-negative-fraction") << 32 << -.5f << quint64(0); QTest::newRow("uint64-negative-fraction") << 64 << -.5f << quint64(0);
	}
	void unsignedValuesKeepDefinedTruncation()
	{
		QFETCH(int, width); QFETCH(float, value); QFETCH(quint64, expected); quint64 actual = 0;
		if (width == 8) { actual = AutomatableModel::castValue<std::uint8_t>(value); }
		if (width == 16) { actual = AutomatableModel::castValue<std::uint16_t>(value); }
		if (width == 32) { actual = AutomatableModel::castValue<std::uint32_t>(value); }
		if (width == 64) { actual = AutomatableModel::castValue<std::uint64_t>(value); }
		QCOMPARE(actual, expected);
	}

	void unrepresentableIntegersRefuseBeforeTheCast_data()
	{
		QTest::addColumn<int>("destination"); QTest::addColumn<float>("value");
		QTest::newRow("int8-low") << 0 << -129.f; QTest::newRow("int8-high") << 0 << 128.f;
		QTest::newRow("uint8-low") << 1 << -1.f; QTest::newRow("uint8-high") << 1 << 256.f;
		QTest::newRow("int16-low") << 2 << -32769.f; QTest::newRow("int16-high") << 2 << 32768.f;
		QTest::newRow("uint16-low") << 3 << -1.f; QTest::newRow("uint16-high") << 3 << 65536.f;
		QTest::newRow("int32-low") << 4 << -0x1.000002p31f; QTest::newRow("int32-high") << 4 << 0x1p31f;
		QTest::newRow("uint32-low") << 5 << -1.f; QTest::newRow("uint32-high") << 5 << 0x1p32f;
		QTest::newRow("int64-low") << 6 << -0x1.000002p63f; QTest::newRow("int64-high") << 6 << 0x1p63f;
		QTest::newRow("uint64-low") << 7 << -1.f; QTest::newRow("uint64-high") << 7 << 0x1p64f;
	}
	void unrepresentableIntegersRefuseBeforeTheCast()
	{ QFETCH(int, destination); QFETCH(float, value); QVERIFY_EXCEPTION_THROWN(convert(destination, value), std::out_of_range); }

	void nonfiniteIntegralAndBooleanValuesRefuse_data()
	{
		QTest::addColumn<int>("destination"); QTest::addColumn<int>("bad");
		// Six destination forms, three nonfinite inputs: eighteen executions.
		for (int destination : {4, 5, 6, 7, 8, 9}) for (int bad = 0; bad < 3; ++bad)
		{ QTest::newRow(qPrintable(QString::number(destination) + '-' + QString::number(bad))) << destination << bad; }
	}
	void nonfiniteIntegralAndBooleanValuesRefuse()
	{
		QFETCH(int, destination); QFETCH(int, bad); const float value = bad == 0 ? std::numeric_limits<float>::quiet_NaN()
			: bad == 1 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
		QVERIFY_EXCEPTION_THROWN(convert(destination, value), std::domain_error);
	}

	void finiteBooleanFormsKeepTheirExistingSemantics_data()
	{
		QTest::addColumn<float>("value"); QTest::addColumn<bool>("normal"); QTest::addColumn<bool>("rounded");
		QTest::newRow("zero") << 0.f << false << false; QTest::newRow("negative-zero") << -0.f << false << false;
		QTest::newRow("quarter") << .25f << true << false; QTest::newRow("half") << .5f << true << true;
		QTest::newRow("negative-quarter") << -.25f << true << false; QTest::newRow("negative-half") << -.5f << true << true;
		QTest::newRow("one") << 1.f << true << true; QTest::newRow("negative-one") << -1.f << true << true;
	}
	void finiteBooleanFormsKeepTheirExistingSemantics()
	{
		QFETCH(float, value); QFETCH(bool, normal); QFETCH(bool, rounded);
		QCOMPARE(AutomatableModel::castValue<bool>(value), normal); QCOMPARE(AutomatableModel::castValue<true>(value), rounded); QCOMPARE(AutomatableModel::castValue<false>(value), rounded);
	}

	void floatingReadsKeepTheirExistingPropagation_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("nan") << 0; QTest::newRow("infinity") << 1; QTest::newRow("negative-infinity") << 2; QTest::newRow("negative-zero") << 3; QTest::newRow("fraction") << 4; }
	void floatingReadsKeepTheirExistingPropagation()
	{
		QFETCH(int, kind); const float value = kind == 0 ? std::numeric_limits<float>::quiet_NaN() : kind == 1 ? std::numeric_limits<float>::infinity()
			: kind == 2 ? -std::numeric_limits<float>::infinity() : kind == 3 ? -0.f : 17.25f;
		const auto single = AutomatableModel::castValue<float>(value); const auto wide = AutomatableModel::castValue<double>(value);
		if (kind == 0) { QVERIFY(std::isnan(single)); QVERIFY(std::isnan(wide)); }
		else { QCOMPARE(single, value); QCOMPARE(wide, double(value)); QCOMPARE(std::signbit(single), std::signbit(value)); }
	}

	void floatRoundedIntegerMaximumIsDiagnosedRatherThanWrapped()
	{
		IntModel model(std::numeric_limits<int>::max(), 0, std::numeric_limits<int>::max()); QCOMPARE(model.storedValue(), 0x1p31f);
		QVERIFY_EXCEPTION_THROWN(model.value(), std::out_of_range); QVERIFY_EXCEPTION_THROWN(model.maxValue(), std::out_of_range);
		model.setValue(0x1.fffffep30f); QCOMPARE(model.value(), 2147483520); QVERIFY_EXCEPTION_THROWN(model.maxValue(), std::out_of_range);
	}

	void boundAndLinkedReadsPreserveStoredValuesAndPermitRepair_data()
	{
		QTest::addColumn<int>("path"); QTest::newRow("integer-nan") << 0; QTest::newRow("boolean-nan") << 1; QTest::newRow("linked-nan") << 2;
		QTest::newRow("integer-infinity") << 3; QTest::newRow("boolean-infinity") << 4;
	}
	void boundAndLinkedReadsPreserveStoredValuesAndPermitRepair()
	{
		QFETCH(int, path); Input input; IntModel integer(42, 0, 100); BoolModel flag(false); FloatModel peer(42, 0, 100, 0);
		if (path == 2) { QVERIFY(integer.linkToModel(&peer)); }
		const bool boolean = path == 1 || path == 4; AutomatableModel* target = boolean ? static_cast<AutomatableModel*>(&flag) : path == 2 ? static_cast<AutomatableModel*>(&peer) : &integer;
		target->setControllerConnection(new ControllerConnection(&input)); const auto stored = integer.storedValue(), booleanStored = flag.storedValue(), peerStored = peer.storedValue();
		input.next = invalid(path >= 3 ? 1 : 0); const auto readValue = [&] { return boolean ? int(flag.value()) : integer.value(); };
		QVERIFY_EXCEPTION_THROWN(readValue(), std::domain_error); QCOMPARE(integer.storedValue(), stored); QCOMPARE(flag.storedValue(), booleanStored); QCOMPARE(peer.storedValue(), peerStored);
		input.next = .25f; QCOMPARE(readValue(), boolean ? 1 : 25);
	}

	void songTempoAndMeterRefuseInvalidControllerRepresentations_data()
	{
		QTest::addColumn<bool>("meter"); QTest::addColumn<int>("bad");
		QTest::newRow("tempo-nan") << false << 0; QTest::newRow("tempo-infinity") << false << 1; QTest::newRow("tempo-range") << false << 2;
		QTest::newRow("meter-nan") << true << 0; QTest::newRow("meter-infinity") << true << 1; QTest::newRow("meter-range") << true << 2;
	}
	void songTempoAndMeterRefuseInvalidControllerRepresentations()
	{
		QFETCH(bool, meter); QFETCH(int, bad); Input input; auto song = Engine::getSong(); auto engine = Engine::audioEngine();
		auto target = meter ? &song->getTimeSigModel().numeratorModel() : &song->tempoModel(); target->setControllerConnection(new ControllerConnection(&input));
		const auto repair = qScopeGuard([&] { input.next = .25f; bool changed = false; QString error; target->disconnectController(changed, error); });
		QSignalSpy later(target, &Model::dataChanged); const auto ticks = TimePos::ticksPerBar(); const auto frames = Engine::framesPerTick(); input.next = invalid(bad);
		if (bad == 2) { QVERIFY_EXCEPTION_THROWN(target->setValue(meter ? 5 : 123), std::out_of_range); }
		else { QVERIFY_EXCEPTION_THROWN(target->setValue(meter ? 5 : 123), std::domain_error); }
		QCOMPARE(later.size(), 1); QCOMPARE(TimePos::ticksPerBar(), ticks); QCOMPARE(Engine::framesPerTick(), frames);
		std::atomic_bool acquired{false}; std::thread observer([&] { const auto changes = engine->requestChangesGuard(); acquired = true; }); observer.join(); QVERIFY(acquired.load());
		input.next = .25f; bool changed = false; QString error; QVERIFY(target->disconnectController(changed, error));
		if (meter) { song->getTimeSigModel().setNumerator(4); } else { song->setTempo(97); QCOMPARE(song->getTempo(), 97); }
	}

	void aFiniteTempoCannotWrapIntoTheSixteenBitSongRange()
	{
		Input input; auto song = Engine::getSong(); auto& model = song->tempoModel(); model.setControllerConnection(new ControllerConnection(&input));
		const auto repair = qScopeGuard([&] { input.next = .25f; bool changed = false; QString error; model.disconnectController(changed, error); });
		const auto frames = Engine::framesPerTick(); input.next = (65536.f + 91.f - model.minValue()) / model.range();
		QVERIFY(model.value() > MaxTempo); QVERIFY(model.value() < std::numeric_limits<int>::max());
		QVERIFY_EXCEPTION_THROWN(song->getTempo(), std::out_of_range); const Song* constant = song;
		QVERIFY_EXCEPTION_THROWN(constant->getTempo(), std::out_of_range);
		QSignalSpy later(&model, &Model::dataChanged); QVERIFY_EXCEPTION_THROWN(song->setTempo(123), std::out_of_range);
		QCOMPARE(later.size(), 1); QCOMPARE(Engine::framesPerTick(), frames);
		input.next = .25f; bool changed = false; QString error; QVERIFY(model.disconnectController(changed, error)); song->setTempo(97);
		QCOMPARE(song->getTempo(), bpm_t(97)); QCOMPARE(constant->getTempo(), 97);
	}

	void actualLfoDspFaultsLatchSilenceUntilExplicitRepair_data()
	{ QTest::addColumn<int>("bad"); QTest::newRow("nan") << 0; QTest::newRow("infinity") << 1; QTest::newRow("range") << 2; }
	void actualLfoDspFaultsLatchSilenceUntilExplicitRepair()
	{
		QFETCH(int, bad); Input input; arrange(input); auto engine = Engine::audioEngine(); auto song = Engine::getSong();
		const auto repair = qScopeGuard([&] { input.next = .25f; song->stop(); engine->resetDspFailure(); }); const auto period = Controller::runningPeriods();
		input.next = invalid(bad); song->playSong(); QVERIFY(silent(engine->renderNextPeriod())); QCOMPARE(Controller::runningPeriods(), period);
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::MixException); const auto failures = engine->dspStatus().failures;
		QVERIFY(silent(engine->renderNextPeriod())); QCOMPARE(engine->dspStatus().failures, failures);
		input.next = .25f; input.publish(); song->stop(); QVERIFY(engine->resetDspFailure()); song->playSong(); engine->renderNextPeriod(); const auto recovered = engine->renderNextPeriod();
		QVERIFY(!silent(recovered)); for (const auto& frame : recovered) { QVERIFY(!frame.containsNaN()); QVERIFY(!frame.containsInf()); }
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None); QCOMPARE(engine->dspStatus().failures, failures);
	}

	void failedNativeExportsPreserveOutputAndDecodeAfterRepair_data()
	{
		QTest::addColumn<bool>("studio"); QTest::addColumn<int>("bad");
		QTest::newRow("renderer-nan") << false << 0; QTest::newRow("renderer-infinity") << false << 1; QTest::newRow("renderer-range") << false << 2;
		QTest::newRow("studio-nan") << true << 0; QTest::newRow("studio-infinity") << true << 1; QTest::newRow("studio-range") << true << 2;
	}
	void failedNativeExportsPreserveOutputAndDecodeAfterRepair()
	{
		QFETCH(bool, studio); QFETCH(int, bad); Input input; arrange(input); auto engine = Engine::audioEngine(); QTemporaryDir output; const auto path = output.filePath("published.wav");
		{ QFile prior(path); QVERIFY(prior.open(QIODevice::WriteOnly)); QCOMPARE(prior.write("prior export"), qint64(12)); }
		const auto repair = qScopeGuard([&] { input.next = .25f; engine->restoreAudioDevice(); Engine::getSong()->stop(); engine->resetDspFailure(); }); input.next = invalid(bad);
		if (studio) { const auto response = render(path); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); }
		else
		{
			QVERIFY(engine->storeAudioDevice());
			ProjectRenderer renderer(OutputSettings(44100, 160, OutputSettings::BitDepth::Depth24Bit), ProjectRenderer::ExportFileFormat::Wave, output.filePath("failed.wav"));
			QVERIFY(renderer.isReady()); QVERIFY(renderer.startProcessing()); QVERIFY(renderer.wait(30000)); QVERIFY(renderer.hasDspFailure()); QVERIFY(!renderer.finalizeOutput());
		}
		QCOMPARE(read(path), QByteArray("prior export")); QVERIFY(!RemotePlugin::hasActiveRenderContext()); QVERIFY(engine->restoreAudioDevice());
		input.next = .25f; input.publish(); Engine::getSong()->stop(); QVERIFY(engine->resetDspFailure());
		const auto response = render(path); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(decoded(path));
		QVERIFY(!engine->hasStoredAudioDevice()); QVERIFY(!Engine::getSong()->isExporting());
	}
};
QTEST_GUILESS_MAIN(NativeParameterConversionTest)
#include "NativeParameterConversionTest.moc"
