/* Checked native meter arithmetic, notification and persistent recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <atomic>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sndfile.h>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NativeValueNotification.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "SharedMemory.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "TimePos.h"
#include "VstSyncData.h"
using namespace lmms;
namespace
{
class Input final : public Controller
{
public:
	Input() : Controller(ControllerType::Lfo, nullptr, "Native meter safety fixture") { }
	float next = .125f; int reads = 0; bool changeAfterFirst = false;
	float currentValue(int) override { const auto value = next; if (++reads == 1 && changeAfterFirst) { next = -1.f / 31; } return value; }
};
[[noreturn]] void raise(bool nonstandard)
{ if (nonstandard) { throw 359; } throw std::runtime_error("Native meter notification fixture"); }
template<class Callback> void receive(Callback&& callback)
{
	try { callback(); }
	catch (...) { QVERIFY(NativeValueNotification::report(std::current_exception())); }
}
QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
{ return {{"id", id}, {"op", operation}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
void arrange()
{
	auto song = Engine::getSong(); song->setTempo(240); song->setMasterVolume(100); std::vector<SampleFrame> pcm(44100);
	for (std::size_t index = 0; index < pcm.size(); ++index)
	{
		pcm[index][0] = .4f * std::sin(6.283185307179586 * 440 * index / 44100);
		pcm[index][1] = .2f * std::sin(6.283185307179586 * 660 * index / 44100);
	}
	auto track = new SampleTrack(song); auto clip = static_cast<SampleClip*>(track->createClip(TimePos()));
	clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100)); clip->changeLength(TimePos(192));
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
class TimeSignatureSafetyTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	void replay(StudioRuntime& runtime, const QJsonObject& command, const QJsonObject& receipt)
	{ auto response = runtime.process(command); QVERIFY(response.take("replayed").toBool()); QCOMPARE(response, receipt); }
	void render(StudioRuntime& runtime, const QString& id, const QString& path)
	{
		const auto response = runtime.process(request(id, "audio.render", {{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}}));
		QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QVERIFY(decoded(path));
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); }

	void positiveMetersKeepIntegerResolutionAndSignedRemainders_data()
	{
		QTest::addColumn<int>("numerator"); QTest::addColumn<int>("denominator"); QTest::addColumn<int>("bar"); QTest::addColumn<int>("beat");
		QTest::newRow("4-4") << 4 << 4 << 192 << 48; QTest::newRow("3-4") << 3 << 4 << 144 << 48;
		QTest::newRow("6-8") << 6 << 8 << 144 << 24; QTest::newRow("7-8") << 7 << 8 << 168 << 24;
		QTest::newRow("11-7-truncation") << 11 << 7 << 301 << 27; QTest::newRow("1-32") << 1 << 32 << 6 << 6;
		QTest::newRow("32-1") << 32 << 1 << 6144 << 192; QTest::newRow("one-tick") << 1 << 192 << 1 << 1;
		QTest::newRow("wide-intermediate-exact-max") << std::numeric_limits<int>::max() << 192 << std::numeric_limits<int>::max() << 1;
		QTest::newRow("last-multiple") << 11184810 << 1 << 2147483520 << 192;
	}
	void positiveMetersKeepIntegerResolutionAndSignedRemainders()
	{
		QFETCH(int, numerator); QFETCH(int, denominator); QFETCH(int, bar); QFETCH(int, beat); const TimeSig signature(numerator, denominator);
		QCOMPARE(TimePos::ticksPerBar(signature), bar); QCOMPARE(TimePos().ticksPerBeat(signature), beat);
		const TimePos position(std::numeric_limits<int>::max()); QCOMPARE(position.getTickWithinBar(signature), std::numeric_limits<int>::max() % bar);
		QCOMPARE(position.getBeatWithinBar(signature), (std::numeric_limits<int>::max() % bar) / beat);
		QCOMPARE(position.getTickWithinBeat(signature), (std::numeric_limits<int>::max() % bar) % beat);
		QCOMPARE(TimePos(-1).getTickWithinBar(signature), -1 % bar); QCOMPARE(TimePos(-1).getBeatWithinBar(signature), (-1 % bar) / beat);
		QCOMPARE(TimePos(-1).getTickWithinBeat(signature), (-1 % bar) % beat);
	}

	void invalidBarsRefuseBeforeMultiplicationDivisionOrRemainder_data()
	{
		QTest::addColumn<int>("numerator"); QTest::addColumn<int>("denominator"); QTest::addColumn<bool>("range");
		QTest::newRow("zero-numerator") << 0 << 4 << false; QTest::newRow("negative-numerator") << -1 << 4 << false;
		QTest::newRow("zero-denominator") << 4 << 0 << false; QTest::newRow("negative-denominator") << 4 << -1 << false;
		QTest::newRow("minimum-over-negative-one") << std::numeric_limits<int>::min() << -1 << false;
		QTest::newRow("minimum-numerator") << std::numeric_limits<int>::min() << 4 << false;
		QTest::newRow("minimum-denominator") << 4 << std::numeric_limits<int>::min() << false;
		QTest::newRow("less-than-a-tick") << 1 << 193 << false;
		QTest::newRow("first-overflow-multiple") << 11184811 << 1 << true;
		QTest::newRow("wide-quotient-overflow") << std::numeric_limits<int>::max() << 191 << true;
	}
	void invalidBarsRefuseBeforeMultiplicationDivisionOrRemainder()
	{
		QFETCH(int, numerator); QFETCH(int, denominator); QFETCH(bool, range); const TimeSig signature(numerator, denominator); const TimePos position(7);
		if (range)
		{
			QVERIFY_EXCEPTION_THROWN(TimePos::ticksPerBar(signature), std::out_of_range); QVERIFY_EXCEPTION_THROWN(position.ticksPerBeat(signature), std::out_of_range);
			QVERIFY_EXCEPTION_THROWN(position.getTickWithinBar(signature), std::out_of_range); QVERIFY_EXCEPTION_THROWN(position.getBeatWithinBar(signature), std::out_of_range);
			QVERIFY_EXCEPTION_THROWN(position.getTickWithinBeat(signature), std::out_of_range);
		}
		else
		{
			QVERIFY_EXCEPTION_THROWN(TimePos::ticksPerBar(signature), std::domain_error); QVERIFY_EXCEPTION_THROWN(position.ticksPerBeat(signature), std::domain_error);
			QVERIFY_EXCEPTION_THROWN(position.getTickWithinBar(signature), std::domain_error); QVERIFY_EXCEPTION_THROWN(position.getBeatWithinBar(signature), std::domain_error);
			QVERIFY_EXCEPTION_THROWN(position.getTickWithinBeat(signature), std::domain_error);
		}
	}

	void aRepresentableBarCannotHideAZeroLengthBeat_data()
	{
		QTest::addColumn<int>("numerator"); QTest::addColumn<int>("denominator"); QTest::newRow("fractional-beat") << 2 << 193;
		QTest::newRow("wide-equal-meter") << std::numeric_limits<int>::max() << std::numeric_limits<int>::max();
	}
	void aRepresentableBarCannotHideAZeroLengthBeat()
	{
		QFETCH(int, numerator); QFETCH(int, denominator); const TimeSig signature(numerator, denominator); const TimePos position(7);
		QVERIFY(TimePos::ticksPerBar(signature) > 0); QVERIFY_EXCEPTION_THROWN(position.ticksPerBeat(signature), std::domain_error);
		QVERIFY_EXCEPTION_THROWN(position.getBeatWithinBar(signature), std::domain_error); QVERIFY_EXCEPTION_THROWN(position.getTickWithinBeat(signature), std::domain_error);
	}

	void badLiveSongControlsLeaveTheAcceptedClockAndVstMeterIntact_data()
	{
		QTest::addColumn<bool>("denominator"); QTest::addColumn<int>("kind");
		QTest::newRow("numerator-zero") << false << 0; QTest::newRow("denominator-zero") << true << 0;
		QTest::newRow("numerator-negative") << false << 1; QTest::newRow("denominator-negative") << true << 1;
		QTest::newRow("numerator-large") << false << 2; QTest::newRow("denominator-large") << true << 2;
		QTest::newRow("numerator-nan") << false << 3; QTest::newRow("denominator-nan") << true << 3;
		QTest::newRow("numerator-unrepresentable") << false << 4; QTest::newRow("denominator-unrepresentable") << true << 4;
	}
	void badLiveSongControlsLeaveTheAcceptedClockAndVstMeterIntact()
	{
		QFETCH(bool, denominator); QFETCH(int, kind); Input input; auto song = Engine::getSong(); auto& meter = song->getTimeSigModel();
		auto& model = denominator ? meter.denominatorModel() : meter.numeratorModel(); model.setControllerConnection(new ControllerConnection(&input));
		const auto repair = qScopeGuard([&] { input.changeAfterFirst = false; input.next = .125f; bool changed = false; QString error; model.disconnectController(changed, error); });
		SharedMemory<const VstSyncData> sync; sync.attach(song->syncKey()); QVERIFY(sync); const int oldNumerator = sync->timeSigNumer, oldDenominator = sync->timeSigDenom;
		const auto ticks = TimePos::ticksPerBar(); QSignalSpy laterModel(&model, &Model::dataChanged), laterMeter(&meter, &Model::dataChanged), songMeter(song, &Song::timeSignatureChanged);
		input.next = kind == 0 ? -1.f / 31 : kind == 1 ? -5.f / 31 : kind == 2 ? (33554432.f - 1) / 31
			: kind == 3 ? std::numeric_limits<float>::quiet_NaN() : 0x1p40f;
		if (kind == 3) { QVERIFY_EXCEPTION_THROWN(model.setValue(5), std::domain_error); }
		else { QVERIFY_EXCEPTION_THROWN(model.setValue(5), std::out_of_range); }
		QCOMPARE(laterModel.size(), 1); QCOMPARE(laterMeter.size(), 1); QCOMPARE(songMeter.size(), 0); QCOMPARE(TimePos::ticksPerBar(), ticks);
		QCOMPARE(sync->timeSigNumer, oldNumerator); QCOMPARE(sync->timeSigDenom, oldDenominator);
		std::atomic_bool acquired{false}; std::thread observer([&] { const auto guard = Engine::audioEngine()->requestChangesGuard(); acquired = true; }); observer.join(); QVERIFY(acquired.load());
		bool changed = false; QString error; QVERIFY2(model.disconnectController(changed, error), qPrintable(error)); QVERIFY(changed);
		meter.setNumerator(4); meter.setDenominator(4); QCOMPARE(TimePos::ticksPerBar(), 192); QCOMPARE(sync->timeSigNumer, 4); QCOMPARE(sync->timeSigDenom, 4);
	}

	void songPublishesOneReadOfEachLiveControl()
	{
		Input input; auto song = Engine::getSong(); auto& model = song->getTimeSigModel().numeratorModel(); model.setControllerConnection(new ControllerConnection(&input));
		const auto repair = qScopeGuard([&] { input.changeAfterFirst = false; input.next = .125f; bool changed = false; QString error; model.disconnectController(changed, error); });
		input.next = .5f; input.reads = 0; input.changeAfterFirst = true; QSignalSpy changed(song, &Song::timeSignatureChanged);
		model.setValue(5); QCOMPARE(input.reads, 1); QCOMPARE(changed.size(), 1); QCOMPARE(changed.at(0).at(1).toInt(), 768); QCOMPARE(TimePos::ticksPerBar(), 768);
		SharedMemory<const VstSyncData> sync; sync.attach(song->syncKey()); QVERIFY(sync); QCOMPARE(sync->timeSigNumer, 16); QCOMPARE(sync->timeSigDenom, 4);
	}

	void theLocalModelGuardIsReleasedBeforeNotifyingObservers()
	{
		auto song = Engine::getSong(); auto engine = Engine::audioEngine(); std::atomic_bool acquired{false}; int calls = 0;
		const auto connection = connect(song, &Song::timeSignatureChanged, this, [&](int, int) {
			receive([&] {
				++calls; std::thread observer([&] { const auto guard = engine->requestChangesGuard(); acquired = true; }); observer.join();
			});
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		song->getTimeSigModel().setNumerator(3); QCOMPARE(calls, 1); QVERIFY(acquired.load());
	}

	void receiverFailuresReturnAfterQtAndKeepPublishedHistoryConsistent_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("data");
		QTest::newRow("meter-standard") << false << false; QTest::newRow("meter-nonstandard") << true << false;
		QTest::newRow("data-standard") << false << true; QTest::newRow("data-nonstandard") << true << true;
	}
	void receiverFailuresReturnAfterQtAndKeepPublishedHistoryConsistent()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, data); auto song = Engine::getSong(); auto& meter = song->getTimeSigModel();
		const auto callback = [&] { receive([&] { raise(nonstandard); }); };
		const auto connection = data ? connect(song, &Model::dataChanged, this, callback, Qt::DirectConnection)
			: connect(song, &Song::timeSignatureChanged, this, callback, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QSignalSpy later(song, &Song::timeSignatureChanged), laterData(song, &Model::dataChanged), laterMeter(&meter, &Model::dataChanged);
		std::exception_ptr failure; try { meter.setNumerator(3); } catch (...) { failure = std::current_exception(); }
		QVERIFY(failure); try { std::rethrow_exception(failure); } catch (const std::runtime_error&) { QVERIFY(!nonstandard); } catch (int value) { QVERIFY(nonstandard); QCOMPARE(value, 359); }
		QCOMPARE(later.size(), 1); QCOMPARE(laterData.size(), data ? 1 : 0); QCOMPARE(laterMeter.size(), 1); QCOMPARE(TimePos::ticksPerBar(), 144);
		SharedMemory<const VstSyncData> sync; sync.attach(song->syncKey()); QVERIFY(sync); QCOMPARE(sync->timeSigNumer, 3); QCOMPARE(sync->timeSigDenom, 4);
		QObject::disconnect(connection); meter.setNumerator(5); QCOMPARE(later.size(), 2); QCOMPARE(later.at(1).at(0).toInt(), 144); QCOMPARE(later.at(1).at(1).toInt(), 240);
	}

	void aNestedMeterUpdateOwnsTheNewerClockAndHistory()
	{
		auto song = Engine::getSong(); auto& meter = song->getTimeSigModel(); bool entered = false;
		const auto connection = connect(song, &Song::timeSignatureChanged, this, [&](int, int ticks) {
			if (ticks == 144 && !entered) { entered = true; receive([&] { meter.setDenominator(8); }); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QSignalSpy later(song, &Song::timeSignatureChanged), laterData(song, &Model::dataChanged);
		QVERIFY_EXCEPTION_THROWN(meter.setNumerator(3), std::runtime_error); QVERIFY(entered); QCOMPARE(later.size(), 2); QCOMPARE(laterData.size(), 1); QCOMPARE(TimePos::ticksPerBar(), 72);
		SharedMemory<const VstSyncData> sync; sync.attach(song->syncKey()); QVERIFY(sync); QCOMPARE(sync->timeSigNumer, 3); QCOMPARE(sync->timeSigDenom, 8);
		QObject::disconnect(connection); meter.setNumerator(5); QCOMPARE(later.last().at(0).toInt(), 72); QCOMPARE(later.last().at(1).toInt(), 120); QCOMPARE(sync->timeSigNumer, 5);
	}

	void runtimeMeterFailuresRestoreAndRenderAfterRevisionAndReopen_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void runtimeMeterFailuresRestoreAndRenderAfterRevisionAndReopen()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory, output; QJsonObject command, receipt;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); arrange();
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 240}})).value("ok").toBool()); const auto before = manifest(directory); bool injected = false;
			auto song = Engine::getSong(); const auto connection = connect(song, &Song::timeSignatureChanged, this, [&](int, int ticks) {
				if (ticks == 144 && !injected) { injected = true; receive([&] { raise(nonstandard); }); }
			}, Qt::DirectConnection);
			const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			command = request("reject-meter", "parameter.set", {{"parameter", int(song->getTimeSigModel().numeratorModel().id())}, {"value", 3}});
			receipt = runtime.process(command); QVERIFY(injected); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!receipt.contains("result")); QVERIFY(!runtime.stopping());
			QCOMPARE(TimePos::ticksPerBar(), 192); QCOMPARE(song->getTimeSigModel().getNumerator(), 4); QCOMPARE(song->getTimeSigModel().getDenominator(), 4);
			const auto after = manifest(directory); QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
			QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1); replay(runtime, command, receipt); QObject::disconnect(connection);
			QVERIFY(runtime.process(request("revise", "parameter.set", {{"parameter", int(song->getTimeSigModel().numeratorModel().id())}, {"value", 5}})).value("ok").toBool());
			render(runtime, "render-revised", output.filePath("revised.wav"));
		}
		StudioRuntime reopened(directory.path()); QString error; QVERIFY2(reopened.open(error), qPrintable(error)); replay(reopened, command, receipt);
		QCOMPARE(Engine::getSong()->getTimeSigModel().getNumerator(), 5); QCOMPARE(Engine::getSong()->getTimeSigModel().getDenominator(), 4); QCOMPARE(TimePos::ticksPerBar(), 240);
		render(reopened, "render-reopened", output.filePath("reopened.wav"));
	}
};
QTEST_GUILESS_MAIN(TimeSignatureSafetyTest)
#include "TimeSignatureSafetyTest.moc"
