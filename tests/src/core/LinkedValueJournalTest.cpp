/* Native linked values retain each peer's distinct pre-edit state in undo.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"

using namespace lmms;
namespace
{
class ReidentifiedModel : public FloatModel
{
public:
	using FloatModel::FloatModel;
	using JournallingObject::changeID;
};
QDomElement snapshot(AutomatableModel& model, QDomDocument& document)
{
	auto root = document.createElement("journal"); document.appendChild(root); return model.saveJournalState(document, root);
}
struct Group
{
	FloatModel first{10, 0, 60, 0}, second{70, 0, 100, 0}, third{90, 0, 1000, 0};
	Group() { first.linkToModel(&second); first.linkToModel(&third); }
};
}
class LinkedValueJournalTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->clearJournal(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::projectJournal()->setJournalling(false); Engine::projectJournal()->clearJournal(); Engine::getSong()->clearProject(); }

	void undoRedoRetainsDistinctRawValuesAndNativeObjectIdentity()
	{
		Group group; QCOMPARE(group.first.storedValue(), 60.f); QCOMPARE(group.second.storedValue(), 70.f); QCOMPARE(group.third.storedValue(), 90.f);
		const auto journal = Engine::projectJournal(); const QPointer<AutomatableModel> first(&group.first), second(&group.second), third(&group.third); const auto id = group.first.id(); journal->setJournalling(true);
		group.first.setValue(20); QCOMPARE(group.second.storedValue(), 20.f); QVERIFY(journal->canUndo()); journal->undo();
		QCOMPARE(group.first.storedValue(), 60.f); QCOMPARE(group.second.storedValue(), 70.f); QCOMPARE(group.third.storedValue(), 90.f); QVERIFY(!journal->canUndo()); QVERIFY(journal->canRedo());
		QCOMPARE(first.data(), &group.first); QCOMPARE(second.data(), &group.second); QCOMPARE(third.data(), &group.third); QCOMPARE(group.first.id(), id); QCOMPARE(group.first.initValue(), 10.f);
		journal->redo(); QCOMPARE(group.first.storedValue(), 20.f); QCOMPARE(group.second.storedValue(), 20.f); QCOMPARE(group.third.storedValue(), 20.f); QVERIFY(!journal->canRedo()); QVERIFY(journal->isJournalling()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void completeVectorIsPublishedBeforeEachNotification()
	{
		Group group; auto journal = Engine::projectJournal(); journal->setJournalling(true); group.first.setValue(20); int notifications = 0;
		for (auto model : {&group.first, &group.second, &group.third})
		{
			QObject::connect(model, &AutomatableModel::dataChanged, this, [&] { ++notifications; QCOMPARE(group.first.storedValue(), 60.f); QCOMPARE(group.second.storedValue(), 70.f); QCOMPARE(group.third.storedValue(), 90.f); }, Qt::DirectConnection);
		}
		journal->undo(); QCOMPARE(notifications, 3); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void samePeriodInterpolationIsRebuiltForUndoAndRedo()
	{
		Group group; const auto journal = Engine::projectJournal(); journal->setJournalling(true); group.first.setValue(20);
		auto first = group.first.valueBuffer(); auto second = group.second.valueBuffer(); QVERIFY(first); QVERIFY(second); QCOMPARE(first->values()[0], 60.f); QCOMPARE(second->values()[0], 70.f);
		journal->undo(); first = group.first.valueBuffer(); second = group.second.valueBuffer(); QVERIFY(first); QVERIFY(second); QCOMPARE(first->values()[0], 20.f); QCOMPARE(second->values()[0], 20.f);
		journal->redo(); first = group.first.valueBuffer(); second = group.second.valueBuffer(); QVERIFY(first); QVERIFY(second); QCOMPARE(first->values()[0], 60.f); QCOMPARE(second->values()[0], 70.f); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void changedMembershipOrFittingRefusesBeforeChangingValues_data()
	{
		QTest::addColumn<QString>("change"); QTest::newRow("detach") << QStringLiteral("detach"); QTest::newRow("join") << QStringLiteral("join"); QTest::newRow("reorder") << QStringLiteral("reorder");
		QTest::newRow("range") << QStringLiteral("range"); QTest::newRow("step") << QStringLiteral("step"); QTest::newRow("scale") << QStringLiteral("scale"); QTest::newRow("strict") << QStringLiteral("strict");
	}
	void changedMembershipOrFittingRefusesBeforeChangingValues()
	{
		QFETCH(QString, change); Group group; FloatModel extra(20, 0, 100, 0); auto journal = Engine::projectJournal(); journal->setJournalling(true); group.first.setValue(20); journal->setJournalling(false);
		if (change == "detach") { group.second.unlink(); } else if (change == "join") { QVERIFY(group.first.linkToModel(&extra)); }
		else if (change == "reorder") { group.second.unlink(); QVERIFY(group.first.linkToModel(&group.second)); }
		else if (change == "range") { group.second.setRange(0, 200, 0); } else if (change == "step") { group.second.setStep(1); }
		else if (change == "scale") { group.second.setScaleType(AutomatableModel::ScaleType::Logarithmic); } else { group.second.setStrictStepSize(true); }
		journal->setJournalling(true); journal->undo(); QCOMPARE(group.first.storedValue(), 20.f); QCOMPARE(group.second.storedValue(), 20.f); QCOMPARE(group.third.storedValue(), 20.f);
		QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo()); QVERIFY(journal->isJournalling());
	}

	void invalidSnapshotsRefuseBeforeMutation_data()
	{
		QTest::addColumn<QString>("invalid");
		QTest::newRow("version") << QStringLiteral("version"); QTest::newRow("receiver-id") << QStringLiteral("receiver-id"); QTest::newRow("receiver-life") << QStringLiteral("receiver-life");
		QTest::newRow("peer-id") << QStringLiteral("peer-id"); QTest::newRow("peer-life") << QStringLiteral("peer-life"); QTest::newRow("nan") << QStringLiteral("nan"); QTest::newRow("infinity") << QStringLiteral("infinity");
		QTest::newRow("out-of-range") << QStringLiteral("out-of-range"); QTest::newRow("missing-member") << QStringLiteral("missing-member"); QTest::newRow("extra-member") << QStringLiteral("extra-member");
		QTest::newRow("nested-member") << QStringLiteral("nested-member"); QTest::newRow("unknown-tag") << QStringLiteral("unknown-tag"); QTest::newRow("missing-value") << QStringLiteral("missing-value");
	}
	void invalidSnapshotsRefuseBeforeMutation()
	{
		QFETCH(QString, invalid); Group group; QDomDocument document; auto state = snapshot(group.first, document); QVERIFY(!state.isNull()); group.first.setValue(20); auto peer = state.firstChildElement().nextSiblingElement();
		if (invalid == "version") { state.setAttribute("version", 99); } else if (invalid == "receiver-id") { state.setAttribute("receiver", group.second.id()); }
		else if (invalid == "receiver-life") { state.setAttribute("identity", "0"); } else if (invalid == "peer-id") { peer.setAttribute("id", 0); }
		else if (invalid == "peer-life") { peer.setAttribute("identity", "0"); } else if (invalid == "nan") { peer.setAttribute("value", "nan"); }
		else if (invalid == "infinity") { peer.setAttribute("value", "inf"); } else if (invalid == "out-of-range") { peer.setAttribute("value", 9999); }
		else if (invalid == "missing-member") { state.removeChild(state.lastChild()); } else if (invalid == "extra-member") { state.appendChild(peer.cloneNode()); }
		else if (invalid == "nested-member") { peer.appendChild(document.createElement("unexpected")); } else if (invalid == "unknown-tag") { peer.setTagName("unexpected"); }
		else { peer.removeAttribute("value"); }
		QString error; QVERIFY(!group.first.restoreJournalState(state, error)); QVERIFY(!error.isEmpty()); QCOMPARE(group.first.storedValue(), 20.f); QCOMPARE(group.second.storedValue(), 20.f); QCOMPARE(group.third.storedValue(), 20.f);
	}

	void reusedAddressAndNativeIdCannotImpersonateRetiredPeer()
	{
		FloatModel first(10, 0, 60, 0); alignas(ReidentifiedModel) unsigned char storage[sizeof(ReidentifiedModel)];
		auto peer = std::construct_at(reinterpret_cast<ReidentifiedModel*>(storage), 90, 0, 100, 0); const auto cleanup = qScopeGuard([&] { if (peer) { std::destroy_at(peer); } }); QVERIFY(first.linkToModel(peer));
		QDomDocument document; auto state = snapshot(first, document); const auto oldId = peer->id(); const QPointer<AutomatableModel> old(peer); std::destroy_at(peer); peer = nullptr;
		peer = std::construct_at(reinterpret_cast<ReidentifiedModel*>(storage), 20, 0, 100, 0); peer->changeID(oldId); QVERIFY(first.linkToModel(peer)); QVERIFY(!old); QCOMPARE(peer->id(), oldId);
		QString error; QVERIFY(!first.restoreJournalState(state, error)); QCOMPARE(first.storedValue(), 20.f); QCOMPARE(peer->storedValue(), 20.f);
	}

	void returnedCallbackExceptionsRollbackNativeValuesAndPermitRetry_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void returnedCallbackExceptionsRollbackNativeValuesAndPermitRetry()
	{
		QFETCH(bool, standard); Group group; auto journal = Engine::projectJournal(); journal->setJournalling(true); group.first.setValue(20);
		ModelValueNotificationProbe fault(&group.second, [&] { if (standard) { throw std::runtime_error("fixture journal notification"); } throw 42; });
		journal->undo(); QCOMPARE(group.first.storedValue(), 20.f); QCOMPARE(group.second.storedValue(), 20.f); QCOMPARE(group.third.storedValue(), 20.f); QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo()); QVERIFY(journal->isJournalling()); QVERIFY(Engine::getSong()->hasErrors());
		fault.reset(); Engine::getSong()->clearErrors(); journal->undo(); QCOMPARE(group.first.storedValue(), 60.f); QCOMPARE(group.second.storedValue(), 70.f); QCOMPARE(group.third.storedValue(), 90.f); QVERIFY(journal->canRedo()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void callbackRetirementOrMutationRejectsAndRestoresSurvivingValues_data()
	{
		QTest::addColumn<QString>("action"); QTest::newRow("receiver") << QStringLiteral("receiver"); QTest::newRow("peer") << QStringLiteral("peer"); QTest::newRow("detach") << QStringLiteral("detach"); QTest::newRow("value") << QStringLiteral("value");
	}
	void callbackRetirementOrMutationRejectsAndRestoresSurvivingValues()
	{
		QFETCH(QString, action); auto first = std::make_unique<FloatModel>(10, 0, 60, 0), second = std::make_unique<FloatModel>(90, 0, 100, 0), third = std::make_unique<FloatModel>(90, 0, 1000, 0);
		QVERIFY(first->linkToModel(second.get())); QVERIFY(first->linkToModel(third.get())); QDomDocument document; auto state = snapshot(*first, document); first->setValue(20); bool done = false;
		QObject::connect(second.get(), &AutomatableModel::dataChanged, this, [&] { if (done) { return; } done = true; if (action == "receiver") { first.reset(); } else if (action == "peer") { third.reset(); } else if (action == "detach") { third->unlink(); } else { third->setValue(30); } }, Qt::DirectConnection);
		QString error; QVERIFY(!first->restoreJournalState(state, error)); QVERIFY(done); QVERIFY(!error.isEmpty()); if (first) { QCOMPARE(first->storedValue(), 20.f); } QCOMPARE(second->storedValue(), 20.f); if (third) { QCOMPARE(third->storedValue(), 20.f); }
	}

	void nestedDirectRestoreCannotEnterTheSameGroup()
	{
		Group group; QDomDocument document; auto state = snapshot(group.first, document); group.first.setValue(20); bool nested = false;
		QObject::connect(&group.first, &AutomatableModel::dataChanged, this, [&] { QString error; QVERIFY(!group.first.restoreJournalState(state, error)); QVERIFY(!error.isEmpty()); nested = true; }, Qt::DirectConnection);
		QString error; QVERIFY(group.first.restoreJournalState(state, error)); QVERIFY(nested); QCOMPARE(group.second.storedValue(), 70.f);
	}

	void cancellationRefusesBeforeCommitWithoutDispatchingEvents()
	{
		Group group; QDomDocument document; auto state = snapshot(group.first, document); group.first.setValue(20); bool timer = false; QObject owner;
		QTimer::singleShot(0, &owner, [&] { timer = true; }); FileResourceIO::Scope cancellation([] { return true; }, true); QString error;
		QVERIFY(!group.first.restoreJournalState(state, error)); QVERIFY(!timer); QCOMPARE(group.first.storedValue(), 20.f); QCOMPARE(group.second.storedValue(), 20.f); QCOMPARE(group.third.storedValue(), 20.f);
	}

	void floatPrecisionAndCurrentControllerBindingSurviveUndo()
	{
		FloatModel first(10, 0, 60, 0), second(std::nextafter(1.f, 2.f), 0, 100, 0); QVERIFY(first.linkToModel(&second)); const auto exact = second.storedValue();
		auto controller = new MidiController(nullptr); second.setControllerConnection(new ControllerConnection(controller)); const auto connection = second.controllerConnection();
		const auto journal = Engine::projectJournal(); journal->setJournalling(true); first.setValue(20); journal->undo(); QCOMPARE(first.storedValue(), exact); QCOMPARE(second.storedValue(), exact); QCOMPARE(second.controllerConnection(), connection); QCOMPARE(connection->getController(), controller); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void unlinkedNativeJournalRemainsAvailableAndNewLinksRefuseScalarHistory()
	{
		FloatModel first(10, 0, 100, 0), second(30, 0, 100, 0); auto journal = Engine::projectJournal(); journal->setJournalling(true); first.setValue(20); journal->undo(); QCOMPARE(first.storedValue(), 10.f); journal->redo(); QCOMPARE(first.storedValue(), 20.f);
		journal->setJournalling(false); QVERIFY(first.linkToModel(&second)); journal->setJournalling(true); journal->undo(); QCOMPARE(first.storedValue(), 30.f); QCOMPARE(second.storedValue(), 30.f); QVERIFY(journal->canUndo()); QVERIFY(Engine::getSong()->hasErrors());
	}

	void realTrackModelsAndLargeGroupsKeepEditableOwners()
	{
		auto firstTrack = new InstrumentTrack(Engine::getSong()), secondTrack = new InstrumentTrack(Engine::getSong()); auto first = firstTrack->volumeModel(), second = secondTrack->volumeModel(); QVERIFY(first->linkToModel(second));
		const auto previous = first->storedValue(); auto journal = Engine::projectJournal(); journal->setJournalling(true); first->setValue(70); journal->undo(); QCOMPARE(first->storedValue(), previous); QCOMPARE(second->storedValue(), previous); QCOMPARE(Engine::getSong()->tracks().at(0), static_cast<Track*>(firstTrack));
		journal->setJournalling(false); FloatModel root(10, 0, 60, 0); std::vector<std::unique_ptr<FloatModel>> peers;
		for (int index = 0; index < 1050; ++index) { peers.push_back(std::make_unique<FloatModel>(90, 0, 100, 0)); QVERIFY(root.linkToModel(peers.back().get())); }
		journal->clearJournal(); journal->setJournalling(true); root.setValue(20); journal->undo(); QCOMPARE(root.storedValue(), 60.f); for (const auto& peer : peers) { QCOMPARE(peer->storedValue(), 90.f); } QVERIFY(!Engine::getSong()->hasErrors());
	}
};
QTEST_GUILESS_MAIN(LinkedValueJournalTest)
#include "LinkedValueJournalTest.moc"
