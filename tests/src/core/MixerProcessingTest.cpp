/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <vector>
#include "AudioEngine.h"
#include "AudioEngineWorkerThread.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Effect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

namespace lmms
{
namespace
{
const Plugin::Descriptor countDescriptor{"mixer-count-fixture", "Mixer count fixture", "Native bus processing counter", "LMMS tests",
	1, Plugin::Type::Effect, nullptr, "", nullptr};
class CountingEffect final : public Effect
{
public:
	explicit CountingEffect(EffectChain* chain) : Effect(&countDescriptor, chain, nullptr) { }
	std::atomic<int> calls{0};
	EffectControls* controls() override { return nullptr; }
protected:
	ProcessStatus processImpl(SampleFrame*, f_cnt_t) override { ++calls; return ProcessStatus::Continue; }
};

QDomElement channelXml(QDomDocument& document, QDomElement& mixer, const QString& index)
{
	auto channel = document.createElement("mixerchannel"); channel.setAttribute("num", index);
	channel.setAttribute("name", "Bus " + index); channel.setAttribute("volume", 1);
	channel.setAttribute("muted", 0); channel.setAttribute("soloed", 0); mixer.appendChild(channel); return channel;
}
void sendXml(QDomDocument& document, QDomElement& channel, const QString& receiver, float amount = 1.f)
{
	auto send = document.createElement("send"); send.setAttribute("channel", receiver);
	send.setAttribute("amount", amount); channel.appendChild(send);
}

void fill(MixerChannel* channel, float left, float right)
{
	auto& buffer = channel->m_buffer;
	for (int side = 0; side < 2; ++side)
	{
		std::fill_n(buffer.group(0).buffer(side), Engine::audioEngine()->framesPerPeriod(), side ? right : left);
		buffer.assumeNonSilent(side);
	}
	toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer()); buffer.updateAllSilenceFlags();
}
std::vector<SampleFrame> mix()
{
	std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(), SampleFrame{0.f, 0.f});
	Engine::mixer()->masterMix(output.data()); AutomatableModel::incrementPeriodCounter(); return output;
}
bool matches(const std::vector<SampleFrame>& output, float left, float right)
{
	return std::all_of(output.begin(), output.end(), [&](const auto& frame) {
		return std::abs(frame[0] - left) < 0.000001f && std::abs(frame[1] - right) < 0.000001f;
	});
}
}

class MixerProcessingTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
	{
		return {{"id", id}, {"op", op}, {"args", args}};
	}
	static QJsonObject manifest(const QString& session)
	{
		QFile file(session + "/session.json");
		return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
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
	void mutedSendersUseTheReceiversCurrentPeriodState_data()
	{
		QTest::addColumn<bool>("forward"); QTest::newRow("forward-reference") << true; QTest::newRow("backward-reference") << false;
	}
	void mutedSendersUseTheReceiversCurrentPeriodState()
	{
		QFETCH(bool, forward); auto mixer = Engine::mixer(); mixer->createChannel(); mixer->createChannel();
		const int sender = forward ? 1 : 2, receiver = forward ? 2 : 1;
		mixer->deleteChannelSend(sender, 0); QVERIFY(mixer->createChannelSend(sender, receiver));
		mixer->mixerChannel(sender)->m_muteModel.setValue(true);
		mixer->mixerChannel(receiver)->m_muteModel.setValue(true);
		QVERIFY(matches(mix(), 0.f, 0.f));
		for (int iteration = 0; iteration < 16; ++iteration)
		{
			mixer->mixerChannel(receiver)->m_muteModel.setValue(false);
			fill(mixer->mixerChannel(sender), 0.75f, 0.75f); // Muting must discard even an existing buffer.
			fill(mixer->mixerChannel(receiver), 0.25f, -0.125f);
			QVERIFY(matches(mix(), 0.25f, -0.125f));
			QCOMPARE(Engine::audioEngine()->dspStatus().failure, AudioEngine::DspFailure::None);
			mixer->mixerChannel(receiver)->m_muteModel.setValue(true);
			fill(mixer->mixerChannel(receiver), 0.25f, -0.125f); QVERIFY(matches(mix(), 0.f, 0.f));
		}
	}
	void masterMuteAndUnroutedBusesStillDrainNativeEffects_data()
	{
		QTest::addColumn<bool>("masterMuted"); QTest::addColumn<bool>("routed");
		QTest::newRow("muted-connected") << true << true; QTest::newRow("muted-unrouted") << true << false;
		QTest::newRow("audible-connected") << false << true; QTest::newRow("audible-unrouted") << false << false;
	}
	void masterMuteAndUnroutedBusesStillDrainNativeEffects()
	{
		QFETCH(bool, masterMuted); QFETCH(bool, routed); auto mixer = Engine::mixer(); mixer->createChannel();
		if (!routed) { mixer->deleteChannelSend(1, 0); }
		mixer->mixerChannel(0)->m_muteModel.setValue(masterMuted);
		const auto chain = &mixer->mixerChannel(1)->m_fxChain;
		auto effect = new CountingEffect(chain); chain->appendEffect(effect);
		const auto cleanup = qScopeGuard([&] { chain->removeEffect(effect); delete effect; });
		for (int iteration = 0; iteration < 8; ++iteration)
		{
			fill(mixer->mixerChannel(1), .25f, -.125f);
			QVERIFY(matches(mix(), !masterMuted && routed ? .25f : 0.f, !masterMuted && routed ? -.125f : 0.f));
			QCOMPARE(effect->calls.load(), iteration + 1);
			const auto workers = AudioEngineWorkerThread::schedulerStatus();
			QVERIFY(!workers.batchActive); QCOMPARE(workers.scanning, 0); QCOMPARE(workers.queued, 0);
		}
	}
	void convergingNativeRoutesMixOnceWithExactSendGains()
	{
		auto mixer = Engine::mixer(); for (int i = 0; i < 4; ++i) { mixer->createChannel(); }
		for (int i = 1; i < 4; ++i) { mixer->deleteChannelSend(i, 0); }
		QVERIFY(mixer->createChannelSend(1, 2, .5f)); QVERIFY(mixer->createChannelSend(1, 3, .25f));
		QVERIFY(mixer->createChannelSend(2, 4, .5f)); QVERIFY(mixer->createChannelSend(3, 4, .75f));
		std::vector<CountingEffect*> effects;
		const auto cleanup = qScopeGuard([&] {
			for (std::size_t i = 0; i < effects.size(); ++i) { mixer->mixerChannel(i)->m_fxChain.removeEffect(effects[i]); delete effects[i]; }
		});
		for (int i = 0; i < 5; ++i)
		{
			auto chain = &mixer->mixerChannel(i)->m_fxChain; auto effect = new CountingEffect(chain);
			chain->appendEffect(effect); effects.push_back(effect);
		}
		for (int iteration = 0; iteration < 64; ++iteration)
		{
			fill(mixer->mixerChannel(1), .4f, -.2f); QVERIFY(matches(mix(), .175f, -.0875f));
			for (const auto effect : effects) { QCOMPARE(effect->calls.load(), iteration + 1); }
			QCOMPARE(Engine::audioEngine()->dspStatus().failure, AudioEngine::DspFailure::None);
		}
	}
	void routeCreationValidatesNativeCallersAndPreservesExistingSends()
	{
		auto mixer = Engine::mixer(); mixer->createChannel(); mixer->createChannel();
		auto route = mixer->createChannelSend(1, 2, .25f); QVERIFY(route);
		const auto count = mixer->m_mixerRoutes.size();
		QCOMPARE(mixer->createRoute(mixer->mixerChannel(1), mixer->mixerChannel(2), .5f), route);
		QCOMPARE(mixer->m_mixerRoutes.size(), count); QCOMPARE(route->amount()->value(), .5f);
		QVERIFY(!mixer->createChannelSend(2, 1)); QVERIFY(!mixer->createChannelSend(1, 1));
		QVERIFY(!mixer->createChannelSend(0, 1)); QVERIFY(!mixer->createChannelSend(1, 99));
		QVERIFY(!mixer->createRoute(nullptr, mixer->mixerChannel(2), .5f));
		MixerChannel foreign(3, nullptr); QVERIFY(!mixer->createRoute(&foreign, mixer->mixerChannel(2), .5f));
		for (const auto invalid : {-1.f, 2.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
		{
			QVERIFY(!mixer->createChannelSend(1, 2, invalid)); QCOMPARE(route->amount()->value(), .5f);
		}
		mixer->deleteChannelSend(99, 1); mixer->deleteChannelSend(static_cast<MixerRoute*>(nullptr));
		QVERIFY(!mixer->channelSendModel(1, 99)); QVERIFY(mixer->isInfiniteLoop(99, 1));
		QCOMPARE(mixer->m_mixerRoutes.size(), count);
	}
	void cycleSearchVisitsConvergingGraphsWithoutEnumeratingEveryPath()
	{
		auto mixer = Engine::mixer(); constexpr int layers = 48;
		for (int i = 0; i < layers * 2 + 1; ++i) { mixer->createChannel(); }
		for (int layer = 0; layer + 1 < layers; ++layer)
		{
			for (int from = 0; from < 2; ++from)
			{
				for (int to = 0; to < 2; ++to) { QVERIFY(mixer->createChannelSend(1 + layer * 2 + from, 3 + layer * 2 + to)); }
			}
		}
		QVERIFY(!mixer->isInfiniteLoop(layers * 2 + 1, 1));
		QVERIFY(mixer->isInfiniteLoop(layers * 2, 1));
	}
	void invalidXmlRoutingDoesNotReplaceTheExistingMixer_data()
	{
		QTest::addColumn<QString>("problem");
		for (const auto row : {"negative-channel", "text-channel", "wrapped-channel", "negative-send", "text-send", "wrapped-send",
			"self", "master-send", "connected-cycle", "disconnected-cycle"}) { QTest::newRow(row) << QString(row); }
	}
	void invalidXmlRoutingDoesNotReplaceTheExistingMixer()
	{
		QFETCH(QString, problem); auto mixer = Engine::mixer(); mixer->createChannel();
		const auto prior = mixer->mixerChannel(1); prior->m_name = "Keep this bus";
		const auto send = mixer->channelSendModel(1, 0); send->setInitValue(.375f);
		QDomDocument document; auto root = document.createElement("mixer"); document.appendChild(root);
		QString index = "1", receiver = "0";
		if (problem == "negative-channel") { index = "-1"; }
		if (problem == "text-channel") { index = "invalid"; }
		if (problem == "wrapped-channel") { index = "65536"; }
		if (problem == "negative-send") { receiver = "-1"; }
		if (problem == "text-send") { receiver = "invalid"; }
		if (problem == "wrapped-send") { receiver = "65536"; }
		if (problem == "self") { receiver = "1"; }
		if (problem == "master-send") { index = "0"; receiver = "1"; }
		auto first = channelXml(document, root, index); sendXml(document, first, receiver);
		if (problem.endsWith("cycle"))
		{
			if (problem == "disconnected-cycle") { first.removeChild(first.firstChildElement("send")); }
			sendXml(document, first, "2"); auto second = channelXml(document, root, "2"); sendXml(document, second, "1");
		}
		mixer->loadSettings(root);
		QVERIFY(Engine::getSong()->hasErrors()); QCOMPARE(mixer->numChannels(), mix_ch_t{2});
		QCOMPARE(mixer->mixerChannel(1), prior); QCOMPARE(prior->m_name, QString("Keep this bus"));
		QCOMPARE(mixer->channelSendModel(1, 0), send); QCOMPARE(send->value(), .375f);
	}
	void forwardReferencesCommentsAndNativeRouteModelsRoundTrip()
	{
		QDomDocument document; auto root = document.createElement("mixer"); document.appendChild(root);
		auto second = channelXml(document, root, "2"); sendXml(document, second, "0", .75f);
		root.appendChild(document.createComment("Do not interpret this as master"));
		root.appendChild(document.createElement("externalannotation"));
		auto first = channelXml(document, root, "1"); sendXml(document, first, "2", .5f);
		auto master = channelXml(document, root, "0"); master.setAttribute("name", "Output");
		auto mixer = Engine::mixer(); mixer->loadSettings(root); QVERIFY(!Engine::getSong()->hasErrors());
		QCOMPARE(mixer->numChannels(), mix_ch_t{3}); QCOMPARE(mixer->mixerChannel(0)->m_name, QString("Output"));
		QCOMPARE(mixer->m_mixerRoutes.size(), std::size_t{2}); QVERIFY(!mixer->channelSendModel(1, 0));
		QCOMPARE(mixer->channelSendModel(1, 2)->value(), .5f); QCOMPARE(mixer->channelSendModel(2, 0)->value(), .75f);
		QDomDocument saved; auto restored = saved.createElement("mixer"); saved.appendChild(restored); mixer->saveSettings(saved, restored);
		mixer->loadSettings(restored); QVERIFY(!Engine::getSong()->hasErrors());
		fill(mixer->mixerChannel(1), .5f, -.25f); QVERIFY(matches(mix(), .1875f, -.09375f));
	}
	void malformedLiveDependenciesLatchAndRequireRepair_data()
	{
		QTest::addColumn<bool>("connected"); QTest::newRow("connected") << true; QTest::newRow("disconnected") << false;
	}
	void malformedLiveDependenciesLatchAndRequireRepair()
	{
		QFETCH(bool, connected); auto mixer = Engine::mixer(); mixer->createChannel(); mixer->createChannel();
		QVERIFY(mixer->createChannelSend(1, 2));
		if (!connected) { mixer->deleteChannelSend(1, 0); mixer->deleteChannelSend(2, 0); }
		// Represent corruption by a native caller bypassing the validated API.
		auto bad = new MixerRoute(mixer->mixerChannel(2), mixer->mixerChannel(1), 1.f);
		mixer->mixerChannel(2)->m_sends.push_back(bad); mixer->mixerChannel(1)->m_receives.push_back(bad);
		mixer->m_mixerRoutes.push_back(bad);
		const auto repair = qScopeGuard([&] { if (bad) { mixer->deleteChannelSend(bad); } });
		const auto engine = Engine::audioEngine(); const auto output = engine->renderNextPeriod();
		QVERIFY(std::all_of(output.begin(), output.end(), [](const auto& frame) { return frame[0] == 0.f && frame[1] == 0.f; }));
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::MixerDependency);
		const auto inspected = StudioCapabilities::execute("audio.processing.inspect", {}).value("result").toObject();
		QCOMPARE(inspected.value("failure").toString(), QString("mixer-dependency"));
		const auto failures = engine->dspStatus().failures; engine->renderNextPeriod(); QCOMPARE(engine->dspStatus().failures, failures);
		mixer->deleteChannelSend(bad); bad = nullptr; QVERIFY(engine->resetDspFailure()); engine->renderNextPeriod();
		QCOMPARE(engine->dspStatus().failure, AudioEngine::DspFailure::None);
	}
	void failedProjectRoutingLoadRollsBackTheNativeCheckpoint()
	{
		QTemporaryDir session; QVERIFY(session.isValid()); StudioRuntime runtime(session.path()); QString error;
		QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("bus", "mixer.add", {{"name", "Preserved bus"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("gain", "mixer.route", {{"from", 1}, {"to", 0}, {"amount", .375}})).value("ok").toBool());
		const auto before = manifest(session.path());
		DataFile project(DataFile::Type::SongProject); auto root = project.createElement("mixer"); project.content().appendChild(root);
		project.content().appendChild(project.createElement("trackcontainer"));
		auto first = channelXml(project, root, "1"); sendXml(project, first, "2");
		auto second = channelXml(project, root, "2"); sendXml(project, second, "1");
		const auto path = session.filePath("cyclic.mmp"); QVERIFY(project.writeFile(path));
		const auto result = runtime.process(request("invalid-load", "project.load", {{"path", path}}));
		QVERIFY(!result.value("ok").toBool()); QVERIFY(!Engine::getSong()->isLoadingProject());
		const auto after = manifest(session.path());
		// Rollback reinstantiates native objects, so it invalidates handles with
		// a new revision while preserving the committed musical history.
		QCOMPARE(after.value("revision").toInt(), before.value("revision").toInt() + 1);
		QCOMPARE(after.value("history"), before.value("history"));
		QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t{2});
		QCOMPARE(Engine::mixer()->mixerChannel(1)->m_name, QString("Preserved bus"));
		QCOMPARE(Engine::mixer()->channelSendModel(1, 0)->value(), .375f);
		QVERIFY(runtime.process(request("after-rollback", "mixer.edit", {{"channel", 1}, {"name", "Recovered bus"}})).value("ok").toBool());
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::MixerProcessingTest)
#include "MixerProcessingTest.moc"
