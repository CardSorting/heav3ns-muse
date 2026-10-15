/* Native nested tempo/meter notification, ownership and durable recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
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
#include "ModelValueNotificationTestUtils.h"
#include "NativeValueNotification.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioRuntime.h"
#include "TempoSyncKnobModel.h"
using namespace lmms;
namespace
{
class Sync final : public TempoSyncKnobModel
{
public:
	Sync() : TempoSyncKnobModel(42, 0, 1000, 0, 1000, nullptr) { }
	void calculate(bpm_t tempo) { calculateTempoSyncTime(tempo); }
};
class Input final : public Controller
{
public:
	Input() : Controller(ControllerType::Dummy, nullptr, "Native tempo getter fault fixture") { }
	int failAt = 0, reads = 0; bool nonstandard = false; float next = .25f;
	float currentValue(int) override
	{
		if (++reads == failAt) { if (nonstandard) { throw 317; } throw std::runtime_error("Native tempo getter fault fixture"); }
		return next;
	}
};
void fail(bool nonstandard)
{ if (nonstandard) { throw 319; } throw std::runtime_error("Native synchronized parameter fault fixture"); }
QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
{ return {{"id", id}, {"op", operation}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
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
	auto lfo = new LfoController(song); song->addController(lfo); auto speed = lfo->findChild<TempoSyncKnobModel*>();
	if (!speed) { throw std::runtime_error("Native LFO speed model is unavailable"); }
	speed->setSyncMode(TempoSyncKnobModel::SyncMode::QuarterNote);
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
		const auto value = pcm[static_cast<std::size_t>(frame) * 2 + channel]; if (!std::isfinite(value)) { return false; }
		squares[channel] += double(value) * value;
	}
	const auto left = std::sqrt(squares[0] / info.frames), right = std::sqrt(squares[1] / info.frames);
	return left > .01 && right > .005 && std::abs(left / right - 2.) < .12;
}
}
class TempoNotificationRecoveryTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	void restoredLfo(bpm_t tempo)
	{
		auto song = Engine::getSong(); QCOMPARE(song->getTempo(), tempo); QCOMPARE(song->controllers().size(), std::size_t(1));
		auto controller = song->controllers().front(); auto speed = controller->findChild<TempoSyncKnobModel*>(); QVERIFY(speed);
		QCOMPARE(speed->syncMode(), TempoSyncKnobModel::SyncMode::QuarterNote);
		QVERIFY(std::abs(speed->storedValue() - 60000.f / (tempo * speed->scale()) * speed->maxValue()) < .001f);
		auto binding = Engine::mixer()->mixerChannel(0)->m_volumeModel.controllerConnection(); QVERIFY(binding); QCOMPARE(binding->getController(), controller);
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
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setTempo(91); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void synchronizedTempoFailuresReturnAfterLaterObservers_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("unchanged"); QTest::addColumn<bool>("enabled");
		QTest::newRow("changed-standard-enabled") << false << false << true; QTest::newRow("changed-nonstandard-enabled") << true << false << true;
		QTest::newRow("unchanged-standard-enabled") << false << true << true; QTest::newRow("unchanged-nonstandard-enabled") << true << true << true;
		QTest::newRow("changed-standard-disabled") << false << false << false; QTest::newRow("changed-nonstandard-disabled") << true << false << false;
		QTest::newRow("unchanged-standard-disabled") << false << true << false; QTest::newRow("unchanged-nonstandard-disabled") << true << true << false;
	}
	void synchronizedTempoFailuresReturnAfterLaterObservers()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, unchanged); QFETCH(bool, enabled); Sync synced; synced.setSyncMode(Sync::SyncMode::QuarterNote); synced.setJournalling(enabled);
		ModelValueNotificationProbe fault(&synced, [&] { fail(nonstandard); }, true); int laterSong = 0;
		const auto connection = connect(Engine::getSong(), &Song::tempoChanged, this, [&] { ++laterSong; }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QSignalSpy laterModel(&Engine::getSong()->tempoModel(), unchanged ? &Model::dataUnchanged : &Model::dataChanged); int caught = 0;
		try { Engine::getSong()->setTempo(unchanged ? 91 : 123); }
		catch (const std::runtime_error&) { caught = 1; } catch (int value) { QCOMPARE(value, 319); caught = 2; }
		QCOMPARE(caught, nonstandard ? 2 : 1); QCOMPARE(laterSong, 1); QCOMPARE(laterModel.size(), 1); QCOMPARE(synced.isJournalling(), enabled);
		fault.reset(); Engine::getSong()->setTempo(97); QVERIFY(std::abs(synced.storedValue() - 60000.f / 97) < .001f); QCOMPARE(synced.isJournalling(), enabled);
		QVERIFY(std::abs(Engine::framesPerTick() - Engine::audioEngine()->outputSampleRate() * 60.f / 48 / 97) < .01f);
	}

	void customMeterFailuresKeepUniqueReceiverAndAllowRetry_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("denominator");
		QTest::newRow("numerator-standard") << false << false; QTest::newRow("numerator-nonstandard") << true << false;
		QTest::newRow("denominator-standard") << false << true; QTest::newRow("denominator-nonstandard") << true << true;
	}
	void customMeterFailuresKeepUniqueReceiverAndAllowRetry()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, denominator); Sync synced; synced.setSyncMode(Sync::SyncMode::Custom); synced.setJournalling(true);
		ModelValueNotificationProbe fault(&synced, [&] { fail(nonstandard); }, true); auto& meter = synced.getCustomMeterModel();
		QSignalSpy later(&meter, &Model::dataChanged); int caught = 0;
		try { if (denominator) { meter.setDenominator(8); } else { meter.setNumerator(3); } }
		catch (const std::runtime_error&) { caught = 1; } catch (int value) { QCOMPARE(value, 319); caught = 2; }
		QCOMPARE(caught, nonstandard ? 2 : 1); QCOMPARE(later.size(), 1); QVERIFY(synced.isJournalling()); fault.reset();
		for (int index = 0; index < 3; ++index) { synced.setSyncMode(Sync::SyncMode::None); synced.setSyncMode(Sync::SyncMode::Custom); }
		QSignalSpy changes(&synced, &Model::dataChanged); meter.setNumerator(meter.getNumerator() + 1); QCOMPARE(changes.size(), 1);
		QVERIFY(std::abs(synced.storedValue() - 60000.f * meter.getNumerator() / (91 * meter.getDenominator())) < .001f); QVERIFY(synced.isJournalling());
	}

	void songGetterFailureReleasesModelOwnership_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<int>("read");
		QTest::newRow("initial-standard") << false << 1; QTest::newRow("initial-nonstandard") << true << 1;
		QTest::newRow("frames-standard") << false << 2; QTest::newRow("frames-nonstandard") << true << 2;
	}
	void songGetterFailureReleasesModelOwnership()
	{
		QFETCH(bool, nonstandard); QFETCH(int, read); Input input; auto song = Engine::getSong(); auto engine = Engine::audioEngine();
		const auto repair = qScopeGuard([&] { input.failAt = 0; bool changed = false; QString error; song->tempoModel().disconnectController(changed, error); });
		song->tempoModel().setControllerConnection(new ControllerConnection(&input)); input.reads = 0; input.failAt = read; input.nonstandard = nonstandard;
		QSignalSpy later(&song->tempoModel(), &Model::dataChanged); int caught = 0;
		try { song->setTempo(123); } catch (const std::runtime_error&) { caught = 1; } catch (int value) { QCOMPARE(value, 317); caught = 2; }
		QCOMPARE(caught, nonstandard ? 2 : 1); QCOMPARE(later.size(), 1); std::atomic_bool acquired{false};
		// The native process timeout bounds a leaked recursive model lock;
		// a same-thread retry would hide that regression.
		std::thread observer([&] { const auto changes = engine->requestChangesGuard(); acquired = true; }); observer.join(); QVERIFY(acquired.load());
		input.failAt = 0; bool changed = false; QString error; QVERIFY(song->tempoModel().disconnectController(changed, error)); song->setTempo(97); QCOMPARE(song->getTempo(), 97);
	}

	void invalidTimingInputsRefuseBeforeValuePublication_data()
	{
		QTest::addColumn<int>("damage"); QTest::newRow("tempo-zero") << 0; QTest::newRow("scale-zero") << 1; QTest::newRow("scale-negative") << 2;
		QTest::newRow("scale-nan") << 3; QTest::newRow("scale-infinite") << 4; QTest::newRow("scale-overflow") << 5; QTest::newRow("custom-negative") << 6;
	}
	void invalidTimingInputsRefuseBeforeValuePublication()
	{
		QFETCH(int, damage); Input input; Sync synced; synced.setSyncMode(damage == 6 ? Sync::SyncMode::Custom : Sync::SyncMode::QuarterNote); synced.setJournalling(true);
		if (damage == 6) { synced.getCustomMeterModel().numeratorModel().setControllerConnection(new ControllerConnection(&input)); input.next = -1.f; }
		const auto previous = synced.storedValue();
		if (damage == 0) { QVERIFY_EXCEPTION_THROWN(synced.calculate(0), std::invalid_argument); }
		else if (damage == 6) { QVERIFY_EXCEPTION_THROWN(synced.calculate(91), std::invalid_argument); }
		else
		{
			const float scale = damage == 1 ? 0.f : damage == 2 ? -1.f : damage == 3 ? std::numeric_limits<float>::quiet_NaN()
				: damage == 4 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::max();
			QVERIFY_EXCEPTION_THROWN(synced.setScale(scale), std::invalid_argument);
		}
		QCOMPARE(synced.storedValue(), previous); QVERIFY(synced.isJournalling()); input.next = .25f; synced.setScale(1000); synced.calculate(97); QVERIFY(std::isfinite(synced.storedValue()));
	}

	void unframedSignalsDiagnoseWithoutUnwindingQt_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("custom");
		QTest::newRow("tempo-standard") << false << false; QTest::newRow("tempo-nonstandard") << true << false;
		QTest::newRow("custom-standard") << false << true; QTest::newRow("custom-nonstandard") << true << true;
	}
	void unframedSignalsDiagnoseWithoutUnwindingQt()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, custom); Sync synced; synced.setSyncMode(custom ? Sync::SyncMode::Custom : Sync::SyncMode::QuarterNote);
		ModelValueNotificationProbe fault(&synced, [&] { fail(nonstandard); }, true);
		if (custom)
		{
			QTest::ignoreMessage(QtWarningMsg, "Custom tempo synchronization failed outside a native value notification frame");
			QVERIFY(QMetaObject::invokeMethod(&synced.getCustomMeterModel(), "dataChanged", Qt::DirectConnection));
		}
		else
		{
			QTest::ignoreMessage(QtWarningMsg, "Tempo synchronization failed outside a native value notification frame"); const bpm_t tempo = 123;
			QVERIFY(QMetaObject::invokeMethod(Engine::getSong(), "tempoChanged", Qt::DirectConnection, Q_ARG(lmms::bpm_t, tempo)));
		}
		fault.reset(); Engine::getSong()->setTempo(97); QVERIFY(std::isfinite(synced.storedValue()));
	}

	void reportedModeAndScaleFailuresLeaveQtBeforeDirectReturn_data()
	{
		QTest::addColumn<bool>("nonstandard"); QTest::addColumn<bool>("scale");
		QTest::newRow("mode-standard") << false << false; QTest::newRow("mode-nonstandard") << true << false;
		QTest::newRow("scale-standard") << false << true; QTest::newRow("scale-nonstandard") << true << true;
	}
	void reportedModeAndScaleFailuresLeaveQtBeforeDirectReturn()
	{
		QFETCH(bool, nonstandard); QFETCH(bool, scale); Sync synced; if (scale) { synced.setSyncMode(Sync::SyncMode::QuarterNote); }
		const auto fault = nonstandard ? std::make_exception_ptr(331) : std::make_exception_ptr(std::runtime_error("Native tempo mode/scale report"));
		const auto receiver = [&] { QVERIFY(NativeValueNotification::report(fault)); };
		const auto connection = scale ? connect(&synced, &Sync::scaleChanged, this, receiver, Qt::DirectConnection)
			: connect(&synced, &Sync::syncModeChanged, this, receiver, Qt::DirectConnection);
		int later = 0; const auto observer = [&] { ++later; };
		const auto laterConnection = scale ? connect(&synced, &Sync::scaleChanged, this, observer, Qt::DirectConnection)
			: connect(&synced, &Sync::syncModeChanged, this, observer, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); QObject::disconnect(laterConnection); });
		std::exception_ptr received; try { if (scale) { synced.setScale(2000); } else { synced.setSyncMode(Sync::SyncMode::QuarterNote); } }
		catch (...) { received = std::current_exception(); }
		QVERIFY(received == fault); QCOMPARE(later, 1); QObject::disconnect(connection);
		if (scale) { synced.setScale(3000); } else { synced.setSyncMode(Sync::SyncMode::QuarterNote); } QCOMPARE(later, 2);
	}

	void runtimeTempoFailureRestoresHistoryAndSurvivesReopen_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void runtimeTempoFailureRestoresHistoryAndSurvivesReopen()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory, output; const auto command = request("reject-tempo", "song.set", {{"tempo", 123}}); QJsonObject receipt;
		{
			StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error)); arrange();
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 91}})).value("ok").toBool()); const auto before = manifest(directory);
			auto speed = Engine::getSong()->controllers().front()->findChild<TempoSyncKnobModel*>(); QVERIFY(speed); const QPointer<TempoSyncKnobModel> old(speed);
			bool injected = false;
			ModelValueNotificationProbe fault(speed, [&] { if (!injected) { injected = true; fail(nonstandard); } }, true); receipt = runtime.process(command);
			QVERIFY(injected);
			QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!receipt.contains("result")); QVERIFY(!runtime.stopping()); QVERIFY(!old); restoredLfo(91);
			const auto after = manifest(directory); QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("cursor"), before.value("cursor"));
			QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
			auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); fault.reset();
			QVERIFY(runtime.process(request("revise", "song.set", {{"tempo", 97}})).value("ok").toBool()); restoredLfo(97);
			const auto path = output.filePath("repaired.wav"); const auto rendered = runtime.process(request("render", "audio.render", {{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}}));
			QVERIFY2(rendered.value("ok").toBool(), qPrintable(rendered.value("error").toString())); QVERIFY(decoded(path));
		}
		StudioRuntime reopened(directory.path()); QString error; QVERIFY2(reopened.open(error), qPrintable(error)); restoredLfo(97);
		auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt);
		const auto path = output.filePath("reopened.wav"); const auto rendered = reopened.process(request("render-again", "audio.render", {{"path", path}, {"sampleRate", 44100}, {"bitDepth", 24}, {"timeoutMs", 30000}}));
		QVERIFY2(rendered.value("ok").toBool(), qPrintable(rendered.value("error").toString())); QVERIFY(decoded(path));
	}
};
QTEST_GUILESS_MAIN(TempoNotificationRecoveryTest)
#include "TempoNotificationRecoveryTest.moc"
