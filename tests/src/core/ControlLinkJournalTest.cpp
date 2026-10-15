/* Native link/unlink edits share object-preserving user history.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "NativeControlLinkJournal.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"

using namespace lmms;
namespace
{
QJsonObject parameters(AutomatableModel* first, AutomatableModel* second) { return {{"parameter", static_cast<int>(first->id())}, {"peer", static_cast<int>(second->id())}}; }
QDomElement nativeState(AutomatableModel& first, QDomDocument& document)
{
	// Get native model lifetime/fitting metadata through its value serializer.
	QDomDocument values; auto root = values.createElement("journal"); values.appendChild(root); const auto saved = first.saveJournalState(values, root);
	auto state = document.createElement("nativeControlLinkJournal"); state.setAttribute("version", 1); state.setAttribute("receiver", first.id()); state.setAttribute("identity", saved.attribute("identity"));
	for (const auto tag : {"desired", "expected"})
	{
		auto section = document.createElement(tag);
		for (auto member = saved.firstChildElement(); !member.isNull(); member = member.nextSiblingElement())
		{
			auto copy = document.importNode(member, true).toElement(); const auto model = dynamic_cast<AutomatableModel*>(Engine::projectJournal()->journallingObject(member.attribute("id").toUInt()));
			copy.setAttribute("next", model->nextLinkedModel()->id()); section.appendChild(copy);
		}
		state.appendChild(section);
	}
	document.appendChild(state); return state;
}
}
class ControlLinkJournalTest : public QObject
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

	void mergeUndoRedoRestoresBothOrderedGroupsAndDistinctValues()
	{
		FloatModel a(10, 0, 60, 0), b(70, 0, 100, 0), c(90, 0, 1000, 0), d(80, 0, 1000, 0); QVERIFY(a.linkToModel(&b)); QVERIFY(c.linkToModel(&d));
		const auto journal = Engine::projectJournal(); const auto id = a.id(); journal->setJournalling(true); QString error; QVERIFY2(NativeControlLinkJournal::link(&a, &c, error), qPrintable(error));
		QCOMPARE(a.countLinks(), std::size_t{3}); QCOMPARE(a.storedValue(), 60.f); QCOMPARE(b.storedValue(), 70.f); QVERIFY(journal->canUndo()); journal->undo();
		QVERIFY(a.isLinkedToModel(&b)); QVERIFY(!a.isLinkedToModel(&c)); QVERIFY(c.isLinkedToModel(&d)); QCOMPARE(a.nextLinkedModel(), static_cast<const AutomatableModel*>(&b)); QCOMPARE(c.nextLinkedModel(), static_cast<const AutomatableModel*>(&d));
		QCOMPARE(a.storedValue(), 60.f); QCOMPARE(b.storedValue(), 70.f); QCOMPARE(c.storedValue(), 80.f); QCOMPARE(d.storedValue(), 80.f); QCOMPARE(a.id(), id); QVERIFY(!journal->canUndo()); QVERIFY(journal->canRedo());
		journal->redo(); QCOMPARE(a.countLinks(), std::size_t{3}); QCOMPARE(b.nextLinkedModel(), static_cast<const AutomatableModel*>(&c)); QVERIFY(!journal->canRedo()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void detachUndoRedoRetainsTheRemainingRingAndStoredValues_data()
	{ QTest::addColumn<bool>("pair"); QTest::newRow("two-controls") << true; QTest::newRow("three-controls") << false; }
	void detachUndoRedoRetainsTheRemainingRingAndStoredValues()
	{
		QFETCH(bool, pair); FloatModel a(10, 0, 60, 0), b(70, 0, 100, 0), c(90, 0, 1000, 0); QVERIFY(a.linkToModel(&b)); if (!pair) { QVERIFY(a.linkToModel(&c)); }
		auto journal = Engine::projectJournal(); journal->setJournalling(true); QString error; QVERIFY(NativeControlLinkJournal::unlink(&a, error)); QVERIFY(!a.isLinked()); QCOMPARE(b.isLinked(), !pair);
		journal->undo(); QVERIFY(a.isLinkedToModel(&b)); QCOMPARE(a.storedValue(), 60.f); QCOMPARE(b.storedValue(), 70.f); if (!pair) { QCOMPARE(b.nextLinkedModel(), static_cast<const AutomatableModel*>(&c)); }
		journal->redo(); QVERIFY(!a.isLinked()); QCOMPARE(b.isLinked(), !pair); QCOMPARE(a.storedValue(), 60.f); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void topologyAndValueEditsShareNativeUndoOrder()
	{
		FloatModel a(10, 0, 60, 0), b(70, 0, 100, 0); auto journal = Engine::projectJournal(); journal->setJournalling(true); QString error;
		QVERIFY(NativeControlLinkJournal::link(&a, &b, error)); a.setValue(20); QVERIFY(NativeControlLinkJournal::unlink(&a, error));
		journal->undo(); QVERIFY(a.isLinkedToModel(&b)); QCOMPARE(a.storedValue(), 20.f); journal->undo(); QCOMPARE(a.storedValue(), 60.f); QCOMPARE(b.storedValue(), 70.f);
		journal->undo(); QVERIFY(!a.isLinked()); QVERIFY(!b.isLinked()); QCOMPARE(a.storedValue(), 10.f); QCOMPARE(b.storedValue(), 70.f); QVERIFY(!journal->canUndo());
		journal->redo(); QVERIFY(a.isLinkedToModel(&b)); journal->redo(); QCOMPARE(a.storedValue(), 20.f); journal->redo(); QVERIFY(!a.isLinked()); QVERIFY(!journal->canRedo()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void repeatedEditsAndDisabledJournallingDoNotCreateExtraCheckpoints()
	{
		FloatModel a(10, 0, 100, 0), b(10, 0, 100, 0); auto journal = Engine::projectJournal(); QString error;
		QVERIFY(NativeControlLinkJournal::link(&a, &b, error)); QVERIFY(!journal->canUndo()); journal->setJournalling(true); QVERIFY(NativeControlLinkJournal::link(&a, &b, error)); QVERIFY(!journal->canUndo());
		QVERIFY(NativeControlLinkJournal::unlink(&a, error)); QVERIFY(NativeControlLinkJournal::unlink(&a, error)); journal->undo(); QVERIFY(a.isLinkedToModel(&b)); QVERIFY(!journal->canUndo());
		journal->clearJournal(); a.setJournalling(false); QVERIFY(NativeControlLinkJournal::unlink(&a, error)); QVERIFY(!journal->canUndo()); a.setJournalling(true);
	}

	void untrackedMembershipOrFittingChangesRefuseHistory_data()
	{
		QTest::addColumn<QString>("change"); QTest::newRow("extra-peer") << QStringLiteral("extra-peer"); QTest::newRow("detach") << QStringLiteral("detach");
		QTest::newRow("range") << QStringLiteral("range"); QTest::newRow("retirement") << QStringLiteral("retirement");
	}
	void untrackedMembershipOrFittingChangesRefuseHistory()
	{
		QFETCH(QString, change); FloatModel a(10, 0, 100, 0), extra(70, 0, 100, 0); auto b = std::make_unique<FloatModel>(70, 0, 100, 0); QString error;
		auto journal = Engine::projectJournal(); journal->setJournalling(true); QVERIFY(NativeControlLinkJournal::link(&a, b.get(), error)); journal->setJournalling(false);
		if (change == "extra-peer") { QVERIFY(a.linkToModel(&extra)); } else if (change == "detach") { b->unlink(); } else if (change == "range") { b->setRange(0, 200, 0); } else { b.reset(); }
		journal->setJournalling(true); const auto before = a.countLinks(); journal->undo(); QCOMPARE(a.countLinks(), before); QCOMPARE(a.storedValue(), 70.f); QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo()); QVERIFY(journal->isJournalling()); QVERIFY(Engine::getSong()->hasErrors());
	}

	void restoredEdgesCannotIntroduceNewControllerFeedback()
	{
		auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller); auto child = controller->findChildren<AutomatableModel*>().first();
		FloatModel a(10, 0, 100, 0); QVERIFY(a.linkToModel(child)); auto journal = Engine::projectJournal(); journal->setJournalling(true); QString error; QVERIFY(NativeControlLinkJournal::unlink(&a, error));
		a.setControllerConnection(new ControllerConnection(controller)); journal->undo(); QVERIFY(!a.isLinkedToModel(child)); QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo()); QVERIFY(Engine::getSong()->hasErrors());
	}

	void notificationExceptionsRollbackTopologyAndValuesThenPermitRetry_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void notificationExceptionsRollbackTopologyAndValuesThenPermitRetry()
	{
		QFETCH(bool, standard); FloatModel a(10, 0, 100, 0), b(70, 0, 100, 0); auto journal = Engine::projectJournal(); journal->setJournalling(true); QString error; QVERIFY(NativeControlLinkJournal::link(&a, &b, error));
		ModelValueNotificationProbe fault(&a, [&] { if (standard) { throw std::runtime_error("fixture topology undo"); } throw 42; });
		journal->undo(); QVERIFY(a.isLinkedToModel(&b)); QCOMPARE(a.storedValue(), 70.f); QCOMPARE(b.storedValue(), 70.f); QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo()); QVERIFY(journal->isJournalling());
		fault.reset(); Engine::getSong()->clearErrors(); journal->undo(); QVERIFY(!a.isLinked()); QVERIFY(!b.isLinked()); QCOMPARE(a.storedValue(), 10.f); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void callbackRetirementOrExternalJoinRestoresSurvivingPriorEdges_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("retired-peer") << 0; QTest::newRow("external-join") << 1; QTest::newRow("retired-receiver") << 2; }
	void callbackRetirementOrExternalJoinRestoresSurvivingPriorEdges()
	{
		QFETCH(int, action); FloatModel outside(30, 0, 100, 0); auto a = std::make_unique<FloatModel>(10, 0, 100, 0), b = std::make_unique<FloatModel>(70, 0, 100, 0); QString error;
		auto journal = Engine::projectJournal(); journal->setJournalling(true); QVERIFY(NativeControlLinkJournal::link(a.get(), b.get(), error)); bool done = false;
		QObject::connect(a.get(), &AutomatableModel::dataChanged, this, [&] { if (done) { return; } done = true; if (action == 0) { b.reset(); } else if (action == 2) { a.reset(); } else { QVERIFY(a->linkToModel(&outside)); } }, Qt::DirectConnection);
		journal->undo(); QVERIFY(done); if (a) { QCOMPARE(a->storedValue(), 70.f); QVERIFY(!a->isLinkedToModel(&outside)); if (b) { QVERIFY(a->isLinkedToModel(b.get())); } else { QVERIFY(!a->isLinked()); } } if (b) { QCOMPARE(b->storedValue(), 70.f); }
		QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo()); QVERIFY(Engine::getSong()->hasErrors());
	}

	void changedBindingDuringFailureDoesNotRebuildAFeedbackRing()
	{
		auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller); auto child = controller->findChildren<AutomatableModel*>().first();
		FloatModel a(10, 0, 100, 0); auto journal = Engine::projectJournal(); journal->setJournalling(true); QString error; QVERIFY(NativeControlLinkJournal::link(&a, child, error)); const auto copied = a.storedValue(); bool done = false;
		ModelValueNotificationProbe fault(&a, [&] { if (done) { return; } done = true; a.setControllerConnection(new ControllerConnection(controller)); throw std::runtime_error("fixture binding changed during undo"); });
		journal->undo(); QVERIFY(done); QVERIFY(!a.isLinkedToModel(child)); QCOMPARE(a.storedValue(), copied); QVERIFY(a.controllerConnection()); QCOMPARE(a.controllerConnection()->getController(), static_cast<Controller*>(controller));
		QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo()); QVERIFY(journal->isJournalling()); QVERIFY(Engine::getSong()->hasErrors());
	}

	void malformedNativeStatesRefuseBeforeMutation_data()
	{
		QTest::addColumn<QString>("damage"); QTest::newRow("version") << QStringLiteral("version"); QTest::newRow("receiver") << QStringLiteral("receiver");
		QTest::newRow("sections") << QStringLiteral("sections"); QTest::newRow("lifetime") << QStringLiteral("lifetime"); QTest::newRow("value") << QStringLiteral("value");
		QTest::newRow("missing-model") << QStringLiteral("missing-model"); QTest::newRow("missing-edge") << QStringLiteral("missing-edge"); QTest::newRow("duplicate-edge") << QStringLiteral("duplicate-edge");
		QTest::newRow("different-members") << QStringLiteral("different-members"); QTest::newRow("unknown-node") << QStringLiteral("unknown-node");
	}
	void malformedNativeStatesRefuseBeforeMutation()
	{
		QFETCH(QString, damage); FloatModel a(10, 0, 60, 0), b(70, 0, 100, 0); QVERIFY(a.linkToModel(&b)); QDomDocument document; auto state = nativeState(a, document); auto desired = state.firstChildElement(), expected = desired.nextSiblingElement(); auto first = desired.firstChildElement();
		if (damage == "version") { state.setAttribute("version", 99); } else if (damage == "receiver") { state.setAttribute("identity", 0); }
		else if (damage == "sections") { state.appendChild(document.createElement("expected")); } else if (damage == "lifetime") { first.setAttribute("identity", 0); }
		else if (damage == "value") { first.setAttribute("value", "nan"); } else if (damage == "missing-model") { desired.removeChild(desired.lastChild()); }
		else if (damage == "missing-edge") { first.setAttribute("next", 0); } else if (damage == "duplicate-edge") { first.setAttribute("next", a.id()); }
		else if (damage == "different-members") { expected.removeChild(expected.lastChild()); } else { first.setTagName("unexpected"); }
		QString error; QVERIFY(!NativeControlLinkJournal::restore(&a, state, error)); QVERIFY(!error.isEmpty()); QVERIFY(a.isLinkedToModel(&b)); QCOMPARE(a.storedValue(), 60.f); QCOMPARE(b.storedValue(), 70.f);
	}

	void counterpartRoundTripAndCancellationPreserveNativeState()
	{
		FloatModel a(10, 0, 60, 0), b(70, 0, 100, 0); QVERIFY(a.linkToModel(&b)); QDomDocument document; auto state = nativeState(a, document); QString error;
		QDomDocument other; auto parent = other.createElement("journal"); other.appendChild(parent); auto inverse = NativeControlLinkJournal::counterpart(&a, state, other, parent, error); QVERIFY(!inverse.isNull()); QVERIFY(NativeControlLinkJournal::restore(&a, inverse, error));
		bool timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); FileResourceIO::Scope cancellation([] { return true; }, true);
		QVERIFY(!NativeControlLinkJournal::restore(&a, state, error)); QVERIFY(!timer); QVERIFY(a.isLinkedToModel(&b)); QCOMPARE(a.storedValue(), 60.f);
	}

	void structuredNativeEditsUseTheSameObjectPreservingJournal()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto first = track->volumeModel(), second = track->panningModel(); second->setValue(25); const auto previous = first->storedValue();
		auto journal = Engine::projectJournal(); journal->clearJournal(); journal->setJournalling(true); const auto result = StudioCapabilities::execute("parameter.link", parameters(first, second)); QVERIFY(result.value("ok").toBool());
		journal->undo(); QVERIFY(!first->isLinked()); QCOMPARE(first->storedValue(), previous); QCOMPARE(Engine::getSong()->tracks().front(), static_cast<Track*>(track)); journal->redo(); QVERIFY(first->isLinkedToModel(second));
		QVERIFY(StudioCapabilities::execute("parameter.unlink", {{"parameter", static_cast<int>(first->id())}}).value("ok").toBool()); journal->undo(); QVERIFY(first->isLinkedToModel(second)); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void largeRingDetachAndDestructorUseDifferentHistoryPaths()
	{
		FloatModel root(10, 0, 100, 0); std::vector<std::unique_ptr<FloatModel>> peers;
		for (int index = 0; index < 1050; ++index) { peers.push_back(std::make_unique<FloatModel>(10, 0, 100, 0)); QVERIFY(root.linkToModel(peers.back().get())); }
		auto journal = Engine::projectJournal(); journal->setJournalling(true); QString error; QVERIFY(NativeControlLinkJournal::unlink(&root, error)); journal->undo(); QCOMPARE(root.countLinks(), std::size_t{1050}); journal->redo(); QVERIFY(!root.isLinked());
		journal->clearJournal(); peers.pop_back(); QVERIFY(!journal->canUndo()); QVERIFY(!Engine::getSong()->hasErrors());
	}
};
QTEST_GUILESS_MAIN(ControlLinkJournalTest)
#include "ControlLinkJournalTest.moc"
