/* Native controller defaults, wide time helpers and notification lifetimes.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "NativeValueNotification.h"
#include "Engine.h"
#include "LfoController.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "ValueBuffer.h"

using namespace lmms;
namespace lmms
{
class ControllerStateTestAccess
{
public:
	// Controlled native clock input, not a real-time longevity benchmark.
	static void period(long value) { const auto changes = Engine::audioEngine()->requestChangesGuard(); Controller::s_periods = value; }
};
}

class ControllerStateTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	long priorPeriod = 0;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		priorPeriod = Controller::runningPeriods(); ControllerStateTestAccess::period(0);
	}
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); ControllerStateTestAccess::period(priorPeriod); }

	void constructedDefaultDoesNotDependOnPriorStorage_data()
	{
		QTest::addColumn<int>("pattern"); QTest::newRow("zero-storage") << 0;
		QTest::newRow("alternating-storage") << 0xa5; QTest::newRow("ones-storage") << 0xff;
	}
	void constructedDefaultDoesNotDependOnPriorStorage()
	{
		QFETCH(int, pattern); alignas(Controller) unsigned char storage[sizeof(Controller)]; std::fill_n(storage, sizeof(storage), static_cast<unsigned char>(pattern));
		auto controller = new (storage) Controller(Controller::ControllerType::Dummy, nullptr, "Native default fixture");
		const auto destroy = qScopeGuard([&] { controller->~Controller(); });
		QVERIFY(!controller->isSampleExact()); QCOMPARE(controller->currentValue(1), .5f);
		QCOMPARE(controller->currentValue(0), .5f); QCOMPARE(controller->currentValue(static_cast<int>(Engine::audioEngine()->framesPerPeriod()) - 1), .5f);
		for (const auto value : *controller->valueBuffer()) { QCOMPARE(value, .5f); }
		controller->setSampleExact(true); QVERIFY(controller->isSampleExact()); QCOMPARE(controller->currentValue(1), .5f);
	}

	void nativeDummyConnectionKeepsScalarMappingAndNoSampleBuffer_data()
	{ QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true; }
	void nativeDummyConnectionKeepsScalarMappingAndNoSampleBuffer()
	{
		QFETCH(bool, logarithmic); FloatModel model(42, 1, 100, 0, nullptr); model.setScaleLogarithmic(logarithmic);
		const float expected = model.scaledValue(model.minValue() + model.range() * .5f); model.setInitValue(expected);
		model.valueBuffer(); // Settle any native interpolation from the manual edit.
		model.setControllerConnection(new ControllerConnection(nullptr)); const auto controller = model.controllerConnection()->getController();
		QCOMPARE(controller->type(), Controller::ControllerType::Dummy); QVERIFY(!controller->isSampleExact()); QCOMPARE(model.value(1), expected);
		AutomatableModel::incrementPeriodCounter(); QVERIFY(!model.valueBuffer()); QCOMPARE(model.value(), expected); QCOMPARE(model.storedValue(), expected);
	}

	void nativeLfoAndMidiStillReadSamplesBeforeFrameZero_data()
	{
		QTest::addColumn<bool>("midi"); QTest::addColumn<int>("offset");
		QTest::newRow("lfo-first-offset") << false << 1; QTest::newRow("lfo-last-offset") << false << -1;
		QTest::newRow("midi-first-offset") << true << 1; QTest::newRow("midi-last-offset") << true << -1;
	}
	void nativeLfoAndMidiStillReadSamplesBeforeFrameZero()
	{
		QFETCH(bool, midi); QFETCH(int, offset); std::unique_ptr<Controller> controller;
		if (midi)
		{
			auto native = std::make_unique<MidiController>(nullptr); native->midiPort()->setInputController(74);
			native->processInEvent(MidiEvent(MidiControlChange, 0, 74, 127), {}); controller = std::move(native);
		}
		else { controller = std::make_unique<LfoController>(nullptr); }
		Controller::triggerFrameCounter();
		QVERIFY(controller->isSampleExact()); const auto actualOffset = offset < 0 ? static_cast<int>(Engine::audioEngine()->framesPerPeriod()) - 1 : offset;
		const float sample = controller->currentValue(actualOffset); QVERIFY(std::isfinite(sample)); QCOMPARE(sample, controller->valueBuffer()->values()[actualOffset]);
		QVERIFY(sample >= 0 && sample <= 1); QVERIFY(sample != .5f);
	}

	void nativeTimeHelpersKeepFractionalSecondsAndWideFrames_data()
	{
		QTest::addColumn<qint64>("period"); QTest::newRow("first-period") << qint64(1);
		const auto frames = Engine::audioEngine()->framesPerPeriod(); const auto rate = Engine::audioEngine()->outputSampleRate();
		QTest::newRow("fractional-second") << qint64(rate / (2 * frames) + 1);
		QTest::newRow("beyond-32-bit-frames") << qint64(std::numeric_limits<unsigned int>::max() / frames + 7);
	}
	void nativeTimeHelpersKeepFractionalSecondsAndWideFrames()
	{
		QFETCH(qint64, period); QVERIFY(period <= std::numeric_limits<long>::max()); ControllerStateTestAccess::period(static_cast<long>(period));
		const auto frames = static_cast<f_cnt_t>(period) * Engine::audioEngine()->framesPerPeriod(); QCOMPARE(Controller::runningFrames(), frames);
		const float expected = static_cast<float>(static_cast<double>(frames) / Engine::audioEngine()->outputSampleRate()); QCOMPARE(Controller::runningTime(), expected);
		if (period == 1) { QVERIFY(Controller::runningTime() > 0 && Controller::runningTime() < 1); }
		if (frames > std::numeric_limits<unsigned int>::max()) { QVERIFY(Controller::runningFrames() > std::numeric_limits<unsigned int>::max()); }
		Controller::triggerFrameCounter(); QCOMPARE(Controller::runningFrames(), frames + Engine::audioEngine()->framesPerPeriod());
	}

	void notificationSnapshotSurvivesRetirementAndDefersNewControllers_data()
	{
		QTest::addColumn<QString>("action"); QTest::newRow("retire-earlier") << QString("earlier");
		QTest::newRow("retire-later") << QString("later"); QTest::newRow("create-during-notification") << QString("create");
	}
	void notificationSnapshotSurvivesRetirementAndDefersNewControllers()
	{
		QFETCH(QString, action); auto a = std::make_unique<LfoController>(nullptr), b = std::make_unique<LfoController>(nullptr), c = std::make_unique<LfoController>(nullptr);
		std::unique_ptr<LfoController> added; std::unique_ptr<QSignalSpy> addedSpy;
		QSignalSpy aSpy(a.get(), &Controller::valueChanged), bSpy(b.get(), &Controller::valueChanged), cSpy(c.get(), &Controller::valueChanged);
		const QPointer<Controller> retired(action == "earlier" ? a.get() : b.get());
		const auto source = action == "later" ? a.get() : b.get();
		const auto connection = connect(source, &Controller::valueChanged, this, [&] {
			if (action == "earlier") { a.reset(); } else if (action == "later") { b.reset(); }
			else if (!added) { added = std::make_unique<LfoController>(nullptr); addedSpy = std::make_unique<QSignalSpy>(added.get(), &Controller::valueChanged); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Controller::triggerFrameCounter(); QCOMPARE(Controller::runningPeriods(), 1L); QCOMPARE(aSpy.size(), 1); QCOMPARE(bSpy.size(), action == "later" ? 0 : 1); QCOMPARE(cSpy.size(), 1);
		if (action == "create") { QVERIFY(addedSpy); QCOMPARE(addedSpy->size(), 0); } else { QVERIFY(!retired); }
		Controller::triggerFrameCounter(); QCOMPARE(Controller::runningPeriods(), 2L); QCOMPARE(cSpy.size(), 2);
		if (action == "create") { QCOMPARE(addedSpy->size(), 1); } QVERIFY(!Engine::getSong()->hasErrors());
	}

	void returnedNotificationExceptionPreservesClockAndAllowsNextPass()
	{
		LfoController first(nullptr), last(nullptr); QSignalSpy notifications(&last, &Controller::valueChanged);
		const auto connection = connect(&first, &Controller::valueChanged, this, [] {
			try { throw std::runtime_error("Native controller notification fixture"); }
			catch (...) { QVERIFY(NativeValueNotification::report(std::current_exception())); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QSignalSpy deliveredFirst(&first, &Controller::valueChanged);
		QVERIFY_EXCEPTION_THROWN(Controller::triggerFrameCounter(), std::runtime_error); QCOMPARE(Controller::runningPeriods(), 0L); QCOMPARE(notifications.size(), 0); QCOMPARE(deliveredFirst.size(), 1);
		QObject::disconnect(connection); Controller::triggerFrameCounter(); QCOMPARE(Controller::runningPeriods(), 1L); QCOMPARE(notifications.size(), 1); QCOMPARE(deliveredFirst.size(), 2);
	}

	void nativeRenderPeriodAllowsRetiringAnotherRackControllerAfterMixing()
	{
		auto song = Engine::getSong(); auto first = new LfoController(song), retired = new LfoController(song), last = new LfoController(song);
		song->addController(first); song->addController(retired); song->addController(last); const QPointer<Controller> gone(retired); QSignalSpy notifications(last, &Controller::valueChanged);
		const auto connection = connect(first, &Controller::valueChanged, this, [&] { if (gone) { song->removeController(gone.data()); } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::audioEngine()->renderNextPeriod(); QVERIFY(!gone); QCOMPARE(song->controllers().size(), std::size_t{2}); QCOMPARE(notifications.size(), 1);
		QVERIFY(Engine::audioEngine()->dspStatus().failure == AudioEngine::DspFailure::None);
		Engine::audioEngine()->renderNextPeriod(); QCOMPARE(notifications.size(), 2); QVERIFY(!song->hasErrors());
		QVERIFY(Engine::audioEngine()->dspStatus().failure == AudioEngine::DspFailure::None);
	}
};
QTEST_GUILESS_MAIN(ControllerStateTest)
#include "ControllerStateTest.moc"
