/* Native graph transitions preserve unselected rings and rejected old edges.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <vector>
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "LfoController.h"
#include "NativeLinkedControls.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"

using namespace lmms;
namespace
{
bool join(const QList<AutomatableModel*>& models)
{ for (int index = 1; index < models.size(); ++index) { if (!models.front()->linkToModel(models.at(index))) { return false; } } return true; }
struct State
{
	QDomDocument document; QDomElement root;
	bool save(const QList<AutomatableModel*>& models, QString& error)
	{
		root = document.createElement("controls"); document.appendChild(root); NativeLinkedControls::SaveScope scope(document, models);
		for (int index = 0; index < models.size(); ++index) { models.at(index)->saveSettings(document, root, "m" + QString::number(index)); }
		return scope.finish(root, error);
	}
	void load(const QList<AutomatableModel*>& models)
	{ for (int index = 0; index < models.size(); ++index) { models.at(index)->loadSettings(root, "m" + QString::number(index)); } }
};
}
class NativeGraphTransitionTest : public QObject
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
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::getSong()->clearProject(); }

	void selectedPartitionsPreserveRemainingPeerOrderAndValues_data()
	{
		QTest::addColumn<int>("layout"); QTest::newRow("interleaved") << 0; QTest::newRow("contiguous") << 1;
		QTest::newRow("two-old-rings") << 2; QTest::newRow("all-selected-plus-unrelated-ring") << 3;
	}
	void selectedPartitionsPreserveRemainingPeerOrderAndValues()
	{
		QFETCH(int, layout); FloatModel a(10, 0, 60, 0), b(70, 0, 100, 0), c(90, 0, 1000, 0), d(80, 0, 1000, 0), x(90, 0, 100, 0), y(80, 0, 100, 0);
		QVERIFY(join({&a, &b})); QVERIFY(join({&c, &d})); const QList<AutomatableModel*> selected{&a, &b, &c, &d}, all{&a, &b, &c, &d, &x, &y}; State state; QString error; QVERIFY(state.save(selected, error));
		JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope restore(state.document); state.load(selected); for (auto model : all) { model->unlink(); }
		if (layout == 0) { QVERIFY(join({&a, &x, &c, &y, &b, &d})); } else if (layout == 1) { QVERIFY(join({&a, &b, &c, &d, &x, &y})); }
		else if (layout == 2) { QVERIFY(join({&a, &x, &b})); QVERIFY(join({&c, &y, &d})); } else { QVERIFY(join({&a, &b, &c, &d})); QVERIFY(join({&x, &y})); }
		std::vector<float> values; std::vector<std::unique_ptr<QSignalSpy>> notifications; for (auto model : all) { values.push_back(model->storedValue()); notifications.push_back(std::make_unique<QSignalSpy>(model, &AutomatableModel::dataChanged)); }
		QVERIFY2(restore.finish(state.root, error), qPrintable(error)); QCOMPARE(a.nextLinkedModel(), static_cast<const AutomatableModel*>(&b)); QCOMPARE(b.nextLinkedModel(), static_cast<const AutomatableModel*>(&a));
		QCOMPARE(c.nextLinkedModel(), static_cast<const AutomatableModel*>(&d)); QCOMPARE(d.nextLinkedModel(), static_cast<const AutomatableModel*>(&c));
		if (layout == 2) { QVERIFY(!x.isLinked()); QVERIFY(!y.isLinked()); } else { QCOMPARE(x.nextLinkedModel(), static_cast<const AutomatableModel*>(&y)); QCOMPARE(y.nextLinkedModel(), static_cast<const AutomatableModel*>(&x)); }
		for (int index = 0; index < all.size(); ++index) { QCOMPARE(all.at(index)->storedValue(), values[index]); QCOMPARE(notifications[index]->size(), 0); } QVERIFY(!Engine::getSong()->hasErrors());
	}

	void feedbackRejectionRestoresEveryOldRingIncludingOutsidePeers()
	{
		auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller); auto child = controller->findChildren<AutomatableModel*>().first();
		FloatModel a(10, 0, 100, 0), x(90, 0, 100, 0), y(80, 0, 100, 0), z(50, 0, 100, 0); QVERIFY(a.linkToModel(child)); State state; QString error; const QList<AutomatableModel*> selected{&a, child}; QVERIFY(state.save(selected, error));
		JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope restore(state.document); state.load(selected); a.unlink(); child->unlink(); QVERIFY(join({&a, &x, &y})); QVERIFY(join({child, &z})); a.setControllerConnection(new ControllerConnection(controller));
		const QList<AutomatableModel*> all{&a, &x, &y, child, &z}; std::vector<const AutomatableModel*> edges; std::vector<float> values;
		for (auto model : all) { edges.push_back(model->nextLinkedModel()); values.push_back(model->storedValue()); }
		QVERIFY(!restore.finish(state.root, error)); QVERIFY(error.contains("feedback"));
		for (int index = 0; index < all.size(); ++index) { QCOMPARE(all.at(index)->nextLinkedModel(), edges[index]); QCOMPARE(all.at(index)->storedValue(), values[index]); }
		QVERIFY(a.controllerConnection()); QCOMPARE(a.controllerConnection()->getController(), static_cast<Controller*>(controller));
	}

	void lateAdmissionUsesWeakEdgesAndCurrentBindings_data()
	{
		QTest::addColumn<int>("action"); QTest::newRow("cancel") << 0; QTest::newRow("retire-outside-peer") << 1;
		QTest::newRow("detach-outside-peer") << 2; QTest::newRow("rewire-controller") << 3;
	}
	void lateAdmissionUsesWeakEdgesAndCurrentBindings()
	{
		QFETCH(int, action); auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller); auto child = controller->findChildren<AutomatableModel*>().first();
		FloatModel a(10, 0, 100, 0), y(80, 0, 100, 0); auto x = std::make_unique<FloatModel>(90, 0, 100, 0); QVERIFY(a.linkToModel(child)); State state; QString error; const QList<AutomatableModel*> selected{&a, child}; QVERIFY(state.save(selected, error));
		JournallingObject::KeepIdentityScope identity; int checks = 0;
		{
			NativeLinkedControls::RestoreScope baseline(state.document); state.load(selected); FileResourceIO::Scope count([&] { ++checks; return false; }); QVERIFY(baseline.finish(state.root, error));
		}
		QVERIFY(checks >= 2); NativeLinkedControls::RestoreScope restore(state.document); state.load(selected); a.unlink(); child->unlink();
		if (action == 3) { QVERIFY(join({&a, x.get(), &y})); } else { QVERIFY(join({&a, x.get(), &y, child})); }
		int observed = 0; bool changed = false, timer = false; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		FileResourceIO::Scope admission([&] {
			// Probe the last cooperative check of a successful native finish, then
			// inject at that boundary without assuming an XML node/check count.
			if (++observed != checks - 1) { return false; } changed = true;
			if (action == 0) { return true; } if (action == 1) { x.reset(); } else if (action == 2) { x->unlink(); } else { a.setControllerConnection(new ControllerConnection(controller)); } return false;
		}, true);
		QVERIFY(!restore.finish(state.root, error)); QVERIFY(changed); QVERIFY(!timer); QVERIFY(!error.isEmpty());
		if (action == 0) { QCOMPARE(a.nextLinkedModel(), static_cast<const AutomatableModel*>(x.get())); }
		else if (action == 1 || action == 2) { QCOMPARE(a.nextLinkedModel(), static_cast<const AutomatableModel*>(&y)); }
		else { QVERIFY(error.contains("feedback")); QCOMPARE(a.nextLinkedModel(), static_cast<const AutomatableModel*>(x.get())); QVERIFY(!child->isLinked()); }
	}

	void largeNativeRestoreRepartitionsWithoutScalarPropagation_data()
	{ QTest::addColumn<int>("count"); QTest::newRow("1050-controls") << 1050; QTest::newRow("4096-controls") << 4096; }
	void largeNativeRestoreRepartitionsWithoutScalarPropagation()
	{
		QFETCH(int, count); std::vector<std::unique_ptr<FloatModel>> owned; QList<AutomatableModel*> selected;
		for (int index = 0; index < count; ++index) { owned.push_back(std::make_unique<FloatModel>(70, 0, 100, 0)); selected.append(owned.back().get()); }
		QVERIFY(join(selected)); State state; QString error; QVERIFY(state.save(selected, error)); JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope restore(state.document); state.load(selected);
		QList<AutomatableModel*> even, odd; for (int index = 0; index < count; ++index) { (index % 2 ? odd : even).append(selected.at(index)); } QVERIFY(join(even)); QVERIFY(join(odd));
		QVERIFY2(restore.finish(state.root, error), qPrintable(error));
		for (int index = 0; index < count; ++index) { QCOMPARE(selected.at(index)->nextLinkedModel(), static_cast<const AutomatableModel*>(selected.at((index + 1) % count))); QCOMPARE(selected.at(index)->storedValue(), 70.f); }
		QVERIFY(!Engine::getSong()->hasErrors());
	}

	void sharedAndLateControllerBindingsRemainEffective_data()
	{ QTest::addColumn<bool>("shared"); QTest::newRow("same-controller-on-two-peers") << true; QTest::newRow("last-peer-only") << false; }
	void sharedAndLateControllerBindingsRemainEffective()
	{
		QFETCH(bool, shared); auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller);
		FloatModel a(42, 0, 100, 0), b(42, 0, 100, 0), c(42, 0, 100, 0); QVERIFY(join({&a, &b, &c})); if (shared) { a.setControllerConnection(new ControllerConnection(controller)); } c.setControllerConnection(new ControllerConnection(controller));
		State state; QString error; const QList<AutomatableModel*> selected{&a, &b, &c}; QVERIFY(state.save(selected, error)); JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope restore(state.document); state.load(selected);
		QVERIFY2(restore.finish(state.root, error), qPrintable(error)); QCOMPARE(c.controllerConnection()->getController(), static_cast<Controller*>(controller)); if (shared) { QCOMPARE(a.controllerConnection()->getController(), static_cast<Controller*>(controller)); }
		QCOMPARE(a.value(), c.value()); QCOMPARE(b.value(), c.value());
	}

	void explicitEmptyGraphPreservesUnreferencedNativeRings()
	{
		FloatModel a(42, 0, 100, 0), b(42, 0, 100, 0); QVERIFY(a.linkToModel(&b)); QDomDocument document; auto root = document.createElement("controls"); document.appendChild(root); auto graph = document.createElement("nativeLinkedControls"); graph.setAttribute("version", 1); root.appendChild(graph);
		NativeLinkedControls::RestoreScope restore(document); QString error; QVERIFY(restore.finish(root, error)); QVERIFY(a.isLinkedToModel(&b)); QCOMPARE(a.storedValue(), 42.f); QCOMPARE(b.storedValue(), 42.f);
	}

	void distinctSavedReferencesCannotAliasOneNativeReceiver()
	{
		FloatModel a(42, 0, 100, 0), b(42, 0, 100, 0); QVERIFY(a.linkToModel(&b)); State state; QString error; QVERIFY(state.save({&a, &b}, error));
		JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope restore(state.document); a.loadSettings(state.root, "m0"); a.loadSettings(state.root, "m1");
		QVERIFY(!restore.finish(state.root, error)); QVERIFY(error.contains("multiple transition positions")); QVERIFY(!a.isLinked()); QVERIFY(!b.isLinked()); QCOMPARE(a.storedValue(), 42.f); QCOMPARE(b.storedValue(), 42.f);
	}
};
QTEST_GUILESS_MAIN(NativeGraphTransitionTest)
#include "NativeGraphTransitionTest.moc"
