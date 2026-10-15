/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

namespace lmms
{
namespace
{
class JournalFixture final : public JournallingObject
{
public:
	int value = 0;
	int failure = 0;
	bool reenter = false;
	bool observedJournallingDisabled = false;
	QString nodeName() const override { return "journal-fixture"; }
	bool restoreJournalState(const QDomElement& state, QString& error) override
	{
		observedJournallingDisabled = !Engine::projectJournal()->isJournalling();
		if (failure == 1) { error = "Fixture refuses restoration"; return false; }
		if (failure == 2) { throw std::runtime_error("Fixture restoration exception"); }
		if (failure == 3) { throw 17; }
		if (reenter) { Engine::projectJournal()->undo(); Engine::projectJournal()->clearJournal(); }
		return JournallingObject::restoreJournalState(state, error);
	}
protected:
	void saveSettings(QDomDocument&, QDomElement& state) override { state.setAttribute("value", value); }
	void loadSettings(const QDomElement& state) override { value = state.attribute("value").toInt(); }
};
}

class MixerSoloTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static bool muted(int channel) { return Engine::mixer()->mixerChannel(channel)->m_muteModel.value(); }
	static void mute(int channel, bool value) { Engine::mixer()->mixerChannel(channel)->m_muteModel.setValue(value); }
	static void solo(int channel, bool value) { Engine::mixer()->mixerChannel(channel)->m_soloModel.setValue(value); }
	static void channels(int count) { while (Engine::mixer()->numChannels() <= count) { Engine::mixer()->createChannel(); } }
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", op}, {"args", args}};
	}
	static QDomDocument savedMixer()
	{
		QDomDocument document; auto root = document.createElement("mixer"); document.appendChild(root);
		Engine::mixer()->saveSettings(document, root); return document;
	}
	static std::vector<bool> muteState()
	{
		std::vector<bool> result; for (int i = 0; i < Engine::mixer()->numChannels(); ++i) { result.push_back(muted(i)); } return result;
	}
	static void fill(int channel, float left, float right)
	{
		auto& buffer = Engine::mixer()->mixerChannel(channel)->m_buffer;
		for (int side = 0; side < 2; ++side)
		{
			std::fill_n(buffer.group(0).buffer(side), Engine::audioEngine()->framesPerPeriod(), side ? right : left);
			buffer.assumeNonSilent(side);
		}
		toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer()); buffer.updateAllSilenceFlags();
	}
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stop(); Engine::getSong()->clearProject();
		Engine::getSong()->clearErrors(); QVERIFY(Engine::audioEngine()->resetDspFailure());
	}
	void modelSoloAppliesWithoutGuiAndRestoresMutes_data()
	{
		QTest::addColumn<bool>("masterMuted"); QTest::addColumn<bool>("selectMaster");
		QTest::newRow("channel-master-audible") << false << false; QTest::newRow("channel-master-muted") << true << false;
		QTest::newRow("master-audible") << false << true; QTest::newRow("master-muted") << true << true;
	}
	void modelSoloAppliesWithoutGuiAndRestoresMutes()
	{
		QFETCH(bool, masterMuted); QFETCH(bool, selectMaster); channels(3); mute(0, masterMuted); mute(2, true);
		const auto original = muteState(); const int selected = selectMaster ? 0 : 1;
		solo(selected, true); QCOMPARE(Engine::mixer()->soloChannel(), selected); QVERIFY(!muted(0));
		QCOMPARE(muted(1), selectMaster); QVERIFY(muted(2)); QVERIFY(muted(3));
		const auto observed = StudioCapabilities::inspect(); QCOMPARE(observed.value("soloChannel").toInt(), selected);
		const auto list = observed.value("channels").toArray();
		for (int i = 0; i < list.size(); ++i)
		{
			const auto channel = list.at(i).toObject(); QCOMPARE(channel.value("solo").toBool(), i == selected);
			QCOMPARE(channel.value("muteBeforeSolo").toBool(), static_cast<bool>(original[i]));
		}
		solo(selected, false); QCOMPARE(Engine::mixer()->soloChannel(), -1); QVERIFY(muteState() == original);
	}
	void soloSelectionIncludesAncestorsAndTheirOutputs()
	{
		channels(6); auto mixer = Engine::mixer();
		for (int i = 1; i <= 5; ++i) { mixer->deleteChannelSend(i, 0); }
		// 1 -> 3 -> 5 -> master, 2 -> 3, and 1 -> 4 -> master.
		// Selecting 3 includes source 1's other output 4, but not independent 6.
		QVERIFY(mixer->createChannelSend(1, 3)); QVERIFY(mixer->createChannelSend(2, 3));
		QVERIFY(mixer->createChannelSend(3, 5)); QVERIFY(mixer->createChannelSend(5, 0));
		QVERIFY(mixer->createChannelSend(1, 4)); QVERIFY(mixer->createChannelSend(4, 0));
		mute(1, true); mute(4, true); const auto original = muteState();
		solo(3, true); for (int i = 0; i <= 5; ++i) { QVERIFY(!muted(i)); } QVERIFY(muted(6));
		solo(3, false); QVERIFY(muteState() == original);
	}
	void changingSoloPreservesOriginalMutesAndManualEdits()
	{
		channels(4); mute(0, true); mute(2, true); solo(1, true);
		solo(2, true); QCOMPARE(Engine::mixer()->soloChannel(), 2);
		QVERIFY(!Engine::mixer()->mixerChannel(1)->m_soloModel.value()); QVERIFY(muted(1)); QVERIFY(!muted(2));
		// Explicit mute edits survive release; same-value solo writes are inert.
		mute(3, false); solo(2, true); QVERIFY(!muted(3));
		mute(4, true); // Already masked: an explicit same-value edit still changes restoration state.
		mute(2, true); solo(2, false);
		QVERIFY(muted(0)); QVERIFY(!muted(1)); QVERIFY(muted(2)); QVERIFY(!muted(3));
		QVERIFY(muted(4));
	}
	void routesAndNewChannelsRefreshSoloWithoutReplacingSnapshot()
	{
		channels(2); solo(1, true); auto mixer = Engine::mixer(); QVERIFY(muted(2));
		QVERIFY(mixer->createChannelSend(1, 2)); QVERIFY(!muted(2));
		mixer->deleteChannelSend(1, 2); QVERIFY(muted(2));
		const int added = mixer->createChannel(); QCOMPARE(added, 3); QVERIFY(muted(3));
		QVERIFY(mixer->createChannelSend(3, 1)); QVERIFY(!muted(3));
		solo(1, false); for (int i = 0; i <= 3; ++i) { QVERIFY(!muted(i)); }
	}
	void movingDeletingAndClearingSoloRestoreNativeState_data()
	{
		QTest::addColumn<QString>("operation");
		QTest::newRow("delete") << QString("delete"); QTest::newRow("clear-channel") << QString("clear-channel");
		QTest::newRow("clear-mixer") << QString("clear-mixer");
	}
	void movingDeletingAndClearingSoloRestoreNativeState()
	{
		QFETCH(QString, operation); channels(3); mute(0, true); mute(3, true); solo(2, true);
		auto mixer = Engine::mixer(); const auto selected = mixer->mixerChannel(2);
		mixer->moveChannelLeft(2); QCOMPARE(mixer->mixerChannel(1), selected); QCOMPARE(mixer->soloChannel(), 1);
		mixer->moveChannelRight(1); QCOMPARE(mixer->mixerChannel(2), selected); QCOMPARE(mixer->soloChannel(), 2);
		mixer->deleteChannel(1); QCOMPARE(mixer->mixerChannel(1), selected); QCOMPARE(mixer->soloChannel(), 1);
		if (operation == "delete") { mixer->deleteChannel(1); QVERIFY(muted(0)); QVERIFY(muted(1)); }
		else if (operation == "clear-channel") { mixer->clearChannel(1); QVERIFY(muted(0)); QVERIFY(!muted(1)); QVERIFY(muted(2)); }
		else { mixer->clear(); QCOMPARE(mixer->numChannels(), mix_ch_t{1}); QVERIFY(!muted(0)); }
		QCOMPARE(mixer->soloChannel(), -1);
	}
	void nativeXmlPreservesEffectiveAndRestoredMutes_data()
	{
		QTest::addColumn<bool>("masterMuted"); QTest::addColumn<bool>("overrideMute");
		QTest::newRow("audible-master") << false << false; QTest::newRow("muted-master") << true << false;
		QTest::newRow("audible-master-override") << false << true; QTest::newRow("muted-master-override") << true << true;
	}
	void nativeXmlPreservesEffectiveAndRestoredMutes()
	{
		QFETCH(bool, masterMuted); QFETCH(bool, overrideMute); channels(3); mute(0, masterMuted); mute(2, true);
		auto restored = muteState(); solo(1, true);
		if (overrideMute) { mute(3, false); mute(1, true); restored[1] = true; restored[3] = false; }
		const auto effective = muteState(); const auto document = savedMixer();
		for (auto channel = document.documentElement().firstChildElement("mixerchannel"); !channel.isNull(); channel = channel.nextSiblingElement("mixerchannel"))
		{
			QVERIFY(channel.hasAttribute("mutedBeforeSolo"));
		}
		Engine::mixer()->clear(); Engine::mixer()->loadSettings(document.documentElement());
		QVERIFY(!Engine::getSong()->hasErrors()); QCOMPARE(Engine::mixer()->soloChannel(), 1); QVERIFY(muteState() == effective);
		solo(1, false); QVERIFY(muteState() == restored);
	}
	void legacySoloXmlUsesSavedMutesAsRestorationBaseline_data()
	{
		QTest::addColumn<bool>("multiple"); QTest::newRow("one-solo") << false; QTest::newRow("multiple-solos") << true;
	}
	void legacySoloXmlUsesSavedMutesAsRestorationBaseline()
	{
		QFETCH(bool, multiple); channels(2); mute(0, true); mute(2, true); const auto original = muteState();
		auto document = savedMixer();
		for (auto channel = document.documentElement().firstChildElement("mixerchannel"); !channel.isNull(); channel = channel.nextSiblingElement("mixerchannel"))
		{
			const int index = channel.attribute("num").toInt();
			channel.setAttribute("soloed", index == 1 || (multiple && index == 2) ? 1 : 0);
		}
		Engine::mixer()->loadSettings(document.documentElement());
		const int selected = multiple ? 2 : 1; QCOMPARE(Engine::mixer()->soloChannel(), selected);
		QVERIFY(!muted(0)); QVERIFY(!muted(selected)); QVERIFY(muted(multiple ? 1 : 2));
		solo(selected, false); QVERIFY(muteState() == original);
	}
	void invalidSoloSnapshotPreservesCurrentMixer()
	{
		channels(2); solo(1, true); const auto selected = Engine::mixer()->mixerChannel(1); const auto original = muteState();
		auto document = savedMixer(); document.documentElement().firstChildElement("mixerchannel").setAttribute("mutedBeforeSolo", "invalid");
		Engine::mixer()->loadSettings(document.documentElement()); QVERIFY(Engine::getSong()->hasErrors());
		QCOMPARE(Engine::mixer()->mixerChannel(1), selected); QCOMPARE(Engine::mixer()->soloChannel(), 1);
		QVERIFY(muteState() == original);
	}
	void studioSoloSurvivesUndoRedoRestartAndReceiptReplay()
	{
		QTemporaryDir session; QVERIFY(session.isValid()); QString error;
		const auto soloRequest = request("solo", "mixer.edit", {{"channel", 1}, {"solo", true}});
		{
			StudioRuntime runtime(session.path()); QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("one", "mixer.add")).value("ok").toBool());
			QVERIFY(runtime.process(request("two", "mixer.add")).value("ok").toBool());
			QVERIFY(runtime.process(request("master-muted", "mixer.edit", {{"channel", 0}, {"muted", true}})).value("ok").toBool());
			QVERIFY(runtime.process(soloRequest).value("ok").toBool()); QCOMPARE(Engine::mixer()->soloChannel(), 1); QVERIFY(!muted(0)); QVERIFY(muted(2));
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool());
			QCOMPARE(Engine::mixer()->soloChannel(), -1); QVERIFY(muted(0)); QVERIFY(!muted(2));
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool());
			QCOMPARE(Engine::mixer()->soloChannel(), 1); QVERIFY(!muted(0)); QVERIFY(muted(2));
			QVERIFY(!runtime.process(request("partial-failure", "mixer.edit", {{"channel", 1}, {"name", "Do not retain"}, {"volume", 99}})).value("ok").toBool());
			QCOMPARE(Engine::mixer()->soloChannel(), 1); QVERIFY(Engine::mixer()->mixerChannel(1)->m_name != "Do not retain");
		}
		Engine::getSong()->clearProject();
		StudioRuntime recovered(session.path()); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(Engine::mixer()->soloChannel(), 1); QVERIFY(!muted(0)); QVERIFY(muted(2));
		QVERIFY(recovered.process(request("release", "mixer.edit", {{"channel", 1}, {"solo", false}})).value("ok").toBool());
		QCOMPARE(Engine::mixer()->soloChannel(), -1); QVERIFY(muted(0)); QVERIFY(!muted(2));
		QVERIFY(recovered.process(soloRequest).value("replayed").toBool()); QCOMPARE(Engine::mixer()->soloChannel(), -1);
	}
	void nativeJournalUndoesWholeSoloTransitionsWithoutReplacingObjects()
	{
		channels(2); mute(0, true); mute(2, true); auto mixer = Engine::mixer();
		const auto first = mixer->mixerChannel(1), second = mixer->mixerChannel(2);
		const auto route = mixer->channelSendModel(1, 0); const auto baseline = muteState();
		const auto journal = Engine::projectJournal(); const auto previous = journal->isJournalling();
		const auto cleanup = qScopeGuard([&] { journal->setJournalling(previous); journal->clearJournal(); });
		journal->clearJournal(); journal->setJournalling(true);
		solo(1, true); const auto firstSolo = muteState(); QVERIFY(journal->canUndo());
		journal->setJournalling(false); first->m_volumeModel.setValue(.75f); journal->setJournalling(true);
		journal->undo(); QCOMPARE(mixer->soloChannel(), -1); QVERIFY(muteState() == baseline);
		QVERIFY(!journal->canUndo()); QVERIFY(journal->canRedo());
		QCOMPARE(mixer->mixerChannel(1), first); QCOMPARE(mixer->channelSendModel(1, 0), route);
		QCOMPARE(first->m_volumeModel.value(), .75f);
		journal->redo(); QCOMPARE(mixer->soloChannel(), 1); QVERIFY(muteState() == firstSolo); QVERIFY(!journal->canRedo());
		solo(2, true); const auto secondSolo = muteState(); QCOMPARE(mixer->soloChannel(), 2);
		journal->undo(); QCOMPARE(mixer->soloChannel(), 1); QVERIFY(muteState() == firstSolo);
		journal->redo(); QCOMPARE(mixer->soloChannel(), 2); QVERIFY(muteState() == secondSolo);
		solo(2, false); QVERIFY(muteState() == baseline);
		journal->undo(); QCOMPARE(mixer->soloChannel(), 2); QVERIFY(muteState() == secondSolo);
		journal->redo(); QCOMPARE(mixer->soloChannel(), -1); QVERIFY(muteState() == baseline);
		QCOMPARE(mixer->mixerChannel(2), second); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void sameValueMuteEditHasReversibleSoloRestorationMeaning()
	{
		channels(2); solo(1, true); auto mixer = Engine::mixer(); QVERIFY(muted(2));
		QVERIFY(!mixer->mixerChannel(2)->m_muteBeforeSolo);
		const auto journal = Engine::projectJournal(); const auto previous = journal->isJournalling();
		const auto cleanup = qScopeGuard([&] { journal->setJournalling(previous); journal->clearJournal(); });
		journal->clearJournal(); journal->setJournalling(true);
		mute(2, true); QVERIFY(mixer->mixerChannel(2)->m_muteBeforeSolo); QVERIFY(journal->canUndo());
		journal->undo(); QCOMPARE(mixer->soloChannel(), 1); QVERIFY(muted(2));
		QVERIFY(!mixer->mixerChannel(2)->m_muteBeforeSolo); QVERIFY(!journal->canUndo());
		journal->redo(); QVERIFY(mixer->mixerChannel(2)->m_muteBeforeSolo);
		solo(1, false); QVERIFY(muted(2));
		journal->undo(); QCOMPARE(mixer->soloChannel(), 1); QVERIFY(mixer->mixerChannel(2)->m_muteBeforeSolo);
	}
	void groupedJournalFollowsMovedChannelsAndRejectsChangedMembership_data()
	{
		QTest::addColumn<bool>("addChannel"); QTest::newRow("removed-channel") << false; QTest::newRow("added-channel") << true;
	}
	void groupedJournalFollowsMovedChannelsAndRejectsChangedMembership()
	{
		QFETCH(bool, addChannel); channels(3); mute(3, true); auto mixer = Engine::mixer();
		const auto selected = mixer->mixerChannel(2);
		const auto journal = Engine::projectJournal(); const auto previous = journal->isJournalling();
		const auto cleanup = qScopeGuard([&] { journal->setJournalling(previous); journal->clearJournal(); });
		journal->clearJournal(); journal->setJournalling(true); solo(2, true);
		mixer->moveChannelLeft(2); QCOMPARE(mixer->mixerChannel(1), selected);
		journal->undo(); QCOMPARE(mixer->soloChannel(), -1); QVERIFY(muted(3)); QVERIFY(!muted(1));
		journal->redo(); QCOMPARE(mixer->soloChannel(), 1); QCOMPARE(mixer->mixerChannel(1), selected);
		if (addChannel) { mixer->createChannel(); } else { mixer->deleteChannel(3); }
		const auto before = muteState();
		journal->undo(); QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(journal->isJournalling());
		QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo());
		QCOMPARE(mixer->soloChannel(), 1); QVERIFY(muteState() == before); QCOMPARE(mixer->mixerChannel(1), selected);
	}
	void malformedSoloJournalPreservesExistingControls_data()
	{
		QTest::addColumn<int>("fault");
		QTest::newRow("version") << 0;
		QTest::newRow("selected-identity") << 1;
		QTest::newRow("channel-identity") << 2;
		QTest::newRow("duplicate-channel") << 3;
		QTest::newRow("mute-value") << 4;
	}
	void malformedSoloJournalPreservesExistingControls()
	{
		QFETCH(int, fault); channels(2); auto mixer = Engine::mixer();
		const auto journal = Engine::projectJournal(); const auto previous = journal->isJournalling();
		const auto cleanup = qScopeGuard([&] { journal->setJournalling(previous); journal->clearJournal(); });
		journal->setJournalling(false); mute(0, true); solo(1, true);
		const auto first = mixer->mixerChannel(1), second = mixer->mixerChannel(2);
		QDomDocument document; auto root = document.createElement("journal"); document.appendChild(root);
		const auto valid = first->m_soloModel.saveJournalState(document, root);
		auto invalid = valid.cloneNode(true).toElement();
		if (fault == 0) { invalid.setAttribute("version", 2); }
		if (fault == 1) { invalid.setAttribute("selected", "4294967295"); }
		if (fault == 2) { invalid.firstChildElement("channel").setAttribute("soloId", first->m_soloModel.id()); }
		if (fault == 3) { invalid.appendChild(invalid.firstChildElement("channel").cloneNode(true)); }
		if (fault == 4) { invalid.lastChildElement("channel").setAttribute("muted", "true"); }
		solo(2, true); mute(1, false); const auto before = muteState();
		std::vector<bool> restoration;
		for (int i = 0; i < mixer->numChannels(); ++i) { restoration.push_back(mixer->mixerChannel(i)->m_muteBeforeSolo); }
		QString error; QVERIFY(!first->m_soloModel.restoreJournalState(invalid, error)); QVERIFY(!error.isEmpty());
		QCOMPARE(mixer->soloChannel(), 2); QVERIFY(muteState() == before);
		QCOMPARE(mixer->mixerChannel(1), first); QCOMPARE(mixer->mixerChannel(2), second);
		for (int i = 0; i < mixer->numChannels(); ++i)
		{
			QCOMPARE(mixer->mixerChannel(i)->m_soloModel.value(), i == 2);
			QCOMPARE(mixer->mixerChannel(i)->m_muteBeforeSolo, static_cast<bool>(restoration[i]));
		}
		error.clear(); QVERIFY(first->m_soloModel.restoreJournalState(valid, error)); QVERIFY(error.isEmpty());
		QCOMPARE(mixer->soloChannel(), 1); QVERIFY(!muted(0)); QVERIFY(!muted(1)); QVERIFY(muted(2));
		solo(1, false); QVERIFY(muted(0)); QVERIFY(!muted(1)); QVERIFY(!muted(2));
	}
	void rejectedJournalRestorationRetainsHistoryAndFlags_data()
	{
		QTest::addColumn<bool>("redo"); QTest::addColumn<int>("failure");
		for (const auto redo : {false, true})
		{
			for (int failure = 1; failure <= 3; ++failure)
			{
				QTest::newRow(qPrintable(QString("%1-failure-%2").arg(redo ? "redo" : "undo").arg(failure))) << redo << failure;
			}
		}
	}
	void rejectedJournalRestorationRetainsHistoryAndFlags()
	{
		QFETCH(bool, redo); QFETCH(int, failure); JournalFixture fixture;
		const auto journal = Engine::projectJournal(); const auto previous = journal->isJournalling();
		const auto cleanup = qScopeGuard([&] { journal->setJournalling(previous); journal->clearJournal(); });
		journal->clearJournal(); journal->setJournalling(true);
		fixture.value = 5; journal->addJournalCheckPoint(&fixture); fixture.value = 9;
		if (redo) { journal->undo(); QCOMPARE(fixture.value, 5); }
		fixture.failure = failure;
		if (redo) { journal->redo(); } else { journal->undo(); }
		QCOMPARE(fixture.value, redo ? 5 : 9); QVERIFY(fixture.observedJournallingDisabled);
		QVERIFY(journal->isJournalling()); QVERIFY(Engine::getSong()->hasErrors());
		QCOMPARE(journal->canUndo(), !redo); QCOMPARE(journal->canRedo(), redo);
		fixture.failure = 0; Engine::getSong()->clearErrors();
		if (redo) { journal->redo(); } else { journal->undo(); }
		QCOMPARE(fixture.value, redo ? 9 : 5); QVERIFY(!Engine::getSong()->hasErrors());
		QCOMPARE(journal->canUndo(), redo); QCOMPARE(journal->canRedo(), !redo); QVERIFY(journal->isJournalling());
	}
	void journalReentryCannotConsumeOrClearTheOuterHistory()
	{
		JournalFixture fixture; const auto journal = Engine::projectJournal(); const auto previous = journal->isJournalling();
		const auto cleanup = qScopeGuard([&] { journal->setJournalling(previous); journal->clearJournal(); });
		journal->clearJournal(); journal->setJournalling(true);
		fixture.value = 1; journal->addJournalCheckPoint(&fixture); fixture.value = 2;
		journal->addJournalCheckPoint(&fixture); fixture.value = 3; fixture.reenter = true;
		journal->undo(); QCOMPARE(fixture.value, 2); QVERIFY(journal->canUndo()); QVERIFY(journal->canRedo());
		QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(journal->isJournalling());
		fixture.reenter = false; Engine::getSong()->clearErrors();
		journal->undo(); QCOMPARE(fixture.value, 1); QVERIFY(!journal->canUndo()); QVERIFY(journal->canRedo());
		journal->redo(); QCOMPARE(fixture.value, 2); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void soloAffectsNativeStereoMixing()
	{
		channels(2); solo(1, true);
		fill(1, .25f, -.125f); fill(2, .5f, -.25f);
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(), SampleFrame{0.f, 0.f});
		Engine::mixer()->masterMix(output.data());
		QVERIFY(std::all_of(output.begin(), output.end(), [](const auto& frame) {
			return std::abs(frame[0] - .25f) < .000001f && std::abs(frame[1] + .125f) < .000001f;
		}));
		solo(1, false); fill(1, .25f, -.125f); fill(2, .5f, -.25f);
		std::fill(output.begin(), output.end(), SampleFrame{0.f, 0.f}); Engine::mixer()->masterMix(output.data());
		QVERIFY(std::all_of(output.begin(), output.end(), [](const auto& frame) {
			return std::abs(frame[0] - .75f) < .000001f && std::abs(frame[1] + .375f) < .000001f;
		}));
		QCOMPARE(Engine::audioEngine()->dspStatus().failure, AudioEngine::DspFailure::None);
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::MixerSoloTest)
#include "MixerSoloTest.moc"
