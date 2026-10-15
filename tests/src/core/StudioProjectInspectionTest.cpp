/* Bounded native arrangement observations. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Track.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject inspect(const QJsonObject& args) { return StudioCapabilities::execute("project.inspect", args); }
QByteArray encoded(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject manifest(const QTemporaryDir& directory)
{
	QFile file(directory.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
QJsonObject job(const QTemporaryDir& directory) { return manifest(directory).value("agent").toObject().value("job").toObject(); }
void fill(MidiClip* clip, int count)
{
	clip->clearNotes(); std::vector<Note> notes; notes.reserve(count);
	for (int index = 0; index < count; ++index) { notes.emplace_back(TimePos(12 + index % 7), TimePos(index * 24), index % 128, 50 + index % 100, index % 101 - 50); }
	clip->appendRecordedNotes(notes);
}
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(encoded(args));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{
		{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
		{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"},
			{"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}

class StudioProjectInspectionTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->clearProject(); }

	void summaryKeepsDenseNativeLayoutsCompactAndUnchanged()
	{
		auto track = Track::create(Track::Type::Instrument, Engine::getSong()); auto midi = static_cast<MidiClip*>(track->createClip(0)); fill(midi, 5001);
		QVERIFY(StudioCapabilities::execute("note.detuning.point", {{"track", 0}, {"clip", 0}, {"note", 0}, {"position", 0}, {"value", .5}}).value("ok").toBool());
		auto curve = midi->notes().front()->detuning()->automationClip();
		for (int index = 1; index < 2049; ++index) { curve->putValue(index, (index % 20) * .25f, false); }
		const auto longName = QString::fromUtf8("\xf0\x9f\x8e\xb5").repeated(100000);
		for (int index = 0; index < 33; ++index) { Track::create(Track::Type::Sample, Engine::getSong())->setName(longName); }
		const auto before = StudioCapabilities::inspect(); int checks = 0; QJsonObject result;
		{ FileResourceIO::Scope work([&] { ++checks; return false; }); result = inspect({{"section", "summary"}}); }
		QVERIFY(result.value("ok").toBool()); const auto summary = result.value("result").toObject();
		QCOMPARE(summary.value("inspection").toString(), QString("summary")); QVERIFY(encoded(summary).size() < 64 * 1024);
		QVERIFY(checks > 0 && checks < 100); QCOMPARE(summary.value("tracks").toArray().size(), 16);
		const auto page = summary.value("trackPages").toObject().value("song").toObject(); QCOMPARE(page.value("total").toInt(), 34); QVERIFY(page.value("hasMore").toBool());
		for (const auto& value : summary.value("tracks").toArray())
		{
			const auto item = value.toObject(); QVERIFY(!item.contains("clips")); QVERIFY(!item.contains("name"));
			QVERIFY(item.value("namePreview").toString().toUtf8().size() <= 256);
			if (item.value("index").toInt()) { QVERIFY(item.value("nameTruncated").toBool()); QVERIFY(longName.startsWith(item.value("namePreview").toString())); }
		}
		const auto observation = StudioAgent::observation(summary, {}, {}, false).value("content").toString();
		const auto payload = QJsonDocument::fromJson(observation.mid(observation.indexOf('\n') + 1).toUtf8()).object();
		QCOMPARE(payload.value("projectSummary").toObject(), summary); QCOMPARE(StudioCapabilities::inspect(), before);
	}

	void trackAndClipPagesPreserveNativeOwnersAndIndices_data()
	{
		QTest::addColumn<QString>("section"); QTest::addColumn<bool>("pattern"); QTest::addColumn<int>("type");
		QTest::newRow("song-tracks") << QString("tracks") << false << int(Track::Type::Sample);
		QTest::newRow("pattern-tracks") << QString("tracks") << true << int(Track::Type::Sample);
		QTest::newRow("midi-clips") << QString("clips") << false << int(Track::Type::Instrument);
		QTest::newRow("sample-clips") << QString("clips") << false << int(Track::Type::Sample);
		QTest::newRow("automation-clips") << QString("clips") << false << int(Track::Type::Automation);
		QTest::newRow("pattern-midi-clips") << QString("clips") << true << int(Track::Type::Instrument);
	}
	void trackAndClipPagesPreserveNativeOwnersAndIndices()
	{
		QFETCH(QString, section); QFETCH(bool, pattern); QFETCH(int, type);
		const auto owner = pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : Engine::getSong();
		const auto container = pattern ? QString("pattern") : QString("song");
		if (pattern) { for (int index = 0; index < (section == "clips" ? 145 : 1); ++index) { Track::create(Track::Type::Pattern, Engine::getSong()); } }
		Track* selected = nullptr;
		if (section == "tracks")
		{
			for (int index = 0; index < 145; ++index) { Track::create(static_cast<Track::Type>(type), owner)->setName("Track " + QString::number(index)); }
		}
		else
		{
			selected = Track::create(static_cast<Track::Type>(type), owner);
			if (!pattern) { for (int index = 0; index < 145; ++index) { selected->createClip(TimePos(index * 192)); } }
			for (auto clip : selected->getClips())
			{
				if (auto midi = dynamic_cast<MidiClip*>(clip)) { fill(midi, 3); }
				if (auto curve = dynamic_cast<AutomationClip*>(clip)) { curve->putValue(17, .5, false); }
			}
		}
		const int count = section == "tracks" ? owner->tracks().size() : selected->numOfClips(); QSet<int> seen; int offset = 0;
		do
		{
			QJsonObject args{{"section", section}, {"container", container}, {"offset", offset}, {"limit", 23}};
			if (section == "clips") { args.insert("track", 0); }
			const auto response = inspect(args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			const auto result = response.value("result").toObject(); QCOMPARE(result.value("container").toString(), container); QCOMPARE(result.value("total").toInt(), count);
			const auto items = result.value(section).toArray(); QVERIFY(!items.isEmpty()); QVERIFY(items.size() <= 23);
			for (const auto& value : items)
			{
				const auto item = value.toObject(); const int index = item.value("index").toInt(); QVERIFY(!seen.contains(index)); seen.insert(index);
				if (section == "tracks") { QCOMPARE(item.value("name").toString(), owner->tracks().at(index)->name()); QVERIFY(!item.contains("clips")); }
				else
				{
					const auto clip = selected->getClip(index); QCOMPARE(item.value("position").toInt(), int(clip->startPosition())); QCOMPARE(item.value("length").toInt(), int(clip->length()));
					QVERIFY(!item.contains("notes")); if (type == int(Track::Type::Instrument)) { QCOMPARE(item.value("noteCount").toInt(), 3); }
					if (type == int(Track::Type::Automation)) { QVERIFY(!item.value("automation").toObject().contains("points")); QCOMPARE(item.value("automation").toObject().value("pointCount").toInt(), 1); }
					if (type == int(Track::Type::Sample)) { QVERIFY(item.value("sample").isObject()); }
				}
			}
			QCOMPARE(result.value("nextOffset").toInt(), offset + items.size()); offset = result.value("nextOffset").toInt();
			QCOMPARE(result.value("hasMore").toBool(), offset < count);
		} while (offset < count);
		QCOMPARE(seen.size(), count);
	}

	void notePagesRetainOrderValuesAndEditableIndices_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void notePagesRetainOrderValuesAndEditableIndices()
	{
		QFETCH(bool, pattern); if (pattern) { Track::create(Track::Type::Pattern, Engine::getSong()); }
		const auto owner = pattern ? static_cast<TrackContainer*>(Engine::patternStore()) : Engine::getSong();
		const auto track = Track::create(Track::Type::Instrument, owner); auto midi = static_cast<MidiClip*>(pattern ? track->getClip(0) : track->createClip(0)); fill(midi, 1025);
		const auto container = pattern ? QString("pattern") : QString("song"); const QJsonObject selector{{"container", container}, {"track", 0}, {"clip", 0}};
		auto detuning = selector; detuning.insert("note", 1024); detuning.insert("position", 0); detuning.insert("value", .75);
		QVERIFY(StudioCapabilities::execute("note.detuning.point", detuning).value("ok").toBool()); int offset = 0;
		while (offset < 1025)
		{
			auto args = selector; args.insert("section", "notes"); args.insert("offset", offset); args.insert("limit", 97);
			const auto response = inspect(args); QVERIFY(response.value("ok").toBool()); const auto page = response.value("result").toObject();
			QCOMPARE(page.value("total").toInt(), 1025); const auto notes = page.value("notes").toArray(); QVERIFY(!notes.isEmpty());
			for (const auto& value : notes)
			{
				const auto item = value.toObject(); QCOMPARE(item.value("index").toInt(), offset++); const auto note = midi->notes().at(item.value("index").toInt());
				QCOMPARE(item.value("position").toInt(), int(note->pos())); QCOMPARE(item.value("length").toInt(), int(note->length()));
				QCOMPARE(item.value("key").toInt(), note->key()); QCOMPARE(item.value("volume").toInt(), int(note->getVolume()));
				QCOMPARE(item.value("panning").toInt(), int(note->getPanning())); QCOMPARE(item.value("type").toInt(), int(note->type()));
				if (item.contains("detuning")) { QCOMPARE(item.value("detuning").toObject().value("pointCount").toInt(), 1); QVERIFY(!item.value("detuning").toObject().contains("points")); }
			}
			QCOMPARE(page.value("nextOffset").toInt(), offset);
		}
		auto edit = selector; edit.insert("note", 1024); edit.insert("key", 77); QVERIFY(StudioCapabilities::execute("note.edit", edit).value("ok").toBool());
		QCOMPARE(midi->notes().at(1024)->key(), 77); QCOMPARE(midi->notes().at(1024)->detuning()->automationClip()->valueAt(0), .75f);
		auto empty = selector; empty.insert("section", "notes"); empty.insert("offset", 2147483647);
		const auto page = inspect(empty).value("result").toObject(); QVERIFY(page.value("notes").toArray().isEmpty()); QVERIFY(!page.value("hasMore").toBool());
	}

	void invalidSectionsAndSelectorsAreReadOnly_data()
	{
		QTest::addColumn<QJsonObject>("args");
		QTest::newRow("unknown-section") << QJsonObject{{"section", "unknown"}};
		QTest::newRow("full-offset") << QJsonObject{{"offset", 0}};
		QTest::newRow("summary-track") << QJsonObject{{"section", "summary"}, {"track", 0}};
		QTest::newRow("tracks-clip") << QJsonObject{{"section", "tracks"}, {"clip", 0}};
		QTest::newRow("clips-no-track") << QJsonObject{{"section", "clips"}};
		QTest::newRow("notes-no-clip") << QJsonObject{{"section", "notes"}, {"track", 0}};
		QTest::newRow("notes-names") << QJsonObject{{"section", "notes"}, {"track", 0}, {"clip", 0}, {"names", false}};
		QTest::newRow("fractional-offset") << QJsonObject{{"section", "tracks"}, {"offset", .5}};
		QTest::newRow("zero-limit") << QJsonObject{{"section", "tracks"}, {"limit", 0}};
		QTest::newRow("excessive-limit") << QJsonObject{{"section", "tracks"}, {"limit", 1025}};
		QTest::newRow("unknown-owner") << QJsonObject{{"section", "tracks"}, {"container", "missing"}};
		QTest::newRow("negative-track") << QJsonObject{{"section", "clips"}, {"track", -1}};
		QTest::newRow("missing-clip") << QJsonObject{{"section", "notes"}, {"track", 0}, {"clip", 7}};
	}
	void invalidSectionsAndSelectorsAreReadOnly()
	{
		QFETCH(QJsonObject, args); auto track = Track::create(Track::Type::Instrument, Engine::getSong()); fill(static_cast<MidiClip*>(track->createClip(0)), 3);
		const auto before = StudioCapabilities::inspect(); const auto result = inspect(args); QVERIFY(!result.value("ok").toBool()); QVERIFY(!result.contains("result"));
		QCOMPARE(StudioCapabilities::inspect(), before);
	}

	void largeInventoryPagesKeepReceiptsAndHistoryUsable()
	{
		QTemporaryDir directory; StudioRuntime studio(directory.path()); QString error; QVERIFY(studio.open(error));
		for (int index = 0; index < 100; ++index) { Track::create(Track::Type::Sample, Engine::getSong())->setName(QString(200000, 'x') + QString::number(index)); }
		const auto before = manifest(directory); int offset = 0; qint64 totalBytes = 0;
		while (offset < 100)
		{
			const auto query = request("page-" + QString::number(offset), "project.inspect", {{"section", "tracks"}, {"offset", offset}});
			const auto response = studio.process(query); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			const auto result = response.value("result").toObject(); const auto items = result.value("tracks").toArray(); QVERIFY(!items.isEmpty());
			const auto bytes = QJsonDocument(items).toJson(QJsonDocument::Compact).size(); QVERIFY(bytes <= 512 * 1024); totalBytes += bytes;
			for (const auto& value : items) { QCOMPARE(value.toObject().value("index").toInt(), offset++); }
			QCOMPARE(result.value("nextOffset").toInt(), offset);
			const auto replay = studio.process(query); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("result"), response.value("result"));
		}
		QVERIFY(totalBytes > 16 * 1024 * 1024); QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		QVERIFY(studio.process(request("continue", "session.inspect")).value("ok").toBool()); QVERIFY(!studio.stopping());
	}

	void oversizedNamesHaveAnExplicitPreviewRoute()
	{
		QTemporaryDir directory; StudioRuntime studio(directory.path()); QString error; QVERIFY(studio.open(error));
		auto track = Track::create(Track::Type::Sample, Engine::getSong()); track->setName(QString(600000, 'x')); auto clip = track->createClip(0); clip->setName(QString(600000, 'y'));
		for (const auto section : {QString("tracks"), QString("clips")})
		{
			QJsonObject args{{"section", section}}; if (section == "clips") { args.insert("track", 0); }
			QVERIFY(!studio.process(request(section + "-full", "project.inspect", args)).value("ok").toBool());
			args.insert("names", false); const auto response = studio.process(request(section + "-preview", "project.inspect", args));
			QVERIFY(response.value("ok").toBool()); const auto item = response.value("result").toObject().value(section).toArray().at(0).toObject();
			QVERIFY(!item.contains("name")); QVERIFY(item.value("nameTruncated").toBool()); QCOMPARE(item.value("namePreview").toString().size(), 256);
		}
		QVERIFY(studio.process(request("summary", "project.inspect", {{"section", "summary"}})).value("ok").toBool()); QVERIFY(!studio.stopping());
	}

	void largeProjectLoadsWithSummaryAndRecoversNativeContents()
	{
		QTemporaryDir directory; QString error; const auto path = directory.filePath("large.mmp");
		{
			StudioRuntime studio(directory.path()); QVERIFY(studio.open(error));
			for (int index = 0; index < 100; ++index) { Track::create(Track::Type::Sample, Engine::getSong())->setName(QString(200000, 'x') + QString::number(index)); }
			QVERIFY(studio.process(request("save", "project.save", {{"path", path}})).value("ok").toBool());
			QVERIFY(studio.process(request("clear", "project.new", {{"empty", true}, {"summary", true}})).value("ok").toBool());
			const auto load = request("load", "project.load", {{"path", path}, {"summary", true}}); const auto response = studio.process(load);
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			QCOMPARE(response.value("result").toObject().value("inspection").toString(), QString("summary")); QVERIFY(encoded(response).size() < 64 * 1024);
			QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(100)); const auto after = manifest(directory);
			QVERIFY(studio.process(load).value("replayed").toBool()); QCOMPARE(manifest(directory), after);
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(100));
		for (int index = 0; index < 100; ++index) { QCOMPARE(Engine::getSong()->tracks().at(index)->name(), QString(200000, 'x') + QString::number(index)); }
		QVERIFY(recovered.process(request("continued", "track.edit", {{"track", 99}, {"name", "Continued composition"}})).value("ok").toBool());
		QCOMPARE(Engine::getSong()->tracks().at(99)->name(), QString("Continued composition"));
	}

	void cancellationCannotDispatchEventsOrPublishPartialPages()
	{
		for (int index = 0; index < 50; ++index) { Track::create(Track::Type::Sample, Engine::getSong()); }
		const auto before = StudioCapabilities::inspect(); QObject timerOwner; bool eventRan = false; QTimer::singleShot(0, &timerOwner, [&] { eventRan = true; });
		for (const auto section : {QString("summary"), QString("tracks")})
		{
			int checks = 0; FileResourceIO::Scope work([&] { return ++checks >= 8; }, true);
			const auto response = inspect({{"section", section}}); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result"));
			QVERIFY(response.value("error").toString().contains("cancelled")); QVERIFY(!eventRan);
		}
		QCOMPARE(StudioCapabilities::inspect(), before); QCoreApplication::processEvents(); QVERIFY(eventRan);
		QVERIFY(inspect({{"section", "summary"}}).value("ok").toBool());
	}

	void nativeAgentReadsPagedNotesFromItsCompactObservation_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void nativeAgentReadsPagedNotesFromItsCompactObservation()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0, editedIndex = -1, editedKey = -1; bool compact = true;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			const auto input = body.value(protocol == "chat" ? "messages" : "input").toArray(); QJsonObject lastOutput; bool observed = false;
			for (const auto& value : input)
			{
				const auto item = value.toObject(); const auto text = item.value("content").toString();
				if (text.startsWith("Native runtime observation (data):\n"))
				{
					observed = true; const auto summary = QJsonDocument::fromJson(text.mid(text.indexOf('\n') + 1).toUtf8()).object().value("projectSummary").toObject();
					compact = compact && summary.value("inspection").toString() == "summary" && encoded(summary).size() < 64 * 1024
						&& summary.value("tracks").toArray().size() == 1 && !summary.value("tracks").toArray().at(0).toObject().contains("clips");
				}
				if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
				{ lastOutput = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); }
			}
			compact = compact && observed;
			if (++round == 1) { return decision(protocol, "discover", "project_inspect", {{"section", "notes"}, {"track", 0}, {"clip", 0}, {"offset", 129}, {"limit", 1}}); }
			if (round == 2)
			{
				if (!lastOutput.value("ok").toBool()) { throw std::runtime_error("Scripted note discovery failed"); }
				const auto note = lastOutput.value("result").toObject().value("notes").toArray().at(0).toObject(); editedIndex = note.value("index").toInt(); editedKey = note.value("key").toInt() + 1;
				return decision(protocol, "edit", "note_edit", {{"track", 0}, {"clip", 0}, {"note", editedIndex}, {"key", editedKey}});
			}
			return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Inspected and revised the native note."}, {"evidence", QJsonArray{"discover", "edit"}}});
		});
		QVERIFY(studio.open(error)); auto track = Track::create(Track::Type::Instrument, Engine::getSong()); track->setName(QString(300000, 'x'));
		auto midi = static_cast<MidiClip*>(track->createClip(0)); fill(midi, 1025);
		QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
			{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "project-inventory-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Inspect the existing notes and revise one."}})).value("ok").toBool());
		for (int step = 0; step < 6; ++step) { studio.advanceAgent(); }
		QVERIFY2(compact, "Native provider observation expanded the arrangement"); QCOMPARE(editedIndex, 129); QCOMPARE(midi->notes().at(129)->key(), editedKey);
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(!studio.stopping());
	}
};

QTEST_GUILESS_MAIN(StudioProjectInspectionTest)
#include "StudioProjectInspectionTest.moc"
