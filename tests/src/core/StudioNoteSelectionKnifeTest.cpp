/* Native knife geometry, longer-half identity and structured agent recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
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
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}, {"notes", QJsonArray{1, 0}}, {"fromPosition", 72}, {"fromKey", 59}, {"toPosition", 168}, {"toKey", 63}}; }
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-note-knife-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
bool applyKnife(MidiClip* clip, const NoteVector& notes, bool keep, MidiClip::KnifeResult& result, QString& error) { return clip->splitNotesAlongLine(notes, 72, 59, 168, 63, keep, result, error); }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioNoteSelectionKnifeTest : public QObject
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

	void nativeLineCutsRetainOriginalsAndCopyIndependentGraph()
	{
		seed(); const auto notes = midi()->notes(); notes[0]->setSelected(true); const auto identity = notes[0]->selectionIdentity(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); MidiClip::KnifeResult result; result.rights = {notes[1], notes[0]}; QString error;
		QVERIFY2(applyKnife(midi(), result.rights, false, result, error), qPrintable(error)); QCOMPARE(result.positions.size(), std::size_t{2}); QCOMPARE(result.positions[0], 108); QCOMPARE(result.positions[1], 84); QCOMPARE(notes[0]->length().getTicks(), 60); QCOMPARE(notes[1]->length().getTicks(), 36); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(result.rights[0]->pos().getTicks(), 108); QCOMPARE(result.rights[1]->pos().getTicks(), 84); QCOMPARE(result.rights[0]->length().getTicks(), 60); QCOMPARE(result.rights[1]->length().getTicks(), 60); QVERIFY(!result.rights[0]->selected()); QVERIFY(result.rights[1]->selected()); QVERIFY(linked(notes[0], notes[1])); QVERIFY(linked(result.rights[0], result.rights[1])); QVERIFY(!helper->isLinkedToModel(result.rights[1]->detuning().get())); QCOMPARE(result.rights[1]->detuning()->automationClip()->valueAt(24), curve->valueAt(24));
	}
	void keepingLongerRetainsControlsAndAllowsOutsideCopyLinks()
	{
		seed(); const auto notes = midi()->notes(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); auto secondHelper = notes[1]->detuning().get(); const auto identity = notes[1]->selectionIdentity(); auto controller = new MidiController(nullptr); helper->setControllerConnection(new ControllerConnection(controller)); QVERIFY(helper->linkToModel(midi(1)->mutedModel())); MidiClip::KnifeResult result; QString error;
		QVERIFY(!applyKnife(midi(), notes, false, result, error)); QVERIFY(result.rights.empty()); QVERIFY(result.positions.empty()); QCOMPARE(notes[0]->length().getTicks(), 120);
		QVERIFY2(applyKnife(midi(), notes, true, result, error), qPrintable(error)); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(notes[0]->pos().getTicks(), 24); QCOMPARE(notes[0]->length().getTicks(), 60); QCOMPARE(notes[1]->pos().getTicks(), 108); QCOMPARE(notes[1]->length().getTicks(), 60); QVERIFY(!result.rights[0] && !result.rights[1]); QCOMPARE(notes[1]->selectionIdentity(), identity); QCOMPARE(notes[1]->detuning().get(), secondHelper); QCOMPARE(helper->controllerConnection()->getController(), static_cast<Controller*>(controller)); QCOMPARE(helper->automationClip(), curve); QVERIFY(linked(notes[0], notes[1])); QVERIFY(helper->isLinkedToModel(midi(1)->mutedModel()));
	}
	void nativeGeometryPreservesRowFilteringTruncationAndExtremeEndpoints_data()
	{
		QTest::addColumn<int>("from"); QTest::addColumn<int>("fromKey"); QTest::addColumn<int>("to"); QTest::addColumn<int>("toKey"); QTest::addColumn<int>("key"); QTest::addColumn<int>("start"); QTest::addColumn<int>("length"); QTest::addColumn<int>("cut");
		QTest::newRow("slope") << 72 << 59 << 168 << 63 << 60 << 24 << 120 << 84;
		QTest::newRow("reversed-endpoints") << 168 << 63 << 72 << 59 << 60 << 24 << 120 << 84;
		QTest::newRow("minimum-row-excluded") << 72 << 59 << 168 << 63 << 59 << 24 << 120 << -1;
		QTest::newRow("maximum-row-included") << 72 << 59 << 168 << 63 << 63 << 24 << 240 << 156;
		QTest::newRow("horizontal") << 72 << 60 << 168 << 60 << 60 << 24 << 120 << -1;
		QTest::newRow("same-tick") << 96 << 59 << 96 << 63 << 60 << 24 << 120 << 96;
		QTest::newRow("fraction-truncates") << 80 << 59 << 90 << 62 << 60 << 24 << 120 << 81;
		QTest::newRow("boundary-skips") << 24 << 59 << 24 << 63 << 60 << 24 << 120 << -1;
		QTest::newRow("before-note") << 0 << 59 << 0 << 63 << 60 << 24 << 120 << -1;
		QTest::newRow("negative-endpoint") << -96 << 59 << 288 << 63 << 61 << 24 << 120 << 48;
		QTest::newRow("full-signed-endpoint-deltas") << std::numeric_limits<int>::min() << std::numeric_limits<int>::min() << std::numeric_limits<int>::max() << std::numeric_limits<int>::max() << 60 << 24 << 120 << 59;
		QTest::newRow("negative-step-skips") << 96 << 59 << 96 << 63 << 60 << 24 << -48 << -1;
	}
	void nativeGeometryPreservesRowFilteringTruncationAndExtremeEndpoints()
	{
		QFETCH(int, from); QFETCH(int, fromKey); QFETCH(int, to); QFETCH(int, toKey); QFETCH(int, key); QFETCH(int, start); QFETCH(int, length); QFETCH(int, cut); seed(); midi()->clearNotes(); auto note = midi()->addNote(Note(length, start, key), false); if (length < 0) { note->setType(Note::Type::Step); } const auto type = note->type(); const auto identity = note->selectionIdentity(); MidiClip::KnifeResult result; QString error;
		QVERIFY2(midi()->splitNotesAlongLine(NoteVector{note}, from, fromKey, to, toKey, false, result, error), qPrintable(error)); QCOMPARE(result.positions.size(), std::size_t{1}); QCOMPARE(result.positions[0], cut); QCOMPARE(note->selectionIdentity(), identity); QCOMPARE(note->type(), type); QCOMPARE(note->length().getTicks(), cut < 0 ? length : cut - start); QCOMPARE(midi()->notes().size(), static_cast<std::size_t>(cut < 0 ? 1 : 2)); if (cut >= 0) { QVERIFY(result.rights[0]); QCOMPARE(result.rights[0]->pos().getTicks(), cut); QCOMPARE(result.rights[0]->length().getTicks(), start + length - cut); } else { QVERIFY(!result.rights[0]); }
	}
	void keepingLongerChoosesRightLeftAndLeftOnTie_data()
	{ QTest::addColumn<int>("cut"); QTest::addColumn<int>("position"); QTest::addColumn<int>("length"); QTest::newRow("right-longer") << 24 << 24 << 72; QTest::newRow("left-longer") << 72 << 0 << 72; QTest::newRow("left-on-tie") << 48 << 0 << 48; }
	void keepingLongerChoosesRightLeftAndLeftOnTie()
	{
		QFETCH(int, cut); QFETCH(int, position); QFETCH(int, length); seed(); midi()->clearNotes(); auto note = midi()->addNote(Note(96, 0, 60), false); const auto identity = note->selectionIdentity(); MidiClip::KnifeResult result; QString error; QVERIFY(midi()->splitNotesAlongLine(NoteVector{note}, cut, 59, cut, 61, true, result, error)); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0], note); QCOMPARE(note->selectionIdentity(), identity); QCOMPARE(note->pos().getTicks(), position); QCOMPARE(note->length().getTicks(), length); QCOMPARE(result.positions[0], cut); QVERIFY(!result.rights[0]);
	}
	void malformedNativeSelectionsRefuseBeforeEdits_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("null") << 0; QTest::newRow("duplicate") << 1; QTest::newRow("foreign") << 2; QTest::newRow("negative-position") << 3; QTest::newRow("source-length") << 4; QTest::newRow("source-endpoint") << 5; QTest::newRow("source-key") << 6; }
	void malformedNativeSelectionsRefuseBeforeEdits()
	{
		QFETCH(int, failure); seed(); const auto notes = midi()->notes(); auto selected = notes; if (failure == 0) { selected.push_back(nullptr); } if (failure == 1) { selected.push_back(notes[0]); } if (failure == 2) { selected.push_back(midi(1)->addNote(Note(48, 0, 60), false)); } if (failure == 3) { notes[1]->setPos(-1); } if (failure == 4) { notes[1]->setLength(MaxSongLength + 1); } if (failure == 5) { notes[1]->setPos(MaxSongLength); } if (failure == 6) { *notes[1] = Note(96, 72, NumKeys); } MidiClip::KnifeResult result; QString error; QVERIFY(!applyKnife(midi(), selected, false, result, error)); QVERIFY(!error.isEmpty()); QVERIFY(result.rights.empty()); QVERIFY(result.positions.empty()); QCOMPARE(notes[0]->length().getTicks(), 120); QCOMPARE(midi()->notes().size(), std::size_t{2});
	}
	void crossingCopyClosureAndKeepLongerHaveDifferentAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("unselected-peer") << 0; QTest::newRow("selected-noncrossing-peer") << 1; QTest::newRow("outside-control") << 2; }
	void crossingCopyClosureAndKeepLongerHaveDifferentAdmission()
	{
		QFETCH(int, failure); seed(); const auto notes = midi()->notes(); auto selected = notes; if (failure == 0) { selected.pop_back(); } if (failure == 1) { notes[1]->setPos(192); } if (failure == 2) { QVERIFY(notes[0]->detuning()->linkToModel(midi(1)->mutedModel())); } MidiClip::KnifeResult result; QString error; QVERIFY(!applyKnife(midi(), selected, false, result, error)); QCOMPARE(notes[0]->length().getTicks(), 120); QVERIFY(result.rights.empty()); QVERIFY(applyKnife(midi(), selected, true, result, error)); QCOMPARE(notes[0]->length().getTicks(), 60); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(notes[0], notes[1]));
	}
	void horizontalAndEmptyCallsPublishNoNativeHistory()
	{
		seed(); QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); auto journal = Engine::projectJournal(); journal->setJournalling(true); QSignalSpy notifications(midi(), &MidiClip::dataChanged); MidiClip::KnifeResult result; QString error; QVERIFY(midi()->splitNotesAlongLine(midi()->notes(), -96, 60, 192, 60, false, result, error)); QCOMPARE(result.positions.size(), std::size_t{2}); QCOMPARE(result.positions[0], -1); QCOMPARE(result.positions[1], -1); QVERIFY(!journal->canUndo()); QVERIFY(applyKnife(midi(), {}, true, result, error)); QVERIFY(result.positions.empty()); QVERIFY(result.rights.empty()); QCOMPARE(notifications.count(), 0); QVERIFY(!journal->canUndo());
	}
	void legacyKnifeHistoryRestoresGraphForBothModes_data()
	{ QTest::addColumn<bool>("keep"); QTest::newRow("both") << false; QTest::newRow("longer") << true; }
	void legacyKnifeHistoryRestoresGraphForBothModes()
	{
		QFETCH(bool, keep); seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); midi()->splitNotesAlongLine(midi()->notes(), 72, 59, 168, 63, keep); QCOMPARE(midi()->notes().size(), static_cast<std::size_t>(keep ? 2 : 4)); QCOMPARE(midi()->notes()[0]->length().getTicks(), 60); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(midi()->notes().size(), static_cast<std::size_t>(keep ? 2 : 4)); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); if (keep) { QCOMPARE(midi()->notes()[1]->pos().getTicks(), 108); } else { QVERIFY(linked(midi()->notes()[2], midi()->notes()[3])); } QVERIFY(!Engine::getSong()->hasErrors());
	}
	void ownershipRecordingAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("source-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("unselected-position-changed") << 5; QTest::newRow("selection-flags-changed") << 6; }
	void ownershipRecordingAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto notes = clip->notes(); auto outside = failure == 5 ? clip->addNote(Note(24, 192, 70), false) : nullptr; const QPointer<MidiClip> weak(clip); MidiClip::KnifeResult result; bool success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = applyKnife(clip, notes, false, result, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = applyKnife(clip, notes, false, result, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(notes[0]); } if (failure == 4) { delete clip; } if (failure == 5) { outside->setPos(200); } if (failure == 6) { notes[0]->setSelected(true); } } return failure == 2; }, false); success = applyKnife(clip, notes, false, result, error); }
		QVERIFY(!success); QVERIFY(!error.isEmpty()); QVERIFY(!timer); QVERIFY(result.rights.empty()); QVERIFY(result.positions.empty()); if (weak && failure != 3) { QCOMPARE(weak->notes()[0]->length().getTicks(), 120); }
	}
	void malformedStructuredRequestsRefuseMutation_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("duplicate") << 2; QTest::newRow("fractional-index") << 3; QTest::newRow("negative-index") << 4; QTest::newRow("string-index") << 5; QTest::newRow("missing-note") << 6; QTest::newRow("too-many") << 7; QTest::newRow("missing-notes") << 8; QTest::newRow("unknown-position") << 9; QTest::newRow("invalid-container") << 10; QTest::newRow("wrong-type") << 11; QTest::newRow("bool-index") << 12; QTest::newRow("missing-endpoint") << 13; QTest::newRow("keep-not-bool") << 14; QTest::newRow("fractional-endpoint") << 15; QTest::newRow("endpoint-out-of-native-int") << 16;
	}
	void malformedStructuredRequestsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.insert("notes", "bad"); } if (failure == 1) { args.insert("notes", QJsonArray{}); } if (failure == 2) { args.insert("notes", QJsonArray{0, 0}); } if (failure == 3) { args.insert("notes", QJsonArray{.5}); } if (failure == 4) { args.insert("notes", QJsonArray{-1}); } if (failure == 5) { args.insert("notes", QJsonArray{"0"}); } if (failure == 6) { args.insert("notes", QJsonArray{2}); } if (failure == 7) { QJsonArray notes; for (int i = 0; i < 4097; ++i) { notes.append(i); } args.insert("notes", notes); } if (failure == 8) { args.remove("notes"); } if (failure == 9) { args.insert("position", 96); } if (failure == 10) { args.insert("container", "other"); } if (failure == 11) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 12) { args.insert("notes", QJsonArray{true}); } if (failure == 13) { args.remove("toKey"); } if (failure == 14) { args.insert("keepLongest", 1); } if (failure == 15) { args.insert("fromPosition", .5); } if (failure == 16) { args.insert("fromKey", 2147483648.0); }
		QVERIFY(!StudioCapabilities::execute("note.selection.knife", args).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void requestOrderReceiptsWorkForBothModesAndContainers_data()
	{ QTest::addColumn<bool>("pattern"); QTest::addColumn<bool>("keep"); QTest::newRow("song-both") << false << false; QTest::newRow("song-longer") << false << true; QTest::newRow("pattern-both") << true << false; QTest::newRow("pattern-longer") << true << true; }
	void requestOrderReceiptsWorkForBothModesAndContainers()
	{
		QFETCH(bool, pattern); QFETCH(bool, keep); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } const auto notes = clip->notes(); auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); if (keep) { args.insert("keepLongest", true); } const auto response = StudioCapabilities::execute("note.selection.knife", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("cut").toInt(), 2); QCOMPARE(result.value("keepLongest").toBool(), keep); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceNote").toInt(), 1); QCOMPARE(receipt[0].toObject().value("cutPosition").toInt(), 108); QCOMPARE(receipt[0].toObject().value("kept").toString(), keep ? QString("right") : QString("both")); QCOMPARE(receipt[1].toObject().value("sourceNote").toInt(), 0); QCOMPARE(receipt[1].toObject().value("cutPosition").toInt(), 84); QCOMPARE(receipt[1].toObject().value("kept").toString(), keep ? QString("left") : QString("both")); QCOMPARE(clip->notes()[0], notes[0]); QCOMPARE(clip->notes()[1], notes[1]); if (keep) { QVERIFY(!receipt[0].toObject().contains("rightNote")); QCOMPARE(receipt[0].toObject().value("position").toInt(), 108); } else { QCOMPARE(receipt[0].toObject().value("rightNote").toInt(), 3); QCOMPARE(receipt[1].toObject().value("rightNote").toInt(), 2); }
	}


	void runtimeHistoryReplayAndReopeningPreserveKnifeGraphs()
	{
		QTemporaryDir directory; QString error; const auto command = request("knife", "note.selection.knife", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3])); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3]));
	}

	void runtimeRecoversReturnedNotificationFailureOrRetirement_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("right-fragment-retired") << 1; QTest::newRow("left-retired-from-song-notification") << 2; }

	void runtimeRecoversReturnedNotificationFailureOrRetirement()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto changed = [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[failure == 1 ? 2 : 0]); } else { throw std::runtime_error("fixture knife notification"); } }; if (failure == 2) { NativeSongTestAccess::setModified(Engine::getSong(), false); } const auto connection = failure == 2 ? QObject::connect(Engine::getSong(), &Song::modified, this, changed, Qt::DirectConnection) : QObject::connect(clip, &MidiClip::dataChanged, this, changed, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "note.selection.knife", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->notes().size(), std::size_t{2}); QCOMPARE(midi()->notes()[0]->length().getTicks(), 120); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}

	void scriptedAgentsKnifeAndInspectFutureRightFragment_data() { protocols(); }

	void scriptedAgentsKnifeAndInspectFutureRightFragment()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Knife the phrase"}, {"steps", QJsonArray{"Knife crossing notes", "Inspect a right fragment"}}, {"decisions", QJsonArray{}}}), call("knife", "note.selection.knife", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 2}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native phrase knife"}, {"evidence", QJsonArray{"knife", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Cut along the sloped line and inspect a new right fragment."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "knife").value("ok").toBool()); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3]));
	}
	void scriptedAgentsReplanAfterLongerHalfReordersExistingNotes_data() { protocols(); }
	void scriptedAgentsReplanAfterLongerHalfReordersExistingNotes()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; auto args = arguments(); args.insert("keepLongest", true); StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("knife", "note.selection.knife", args, catalog), call("stale", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 1}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old indices"}, {"evidence", QJsonArray{"knife"}}})}); } return decision(protocol, {call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 2}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed moved retained note"}, {"evidence", QJsonArray{"knife", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); midi()->addNote(Note(24, 96, 70), false)->createDetuning(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Keep the longer halves and rediscover the moved note before inspecting it."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "knife").value("ok").toBool()); QVERIFY(!record(directory, "stale").value("ok").toBool()); QVERIFY(record(directory, "stale").value("error").toString().contains("changed a queued native selection")); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes().size(), std::size_t{3}); QCOMPARE(midi()->notes()[2]->pos().getTicks(), 108);
	}
	void largeCrossingSelectionKeepsIndependentControlGroups()
	{
		seed(); midi()->clearNotes(); QJsonArray selected; DetuningHelper* anchor = nullptr; for (int index = 0; index < 1050; ++index) { auto note = midi()->addNote(Note(120, 24, 60 + index % 2), false); note->createDetuning(); if (anchor) { QVERIFY(anchor->linkToModel(note->detuning().get())); } else { anchor = note->detuning().get(); } selected.append(index); } const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); auto args = arguments(); args.insert("notes", selected); const auto response = StudioCapabilities::execute("note.selection.knife", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("cut").toInt(), 1050); QCOMPARE(response.value("result").toObject().value("notes").toArray().size(), 1050); QCOMPARE(midi()->notes().size(), std::size_t{2100}); QCOMPARE(midi()->notes()[0], first); QCOMPARE(first->selectionIdentity(), identity); QCOMPARE(first->length().getTicks(), 84); QCOMPARE(first->detuning()->countLinks(), std::size_t{1049}); QCOMPARE(midi()->notes()[1050]->detuning()->countLinks(), std::size_t{1049}); QVERIFY(!first->detuning()->isLinkedToModel(midi()->notes()[1050]->detuning().get()));
	}
	void schemaAndBothToolRoutesRetainNativeBoundsAndBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "note.selection.knife") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); const auto properties = schema.value("inputSchema").toObject().value("properties").toObject(); QCOMPARE(properties.value("notes").toObject().value("maxItems").toInt(), 4096); QCOMPARE(properties.value("fromPosition").toObject().value("minimum").toInt(), std::numeric_limits<int>::min()); QCOMPARE(properties.value("toKey").toObject().value("maximum").toInt(), std::numeric_limits<int>::max()); QCOMPARE(properties.value("keepLongest").toObject().value("type").toString(), QString("boolean")); QVERIFY(!properties.value("keepLongest").toObject().value("default").toBool());
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "note_selection_knife"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "note.selection.knife"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("note.selection.knife")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioNoteSelectionKnifeTest)
#include "StudioNoteSelectionKnifeTest.moc"
