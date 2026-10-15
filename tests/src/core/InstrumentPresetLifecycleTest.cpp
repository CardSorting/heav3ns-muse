/* Native preset restore ownership, preservation and recovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <functional>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "EffectChain.h"
#include "Engine.h"
#include "EnvelopeAndLfoParameters.h"
#include "FileResourceIO.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
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
const Plugin::Descriptor fixtureDescriptor{"preset-fixture", "Preset fixture", "Native preset fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
class PresetInstrument final : public Instrument
{
public:
	explicit PresetInstrument(InstrumentTrack* owner) : Instrument(owner, &fixtureDescriptor) { }
	FloatModel gain{.25f, 0, 1, .01f, this, "Gain"};
	std::function<void()> onLoad, onRetire;
	~PresetInstrument() override { const auto callback = onRetire; if (callback) { callback(); } }
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	QString nodeName() const override { return "preset-fixture"; }
	void saveSettings(QDomDocument& document, QDomElement& node) override { gain.saveSettings(document, node, "gain"); }
	void loadSettings(const QDomElement& node) override
	{ const auto callback = onLoad; const QPointer<PresetInstrument> owner(this); gain.loadSettings(node, "gain"); if (owner && callback) { callback(); } }
};
class PresetTrack final : public InstrumentTrack
{
public:
	explicit PresetTrack(TrackContainer* owner) : InstrumentTrack(owner) { }
	std::function<void(PresetInstrument*)> onCreate;
	int factories = 0;
	bool nullFactory = false;
	bool dispatching() const { return isDispatchingTrackSpecificState(); }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override
	{
		++factories; if (nullFactory) { return nullptr; } const auto callback = onCreate; const QPointer<PresetInstrument> native(new PresetInstrument(this)); bool released = false;
		const auto cleanup = qScopeGuard([&] { if (!released) { delete native.data(); } }); if (callback) { callback(native.data()); } released = true; return native.data();
	}
};
DataFile preset(bool legacy = false)
{
	DataFile file(DataFile::Type::InstrumentTrackSettings); auto root = file.content(); root.setAttribute("type", static_cast<int>(Track::Type::Instrument)); root.setAttribute("name", "Loaded preset"); root.setAttribute("muted", 0); root.setAttribute("solo", 0);
	auto settings = file.createElement("instrumenttrack"); settings.setAttribute("vol", 64); settings.setAttribute("pan", 12); settings.setAttribute("pitchrange", 12); settings.setAttribute("mixch", 0); settings.setAttribute("enablecc", 0); settings.setAttribute("activecc", ""); root.appendChild(settings);
	auto state = file.createElement("preset-fixture"); state.setAttribute("gain", .75);
	if (legacy) { settings.appendChild(state); } else { auto instrument = file.createElement("instrument"); instrument.setAttribute("name", "preset-fixture"); instrument.appendChild(state); settings.appendChild(instrument); }
	return file;
}
QDomElement settings(const DataFile& file) { return file.content().firstChildElement("instrumenttrack"); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
class ObservedSerializer final : public QObject, public SerializingObject
{
public:
	std::function<void()> callback;
	QString nodeName() const override { return "observed"; }
	void saveSettings(QDomDocument&, QDomElement&) override { const auto call = callback; if (call) { call(); } }
	void loadSettings(const QDomElement&) override { const auto call = callback; if (call) { call(); } }
};
class PlainSerializer final : public SerializingObject
{
public:
	int value = 7;
	QString nodeName() const override { return "plain"; }
	void saveSettings(QDomDocument&, QDomElement& element) override { element.setAttribute("value", value); }
	void loadSettings(const QDomElement& element) override { value = element.attribute("value").toInt(); }
};
class ObservedHook final : public SerializingObjectHook
{
public:
	int calls = 0;
	std::function<void()> callback;
	void saveSettings(QDomDocument&, QDomElement&) override { ++calls; const auto call = callback; if (call) { call(); } }
	void loadSettings(const QDomElement&) override { ++calls; const auto call = callback; if (call) { call(); } }
};
}
class InstrumentPresetLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
	PresetTrack* seed() { auto track = new PresetTrack(Engine::getSong()); track->loadInstrument("preset-fixture"); track->volumeModel()->setValue(42); return track; }
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void modernAndLegacyNativeSettingsRetainModelsAndApplyPluginState_data()
	{ QTest::addColumn<bool>("legacy"); QTest::newRow("modern") << false; QTest::newRow("legacy") << true; }
	void modernAndLegacyNativeSettingsRetainModelsAndApplyPluginState()
	{
		QFETCH(bool, legacy); auto target = seed(); const QPointer<Instrument> old(target->instrument()); const auto id = target->volumeModel()->id(); auto file = preset(legacy); target->loadTrackSpecificSettings(settings(file));
		QVERIFY(!old); QVERIFY(target->instrument()); QCOMPARE(target->factories, 2); QCOMPARE(target->volumeModel()->id(), id); QCOMPARE(target->volumeModel()->storedValue(), 64.f); QCOMPARE(target->panningModel()->storedValue(), 12.f); QCOMPARE(static_cast<PresetInstrument*>(target->instrument())->gain.value(), .75f); QVERIFY(!target->isReplacingInstrument());
	}
	void previewRestoreReusesTheMatchingNativeInstrument()
	{
		auto target = seed(); target->setPreviewMode(true); const QPointer<Instrument> old(target->instrument()); QSignalSpy changed(target, &InstrumentTrack::instrumentChanged); auto file = preset(); target->loadTrackSpecificSettings(settings(file));
		QVERIFY(old); QCOMPARE(target->instrument(), old.data()); QCOMPARE(target->factories, 1); QCOMPARE(changed.count(), 0); QCOMPARE(static_cast<PresetInstrument*>(old.data())->gain.value(), .75f);
	}
	void replacementPreservesDestinationRoutingFlagsAndClips_data()
	{ QTest::addColumn<bool>("solo"); QTest::newRow("muted") << false; QTest::newRow("solo") << true; }
	void replacementPreservesDestinationRoutingFlagsAndClips()
	{
		QFETCH(bool, solo); auto target = seed(); const auto channel = Engine::mixer()->createChannel(); target->mixerChannelModel()->setValue(channel); target->midiPort()->setOutputChannel(5); target->midiPort()->setInputChannel(3); if (solo) { target->setSolo(true); } else { target->setMuted(true); } target->setMutedBeforeSolo(true);
		const bool oldMute = target->isMutedByModel(); const QPointer<Clip> clip(target->createClip(0)); const auto id = target->volumeModel()->id(); auto file = preset(); auto port = file.createElement("midiport"); port.setAttribute("outputchannel", 13); settings(file).appendChild(port);
		target->replaceInstrument(file); QCOMPARE(target->mixerChannelModel()->value(), channel); QCOMPARE(target->midiPort()->outputChannel(), 5); QCOMPARE(target->midiPort()->inputChannel(), 3); QCOMPARE(target->isMutedByModel(), oldMute); QCOMPARE(target->isSolo(), solo); QVERIFY(target->isMutedBeforeSolo());
		QVERIFY(clip); QCOMPARE(target->getClip(0), clip.data()); QCOMPARE(target->volumeModel()->id(), id); QVERIFY(!target->isReplacingInstrument());
	}
	void settingsCallbacksCannotContinueAfterOwnerRetirement_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("volume") << 0; QTest::newRow("effects") << 1; QTest::newRow("factory") << 2; QTest::newRow("plugin") << 3; QTest::newRow("instrument") << 4; QTest::newRow("old-retirement") << 5; QTest::newRow("microtuner") << 6; QTest::newRow("filter") << 7; QTest::newRow("envelope") << 8; }
	void settingsCallbacksCannotContinueAfterOwnerRetirement()
	{
		QFETCH(int, phase); auto target = seed(); const QPointer<InstrumentTrack> owner(target); bool invoked = false; const auto retire = [&] { if (!invoked) { invoked = true; delete target; } };
		if (phase == 0) { connect(target->volumeModel(), &Model::dataChanged, this, retire); }
		if (phase == 1) { connect(target->audioBusHandle()->effects(), &EffectChain::aboutToClear, this, retire); }
		if (phase == 2) { target->onCreate = [&](PresetInstrument*) { retire(); }; }
		if (phase == 3) { target->onCreate = [&](PresetInstrument* native) { native->onLoad = retire; }; }
		if (phase == 4) { connect(target, &InstrumentTrack::instrumentChanged, this, retire); }
		if (phase == 5) { static_cast<PresetInstrument*>(target->instrument())->onRetire = retire; }
		auto file = preset();
		if (phase == 6) { settings(file).setAttribute("enabled", 1); connect(target->microtuner()->enabledModel(), &Model::dataChanged, this, retire); }
		if (phase >= 7)
		{
			auto shape = target->findChild<InstrumentSoundShaping*>(); QVERIFY(shape); auto state = file.createElement(shape->nodeName()); settings(file).appendChild(state);
			if (phase == 7) { state.setAttribute("fcut", 1000); connect(&shape->getFilterCutModel(), &Model::dataChanged, this, retire); }
			else { auto envelope = file.createElement(shape->getVolumeParameters().nodeName() + "vol"); envelope.setAttribute("pdel", .25); state.appendChild(envelope); connect(&shape->getVolumeParameters(), &Model::dataChanged, this, retire); }
		}
		QVERIFY_EXCEPTION_THROWN(target->loadTrackSpecificSettings(settings(file)), std::runtime_error); QVERIFY(invoked); QVERIFY(!owner); QVERIFY(Engine::getSong()->tracks().empty());
	}
	void genericTrackAndPresetLoadStopAfterRenameRetirement_data()
	{ QTest::addColumn<bool>("portable"); QTest::newRow("preset") << true; QTest::newRow("track") << false; }
	void genericTrackAndPresetLoadStopAfterRenameRetirement()
	{
		QFETCH(bool, portable); auto target = seed(); const QPointer<Track> owner(target); bool invoked = false; connect(target, &Track::nameChanged, this, [&] { if (!invoked) { invoked = true; delete target; } }); auto file = preset();
		if (portable) { QVERIFY_EXCEPTION_THROWN(target->loadPreset(file.content()), std::runtime_error); } else { QVERIFY_EXCEPTION_THROWN(target->loadSettings(file.content()), std::runtime_error); }
		QVERIFY(invoked); QVERIFY(!owner); QVERIFY(Engine::getSong()->tracks().empty());
	}
	void nestedPresetEntriesAreRefusedAcrossRestoration_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("name") << 0; QTest::newRow("volume") << 1; QTest::newRow("plugin") << 2; QTest::newRow("modified") << 3; }
	void nestedPresetEntriesAreRefusedAcrossRestoration()
	{
		QFETCH(int, phase); auto target = seed(); auto file = preset(); bool invoked = false;
		const auto nested = [&] { if (invoked) { return; } invoked = true; QVERIFY(!target->loadInstrument("nested")); QVERIFY_EXCEPTION_THROWN(target->replaceInstrument(file), std::logic_error); QVERIFY_EXCEPTION_THROWN(target->loadPreset(file.content()), std::logic_error); QVERIFY_EXCEPTION_THROWN(target->loadTrackSpecificSettings(settings(file)), std::logic_error); };
		QMetaObject::Connection connection;
		if (phase == 0) { connection = connect(target, &Track::nameChanged, this, nested); }
		if (phase == 1) { connection = connect(target->volumeModel(), &Model::dataChanged, this, nested); }
		if (phase == 2) { target->onCreate = [&](PresetInstrument* native) { native->onLoad = nested; }; }
		if (phase == 3) { connect(target, &InstrumentTrack::instrumentChanged, this, [&] { NativeSongTestAccess::setModified(Engine::getSong(), false); }); connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (!target->dispatching()) { nested(); } }); }
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); target->replaceInstrument(file); QVERIFY(invoked); QVERIFY(!target->isReplacingInstrument()); target->onCreate = {};
		static_cast<PresetInstrument*>(target->instrument())->onLoad = {};
	}
	void preservationCallbacksCannotReturnStaleSuccess_data()
	{ QTest::addColumn<int>("action"); QTest::newRow("clip") << 0; QTest::newRow("mixer") << 1; QTest::newRow("mute") << 2; QTest::newRow("instrument") << 3; }
	void preservationCallbacksCannotReturnStaleSuccess()
	{
		QFETCH(int, action); auto target = seed(); const auto channel = Engine::mixer()->createChannel(); target->mixerChannelModel()->setValue(channel); target->createClip(0); auto file = preset(); bool invoked = false;
		connect(target, &InstrumentTrack::instrumentChanged, this, [&] { NativeSongTestAccess::setModified(Engine::getSong(), false); }); const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] {
			if (invoked || target->dispatching()) { return; } invoked = true; if (action == 0) { delete target->getClip(0); } if (action == 1) { target->mixerChannelModel()->setValue(0); } if (action == 2) { target->setMuted(!target->isMutedByModel()); } if (action == 3) { delete target->instrument(); }
		}); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); }); QVERIFY_EXCEPTION_THROWN(target->replaceInstrument(file), std::runtime_error); QVERIFY(invoked); QVERIFY(!target->isReplacingInstrument());
	}
	void presetFactoryFailuresReleaseAllAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("standard") << 0; QTest::newRow("nonstandard") << 1; QTest::newRow("null") << 2; }
	void presetFactoryFailuresReleaseAllAdmission()
	{
		QFETCH(int, failure); auto target = seed(); target->nullFactory = failure == 2; target->onCreate = [&](PresetInstrument*) { if (failure == 0) { throw std::runtime_error("fixture preset factory"); } throw 7; }; auto file = preset(); bool caught = false;
		try { target->replaceInstrument(file); } catch (...) { caught = true; } QVERIFY(caught); QVERIFY(!target->isReplacingInstrument()); QVERIFY(!target->instrument()); target->onCreate = {}; target->nullFactory = false; target->replaceInstrument(file); QVERIFY(target->instrument());
	}
	void cooperativeCancellationRetainsExplicitPartialState_data()
	{ QTest::addColumn<bool>("prepared"); QTest::newRow("before") << false; QTest::newRow("after-volume") << true; }
	void cooperativeCancellationRetainsExplicitPartialState()
	{
		QFETCH(bool, prepared); auto target = seed(); const QPointer<Instrument> old(target->instrument()); auto file = preset();
		{ FileResourceIO::Scope work([&] { return !prepared || target->volumeModel()->storedValue() == 64; }, false); QVERIFY_EXCEPTION_THROWN(target->replaceInstrument(file), std::runtime_error); }
		QVERIFY(old); QCOMPARE(target->instrument(), old.data()); QCOMPARE(target->volumeModel()->storedValue(), prepared ? 64.f : 42.f); QVERIFY(!target->isReplacingInstrument()); target->replaceInstrument(file); QVERIFY(target->instrument());
	}
	void serializingBoundaryStopsAfterAnObservableReceiverDies_data()
	{ QTest::addColumn<bool>("save"); QTest::addColumn<bool>("hook"); QTest::newRow("save-settings") << true << false; QTest::newRow("restore-settings") << false << false; QTest::newRow("save-hook") << true << true; QTest::newRow("restore-hook") << false << true; }
	void serializingBoundaryStopsAfterAnObservableReceiverDies()
	{
		QFETCH(bool, save); QFETCH(bool, hook); auto native = new ObservedSerializer; const QPointer<ObservedSerializer> weak(native); ObservedHook observer; native->setHook(&observer); const auto retire = [&] { delete native; };
		if (hook) { observer.callback = retire; } else { native->callback = retire; } QDomDocument document; auto node = document.createElement("root"); document.appendChild(node);
		if (save) { QVERIFY_EXCEPTION_THROWN(native->saveState(document, node), std::runtime_error); } else { QVERIFY_EXCEPTION_THROWN(native->restoreState(node), std::runtime_error); }
		QVERIFY(!weak); QCOMPARE(observer.calls, hook ? 1 : 0);
	}
	void nonQObjectSerializationRetainsItsNativeContract()
	{
		PlainSerializer native; QDomDocument document; auto root = document.createElement("root"); document.appendChild(root); auto state = native.saveState(document, root); native.value = 0; native.restoreState(state); QCOMPARE(native.value, 7);
	}
	void structuredPresetReceiptUsesTheSurvivingTrackIndex()
	{
		QTemporaryDir directory; auto file = preset(); const auto path = directory.filePath("fixture.xpf"); QVERIFY(file.writeFile(path)); auto earlier = new InstrumentTrack(Engine::getSong()); auto target = seed();
		connect(target, &InstrumentTrack::instrumentChanged, this, [&] { if (earlier) { delete earlier; earlier = nullptr; } }); const auto result = StudioCapabilities::execute("preset.load", {{"track", 1}, {"path", path}});
		QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QCOMPARE(result.value("result").toObject().value("track").toInt(), 0); QCOMPARE(Engine::getSong()->tracks().front(), static_cast<Track*>(target));
	}
	void runtimeRecoversAndCachesPresetNotificationFailure_data()
	{ QTest::addColumn<bool>("standard"); QTest::newRow("standard") << true; QTest::newRow("nonstandard") << false; }
	void runtimeRecoversAndCachesPresetNotificationFailure()
	{
		QFETCH(bool, standard); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.filePath("session")); QVERIFY(runtime.open(error));
		QVERIFY(runtime.process(request("track", "track.add", {{"type", "instrument"}})).value("ok").toBool()); QVERIFY(runtime.process(request("instrument", "instrument.load", {{"track", 0}, {"instrument", "tripleoscillator"}})).value("ok").toBool());
		const auto path = directory.filePath("builtin.xpf"); QVERIFY(runtime.process(request("save", "preset.save", {{"track", 0}, {"path", path}})).value("ok").toBool()); auto target = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().front()); bool invoked = false;
		const auto connection = connect(target, &InstrumentTrack::instrumentChanged, this, [&] { if (!invoked) { invoked = true; if (standard) { throw std::runtime_error("fixture preset notification"); } throw 7; } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto command = request("load", "preset.load", {{"track", 0}, {"path", path}}); const auto receipt = runtime.process(command); QVERIFY(invoked); QVERIFY(!receipt.value("ok").toBool()); QVERIFY(static_cast<InstrumentTrack*>(Engine::getSong()->tracks().front())->instrument());
		auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, receipt); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(InstrumentPresetLifecycleTest)
#include "InstrumentPresetLifecycleTest.moc"
