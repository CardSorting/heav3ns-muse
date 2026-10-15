/* Native note splitting, independent right control graphs and runtime recovery.
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
#include "NativeNoteTransfer.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "NativeSongTestAccess.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "NativeReplayTestAccess.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}, {"position", 96}}; }
MidiClip* midi(int track = 0, int clip = 0) { return static_cast<MidiClip*>(Engine::getSong()->tracks().at(track)->getClip(clip)); }
void populate(MidiClip* clip)
{
	clip->clearNotes(); for (int index = 0; index < 2; ++index) { auto note = clip->addNote(Note(120 - index * 24, 24 + index * 48, 60 + index, 85 + index, -20 + index), false); note->createDetuning(); note->detuning()->automationClip()->putValue(24, 3.1415927f, false); }
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-split-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionSplitTest : public QObject
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

	void directSplitRetainsLeftIdentitiesAndCopiesIndependentRightGraph()
	{
		seed(); const auto notes = midi()->notes(); notes[0]->setSelected(true); notes[1]->setSelected(false); const auto identity = notes[0]->selectionIdentity(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); NoteVector rights{notes[1], notes[0]}; QString error;
		QVERIFY2(NativeNoteTransfer::split(midi(), rights, 96, rights, error), qPrintable(error)); QCOMPARE(rights.size(), std::size_t{2}); QCOMPARE(midi()->notes().size(), std::size_t{4}); QCOMPARE(midi()->notes()[0], notes[0]); QCOMPARE(midi()->notes()[1], notes[1]); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(notes[0]->length().getTicks(), 72); QCOMPARE(notes[1]->length().getTicks(), 24); QCOMPARE(rights[0]->pos().getTicks(), 96); QCOMPARE(rights[0]->length().getTicks(), 72); QCOMPARE(rights[1]->length().getTicks(), 48); QVERIFY(!rights[0]->selected()); QVERIFY(rights[1]->selected()); QVERIFY(linked(notes[0], notes[1])); QVERIFY(linked(rights[0], rights[1])); QVERIFY(!helper->isLinkedToModel(rights[1]->detuning().get())); QVERIFY(rights[1]->selectionIdentity() != identity); QCOMPARE(rights[1]->volume(), notes[0]->volume()); QCOMPARE(rights[1]->panning(), notes[0]->panning()); QCOMPARE(rights[1]->detuning()->automationClip()->valueAt(24), curve->valueAt(24));
	}
	void ownedMidiControllerRemainsIndependentOnRight()
	{
		seed(); auto helper = midi()->notes()[0]->detuning().get(); helper->setValue(2); auto controller = new MidiController(nullptr); controller->midiPort()->setInputController(74); helper->setControllerConnection(new ControllerConnection(controller)); NoteVector rights; QString error; QVERIFY(midi()->splitNotes(midi()->notes(), 96, rights, error)); QCOMPARE(helper->controllerConnection()->getController(), static_cast<Controller*>(controller)); QVERIFY(rights[0]->detuning()->controllerConnection()); QVERIFY(rights[0]->detuning()->controllerConnection()->getController() != controller); QVERIFY(linked(rights[0], rights[1]));
	}
	void nativeGeometrySkipsBoundariesAndNonpositiveLengths_data()
	{
		QTest::addColumn<int>("start"); QTest::addColumn<int>("length"); QTest::addColumn<int>("cut"); QTest::addColumn<int>("left"); QTest::addColumn<int>("right");
		QTest::newRow("inside") << 0 << 96 << 48 << 48 << 48; QTest::newRow("at-start") << 0 << 96 << 0 << 96 << 0;
		QTest::newRow("at-end") << 0 << 96 << 96 << 96 << 0; QTest::newRow("after-end") << 0 << 96 << 120 << 96 << 0;
		QTest::newRow("before-start") << 24 << 48 << 0 << 48 << 0; QTest::newRow("zero-length") << 24 << 0 << 24 << 0 << 0;
		QTest::newRow("negative-step") << 24 << -48 << 48 << -48 << 0; QTest::newRow("one-tick-halves") << 0 << 2 << 1 << 1 << 1;
		QTest::newRow("near-song-end") << MaxSongLength - 48 << 48 << MaxSongLength - 24 << 24 << 24;
	}
	void nativeGeometrySkipsBoundariesAndNonpositiveLengths()
	{
		QFETCH(int, start); QFETCH(int, length); QFETCH(int, cut); QFETCH(int, left); QFETCH(int, right); seed(); midi()->clearNotes(); auto note = midi()->addNote(Note(length, start, 60), false); if (length < 0) { note->setType(Note::Type::Step); } const auto type = note->type(); const auto identity = note->selectionIdentity(); NoteVector rights; QString error; QVERIFY2(midi()->splitNotes(NoteVector{note}, cut, rights, error), qPrintable(error)); QCOMPARE(rights.size(), std::size_t{1}); QCOMPARE(note->length().getTicks(), left); QCOMPARE(note->type(), type); QCOMPARE(note->selectionIdentity(), identity); QCOMPARE(midi()->notes().size(), static_cast<std::size_t>(right ? 2 : 1)); if (right) { QVERIFY(rights[0]); QCOMPARE(rights[0]->length().getTicks(), right); QCOMPARE(rights[0]->pos().getTicks(), cut); QCOMPARE(rights[0]->type(), type); } else { QVERIFY(!rights[0]); }
	}
	void rightFragmentOrderingSupportsLaterNativeInsertion()
	{
		seed(); midi()->clearNotes(); auto low = midi()->addNote(Note(96, 0, 60), false), high = midi()->addNote(Note(96, 0, 72), false); NoteVector rights; QString error; QVERIFY(midi()->splitNotes(NoteVector{low, high}, 48, rights, error)); QCOMPARE(midi()->notes()[2], rights[1]); QCOMPARE(midi()->notes()[3], rights[0]); auto middle = midi()->addNote(Note(24, 48, 65), false); QCOMPARE(midi()->notes()[2], rights[1]); QCOMPARE(midi()->notes()[3], middle); QCOMPARE(midi()->notes()[4], rights[0]); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan));
	}
	void incompleteCrossingGroupsRefuseBeforeShortening_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("unselected-linked-peer") << 0; QTest::newRow("selected-noncrossing-peer") << 1; QTest::newRow("outside-linked-control") << 2; }
	void incompleteCrossingGroupsRefuseBeforeShortening()
	{
		QFETCH(int, failure); seed(); auto selected = midi()->notes(); if (failure == 0) { selected.pop_back(); } if (failure == 1) { midi()->notes()[1]->setPos(120); } if (failure == 2) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(midi(1)->mutedModel())); } NoteVector rights; QString error; QVERIFY(!midi()->splitNotes(selected, 96, rights, error)); QVERIFY(rights.empty()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void malformedNativeSelectionsRefuseBeforeEditing_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("null") << 0; QTest::newRow("duplicate") << 1; QTest::newRow("foreign") << 2; QTest::newRow("negative-source-position") << 3; QTest::newRow("source-length") << 4; QTest::newRow("source-endpoint") << 5; QTest::newRow("source-key") << 6; QTest::newRow("negative-cut") << 7; QTest::newRow("oversize-cut") << 8; }
	void malformedNativeSelectionsRefuseBeforeEditing()
	{
		QFETCH(int, failure); seed(); const auto notes = midi()->notes(); auto selected = notes; int cut = 96; if (failure == 0) { selected.push_back(nullptr); } if (failure == 1) { selected.push_back(notes[0]); } if (failure == 2) { selected.push_back(midi(1)->addNote(Note(48, 0, 60), false)); } if (failure == 3) { notes[1]->setPos(-1); } if (failure == 4) { notes[1]->setLength(MaxSongLength + 1); } if (failure == 5) { notes[1]->setPos(MaxSongLength); } if (failure == 6) { *notes[1] = Note(96, 72, NumKeys); } if (failure == 7) { cut = -1; } if (failure == 8) { cut = MaxSongLength + 1; } NoteVector rights; QString error; QVERIFY(!midi()->splitNotes(selected, cut, rights, error)); QVERIFY(!error.isEmpty()); QVERIFY(rights.empty()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(notes[0]->length().getTicks(), 120);
	}
	void unchangedAndEmptySelectionsPublishNoNativeHistory()
	{
		seed(); auto journal = Engine::projectJournal(); QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); journal->setJournalling(true); QSignalSpy notifications(midi(), &MidiClip::dataChanged); NoteVector rights; QString error; QVERIFY(midi()->splitNotes(midi()->notes(), 0, rights, error)); QCOMPARE(rights.size(), std::size_t{2}); QVERIFY(!rights[0] && !rights[1]); QVERIFY(!journal->canUndo()); QVERIFY(midi()->splitNotes({}, 96, rights, error)); QVERIFY(rights.empty()); QVERIFY(!journal->canUndo()); QCOMPARE(notifications.count(), 0);
	}
	void nativeClipUndoRedoPreservesBothIndependentGraphs()
	{
		seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); midi()->splitNotes(midi()->notes(), 96); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3])); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3])); QVERIFY(!midi()->notes()[0]->detuning()->isLinkedToModel(midi()->notes()[2]->detuning().get())); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void directNotificationFailureLeavesCheckpointAndEmptyResult()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [] { throw std::runtime_error("fixture direct split notification"); }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); NoteVector rights; QString error; QVERIFY(!midi()->splitNotes(midi()->notes(), 96, rights, error)); QVERIFY(rights.empty()); QCOMPARE(midi()->notes().size(), std::size_t{4}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 72); QVERIFY(journal->canUndo()); QObject::disconnect(connection); journal->undo(); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void ownershipRecordingAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("source-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("unselected-position-changed") << 5; QTest::newRow("selection-flags-changed") << 6; }
	void ownershipRecordingAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto notes = clip->notes(); auto outside = failure == 5 ? clip->addNote(Note(48, 192, 70), false) : nullptr; const QPointer<MidiClip> weak(clip); NoteVector rights; bool success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->splitNotes(notes, 96, rights, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->splitNotes(notes, 96, rights, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(notes[0]); } if (failure == 4) { delete clip; } if (failure == 5) { outside->setPos(200); } if (failure == 6) { notes[0]->setSelected(true); } } return failure == 2; }, false); success = clip->splitNotes(notes, 96, rights, error); }
		QVERIFY(!success); QVERIFY(!error.isEmpty()); QVERIFY(!timer); QVERIFY(rights.empty()); if (weak && failure != 3) { QCOMPARE(weak->notes()[0]->length().getTicks(), 120); } if (failure == 5) { QCOMPARE(outside->pos().getTicks(), 200); }
	}
	void invalidStructuredRequestsRefuseMutation_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fractional-index") << 3; QTest::newRow("negative-index") << 4; QTest::newRow("string-index") << 5; QTest::newRow("missing-note") << 6; QTest::newRow("too-many") << 7; QTest::newRow("missing-notes") << 8; QTest::newRow("unknown-grid") << 9; QTest::newRow("invalid-container") << 10; QTest::newRow("wrong-type") << 11; QTest::newRow("bool-index") << 12; QTest::newRow("missing-cut") << 13; QTest::newRow("negative-cut") << 14; QTest::newRow("oversize-cut") << 15; QTest::newRow("fractional-cut") << 16;
	}
	void invalidStructuredRequestsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{2}); } if (failure == 7) { QJsonArray notes; for (int i = 0; i < 4097; ++i) { notes.append(i); } args.insert("notes", notes); } if (failure == 8) { args.remove("notes"); } if (failure == 9) { args.insert("grid", 48); } if (failure == 10) { args.insert("container", "other"); } if (failure == 11) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 12) { args.insert("notes", QJsonArray{true}); } if (failure == 13) { args.remove("position"); } if (failure == 14) { args.insert("position", -1); } if (failure == 15) { args.insert("position", MaxSongLength + 1); } if (failure == 16) { args.insert("position", .5); }
		QVERIFY(!StudioCapabilities::execute("note.selection.split", args).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void receiptsIncludeCurrentLeftRightAndUnchangedIndices_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void receiptsIncludeCurrentLeftRightAndUnchangedIndices()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto outside = clip->addNote(Note(24, 144, 70), false); auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); args.insert("notes", QJsonArray{2, 1, 0}); const auto before = StudioCapabilities::selection("note.selection.split", args); const auto response = StudioCapabilities::execute("note.selection.split", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("split").toInt(), 2); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 2); QVERIFY(!receipt[0].toObject().value("split").toBool()); QCOMPARE(receipt[0].toObject().value("note").toInt(), 4); QVERIFY(!receipt[0].toObject().contains("rightNote")); QCOMPARE(receipt[1].toObject().value("sourceNote").toInt(), 1); QCOMPARE(receipt[1].toObject().value("note").toInt(), 1); QCOMPARE(receipt[1].toObject().value("length").toInt(), 24); QCOMPARE(receipt[1].toObject().value("rightNote").toInt(), 2); QCOMPARE(receipt[1].toObject().value("rightLength").toInt(), 72); QCOMPARE(receipt[2].toObject().value("rightNote").toInt(), 3); QCOMPARE(clip->notes()[4], outside); QVERIFY(before.changed(StudioCapabilities::selection("note.selection.split", args)));
	}
	void runtimeHistoryReplayAndReopeningPreserveSplitGraphs()
	{
		QTemporaryDir directory; QString error; const auto command = request("split", "note.selection.split", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3])); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3]));
	}
	void runtimeRecoversReturnedNotificationFailureOrRetirement_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("right-fragment-retired") << 1; QTest::newRow("left-retired-from-song-notification") << 2; }
	void runtimeRecoversReturnedNotificationFailureOrRetirement()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto changed = [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[failure == 1 ? 2 : 0]); } else { throw std::runtime_error("fixture split notification"); } }; if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); } const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(clip, &MidiClip::dataChanged, this, changed, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.selection.split", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsSplitAndInspectFutureRightFragment_data() { protocols(); }
	void scriptedAgentsSplitAndInspectFutureRightFragment()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Split the phrase"}, {"steps", QJsonArray{"Split crossing notes", "Inspect a right fragment"}}, {"decisions", QJsonArray{}}}), call("split", "note.selection.split", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 2}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase split"}, {"evidence", QJsonArray{"split", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Split the phrase at tick 96 and inspect a new right fragment."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "split").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3]));
	}
	void scriptedAgentsSkipShiftedUnchangedIndicesAndReplan_data() { protocols(); }
	void scriptedAgentsSkipShiftedUnchangedIndicesAndReplan()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("split", "note.selection.split", arguments(), catalog), call("stale", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 2}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old indices"}, {"evidence", QJsonArray{"split"}}})}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 4}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed new indices"}, {"evidence", QJsonArray{"split", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); midi()->addNote(Note(24, 144, 70), false)->createDetuning(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Split the phrase and rediscover the unchanged later note."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "split").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes()[4]->pos().getTicks(), 144);
	}
	void largeSelectionPreservesTwoIndependentLinkedGroups()
	{
		seed(); midi()->clearNotes(); QJsonArray selected; DetuningHelper* anchor = nullptr; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(120, 24, 60 + index % 12), false); note->createDetuning(); if (anchor) { QVERIFY(anchor->linkToModel(note->detuning().get())); } else { anchor = note->detuning().get(); } selected.append(index); } const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); auto args = arguments(); args.insert("notes", selected); const auto response = StudioCapabilities::execute("note.selection.split", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("split").toInt(), 1050); QCOMPARE(result.value("notes").toArray().size(), 1050); QCOMPARE(midi()->notes().size(), std::size_t{2100}); QCOMPARE(midi()->notes()[0], first); QCOMPARE(first->selectionIdentity(), identity); QCOMPARE(first->length().getTicks(), 72); QCOMPARE(first->detuning()->countLinks(), std::size_t{1049}); QCOMPARE(midi()->notes()[1050]->detuning()->countLinks(), std::size_t{1049}); QVERIFY(!first->detuning()->isLinkedToModel(midi()->notes()[1050]->detuning().get()));
	}
	void schemaAndBothToolRoutesRetainBoundsAndDeclarationBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.split") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(properties.size(), 5); QCOMPARE(properties.value("position").toObject().value("maximum").toInt(), MaxSongLength); QCOMPARE(properties.value("notes").toObject().value("maxItems").toInt(), 4096);
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_split"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.split"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.split")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionSplitTest)
#include "StudioNoteSelectionSplitTest.moc"
