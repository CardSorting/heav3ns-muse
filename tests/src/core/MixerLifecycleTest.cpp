/* Native mixer continuations and recovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <functional>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "SerializingObject.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
class Hook final : public SerializingObjectHook
{
public:
	std::function<void()> callback;
	void saveSettings(QDomDocument&, QDomElement&) override { }
	void loadSettings(const QDomElement&) override { const auto copy = callback; if (copy) { copy(); } }
};
Mixer* seed(int count = 3) { auto mixer = new Mixer; while (mixer->numChannels() < count) { mixer->createChannel(); } return mixer; }
QDomElement settings(QDomDocument& document)
{
	auto root = document.createElement("mixer"); document.appendChild(root);
	for (int index = 0; index < 3; ++index)
	{
		auto channel = document.createElement("mixerchannel"); channel.setAttribute("num", index); channel.setAttribute("volume", .4); channel.setAttribute("muted", 1); channel.setAttribute("soloed", 1); channel.setAttribute("mutedBeforeSolo", 0); channel.setAttribute("name", "Loaded channel"); channel.appendChild(document.createElement("fxchain"));
		if (index > 0) { auto send = document.createElement("send"); send.setAttribute("channel", 0); send.setAttribute("amount", .3); channel.appendChild(send); } root.appendChild(channel);
	}
	return root;
}
[[noreturn]] void raise(bool nonstandard) { if (nonstandard) { throw 17; } throw std::runtime_error("fixture mixer transition failure"); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}
class MixerLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void nativeMixerRoundTripPreservesRoutesAndSoloReleaseState()
	{
		Mixer original; original.createChannel(); original.createChannel(); original.mixerChannel(1)->m_muteModel.setValue(true); original.mixerChannel(2)->m_volumeModel.setValue(.6f); QVERIFY(original.createChannelSend(2, 1, .3f)); original.mixerChannel(2)->m_soloModel.setValue(true);
		QDomDocument document; auto parent = document.createElement("project"); document.appendChild(parent); const auto node = original.saveState(document, parent); Mixer restored; restored.loadSettings(node); QCOMPARE(restored.numChannels(), original.numChannels()); QCOMPARE(restored.soloChannel(), original.soloChannel()); QCOMPARE(restored.mixerChannel(2)->m_volumeModel.storedValue(), .6f); QVERIFY(restored.channelSendModel(2, 1)); QCOMPARE(restored.channelSendModel(2, 1)->storedValue(), .3f); restored.mixerChannel(2)->m_soloModel.setValue(false); QVERIFY(restored.mixerChannel(1)->m_muteModel.value());
	}
	void loadModelCallbacksCanRetireTheMixer_data()
	{ QTest::addColumn<int>("field"); QTest::newRow("volume") << 0; QTest::newRow("mute") << 1; QTest::newRow("solo") << 2; }
	void loadModelCallbacksCanRetireTheMixer()
	{
		QFETCH(int, field); auto target = seed(); const QPointer<Mixer> owner(target); auto channel = target->mixerChannel(0); AutomatableModel* model = field == 0 ? static_cast<AutomatableModel*>(&channel->m_volumeModel) : field == 1 ? static_cast<AutomatableModel*>(&channel->m_muteModel) : static_cast<AutomatableModel*>(&channel->m_soloModel); bool invoked = false; connect(model, &AutomatableModel::dataChanged, this, [&] { if (!invoked) { invoked = true; delete target; } }); QDomDocument document; const auto node = settings(document);
		QVERIFY_EXCEPTION_THROWN(target->loadSettings(node), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void returnedEffectRestorationCanRetireTheMixer()
	{
		auto target = seed(); const QPointer<Mixer> owner(target); Hook hook; bool invoked = false; hook.callback = [&] { invoked = true; delete target; }; target->mixerChannel(0)->m_fxChain.setHook(&hook); QDomDocument document; const auto node = settings(document);
		QVERIFY_EXCEPTION_THROWN(target->loadSettings(node), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void resetNotificationsCanRetireTheMixer_data()
	{ QTest::addColumn<int>("field"); QTest::newRow("volume") << 0; QTest::newRow("mute") << 1; QTest::newRow("solo") << 2; }
	void resetNotificationsCanRetireTheMixer()
	{
		QFETCH(int, field); auto target = seed(); const QPointer<Mixer> owner(target); auto channel = target->mixerChannel(1); if (field == 0) { channel->m_volumeModel.setValue(.2f); } if (field == 1) { channel->m_muteModel.setValue(true); } if (field == 2) { channel->m_soloModel.setValue(true); }
		AutomatableModel* model = field == 0 ? static_cast<AutomatableModel*>(&channel->m_volumeModel) : field == 1 ? static_cast<AutomatableModel*>(&channel->m_muteModel) : static_cast<AutomatableModel*>(&channel->m_soloModel); bool invoked = false; connect(model, &AutomatableModel::dataChanged, this, [&] { if (!invoked) { invoked = true; delete target; } });
		QVERIFY_EXCEPTION_THROWN(target->clearChannel(1), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void soloAndMuteMaskCallbacksCanRetireTheMixer_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("peer-solo-release") << 0; QTest::newRow("mute-mask") << 1; QTest::newRow("mute-release") << 2; }
	void soloAndMuteMaskCallbacksCanRetireTheMixer()
	{
		QFETCH(int, phase); auto target = seed(); const QPointer<Mixer> owner(target); if (phase == 0) { target->mixerChannel(2)->m_soloModel.setValue(true); } if (phase == 2) { target->mixerChannel(1)->m_soloModel.setValue(true); }
		AutomatableModel* observed = phase == 0 ? static_cast<AutomatableModel*>(&target->mixerChannel(2)->m_soloModel) : static_cast<AutomatableModel*>(&target->mixerChannel(2)->m_muteModel); bool invoked = false; connect(observed, &AutomatableModel::dataChanged, this, [&] { if (!invoked) { invoked = true; delete target; } });
		QVERIFY_EXCEPTION_THROWN(target->mixerChannel(1)->m_soloModel.setValue(phase != 2), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void existingRouteAmountCallbackCannotReturnADeadRoute()
	{
		auto target = seed(); const QPointer<Mixer> owner(target); auto route = target->m_mixerRoutes.front(); const QPointer<MixerRoute> live(route); bool invoked = false; connect(route->amount(), &AutomatableModel::dataChanged, this, [&] { invoked = true; delete target; });
		QVERIFY_EXCEPTION_THROWN(target->createChannelSend(1, 0, .2f), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner); QVERIFY(!live);
	}
	void soloJournalRestorationCanObserveMixerRetirement()
	{
		auto target = seed(); const QPointer<Mixer> owner(target); target->mixerChannel(1)->m_soloModel.setValue(true); QDomDocument document; auto parent = document.createElement("journal"); document.appendChild(parent); const auto state = target->mixerChannel(0)->m_soloModel.saveJournalState(document, parent); target->mixerChannel(2)->m_soloModel.setValue(true); bool invoked = false;
		connect(&target->mixerChannel(2)->m_soloModel, &AutomatableModel::dataChanged, this, [&] { if (!invoked) { invoked = true; delete target; } }); QString error;
		QVERIFY_EXCEPTION_THROWN(target->mixerChannel(0)->m_soloModel.restoreJournalState(state, error), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void directRouteRetirementPrunesEveryOwnershipView()
	{
		Mixer target; target.createChannel(); auto route = target.m_mixerRoutes.front(); const QPointer<MixerRoute> live(route); bool invoked = false; connect(route->amount(), &AutomatableModel::dataChanged, this, [&] { invoked = true; delete route; });
		QVERIFY_EXCEPTION_THROWN(target.createChannelSend(1, 0, .2f), std::runtime_error); QVERIFY(invoked); QVERIFY(!live); QVERIFY(target.m_mixerRoutes.empty()); QVERIFY(target.mixerChannel(1)->m_sends.empty()); QVERIFY(target.mixerChannel(0)->m_receives.empty()); QVERIFY(target.createChannelSend(1, 0, .4f));
	}
	void routeModelDestructionCanRetireTheMixer()
	{
		auto target = seed(); const QPointer<Mixer> owner(target); auto route = target->m_mixerRoutes.front(); bool invoked = false;
		connect(route->amount(), &QObject::destroyed, this, [&] { if (!invoked) { invoked = true; delete target; } }); delete route; QVERIFY(invoked); QVERIFY(!owner);
	}
	void routeRemovalObserverCanRetireTheMixer()
	{
		auto target = seed(); const QPointer<Mixer> owner(target); auto route = target->m_mixerRoutes.front(); bool invoked = false; connect(route, &QObject::destroyed, this, [&] { if (!invoked) { invoked = true; delete target; } });
		QVERIFY_EXCEPTION_THROWN(target->deleteChannelSend(route), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void channelMemberDestructionDoesNotLeaveMixerOwnedChildren()
	{
		auto target = seed(); const QPointer<Mixer> owner(target); auto channel = target->mixerChannel(1); bool invoked = false; connect(&channel->m_volumeModel, &QObject::destroyed, this, [&] { if (!invoked) { invoked = true; delete target; } });
		QVERIFY_EXCEPTION_THROWN(target->deleteChannel(1), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner);
	}
	void selectorRangeAndMoveNotificationsCanRetireTheMixer_data()
	{ QTest::addColumn<bool>("selector"); QTest::newRow("selector-range") << true; QTest::newRow("channel-moved") << false; }
	void selectorRangeAndMoveNotificationsCanRetireTheMixer()
	{
		QFETCH(bool, selector); auto target = seed(selector ? 1 : 3); const QPointer<Mixer> owner(target); bool invoked = false; QMetaObject::Connection connection;
		if (selector) { auto instrument = new InstrumentTrack(Engine::getSong()); connection = connect(instrument->mixerChannelModel(), &AutomatableModel::propertiesChanged, this, [&] { if (!invoked) { invoked = true; delete target; } }); } else { connection = connect(target, &Mixer::channelMoved, this, [&](int, int) { invoked = true; delete target; }); }
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); bool caught = false; try { if (selector) { target->createChannel(); } else { target->moveChannel(1, 2); } } catch (const std::runtime_error&) { caught = true; } QVERIFY(caught); QVERIFY(invoked); QVERIFY(!owner);
	}
	void resetExceptionsReleaseTopologyAndSoloAdmission_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void resetExceptionsReleaseTopologyAndSoloAdmission()
	{
		QFETCH(bool, nonstandard); Mixer target; target.mixerChannel(0)->m_volumeModel.setValue(.2f); bool armed = true; connect(&target.mixerChannel(0)->m_volumeModel, &AutomatableModel::dataChanged, this, [&] { if (armed) { armed = false; raise(nonstandard); } }); bool caught = false;
		try { target.clearChannel(0); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(!armed); QVERIFY(target.clearChannel(0)); QCOMPARE(target.createChannel(), 1);
	}
	void loadRefusesNestedTopologyAndRecoversCancellation_data()
	{ QTest::addColumn<bool>("cancel"); QTest::newRow("nested") << false; QTest::newRow("cancelled") << true; }
	void loadRefusesNestedTopologyAndRecoversCancellation()
	{
		QFETCH(bool, cancel); Mixer target; QDomDocument document; const auto node = settings(document); bool invoked = false;
		if (!cancel) { connect(&target.mixerChannel(0)->m_volumeModel, &AutomatableModel::dataChanged, this, [&] { if (invoked) { return; } invoked = true; QVERIFY_EXCEPTION_THROWN(target.createChannel(), std::logic_error); QVERIFY(!target.deleteChannel(1)); QVERIFY(!target.clearChannel(0)); QVERIFY_EXCEPTION_THROWN(target.loadSettings(node), std::logic_error); QDomDocument saved; auto element = saved.createElement("mixer"); QVERIFY_EXCEPTION_THROWN(target.saveSettings(saved, element), std::logic_error); }); target.loadSettings(node); }
		else { FileResourceIO::Scope work([&] { invoked = invoked || target.mixerChannel(0)->m_volumeModel.storedValue() == .4f; return invoked; }); QVERIFY_EXCEPTION_THROWN(target.loadSettings(node), std::runtime_error); }
		QVERIFY(invoked); QVERIFY(target.createChannel() >= 1);
	}
	void invalidGraphValidationPreservesTheExistingMixer()
	{
		Mixer target; target.createChannel(); target.mixerChannel(1)->m_volumeModel.setValue(.7f); const auto generation = target.replacementGeneration(); QDomDocument document; auto root = settings(document); auto first = root.firstChildElement("mixerchannel").nextSiblingElement("mixerchannel"); first.firstChildElement("send").setAttribute("channel", 2); auto second = first.nextSiblingElement("mixerchannel"); second.firstChildElement("send").setAttribute("channel", 1);
		target.loadSettings(root); QCOMPARE(target.numChannels(), 2); QCOMPARE(target.mixerChannel(1)->m_volumeModel.storedValue(), .7f); QCOMPARE(target.replacementGeneration(), generation); QVERIFY(Engine::getSong()->hasErrors());
	}
	void structuredEditRejectsTargetRemovalAndPersistentMasterReplacement_data()
	{ QTest::addColumn<bool>("master"); QTest::newRow("target-removal") << false; QTest::newRow("master-replacement") << true; }
	void structuredEditRejectsTargetRemovalAndPersistentMasterReplacement()
	{
		QFETCH(bool, master); auto mixer = Engine::mixer(); mixer->createChannel(); auto channel = mixer->mixerChannel(master ? 0 : 1); const auto generation = mixer->replacementGeneration(); bool invoked = false; const auto connection = connect(&channel->m_volumeModel, &AutomatableModel::dataChanged, this, [&] { if (invoked) { return; } invoked = true; if (master) { mixer->clear(); } else { mixer->deleteChannel(1); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("mixer.edit", {{"channel", master ? 0 : 1}, {"volume", .4}, {"muted", true}}); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(mixer->numChannels(), 1); if (master) { QVERIFY(mixer->replacementGeneration() > generation); QCOMPARE(mixer->mixerChannel(0), channel); }
	}
	void dirtyNotificationCannotPublishAChangedMixerReceipt()
	{
		auto song = Engine::getSong(); NativeSongTestAccess::setModified(song, false); bool invoked = false; const auto connection = connect(song, &Song::modified, this, [&] { if (!invoked) { invoked = true; Engine::mixer()->createChannel(); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("mixer.add", {}); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(Engine::mixer()->numChannels(), 3);
	}
	void editKeepsItsSelectedChannelAcrossCallbackReordering()
	{
		auto mixer = Engine::mixer(); mixer->createChannel(); mixer->createChannel(); auto selected = mixer->mixerChannel(1); auto peer = mixer->mixerChannel(2); bool invoked = false;
		const auto connection = connect(&selected->m_volumeModel, &AutomatableModel::dataChanged, this, [&] { if (!invoked) { invoked = true; QVERIFY(mixer->moveChannel(1, 2)); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("mixer.edit", {{"channel", 1}, {"volume", .4}, {"muted", true}}); QVERIFY(invoked); QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("channel").toInt(), 2); QCOMPARE(mixer->mixerChannel(2), selected); QVERIFY(selected->m_muteModel.value()); QCOMPARE(peer->m_volumeModel.storedValue(), 1.f); QVERIFY(!peer->m_muteModel.value());
	}
	void nestedProjectReplacementCannotReuseAStaleMixerReceipt()
	{
		auto mixer = Engine::mixer(); mixer->createChannel(); mixer->createChannel(); const auto generation = mixer->replacementGeneration(); const auto project = Engine::getSong()->replacementGeneration(); bool invoked = false;
		const auto connection = connect(mixer, &Mixer::channelMoved, this, [&](int, int) { if (!invoked) { invoked = true; Engine::getSong()->createEmptyProject(); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto response = StudioCapabilities::execute("mixer.edit", {{"channel", 1}, {"to", 2}}); QVERIFY(invoked); QVERIFY(!response.value("ok").toBool()); QCOMPARE(mixer->replacementGeneration(), generation); QVERIFY(Engine::getSong()->replacementGeneration() > project);
	}
	void runtimeRecoversMixerLoadFailureAndReplaysIt_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void runtimeRecoversMixerLoadFailureAndReplaysIt()
	{
		QFETCH(bool, nonstandard); QTemporaryDir directory; QTemporaryDir input; const auto path = input.filePath("incoming.mmp"); DataFile file(DataFile::Type::SongProject); QDomDocument fixture; const auto node = settings(fixture); file.content().appendChild(file.importNode(node, true)); QVERIFY(file.writeFile(path)); const auto command = request("load", "project.load", {{"path", path}}); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "mixer.edit", {{"channel", 0}, {"volume", .8}})).value("ok").toBool()); bool armed = true; const auto connection = connect(&Engine::mixer()->mixerChannel(0)->m_volumeModel, &AutomatableModel::dataChanged, this, [&] { if (armed && Engine::getSong()->isLoadingProject() && Engine::mixer()->mixerChannel(0)->m_volumeModel.storedValue() == .4f) { armed = false; raise(nonstandard); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); receipt = runtime.process(command); QVERIFY(!armed); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(!runtime.stopping()); QCOMPARE(Engine::mixer()->mixerChannel(0)->m_volumeModel.storedValue(), .8f); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); }
		StudioRuntime reopened(directory.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QCOMPARE(Engine::mixer()->mixerChannel(0)->m_volumeModel.storedValue(), .8f);
	}
};
QTEST_GUILESS_MAIN(MixerLifecycleTest)
#include "MixerLifecycleTest.moc"
