/* Native new-note batches, complete admission, insertion history and agent recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <algorithm>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <thread>
#include "FileResourceIO.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"items", QJsonArray{QJsonObject{{"position", 144}, {"length", 24}, {"key", 65}}, QJsonObject{{"position", 120}, {"length", 36}, {"key", 67}, {"volume", 110.9}, {"panning", -20.9}}}}}; }
std::vector<MidiClip::NoteSpec> inputs() { return {{144, 24, 65, 85, -20, Note::Type::Regular}, {120, 36, 67, 86, -19, Note::Type::Regular}}; }
MidiClip* midi(int track = 0, int clip = 0) { return static_cast<MidiClip*>(Engine::getSong()->tracks().at(track)->getClip(clip)); }
void populate(MidiClip* clip)
{
	clip->clearNotes(); for (int index = 0; index < 2; ++index) { auto note = clip->addNote(Note(48, 24 + index * 48, 60, 85 + index, -20 + index), false); note->createDetuning(); note->detuning()->automationClip()->putValue(24, 3.1415927f, false); }
	clip->notes()[0]->detuning()->linkToModel(clip->notes()[1]->detuning().get()); clip->notes()[0]->detuning()->automationClip()->mutedModel()->linkToModel(clip->notes()[1]->detuning()->automationClip()->soloModel());
}
void seed()
{
	auto first = new InstrumentTrack(Engine::getSong()), second = new InstrumentTrack(Engine::getSong()); populate(static_cast<MidiClip*>(first->createClip(0))); second->createClip(0);
}
bool linked(const Note* a, const Note* b)
{
	if (!a->detuning() || !b->detuning() || !a->detuning()->isLinkedToModel(b->detuning().get())) { return false; }
	const auto left = a->detuning()->automationClip(), right = b->detuning()->automationClip();
	return left->mutedModel()->isLinkedToModel(right->soloModel()) || right->mutedModel()->isLinkedToModel(left->soloModel());
}
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args, bool catalog = false)
{ if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; } auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}}; }
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output; for (const auto& value : calls) { const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact)); if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); } else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); } }
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; } return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-add-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteBatchAddTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing(); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->clearJournal(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::projectJournal()->setJournalling(false); Engine::projectJournal()->clearJournal(); Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void nativeInputOrderAndOnePublicationRetainOldCompleteControls()
	{
		seed(); const auto old = midi()->notes(); const auto identity = old[0]->selectionIdentity(); old[0]->setSelected(true); const auto helper = old[0]->detuning().get(); const auto curve = helper->automationClip(); auto controller = new MidiController(nullptr); helper->setControllerConnection(new ControllerConnection(controller)); int notifications = 0; bool complete = true; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; complete = complete && midi()->notes().size() == 4 && midi()->notes()[2]->pos().getTicks() == 120 && midi()->notes()[3]->pos().getTicks() == 144; }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); NoteVector inserted; QString error; QVERIFY2(midi()->addNotes(inputs(), inserted, error), qPrintable(error)); QCOMPARE(inserted.size(), std::size_t{2}); QCOMPARE(inserted[0], midi()->notes()[3]); QCOMPARE(inserted[1], midi()->notes()[2]); QCOMPARE(notifications, 1); QVERIFY(complete); QCOMPARE(old[0]->selectionIdentity(), identity); QVERIFY(old[0]->selected()); QCOMPARE(old[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(helper->controllerConnection()->getController(), controller); QVERIFY(linked(old[0], old[1])); QVERIFY(!inserted[0]->selected()); QVERIFY(!inserted[0]->detuning()); QCOMPARE(inserted[0]->volume(), volume_t{85}); QCOMPARE(inserted[1]->panning(), panning_t{-19}); QCOMPARE(midi()->stepCount(), 16);
	}
	void duplicateInputsHaveFreshIdentitiesAndNativeTieOrdering()
	{
		seed(); const auto old = midi()->notes()[0]; auto specs = inputs(); specs[0].position = 24; specs[0].key = 60; specs[1].position = 24; specs[1].key = 80; specs.push_back(specs[1]); NoteVector inserted; QString error; QVERIFY(midi()->addNotes(specs, inserted, error)); QCOMPARE(midi()->notes()[0], inserted[1]); QCOMPARE(midi()->notes()[1], inserted[2]); QCOMPARE(midi()->notes()[2], old); QCOMPARE(midi()->notes()[3], inserted[0]); QVERIFY(inserted[1] != inserted[2]); QVERIFY(inserted[1]->selectionIdentity() != inserted[2]->selectionIdentity()); QVERIFY(inserted[0]->selectionIdentity() != old->selectionIdentity()); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan));
	}
	void nativeSignedLengthsAndStepTypesAreRetained_data()
	{ QTest::addColumn<int>("length"); QTest::newRow("zero") << 0; QTest::newRow("negative-step") << -12; QTest::newRow("positive") << 24; }
	void nativeSignedLengthsAndStepTypesAreRetained()
	{
		QFETCH(int, length); seed(); MidiClip::NoteSpec spec; spec.position = 120; spec.length = length; if (length < 0) { spec.type = Note::Type::Step; } NoteVector inserted; QString error; QVERIFY(midi()->addNotes({spec}, inserted, error)); QCOMPARE(inserted[0]->length().getTicks(), length); QCOMPARE(inserted[0]->type(), spec.type); QCOMPARE(inserted[0]->key(), DefaultKey); QCOMPARE(inserted[0]->volume(), volume_t{DefaultVolume}); QCOMPARE(midi()->stepCount(), 16);
	}
	void emptyNativeBatchClearsOutputWithoutHistoryOrNotification()
	{
		seed(); Engine::projectJournal()->setJournalling(true); int notifications = 0; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); NoteVector inserted{midi()->notes()[0]}; QString error; QVERIFY(midi()->addNotes({}, inserted, error)); QVERIFY(inserted.empty()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(notifications, 0); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void everyInputAndNativeLengthPathIsAdmittedBeforePublication_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("late-endpoint") << 0; QTest::newRow("late-key") << 1; QTest::newRow("late-volume") << 2; QTest::newRow("late-pan") << 3; QTest::newRow("late-position") << 4; QTest::newRow("late-type") << 5; QTest::newRow("zero-meter") << 6; QTest::newRow("invalid-resident-end") << 7; QTest::newRow("history-outside") << 8; QTest::newRow("late-length-overflow") << 9; QTest::newRow("negative-volume") << 10; QTest::newRow("negative-key") << 11; }
	void everyInputAndNativeLengthPathIsAdmittedBeforePublication()
	{
		QFETCH(int, failure); seed(); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); auto specs = inputs(); if (failure == 0) { specs[1].position = MaxSongLength; specs[1].length = 1; } if (failure == 1) { specs[1].key = 128; } if (failure == 2) { specs[1].volume = 201; } if (failure == 3) { specs[1].panning = 101; } if (failure == 4) { specs[1].position = -1; } if (failure == 5) { specs[1].type = static_cast<Note::Type>(2); } if (failure == 6) { TimePos::setTicksPerBar(0); } if (failure == 7) { midi()->notes()[1]->setLength(MaxSongLength); } if (failure == 8) { auto outside = midi(1)->addStepNote(0); outside->createDetuning(); QVERIFY(midi()->notes()[0]->detuning()->linkToModel(outside->detuning().get())); Engine::projectJournal()->setJournalling(true); } if (failure == 9) { specs[1].length = std::numeric_limits<int>::max(); } if (failure == 10) { specs[1].volume = -1; } if (failure == 11) { specs[1].key = -1; } const auto roster = midi()->notes(); NoteVector inserted; QString error; QVERIFY(!midi()->addNotes(specs, inserted, error)); QVERIFY(!error.isEmpty()); QVERIFY(inserted.empty()); QCOMPARE(midi()->notes(), roster); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void ownerAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("old-note-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("meter-changed") << 5; QTest::newRow("old-volume-changed") << 6; QTest::newRow("count-changed") << 7; }
	void ownerAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto first = clip->notes()[0]; const QPointer<MidiClip> weak(clip); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); bool success = true, invoked = false, timer = false; int capture; NoteVector inserted; QString error; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->addNotes(inputs(), inserted, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->addNotes(inputs(), inserted, error); }); worker.join(); } else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(first); } if (failure == 4) { delete clip; } if (failure == 5) { TimePos::setTicksPerBar(bar + 12); } if (failure == 6) { first->setVolume(99); } if (failure == 7) { clip->addSteps(); } } return failure == 2; }, false); success = clip->addNotes(inputs(), inserted, error); } QVERIFY(!success); QVERIFY(inserted.empty()); QVERIFY(!timer); QVERIFY(!error.isEmpty()); if (weak) { QCOMPARE(weak->notes().size(), failure == 3 ? std::size_t{1} : std::size_t{2}); }
	}
	void nativeUndoRedoUsesOneCompleteClipCheckpoint()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); NoteVector inserted; QString error; QVERIFY(midi()->addNotes(inputs(), inserted, error)); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(!journal->canUndo()); journal->redo(); QCOMPARE(midi()->notes().size(), std::size_t{4}); QCOMPARE(midi()->notes()[2]->key(), 67); QCOMPARE(midi()->notes()[3]->key(), 65); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void malformedStructuredItemsRefuseTheCompleteBatch_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("unknown-nested") << 0; QTest::newRow("missing-position") << 1; QTest::newRow("missing-length") << 2; QTest::newRow("missing-key") << 3; QTest::newRow("non-object") << 4; QTest::newRow("empty") << 5; QTest::newRow("too-many") << 6; QTest::newRow("fractional-key") << 7; QTest::newRow("late-endpoint") << 8; QTest::newRow("late-volume") << 9; QTest::newRow("sample-track") << 10; QTest::newRow("missing-track") << 11; }
	void malformedStructuredItemsRefuseTheCompleteBatch()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); auto items = args.value("items").toArray(); auto last = items[1].toObject(); if (failure == 0) { last.insert("unknown", true); } if (failure == 1) { last.remove("position"); } if (failure == 2) { last.remove("length"); } if (failure == 3) { last.remove("key"); } if (failure == 7) { last.insert("key", 60.5); } if (failure == 8) { last.insert("position", MaxSongLength); } if (failure == 9) { last.insert("volume", 201); } items[1] = last; if (failure == 4) { items[1] = 0; } if (failure == 5) { items = {}; } if (failure == 6) { items = {}; for (int index = 0; index < 4097; ++index) { items.append(last); } } args.insert("items", items); if (failure == 10) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 11) { args.remove("track"); } const auto roster = midi()->notes(); QVERIFY(!StudioCapabilities::execute("note.batch.add", args).value("ok").toBool()); QCOMPARE(midi()->notes(), roster);
	}
	void inputOrderReceiptsAndNativeNumericConversionWorkInBothContainers_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void inputOrderReceiptsAndNativeNumericConversionWorkInBothContainers()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); const auto response = StudioCapabilities::execute("note.batch.add", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("added").toInt(), 2); const auto receipts = result.value("notes").toArray(); QCOMPARE(receipts[0].toObject().value("input").toInt(), 0); QCOMPARE(receipts[0].toObject().value("note").toInt(), 3); QCOMPARE(receipts[1].toObject().value("input").toInt(), 1); QCOMPARE(receipts[1].toObject().value("note").toInt(), 2); QCOMPARE(receipts[0].toObject().value("volume").toInt(), int{DefaultVolume}); QCOMPARE(receipts[1].toObject().value("volume").toInt(), 110); QCOMPARE(receipts[1].toObject().value("panning").toInt(), -20); QVERIFY(linked(clip->notes()[0], clip->notes()[1]));
	}
	void existingSingleNoteAddKeepsItsReceiptAndGetsNativeHistory()
	{
		seed(); Engine::projectJournal()->setJournalling(true); const auto response = StudioCapabilities::execute("note.add", {{"track", 0}, {"clip", 0}, {"position", 48}, {"length", 24}, {"key", 65}}); QVERIFY(response.value("ok").toBool()); const auto result = response.value("result").toObject(); QCOMPARE(result.size(), 1); QCOMPARE(result.value("note").toInt(), 1); QCOMPARE(midi()->notes().size(), std::size_t{3}); Engine::projectJournal()->undo(); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeReplayHistoryAndReopeningPreserveCreatedNotes_data()
	{ QTest::addColumn<bool>("single"); QTest::newRow("single") << true; QTest::newRow("batch") << false; }
	void runtimeReplayHistoryAndReopeningPreserveCreatedNotes()
	{
		QFETCH(bool, single); QTemporaryDir directory; QString error; const auto command = request("add", single ? "note.add" : "note.batch.add", single ? QJsonObject{{"track", 0}, {"clip", 0}, {"position", 120}, {"length", 24}, {"key", 65}} : arguments()); QJsonObject receipt; const auto size = single ? std::size_t{3} : std::size_t{4}; { StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QCOMPARE(runtime.process(command), receipt); QCOMPARE(midi()->notes().size(), size); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), size); } StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(reopened.process(command), receipt); QCOMPARE(midi()->notes().size(), size); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRecoversReturnedCreationRetirementAndScalarFailures_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("new-note-retired") << 1; QTest::newRow("old-note-retired") << 2; QTest::newRow("new-scalar-changed") << 3; }
	void runtimeRecoversReturnedCreationRetirementAndScalarFailures()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto connection = QObject::connect(clip, &MidiClip::dataChanged, this, [&] { if (done) { return; } done = true; if (failure == 0) { throw std::runtime_error("fixture batch creation notification"); } if (failure == 3) { clip->notes()[2]->setVolume(111); } else { clip->removeNote(clip->notes()[failure == 1 ? 2 : 0]); } }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.batch.add", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QCOMPARE(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsCreateThenEditAndInspectFutureNotes_data() { protocols(); }
	void scriptedAgentsCreateThenEditAndInspectFutureNotes()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Compose the next phrase"}, {"steps", QJsonArray{"Create notes", "Change a new velocity", "Inspect the note"}}}), call("add", "note.batch.add", arguments(), catalog), call("edit", "note.edit", {{"track", 0}, {"clip", 0}, {"note", 3}, {"volume", 110}}), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 3}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase created"}, {"evidence", QJsonArray{"add", "edit", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Create two notes, make the later one louder and inspect it."}})).value("ok").toBool()); for (int index = 0; index < 8; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "add").value("ok").toBool()); QVERIFY(record(directory, "edit").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes()[3]->volume(), volume_t{110});
	}
	void scriptedAgentsSkipReplacedClipOwners_data() { protocols(); }
	void scriptedAgentsSkipReplacedClipOwners()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("first", "clip.remove", {{"track", 0}, {"clip", 0}}, true), call("stale", "note.batch.add", arguments(), catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old clip"}, {"evidence", QJsonArray{"first"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed removed clip"}, {"evidence", QJsonArray{"first"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Create notes after their clip changes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "first").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void scriptedAgentsResolveCurrentContentsAfterClear_data() { protocols(); }
	void scriptedAgentsResolveCurrentContentsAfterClear()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("clear", "note.clear", {{"track", 0}, {"clip", 0}}, true), call("add", "note.batch.add", arguments(), catalog), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Current row replaced with new notes"}, {"evidence", QJsonArray{"clear", "add"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Clear this row then create two new notes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "add").value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->key(), 67); QCOMPARE(midi()->notes()[1]->key(), 65);
	}
	void largeBatchHasCompleteReceiptsAndLeavesOldControlGraphIntact()
	{
		seed(); const auto old = midi()->notes(); QJsonArray items; for (int index = 0; index < 1050; ++index) { items.append(QJsonObject{{"position", 192 + index * 24}, {"length", 24}, {"key", 60 + index % 2}}); } auto args = arguments(); args.insert("items", items); const auto response = StudioCapabilities::execute("note.batch.add", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("added").toInt(), 1050); QCOMPARE(response.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(midi()->notes().size(), std::size_t{1052}); QCOMPARE(midi()->notes()[0], old[0]); QCOMPARE(midi()->notes()[1], old[1]); QVERIFY(linked(old[0], old[1])); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan));
	}
	void schemaAndDirectCatalogRoutesExposeNestedArraysAndFitBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.batch.add") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(properties.size(), 4); const auto items = properties.value("items").toObject(); QCOMPARE(items.value("type").toString(), QString("array")); QCOMPARE(items.value("minItems").toInt(), 1); QCOMPARE(items.value("maxItems").toInt(), 4096); QVERIFY(!items.contains("uniqueItems")); const auto entry = items.value("items").toObject(); QCOMPARE(entry.value("type").toString(), QString("object")); QVERIFY(!entry.value("additionalProperties").toBool()); QCOMPARE(entry.value("required").toArray().size(), 3); QCOMPARE(entry.value("properties").toObject().size(), 5); for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, single = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_batch_add"; single = single || tool.value("name") == "note_add"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(single); QVERIFY(catalog); } QJsonObject args{{"op", "note.batch.add"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.batch.add")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteBatchAddTest)
#include "StudioNoteBatchAddTest.moc"
