/* Native retained FluidSynth rate conversion. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <fluidsynth.h>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include "AudioDummy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "PluginFactory.h"
#include "Sf2Player.h"
#include "Song.h"
#include "StudioCapabilities.h"
using namespace lmms;
namespace lmms
{
class Sf2RateFallbackTestAccess
{
public:
	struct State { fluid_synth_t* synth; std::uint64_t generation; sample_rate_t rate; double ratio; std::size_t buffered; };
	struct Voice { fluid_voice_t* pointer; unsigned int id; bool playing; };
	static State state(Sf2Instrument* object) { QMutexLocker lock(&object->m_synthMutex); return {object->m_synth, object->m_synthGeneration, object->m_internalSampleRate, object->m_resampler.ratio(), object->m_bufferView.size()}; }
	static std::shared_ptr<Sf2BankSnapshot> snapshot(Sf2Instrument* object) { return object->m_bankSnapshot; }
	static void snapshot(Sf2Instrument* object, const std::shared_ptr<Sf2BankSnapshot>& snapshot) { object->m_bankSnapshot = snapshot; }
	static Voice voice(Sf2Instrument* object) { QMutexLocker lock(&object->m_synthMutex); std::array<fluid_voice_t*, 512> voices{}; fluid_synth_get_voicelist(object->m_synth, voices.data(), static_cast<int>(voices.size()), -1); const auto first = voices.front(); return {first, first ? fluid_voice_get_id(first) : 0, first && fluid_voice_is_playing(first)}; }
};
}
namespace
{
// Production dummy device with an explicit test clock. It is never started;
// samples below come from the actual NotePlayHandle/SF2/libsamplerate pipeline.
class RateClock final : public AudioDummy
{
public:
	RateClock(bool& good, sample_rate_t rate) : AudioDummy(good, Engine::audioEngine()) { setSampleRate(rate); }
};
void clock(sample_rate_t rate)
{
	bool good = false; auto device = std::make_unique<RateClock>(good, rate); if (!good) { throw std::runtime_error("Native dummy clock could not initialize"); }
	try { Engine::audioEngine()->setAudioDevice(device.get(), false); }
	catch (...) { if (Engine::audioEngine()->audioDev() == device.get()) { device.release(); } throw; }
	device.release();
}
class NativeTrack final : public InstrumentTrack
{
public:
	NativeTrack() : InstrumentTrack(Engine::getSong()) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new Sf2Instrument(this); }
};
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
bool alterMetadata(const QString& path, const QByteArray& original)
{
	// Keep PCM and RIFF structure intact while making accepted identity invalid.
	auto changed = original; const auto at = changed.indexOf("LMMS native calibration"); if (at < 0) { return false; } changed[at] = 'X'; return write(path, changed);
}
std::array<double, 2> measurePlaying(Sf2Instrument* object)
{
	const auto rate = Engine::audioEngine()->outputSampleRate(); const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> pcm(frames); std::vector<int> crossings; double square = 0; int accepted = 0; float previous = 0;
	for (int offset = 0; offset < int(rate * 3 / 4); offset += frames)
	{
		object->play(pcm.data());
		for (int frame = 0; frame < int(frames); ++frame)
		{
			if (!std::isfinite(pcm[frame][0]) || !std::isfinite(pcm[frame][1])) { throw std::runtime_error("Nonfinite retained SoundFont PCM"); }
			if (offset + frame < int(rate / 10)) { continue; } const auto value = pcm[frame][0]; if (accepted && previous <= 0 && value > 0) { crossings.push_back(accepted); } previous = value; square += double(value) * value; ++accepted;
		}
	}
	const double pitch = crossings.size() > 1 ? double(rate) * (crossings.size() - 1) / (crossings.back() - crossings.front()) : 0.; return {accepted ? std::sqrt(square / accepted) : 0., pitch};
}
std::array<double, 2> measure(Sf2Instrument* object)
{
	const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> pcm(frames); NotePlayHandle note(object->instrumentTrack(), 0, Engine::audioEngine()->outputSampleRate() * 2, Note(TimePos(384), TimePos(), 69)); const auto release = qScopeGuard([&] { object->deleteNotePluginData(&note); }); object->playNote(&note, pcm.data()); return measurePlaying(object);
}
}
class Sf2RateFallbackTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QString bank() const { return QString::fromUtf8(LMMS_TEST_FIXTURE_DIR) + "/native-calibration.sf2"; }
	Sf2Instrument* seed() { auto track = new NativeTrack; auto object = dynamic_cast<Sf2Instrument*>(track->loadInstrument("sf2player")); QString error; if (!object || !object->setFileResource("soundfont", bank(), error)) { throw std::runtime_error("Native SoundFont fallback fixture could not load"); } return object; }
	void retained(Sf2Instrument* object, const Sf2RateFallbackTestAccess::State& before, sample_rate_t rate)
	{
		const auto after = Sf2RateFallbackTestAccess::state(object); QCOMPARE(after.synth, before.synth); QCOMPARE(after.generation, before.generation); QCOMPARE(after.rate, before.rate); QVERIFY(std::abs(after.ratio - double(rate) / before.rate) < 1e-12); QCOMPARE(after.buffered, std::size_t(0)); QCOMPARE(object->getBank(), 0); QCOMPARE(object->getPatch(), 0);
	}
	void audible(Sf2Instrument* object) { const auto pcm = measure(object); QVERIFY(pcm[0] > .0001); QVERIFY(std::abs(pcm[1] - 440) < 6); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setSF2File(""); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(QFile::exists(bank())); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); clock(44100); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); Engine::getSong()->setProjectFileName(""); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); clock(44100); }
	void verificationFailureKeepsTheRetainedSynthAtTheNewOutputRate_data()
	{ QTest::addColumn<int>("rate"); QTest::newRow("32000") << 32000; QTest::newRow("48000") << 48000; QTest::newRow("88200") << 88200; QTest::newRow("96000") << 96000; QTest::newRow("192000") << 192000; }
	void verificationFailureKeepsTheRetainedSynthAtTheNewOutputRate()
	{
		QFETCH(int, rate); auto object = seed(); const auto before = Sf2RateFallbackTestAccess::state(object); const auto snapshot = Sf2RateFallbackTestAccess::snapshot(object); const auto original = read(snapshot->path()); QVERIFY(!original.isEmpty()); const auto repair = qScopeGuard([&] { write(snapshot->path(), original); }); QVERIFY(alterMetadata(snapshot->path(), original)); clock(rate); QVERIFY(Engine::getSong()->hasFatalErrors()); retained(object, before, rate); audible(object);
	}
	void absentSnapshotStillPreparesTheRetainedSynthRate()
	{
		auto object = seed(); const auto before = Sf2RateFallbackTestAccess::state(object); const auto snapshot = Sf2RateFallbackTestAccess::snapshot(object); QVERIFY(snapshot); const auto restore = qScopeGuard([&] { Sf2RateFallbackTestAccess::snapshot(object, snapshot); }); Sf2RateFallbackTestAccess::snapshot(object, {}); clock(48000); QVERIFY(Engine::getSong()->hasFatalErrors()); retained(object, before, 48000); audible(object);
	}
	void cancellationAndCallbackFailuresKeepThePublishedClockUsable_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("cancel-before-verification") << 0; QTest::newRow("standard-callback-exception") << 1; QTest::newRow("nonstandard-callback-exception") << 2; }
	void cancellationAndCallbackFailuresKeepThePublishedClockUsable()
	{
		QFETCH(int, failure); auto object = seed(); const auto before = Sf2RateFallbackTestAccess::state(object); QString error; Sf2Instrument::ProgramPreviewToken token; QVERIFY(object->previewProgram(1024, 7, error, token)); int polls = 0;
		int delivered = 0; const auto connection = connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this, [&] { ++delivered; }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		{ FileResourceIO::Scope work([&] { ++polls; if (failure == 1) { throw std::runtime_error("Rate fallback file callback"); } if (failure == 2) { throw 71; } return true; }); if (failure == 2) { QVERIFY_EXCEPTION_THROWN(clock(48000), int); } else { QVERIFY_EXCEPTION_THROWN(clock(48000), std::runtime_error); } }
		QCOMPARE(delivered, 1);
		QVERIFY(polls > 0); QCOMPARE(Engine::audioEngine()->outputSampleRate(), sample_rate_t(48000)); retained(object, before, 48000); audible(object);
	}
	void aSecondFailedRateChangeClearsBufferedInputWithoutRetiringTheVoice()
	{
		auto object = seed(); const auto snapshot = Sf2RateFallbackTestAccess::snapshot(object); const auto original = read(snapshot->path()); const auto repair = qScopeGuard([&] { write(snapshot->path(), original); }); QVERIFY(alterMetadata(snapshot->path(), original)); clock(32000); const auto before = Sf2RateFallbackTestAccess::state(object); std::vector<SampleFrame> pcm(Engine::audioEngine()->framesPerPeriod()); NotePlayHandle note(object->instrumentTrack(), 0, 192000, Note(TimePos(384), TimePos(), 69)); const auto release = qScopeGuard([&] { object->deleteNotePluginData(&note); }); object->playNote(&note, pcm.data()); object->play(pcm.data()); QVERIFY(Sf2RateFallbackTestAccess::state(object).buffered > 0); const auto voice = Sf2RateFallbackTestAccess::voice(object); QVERIFY(voice.pointer && voice.playing); clock(48000); retained(object, before, 48000); const auto after = Sf2RateFallbackTestAccess::voice(object); QCOMPARE(after.pointer, voice.pointer); QCOMPARE(after.id, voice.id); QVERIFY(after.playing); const auto continued = measurePlaying(object); QVERIFY(continued[0] > .0001); QVERIFY(std::abs(continued[1] - 440) < 6);
	}
	void explicitRetryRebuildsAtTheCurrentRateAndSafelyRetiresOldNoteData()
	{
		auto object = seed(); const auto snapshot = Sf2RateFallbackTestAccess::snapshot(object); const auto original = read(snapshot->path()); const auto repair = qScopeGuard([&] { write(snapshot->path(), original); }); QVERIFY(alterMetadata(snapshot->path(), original)); clock(48000); std::vector<SampleFrame> pcm(Engine::audioEngine()->framesPerPeriod()); NotePlayHandle note(object->instrumentTrack(), 0, 192000, Note(TimePos(384), TimePos(), 69)); const auto release = qScopeGuard([&] { object->deleteNotePluginData(&note); }); object->playNote(&note, pcm.data()); object->play(pcm.data()); const auto before = Sf2RateFallbackTestAccess::state(object); QVERIFY(write(snapshot->path(), original)); Engine::getSong()->clearErrors(); object->reloadSynth(); const auto after = Sf2RateFallbackTestAccess::state(object); QVERIFY(after.synth != before.synth); QCOMPARE(after.generation, before.generation + 1); QCOMPARE(after.rate, sample_rate_t(48000)); QCOMPARE(after.ratio, 1.); QCOMPARE(after.buffered, std::size_t(0)); QVERIFY(!Engine::getSong()->hasFatalErrors()); object->deleteNotePluginData(&note); audible(object);
	}
	void requiredModuleRetainsPitchThroughRepeatedRejectedRateChanges()
	{
		QVERIFY2(!getPluginFactory()->pluginInfo("sf2player").isNull(), "The built native SoundFont module must be discoverable"); QVERIFY(StudioCapabilities::execute("track.add", {{"type", "instrument"}, {"instrument", "sf2player"}}).value("ok").toBool()); auto object = static_cast<Sf2Instrument*>(static_cast<InstrumentTrack*>(Engine::getSong()->tracks()[0])->instrument()); QString error; QVERIFY(object->setFileResource("soundfont", bank(), error)); const auto before = Sf2RateFallbackTestAccess::state(object); const auto snapshot = Sf2RateFallbackTestAccess::snapshot(object); const auto original = read(snapshot->path()); const auto repair = qScopeGuard([&] { write(snapshot->path(), original); }); QVERIFY(alterMetadata(snapshot->path(), original)); for (const auto rate : {48000, 32000, 96000, 44100}) { clock(rate); retained(object, before, rate); audible(object); }
	}
};
QTEST_GUILESS_MAIN(Sf2RateFallbackTest)
#include "Sf2RateFallbackTest.moc"
