/* Native step resizing, atomic roster/count edits, history and agent recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
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
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "NativeReplayTestAccess.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonObject arguments() { return {{"track", 0}, {"clip", 0}}; }
MidiClip* midi(int track = 0, int clip = 0) { return static_cast<MidiClip*>(Engine::getSong()->tracks().at(track)->getClip(clip)); }
void populate(MidiClip* clip)
{
    clip->clearNotes(); for (int index = 0; index < 2; ++index) { auto note = clip->addStepNote(index * 4); note->setKey(60 + index); note->setLength(24 + index * 12); note->setVolume(85 + index); note->setPanning(-20 + index); note->createDetuning(); note->detuning()->automationClip()->putValue(12, 3.1415927f, false); }
    clip->notes()[0]->detuning()->linkToModel(clip->notes()[1]->detuning().get()); clip->notes()[0]->detuning()->automationClip()->mutedModel()->linkToModel(clip->notes()[1]->detuning()->automationClip()->soloModel());
}
void setCount(MidiClip* clip, int count) { QDomDocument document; auto state = document.createElement("midiclip"); document.appendChild(state); clip->saveSettings(document, state); state.setAttribute("steps", count); state.setAttribute("len", 192); clip->loadSettings(state); }

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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-step-resize-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioStepResizeTest : public QObject
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

	void shrinkingClearsAllRemovedCellStepsAndRetainsOtherNotes()
	{
		seed(); MidiClip::StepResizeResult result; QString error; QVERIFY(midi()->resizeSteps(32, result, error)); const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); first->setSelected(true); auto consumed = midi()->addStepNote(16); consumed->createDetuning(); QVERIFY(first->detuning()->linkToModel(consumed->detuning().get())); midi()->addStepNote(20); Note duplicate(12, 192, 80); duplicate.setType(Note::Type::Step); midi()->addNote(duplicate, false); auto regular = midi()->addNote(Note(48, 192, 70), false); Note off(12, 193, 65); off.setType(Note::Type::Step); auto offGrid = midi()->addNote(off, false); auto outside = midi()->addStepNote(32); int notifications = 0; bool complete = true;
		const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; complete = complete && midi()->stepCount() == 16 && midi()->notes().size() == 5; }, Qt::DirectConnection);
		QVERIFY2(midi()->resizeSteps(16, result, error), qPrintable(error)); QObject::disconnect(connection); QCOMPARE(result.fromCount, 32); QCOMPARE(result.removed, 3); QVERIFY(result.changed); QCOMPARE(notifications, 1); QVERIFY(complete); QCOMPARE(first->selectionIdentity(), identity); QVERIFY(first->selected()); QCOMPARE(first->detuning()->countLinks(), std::size_t{1}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(std::find(midi()->notes().begin(), midi()->notes().end(), regular) != midi()->notes().end()); QVERIFY(std::find(midi()->notes().begin(), midi()->notes().end(), offGrid) != midi()->notes().end()); QVERIFY(std::find(midi()->notes().begin(), midi()->notes().end(), outside) != midi()->notes().end()); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan));
	}
	void growthPublishesOneEmptyExtension()
	{
		seed(); const auto roster = midi()->notes(); int notifications = 0; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; }, Qt::DirectConnection); MidiClip::StepResizeResult result; QString error; QVERIFY(midi()->resizeSteps(4096, result, error, 4096)); QObject::disconnect(connection); QCOMPARE(result.fromCount, 16); QCOMPARE(result.removed, 0); QVERIFY(result.changed); QCOMPARE(midi()->stepCount(), 4096); QCOMPARE(midi()->notes(), roster); QCOMPARE(notifications, 1);
	}
	void nativeNoopDoesNotPublishHistoryOrNotifications()
	{
		seed(); Engine::projectJournal()->setJournalling(true); int notifications = 0; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; }); MidiClip::StepResizeResult result; QString error; QVERIFY(midi()->resizeSteps(16, result, error)); QObject::disconnect(connection); QCOMPARE(result.fromCount, 16); QCOMPARE(result.removed, 0); QVERIFY(!result.changed); QCOMPARE(notifications, 0); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void nativeCountAndGraphUndoRedo_data()
	{ QTest::addColumn<bool>("shrink"); QTest::newRow("grow") << false; QTest::newRow("shrink") << true; }
	void nativeCountAndGraphUndoRedo()
	{
		QFETCH(bool, shrink); seed(); MidiClip::StepResizeResult result; QString error; if (shrink) { QVERIFY(midi()->resizeSteps(32, result, error)); auto last = midi()->addStepNote(20); last->createDetuning(); QVERIFY(midi()->notes()[0]->detuning()->linkToModel(last->detuning().get())); } const int before = midi()->stepCount(); const auto size = midi()->notes().size(); auto journal = Engine::projectJournal(); journal->setJournalling(true); QVERIFY(midi()->resizeSteps(shrink ? 16 : 32, result, error)); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(midi()->stepCount(), before); QCOMPARE(midi()->notes().size(), size); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); if (shrink) { QCOMPARE(midi()->notes()[0]->detuning()->countLinks(), std::size_t{2}); } QVERIFY(!journal->canUndo()); journal->redo(); QCOMPARE(midi()->stepCount(), shrink ? 16 : 32); QCOMPARE(midi()->notes().size(), size - (shrink ? 1 : 0)); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void legacyAddAndRemoveUseCheckedHistory()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); midi()->addSteps(); QCOMPARE(midi()->stepCount(), 32); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(midi()->stepCount(), 16); journal->redo(); QCOMPARE(midi()->stepCount(), 32); midi()->addStepNote(20); journal->clearJournal(); midi()->removeSteps(); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2}); journal->undo(); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->notes().size(), std::size_t{3}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void nonintegralGridClearsExactCellsWithoutAnAffineOffset()
	{
		const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); TimePos::setTicksPerBar(50); auto clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::getSong()))->createClip(0)); setCount(clip, 6); for (const int step : {1, 3, 4, 5, 4}) { clip->addStepNote(step); } Note off(12, 38, 70); off.setType(Note::Type::Step); auto offGrid = clip->addNote(off, false); auto regular = clip->addNote(Note(12, 50, 72), false); MidiClip::StepResizeResult result; QString error; QVERIFY(clip->resizeSteps(3, result, error)); QCOMPARE(result.removed, 4); QCOMPARE(clip->notes().size(), std::size_t{3}); QCOMPARE(clip->notes()[0]->pos().getTicks(), 12); QVERIFY(std::find(clip->notes().begin(), clip->notes().end(), offGrid) != clip->notes().end()); QVERIFY(std::find(clip->notes().begin(), clip->notes().end(), regular) != clip->notes().end());
	}
	void nativeBoundsAndHistoryCoverageRefuseBeforePublication_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("zero-target") << 0; QTest::newRow("negative-target") << 1; QTest::newRow("tool-limit") << 2; QTest::newRow("negative-limit") << 3; QTest::newRow("bad-source-count") << 4; QTest::newRow("target-overflow") << 5; QTest::newRow("source-overflow") << 6; QTest::newRow("zero-meter") << 7; QTest::newRow("negative-meter") << 8; QTest::newRow("invalid-note-end") << 9; QTest::newRow("outside-history-group") << 10;
	}
	void nativeBoundsAndHistoryCoverageRefuseBeforePublication()
	{
		QFETCH(int, failure); seed(); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); int target = 32, limit = 4096;
		if (failure == 0) { target = 0; } if (failure == 1) { target = -1; } if (failure == 2) { target = 4097; } if (failure == 3) { limit = -1; } if (failure == 4 || failure == 6) { midi()->clearNotes(); setCount(midi(), failure == 4 ? -1 : std::numeric_limits<int>::max()); } if (failure == 5) { target = std::numeric_limits<int>::max(); limit = 0; } if (failure == 7) { TimePos::setTicksPerBar(0); } if (failure == 8) { TimePos::setTicksPerBar(-1); } if (failure == 9) { midi()->notes()[0]->setLength(MaxSongLength); midi()->notes()[0]->setPos(1); } if (failure == 10) { auto outside = midi(1)->addStepNote(0); outside->createDetuning(); QVERIFY(midi()->notes()[0]->detuning()->linkToModel(outside->detuning().get())); Engine::projectJournal()->setJournalling(true); }
		const auto roster = midi()->notes(); const int before = midi()->stepCount(); MidiClip::StepResizeResult result; result.changed = true; result.fromCount = 99; QString error; QVERIFY(!midi()->resizeSteps(target, result, error, limit)); QVERIFY(!error.isEmpty()); QCOMPARE(midi()->stepCount(), before); QCOMPARE(midi()->notes(), roster); QCOMPARE(result.fromCount, 0); QCOMPARE(result.removed, 0); QVERIFY(!result.changed); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void ownershipAndCooperativeChangesRefuseAdmission_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("source-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("meter-changed") << 5; QTest::newRow("scalar-changed") << 6;
	}
	void ownershipAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto first = clip->notes()[0]; const QPointer<MidiClip> weak(clip); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); MidiClip::StepResizeResult result; bool success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->resizeSteps(32, result, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->resizeSteps(32, result, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(first); } if (failure == 4) { delete clip; } if (failure == 5) { TimePos::setTicksPerBar(bar + 12); } if (failure == 6) { first->setSelected(!first->selected()); } } return failure == 2; }, false); success = clip->resizeSteps(32, result, error); }
		QVERIFY(!success); QVERIFY(!timer); QVERIFY(!error.isEmpty()); QVERIFY(!result.changed); if (weak) { QCOMPARE(weak->stepCount(), 16); }
	}
	void structuredWholeBarBoundsAndWrongOwnersRefuse_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("missing-track") << 0; QTest::newRow("missing-clip") << 1; QTest::newRow("fractional-count") << 2; QTest::newRow("zero") << 3; QTest::newRow("limit") << 4; QTest::newRow("partial-target-bar") << 5; QTest::newRow("partial-source-bar") << 6; QTest::newRow("sample-track") << 7; QTest::newRow("zero-meter") << 8;
	}
	void structuredWholeBarBoundsAndWrongOwnersRefuse()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); args.insert("count", 32); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); if (failure == 0) { args.remove("track"); } if (failure == 1) { args.remove("clip"); } if (failure == 2) { args.insert("count", 32.5); } if (failure == 3) { args.insert("count", 0); } if (failure == 4) { args.insert("count", 4097); } if (failure == 5) { args.insert("count", 17); } if (failure == 6) { midi()->clearNotes(); setCount(midi(), 17); } if (failure == 7) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 8) { TimePos::setTicksPerBar(0); } const int count = midi()->stepCount(); const auto roster = midi()->notes(); QVERIFY(!StudioCapabilities::execute("step.resize", args).value("ok").toBool()); QCOMPARE(midi()->stepCount(), count); QCOMPARE(midi()->notes(), roster);
	}
	void structuredSongAndPatternReceipts_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void structuredSongAndPatternReceipts()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); args.insert("count", 32); auto response = StudioCapabilities::execute("step.resize", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); auto result = response.value("result").toObject(); QCOMPARE(result.value("fromCount").toInt(), 16); QCOMPARE(result.value("count").toInt(), 32); QCOMPARE(result.value("removed").toInt(), 0); QVERIFY(result.value("changed").toBool()); clip->addStepNote(20); args.insert("count", 16); response = StudioCapabilities::execute("step.resize", args); QVERIFY(response.value("ok").toBool()); QCOMPARE(response.value("result").toObject().value("removed").toInt(), 1); QCOMPARE(clip->notes().size(), std::size_t{2}); QVERIFY(linked(clip->notes()[0], clip->notes()[1]));
	}
	void runtimeReplayHistoryAndReopeningPreserveResize()
	{
		QTemporaryDir directory; QString error; auto args = arguments(); args.insert("count", 32); const auto command = request("resize", "step.resize", args); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->stepCount(), 32); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->stepCount(), 16); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->stepCount(), 32); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->stepCount(), 32); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1]));
	}
	void runtimeRestoresCountRosterAndGraphAfterReturnedFailures_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("survivor-retired") << 1; QTest::newRow("count-changed") << 2; }
	void runtimeRestoresCountRosterAndGraphAfterReturnedFailures()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); midi()->addSteps(); auto last = midi()->addStepNote(20); last->createDetuning(); QVERIFY(midi()->notes()[0]->detuning()->linkToModel(last->detuning().get())); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto connection = QObject::connect(clip, &MidiClip::dataChanged, this, [&] { if (done) { return; } done = true; if (failure == 1) { clip->removeNote(clip->notes()[0]); } else if (failure == 2) { clip->addSteps(); } else { throw std::runtime_error("fixture step resize notification"); } }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); auto args = arguments(); args.insert("count", 16); const auto command = request("reject", "step.resize", args); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->notes().size(), std::size_t{3}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QCOMPARE(midi()->notes()[0]->detuning()->countLinks(), std::size_t{2}); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsResizeAfterStepEdits_data() { protocols(); }
	void scriptedAgentsResizeAfterStepEdits()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Shorten the beat"}, {"steps", QJsonArray{"Enable a tail step", "Shorten the row"}}, {"decisions", QJsonArray{}}}), call("add", "step.set", {{"track", 0}, {"clip", 0}, {"step", 20}, {"enabled", true}}), call("resize", "step.resize", {{"track", 0}, {"clip", 0}, {"count", 16}}, catalog), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native row shortened"}, {"evidence", QJsonArray{"add", "resize"}}})}); }); QVERIFY(runtime.open(error)); seed(); midi()->addSteps(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Enable step 21 then shorten the row to one bar."}})).value("ok").toBool()); for (int index = 0; index < 7; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "resize").value("ok").toBool()); QCOMPARE(record(directory, "resize").value("result").toObject().value("removed").toInt(), 1); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2});
	}
	void scriptedAgentsSkipAReplacedRow_data() { protocols(); }
	void scriptedAgentsSkipAReplacedRow()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("remove", "clip.remove", {{"track", 0}, {"clip", 0}}), call("resize", "step.resize", {{"track", 0}, {"clip", 0}, {"count", 32}}, catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old row"}, {"evidence", QJsonArray{"remove"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed retired row"}, {"evidence", QJsonArray{"remove"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Resize the row after its clip changes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "remove").value("ok").toBool()); QVERIFY(!record(directory, "resize").value("ok").toBool()); QVERIFY(record(directory, "resize").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void scriptedAgentsSkipRetiredNotesAfterResize_data() { protocols(); }
	void scriptedAgentsSkipRetiredNotesAfterResize()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (!inferred) { inferred = true; return decision(protocol, {call("resize", "step.resize", {{"track", 0}, {"clip", 0}, {"count", 16}}, catalog), call("old-note", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 2}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old note"}, {"evidence", QJsonArray{"resize"}}})}); }
			return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed removed tail note"}, {"evidence", QJsonArray{"resize"}}})});
		});
		QVERIFY(runtime.open(error)); seed(); midi()->addSteps(); midi()->addStepNote(20); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Shorten this row and inspect its old tail note."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "resize").value("ok").toBool()); QVERIFY(!record(directory, "old-note").value("ok").toBool()); QVERIFY(record(directory, "old-note").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2});
	}
	void largeRowClearsTailAndDetachesNativeLinks()
	{
		seed(); midi()->clearNotes(); setCount(midi(), 2048); DetuningHelper* anchor = nullptr; for (int index = 0; index < 1050; ++index) { auto note = midi()->addStepNote(index); note->createDetuning(); if (anchor) { QVERIFY(anchor->linkToModel(note->detuning().get())); } else { anchor = note->detuning().get(); } } auto args = arguments(); args.insert("count", 1024); const auto response = StudioCapabilities::execute("step.resize", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("removed").toInt(), 26); QCOMPARE(midi()->notes().size(), std::size_t{1024}); QCOMPARE(anchor->countLinks(), std::size_t{1023});
	}
	void schemaAndDirectCatalogRoutesStayAvailable()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "step.resize") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); QCOMPARE(schema.value("inputSchema").toObject().value("properties").toObject().size(), 4);
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "step_resize"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } auto args = arguments(); args.insert("count", 32); QJsonObject catalog{{"op", "step.resize"}, {"args", args}}; QCOMPARE(StudioAgent::operation("studio_execute", catalog), QString("step.resize")); QCOMPARE(catalog, args);
	}
};
QTEST_GUILESS_MAIN(StudioStepResizeTest)
#include "StudioStepResizeTest.moc"
