/* Native linked value writes under callback retirement and ring mutation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ModelValueNotificationTestUtils.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "NativeLinkedControls.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
std::unique_ptr<FloatModel> model(float value = 42) { return std::make_unique<FloatModel>(value, 0, 100, 0, nullptr); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}
class LinkedValueOwnershipTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void ordinaryAndAutomatedWritesKeepNativeFittingAndSignalCounts_data()
	{ QTest::addColumn<bool>("automated"); QTest::newRow("manual") << false; QTest::newRow("automated") << true; }
	void ordinaryAndAutomatedWritesKeepNativeFittingAndSignalCounts()
	{
		QFETCH(bool, automated); FloatModel first(42, 0, 100, 0), second(42, 40, 60, 0), third(42, 0, 100, 0); QVERIFY(first.linkToModel(&second)); QVERIFY(first.linkToModel(&third));
		QSignalSpy a(&first, &AutomatableModel::dataChanged), b(&second, &AutomatableModel::dataChanged), c(&third, &AutomatableModel::dataChanged);
		first.setValue(70, automated); QCOMPARE(first.storedValue(), 70.f); QCOMPARE(second.storedValue(), 60.f); QCOMPARE(third.storedValue(), 70.f); QCOMPARE(a.size(), 1); QCOMPARE(b.size(), 1); QCOMPARE(c.size(), 1);
	}

	void retiredCurrentNextLastOrReceiverNeverGetsDereferenced_data()
	{ QTest::addColumn<int>("retired"); QTest::newRow("current-peer") << 1; QTest::newRow("next-peer") << 2; QTest::newRow("last-peer") << 3; QTest::newRow("receiver") << 0; }
	void retiredCurrentNextLastOrReceiverNeverGetsDereferenced()
	{
		QFETCH(int, retired); auto a = model(), b = model(), c = model(), d = model(); QVERIFY(a->linkToModel(b.get())); QVERIFY(a->linkToModel(c.get())); QVERIFY(a->linkToModel(d.get()));
		const QPointer<AutomatableModel> selected(retired == 0 ? a.get() : retired == 1 ? b.get() : retired == 2 ? c.get() : d.get()); bool done = false;
		QObject::connect(b.get(), &AutomatableModel::dataChanged, this, [&] { if (done) { return; } done = true; if (retired == 0) { a.reset(); } else if (retired == 1) { b.reset(); } else if (retired == 2) { c.reset(); } else { d.reset(); } }, Qt::DirectConnection);
		a->setValue(70); QVERIFY(done); QVERIFY(!selected);
		if (retired == 0) { QCOMPARE(c->storedValue(), 42.f); QCOMPARE(d->storedValue(), 42.f); }
		else { QCOMPARE(a->storedValue(), 70.f); if (c) { QCOMPARE(c->storedValue(), 70.f); } if (d) { QCOMPARE(d->storedValue(), 70.f); } }
	}

	void detachingCurrentOrFuturePeersDoesNotDropTheRestOfTheWrite_data()
	{ QTest::addColumn<bool>("current"); QTest::newRow("current") << true; QTest::newRow("future") << false; }
	void detachingCurrentOrFuturePeersDoesNotDropTheRestOfTheWrite()
	{
		QFETCH(bool, current); auto a = model(), b = model(), c = model(), d = model(); QVERIFY(a->linkToModel(b.get())); QVERIFY(a->linkToModel(c.get())); QVERIFY(a->linkToModel(d.get())); bool done = false;
		QObject::connect(b.get(), &AutomatableModel::dataChanged, this, [&] { if (!done) { done = true; (current ? b.get() : c.get())->unlink(); } }, Qt::DirectConnection);
		a->setValue(70); QCOMPARE(d->storedValue(), 70.f); QCOMPARE(c->storedValue(), current ? 70.f : 42.f); QCOMPARE(b->storedValue(), 70.f); QVERIFY(!(current ? b.get() : c.get())->isLinked());
	}

	void initialReceiverCallbackCanJoinAGroupBeforePeerPropagation()
	{
		auto a = model(), b = model(50); bool done = false;
		QObject::connect(a.get(), &AutomatableModel::dataChanged, this, [&] { if (!done) { done = true; QVERIFY(a->linkToModel(b.get())); } }, Qt::DirectConnection);
		a->setValue(70); QVERIFY(a->isLinkedToModel(b.get())); QCOMPARE(a->storedValue(), 50.f); QCOMPARE(b->storedValue(), 70.f);
	}

	void callbackGroupJoinsRetainNativeNestedValueOrder()
	{
		auto a = model(), b = model(), c = model(), d = model(), e = model(50), f = model(50); QVERIFY(a->linkToModel(b.get())); QVERIFY(a->linkToModel(c.get())); QVERIFY(a->linkToModel(d.get())); QVERIFY(e->linkToModel(f.get())); bool done = false;
		QObject::connect(b.get(), &AutomatableModel::dataChanged, this, [&] { if (!done) { done = true; QVERIFY(a->linkToModel(e.get())); } }, Qt::DirectConnection);
		a->setValue(70); QCOMPARE(a->storedValue(), 50.f); QCOMPARE(b->storedValue(), 50.f); for (auto peer : {c.get(), d.get(), e.get(), f.get()}) { QCOMPARE(peer->storedValue(), 70.f); } QCOMPARE(a->countLinks(), std::size_t{5});
	}

	void reusedVisitedAddressReceivesTheRemainingOuterWrite()
	{
		auto a = model(), c = model(), d = model(); alignas(FloatModel) unsigned char storage[sizeof(FloatModel)]; auto b = std::construct_at(reinterpret_cast<FloatModel*>(storage), 42, 0, 100, 0);
		const QPointer<AutomatableModel> original(b); const auto cleanup = qScopeGuard([&] { if (b) { std::destroy_at(b); } }); QVERIFY(a->linkToModel(b)); QVERIFY(a->linkToModel(c.get())); QVERIFY(a->linkToModel(d.get())); bool done = false;
		const auto connection = QObject::connect(c.get(), &AutomatableModel::dataChanged, this, [&] {
			if (done) { return; } done = true; std::destroy_at(b); b = nullptr; b = std::construct_at(reinterpret_cast<FloatModel*>(storage), 30, 0, 100, 0); QVERIFY(a->linkToModel(b));
		}, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		a->setValue(70); QVERIFY(done); QVERIFY(!original); QCOMPARE(b->storedValue(), 70.f); QCOMPARE(d->storedValue(), 70.f);
	}

	void nativeGraphRestoreDuringCallbackRejoinsOnlyCurrentUnvisitedPeers()
	{
		auto a = model(), b = model(), c = model(); QVERIFY(a->linkToModel(b.get())); QVERIFY(a->linkToModel(c.get()));
		QDomDocument document; auto root = document.createElement("controls"); document.appendChild(root); QString error;
		{ NativeLinkedControls::SaveScope save(document, {a.get(), b.get(), c.get()}); a->saveSettings(document, root, "a"); b->saveSettings(document, root, "b"); c->saveSettings(document, root, "c"); QVERIFY(save.finish(root, error)); }
		const auto ref = root.firstChildElement("b").attribute("native_link_ref"); root.firstChildElement("b").removeAttribute("native_link_ref"); auto group = root.firstChildElement("nativeLinkedControls").firstChildElement();
		for (auto member = group.firstChildElement(); !member.isNull(); ) { const auto next = member.nextSiblingElement(); if (member.attribute("ref") == ref) { group.removeChild(member); } member = next; }
		bool done = false; const auto connection = QObject::connect(b.get(), &AutomatableModel::dataChanged, this, [&] {
			if (done) { return; } done = true; JournallingObject::KeepIdentityScope identity; NativeLinkedControls::RestoreScope restore(document);
			a->loadSettings(root, "a"); b->loadSettings(root, "b"); c->loadSettings(root, "c"); QVERIFY2(restore.finish(root, error), qPrintable(error));
		}, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		a->setValue(70); QVERIFY(done); QVERIFY(a->isLinkedToModel(c.get())); QVERIFY(!b->isLinked()); QCOMPARE(a->storedValue(), 42.f); QCOMPARE(c->storedValue(), 70.f);
	}

	void returnedCallbackFailuresKeepLaterWritesPossible_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void returnedCallbackFailuresKeepLaterWritesPossible()
	{
		QFETCH(bool, standard); auto a = model(), b = model(), c = model(); QVERIFY(a->linkToModel(b.get())); QVERIFY(a->linkToModel(c.get()));
		ModelValueNotificationProbe fault(b.get(), [&] { if (standard) { throw std::runtime_error("fixture linked value callback"); } throw 42; });
		try { a->setValue(70); QFAIL("Callback must reject this write"); } catch (const std::runtime_error&) { QVERIFY(standard); } catch (int) { QVERIFY(!standard); }
		fault.reset(); QCOMPARE(c->storedValue(), 42.f); a->setValue(80); QCOMPARE(a->storedValue(), 80.f); QCOMPARE(b->storedValue(), 80.f); QCOMPARE(c->storedValue(), 80.f);
	}

	void receiverRetirementRefusesStructuredReceiptsAndRuntimeRestores_data()
	{ QTest::addColumn<bool>("runtime"); QTest::newRow("native") << false; QTest::newRow("runtime") << true; }
	void receiverRetirementRefusesStructuredReceiptsAndRuntimeRestores()
	{
		QFETCH(bool, runtime); QTemporaryDir directory; QString error; std::unique_ptr<StudioRuntime> studio; if (runtime) { studio = std::make_unique<StudioRuntime>(directory.path()); QVERIFY(studio->open(error)); }
		auto track = new InstrumentTrack(Engine::getSong()); const auto id = track->volumeModel()->id(); const float stored = track->volumeModel()->storedValue(); const QPointer<AutomatableModel> old(track->volumeModel());
		if (runtime) { QVERIFY(studio->process(request("baseline", "song.set", {{"tempo", Engine::getSong()->getTempo()}})).value("ok").toBool()); }
		const auto connection = QObject::connect(track->volumeModel(), &AutomatableModel::dataChanged, this, [track] { delete track; }, Qt::DirectConnection); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const QJsonObject args{{"parameter", static_cast<int>(id)}, {"value", 70}}; const auto result = runtime ? studio->process(request("retire", "parameter.set", args)) : StudioCapabilities::execute("parameter.set", args);
		QVERIFY(!result.value("ok").toBool()); QVERIFY(!old);
		if (runtime) { auto restored = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); QCOMPARE(restored->volumeModel()->storedValue(), stored); QVERIFY(!studio->stopping()); QVERIFY(!Engine::getSong()->hasErrors()); }
		else { QVERIFY(Engine::getSong()->tracks().empty()); }
	}

	void largeUnchangedRingsVisitEachLivePeerOnce()
	{
		auto root = model(); std::vector<std::unique_ptr<FloatModel>> peers; for (int index = 0; index < 1050; ++index) { peers.push_back(model()); QVERIFY(root->linkToModel(peers.back().get())); }
		int notifications = 0; for (const auto& peer : peers) { QObject::connect(peer.get(), &AutomatableModel::dataChanged, this, [&] { ++notifications; }, Qt::DirectConnection); }
		root->setValue(70); QCOMPARE(notifications, 1050); for (const auto& peer : peers) { QCOMPARE(peer->storedValue(), 70.f); }
	}
};
QTEST_GUILESS_MAIN(LinkedValueOwnershipTest)
#include "LinkedValueOwnershipTest.moc"
