#include "Instrument.h"
/* Native declared dependency pages and agent discovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NativeSongTestAccess.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "ProjectJournal.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
int saves = 0, loads = 0, refreshes = 0;
const Plugin::Descriptor resourceInstrumentDescriptor{"project-resource-fixture", "Project resource fixture", "Native dependency fault fixture", "LMMS tests", 1, Plugin::Type::Instrument, nullptr, "", nullptr};
const Plugin::Descriptor resourceEffectDescriptor{"project-resource-effect", "Resource effect fixture", "Native dependency fault fixture", "LMMS tests", 1, Plugin::Type::Effect, nullptr, "", nullptr};
class ResourceInstrument final : public Instrument
{
public:
	explicit ResourceInstrument(InstrumentTrack* track) : Instrument(track, &resourceInstrumentDescriptor), selector(std::make_unique<FloatModel>(.5f, 0, 1, .01f, this, "Selection")) { declared.push_back({"sample", "Fixture source", {}, {"wav"}, selector.get(), .5f}); }
	std::unique_ptr<FloatModel> selector;
	std::shared_ptr<const void> backing = std::make_shared<const char>(0);
	std::vector<FileResource> declared;
	std::function<void()> onInspect;
	std::vector<FileResource> fileResources() const override { auto result = declared; for (auto& row : result) { row.identity = backing; } const auto callback = onInspect; if (callback) { callback(); } return result; }
	void saveSettings(QDomDocument&, QDomElement&) override { ++saves; }
	void loadSettings(const QDomElement&) override { ++loads; }
	void refreshParameterModels() override { ++refreshes; }
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	QString nodeName() const override { return "project-resource-fixture"; }
};
class ResourceTrack final : public InstrumentTrack
{
public:
	explicit ResourceTrack(TrackContainer* owner) : InstrumentTrack(owner) { }
protected:
	Instrument* createInstrument(const QString&, const Plugin::Descriptor::SubPluginFeatures::Key*, bool) override { return new ResourceInstrument(this); }
};
ResourceInstrument* instrument(TrackContainer* owner = Engine::getSong()) { auto track = new ResourceTrack(owner); return static_cast<ResourceInstrument*>(track->loadInstrument("project-resource-fixture")); }
class ResourceEffect final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	explicit ResourceEffect(EffectChain* chain) : Effect(&resourceEffectDescriptor, chain, nullptr) { }
	QString path;
	EffectControls* controls() override { return &m_controls; }
	std::vector<FileResource> fileResources() const override { return {{"impulse", "Effect source", path, {"wav"}}}; }
protected:
	ProcessStatus processImpl(SampleFrame*, f_cnt_t) override { return ProcessStatus::Continue; }
};
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject inspect(QJsonObject args = {}) { args.insert("section", "resources"); return StudioCapabilities::execute("project.inspect", args); }
QJsonObject result(QJsonObject args = {}) { const auto response = inspect(args); if (!response.value("ok").toBool()) { throw std::runtime_error(QJsonDocument(response).toJson().constData()); } return response.value("result").toObject(); }
QJsonObject manifest(const QTemporaryDir& dir) { QFile file(dir.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray little(quint32 value, int count) { QByteArray out; for (int i = 0; i < count; ++i) { out.append(static_cast<char>(value >> (8 * i))); } return out; }
bool wave(const QString& path) { QByteArray pcm(128, '\0'); pcm[1] = 0x20; return write(path, QByteArray("RIFF") + little(36 + pcm.size(), 4) + "WAVEfmt " + little(16, 4) + little(1, 2) + little(1, 2) + little(44100, 4) + little(88200, 4) + little(2, 2) + little(16, 2) + "data" + little(pcm.size(), 4) + pcm); }
SampleClip* sample(TrackContainer* owner = Engine::getSong()) { return static_cast<SampleClip*>(Track::create(Track::Type::Sample, owner)->createClip(0)); }
void pending(SampleClip* clip, const QString& path) { QDomDocument doc; auto element = doc.createElement("sampleclip"); doc.appendChild(element); element.setAttribute("src", path); element.setAttribute("unresolved", 1); element.setAttribute("len", 192); clip->loadSettings(element); }
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"}, {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}
class StudioProjectResourceTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	QString audio() const { return configuration.filePath("audio.wav"); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(wave(audio())); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); saves = loads = refreshes = 0; }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void resourceAndWarningSectionsAreDeclaredAndReachable()
	{
		QJsonObject spec; for (const auto& item : StudioCapabilities::schema()) { if (item.toObject().value("name") == QJsonValue("project.inspect")) { spec = item.toObject(); } } const auto properties = spec.value("inputSchema").toObject().value("properties").toObject(); const auto sections = properties.value("section").toObject().value("enum").toArray(); QVERIFY(sections.contains("resources")); QVERIFY(sections.contains("warnings")); QCOMPARE(properties.value("resourceGeneration").toObject().value("type").toString(), QString("string")); QVERIFY(result().value("resources").toArray().isEmpty()); Engine::getSong()->collectError("Native diagnostic remains visible"); QVERIFY(StudioCapabilities::execute("project.inspect", {{"section", "warnings"}}).value("ok").toBool()); QVERIFY(StudioCapabilities::inspectSummary().value("detailSections").toArray().contains("resources"));
	}
	void nativeResourceOwnersHaveCurrentCoordinates_data()
	{ QTest::addColumn<int>("scope"); for (int i = 0; i < 8; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; } }
	void nativeResourceOwnersHaveCurrentCoordinates()
	{
		QFETCH(int, scope); QString expected;
		if (scope == 0 || scope == 1) { if (scope == 1) { Track::create(Track::Type::Pattern, Engine::getSong()); } auto native = instrument(scope == 0 ? static_cast<TrackContainer*>(Engine::getSong()) : Engine::patternStore()); native->declared[0].path = audio(); expected = "instrument"; }
		else if (scope == 2 || scope == 3) { auto track = static_cast<SampleTrack*>(Track::create(Track::Type::Sample, Engine::getSong())); auto chain = scope == 2 ? track->audioBusHandle()->effects() : &Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ResourceEffect(chain); effect->path = audio(); QVERIFY(chain->appendEffect(effect)); expected = "effect"; }
		else if (scope == 4 || scope == 5) { if (scope == 5) { Track::create(Track::Type::Pattern, Engine::getSong()); } auto clip = scope == 4 ? sample() : static_cast<SampleClip*>(Track::create(Track::Type::Sample, Engine::patternStore())->getClip(0)); QString error; QVERIFY(clip->loadSampleFile(audio(), error)); expected = "sample"; }
		else if (scope == 6) { auto lfo = new LfoController(Engine::getSong()); Engine::getSong()->addController(lfo); QString error; QVERIFY(lfo->setUserWave(audio(), error)); expected = "controllerWave"; }
		else { QDomDocument doc; auto element = doc.createElement("effect"); doc.appendChild(element); element.setAttribute("name", "unavailable-native-effect"); auto chain = &Engine::mixer()->mixerChannel(0)->m_fxChain; QVERIFY(chain->appendEffect(new DummyEffect(chain, element))); expected = "effect"; }
		const auto page = result(); const auto rows = page.value("resources").toArray(); QVERIFY(!rows.isEmpty()); bool found = false; for (const auto& value : rows) { const auto row = value.toObject(); const auto owner = row.value("owner").toObject(); if (owner.value("kind").toString() != expected) { continue; } found = true; if (scope == 1 || scope == 5) { QCOMPARE(owner.value("container").toString(), QString("pattern")); } if (scope == 3 || scope == 7) { QCOMPARE(owner.value("scope").toString(), QString("mixer")); QCOMPARE(owner.value("channel").toInt(), 0); } if (scope == 7) { QCOMPARE(row.value("file").toObject().value("status").toString(), QString("missingPlugin")); QVERIFY(row.value("requiredForRestore").toBool()); QVERIFY(row.value("repairOperation").isNull()); } else { QCOMPARE(row.value("file").toObject().value("status").toString(), QString("readableFile")); } } QVERIFY(found);
	}
	void pendingSourcesStayUnresolvedWhenAFileAppears_data()
	{ QTest::addColumn<bool>("present"); QTest::newRow("missing") << false; QTest::newRow("appeared") << true; }
	void pendingSourcesStayUnresolvedWhenAFileAppears()
	{ QFETCH(bool, present); QTemporaryDir source; const auto path = source.filePath("later.wav"); auto clip = sample(); pending(clip, path); if (present) { QVERIFY(wave(path)); } const auto page = result(); QCOMPARE(page.value("declaredUnresolvedCount").toInt(), 1); QCOMPARE(page.value("requiredForRestoreCount").toInt(), 1); const auto row = page.value("resources").toArray()[0].toObject(); QVERIFY(row.value("unresolved").toBool()); QCOMPARE(row.value("file").toObject().value("status").toString(), present ? QString("readableFile") : QString("missing")); QCOMPARE(row.value("repairOperation").toString(), QString("sample.load")); QVERIFY(clip->sample().buffer()->empty()); }
	void inventoryDoesNotSerializeDecodeRefreshOrModifyNativeState()
	{
		auto native = instrument(); native->declared[0].path = audio(); saves = loads = refreshes = 0; NativeSongTestAccess::setModified(Engine::getSong(), false); const auto diagnostics = Engine::getSong()->nativeErrors(); const auto journal = Engine::projectJournal()->isJournalling(); const auto selector = native->selector->storedValue(); const auto backing = native->backing; const auto rows = result().value("resources").toArray(); QCOMPARE(rows.size(), 1); QCOMPARE(saves, 0); QCOMPARE(loads, 0); QCOMPARE(refreshes, 0); QVERIFY(!Engine::getSong()->isModified()); QCOMPARE(Engine::getSong()->nativeErrors(), diagnostics); QCOMPARE(Engine::projectJournal()->isJournalling(), journal); QCOMPARE(native->selector->storedValue(), selector); QVERIFY(native->backing == backing);
	}
	void pagesRetainTokensAcrossOffsetsLimitsAndTextModes()
	{
		auto native = instrument(); const auto base = native->declared[0]; for (int i = 1; i < 11; ++i) { auto row = base; row.key = QString("source%1").arg(i); native->declared.push_back(row); } QString token; int offset = 0; QSet<int> seen;
		do { QJsonObject args{{"offset", offset}, {"limit", 3}, {"names", offset % 2 == 0}}; if (!token.isEmpty()) { args.insert("resourceGeneration", token); } const auto page = result(args); if (token.isEmpty()) { token = page.value("resourceGeneration").toString(); } QCOMPARE(page.value("resourceGeneration").toString(), token); QCOMPARE(page.value("total").toInt(), 11); const auto rows = page.value("resources").toArray(); QVERIFY(!rows.isEmpty()); for (const auto& row : rows) { QVERIFY(!seen.contains(row.toObject().value("index").toInt())); seen.insert(row.toObject().value("index").toInt()); } offset = page.value("nextOffset").toInt(); if (!page.value("hasMore").toBool()) { break; } } while (offset < 11); QCOMPARE(seen.size(), 11); QVERIFY(!result({{"offset", 99}, {"resourceGeneration", token}}).value("hasMore").toBool());
	}
	void changedObservationsRejectEarlierTokens_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("metadata") << 0; QTest::newRow("filesystem") << 1; QTest::newRow("project") << 2; QTest::newRow("selector") << 3; }
	void changedObservationsRejectEarlierTokens()
	{
		QFETCH(int, change); QTemporaryDir source; const auto path = source.filePath("source.bin"); QVERIFY(write(path, "abc")); auto native = instrument(); native->declared[0].path = path; const auto token = result().value("resourceGeneration").toString(); if (change == 0) { native->declared[0].name += " changed"; } else if (change == 1) { QVERIFY(QFile::remove(path)); } else if (change == 2) { Engine::getSong()->createEmptyProject(); } else { native->selector->setValue(.75f); } QVERIFY(!inspect({{"resourceGeneration", token}}).value("ok").toBool()); QVERIFY(inspect().value("ok").toBool());
	}
	void readableFileMetadataDoesNotClaimByteOrAudioVerification()
	{
		QTemporaryDir source; const auto path = source.filePath("not-a-wave.bin"); QVERIFY(write(path, "aaa")); auto native = instrument(); native->declared[0].path = path; const auto first = result(); QVERIFY(write(path, "bbb")); const auto second = result({{"resourceGeneration", first.value("resourceGeneration")}}); QCOMPARE(second.value("resourceGeneration"), first.value("resourceGeneration")); QCOMPARE(second.value("resources").toArray()[0].toObject().value("file").toObject().value("status").toString(), QString("readableFile")); QVERIFY(second.value("verification").toString().contains("no byte hashing")); QCOMPARE(loads, 0);
	}
	void oversizedTextHasBoundedPreviewPages_data()
	{ QTest::addColumn<QString>("field"); QTest::newRow("name") << QString("name"); QTest::newRow("key") << QString("key"); }
	void oversizedTextHasBoundedPreviewPages()
	{
		QFETCH(QString, field); auto native = instrument(); if (field == "name") { native->declared[0].name = QString(600000, 'n'); } else { native->declared[0].key = QString(600000, 'k'); } QVERIFY(!inspect().value("ok").toBool()); const auto page = result({{"names", false}}); const auto row = page.value("resources").toArray()[0].toObject(); QCOMPARE(row.value(field + "Preview").toString().size(), 256); QCOMPARE(row.value(field + "Length").toInt(), 600000); QVERIFY(row.value(field + "Truncated").toBool()); QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() < 512 * 1024);
	}
	void fullMetadataChangesBeyondPreviewsStillChangeTokens()
	{
		auto native = instrument(); native->declared[0].name = QString(300, 'x') + "a"; const auto first = result({{"names", false}}); native->declared[0].name = QString(300, 'x') + "b"; QVERIFY(!inspect({{"names", false}, {"resourceGeneration", first.value("resourceGeneration")}}).value("ok").toBool());
	}
	void typeListsAreProjectedWithoutExpandingAllStrings()
	{
		auto native = instrument(); native->declared[0].fileTypes.clear(); for (int i = 0; i < 40; ++i) { native->declared[0].fileTypes.append(QString(1000, 't')); } const auto row = result().value("resources").toArray()[0].toObject(); QCOMPARE(row.value("fileTypeCount").toInt(), 40); const auto types = row.value("fileTypePreviews").toArray(); QCOMPARE(types.size(), 16); for (const auto& type : types) { QCOMPARE(type.toString().size(), 64); }
	}
	void cooperativeRetirementCannotPublishMixedOwners_data()
	{ QTest::addColumn<int>("owner"); for (int i = 0; i < 5; ++i) { QTest::newRow(qPrintable(QString::number(i))) << i; } }
	void cooperativeRetirementCannotPublishMixedOwners()
	{
		QFETCH(int, owner); const QPointer<ResourceInstrument> native(instrument()); const QPointer<SampleClip> clip(sample()); auto lfo = new LfoController(Engine::getSong()); Engine::getSong()->addController(lfo); bool captured = false, changed = false; native->onInspect = [&] { captured = true; }; QJsonObject response;
		{ FileResourceIO::Scope work([&] { if (captured && !changed) { changed = true; if (owner == 0) { delete native.data(); } else if (owner == 1) { delete clip.data(); } else if (owner == 2) { delete clip->getTrack(); } else if (owner == 3) { Engine::getSong()->removeController(lfo); } else { Engine::mixer()->clear(); } } return false; }); response = inspect(); }
		QVERIFY(changed); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
	}
	void getterRetirementIsCheckedBeforeUsingReturnedSelectors_data()
	{ QTest::addColumn<int>("call"); QTest::newRow("initial") << 1; QTest::newRow("final") << 2; }
	void getterRetirementIsCheckedBeforeUsingReturnedSelectors()
	{
		QFETCH(int, call); const QPointer<ResourceInstrument> native(instrument()); int calls = 0; native->onInspect = [&] { if (++calls == call) { delete native.data(); } }; const auto response = inspect(); QVERIFY(!response.value("ok").toBool()); QVERIFY(!native); QVERIFY(!response.contains("result"));
	}
	void finalGetterCannotRetireAnAlreadyCapturedSelector()
	{
		auto native = instrument(); int calls = 0; native->onInspect = [&] { if (++calls == 2) { native->selector.reset(); } }; QVERIFY(!inspect().value("ok").toBool()); QVERIFY(!native->selector);
	}
	void cooperativeMetadataAndBackingChangesAreDetected_data()
	{ QTest::addColumn<bool>("backing"); QTest::newRow("path") << false; QTest::newRow("backing") << true; }
	void cooperativeMetadataAndBackingChangesAreDetected()
	{
		QFETCH(bool, backing); auto native = instrument(); bool captured = false, changed = false; native->onInspect = [&] { captured = true; }; QJsonObject response; { FileResourceIO::Scope work([&] { if (captured && !changed) { changed = true; if (backing) { native->backing = std::make_shared<const char>(0); } else { native->declared[0].path = audio(); } } return false; }); response = inspect(); } QVERIFY(changed); QVERIFY(!response.value("ok").toBool()); QVERIFY(inspect().value("ok").toBool());
	}
	void malformedPagesAreRefused_data()
	{ QTest::addColumn<QJsonObject>("args"); QTest::newRow("negative-offset") << QJsonObject{{"offset", -1}}; QTest::newRow("fractional-offset") << QJsonObject{{"offset", .5}}; QTest::newRow("zero-limit") << QJsonObject{{"limit", 0}}; QTest::newRow("large-limit") << QJsonObject{{"limit", 1025}}; QTest::newRow("names-type") << QJsonObject{{"names", "false"}}; QTest::newRow("token-type") << QJsonObject{{"resourceGeneration", 4}}; QTest::newRow("token-shape") << QJsonObject{{"resourceGeneration", "unknown"}}; QTest::newRow("track") << QJsonObject{{"track", 0}}; QTest::newRow("warning-field") << QJsonObject{{"diagnosticGeneration", "1"}}; }
	void malformedPagesAreRefused() { QFETCH(QJsonObject, args); QVERIFY(!inspect(args).value("ok").toBool()); }
	void pendingQtEventsStayOutsideTheNativeScan()
	{
		instrument(); bool queued = false; QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(inspect().value("ok").toBool()); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued);
	}
	void cancellationAndExceptionsReleaseReadAdmission_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("cancel") << 0; QTest::newRow("standard") << 1; QTest::newRow("nonstandard") << 2; }
	void cancellationAndExceptionsReleaseReadAdmission()
	{
		QFETCH(int, kind); auto native = instrument(); bool captured = false; native->onInspect = [&] { captured = true; }; const auto response = StudioCapabilities::execute("project.inspect", {{"section", "resources"}}, [&] { if (!captured) { return false; } if (kind == 1) { throw std::runtime_error("fixture resource read failure"); } if (kind == 2) { throw 71; } return true; }); QVERIFY(captured); QVERIFY(!response.value("ok").toBool()); QVERIFY(inspect().value("ok").toBool());
	}
	void runtimeReceiptsPreserveHistoryAndHistoricalObservations()
	{
		QTemporaryDir source, directory; const auto path = source.filePath("audio.wav"); QVERIFY(wave(path)); const auto query = request("resources", "project.inspect", {{"section", "resources"}}); QJsonObject observed; QString token, error;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool()); QVERIFY(runtime.process(request("clip", "clip.add", {{"track", 0}, {"position", 0}})).value("ok").toBool()); QVERIFY(runtime.process(request("load", "sample.load", {{"track", 0}, {"clip", 0}, {"path", path}})).value("ok").toBool()); const auto before = manifest(directory); observed = runtime.process(query); QVERIFY(observed.value("ok").toBool()); token = observed.value("result").toObject().value("resourceGeneration").toString(); QVERIFY(QFile::remove(path)); auto replay = runtime.process(query); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, observed); const auto fresh = runtime.process(request("fresh", "project.inspect", {{"section", "resources"}})); QVERIFY(fresh.value("ok").toBool()); QCOMPARE(fresh.value("result").toObject().value("resources").toArray()[0].toObject().value("file").toObject().value("status").toString(), QString("missing")); QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision")); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QVERIFY(!reopened.process(request("stale", "project.inspect", {{"section", "resources"}, {"resourceGeneration", token}})).value("ok").toBool()); QVERIFY(reopened.process(request("rediscover", "project.inspect", {{"section", "resources"}})).value("ok").toBool());
	}
	void scriptedAgentsDiscoverAndPageResources_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void scriptedAgentsDiscoverAndPageResources()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error, token; int rounds = 0; bool advertised = false; QSet<int> seen; QJsonArray evidence;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			QJsonObject output; for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray()) { const auto item = value.toObject(); const auto content = item.value("content").toString(); if (content.startsWith("Native runtime observation (data):\n")) { const auto summary = QJsonDocument::fromJson(content.mid(content.indexOf('\n') + 1).toUtf8()).object().value("projectSummary").toObject(); advertised = advertised || summary.value("detailSections").toArray().contains("resources"); } if (item.value("role") == QJsonValue("tool") || item.value("type") == QJsonValue("function_call_output")) { output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); } }
			++rounds; if (rounds > 1) { if (!output.value("ok").toBool()) { throw std::runtime_error("Resource page failed"); } const auto page = output.value("result").toObject(); token = page.value("resourceGeneration").toString(); for (const auto& row : page.value("resources").toArray()) { seen.insert(row.toObject().value("index").toInt()); } if (!page.value("hasMore").toBool()) { return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Observed declared dependency metadata; no audio verification claimed."}, {"evidence", evidence}}); } }
			const auto id = QString("resource-%1").arg(rounds); evidence.append(id); QJsonObject args{{"section", "resources"}, {"limit", 1}, {"names", false}}; if (rounds > 1) { args.insert("offset", output.value("result").toObject().value("nextOffset")); args.insert("resourceGeneration", token); } return decision(protocol, id, "project_inspect", args);
		});
		QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("track", "track.add", {{"type", "sample"}})).value("ok").toBool()); for (int i = 0; i < 3; ++i) { QVERIFY(runtime.process(request(QString("clip-%1").arg(i), "clip.add", {{"track", 0}, {"position", i * 192}})).value("ok").toBool()); } QVERIFY(runtime.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-dependency-fixture"}}}})).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Inspect declared dependencies without changing the composition."}})).value("ok").toBool()); for (int i = 0; i < 8; ++i) { runtime.advanceAgent(); } QVERIFY(advertised); QCOMPARE(seen.size(), 3); QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete")); QVERIFY(!runtime.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioProjectResourceTest)
#include "StudioProjectResourceTest.moc"
