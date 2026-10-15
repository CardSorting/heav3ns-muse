/* Native Hydrogen input, arrangement and recovery. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "HydrogenImport.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "LocalFileMng.h"
#include "MidiClip.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PluginFactory.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "../core/NativeSongTestAccess.h"
namespace lmms
{
namespace
{
bool write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QByteArray little(quint32 value, int count)
{ QByteArray result; for (int index = 0; index < count; ++index) { result.append(static_cast<char>(value >> (8 * index))); } return result; }
bool wave(const QString& path)
{
	QByteArray pcm(64, '\0'); pcm[1] = 0x20;
	return write(path, QByteArray("RIFF") + little(36 + pcm.size(), 4) + "WAVEfmt " + little(16, 4) + little(1, 2) + little(1, 2) + little(44100, 4) + little(88200, 4) + little(2, 2) + little(16, 2) + "data" + little(pcm.size(), 4) + pcm);
}
QDomElement child(QDomDocument& document, QDomElement parent, const QString& name, const QString& value)
{ auto node = document.createElement(name); node.appendChild(document.createTextNode(value)); parent.appendChild(node); return node; }
QDomDocument project(const QString& sample, int count = 1)
{
	QDomDocument document; auto root = document.createElement("hydrogen"), song = document.createElement("song"); document.appendChild(root); root.appendChild(song);
	auto instruments = document.createElement("instrumentList"); song.appendChild(instruments);
	for (const auto& id : {QString("kick"), QString("hat")})
	{ auto instrument = document.createElement("instrument"); instruments.appendChild(instrument); child(document, instrument, "id", id); child(document, instrument, "name", id); child(document, instrument, "volume", ".5"); child(document, instrument, "pan_L", ".2"); child(document, instrument, "pan_R", ".8"); auto layer = document.createElement("layer"); instrument.appendChild(layer); child(document, layer, "filename", sample); }
	auto patterns = document.createElement("patternList"); song.appendChild(patterns);
	for (const auto& name : {QString("A"), QString("Empty"), QString("Long")})
	{
		auto pattern = document.createElement("pattern"); patterns.appendChild(pattern); child(document, pattern, "name", name); child(document, pattern, "size", name == "Empty" ? "96" : name == "Long" ? "384" : "192"); auto notes = document.createElement("noteList"); pattern.appendChild(notes);
		for (int index = 0; name != "Empty" && index < (name == "Long" ? 1 : count); ++index)
		{ auto note = document.createElement("note"); notes.appendChild(note); child(document, note, "instrument", name == "Long" ? "hat" : "kick"); child(document, note, "position", name == "Long" ? "383" : QString::number(index % 192)); child(document, note, "velocity", ".5"); child(document, note, "key", name == "Long" ? "Bf-1" : "C0"); }
	}
	auto sequence = document.createElement("patternSequence"); song.appendChild(sequence);
	for (const auto& names : {QStringList{"A", "Long"}, QStringList{"Empty"}, QStringList{"A"}})
	{ auto group = document.createElement("group"); sequence.appendChild(group); for (const auto& name : names) { child(document, group, "patternID", name); } }
	return document;
}
QDomElement songElement(QDomDocument& document) { return document.documentElement().firstChildElement("song"); }
QDomElement firstInstrument(QDomDocument& document) { return songElement(document).firstChildElement("instrumentList").firstChildElement("instrument"); }
QDomElement firstPattern(QDomDocument& document) { return songElement(document).firstChildElement("patternList").firstChildElement("pattern"); }
QDomElement firstNote(QDomDocument& document) { return firstPattern(document).firstChildElement("noteList").firstChildElement("note"); }
void value(QDomDocument& document, QDomElement parent, const QString& name, const QString& data)
{ auto old = parent.firstChildElement(name); if (!old.isNull()) { parent.removeChild(old); } child(document, parent, name, data); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
class Container final : public TrackContainer
{ public: Container() { setType(Type::Song); } QString nodeName() const override { return "hydrogen-fixture-container"; } };
}
class HydrogenImportTest : public QObject
{
	Q_OBJECT
	QTemporaryDir directory;
	bool journalling = false;
	QString input() const { return directory.filePath("incoming.h2song"); }
	QString sample() const { return directory.filePath("sample.wav"); }
	bool import(QDomDocument document) { if (!write(input(), document.toByteArray())) { throw std::runtime_error("Could not write Hydrogen fixture"); } HydrogenImport importer(input()); return importer.readSong(); }
	std::vector<InstrumentTrack*> rows() const { std::vector<InstrumentTrack*> result; for (auto row : Engine::patternStore()->tracks()) { if (auto instrument = dynamic_cast<InstrumentTrack*>(row)) { result.push_back(instrument); } } return result; }
	PatternTrack* column(const QString& name) const { for (auto track : Engine::getSong()->tracks()) { if (auto pattern = dynamic_cast<PatternTrack*>(track); pattern && pattern->name() == name) { return pattern; } } return nullptr; }
	QString resource(Instrument* instrument) const { for (const auto& entry : instrument->fileResources()) { if (entry.key == "sample") { return entry.path; } } return {}; }
private slots:
	void initTestCase()
	{
		QVERIFY(directory.isValid()); ConfigManager::inst()->setWorkingDir(directory.filePath("workspace")); ConfigManager::inst()->loadConfigFile(directory.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		journalling = Engine::projectJournal()->isJournalling(); QVERIFY(wave(sample())); QVERIFY(!getPluginFactory()->pluginInfo("audiofileprocessor").isNull());
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setLoadOnLaunch(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void malformedReferencesAndValuesPreserveTheOriginalProject_data()
	{ QTest::addColumn<int>("kind"); for (int index = 0; index < 15; ++index) { QTest::newRow(qPrintable(QString::number(index))) << index; } }
	void malformedReferencesAndValuesPreserveTheOriginalProject()
	{
		QFETCH(int, kind); auto keep = new AutomationTrack(Engine::getSong()); auto document = project(sample()); const auto generation = Engine::getSong()->replacementGeneration();
		if (kind == 0) { value(document, firstNote(document), "instrument", "unknown"); } if (kind == 1) { firstInstrument(document).parentNode().appendChild(firstInstrument(document).cloneNode(true)); }
		if (kind == 2) { value(document, firstNote(document), "position", "-1"); } if (kind == 3) { value(document, firstNote(document), "position", "192"); } if (kind == 4) { value(document, firstPattern(document), "size", "0"); }
		if (kind == 5) { value(document, firstPattern(document), "size", "bad"); } if (kind == 6) { value(document, firstNote(document), "velocity", "nan"); } if (kind == 7) { value(document, firstNote(document), "velocity", "2"); }
		if (kind == 8) { value(document, firstNote(document), "pan_R", "2"); } if (kind == 9) { value(document, firstNote(document), "key", "C100"); }
		if (kind == 10) { value(document, songElement(document).firstChildElement("patternSequence").firstChildElement("group"), "patternID", "unknown"); } if (kind == 11) { firstPattern(document).parentNode().appendChild(firstPattern(document).cloneNode(true)); }
		if (kind == 12) { value(document, songElement(document), "bpm", "inf"); } if (kind == 13) { document.documentElement().appendChild(songElement(document).cloneNode(true)); } if (kind == 14) { value(document, firstInstrument(document), "id", ""); }
		QVERIFY(!import(document)); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0], static_cast<Track*>(keep)); QCOMPARE(Engine::getSong()->replacementGeneration(), generation); QVERIFY(rows().empty());
	}
	void unrelatedFormatsRemainAvailableWithoutSpuriousWarnings()
	{
		const auto path = directory.filePath("unrelated.mid"); QVERIFY(write(path, "MThd unrelated fixture")); HydrogenImport importer(path); QVERIFY(!importer.readSong()); QVERIFY(!Engine::getSong()->hasErrors()); QVERIFY(Engine::getSong()->tracks().empty());
	}
	void relativeLayerDirectAndComponentSamplesUseNativeAfp_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("layer-relative") << 0; QTest::newRow("direct") << 1; QTest::newRow("component") << 2; QTest::newRow("silent-instrument") << 3; }
	void relativeLayerDirectAndComponentSamplesUseNativeAfp()
	{
		QFETCH(int, kind); auto document = project("sample.wav"); auto instrument = firstInstrument(document); auto layer = instrument.firstChildElement("layer");
		if (kind == 1) { instrument.removeChild(layer); child(document, instrument, "filename", "sample.wav"); } if (kind == 2) { instrument.removeChild(layer); auto component = document.createElement("instrumentComponent"); instrument.appendChild(component); component.appendChild(layer); } if (kind == 3) { instrument.removeChild(layer); }
		QVERIFY(import(document)); QCOMPARE(rows().size(), std::size_t(2)); auto row = rows()[0]; QVERIFY(row->instrument()); QCOMPARE(QString::fromUtf8(row->instrument()->descriptor()->name), QString("audiofileprocessor")); QCOMPARE(resource(row->instrument()), kind == 3 ? QString{} : sample()); QCOMPARE(row->volumeModel()->value(), 50.f); QCOMPARE(row->panningModel()->value(), 60.f);
		auto cell = static_cast<MidiClip*>(row->getClips()[column("A")->patternIndex()]); QCOMPARE(cell->notes().size(), std::size_t(1)); QCOMPARE(cell->notes()[0]->key(), 36); QCOMPARE(cell->notes()[0]->getVolume(), 50);
	}
	void emptyPatternsAndSimultaneousSequenceGroupsRetainNativeIdentities()
	{
		new AutomationTrack(Engine::getSong()); auto old = static_cast<PatternTrack*>(Track::create(Track::Type::Pattern, Engine::getSong())); old->setName("Old"); Engine::patternStore()->setCurrentPattern(0);
		QVERIFY(import(project(sample()))); QVERIFY(column("A") && column("Empty") && column("Long")); QCOMPARE(Engine::patternStore()->numOfPatterns(), 4); QCOMPARE(Engine::patternStore()->currentPattern(), 0);
		QCOMPARE(column("A")->getClips().size(), std::size_t(2)); QCOMPARE(column("A")->getClips()[0]->startPosition().getTicks(), 0); QCOMPARE(column("A")->getClips()[1]->startPosition().getTicks(), 480);
		QCOMPARE(column("Empty")->getClips()[0]->startPosition().getTicks(), 384); QCOMPARE(column("Empty")->getClips()[0]->length().getTicks(), 96); QCOMPARE(column("Long")->getClips()[0]->length().getTicks(), 384);
		for (auto row : rows()) { QCOMPARE(row->numOfClips(), 4); auto cell = static_cast<MidiClip*>(row->getClips()[column("Empty")->patternIndex()]); QVERIFY(cell->notes().empty()); QCOMPARE(cell->length().getTicks(), 96); QVERIFY(!cell->getAutoResize()); }
		auto note = static_cast<MidiClip*>(rows()[1]->getClips()[column("Long")->patternIndex()])->notes()[0]; QCOMPARE(note->key(), 34); QCOMPARE(note->length().getTicks(), 1);
	}
	void priorPatternCellsAndSelectionSurviveAppendedColumns()
	{
		Track::create(Track::Type::Pattern, Engine::getSong()); Track::create(Track::Type::Pattern, Engine::getSong()); auto row = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::patternStore())); auto cell = static_cast<MidiClip*>(row->getClips()[1]); cell->clearNotes(); cell->addNote(Note(31, 7, 72), false); Engine::patternStore()->setCurrentPattern(1);
		QVERIFY(import(project(sample()))); QCOMPARE(row->getClips()[1], static_cast<Clip*>(cell)); QCOMPARE(cell->notes().size(), std::size_t(1)); QCOMPARE(cell->notes()[0]->key(), 72); QCOMPARE(Engine::patternStore()->currentPattern(), 1); QCOMPARE(row->numOfClips(), 5);
	}
	void tempoUsesNativeAutomationWithoutReplacingTheStoredTempo()
	{
		Engine::getSong()->setTempo(83); auto document = project(sample()); child(document, songElement(document), "bpm", "93"); QVERIFY(import(document)); QCOMPARE(Engine::getSong()->getTempo(), 83); bool found = false;
		for (auto track : Engine::getSong()->tracks()) { if (track->name() == "Hydrogen tempo") { auto clip = dynamic_cast<AutomationClip*>(track->getClips()[0]); QVERIFY(clip); QCOMPARE(clip->valueAt(0), 93.f); QVERIFY(std::any_of(clip->objects().begin(), clip->objects().end(), [&](const auto& model) { return model.data() == &Engine::getSong()->tempoModel(); })); found = true; } } QVERIFY(found);
	}
	void importerInstancesRetainTheirOwnFilenames()
	{
		const auto other = directory.filePath("other.h2song"); auto first = project(sample()), second = project(sample()); value(first, firstNote(first), "key", "C1"); value(second, firstNote(second), "key", "C2"); QVERIFY(write(input(), first.toByteArray())); QVERIFY(write(other, second.toByteArray())); HydrogenImport a(input()), b(other);
		QVERIFY(a.readSong()); QCOMPARE(static_cast<MidiClip*>(rows()[0]->getClips()[0])->notes()[0]->key(), 48); QVERIFY(b.readSong()); QCOMPARE(static_cast<MidiClip*>(rows()[2]->getClips()[3])->notes()[0]->key(), 60);
	}
	void missingSamplesRemainEditableAndSurviveNativeProjectSave()
	{
		const auto missing = directory.filePath("missing.wav"); QVERIFY(import(project(missing))); QCOMPARE(rows().size(), std::size_t(2)); QCOMPARE(resource(rows()[0]->instrument()), missing); QVERIFY(Engine::getSong()->errorSummary().contains("unresolved"));
		const auto saved = directory.filePath("missing-sample.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(saved)); QVERIFY(Engine::getSong()->loadProject(saved)); QCOMPARE(resource(rows()[0]->instrument()), missing); QCOMPARE(static_cast<MidiClip*>(rows()[0]->getClips()[0])->notes().size(), std::size_t(1));
	}
	void failedReplacementKeepsAnAlreadyDecodedSampleResource()
	{
		QVERIFY(import(project(sample()))); auto instrument = rows()[0]->instrument(); const auto before = resource(instrument); QString error; QVERIFY(!instrument->setFileResource("sample", directory.filePath("missing-replacement.wav"), error)); QCOMPARE(resource(instrument), before);
	}
	void approximatedFeaturesProduceExplicitNativeWarnings()
	{
		auto document = project(sample()); auto instrument = firstInstrument(document); instrument.appendChild(instrument.firstChildElement("layer").cloneNode(true)); value(document, firstNote(document), "note_off", "true"); child(document, songElement(document), "author", "Fixture author"); QVERIFY(import(document));
		const auto warning = Engine::getSong()->errorSummary(); QVERIFY(warning.contains("first sample")); QVERIFY(warning.contains("note-off")); QVERIFY(warning.contains("author"));
	}
	void cancellationRemovesOnlyCreatedRowsAndColumns_data()
	{ QTest::addColumn<int>("phase"); QTest::newRow("preflight") << 0; QTest::newRow("columns") << 1; QTest::newRow("notes") << 2; }
	void cancellationRemovesOnlyCreatedRowsAndColumns()
	{
		QFETCH(int, phase); auto prior = new AutomationTrack(Engine::getSong()); QVERIFY(write(input(), project(sample(), 1024).toByteArray())); HydrogenImport importer(input()); bool observed = false;
		{ FileResourceIO::Scope work([&] { if (phase == 0) { observed = true; return true; } if (phase == 1 && column("A")) { observed = true; return true; } if (phase == 2) { for (auto row : rows()) { for (auto item : row->getClips()) { if (auto cell = dynamic_cast<MidiClip*>(item); cell && cell->notes().size() >= 64) { observed = true; return true; } } } } return false; }); QVERIFY(!importer.readSong()); }
		QVERIFY(observed); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0], static_cast<Track*>(prior)); QVERIFY(rows().empty()); QCOMPARE(Engine::patternStore()->numOfPatterns(), 0); QVERIFY(importer.readSong());
	}
	void retiredActorsStopBeforeTheirNextUse_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("row") << 0; QTest::newRow("clip") << 1; QTest::newRow("instrument") << 2; }
	void retiredActorsStopBeforeTheirNextUse()
	{
		QFETCH(int, kind); QVERIFY(write(input(), project(sample(), 1024).toByteArray())); HydrogenImport importer(input()); bool retired = false;
		{ FileResourceIO::Scope work([&] { if (retired) { return false; } for (auto row : rows()) { for (auto item : row->getClips()) { if (auto cell = dynamic_cast<MidiClip*>(item); cell && cell->notes().size() >= 64) { retired = true; if (kind == 0) { delete row; } else if (kind == 1) { delete cell; } else { delete row->instrument(); } return false; } } } return false; }); QVERIFY(!importer.readSong()); }
		QVERIFY(retired); QVERIFY(Engine::getSong()->tracks().empty()); QVERIFY(rows().empty()); QVERIFY(importer.readSong());
	}
	void callbackOwnedTracksSurvivePartialCleanup()
	{
		QVERIFY(write(input(), project(sample(), 1024).toByteArray())); HydrogenImport importer(input()); QPointer<Track> outsider;
		{ FileResourceIO::Scope work([&] { if (outsider) { return true; } for (auto row : rows()) { for (auto item : row->getClips()) { if (auto cell = dynamic_cast<MidiClip*>(item); cell && cell->notes().size() >= 64) { outsider = new AutomationTrack(Engine::getSong()); return true; } } } return false; }); QVERIFY(!importer.readSong()); }
		QVERIFY(outsider); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0], outsider.data()); QVERIFY(rows().empty());
	}
	void partialWorkExceptionsRestoreOwnedStateAndAllowRetry_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void partialWorkExceptionsRestoreOwnedStateAndAllowRetry()
	{
		QFETCH(bool, nonstandard); auto prior = new AutomationTrack(Engine::getSong()); const auto journalling = Engine::projectJournal()->isJournalling(); QVERIFY(write(input(), project(sample(), 1024).toByteArray())); HydrogenImport importer(input()); bool raised = false;
		{ FileResourceIO::Scope work([&]() -> bool { if (!raised) { for (auto row : rows()) { for (auto item : row->getClips()) { if (auto cell = dynamic_cast<MidiClip*>(item); cell && cell->notes().size() >= 64) { raised = true; if (nonstandard) { throw 47; } throw std::runtime_error("fixture Hydrogen work failure"); } } } } return false; }); QVERIFY(!importer.readSong()); }
		QVERIFY(raised); QCOMPARE(Engine::projectJournal()->isJournalling(), journalling); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0], static_cast<Track*>(prior)); QVERIFY(rows().empty()); QVERIFY(importer.readSong());
	}
	void nestedImporterAndProjectWorkIsRefused()
	{
		QVERIFY(write(input(), project(sample()).toByteArray())); HydrogenImport importer(input()), other(input()); bool invoked = false, same = false, different = false, replacement = false;
		{ FileResourceIO::Scope work([&] { if (invoked) { return false; } invoked = true; same = !importer.readSong(); different = !other.readSong(); try { Engine::getSong()->createEmptyProject(); } catch (const std::logic_error&) { replacement = true; } return false; }); QVERIFY(importer.readSong()); } QVERIFY(invoked && same && different && replacement); QVERIFY(other.readSong());
	}
	void importerRetirementDuringXmlReadKeepsTheInputAlive()
	{
		QVERIFY(write(input(), project(sample()).toByteArray())); QPointer<HydrogenImport> importer(new HydrogenImport(input())); int checks = 0;
		{ FileResourceIO::Scope work([&] { if (++checks == 3) { delete importer.data(); } return false; }); auto active = importer.data(); QVERIFY(!active->readSong()); } QVERIFY(!importer); QVERIFY(rows().empty()); QVERIFY(Engine::getSong()->tracks().empty());
	}
	void wrongDestinationsAreRefused_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("null") << 0; QTest::newRow("pattern-store") << 1; QTest::newRow("custom") << 2; }
	void wrongDestinationsAreRefused()
	{ QFETCH(int, kind); Container custom; HydrogenImport importer(input()); QVERIFY(!importer.tryImport(kind == 0 ? nullptr : kind == 1 ? static_cast<TrackContainer*>(Engine::patternStore()) : &custom)); QVERIFY(Engine::getSong()->tracks().empty()); }
	void journalPolicyThreadAdmissionAndQueuedWorkAreRetained()
	{
		QVERIFY(write(input(), project(sample()).toByteArray())); HydrogenImport importer(input()); const bool previous = Engine::projectJournal()->isJournalling(); const auto restore = qScopeGuard([&] { Engine::projectJournal()->setJournalling(previous); }); Engine::projectJournal()->setJournalling(false); bool queued = false;
		QTimer::singleShot(0, this, [&] { queued = true; }); { FileResourceIO::Scope work({}, true); QVERIFY(importer.readSong()); QVERIFY(!queued); } QCoreApplication::processEvents(); QVERIFY(queued); QVERIFY(!Engine::projectJournal()->isJournalling()); bool accepted = true; std::thread foreign([&] { accepted = importer.readSong(); }); foreign.join(); QVERIFY(!accepted);
	}
	void malformedLegacyEntitiesTerminateWithoutOutOfBoundsReads_data()
	{ QTest::addColumn<QByteArray>("bytes"); QTest::newRow("truncated") << QByteArray("&#x"); QTest::newRow("bad-digit") << QByteArray("&#xZ1;"); QTest::newRow("one-digit") << QByteArray("&#xA;"); QTest::newRow("unicode-scalar") << QByteArray("&#x1234;"); QTest::newRow("reserved-ascii") << QByteArray("&#x26;"); }
	void malformedLegacyEntitiesTerminateWithoutOutOfBoundsReads()
	{ QFETCH(QByteArray, bytes); const auto before = bytes; LocalFileMng::convertFromTinyXMLString(&bytes); QCOMPARE(bytes, before); QByteArray utf8("&#xD1;&#x84;"); LocalFileMng::convertFromTinyXMLString(&utf8); QCOMPARE(utf8, QByteArray::fromHex("d184")); }
	void declaredBomUnicodeXmlUsesTheModernParser()
	{
		const auto unicode = directory.filePath(QStringLiteral("\u97f3.wav")); QVERIFY(wave(unicode)); auto document = project(unicode); const auto bytes = QByteArray::fromHex("efbbbf") + "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + document.toByteArray(); QVERIFY(write(input(), bytes)); QVERIFY(!LocalFileMng::checkTinyXMLCompatMode(input())); HydrogenImport importer(input()); QVERIFY(importer.readSong()); QCOMPARE(resource(rows()[0]->instrument()), unicode);
	}
	void actualPluginRuntimeRetainsWarningsHistoryAndReplay()
	{
		if (getPluginFactory()->pluginInfo("hydrogenimport").isNull()) { QSKIP("Requires the built native Hydrogen import module"); } auto document = project(sample()); firstInstrument(document).appendChild(firstInstrument(document).firstChildElement("layer").cloneNode(true)); QVERIFY(write(input(), document.toByteArray())); QTemporaryDir session; const auto command = request("import", "project.import", {{"path", input()}, {"summary", true}}); QJsonObject result;
		{ StudioRuntime runtime(session.path()); QString error; QVERIFY(runtime.open(error)); result = runtime.process(command); QVERIFY2(result.value("ok").toBool(), qPrintable(QJsonDocument(result).toJson())); QVERIFY(result.value("nativeWarnings").toString().contains("first sample")); QVERIFY(runtime.process(command).value("replayed").toBool()); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QVERIFY(rows().empty()); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(rows().size(), std::size_t(2)); }
		StudioRuntime reopened(session.path()); QString error; QVERIFY(reopened.open(error)); QCOMPARE(rows().size(), std::size_t(2)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, result);
	}
	void runtimePublicationFailureRollsBackAndReplays_data()
	{ QTest::addColumn<bool>("nonstandard"); QTest::newRow("standard") << false; QTest::newRow("nonstandard") << true; }
	void runtimePublicationFailureRollsBackAndReplays()
	{
		QFETCH(bool, nonstandard); if (getPluginFactory()->pluginInfo("hydrogenimport").isNull()) { QSKIP("Requires the built native Hydrogen import module"); } QVERIFY(write(input(), project(sample()).toByteArray())); QTemporaryDir session; const auto command = request("import", "project.import", {{"path", input()}, {"summary", true}}); QJsonObject rejected;
		{ StudioRuntime runtime(session.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 83}})).value("ok").toBool()); bool armed = true, prepared = false;
			const auto connection = connect(Engine::getSong(), &Song::modified, this, [&] { if (armed && prepared && !NativeSongTestAccess::isImporting(Engine::getSong()) && Engine::getSong()->isModified()) { armed = false; if (nonstandard) { throw 43; } throw std::runtime_error("fixture Hydrogen publication failure"); } }); const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
			{ FileResourceIO::Scope work([&] { if (NativeSongTestAccess::isImporting(Engine::getSong()) && !rows().empty() && rows()[0]->getClips().size() && !static_cast<MidiClip*>(rows()[0]->getClips()[0])->notes().empty()) { prepared = true; NativeSongTestAccess::setModified(Engine::getSong(), false); } return false; }); rejected = runtime.process(command); }
			QVERIFY(prepared && !armed); QVERIFY(!rejected.value("ok").toBool()); QVERIFY(!runtime.stopping()); QVERIFY(rows().empty()); QVERIFY(Engine::getSong()->tracks().empty()); QCOMPARE(Engine::getSong()->getTempo(), 83); auto replay = runtime.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected); }
		StudioRuntime reopened(session.path()); QString error; QVERIFY(reopened.open(error)); auto replay = reopened.process(command); QVERIFY(replay.take("replayed").toBool()); QCOMPARE(replay, rejected);
	}
};
}
QTEST_GUILESS_MAIN(lmms::HydrogenImportTest)
#include "HydrogenImportTest.moc"
