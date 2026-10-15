/* Failure transport across actual native model/controller/MIDI emissions.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutoDetectMidiController.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "LfoController.h"
#include "ModelValueNotificationTestUtils.h"
#include "NativeValueNotification.h"
#include "NotePlayHandle.h"
#include "Song.h"
using namespace lmms;
class NativeValueNotificationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void eachNativeEmitterRetainsTheFirstReportAndDeliversLaterObservers_data()
	{
		QTest::addColumn<int>("emitter"); QTest::addColumn<bool>("nonstandard");
		QTest::newRow("model-standard") << 0 << false; QTest::newRow("model-nonstandard") << 0 << true;
		QTest::newRow("unchanged-standard") << 1 << false; QTest::newRow("unchanged-nonstandard") << 1 << true;
		QTest::newRow("periodic-standard") << 2 << false; QTest::newRow("periodic-nonstandard") << 2 << true;
		QTest::newRow("binding-standard") << 3 << false; QTest::newRow("binding-nonstandard") << 3 << true;
		QTest::newRow("midi-standard") << 4 << false; QTest::newRow("midi-nonstandard") << 4 << true;
		QTest::newRow("detect-standard") << 5 << false; QTest::newRow("detect-nonstandard") << 5 << true;
	}
	void eachNativeEmitterRetainsTheFirstReportAndDeliversLaterObservers()
	{
		QFETCH(int, emitter); QFETCH(bool, nonstandard); FloatModel model(42, 0, 100, 0); LfoController first(nullptr), last(nullptr);
		ControllerConnection binding(&first); MidiController midi(nullptr); AutoDetectMidiController detector(nullptr); midi.midiPort()->setInputController(74);
		const auto fault = nonstandard ? std::make_exception_ptr(277) : std::make_exception_ptr(std::runtime_error("First native value failure"));
		const auto later = nonstandard ? std::make_exception_ptr(std::runtime_error("Later native value failure")) : std::make_exception_ptr(281);
		int reports = 0; const auto receiver = [&] {
			QVERIFY(NativeValueNotification::report(fault)); ++reports; QVERIFY(NativeValueNotification::report(later)); ++reports;
		};
		QMetaObject::Connection connection; std::unique_ptr<QSignalSpy> delivered;
		if (emitter == 0) { connection = connect(&model, &Model::dataChanged, this, receiver, Qt::DirectConnection); delivered = std::make_unique<QSignalSpy>(&model, &Model::dataChanged); }
		else if (emitter == 1) { connection = connect(&model, &Model::dataUnchanged, this, receiver, Qt::DirectConnection); delivered = std::make_unique<QSignalSpy>(&model, &Model::dataUnchanged); }
		else if (emitter == 2) { connection = connect(&first, &Controller::valueChanged, this, receiver, Qt::DirectConnection); delivered = std::make_unique<QSignalSpy>(&first, &Controller::valueChanged); }
		else if (emitter == 3) { connection = connect(&binding, &ControllerConnection::valueChanged, this, receiver, Qt::DirectConnection); delivered = std::make_unique<QSignalSpy>(&binding, &ControllerConnection::valueChanged); }
		else if (emitter == 4) { connection = connect(&midi, &Controller::valueChanged, this, receiver, Qt::DirectConnection); delivered = std::make_unique<QSignalSpy>(&midi, &Controller::valueChanged); }
		else { connection = connect(&detector, &Controller::valueChanged, this, receiver, Qt::DirectConnection); delivered = std::make_unique<QSignalSpy>(&detector, &Controller::valueChanged); }
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QSignalSpy lastController(&last, &Controller::valueChanged);
		float next = 70; const auto send = [&] {
			if (emitter == 0) { model.setValue(next); }
			else if (emitter == 1) { model.setValue(42); }
			else if (emitter == 2) { Controller::triggerFrameCounter(); }
			else if (emitter == 3) { binding.setController(binding.getController() == &first ? &last : &first); }
			else if (emitter == 4) { midi.processInEvent(MidiEvent(MidiControlChange, 0, 74, 84), {}); }
			else { detector.processInEvent(MidiEvent(MidiControlChange, 0, 74, 84), {}); }
		};
		const auto period = Controller::runningPeriods(); std::exception_ptr received;
		try { send(); } catch (...) { received = std::current_exception(); }
		QVERIFY(received == fault); QCOMPARE(reports, 2); QCOMPARE(delivered->size(), 1); QVERIFY(!NativeValueNotification::report(later));
		if (emitter == 2) { QCOMPARE(Controller::runningPeriods(), period); QCOMPARE(lastController.size(), 0); }
		if (emitter == 5) { QVERIFY(detector.detection().received); QCOMPARE(detector.detection().value, 84); }
		QObject::disconnect(connection); next = 80; send(); QCOMPARE(delivered->size(), 2);
		if (emitter == 2) { QCOMPARE(Controller::runningPeriods(), period + 1); QCOMPARE(lastController.size(), 1); }
	}

	void nestedModelNotificationsKeepSeparateFailures_data()
	{
		QTest::addColumn<int>("outerKind"); QTest::addColumn<int>("innerKind");
		QTest::newRow("none-none") << 0 << 0; QTest::newRow("none-standard") << 0 << 1; QTest::newRow("none-nonstandard") << 0 << 2;
		QTest::newRow("standard-none") << 1 << 0; QTest::newRow("standard-standard") << 1 << 1; QTest::newRow("standard-nonstandard") << 1 << 2;
		QTest::newRow("nonstandard-none") << 2 << 0; QTest::newRow("nonstandard-standard") << 2 << 1; QTest::newRow("nonstandard-nonstandard") << 2 << 2;
	}
	void nestedModelNotificationsKeepSeparateFailures()
	{
		QFETCH(int, outerKind); QFETCH(int, innerKind); FloatModel outer(42, 0, 100, 0), inner(42, 0, 100, 0);
		const auto failure = [](int kind) { return kind == 1 ? std::make_exception_ptr(std::runtime_error("Native nested failure")) : kind == 2 ? std::make_exception_ptr(283) : std::exception_ptr{}; };
		const auto outerFailure = failure(outerKind), innerFailure = failure(innerKind); std::exception_ptr caughtInner, caughtOuter;
		const auto innerConnection = connect(&inner, &Model::dataChanged, this, [&] { if (innerFailure) { QVERIFY(NativeValueNotification::report(innerFailure)); } }, Qt::DirectConnection);
		const auto outerConnection = connect(&outer, &Model::dataChanged, this, [&] {
			try { inner.setValue(70); } catch (...) { caughtInner = std::current_exception(); }
			if (outerFailure) { QVERIFY(NativeValueNotification::report(outerFailure)); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(innerConnection); QObject::disconnect(outerConnection); });
		QSignalSpy laterOuter(&outer, &Model::dataChanged), laterInner(&inner, &Model::dataChanged);
		try { outer.setValue(70); } catch (...) { caughtOuter = std::current_exception(); }
		QVERIFY(caughtOuter == outerFailure); QVERIFY(caughtInner == innerFailure); QCOMPARE(laterOuter.size(), 1); QCOMPARE(laterInner.size(), 1);
		QVERIFY(!NativeValueNotification::report(failure(1)));
	}

	void emptyOutsideAndForeignThreadReportsAreRejected()
	{
		const auto failure = std::make_exception_ptr(293); QVERIFY(!NativeValueNotification::report(failure)); QVERIFY(!NativeValueNotification::report({}));
		FloatModel model(42, 0, 100, 0); std::atomic_bool refused{false};
		const auto connection = connect(&model, &Model::dataChanged, this, [&] {
			QVERIFY(!NativeValueNotification::report({}));
			auto worker = std::unique_ptr<QThread>(QThread::create([&] { refused = !NativeValueNotification::report(failure); }));
			worker->start(); QVERIFY(worker->wait()); QVERIFY(refused.load()); QVERIFY(NativeValueNotification::report(failure));
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); std::exception_ptr received;
		try { model.setValue(70); } catch (...) { received = std::current_exception(); }
		QVERIFY(received == failure); QVERIFY(!NativeValueNotification::report(failure));
	}

	void aReportedReceiverFailurePrecedesThePrivateTestProbe()
	{
		FloatModel model(42, 0, 100, 0); const auto receiverFailure = std::make_exception_ptr(307), probeFailure = std::make_exception_ptr(311); int probes = 0;
		ModelValueNotificationProbe probe(&model, [&] { ++probes; std::rethrow_exception(probeFailure); });
		const auto connection = connect(&model, &Model::dataChanged, this, [&] { QVERIFY(NativeValueNotification::report(receiverFailure)); }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); std::exception_ptr received;
		try { model.setValue(70); } catch (...) { received = std::current_exception(); }
		QVERIFY(received == receiverFailure); QCOMPARE(probes, 0); QObject::disconnect(connection);
		try { model.setValue(80); } catch (...) { received = std::current_exception(); }
		QVERIFY(received == probeFailure); QCOMPARE(probes, 1); QVERIFY(!NativeValueNotification::report(receiverFailure));
	}
};
QTEST_GUILESS_MAIN(NativeValueNotificationTest)
#include "NativeValueNotificationTest.moc"
