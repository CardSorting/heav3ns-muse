/* Native automation target lifetime and controller ownership.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <memory>
#include <new>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"

using namespace lmms;
namespace lmms
{
class SongAutomationOwnershipTestAccess
{
public:
	static void tick(TimePos time = 0)
	{
		const auto changes = Engine::audioEngine()->requestChangesGuard();
		std::vector<QPointer<Track>> tracks;
		for (auto track : Engine::getSong()->tracks()) { tracks.emplace_back(track); }
		Engine::getSong()->processAutomations(tracks, time, 32);
	}
};
}
namespace
{
void phases()
{ QTest::addColumn<bool>("stop"); QTest::newRow("next-tick") << false; QTest::newRow("native-stop") << true; }
void control(AutomatableModel* model)
{
	auto controller = new MidiController(nullptr); controller->midiPort()->setInputController(74);
	controller->processInEvent(MidiEvent(MidiControlChange, 0, 74, 63), {});
	Controller::triggerFrameCounter(); controller->currentValue(0); Controller::triggerFrameCounter(); controller->currentValue(0);
	model->setControllerConnection(new ControllerConnection(controller));
}
AutomationClip* curve(AutomatableModel* model)
{
	auto track = new AutomationTrack(Engine::getSong()); auto clip = static_cast<AutomationClip*>(track->createClip(0));
	clip->addObject(model); clip->putValue(0, 25, false); clip->setAutoResize(false); clip->changeLength(192); return clip;
}
}

class SongAutomationOwnershipTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void retiredTrackModelCannotBeReadByLaterAutomationOrStop_data() { phases(); }
	void nativeAutomationPropagatesAfterALinkedPeerRetiresItself()
	{
		auto first = new InstrumentTrack(Engine::getSong()), retired = new InstrumentTrack(Engine::getSong()), last = new InstrumentTrack(Engine::getSong());
		QVERIFY(first->volumeModel()->linkToModel(retired->volumeModel())); QVERIFY(first->volumeModel()->linkToModel(last->volumeModel()));
		auto native = curve(first->volumeModel()); native->putValue(0, 70, false); const QPointer<AutomatableModel> old(retired->volumeModel());
		QObject::connect(retired->volumeModel(), &AutomatableModel::dataChanged, this, [retired] { delete retired; }, Qt::DirectConnection);
		Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick(); QVERIFY(!old); QCOMPARE(first->volumeModel()->storedValue(), 70.f); QCOMPARE(last->volumeModel()->storedValue(), 70.f);
	}
	void retiredTrackModelCannotBeReadByLaterAutomationOrStop()
	{
		QFETCH(bool, stop); auto instrument = new InstrumentTrack(Engine::getSong()); const QPointer<AutomatableModel> target(instrument->volumeModel());
		control(target); curve(target); Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick(); QVERIFY(!target->useControllerValue());
		delete instrument; QVERIFY(!target);
		if (stop) { Engine::getSong()->stop(); } else { SongAutomationOwnershipTestAccess::tick(1); }
		QVERIFY(!Engine::getSong()->automatedValuesAt(1).contains(nullptr));
	}

	void reusedAddressDoesNotInheritPriorAutomationOwnership_data() { phases(); }
	void reusedAddressDoesNotInheritPriorAutomationOwnership()
	{
		QFETCH(bool, stop); alignas(FloatModel) unsigned char storage[sizeof(FloatModel)];
		auto model = new (storage) FloatModel(42, 0, 100, 1, nullptr); const QPointer<AutomatableModel> original(model);
		auto destroy = qScopeGuard([&] { if (model) { model->~FloatModel(); } }); control(model); curve(model); Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick();
		QVERIFY(!model->useControllerValue()); model->~FloatModel(); model = nullptr; QVERIFY(!original); model = new (storage) FloatModel(42, 0, 100, 1, nullptr);
		QSignalSpy changed(model, &AutomatableModel::dataChanged);
		if (stop) { Engine::getSong()->stop(); } else { SongAutomationOwnershipTestAccess::tick(1); }
		QCOMPARE(changed.size(), 0); QCOMPARE(model->value(), 42.f);
	}

	void removingCurveReturnsLiveModelToNativeController_data() { phases(); }
	void removingCurveReturnsLiveModelToNativeController()
	{
		QFETCH(bool, stop); FloatModel model(42, 0, 100, 1, nullptr); control(&model); auto clip = curve(&model);
		Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick(); QVERIFY(!model.useControllerValue()); QCOMPARE(model.value(), 25.f);
		delete clip;
		if (stop) { Engine::getSong()->stop(); } else { SongAutomationOwnershipTestAccess::tick(1); }
		QVERIFY(model.useControllerValue()); QVERIFY(model.value() > 25.f); QCOMPARE(model.storedValue(), 25.f);
	}

	void callbackRetirementSkipsAnotherSnapshottedTarget()
	{
		auto a = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr), b = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr);
		AutomatedValueMap order; order.insert(a.get(), 0); order.insert(b.get(), 0);
		if (order.firstKey() != a.get()) { a.swap(b); } control(a.get()); control(b.get());
		auto clip = curve(a.get()); QVERIFY(clip->addObject(b.get())); const QPointer<AutomatableModel> retired(b.get());
		bool deleted = false; const auto connection = connect(a.get(), &AutomatableModel::dataChanged, this, [&] {
			if (!a->useControllerValue() && b) { deleted = true; b.reset(); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick(); QVERIFY(deleted); QVERIFY(!retired); QCOMPARE(a->value(), 25.f);
		SongAutomationOwnershipTestAccess::tick(1); Engine::getSong()->stop(); QVERIFY(a->useControllerValue());
	}

	void nullLegacyTargetsAreOmittedFromNativeCollections_data()
	{
		QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true;
	}
	void nullLegacyTargetsAreOmittedFromNativeCollections()
	{
		QFETCH(bool, pattern); auto model = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr); AutomationClip* clip;
		if (pattern)
		{
			new PatternTrack(Engine::getSong()); auto track = Track::create(Track::Type::Automation, Engine::patternStore());
			clip = static_cast<AutomationClip*>(track->getClip(0)); QVERIFY(clip->addObject(model.get())); clip->putValue(0, 25, false);
		}
		else { clip = curve(model.get()); }
		// A legacy/copied null reference can remain when its old destruction
		// subscription was absent. Production collectors must not emit null keys.
		QObject::disconnect(model.get(), nullptr, clip, nullptr); model.reset(); QVERIFY(!clip->objects().empty()); QVERIFY(!clip->objects().front());
		const auto values = pattern ? Engine::patternStore()->automatedValuesAt(0, 0) : Engine::getSong()->automatedValuesAt(0);
		QVERIFY(values.isEmpty()); QVERIFY(!values.contains(nullptr));
	}

	void projectReplacementForgetsPriorLiveOwnership()
	{
		FloatModel previous(42, 0, 100, 1, nullptr); control(&previous); curve(&previous); Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick();
		QVERIFY(!previous.useControllerValue()); Engine::getSong()->clearProject(); QVERIFY(previous.useControllerValue()); previous.setUseControllerValue(false);
		FloatModel current(42, 0, 100, 1, nullptr); control(&current); curve(&current); Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick();
		QVERIFY(!previous.useControllerValue()); QVERIFY(!current.useControllerValue()); QCOMPARE(current.value(), 25.f);
		Engine::getSong()->stop(); QVERIFY(current.useControllerValue()); QVERIFY(!previous.useControllerValue());
	}

	void recordingCallbackRetirementSkipsAnotherCurveAndTarget()
	{
		FloatModel a(42, 0, 100, 1, nullptr); auto b = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr);
		control(&a); control(b.get()); auto first = curve(&a); auto second = curve(b.get());
		first->setRecording(true); second->setRecording(true); const QPointer<AutomationClip> retiredCurve(second); const QPointer<AutomatableModel> retiredModel(b.get());
		const auto connection = connect(&a, &AutomatableModel::dataChanged, this, [&] {
			if (a.useControllerValue() && b) { delete second; b.reset(); }
		}, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick();
		QVERIFY(!retiredCurve); QVERIFY(!retiredModel); QVERIFY(a.useControllerValue());
		QCOMPARE(first->valueAt(0), a.inverseScaledValue(a.value())); Engine::getSong()->stop();
	}

	void stopCallbackRetirementSkipsAnotherRetainedTarget()
	{
		auto a = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr), b = std::make_unique<FloatModel>(42, 0, 100, 1, nullptr);
		AutomatedValueMap order; order.insert(a.get(), 0); order.insert(b.get(), 0);
		if (order.firstKey() != a.get()) { a.swap(b); } control(a.get()); control(b.get()); auto clip = curve(a.get()); QVERIFY(clip->addObject(b.get()));
		Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick(); const QPointer<AutomatableModel> retired(b.get());
		const auto connection = connect(a.get(), &AutomatableModel::dataChanged, this, [&] { if (a->useControllerValue()) { b.reset(); } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		Engine::getSong()->stop(); QVERIFY(!retired); QVERIFY(a->useControllerValue());
	}

	void guardedSnapshotsKeepNativeCurveScaling_data()
	{
		QTest::addColumn<bool>("logarithmic"); QTest::newRow("linear") << false; QTest::newRow("logarithmic") << true;
	}
	void guardedSnapshotsKeepNativeCurveScaling()
	{
		QFETCH(bool, logarithmic); FloatModel model(42, 1, 100, 0, nullptr); model.setScaleLogarithmic(logarithmic); control(&model); curve(&model);
		const float expected = model.scaledValue(25); Engine::getSong()->playSong(); SongAutomationOwnershipTestAccess::tick();
		QVERIFY(!model.useControllerValue()); QCOMPARE(model.value(), expected); QCOMPARE(Engine::getSong()->automatedValuesAt(0).value(&model), 25.f);
		Engine::getSong()->stop(); QVERIFY(model.useControllerValue());
	}

	void failedAutomationCallbackKeepsLiveOwnershipRecoverable()
	{
		FloatModel model(42, 0, 100, 1, nullptr); control(&model); curve(&model); Engine::getSong()->playSong();
		const auto connection = connect(&model, &AutomatableModel::dataChanged, this, [&] { if (!model.useControllerValue()) { throw std::runtime_error("Automation ownership fixture"); } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		QVERIFY_EXCEPTION_THROWN(SongAutomationOwnershipTestAccess::tick(), std::runtime_error); QVERIFY(!model.useControllerValue());
		QObject::disconnect(connection); Engine::getSong()->stop(); QVERIFY(model.useControllerValue());
	}
};
QTEST_GUILESS_MAIN(SongAutomationOwnershipTest)
#include "SongAutomationOwnershipTest.moc"
