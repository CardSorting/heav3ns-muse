/* Native dependency names across returned virtual callbacks. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
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
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
struct NameState { QString value = "Reported native name"; std::function<void()> callback; int calls = 0; };
const QString& reported(const std::shared_ptr<NameState>& state) { ++state->calls; const auto callback = state->callback; if (callback) { callback(); } return state->value; }
const Plugin::Descriptor nameResourceDescriptor{"resource-name-fixture", "Resource name fixture", "Native name callback fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
class ResourceInstrument final : public Instrument
{
public:
	explicit ResourceInstrument(InstrumentTrack* track) : Instrument(track, &nameResourceDescriptor) { }
	std::function<void()> callback;
	std::vector<FileResource> fileResources() const override { const std::vector<FileResource> result{{"source", "Native source metadata", {}, {}}}; const auto work = callback; if (work) { work(); } return result; }
	void saveSettings(QDomDocument&, QDomElement&) override { }
	void loadSettings(const QDomElement&) override { }
	QString nodeName() const override { return "resource-name-fixture"; }
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
};
class NamedTrack final : public InstrumentTrack
{
public:
	explicit NamedTrack(std::shared_ptr<NameState> state) : InstrumentTrack(Engine::getSong()), m_state(std::move(state)) { }
	const QString& name() const override { const auto state = m_state; return reported(state); }
	void clearCallback() { m_state->callback = {}; }
private:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new ResourceInstrument(this); }
	std::shared_ptr<NameState> m_state;
};
class NamedSampleTrack final : public SampleTrack
{
public:
	explicit NamedSampleTrack(std::shared_ptr<NameState> state) : SampleTrack(Engine::getSong()), m_state(std::move(state)) { }
	const QString& name() const override { const auto state = m_state; return reported(state); }
	void clearCallback() { m_state->callback = {}; }
private:
	std::shared_ptr<NameState> m_state;
};
class NamedLfo final : public LfoController
{
public:
	explicit NamedLfo(std::shared_ptr<NameState> state) : LfoController(Engine::getSong()), m_state(std::move(state)) { }
	const QString& name() const override { const auto state = m_state; return reported(state); }
	void clearCallback() { m_state->callback = {}; }
private:
	std::shared_ptr<NameState> m_state;
};
QJsonObject inspect(QJsonObject args = {}) { args.insert("section", "resources"); return StudioCapabilities::execute("project.inspect", args); }
QJsonObject page(QJsonObject args = {}) { const auto response = inspect(args); if (!response.value("ok").toBool()) { throw std::runtime_error(QJsonDocument(response).toJson().constData()); } return response.value("result").toObject(); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject manifest(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
void effect(EffectChain* chain) { QDomDocument document; auto node = document.createElement("effect"); document.appendChild(node); node.setAttribute("name", "missing-name-fixture-effect"); if (!chain->appendEffect(new DummyEffect(chain, node))) { throw std::runtime_error("Native effect fixture was refused"); } }
struct Actors { QPointer<NamedTrack> track; QPointer<NamedLfo> lfo; QPointer<ResourceInstrument> plugin; };
Actors seed(const std::shared_ptr<NameState>& state, bool controller)
{
	Actors out;
	if (controller) { out.lfo = new NamedLfo(state); Engine::getSong()->addController(out.lfo.data()); }
	else { out.track = new NamedTrack(state); out.plugin = static_cast<ResourceInstrument*>(out.track->loadInstrument("resource-name-fixture")); }
	state->calls = 0; return out;
}
}
class StudioResourceNameTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanup()
	{
		// Clear reference-capturing fixture callbacks before any native teardown.
		for (auto track : Engine::getSong()->tracks())
		{
			if (auto named = dynamic_cast<NamedTrack*>(track)) { named->clearCallback(); if (auto plugin = dynamic_cast<ResourceInstrument*>(named->instrument())) { plugin->callback = {}; } }
			if (auto named = dynamic_cast<NamedSampleTrack*>(track)) { named->clearCallback(); }
		}
		for (auto controller : Engine::getSong()->controllers()) { if (auto named = dynamic_cast<NamedLfo*>(controller)) { named->clearCallback(); } }
		Engine::getSong()->stop(); Engine::getSong()->clearProject();
	}
	void reportedNamesRemainVirtualWithoutRenamingNativeObjects_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("instrument") << 0; QTest::newRow("sample-track-effect") << 1; QTest::newRow("controller") << 2; QTest::newRow("instrument-shaping") << 3; }
	void reportedNamesRemainVirtualWithoutRenamingNativeObjects()
	{
		QFETCH(int, kind); const auto state = std::make_shared<NameState>(); QString nativeName; QPointer<Track> track; QPointer<NamedLfo> controller;
		if (kind == 1) { auto native = new NamedSampleTrack(state); track = native; nativeName = native->Track::name(); auto clip = native->createClip(0); clip->setName("Native sample clip name"); effect(native->audioBusHandle()->effects()); state->calls = 0; }
		else { const auto actors = seed(state, kind == 2); track = actors.track.data(); controller = actors.lfo; nativeName = controller ? controller->Controller::name() : track->Track::name(); if (kind == 3) { QDomDocument document; auto node = document.createElement("elvol"); document.appendChild(node); node.setAttribute("userwavefile", configuration.filePath("missing-wave.wav")); node.setAttribute("userwaveunresolved", 1); actors.track->soundShaping()->getVolumeParameters().loadSettings(node); } }
		const auto rows = page().value("resources").toArray(); bool found = false; for (const auto& value : rows) { const auto row = value.toObject(); if (row.value("owner").toObject().value("kind") == QJsonValue("sample")) { QCOMPARE(row.value("ownerName").toString(), QString("Native sample clip name")); continue; } QCOMPARE(row.value("ownerName").toString(), state->value); found = true; } QVERIFY(found); QCOMPARE(state->calls, 2); QCOMPARE(controller ? controller->Controller::name() : track->Track::name(), nativeName);
	}
	void nameGettersCanRetireTheirOwnersAtEitherBoundary_data()
	{ QTest::addColumn<bool>("controller"); QTest::addColumn<int>("phase"); QTest::newRow("track-initial") << false << 1; QTest::newRow("track-final") << false << 2; QTest::newRow("controller-initial") << true << 1; QTest::newRow("controller-final") << true << 2; }
	void nameGettersCanRetireTheirOwnersAtEitherBoundary()
	{
		QFETCH(bool, controller); QFETCH(int, phase); const auto state = std::make_shared<NameState>(); const auto actors = seed(state, controller); bool retired = false; state->callback = [&] { if (state->calls != phase) { return; } retired = true; if (controller) { Engine::getSong()->removeController(actors.lfo.data()); } else { delete actors.track.data(); } }; const auto response = inspect(); QVERIFY(retired); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(controller ? !actors.lfo : !actors.track); QVERIFY(Engine::audioEngine()->canJoinBackendCallbacks());
	}
	void returnedNamesCannotResumeAcrossPeerRetirement_data()
	{ QTest::addColumn<bool>("controller"); QTest::addColumn<int>("phase"); QTest::addColumn<int>("peer"); for (int peer = 0; peer < 4; ++peer) { QTest::newRow(qPrintable(QString::number(peer) + "-track-initial")) << false << 1 << peer; QTest::newRow(qPrintable(QString::number(peer) + "-track-final")) << false << 2 << peer; QTest::newRow(qPrintable(QString::number(peer) + "-controller-initial")) << true << 1 << peer; QTest::newRow(qPrintable(QString::number(peer) + "-controller-final")) << true << 2 << peer; } }
	void returnedNamesCannotResumeAcrossPeerRetirement()
	{
		QFETCH(bool, controller); QFETCH(int, phase); QFETCH(int, peer); const auto state = std::make_shared<NameState>(); const auto actors = seed(state, controller); auto sample = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong())); const QPointer<Clip> clip(sample->createClip(0)); auto chain = sample->audioBusHandle()->effects(); effect(chain); const QPointer<LfoController> lfo(new LfoController(Engine::getSong())); Engine::getSong()->addController(lfo.data()); bool retired = false; state->calls = 0; state->callback = [&] { if (state->calls != phase) { return; } retired = true; if (peer == 0) { delete clip.data(); } if (peer == 1) { Engine::getSong()->removeController(lfo.data()); } if (peer == 2) { if (!chain->clear()) { throw std::runtime_error("Native chain clear was refused"); } } if (peer == 3) { Engine::mixer()->clear(); } }; const auto response = inspect(); QVERIFY(retired); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(controller ? bool(actors.lfo) : bool(actors.track));
	}
	void nativeBackingNameChangesAreRefused_data()
	{ QTest::addColumn<bool>("controller"); QTest::addColumn<int>("phase"); QTest::newRow("track-initial") << false << 1; QTest::newRow("track-final") << false << 2; QTest::newRow("controller-initial") << true << 1; QTest::newRow("controller-final") << true << 2; }
	void nativeBackingNameChangesAreRefused()
	{
		QFETCH(bool, controller); QFETCH(int, phase); const auto state = std::make_shared<NameState>(); const auto actors = seed(state, controller); bool renamed = false; state->callback = [&] { if (state->calls != phase) { return; } renamed = true; if (controller) { actors.lfo->Controller::setName("Changed native backing name"); } else { actors.track->Track::setName("Changed native backing name"); } }; QVERIFY(!inspect().value("ok").toBool()); QVERIFY(renamed); state->callback = {}; QVERIFY(inspect().value("ok").toBool());
	}
	void reportedNameChangesAreObservedIndependentlyFromBackingNames_data()
	{ QTest::addColumn<bool>("controller"); QTest::newRow("track") << false; QTest::newRow("controller") << true; }
	void reportedNameChangesAreObservedIndependentlyFromBackingNames()
	{
		QFETCH(bool, controller); const auto state = std::make_shared<NameState>(); seed(state, controller); state->callback = [&] { if (state->calls == 2) { state->value = "Changed reported name"; } }; QVERIFY(!inspect().value("ok").toBool()); state->callback = {}; const auto row = page().value("resources").toArray()[0].toObject(); QCOMPARE(row.value("ownerName").toString(), state->value);
	}
	void pluginGettersCannotSilentlyChangeReportedOwnerNames_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("initial-plugin-getter") << 1; QTest::newRow("final-plugin-getter") << 2; }
	void pluginGettersCannotSilentlyChangeReportedOwnerNames()
	{
		QFETCH(int, phase); const auto state = std::make_shared<NameState>(); const auto actors = seed(state, false); int calls = 0; actors.plugin->callback = [&] { if (++calls == phase) { state->value = "Changed by plugin getter"; } }; QVERIFY(!inspect().value("ok").toBool()); actors.plugin->callback = {}; QCOMPARE(page().value("resources").toArray()[0].toObject().value("ownerName").toString(), state->value);
	}
	void getterExceptionsReleaseReadState_data()
	{ QTest::addColumn<bool>("controller"); QTest::addColumn<int>("phase"); QTest::addColumn<bool>("nonstandard"); for (int phase = 1; phase <= 2; ++phase) { QTest::newRow(qPrintable(QString::number(phase) + "-track-standard")) << false << phase << false; QTest::newRow(qPrintable(QString::number(phase) + "-track-nonstandard")) << false << phase << true; QTest::newRow(qPrintable(QString::number(phase) + "-controller-standard")) << true << phase << false; QTest::newRow(qPrintable(QString::number(phase) + "-controller-nonstandard")) << true << phase << true; } }
	void getterExceptionsReleaseReadState()
	{
		QFETCH(bool, controller); QFETCH(int, phase); QFETCH(bool, nonstandard); const auto state = std::make_shared<NameState>(); seed(state, controller); bool thrown = false; state->callback = [&] { if (state->calls != phase) { return; } thrown = true; if (nonstandard) { throw 127; } throw std::runtime_error("Native name getter fixture"); }; const auto response = inspect(); QVERIFY(thrown); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); state->callback = {}; QVERIFY(inspect().value("ok").toBool()); QVERIFY(Engine::audioEngine()->canJoinBackendCallbacks());
	}
	void cancellationBetweenInitialNameCallsDoesNotInvokeTheNextGetter()
	{
		const auto first = std::make_shared<NameState>(), second = std::make_shared<NameState>(); seed(first, false); seed(second, false); first->calls = second->calls = 0; const auto response = StudioCapabilities::execute("project.inspect", {{"section", "resources"}}, [&] { return first->calls > 0; }); QVERIFY(!response.value("ok").toBool()); QCOMPARE(first->calls, 1); QCOMPARE(second->calls, 0); QVERIFY(inspect().value("ok").toBool());
	}
	void longReportedNamesHaveBoundedMetadataAndStablePaging()
	{
		const auto state = std::make_shared<NameState>(); state->value = QString(600000, 'n'); seed(state, false); QVERIFY(!inspect().value("ok").toBool()); const auto first = page({{"names", false}, {"limit", 1}}); const auto row = first.value("resources").toArray()[0].toObject(); QCOMPARE(row.value("ownerNamePreview").toString().size(), 256); QVERIFY(row.value("ownerNameTruncated").toBool()); QCOMPARE(page({{"names", false}, {"limit", 8}, {"resourceGeneration", first.value("resourceGeneration")}}).value("resourceGeneration"), first.value("resourceGeneration"));
	}
	void nativeRuntimeNameReceiptsStayHistoricalAcrossEditAndReopen()
	{
		QTemporaryDir directory; QString error; const auto query = request("names", "project.inspect", {{"section", "resources"}}); QJsonObject observed;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("create", "controller.add", {{"type", "lfo"}, {"name", "Original"}, {"summary", true}})).value("ok").toBool()); observed = runtime.process(query); QVERIFY(observed.value("ok").toBool()); QCOMPARE(observed.value("result").toObject().value("resources").toArray()[0].toObject().value("ownerName").toString(), QString("Original")); QVERIFY(runtime.process(request("rename", "controller.edit", {{"controller", 0}, {"name", "Renamed"}})).value("ok").toBool()); const auto before = manifest(directory); auto replay = runtime.process(query); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, observed); const auto fresh = runtime.process(request("fresh", "project.inspect", {{"section", "resources"}})); QVERIFY(fresh.value("ok").toBool()); QCOMPARE(fresh.value("result").toObject().value("resources").toArray()[0].toObject().value("ownerName").toString(), QString("Renamed")); QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision")); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(reopened.process(request("after-reopen", "project.inspect", {{"section", "resources"}})).value("result").toObject().value("resources").toArray()[0].toObject().value("ownerName").toString(), QString("Renamed"));
	}
};
QTEST_GUILESS_MAIN(StudioResourceNameTest)
#include "StudioResourceNameTest.moc"
