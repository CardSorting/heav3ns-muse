/* Native automation reservation source stability and capture invalidation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
using namespace lmms;

class AutomationTakeReservationTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QPointer<AutomationClip> clip;
	InstrumentTrack* instrument = nullptr;
	bool journalling = false;
	void stage()
	{
		clip->reserveRecordingTake(16, 0); clip->recordValue(12, 25);
	}
	void change(int kind)
	{
		if (kind == 0) { clip->putValue(48, 55, false, true); }
		if (kind == 1) { clip->getTimeMap()[96].setOutValue(95); }
		if (kind == 2) { clip->getTimeMap()[96].setInTangent(0.5f); }
		if (kind == 3) { clip->movePosition(24); } if (kind == 4) { clip->changeLength(240); }
		if (kind == 5) { clip->setStartTimeOffset(-12); } if (kind == 6) { clip->setAutoResize(true); }
		if (kind == 7) { clip->setProgressionType(AutomationClip::ProgressionType::Discrete); }
		if (kind == 8) { clip->setTension(0.75f); }
		if (kind == 9) { clip->clearObjects(); clip->addObject(instrument->panningModel()); }
		if (kind == 10) { TimePos::setTicksPerBar(TimePos::ticksPerBar() + 12); }
		if (kind == 11) { instrument->volumeModel()->setRange(0, 120, 0.1f); }
		if (kind == 12) { instrument->volumeModel()->setStep(0.5f); }
		if (kind == 13) { instrument->volumeModel()->setScaleLogarithmic(true); }
		if (kind == 14) { delete instrument; instrument = nullptr; }
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
	void init()
	{
		Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors();
		instrument = new InstrumentTrack(Engine::getSong());
		clip = static_cast<AutomationClip*>((new AutomationTrack(Engine::getSong()))->createClip(0));
		QVERIFY(clip->addObject(instrument->volumeModel())); clip->putValues(0, 10, 20, false, true); clip->putValues(96, 80, 90, false, true);
		clip->setProgressionType(AutomationClip::ProgressionType::CubicHermite); clip->setTension(0.345678f);
		QVERIFY(clip->setNodeTangents(96, 0.125f, -0.375f, true)); clip->setAutoResize(false); clip->changeLength(192);
	}
	void cleanup() { if (clip) { clip->setRecording(false); clip->discardRecordingTake(); } Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void changesBeforeCommitEntryCannotOverwriteReservedSource_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("point") << 0; QTest::newRow("native-node") << 1;
		QTest::newRow("tangent") << 2; QTest::newRow("position") << 3; QTest::newRow("length") << 4;
		QTest::newRow("offset") << 5; QTest::newRow("auto-resize") << 6; QTest::newRow("progression") << 7;
		QTest::newRow("tension") << 8; QTest::newRow("binding") << 9; QTest::newRow("meter") << 10;
		QTest::newRow("model-range") << 11; QTest::newRow("model-step") << 12; QTest::newRow("model-scale") << 13;
	}
	void changesBeforeCommitEntryCannotOverwriteReservedSource()
	{
		QFETCH(int, kind); const int meter = TimePos::ticksPerBar(); const auto restore = qScopeGuard([&] { TimePos::setTicksPerBar(meter); });
		stage(); change(kind); const auto edited = clip->getTimeMap();
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error);
		QVERIFY(clip->getTimeMap() == edited); QVERIFY(!clip->getTimeMap().contains(12)); QVERIFY(clip->hasReservedRecording());
	}
	void changedCaptureContextLatchesAnErrorBeforeAnotherSample_data()
	{
		QTest::addColumn<int>("kind"); QTest::newRow("position") << 3; QTest::newRow("length") << 4;
		QTest::newRow("offset") << 5; QTest::newRow("auto-resize") << 6; QTest::newRow("progression") << 7;
		QTest::newRow("tension") << 8; QTest::newRow("binding") << 9; QTest::newRow("meter") << 10;
		QTest::newRow("model-range") << 11; QTest::newRow("model-step") << 12; QTest::newRow("model-scale") << 13;
		QTest::newRow("retired-model") << 14;
	}
	void changedCaptureContextLatchesAnErrorBeforeAnotherSample()
	{
		QFETCH(int, kind); const int meter = TimePos::ticksPerBar(); const auto restore = qScopeGuard([&] { TimePos::setTicksPerBar(meter); });
		stage(); change(kind); const auto edited = clip->getTimeMap(); clip->recordValue(24, 75);
		QVERIFY(!clip->recordingError().isEmpty()); QCOMPARE(clip->recordingSamples(), std::uint64_t(1));
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error); QVERIFY(clip->getTimeMap() == edited);
	}
	void visiblePointConflictIsDetectedAtCommitWithoutDiscardingTheEdit()
	{
		stage(); change(0); const auto edited = clip->getTimeMap(); clip->recordValue(24, 75);
		QVERIFY(clip->recordingError().isEmpty()); QCOMPARE(clip->recordingSamples(), std::uint64_t(2));
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error); QVERIFY(clip->getTimeMap() == edited);
	}
	void aNodeReferenceHeldBeforeReservationCannotChangeTheBaseline()
	{
		auto& node = clip->getTimeMap()[96]; stage(); node.setOutValue(95);
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error);
		QCOMPARE(clip->getTimeMap().value(96).getOutValue(), 95.f); QVERIFY(!clip->getTimeMap().contains(12));
	}
	void effectiveSourceRestorationCanCommitWhenNoInvalidSampleWasAccepted()
	{
		const auto original = clip->getTimeMap(); stage(); change(0); clip->getTimeMap() = original;
		clip->commitRecordingTake(); QCOMPARE(clip->valueAt(12), 25.f); QVERIFY(!clip->getTimeMap().contains(48));
	}
	void captureErrorRemainsLatchedAfterRestoringGeometry()
	{
		stage(); clip->setStartTimeOffset(-12); clip->recordValue(24, 75); clip->setStartTimeOffset(0);
		QVERIFY(!clip->recordingError().isEmpty()); QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error);
		clip->discardRecordingTake(); stage(); clip->commitRecordingTake(); QCOMPARE(clip->valueAt(12), 25.f);
	}
	void discardStartsANewBaselineIncludingTheUsersVisibleEdit()
	{
		stage(); change(0); QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error);
		clip->discardRecordingTake(); clip->reserveRecordingTake(16, 0); clip->recordValue(24, 75); clip->commitRecordingTake();
		QCOMPARE(clip->valueAt(24), 75.f); QCOMPARE(clip->valueAt(48), 55.f); QVERIFY(!clip->getTimeMap().contains(12));
	}
	void ordinaryControlValuesAndIndependentNameMuteRemainEditable()
	{
		stage(); instrument->volumeModel()->setValue(120); clip->setName("Retained"); clip->setMuted(true);
		clip->recordValue(24, instrument->volumeModel()->inverseScaledValue(instrument->volumeModel()->value<float>()));
		QVERIFY(clip->recordingError().isEmpty()); clip->commitRecordingTake(); QCOMPARE(clip->valueAt(24), 120.f);
		QCOMPARE(clip->name(), QString("Retained")); QVERIFY(clip->isMuted());
	}
	void detachedConstructorCallbackEditCannotPublishAReservation()
	{
		bool invoked = false; const auto connection = connect(Engine::getSong(), &Song::lengthChanged, this, [&] {
			if (!invoked) { invoked = true; change(0); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY_EXCEPTION_THROWN(clip->reserveRecordingTake(16, 0), std::runtime_error);
		QVERIFY(invoked); QVERIFY(!clip->hasReservedRecording()); QCOMPARE(clip->valueAt(48), 55.f);
	}
	void detachedConstructorCallbackCanReturnAfterReceiverRetirement()
	{
		bool invoked = false; const QPointer<AutomationClip> previous(clip);
		const auto connection = connect(Engine::getSong(), &Song::lengthChanged, this, [&] {
			if (!invoked) { invoked = true; delete clip.data(); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY_EXCEPTION_THROWN(clip->reserveRecordingTake(16, 0), std::runtime_error); QVERIFY(invoked); QVERIFY(!previous);
	}
	void nativeConstructionCallbackCanReadWithoutReceiverMutex()
	{
		int calls = 0; float value = -1; const auto connection = connect(Engine::getSong(), &Song::lengthChanged, this, [&] {
			std::thread reader([&] { value = clip->valueAt(0); }); reader.join(); ++calls;
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		clip->reserveRecordingTake(16, 0); QCOMPARE(calls, 1); QCOMPARE(value, 10.f);
		clip->recordValue(12, 25); clip->commitRecordingTake(); QCOMPARE(clip->valueAt(12), 25.f);
	}
	void nestedReservationKeepsTheFirstPublishedTake()
	{
		bool invoked = false; const auto connection = connect(Engine::getSong(), &Song::lengthChanged, this, [&] {
			if (!invoked) { invoked = true; clip->reserveRecordingTake(16, 0); clip->recordValue(24, 75); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY_EXCEPTION_THROWN(clip->reserveRecordingTake(16, 0), std::runtime_error); QVERIFY(invoked);
		QVERIFY(clip->hasReservedRecording()); QCOMPARE(clip->recordingSamples(), std::uint64_t(1));
		clip->commitRecordingTake(); QCOMPARE(clip->valueAt(24), 75.f);
	}
	void checkpointTargetMetadataChangesAlsoInvalidateTheBaseline_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("range") << 11; QTest::newRow("step") << 12; QTest::newRow("scale") << 13; }
	void checkpointTargetMetadataChangesAlsoInvalidateTheBaseline()
	{
		QFETCH(int, kind); stage(); const auto original = clip->getTimeMap();
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] { change(kind); }), std::runtime_error);
		QVERIFY(clip->getTimeMap() == original); QVERIFY(clip->hasReservedRecording());
	}
	void reusedTargetAddressCannotInheritAnOldReservation()
	{
		alignas(FloatModel) unsigned char storage[sizeof(FloatModel)]; auto model = new (storage) FloatModel(42, 0, 100, 1, nullptr);
		const auto destroy = qScopeGuard([&] { if (model) { model->~FloatModel(); } });
		clip->clearObjects(); QVERIFY(clip->addObject(model)); stage();
		const QPointer<AutomatableModel> previous(model); model->~FloatModel(); model = nullptr; QVERIFY(!previous);
		model = new (storage) FloatModel(42, 0, 100, 1, nullptr); clip->clearObjects(); QVERIFY(clip->addObject(model));
		clip->recordValue(24, 75); QVERIFY(!clip->recordingError().isEmpty());
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error); QVERIFY(!clip->getTimeMap().contains(12));
	}
};
QTEST_GUILESS_MAIN(AutomationTakeReservationTest)
#include "AutomationTakeReservationTest.moc"
