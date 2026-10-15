/* Native mixer topology, channel ordering and selector tests.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QSignalSpy>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "Controller.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Effect.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "MixHelpers.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;

namespace
{
QJsonObject xmlState(const QDomNode& node)
{
	QJsonObject attributes;
	const auto entries = node.attributes();
	for (int i = 0; i < entries.count(); ++i) { const auto entry = entries.item(i); attributes.insert(entry.nodeName(), entry.nodeValue()); }
	QJsonArray children;
	for (auto child = node.firstChild(); !child.isNull(); child = child.nextSibling()) { children.append(xmlState(child)); }
	return {{"type", int(node.nodeType())}, {"name", node.nodeName()}, {"value", node.nodeValue()}, {"attributes", attributes}, {"children", children}};
}
const Plugin::Descriptor fixtureDescriptor{"mixer-order-fixture", "Mixer order fixture", "Native channel identity fixture", "LMMS tests",
	1, Plugin::Type::Effect, nullptr, "", nullptr};
class IdentityEffect final : public Effect
{
public:
	explicit IdentityEffect(EffectChain* chain) : Effect(&fixtureDescriptor, chain, nullptr) {}
	EffectControls* controls() override { return nullptr; }
protected:
	ProcessStatus processImpl(SampleFrame*, f_cnt_t) override { return ProcessStatus::Continue; }
};
class FixedController final : public Controller
{
public:
	FixedController() : Controller(ControllerType::Lfo, nullptr, "Numeric channel controller") {}
	float currentValue(int) override { return .75f; }
};
}

class MixerStructureTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	Mixer* mixer() const { return Engine::mixer(); }
	Song* song() const { return Engine::getSong(); }
	static QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
	void channels(int count) { while (mixer()->numChannels() <= count) { mixer()->createChannel(); } }
	IntModel* selector(Track* track) const
	{
		if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { return instrument->mixerChannelModel(); }
		return static_cast<SampleTrack*>(track)->mixerChannelModel();
	}
	AudioBusHandle* bus(Track* track) const
	{
		if (auto instrument = dynamic_cast<InstrumentTrack*>(track)) { return instrument->audioBusHandle(); }
		return static_cast<SampleTrack*>(track)->audioBusHandle();
	}
	Track* track(bool sample, bool pattern)
	{
		return Track::create(sample ? Track::Type::Sample : Track::Type::Instrument,
			pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : song());
	}
	QDomDocument saved() const
	{
		QDomDocument document; auto node = document.createElement("mixer"); document.appendChild(node); mixer()->saveSettings(document, node); return document;
	}
	void verifyTopologyReentryIsRefused()
	{
		std::vector<MixerChannel*> before;
		for (int index = 0; index < mixer()->numChannels(); ++index) { before.push_back(mixer()->mixerChannel(index)); }
		const auto routes = mixer()->m_mixerRoutes;
		QVERIFY(!mixer()->deleteChannel(1)); QVERIFY(!mixer()->clearChannel(0)); QVERIFY(!mixer()->moveChannel(1, 2));
		QVERIFY_EXCEPTION_THROWN(mixer()->createChannel(), std::logic_error);
		QDomDocument empty; auto node = empty.createElement("mixer"); empty.appendChild(node);
		QVERIFY_EXCEPTION_THROWN(mixer()->loadSettings(node), std::logic_error);
		mixer()->clear();
		if (before.size() > 1)
		{
			QVERIFY(!mixer()->createChannelSend(1, 0, .125f)); QVERIFY(!mixer()->createRoute(before[1], before[0], .125f));
			mixer()->deleteChannelSend(1, 0);
		}
		if (!routes.empty()) { mixer()->deleteChannelSend(routes.front()); }
		QCOMPARE(static_cast<std::size_t>(mixer()->numChannels()), before.size()); QVERIFY(mixer()->m_mixerRoutes == routes);
		for (int index = 0; index < mixer()->numChannels(); ++index) { QCOMPARE(mixer()->mixerChannel(index), before[index]); }
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		song()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::audioEngine()->audioDev()->stopProcessing(); song()->stop(); song()->clearProject(); song()->clearErrors(); }
	void cleanup() { song()->clearProject(); }

	void channelRangesFollowNativeCreationAndDeletionWithoutViews_data()
	{
		QTest::addColumn<bool>("sample"); QTest::addColumn<bool>("pattern");
		QTest::newRow("song-instrument") << false << false; QTest::newRow("song-sample") << true << false;
		QTest::newRow("pattern-instrument") << false << true; QTest::newRow("pattern-sample") << true << true;
	}
	void channelRangesFollowNativeCreationAndDeletionWithoutViews()
	{
		QFETCH(bool, sample); QFETCH(bool, pattern); const auto native = track(sample, pattern); const auto model = selector(native);
		QCOMPARE(model->maxValue(), 0); channels(3); QCOMPARE(model->maxValue(), 3);
		QVERIFY(StudioCapabilities::execute("parameter.set", {{"parameter", static_cast<int>(model->id())}, {"value", 3}}).value("ok").toBool());
		QCOMPARE(model->value(), 3); QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), 3);
		model->setInitValue(3);
		mixer()->deleteChannel(2); QCOMPARE(model->maxValue(), 2); QCOMPARE(model->value(), 2); QCOMPARE(model->initValue(), 2); QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), 2);
		mixer()->deleteChannel(2); QCOMPARE(model->maxValue(), 1); QCOMPARE(model->value(), 0); QCOMPARE(model->initValue(), 0); QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), 0);
		mixer()->clear(); QCOMPARE(model->maxValue(), 0);
	}

	void movingChannelsPreservesGraphModelsEffectsAndEveryTrackAssignment_data()
	{
		QTest::addColumn<int>("from"); QTest::addColumn<int>("to"); QTest::addColumn<QString>("order");
		QTest::newRow("first-last") << 1 << 4 << QString("02341"); QTest::newRow("last-first") << 4 << 1 << QString("04123");
		QTest::newRow("middle-last") << 2 << 4 << QString("01342"); QTest::newRow("last-middle") << 4 << 2 << QString("01423");
		QTest::newRow("adjacent") << 2 << 3 << QString("01324"); QTest::newRow("same-index") << 2 << 2 << QString("01234");
	}
	void movingChannelsPreservesGraphModelsEffectsAndEveryTrackAssignment()
	{
		QFETCH(int, from); QFETCH(int, to); QFETCH(QString, order); channels(4);
		std::array<MixerChannel*, 5> original{}; std::array<jo_id_t, 5> ids{};
		for (int index = 0; index < 5; ++index) { original[index] = mixer()->mixerChannel(index); ids[index] = original[index]->m_volumeModel.id(); }
		original[2]->m_name = "Custom percussion"; original[2]->setColor(QColor("#ac48e9"));
		auto effect = new IdentityEffect(&original[2]->m_fxChain); original[2]->m_fxChain.appendEffect(effect);
		mixer()->deleteChannelSend(1, 0); auto route = mixer()->createChannelSend(1, 3, .25f); QVERIFY(route); const auto routeId = route->amount()->id();
		auto curveTrack = Track::create(Track::Type::Automation, song()); auto curve = static_cast<AutomationClip*>(curveTrack->createClip(0));
		QVERIFY(curve->addObject(&original[2]->m_volumeModel)); QVERIFY(curve->addObject(route->amount())); curve->putValue(0, .375f, false);
		std::vector<std::pair<Track*, MixerChannel*>> tracks;
		for (bool pattern : {false, true})
		{
			for (bool sample : {false, true})
			{
				for (int index = 0; index < 5; ++index) { auto native = track(sample, pattern); selector(native)->setInitValue(index); tracks.emplace_back(native, original[index]); }
			}
		}
		original[2]->m_soloModel.setValue(true); const auto originalName = original[2]->m_name;
		QSignalSpy moved(mixer(), &Mixer::channelMoved); QVERIFY(mixer()->moveChannel(from, to)); QCOMPARE(moved.size(), from == to ? 0 : 1);
		for (int index = 0; index < 5; ++index)
		{
			const auto previous = order.at(index).digitValue(); const auto channel = mixer()->mixerChannel(index);
			QCOMPARE(channel, original[previous]); QCOMPARE(channel->index(), index); QCOMPARE(channel->m_volumeModel.id(), ids[previous]);
			if (previous && previous != 2) { QCOMPARE(channel->m_name, Mixer::tr("Channel %1").arg(index)); }
		}
		QCOMPARE(original[2]->m_name, originalName); QCOMPARE(original[2]->color().value(), QColor("#ac48e9"));
		QCOMPARE(original[2]->m_fxChain.effects().front(), static_cast<Effect*>(effect)); QCOMPARE(mixer()->mixerChannel(mixer()->soloChannel()), original[2]);
		QCOMPARE(route->sender(), original[1]); QCOMPARE(route->receiver(), original[3]); QCOMPARE(route->amount()->id(), routeId);
		QCOMPARE(mixer()->channelSendModel(original[1]->index(), original[3]->index()), route->amount());
		QCOMPARE(curve->objects().at(0).data(), &original[2]->m_volumeModel); QCOMPARE(curve->objects().at(1).data(), route->amount()); QCOMPARE(curve->valueAt(0), .375f);
		for (const auto& [native, destination] : tracks)
		{
			QCOMPARE(selector(native)->value(), destination->index()); QCOMPARE(selector(native)->initValue(), destination->index());
			QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), destination->index());
		}
	}

	void invalidMovesPreserveTheNativeGraph_data()
	{
		QTest::addColumn<int>("from"); QTest::addColumn<int>("to");
		QTest::newRow("master-source") << 0 << 1; QTest::newRow("master-destination") << 1 << 0;
		QTest::newRow("negative-source") << -1 << 1; QTest::newRow("negative-destination") << 1 << -1;
		QTest::newRow("missing-source") << 4 << 1; QTest::newRow("missing-destination") << 1 << 4;
		QTest::newRow("overflow-source") << std::numeric_limits<int>::max() << 1; QTest::newRow("overflow-destination") << 1 << std::numeric_limits<int>::max();
	}
	void invalidMovesPreserveTheNativeGraph()
	{
		QFETCH(int, from); QFETCH(int, to); channels(3); const auto before = saved().toString(); QSignalSpy moved(mixer(), &Mixer::channelMoved);
		QVERIFY(!mixer()->moveChannel(from, to)); QCOMPARE(saved().toString(), before); QVERIFY(moved.isEmpty());
		mixer()->moveChannelRight(std::numeric_limits<int>::max()); mixer()->moveChannelLeft(std::numeric_limits<int>::min()); QCOMPARE(saved().toString(), before);
	}

	void editPrevalidatesEveryFieldBeforeMovingOrRenaming_data()
	{
		QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
		QTest::newRow("invalid-destination") << QString("to") << QJsonValue(0);
		QTest::newRow("fractional-destination") << QString("to") << QJsonValue(1.5);
		QTest::newRow("invalid-name") << QString("name") << QJsonValue(23);
		QTest::newRow("invalid-volume") << QString("volume") << QJsonValue(-1);
		QTest::newRow("invalid-muted") << QString("muted") << QJsonValue(1);
		QTest::newRow("invalid-solo") << QString("solo") << QJsonValue("yes");
	}
	void editPrevalidatesEveryFieldBeforeMovingOrRenaming()
	{
		QFETCH(QString, field); QFETCH(QJsonValue, value); channels(3); const auto before = saved().toString();
		QJsonObject args{{"channel", 1}, {"to", 3}, {"name", "Must not be applied"}}; args.insert(field, value);
		QVERIFY(!StudioCapabilities::execute("mixer.edit", args).value("ok").toBool()); QCOMPARE(saved().toString(), before);
		QVERIFY(!StudioCapabilities::execute("mixer.add", {{"name", 23}}).value("ok").toBool()); QCOMPARE(saved().toString(), before);
	}

	void linkedTrackSelectorsAreRemappedOnlyOnceAndRetainResetValues()
	{
		channels(4); auto a = track(false, false), b = track(true, true);
		selector(a)->setInitValue(1); selector(a)->setValue(2); selector(b)->setInitValue(1); selector(b)->linkToModel(selector(a));
		QVERIFY(mixer()->moveChannel(1, 4)); QCOMPARE(selector(a)->value(), 1); QCOMPARE(selector(b)->value(), 1);
		QCOMPARE(selector(a)->initValue(), 4); QCOMPARE(selector(b)->initValue(), 4);
		QCOMPARE(static_cast<int>(bus(a)->nextMixerChannel()), 1); QCOMPARE(static_cast<int>(bus(b)->nextMixerChannel()), 1);
		selector(a)->reset(); QCOMPARE(selector(a)->value(), 4); QCOMPARE(selector(b)->value(), 4);
	}

	void deletingChannelsRemapsLinkedAssignmentsAndResetValuesOnce()
	{
		channels(4); auto a = track(false, false), b = track(true, true);
		selector(a)->setInitValue(4); selector(b)->setInitValue(4); selector(b)->linkToModel(selector(a));
		mixer()->deleteChannel(1);
		QCOMPARE(selector(a)->value(), 3); QCOMPARE(selector(b)->value(), 3); QCOMPARE(selector(a)->maxValue(), 3); QCOMPARE(selector(b)->maxValue(), 3);
		QCOMPARE(selector(a)->initValue(), 3); QCOMPARE(selector(b)->initValue(), 3);
		mixer()->deleteChannel(3); QCOMPARE(selector(a)->value(), 0); QCOMPARE(selector(b)->value(), 0);
		QCOMPARE(selector(a)->initValue(), 0); QCOMPARE(selector(b)->initValue(), 0);
	}

	void numericRoutingAutomationAndControllersKeepTheirExistingSemantics()
	{
		channels(4); auto native = track(false, false); auto model = selector(native); model->setInitValue(1);
		auto curveTrack = Track::create(Track::Type::Automation, song()); auto curve = static_cast<AutomationClip*>(curveTrack->createClip(0));
		QVERIFY(curve->addObject(model)); curve->putValues(0, 1, 2, false, true); curve->putValue(48, 4, false);
		FixedController controller; auto connection = new ControllerConnection(&controller); model->setControllerConnection(connection);
		QCOMPARE(model->value(), 3); QVERIFY(mixer()->moveChannel(1, 4)); QCOMPARE(model->storedValue(), 4.f);
		QCOMPARE(model->value(), 3); QCOMPARE(model->controllerConnection(), connection); QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), 3);
		QCOMPARE(curve->objects().front().data(), model); QCOMPARE(curve->getTimeMap().value(0).getInValue(), 1.f);
		QCOMPARE(curve->getTimeMap().value(0).getOutValue(), 2.f); QCOMPARE(curve->valueAt(48), 4.f);
	}

	void changedRangesRefreshControlledAudioBusesEvenWhenStoredValuesAreUnchanged()
	{
		channels(3); auto a = track(false, false), b = track(true, true); FixedController controller;
		for (auto native : {a, b})
		{
			selector(native)->setInitValue(3); selector(native)->setControllerConnection(new ControllerConnection(&controller));
			QCOMPARE(selector(native)->storedValue(), 3.f); QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), 2);
		}
		mixer()->createChannel();
		for (auto native : {a, b})
		{
			QCOMPARE(selector(native)->storedValue(), 3.f); QCOMPARE(selector(native)->maxValue(), 4);
			QCOMPARE(selector(native)->value(), 3); QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), 3);
		}
		mixer()->deleteChannel(4);
		for (auto native : {a, b}) { QCOMPARE(selector(native)->maxValue(), 3); QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), 2); }
	}

	void reentrantStructuralChangesAreRefusedUntilTheMoveIsPublished()
	{
		channels(3); auto original = mixer()->mixerChannel(1); bool observed = false;
		const auto connection = connect(mixer(), &Mixer::channelMoved, this, [&](int from, int to) {
			QCOMPARE(from, 1); QCOMPARE(to, 3); QCOMPARE(mixer()->mixerChannel(3), original); observed = true;
			QVERIFY(!mixer()->moveChannel(3, 1)); QVERIFY(!mixer()->deleteChannel(3)); mixer()->clear(); QVERIFY(!mixer()->clearChannel(3));
			QVERIFY_EXCEPTION_THROWN(mixer()->createChannel(), std::logic_error);
			QCOMPARE(mixer()->numChannels(), mix_ch_t(4)); QCOMPARE(mixer()->mixerChannel(3), original);
		});
		QVERIFY(mixer()->moveChannel(1, 3)); QVERIFY(observed); disconnect(connection);
		QVERIFY(mixer()->moveChannel(3, 1)); QCOMPARE(mixer()->mixerChannel(1), original);
	}

	void callbacksCannotChangeAnActiveNativeTopologyTransition_data()
	{
		QTest::addColumn<QString>("transition");
		QTest::newRow("create-selector") << QString("create"); QTest::newRow("delete-selector") << QString("delete");
		QTest::newRow("reset-effects") << QString("reset"); QTest::newRow("clear-master") << QString("clear");
		QTest::newRow("load-master") << QString("load"); QTest::newRow("route-amount") << QString("route-update");
		QTest::newRow("route-destruction") << QString("route-delete"); QTest::newRow("route-solo-refresh") << QString("route-add");
		QTest::newRow("solo-mask") << QString("solo");
	}
	void callbacksCannotChangeAnActiveNativeTopologyTransition()
	{
		QFETCH(QString, transition); channels(3); const auto document = saved();
		const auto native = track(false, false); auto model = selector(native); model->setInitValue(3);
		int callbacks = 0; QMetaObject::Connection connection;
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); });
		const auto probe = [&] { ++callbacks; verifyTopologyReentryIsRefused(); };
		if (transition == "create" || transition == "delete")
		{
			connection = connect(model, &AutomatableModel::propertiesChanged, this, probe, Qt::DirectConnection);
			if (transition == "create") { QCOMPARE(mixer()->createChannel(), 4); QCOMPARE(model->maxValue(), 4); }
			else { QVERIFY(mixer()->deleteChannel(2)); QCOMPARE(model->value(), 2); QCOMPARE(model->maxValue(), 2); }
		}
		else if (transition == "reset" || transition == "clear" || transition == "load")
		{
			const auto selected = transition == "reset" ? 1 : 0;
			connection = connect(&mixer()->mixerChannel(selected)->m_fxChain, &EffectChain::aboutToClear, this, probe, Qt::DirectConnection);
			if (transition == "reset") { QVERIFY(mixer()->clearChannel(1)); }
			else if (transition == "clear") { mixer()->clear(); QCOMPARE(mixer()->numChannels(), mix_ch_t(1)); }
			else { mixer()->loadSettings(document.documentElement()); QCOMPARE(mixer()->numChannels(), mix_ch_t(4)); }
		}
		else if (transition == "route-update")
		{
			const auto amount = mixer()->channelSendModel(1, 0);
			connection = connect(amount, &AutomatableModel::dataChanged, this, probe, Qt::DirectConnection);
			const auto route = mixer()->createChannelSend(1, 0, .25f); QVERIFY(route); QCOMPARE(route->amount(), amount); QCOMPARE(amount->value(), .25f);
		}
		else if (transition == "route-delete")
		{
			connection = connect(mixer()->channelSendModel(1, 0), &QObject::destroyed, this, probe, Qt::DirectConnection);
			mixer()->deleteChannelSend(1, 0); QVERIFY(!mixer()->channelSendModel(1, 0));
		}
		else
		{
			if (transition == "route-add") { mixer()->mixerChannel(1)->m_soloModel.setValue(true); }
			connection = connect(&mixer()->mixerChannel(2)->m_muteModel, &AutomatableModel::dataChanged, this, probe, Qt::DirectConnection);
			if (transition == "route-add") { QVERIFY(mixer()->createChannelSend(1, 2)); }
			else { mixer()->mixerChannel(1)->m_soloModel.setValue(true); }
		}
		QVERIFY(callbacks > 0); disconnect(connection);
		mixer()->clear(); QCOMPARE(mixer()->numChannels(), mix_ch_t(1)); QCOMPARE(mixer()->createChannel(), 1);
		QVERIFY(mixer()->createChannelSend(1, 0, .5f)); QVERIFY(mixer()->deleteChannel(1));
	}

	void callbackExceptionsReleaseTheTopologyAdmissionGuard_data()
	{
		QTest::addColumn<QString>("transition");
		QTest::newRow("reset") << QString("reset"); QTest::newRow("load") << QString("load");
		QTest::newRow("route-update") << QString("route-update");
	}
	void callbackExceptionsReleaseTheTopologyAdmissionGuard()
	{
		QFETCH(QString, transition); channels(3); const auto document = saved(); QMetaObject::Connection connection;
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); });
		const auto fail = [] { throw std::runtime_error("Native topology callback fixture"); };
		if (transition == "route-update")
		{
			connection = connect(mixer()->channelSendModel(1, 0), &AutomatableModel::dataChanged, this, fail, Qt::DirectConnection);
			QVERIFY_EXCEPTION_THROWN(mixer()->createChannelSend(1, 0, .25f), std::runtime_error);
		}
		else
		{
			connection = connect(&mixer()->mixerChannel(0)->m_fxChain, &EffectChain::aboutToClear, this, fail, Qt::DirectConnection);
			if (transition == "reset") { QVERIFY_EXCEPTION_THROWN(mixer()->clearChannel(0), std::runtime_error); }
			else { QVERIFY_EXCEPTION_THROWN(mixer()->loadSettings(document.documentElement()), std::runtime_error); }
		}
		disconnect(connection);
		// Direct core exceptions can leave partial changes. Admission must recover
		// so StudioRuntime can restore its native checkpoint.
		mixer()->loadSettings(document.documentElement()); QCOMPARE(saved().toString(), document.toString());
		QCOMPARE(mixer()->createChannel(), 4); QVERIFY(mixer()->deleteChannel(4));
	}

	void foreignThreadsCannotChangeMixerRoutes()
	{
		channels(2); const auto document = saved().toString(); auto route = mixer()->createChannelSend(1, 0); QVERIFY(route);
		MixerRoute* added = route; MixerRoute* direct = route;
		std::thread worker([&] {
			added = mixer()->createChannelSend(1, 2); direct = mixer()->createRoute(mixer()->mixerChannel(1), mixer()->mixerChannel(2), .5f);
			mixer()->deleteChannelSend(1, 0); mixer()->deleteChannelSend(route);
		}); worker.join();
		QVERIFY(!added); QVERIFY(!direct); QCOMPARE(saved().toString(), document);
	}

	void foreignThreadCannotReorderChannels()
	{
		channels(3); const auto before = saved().toString(); bool result = true;
		std::thread worker([&] { result = mixer()->moveChannel(1, 3); }); worker.join();
		QVERIFY(!result); QCOMPARE(saved().toString(), before); QVERIFY(mixer()->moveChannel(1, 3));
	}

	void nativeMixerAudioIsInvariantUnderChannelReordering_data()
	{
		QTest::addColumn<bool>("forward"); QTest::newRow("forward") << true; QTest::newRow("backward") << false;
	}
	void nativeMixerAudioIsInvariantUnderChannelReordering()
	{
		QFETCH(bool, forward); channels(4); const auto a = mixer()->mixerChannel(1), b = mixer()->mixerChannel(2), d = mixer()->mixerChannel(4);
		mixer()->deleteChannelSend(1, 0); mixer()->deleteChannelSend(2, 0);
		QVERIFY(mixer()->createChannelSend(1, 3, .5f)); QVERIFY(mixer()->createChannelSend(2, 3, .25f));
		// These are static fixture gains, without a pending live fader ramp.
		mixer()->channelSendModel(3, 0)->setInitValue(.5f); mixer()->channelSendModel(4, 0)->setInitValue(.125f);
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		const auto fill = [frames](MixerChannel* channel, float left, float right) {
			auto& buffer = channel->m_buffer;
			for (int side = 0; side < 2; ++side) { std::fill_n(buffer.group(0).buffer(side), frames, side ? right : left); buffer.assumeNonSilent(side); }
			toInterleaved(buffer.groupBuffers(0), buffer.interleavedBuffer()); buffer.updateAllSilenceFlags();
		};
		const auto render = [&] {
			fill(a, .25f, -.125f); fill(b, .5f, .25f); fill(d, .25f, -.25f);
			std::vector<SampleFrame> output(frames, SampleFrame(0.f, 0.f)); mixer()->masterMix(output.data()); AutomatableModel::incrementPeriodCounter(); return output;
		};
		const auto before = render(); QVERIFY(mixer()->moveChannel(forward ? 1 : 4, forward ? 4 : 1)); const auto after = render();
		for (std::size_t frame = 0; frame < before.size(); ++frame)
		{
			QVERIFY(std::abs(before[frame][0] - .15625f) < 1e-6f); QVERIFY(std::abs(before[frame][1] + .03125f) < 1e-6f);
			QVERIFY(std::abs(before[frame][0] - after[frame][0]) < 1e-6f); QVERIFY(std::abs(before[frame][1] - after[frame][1]) < 1e-6f);
		}
		QCOMPARE(Engine::audioEngine()->dspStatus().failure, AudioEngine::DspFailure::None);
	}

	void nativeRemovalRetiresOnlyTheSelectedChannelAndItsRoutes_data()
	{
		QTest::addColumn<int>("selected");
		QTest::newRow("first") << 1; QTest::newRow("middle") << 2; QTest::newRow("last") << 3;
	}
	void nativeRemovalRetiresOnlyTheSelectedChannelAndItsRoutes()
	{
		QFETCH(int, selected); channels(3);
		const auto target = mixer()->mixerChannel(selected);
		QPointer<AutomatableModel> volume = &target->m_volumeModel;
		QPointer<Effect> effect = new IdentityEffect(&target->m_fxChain); target->m_fxChain.appendEffect(effect.data());
		QVERIFY(mixer()->createChannelSend(1, 2, .25f)); QVERIFY(mixer()->createChannelSend(2, 3, .5f));
		std::vector<std::pair<Track*, int>> assignments;
		for (bool pattern : {false, true})
		{
			for (bool sample : {false, true})
			{
				for (int index = 0; index <= 3; ++index)
				{
					auto native = track(sample, pattern); selector(native)->setInitValue(index); assignments.emplace_back(native, index);
				}
			}
		}
		target->m_soloModel.setValue(true);
		const auto result = StudioCapabilities::execute("mixer.remove", {{"channel", selected}});
		QVERIFY(result.value("ok").toBool()); QVERIFY(result.value("result").toObject().value("removed").toBool());
		QCOMPARE(mixer()->numChannels(), mix_ch_t(3)); QVERIFY(volume.isNull()); QVERIFY(effect.isNull()); QCOMPARE(mixer()->soloChannel(), -1);
		for (const auto& [native, previous] : assignments)
		{
			const int expected = previous == selected ? 0 : previous > selected ? previous - 1 : previous;
			QCOMPARE(selector(native)->value(), expected); QCOMPARE(selector(native)->initValue(), expected); QCOMPARE(selector(native)->maxValue(), 2);
			QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), expected);
		}
		for (auto route : mixer()->m_mixerRoutes)
		{
			QVERIFY(route->sender() != target); QVERIFY(route->receiver() != target);
			QCOMPARE(mixer()->mixerChannel(route->senderIndex()), route->sender()); QCOMPARE(mixer()->mixerChannel(route->receiverIndex()), route->receiver());
		}
		if (selected != 2)
		{
			const auto retained = mixer()->channelSendModel(1, 2); QVERIFY(retained); QCOMPARE(retained->value(), selected == 1 ? .5f : .25f);
		}
	}

	void nativeResetPreservesChannelBindingsAndTrackAssignments_data()
	{
		QTest::addColumn<int>("selected"); QTest::addColumn<bool>("ownSolo");
		QTest::newRow("master") << 0 << true; QTest::newRow("channel") << 2 << true;
		QTest::newRow("other-solo") << 2 << false;
	}
	void nativeResetPreservesChannelBindingsAndTrackAssignments()
	{
		QFETCH(int, selected); QFETCH(bool, ownSolo); channels(3);
		const auto target = mixer()->mixerChannel(selected); const auto id = target->m_volumeModel.id();
		target->setName("Reset probe"); target->setColor(QColor("#ac48e9")); target->m_volumeModel.setValue(.25f); target->m_muteModel.setValue(true);
		QPointer<Effect> effect = new IdentityEffect(&target->m_fxChain); target->m_fxChain.appendEffect(effect.data());
		QVERIFY(mixer()->createChannelSend(1, 2, .25f)); QVERIFY(mixer()->createChannelSend(2, 3, .5f));
		auto curveTrack = Track::create(Track::Type::Automation, song()); auto curve = static_cast<AutomationClip*>(curveTrack->createClip(0));
		QVERIFY(curve->addObject(&target->m_volumeModel)); curve->putValue(0, .375f, false);
		std::vector<Track*> assignments;
		for (bool pattern : {false, true})
		{
			for (bool sample : {false, true}) { auto native = track(sample, pattern); selector(native)->setInitValue(selected); assignments.push_back(native); }
		}
		mixer()->mixerChannel(ownSolo ? selected : 1)->m_soloModel.setValue(true);
		const auto result = StudioCapabilities::execute("mixer.reset", {{"channel", selected}});
		QVERIFY(result.value("ok").toBool()); QVERIFY(!result.value("result").toObject().value("removed").toBool());
		QCOMPARE(mixer()->numChannels(), mix_ch_t(4)); QCOMPARE(mixer()->mixerChannel(selected), target); QVERIFY(effect.isNull());
		QCOMPARE(target->m_volumeModel.id(), id); QCOMPARE(target->m_volumeModel.value(), 1.f); QVERIFY(!target->color().has_value());
		QCOMPARE(target->m_name, selected ? Mixer::tr("Channel %1").arg(selected) : Mixer::tr("Master"));
		QVERIFY(!target->m_soloModel.value()); QCOMPARE(mixer()->soloChannel(), ownSolo ? -1 : 1);
		if (!ownSolo) { mixer()->mixerChannel(1)->m_soloModel.setValue(false); }
		QVERIFY(!target->m_muteModel.value());
		QCOMPARE(curve->objects().at(0).data(), &target->m_volumeModel); QCOMPARE(curve->valueAt(0), .375f);
		QVERIFY(target->m_receives.empty()); QCOMPARE(target->m_sends.size(), selected ? std::size_t(1) : std::size_t(0));
		if (selected) { const auto masterSend = mixer()->channelSendModel(selected, 0); QVERIFY(masterSend); QCOMPARE(masterSend->value(), 1.f); QVERIFY(mixer()->channelSendModel(3, 0)); }
		else { QVERIFY(mixer()->channelSendModel(1, 2)); QVERIFY(mixer()->channelSendModel(2, 3)); }
		for (const auto native : assignments)
		{
			QCOMPARE(selector(native)->value(), selected); QCOMPARE(selector(native)->initValue(), selected);
			QCOMPARE(static_cast<int>(bus(native)->nextMixerChannel()), selected);
		}
	}

	void invalidRemovalAndResetPreserveTheNativeGraph_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("args");
		QTest::newRow("remove-master") << QString("mixer.remove") << QJsonObject{{"channel", 0}};
		QTest::newRow("remove-missing") << QString("mixer.remove") << QJsonObject{{"channel", 4}};
		QTest::newRow("reset-negative") << QString("mixer.reset") << QJsonObject{{"channel", -1}};
		QTest::newRow("reset-fractional") << QString("mixer.reset") << QJsonObject{{"channel", 1.5}};
		QTest::newRow("reset-missing") << QString("mixer.reset") << QJsonObject{};
		QTest::newRow("reset-extra") << QString("mixer.reset") << QJsonObject{{"channel", 1}, {"volume", .5}};
	}
	void invalidRemovalAndResetPreserveTheNativeGraph()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, args); channels(3); const auto before = saved().toString();
		QVERIFY(!StudioCapabilities::execute(operation, args).value("ok").toBool()); QCOMPARE(saved().toString(), before);
		bool removed = true, reset = true;
		std::thread foreign([&] { removed = mixer()->deleteChannel(1); reset = mixer()->clearChannel(1); }); foreign.join();
		QVERIFY(!removed); QVERIFY(!reset); QCOMPARE(saved().toString(), before);
	}

	void studioResetCallbackFailureRestoresNativeCheckpoint()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(runtime.process(request("bus", "mixer.add", {{"name", "Preserved bus"}})).value("ok").toBool());
		QVERIFY(runtime.process(request("level", "mixer.edit", {{"channel", 1}, {"volume", .25}, {"muted", true}})).value("ok").toBool());
		const auto before = xmlState(saved()); bool first = true;
		const auto connection = connect(&mixer()->mixerChannel(1)->m_fxChain, &EffectChain::aboutToClear, this, [&] {
			if (first) { first = false; throw std::runtime_error("Reset callback failed before native defaults"); }
		}, Qt::DirectConnection);
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); });
		const auto reset = request("reset-failure", "mixer.reset", {{"channel", 1}});
		const auto response = runtime.process(reset); QVERIFY(!response.value("ok").toBool());
		QVERIFY(response.value("error").toString().contains("Reset callback failed")); QVERIFY(!first); QCOMPARE(xmlState(saved()), before);
		QVERIFY(runtime.process(reset).value("replayed").toBool()); QCOMPARE(xmlState(saved()), before);
		QVERIFY(runtime.process(request("reset-retry", "mixer.reset", {{"channel", 1}})).value("ok").toBool());
		QCOMPARE(mixer()->mixerChannel(1)->m_volumeModel.value(), 1.f); QVERIFY(!mixer()->mixerChannel(1)->m_muteModel.value());
		QVERIFY(runtime.process(request("undo-reset", "session.undo")).value("ok").toBool()); QCOMPARE(xmlState(saved()), before);
	}

	void studioMovesSurviveNativeXmlHistoryReplayAndReopening()
	{
		QTemporaryDir directory; const auto session = directory.filePath("session"); QString error;
		{
			StudioRuntime runtime(session); QVERIFY2(runtime.open(error), qPrintable(error));
			QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool());
			for (int index = 1; index <= 3; ++index) { QVERIFY(runtime.process(request(QString("bus%1").arg(index), "mixer.add", {{"name", QString("Bus %1").arg(index)}})).value("ok").toBool()); }
			auto model = selector(song()->tracks().front());
			QVERIFY(runtime.process(request("assign", "parameter.set", {{"parameter", static_cast<int>(model->id())}, {"value", 1}})).value("ok").toBool());
			const auto move = request("move", "mixer.edit", {{"channel", 1}, {"to", 3}, {"name", "Moved bus"}, {"volume", .625}});
			const auto response = runtime.process(move); QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("channel").toInt(), 3);
			QCOMPARE(selector(song()->tracks().front())->value(), 3); QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool());
			QCOMPARE(mixer()->mixerChannel(1)->m_name, QString("Bus 1")); QCOMPARE(selector(song()->tracks().front())->value(), 1);
			QVERIFY(runtime.process(move).value("replayed").toBool()); QCOMPARE(selector(song()->tracks().front())->value(), 1);
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); QCOMPARE(selector(song()->tracks().front())->value(), 3);
			QVERIFY(runtime.process(request("save", "project.save", {{"path", directory.filePath("moved.mmp")}})).value("ok").toBool());
			QVERIFY(runtime.process(request("load", "project.load", {{"path", directory.filePath("moved.mmp")}})).value("ok").toBool());
			QCOMPARE(mixer()->mixerChannel(3)->m_name, QString("Moved bus")); QCOMPARE(selector(song()->tracks().front())->value(), 3);
			QCOMPARE(mixer()->mixerChannel(3)->m_volumeModel.displayName(), QString("Moved bus>") + Mixer::tr("Volume"));
		}
		song()->clearProject(); StudioRuntime recovered(session); QVERIFY2(recovered.open(error), qPrintable(error));
		QCOMPARE(mixer()->mixerChannel(3)->m_name, QString("Moved bus")); QCOMPARE(mixer()->mixerChannel(3)->m_volumeModel.value(), .625f);
		QCOMPARE(selector(song()->tracks().front())->value(), 3); QCOMPARE(selector(song()->tracks().front())->maxValue(), 3);
	}
};

QTEST_GUILESS_MAIN(MixerStructureTest)
#include "MixerStructureTest.moc"
