/* Native effect ownership through returned callbacks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <stdexcept>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"effect-lifecycle-fixture", "Effect lifecycle fixture", "Native ownership fixture", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
class LifecycleEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	explicit LifecycleEffect(Model* parent) : Effect(&fixtureDescriptor, parent, nullptr) { }
	std::function<void()> onRetire, onSave, onRefresh;
	std::unique_ptr<FloatModel> parameter = std::make_unique<FloatModel>(1, 0, 2, .01f, this, "Lifecycle parameter");
	~LifecycleEffect() override { const auto callback = onRetire; if (callback) { callback(); } }
	EffectControls* controls() override { return &m_controls; }
	void refreshParameterModels() override { const auto callback = onRefresh; if (callback) { callback(); } }
	void saveSettings(QDomDocument& document, QDomElement& node) override
	{ const auto callback = onSave; if (callback) { callback(); } Effect::saveSettings(document, node); }
protected:
	ProcessStatus processImpl(SampleFrame*, f_cnt_t) override { return ProcessStatus::Continue; }
};
LifecycleEffect* append(EffectChain* chain)
{ auto effect = new LifecycleEffect(chain); return chain->appendEffect(effect) ? effect : nullptr; }
void missing(EffectChain* chain)
{
	for (int index = 0; index < 3; ++index)
	{
		QDomDocument document; auto node = document.createElement("effect"); document.appendChild(node); node.setAttribute("name", "missing-lifecycle-fixture"); node.setAttribute("identity", index);
		chain->appendEffect(new DummyEffect(chain, node));
	}
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}
class EffectChainLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	EffectChain* master() const { return &Engine::mixer()->mixerChannel(0)->m_fxChain; }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
		journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void independentDeletionPrunesRosterAndDisablesAnEmptyChain()
	{
		EffectChain chain(nullptr); const QPointer<Effect> effect(append(&chain)); QVERIFY(effect); QSignalSpy changed(&chain, &Model::dataChanged);
		delete effect.data(); QVERIFY(!effect); QVERIFY(chain.effects().empty()); QVERIFY(!chain.enabledModel()->value()); QCOMPARE(changed.count(), 1);
		QVERIFY(append(&chain)); QVERIFY(chain.clear());
	}
	void appendObserversCanRetireTheTargetOrOwner_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("target") << 0; QTest::newRow("chain") << 1; QTest::newRow("enabled-owner") << 2; }
	void appendObserversCanRetireTheTargetOrOwner()
	{
		QFETCH(int, action); auto chain = std::make_unique<EffectChain>(nullptr); const QPointer<EffectChain> owner(chain.get());
		std::unique_ptr<Effect> candidate = std::make_unique<LifecycleEffect>(nullptr); const QPointer<Effect> target(candidate.get()); bool invoked = false;
		const auto callback = [&] { if (invoked) { return; } invoked = true; if (action == 0) { delete target.data(); } else { chain.reset(); } };
		const auto connection = action == 2 ? connect(chain->enabledModel(), &Model::dataChanged, this, callback) : connect(chain.get(), &Model::dataChanged, this, callback);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(!owner->appendEffect(candidate)); QVERIFY(invoked); QVERIFY(!candidate); QVERIFY(!target);
		if (owner) { QVERIFY(owner->effects().empty()); QVERIFY(append(owner.data())); }
	}
	void detachObserversCanRetireTheTargetOrOwner_data()
	{
		QTest::addColumn<bool>("owned"); QTest::addColumn<bool>("retireOwner"); QTest::newRow("take-target") << true << false; QTest::newRow("take-owner") << true << true;
		QTest::newRow("legacy-target") << false << false; QTest::newRow("legacy-owner") << false << true;
	}
	void detachObserversCanRetireTheTargetOrOwner()
	{
		QFETCH(bool, owned); QFETCH(bool, retireOwner); auto chain = std::make_unique<EffectChain>(nullptr); const QPointer<EffectChain> owner(chain.get()); const QPointer<Effect> target(append(chain.get())); bool invoked = false;
		const auto connection = connect(chain.get(), &Model::dataChanged, this, [&] { if (invoked) { return; } invoked = true; if (retireOwner) { chain.reset(); } else { delete target.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		if (owned) { auto detached = owner->takeEffect(target.data()); QVERIFY(!detached); } else { QVERIFY(!owner->removeEffect(target.data())); }
		QVERIFY(invoked); QVERIFY(!target); if (owner) { QVERIFY(owner->effects().empty()); QVERIFY(append(owner.data())); }
	}
	void aDetachedEffectAdoptedElsewhereIsNotReturnedOrDeleted()
	{
		EffectChain source(nullptr), destination(nullptr); const QPointer<Effect> target(append(&source)); bool adopted = false;
		const auto connection = connect(&source, &Model::dataChanged, this, [&] { if (!adopted) { adopted = destination.appendEffect(target.data()); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); auto detached = source.takeEffect(target.data());
		QVERIFY(adopted); QVERIFY(!detached); QVERIFY(target); QCOMPARE(target->effectChain(), &destination); QVERIFY(source.effects().empty()); QCOMPARE(destination.effects().size(), std::size_t(1));
	}
	void clearObserversCanRetireTheOwnerWithoutRepeatingAboutToClear_data()
	{ QTest::addColumn<bool>("enabled"); QTest::newRow("about-to-clear") << false; QTest::newRow("enabled") << true; }
	void clearObserversCanRetireTheOwnerWithoutRepeatingAboutToClear()
	{
		QFETCH(bool, enabled); auto chain = std::make_unique<EffectChain>(nullptr); const QPointer<EffectChain> owner(chain.get()); const QPointer<Effect> target(append(chain.get())); int notifications = 0;
		connect(chain.get(), &EffectChain::aboutToClear, this, [&] { ++notifications; if (!enabled) { chain.reset(); } });
		if (enabled) { connect(chain->enabledModel(), &Model::dataChanged, this, [&] { chain.reset(); }); }
		QVERIFY(!owner->clear()); QVERIFY(!owner); QVERIFY(!target); QCOMPARE(notifications, 1);
	}
	void clearingAnEffectCanRetireAnotherPendingEffect()
	{
		EffectChain chain(nullptr); const QPointer<Effect> first(append(&chain)); const QPointer<Effect> second(append(&chain)); auto last = append(&chain); int deaths = 0;
		for (auto effect : chain.effects()) { connect(effect, &QObject::destroyed, this, [&] { ++deaths; }); }
		last->onRetire = [&] { delete first.data(); }; QVERIFY(chain.clear()); QVERIFY(!first); QVERIFY(!second); QCOMPARE(deaths, 3); QVERIFY(chain.effects().empty());
	}
	void throwingDetachObserversCleanOnlyUnadoptedTargets_data()
	{ QTest::addColumn<bool>("adopt"); QTest::addColumn<bool>("standard"); QTest::newRow("owned-standard") << false << true; QTest::newRow("owned-nonstandard") << false << false; QTest::newRow("adopted-standard") << true << true; QTest::newRow("adopted-nonstandard") << true << false; }
	void throwingDetachObserversCleanOnlyUnadoptedTargets()
	{
		QFETCH(bool, adopt); QFETCH(bool, standard); EffectChain source(nullptr), destination(nullptr); const QPointer<Effect> target(append(&source));
		const auto connection = connect(&source, &Model::dataChanged, this, [&] { if (adopt) { QVERIFY(destination.appendEffect(target.data())); } if (standard) { throw std::runtime_error("fixture detach"); } throw 7; });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool caught = false; try { auto detached = source.takeEffect(target.data()); } catch (...) { caught = true; }
		QVERIFY(caught); QCOMPARE(bool(target), adopt); QVERIFY(source.effects().empty()); QObject::disconnect(connection); QVERIFY(append(&source));
	}
	void serializationRefusesADeletedPeerAndReleasesTheTransition()
	{
		EffectChain chain(nullptr); auto first = append(&chain); const QPointer<Effect> peer(append(&chain)); bool invoked = false;
		first->onSave = [&] { if (!invoked) { invoked = true; delete peer.data(); } }; QDomDocument document; auto node = document.createElement("fxchain"); document.appendChild(node);
		QVERIFY_EXCEPTION_THROWN(chain.saveSettings(document, node), std::runtime_error); first->onSave = {}; QVERIFY(invoked); QVERIFY(!peer); QVERIFY(append(&chain));
	}
	void settingsReplacementRefusesRetirementDuringFinalNotification()
	{
		EffectChain chain(nullptr); missing(&chain); QDomDocument document; auto node = document.createElement("fxchain"); document.appendChild(node); chain.saveSettings(document, node); bool invoked = false;
		const auto connection = connect(&chain, &Model::dataChanged, this, [&] { if (!invoked) { invoked = true; delete chain.effects().back(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY_EXCEPTION_THROWN(chain.loadSettings(node), std::runtime_error); QVERIFY(invoked); QCOMPARE(chain.effects().size(), std::size_t(2));
		QObject::disconnect(connection); QVERIFY(append(&chain));
	}
	void movingAnEffectCannotReportSuccessAfterItsRetirement_data()
	{ QTest::addColumn<bool>("ownerDeath"); QTest::newRow("target") << false; QTest::newRow("owner") << true; }
	void movingAnEffectCannotReportSuccessAfterItsRetirement()
	{
		QFETCH(bool, ownerDeath); auto chain = std::make_unique<EffectChain>(nullptr); const QPointer<EffectChain> owner(chain.get()); const QPointer<Effect> target(append(chain.get())); QVERIFY(append(chain.get()));
		const auto connection = connect(chain.get(), &Model::dataChanged, this, [&] { if (ownerDeath) { chain.reset(); } else { delete target.data(); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY(!owner->moveEffect(0, 1)); QVERIFY(!target);
	}
	void structuredMutationsRejectRetiredTrackChains_data()
	{ QTest::addColumn<QString>("operation"); QTest::newRow("move") << QString("effect.move"); QTest::newRow("remove") << QString("effect.remove"); QTest::newRow("clear") << QString("effect.clear"); }
	void structuredMutationsRejectRetiredTrackChains()
	{
		QFETCH(QString, operation); auto track = new InstrumentTrack(Engine::getSong()); const QPointer<EffectChain> chain(track->audioBusHandle()->effects()); missing(chain.data()); bool invoked = false;
		connect(chain.data(), &Model::dataChanged, this, [&] { if (!invoked) { invoked = true; delete track; } });
		QJsonObject args{{"track", 0}}; if (operation != "effect.clear") { args.insert("effect", 0); } if (operation == "effect.move") { args.insert("to", 2); }
		QVERIFY(!StudioCapabilities::execute(operation, args).value("ok").toBool()); QVERIFY(invoked); QVERIFY(!chain); QVERIFY(Engine::getSong()->tracks().empty());
	}
	void structuredInspectionRefusesCooperativeRosterMutation_data()
	{ QTest::addColumn<bool>("initiallyEmpty"); QTest::newRow("retire") << false; QTest::newRow("empty-to-populated") << true; }
	void structuredInspectionRefusesCooperativeRosterMutation()
	{
		QFETCH(bool, initiallyEmpty); if (!initiallyEmpty) { missing(master()); } bool changed = false;
		FileResourceIO::Scope work([&] { if (!changed) { changed = true; if (initiallyEmpty) { missing(master()); } else { delete master()->effects().back(); } } return false; }, false);
		const auto response = StudioCapabilities::execute("effect.inspect", {{"channel", 0}, {"details", false}}); QVERIFY(changed); QVERIFY(!response.value("ok").toBool());
	}
	void parameterDiscoveryRefusesAChildDeletedAfterRefresh()
	{
		auto effect = append(master()); bool refreshed = false, removed = false; effect->onRefresh = [&] { refreshed = true; };
		FileResourceIO::Scope work([&] { if (refreshed && !removed) { removed = true; effect->parameter.reset(); } return false; }, false);
		const auto response = StudioCapabilities::execute("effect.inspect", {{"channel", 0}, {"effect", 0}}); effect->onRefresh = {};
		QVERIFY(refreshed); QVERIFY(removed); QVERIFY(!response.value("ok").toBool()); QCOMPARE(master()->effects().size(), std::size_t(1));
	}
	void cancelledStructuredMutationsPreserveTheRoster_data()
	{ QTest::addColumn<QString>("operation"); QTest::newRow("move") << QString("effect.move"); QTest::newRow("remove") << QString("effect.remove"); QTest::newRow("clear") << QString("effect.clear"); }
	void cancelledStructuredMutationsPreserveTheRoster()
	{
		QFETCH(QString, operation); missing(master()); const auto before = master()->effects(); QJsonObject args{{"channel", 0}};
		if (operation != "effect.clear") { args.insert("effect", 0); } if (operation == "effect.move") { args.insert("to", 2); }
		QVERIFY(!StudioCapabilities::execute(operation, args, [] { return true; }).value("ok").toBool()); QVERIFY(master()->effects() == before);
	}
	void projectNotificationCannotInvalidateSuccessfulEffectReceipts_data()
	{ QTest::addColumn<bool>("retire"); QTest::newRow("retire") << true; QTest::newRow("reorder") << false; }
	void projectNotificationCannotInvalidateSuccessfulEffectReceipts()
	{
		QFETCH(bool, retire); missing(master()); NativeSongTestAccess::setModified(Engine::getSong(), false); bool invoked = false;
		const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (invoked) { return; } invoked = true; if (retire) { delete master()->effects().front(); } else { master()->moveEffect(0, 1); } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto response = StudioCapabilities::execute("effect.move", {{"channel", 0}, {"effect", 0}, {"to", 2}});
		QVERIFY(invoked); QVERIFY(!response.value("ok").toBool());
	}
	void runtimeRecoversAndReplaysNonstandardTransitionFailure()
	{
		QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); missing(master());
		QVERIFY(runtime.process(request("seed", "mixer.edit", {{"channel", 0}, {"name", "Effects"}})).value("ok").toBool()); bool invoked = false;
		const auto connection = connect(master(), &Model::dataChanged, this, [&] { if (!invoked) { invoked = true; throw 7; } });
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("remove", "effect.remove", {{"channel", 0}, {"effect", 0}});
		const auto receipt = runtime.process(command); QVERIFY(invoked); QVERIFY(!receipt.value("ok").toBool()); QCOMPARE(master()->effects().size(), std::size_t(3));
		auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(EffectChainLifecycleTest)
#include "EffectChainLifecycleTest.moc"
