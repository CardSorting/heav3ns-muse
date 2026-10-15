/* Native staged automation publication and cooperative invalidation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <limits>
#include <memory>
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

class AutomationTakeCommitTest : public QObject
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
	void successfulCommitPreservesNativeNodesAndRebindsTheirOwner()
	{
		stage(); clip->commitRecordingTake(); QVERIFY(!clip->hasReservedRecording()); QCOMPARE(clip->valueAt(12), 25.f);
		const auto node = clip->getTimeMap().value(96); QCOMPARE(node.getInValue(), 80.f); QCOMPARE(node.getOutValue(), 90.f);
		QVERIFY(node.lockedTangents()); QCOMPARE(node.getInTangent(), 0.125f); QCOMPARE(node.getOutTangent(), -0.375f);
		clip->getTimeMap()[12].setInValue(35); QCOMPARE(clip->valueAt(12), 35.f);
		QCOMPARE(clip->getTension(), 0.345678f); QCOMPARE(clip->length().getTicks(), 192);
	}
	void discardedTakeCannotPublishFromAnOldPreparedMap()
	{
		const auto before = clip->getTimeMap(); stage();
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] { clip->discardRecordingTake(); }), std::runtime_error);
		QVERIFY(clip->getTimeMap() == before); QVERIFY(!clip->hasReservedRecording());
	}
	void replacedTakeRetainsItsOwnLifetimeAndCanCommitFreshly()
	{
		const auto before = clip->getTimeMap(); stage();
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] {
			clip->discardRecordingTake(); clip->reserveRecordingTake(16, 0); clip->recordValue(24, 75);
		}), std::runtime_error);
		QVERIFY(clip->getTimeMap() == before); QVERIFY(clip->hasReservedRecording());
		clip->commitRecordingTake(); QVERIFY(!clip->getTimeMap().contains(12)); QCOMPARE(clip->valueAt(24), 75.f);
	}
	void additionalRecordingInvalidatesPreparedSamples_data()
	{ QTest::addColumn<bool>("duplicate"); QTest::newRow("new-point") << false; QTest::newRow("same-point-same-value") << true; }
	void additionalRecordingInvalidatesPreparedSamples()
	{
		QFETCH(bool, duplicate); const auto before = clip->getTimeMap(); stage(); bool invoked = false;
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] {
			if (!invoked) { invoked = true; clip->recordValue(duplicate ? 12 : 24, duplicate ? 25 : 75); }
		}), std::runtime_error);
		QVERIFY(clip->getTimeMap() == before); QCOMPARE(clip->recordingSamples(), std::uint64_t(2));
		clip->commitRecordingTake(); QCOMPARE(clip->valueAt(12), 25.f); if (!duplicate) { QCOMPARE(clip->valueAt(24), 75.f); }
	}
	void changedCurveContextRefusesPublication_data()
	{
		QTest::addColumn<int>("change"); QTest::newRow("position") << 0; QTest::newRow("length") << 1;
		QTest::newRow("offset") << 2; QTest::newRow("auto-resize") << 3; QTest::newRow("progression") << 4;
		QTest::newRow("tension") << 5; QTest::newRow("binding") << 6; QTest::newRow("meter") << 7;
	}
	void changedCurveContextRefusesPublication()
	{
		QFETCH(int, change); stage(); const int meter = TimePos::ticksPerBar(); const auto restore = qScopeGuard([&] { TimePos::setTicksPerBar(meter); });
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] {
			if (change == 0) { clip->movePosition(24); } if (change == 1) { clip->changeLength(240); }
			if (change == 2) { clip->setStartTimeOffset(-12); } if (change == 3) { clip->setAutoResize(true); }
			if (change == 4) { clip->setProgressionType(AutomationClip::ProgressionType::Discrete); }
			if (change == 5) { clip->setTension(0.75f); }
			if (change == 6) { clip->clearObjects(); clip->addObject(instrument->panningModel()); }
			if (change == 7) { TimePos::setTicksPerBar(meter + 12); }
		}), std::runtime_error);
		QVERIFY(!clip->getTimeMap().contains(12)); QVERIFY(clip->hasReservedRecording());
	}
	void directNativeNodeChangesCannotBeOverwritten_data()
	{
		QTest::addColumn<int>("change"); QTest::newRow("in-value") << 0; QTest::newRow("out-value") << 1;
		QTest::newRow("tangent") << 2; QTest::newRow("locking") << 3;
	}
	void directNativeNodeChangesCannotBeOverwritten()
	{
		QFETCH(int, change); stage(); bool invoked = false;
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] {
			if (invoked) { return; } invoked = true; auto& node = clip->getTimeMap()[96];
			if (change == 0) { node.setInValue(65); } if (change == 1) { node.setOutValue(95); }
			if (change == 2) { node.setInTangent(0.5f); } if (change == 3) { node.setLockedTangents(false); }
		}), std::runtime_error);
		QVERIFY(invoked); QVERIFY(!clip->getTimeMap().contains(12)); QVERIFY(clip->hasReservedRecording());
		const auto node = clip->getTimeMap().value(96);
		if (change == 0) { QCOMPARE(node.getInValue(), 65.f); } if (change == 1) { QCOMPARE(node.getOutValue(), 95.f); }
		if (change == 2) { QCOMPARE(node.getInTangent(), 0.5f); } if (change == 3) { QVERIFY(!node.lockedTangents()); }
	}
	void recordingErrorsRaisedByACheckpointPreventPublication()
	{
		const auto before = clip->getTimeMap(); stage();
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] { clip->recordValue(24, std::numeric_limits<float>::quiet_NaN()); }), std::runtime_error);
		QVERIFY(clip->getTimeMap() == before); QVERIFY(!clip->recordingError().isEmpty()); QVERIFY(clip->hasReservedRecording());
	}
	void legacyDisconnectedTargetRetirementInvalidatesItsIdentity()
	{
		auto model = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr); clip->clearObjects(); QVERIFY(clip->addObject(model.get()));
		QObject::disconnect(model.get(), nullptr, clip.data(), nullptr); stage(); const auto before = clip->getTimeMap();
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] { model.reset(); }), std::runtime_error);
		QVERIFY(clip->getTimeMap() == before); QVERIFY(clip->hasReservedRecording());
		QVERIFY(!clip->objects().empty()); QVERIFY(!clip->objects().front());
	}
	void detachedClipCannotPublishIntoItsFormerRow()
	{
		stage(); const auto before = clip->getTimeMap(); const auto dispose = qScopeGuard([&] { if (clip) { delete clip.data(); } });
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] { clip->getTrack()->removeClip(clip.data()); }), std::runtime_error);
		QVERIFY(clip->getTimeMap() == before); QVERIFY(clip->hasReservedRecording());
	}
	void activeCaptureMustBeDisarmedBeforePublication()
	{
		stage(); clip->setRecording(true); const auto before = clip->getTimeMap();
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error); QVERIFY(clip->getTimeMap() == before);
		clip->setRecording(false); clip->commitRecordingTake(); QCOMPARE(clip->valueAt(12), 25.f);
	}
	void checkpointExceptionLeavesOriginalAndTakeAvailable_data()
	{ QTest::addColumn<int>("at"); QTest::newRow("first") << 1; QTest::newRow("final") << 2; }
	void checkpointExceptionLeavesOriginalAndTakeAvailable()
	{
		QFETCH(int, at); stage(); const auto before = clip->getTimeMap(); int calls = 0;
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] { if (++calls == at) { throw std::runtime_error("fixture checkpoint"); } }), std::runtime_error);
		QCOMPARE(calls, at); QVERIFY(clip->getTimeMap() == before); QVERIFY(clip->hasReservedRecording());
		clip->commitRecordingTake(); QCOMPARE(clip->valueAt(12), 25.f);
	}
	void returnedReceiverRetirementDoesNotUseItsMutex()
	{
		stage(); const QPointer<AutomationClip> previous(clip);
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake([&] { delete clip.data(); }), std::runtime_error);
		QVERIFY(!previous); QVERIFY(!clip);
	}
	void nativeReadersCanRunInsideCheckpointsAndFinalNotifications()
	{
		stage(); int checks = 0, notifications = 0; float sampled = -1;
		const auto connection = connect(clip.data(), &Clip::dataChanged, this, [&] {
			std::thread reader([&] { sampled = clip->valueAt(12); }); reader.join(); ++notifications;
		});
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		clip->commitRecordingTake([&] { float initial = -1; std::thread reader([&] { initial = clip->valueAt(0); }); reader.join(); QCOMPARE(initial, 10.f); ++checks; });
		QCOMPARE(checks, 2); QCOMPARE(notifications, 1); QCOMPARE(sampled, 25.f);
	}
	void independentNameAndMuteChangesAreRetained()
	{
		stage(); bool invoked = false; clip->commitRecordingTake([&] {
			if (invoked) { return; } invoked = true; clip->setName("Retained"); clip->setMuted(true);
		});
		QCOMPARE(clip->name(), QString("Retained")); QVERIFY(clip->isMuted()); QCOMPARE(clip->valueAt(12), 25.f);
	}
	void postPublicationNotificationFailureRetainsPublishedNativeData()
	{
		stage(); const auto connection = connect(clip.data(), &Clip::dataChanged, this, [] { throw std::runtime_error("fixture published notification"); });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error);
		QVERIFY(!clip->hasReservedRecording()); QCOMPARE(clip->valueAt(12), 25.f);
	}
	void finalNotificationCanReturnAfterReceiverRetirement()
	{
		stage(); const QPointer<AutomationClip> previous(clip);
		connect(clip.data(), &Clip::dataChanged, this, [&] { delete clip.data(); });
		QVERIFY_EXCEPTION_THROWN(clip->commitRecordingTake(), std::runtime_error); QVERIFY(!previous); QVERIFY(!clip);
	}
};
QTEST_GUILESS_MAIN(AutomationTakeCommitTest)
#include "AutomationTakeCommitTest.moc"
