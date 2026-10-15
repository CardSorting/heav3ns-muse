/* Native effect-chain ownership, DSP ordering and studio history.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <array>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include "AudioBusHandle.h"
#include "AudioBuffer.h"
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;

namespace
{
const Plugin::Descriptor fixtureDescriptor{"effect-chain-fixture", "Effect chain fixture", "Native ordering fixture", "LMMS tests",
	1, Plugin::Type::Effect, nullptr, "", nullptr};
class AffineEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	explicit AffineEffect(Model* parent, float gain = 1.f, float bias = 0.f) : Effect(&fixtureDescriptor, parent, nullptr), gain(gain), bias(bias) {}
	float gain, bias;
	std::function<void()> saving;
	EffectControls* controls() override { return &m_controls; }
	void saveSettings(QDomDocument& document, QDomElement& node) override
	{
		if (saving) { saving(); }
		Effect::saveSettings(document, node);
	}
protected:
	ProcessStatus processImpl(SampleFrame* buffer, f_cnt_t frames) override
	{
		for (f_cnt_t frame = 0; frame < frames; ++frame)
		{
			for (int side = 0; side < 2; ++side) { buffer[frame][side] = buffer[frame][side] * gain + bias; }
		}
		return ProcessStatus::Continue;
	}
};

QDomDocument saved(EffectChain& chain)
{
	QDomDocument document; auto node = document.createElement("fxchain"); document.appendChild(node);
	chain.saveSettings(document, node); return document;
}
bool populateMissing(EffectChain& chain)
{
	for (int index = 0; index < 3; ++index)
	{
		QDomDocument document; auto node = document.createElement("effect"); document.appendChild(node);
		node.setAttribute("name", "unavailable-native-fixture"); node.setAttribute("identity", index);
		auto opaque = document.createElement("opaque-plugin-state"); opaque.setAttribute("value", QString("state-%1").arg(index)); node.appendChild(opaque);
		std::unique_ptr<Effect> effect = std::make_unique<DummyEffect>(&chain, node);
		if (!chain.appendEffect(effect)) { return false; }
	}
	return true;
}
QString identities(const EffectChain& chain)
{
	QString result;
	for (const auto effect : chain.effects())
	{
		const auto dummy = dynamic_cast<DummyEffect*>(effect);
		if (!dummy) { return "unexpected native effect"; }
		result += dummy->originalPluginData().attribute("identity");
	}
	return result;
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}

class EffectChainTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	EffectChain& master() const { return Engine::mixer()->mixerChannel(0)->m_fxChain; }
	void verifyReentryRefused(EffectChain& chain)
	{
		const auto before = chain.effects();
		std::unique_ptr<Effect> candidate = std::make_unique<AffineEffect>(nullptr); const auto pointer = candidate.get();
		QVERIFY(!chain.appendEffect(candidate)); QCOMPARE(candidate.get(), pointer);
		QVERIFY(!chain.appendEffect(pointer)); QVERIFY(!chain.clear()); QVERIFY(!chain.moveEffect(0, 0));
		QVERIFY(!chain.takeEffect(before.empty() ? nullptr : before.front()));
		QVERIFY(!chain.removeEffect(before.empty() ? nullptr : before.front()));
		QVERIFY(!chain.moveUp(before.empty() ? nullptr : before.front())); QVERIFY(!chain.moveDown(before.empty() ? nullptr : before.front()));
		QDomDocument document; const auto empty = document.createElement("fxchain");
		QVERIFY_EXCEPTION_THROWN(chain.loadSettings(empty), std::logic_error);
		QVERIFY(chain.effects() == before);
	}
	void verifyLockReleased()
	{
		bool acquired = false;
		std::thread worker([&] { const auto guard = Engine::audioEngine()->requestChangesGuard(); acquired = true; });
		worker.join(); QVERIFY(acquired); // CTest's timeout also detects a leaked native mutex.
	}
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->clearProject(); }

	void finalPositionMovesPreserveModelsAndChangeNativeDspOrder_data()
	{
		QTest::addColumn<int>("from"); QTest::addColumn<int>("to"); QTest::addColumn<QString>("order");
		QTest::newRow("first-last") << 0 << 2 << QString("120"); QTest::newRow("last-first") << 2 << 0 << QString("201");
		QTest::newRow("first-middle") << 0 << 1 << QString("102"); QTest::newRow("last-middle") << 2 << 1 << QString("021");
		QTest::newRow("middle-last") << 1 << 2 << QString("021"); QTest::newRow("unchanged") << 1 << 1 << QString("012");
	}
	void finalPositionMovesPreserveModelsAndChangeNativeDspOrder()
	{
		QFETCH(int, from); QFETCH(int, to); QFETCH(QString, order); EffectChain chain(nullptr);
		std::array<AffineEffect*, 3> original{new AffineEffect(&chain, 2.f, .125f), new AffineEffect(&chain, .5f, -.25f), new AffineEffect(&chain, 3.f, .5f)};
		for (const auto effect : original) { QVERIFY(chain.appendEffect(effect)); }
		const auto model = original[1]->autoQuitModel(); const auto id = model->id(); model->setValue(400.f);
		auto automationTrack = Track::create(Track::Type::Automation, Engine::getSong());
		auto curve = static_cast<AutomationClip*>(automationTrack->createClip(0)); QVERIFY(curve->addObject(model)); curve->putValue(0, 700.f, false);
		QSignalSpy changed(&chain, &Model::dataChanged); QVERIFY(chain.moveEffect(from, to)); QCOMPARE(changed.size(), from == to ? 0 : 1);
		QCOMPARE(model->id(), id); QCOMPARE(model->value(), 400.f); QCOMPARE(curve->objects().size(), 1); QCOMPARE(curve->objects().front().data(), static_cast<AutomatableModel*>(model));
		AudioBuffer buffer(Engine::audioEngine()->framesPerPeriod()); buffer.allocateInterleavedBuffer();
		for (auto& frame : buffer.interleavedBuffer().asSampleFrames()) { frame[0] = .125f; frame[1] = -.125f; }
		buffer.assumeNonSilent(0); buffer.assumeNonSilent(1); QVERIFY(chain.processAudioBuffer(buffer));
		for (int side = 0; side < 2; ++side)
		{
			float expected = side ? -.125f : .125f;
			for (int index = 0; index < 3; ++index)
			{
				const auto effect = original[order.at(index).digitValue()]; QCOMPARE(chain.effects()[index], static_cast<Effect*>(effect));
				expected = expected * effect->gain + effect->bias; QCOMPARE(effect->effectChain(), &chain);
			}
			for (const auto& frame : buffer.interleavedBuffer().asSampleFrames()) { QCOMPARE(frame[side], expected); }
		}
	}

	void emptyInvalidAndForeignEffectsDoNotChangeTheChain()
	{
		EffectChain chain(nullptr), other(nullptr); std::unique_ptr<Effect> outside = std::make_unique<AffineEffect>(&other);
		QVERIFY(!chain.moveUp(nullptr)); QVERIFY(!chain.moveDown(nullptr)); QVERIFY(!chain.moveEffect(0, 0));
		QVERIFY(!chain.takeEffect(nullptr)); QVERIFY(!chain.appendEffect(static_cast<Effect*>(nullptr))); QVERIFY(!chain.appendEffect(outside)); QVERIFY(outside);
		auto effect = new AffineEffect(&chain); QVERIFY(chain.appendEffect(effect)); QSignalSpy changed(&chain, &Model::dataChanged);
		QVERIFY(!chain.appendEffect(effect)); QVERIFY(!chain.removeEffect(outside.get())); QVERIFY(!chain.moveUp(outside.get())); QVERIFY(!chain.moveDown(outside.get()));
		for (const auto indices : {std::pair{-1, 0}, std::pair{0, -1}, std::pair{1, 0}, std::pair{0, 1}}) { QVERIFY(!chain.moveEffect(indices.first, indices.second)); }
		QVERIFY(chain.moveUp(effect)); QVERIFY(chain.moveDown(effect)); QCOMPARE(changed.size(), 0); QCOMPARE(chain.effects().size(), std::size_t(1));
	}

	void ownedDetachTransfersBothNativeAndQObjectOwnership()
	{
		EffectChain chain(nullptr), other(nullptr); std::unique_ptr<Effect> candidate = std::make_unique<AffineEffect>(nullptr);
		QPointer<Effect> effect = candidate.get(); QVERIFY(chain.appendEffect(candidate)); QVERIFY(!candidate); QCOMPARE(effect->parent(), static_cast<QObject*>(&chain));
		auto detached = chain.takeEffect(effect); QVERIFY(detached); QVERIFY(!effect->effectChain()); QVERIFY(!effect->parent()); QVERIFY(chain.effects().empty());
		QCOMPARE(saved(chain).documentElement().attribute("enabled"), QString("0"));
		QVERIFY(other.appendEffect(detached)); QVERIFY(!detached); QCOMPARE(effect->effectChain(), &other); QCOMPARE(effect->parent(), static_cast<QObject*>(&other));
		QVERIFY(other.clear()); QVERIFY(effect.isNull());
	}

	void legacyDetachRetainsQObjectLifetimeUntilCallerOrParentDeletes()
	{
		QPointer<Effect> effect;
		{
			EffectChain chain(nullptr); effect = new AffineEffect(&chain); QVERIFY(chain.appendEffect(effect)); QVERIFY(chain.removeEffect(effect));
			QVERIFY(effect); QVERIFY(!effect->effectChain()); QCOMPARE(effect->parent(), static_cast<QObject*>(&chain)); QVERIFY(chain.effects().empty());
		}
		QVERIFY(effect.isNull());
	}

	void transitionCallbacksCannotReenterTheChainOrRetireItsMixer_data()
	{
		QTest::addColumn<QString>("operation");
		QTest::newRow("append") << QString("append"); QTest::newRow("detach") << QString("detach"); QTest::newRow("move") << QString("move");
		QTest::newRow("clear") << QString("clear"); QTest::newRow("load") << QString("load"); QTest::newRow("serialize") << QString("save");
	}
	void transitionCallbacksCannotReenterTheChainOrRetireItsMixer()
	{
		QFETCH(QString, operation); Engine::mixer()->createChannel(); Engine::mixer()->createChannel();
		auto& chain = Engine::mixer()->mixerChannel(1)->m_fxChain; QVERIFY(populateMissing(chain)); bool observed = false;
		const auto observer = [&] {
			observed = true; verifyReentryRefused(chain);
			QVERIFY(!Engine::mixer()->deleteChannel(1)); QVERIFY(!Engine::mixer()->clearChannel(1)); QVERIFY(!Engine::mixer()->moveChannel(1, 2));
			QVERIFY_EXCEPTION_THROWN(Engine::mixer()->createChannel(), std::logic_error); Engine::mixer()->clear(); QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(3));
		};
		AffineEffect* serializing = nullptr;
		if (operation == "save") { serializing = new AffineEffect(&chain); QVERIFY(chain.appendEffect(serializing)); serializing->saving = observer; }
		void (EffectChain::*signal)() = &EffectChain::dataChanged;
		if (operation == "clear" || operation == "load") { signal = &EffectChain::aboutToClear; }
		const auto connection = connect(&chain, signal, this, observer, Qt::DirectConnection);
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); if (serializing) { serializing->saving = {}; } });
		if (operation == "append") { std::unique_ptr<Effect> effect = std::make_unique<AffineEffect>(nullptr); QVERIFY(chain.appendEffect(effect)); }
		else if (operation == "detach") { QVERIFY(chain.takeEffect(chain.effects().front())); }
		else if (operation == "move") { QVERIFY(chain.moveEffect(0, 2)); }
		else if (operation == "clear") { QVERIFY(chain.clear()); }
		else if (operation == "load") { QDomDocument document; const auto node = document.createElement("fxchain"); chain.loadSettings(node); }
		else { saved(chain); }
		QVERIFY(observed); disconnect(connection); if (serializing) { serializing->saving = {}; }
		QVERIFY(chain.clear()); serializing = nullptr; QVERIFY(Engine::mixer()->deleteChannel(1)); verifyLockReleased();
	}

	void foreignThreadCannotMutateOrSerializeOwnerModels()
	{
		EffectChain chain(nullptr); QVERIFY(populateMissing(chain)); const auto before = chain.effects(); bool refused = false, saveRefused = false, loadRefused = false;
		std::unique_ptr<Effect> candidate = std::make_unique<AffineEffect>(nullptr);
		std::thread worker([&] {
			refused = !chain.appendEffect(candidate) && !chain.takeEffect(before.front()) && !chain.removeEffect(before.front())
				&& !chain.clear() && !chain.moveEffect(0, 2) && !chain.moveUp(before.front()) && !chain.moveDown(before.front());
			QDomDocument document; auto node = document.createElement("fxchain");
			try { chain.saveSettings(document, node); } catch (const std::logic_error&) { saveRefused = true; }
			try { chain.loadSettings(node); } catch (const std::logic_error&) { loadRefused = true; }
		});
		worker.join(); QVERIFY(refused); QVERIFY(saveRefused); QVERIFY(loadRefused); QVERIFY(candidate); QVERIFY(chain.effects() == before);
		QVERIFY(chain.moveEffect(0, 2)); verifyLockReleased();
	}

	void observerExceptionsReleaseLocksAndPreservePublishedOwnership_data()
	{
		QTest::addColumn<QString>("operation");
		QTest::newRow("append") << QString("append"); QTest::newRow("detach") << QString("detach");
		QTest::newRow("move") << QString("move"); QTest::newRow("clear") << QString("clear"); QTest::newRow("load") << QString("load"); QTest::newRow("save") << QString("save");
	}
	void observerExceptionsReleaseLocksAndPreservePublishedOwnership()
	{
		QFETCH(QString, operation); EffectChain chain(nullptr); QVERIFY(populateMissing(chain));
		const auto fail = [] { throw std::runtime_error("Effect observer failure"); };
		AffineEffect* serializer = nullptr;
		if (operation == "save") { serializer = new AffineEffect(&chain); QVERIFY(chain.appendEffect(serializer)); serializer->saving = fail; }
		void (EffectChain::*signal)() = &EffectChain::dataChanged;
		if (operation == "clear" || operation == "load") { signal = &EffectChain::aboutToClear; }
		const auto connection = connect(&chain, signal, this, fail, Qt::DirectConnection);
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); if (serializer) { serializer->saving = {}; } });
		QPointer<Effect> effect = chain.effects().front(); std::unique_ptr<Effect> candidate = std::make_unique<AffineEffect>(nullptr); QPointer<Effect> appended = candidate.get();
		if (operation == "append") { QVERIFY_EXCEPTION_THROWN(chain.appendEffect(candidate), std::runtime_error); QVERIFY(!candidate); QVERIFY(appended); QCOMPARE(chain.effects().back(), appended.data()); }
		else if (operation == "detach") { QVERIFY_EXCEPTION_THROWN(chain.takeEffect(effect), std::runtime_error); QVERIFY(effect.isNull()); QCOMPARE(chain.effects().size(), std::size_t(2)); }
		else if (operation == "move") { QVERIFY_EXCEPTION_THROWN(chain.moveEffect(0, 2), std::runtime_error); QCOMPARE(chain.effects().back(), effect.data()); }
		else if (operation == "clear") { QVERIFY_EXCEPTION_THROWN(chain.clear(), std::runtime_error); QCOMPARE(chain.effects().size(), std::size_t(3)); }
		else if (operation == "load") { const auto before = saved(chain); QVERIFY_EXCEPTION_THROWN(chain.loadSettings(before.documentElement()), std::runtime_error); QCOMPARE(chain.effects().size(), std::size_t(3)); }
		else { QVERIFY_EXCEPTION_THROWN(saved(chain), std::runtime_error); }
		disconnect(connection); if (serializer) { serializer->saving = {}; }
		verifyLockReleased(); QVERIFY(chain.clear()); serializer = nullptr; QVERIFY(chain.effects().empty());
		if (operation == "append") { QVERIFY(appended.isNull()); }
	}

	void missingPluginXmlRetainsOrderOpaqueDataAndChainOwnership()
	{
		EffectChain chain(nullptr); QVERIFY(populateMissing(chain)); QVERIFY(chain.moveEffect(0, 2));
		const auto original = static_cast<DummyEffect*>(chain.effects().front())->originalPluginData(); const auto originalParent = original.parentNode();
		const auto before = saved(chain); const auto beforeText = before.toString(); QVERIFY(original.parentNode() == originalParent);
		QPointer<Effect> retired = chain.effects().front(); chain.loadSettings(before.documentElement()); QVERIFY(retired.isNull());
		QCOMPARE(identities(chain), QString("120")); QCOMPARE(saved(chain).toString(), beforeText); QCOMPARE(before.toString(), beforeText);
		for (const auto effect : chain.effects()) { QCOMPARE(effect->effectChain(), &chain); QCOMPARE(effect->parent(), static_cast<QObject*>(&chain)); }
		QVERIFY(chain.clear()); QCOMPARE(saved(chain).documentElement().attribute("enabled"), QString("0"));
	}

	void capabilityMovesAndClearsEveryNativeChainKind_data()
	{
		QTest::addColumn<QString>("kind");
		QTest::newRow("mixer") << QString("mixer"); QTest::newRow("song-instrument") << QString("instrument"); QTest::newRow("song-sample") << QString("sample");
		QTest::newRow("pattern-instrument") << QString("pattern-instrument"); QTest::newRow("pattern-sample") << QString("pattern-sample");
	}
	void capabilityMovesAndClearsEveryNativeChainKind()
	{
		QFETCH(QString, kind); EffectChain* chain = &master(); QJsonObject selector{{"channel", 0}};
		if (kind != "mixer")
		{
			const bool pattern = kind.startsWith("pattern-"); const bool sample = kind.endsWith("sample");
			const auto track = Track::create(sample ? Track::Type::Sample : Track::Type::Instrument,
				pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : Engine::getSong());
			chain = sample ? static_cast<SampleTrack*>(track)->audioBusHandle()->effects() : static_cast<InstrumentTrack*>(track)->audioBusHandle()->effects();
			selector = {{"container", pattern ? "pattern" : "song"}, {"track", 0}};
		}
		QVERIFY(populateMissing(*chain)); auto args = selector; args.insert("effect", 0); args.insert("to", 2);
		const auto moved = StudioCapabilities::execute("effect.move", args); QVERIFY(moved.value("ok").toBool()); QCOMPARE(moved.value("result").toObject().value("effect").toInt(), 2); QCOMPARE(identities(*chain), QString("120"));
		args.remove("to"); args.insert("effect", 2); args.insert("direction", "down"); QVERIFY(StudioCapabilities::execute("effect.move", args).value("ok").toBool()); QCOMPARE(identities(*chain), QString("120"));
		args.insert("direction", "up"); QVERIFY(StudioCapabilities::execute("effect.move", args).value("ok").toBool()); QCOMPARE(identities(*chain), QString("102"));
		QPointer<Effect> removed = chain->effects()[1]; args = selector; args.insert("effect", 1); QVERIFY(StudioCapabilities::execute("effect.remove", args).value("ok").toBool()); QVERIFY(removed.isNull());
		const auto cleared = StudioCapabilities::execute("effect.clear", selector); QVERIFY(cleared.value("ok").toBool()); QCOMPARE(cleared.value("result").toObject().value("removed").toInt(), 2);
		QVERIFY(chain->effects().empty()); QCOMPARE(saved(*chain).documentElement().attribute("enabled"), QString("0"));
		QCOMPARE(StudioCapabilities::execute("effect.clear", selector).value("result").toObject().value("removed").toInt(-1), 0);
	}

	void invalidMoveArgumentsPreserveTheChain_data()
	{
		QTest::addColumn<QJsonObject>("extra");
		QTest::newRow("neither") << QJsonObject{}; QTest::newRow("both") << QJsonObject{{"to", 1}, {"direction", "up"}};
		QTest::newRow("negative") << QJsonObject{{"to", -1}}; QTest::newRow("outside") << QJsonObject{{"to", 3}};
		QTest::newRow("fractional") << QJsonObject{{"to", 1.5}}; QTest::newRow("string") << QJsonObject{{"to", "1"}};
		QTest::newRow("bad-direction") << QJsonObject{{"direction", "left"}}; QTest::newRow("null-direction") << QJsonObject{{"direction", QJsonValue::Null}};
	}
	void invalidMoveArgumentsPreserveTheChain()
	{
		QFETCH(QJsonObject, extra); QVERIFY(populateMissing(master())); const auto before = master().effects();
		extra.insert("effect", 0); extra.insert("channel", 0); QVERIFY(!StudioCapabilities::execute("effect.move", extra).value("ok").toBool()); QVERIFY(master().effects() == before);
	}

	void studioEffectEditsSurviveHistoryReplayAndReopening_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QString>("expected");
		QTest::newRow("move") << QString("effect.move") << QString("120"); QTest::newRow("remove") << QString("effect.remove") << QString("12");
		QTest::newRow("clear") << QString("effect.clear") << QString("");
	}
	void studioEffectEditsSurviveHistoryReplayAndReopening()
	{
		QFETCH(QString, operation); QFETCH(QString, expected); QTemporaryDir directory; const auto session = directory.filePath("session"); QString error;
		{
			StudioRuntime runtime(session); QVERIFY2(runtime.open(error), qPrintable(error)); QVERIFY(populateMissing(master()));
			QVERIFY(runtime.process(request("seed", "mixer.edit", {{"channel", 0}, {"name", "Effect fixture"}})).value("ok").toBool());
			QJsonObject args{{"channel", 0}}; if (operation != "effect.clear") { args.insert("effect", 0); } if (operation == "effect.move") { args.insert("to", 2); }
			const auto edit = request("edit", operation, args); QVERIFY(runtime.process(edit).value("ok").toBool()); QCOMPARE(identities(master()), expected);
			QVERIFY(runtime.process(request("undo", "session.undo")).value("ok").toBool()); QCOMPARE(identities(master()), QString("012"));
			QVERIFY(runtime.process(edit).value("replayed").toBool()); QCOMPARE(identities(master()), QString("012"));
			QVERIFY(runtime.process(request("redo", "session.redo")).value("ok").toBool()); QCOMPARE(identities(master()), expected);
			QVERIFY(runtime.process(request("save", "project.save", {{"path", directory.filePath("effects.mmp")}})).value("ok").toBool());
			QVERIFY(runtime.process(request("load", "project.load", {{"path", directory.filePath("effects.mmp")}})).value("ok").toBool()); QCOMPARE(identities(master()), expected);
		}
		Engine::getSong()->clearProject(); StudioRuntime recovered(session); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(identities(master()), expected);
	}

	void studioRollsBackAndReplaysFailedEffectTransitions_data()
	{
		QTest::addColumn<QString>("operation");
		QTest::newRow("move") << QString("effect.move"); QTest::newRow("remove") << QString("effect.remove"); QTest::newRow("clear") << QString("effect.clear");
	}
	void studioRollsBackAndReplaysFailedEffectTransitions()
	{
		QFETCH(QString, operation); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.filePath("session")); QVERIFY2(runtime.open(error), qPrintable(error));
		QVERIFY(populateMissing(master())); QVERIFY(runtime.process(request("seed", "mixer.edit", {{"channel", 0}, {"name", "Effects before failure"}})).value("ok").toBool());
		const auto before = saved(master()).toString(); bool first = true;
		void (EffectChain::*signal)() = &EffectChain::dataChanged; if (operation == "effect.clear") { signal = &EffectChain::aboutToClear; }
		const auto connection = connect(&master(), signal, this, [&] { if (first) { first = false; throw std::runtime_error("Native effect observer failed"); } }, Qt::DirectConnection);
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); });
		QJsonObject args{{"channel", 0}}; if (operation != "effect.clear") { args.insert("effect", 0); } if (operation == "effect.move") { args.insert("to", 2); }
		const auto edit = request("failed-effect-edit", operation, args); const auto response = runtime.process(edit);
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("Native effect observer failed")); QVERIFY(!first);
		QCOMPARE(saved(master()).toString(), before); QVERIFY(runtime.process(edit).value("replayed").toBool()); QCOMPARE(saved(master()).toString(), before);
		QVERIFY(runtime.process(request("retry", operation, args)).value("ok").toBool());
		QVERIFY(runtime.process(request("undo-retry", "session.undo")).value("ok").toBool()); QCOMPARE(saved(master()).toString(), before); verifyLockReleased();
	}
};

QTEST_GUILESS_MAIN(EffectChainTest)
#include "EffectChainTest.moc"
