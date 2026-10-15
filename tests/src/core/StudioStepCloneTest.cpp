/* Native step cloning, complete control content, grid bounds and agent recovery.
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-step-clone-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioStepCloneTest : public QObject
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

	void nativeCloneRetainsOriginalsAndIndependentCompleteControls()
	{
		seed(); const auto notes = midi()->notes(); notes[0]->setSelected(true); const auto identity = notes[0]->selectionIdentity(); auto helper = notes[0]->detuning().get(); auto curve = helper->automationClip(); auto controller = new MidiController(nullptr); helper->setControllerConnection(new ControllerConnection(controller)); MidiClip::StepCloneResult result; QString error;
		QVERIFY2(midi()->cloneSteps(result, error), qPrintable(error)); QCOMPARE(result.fromCount, 16); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(result.steps.size(), std::size_t{2}); QCOMPARE(result.steps[0], 0); QCOMPARE(result.steps[1], 4); QCOMPARE(result.sources[0], notes[0]); QCOMPARE(notes[0]->selectionIdentity(), identity); QCOMPARE(notes[0]->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QCOMPARE(result.copies[0]->pos().getTicks(), 192); QCOMPARE(result.copies[1]->pos().getTicks(), 240); QCOMPARE(result.copies[0]->type(), Note::Type::Step); QCOMPARE(result.copies[0]->length().getTicks(), 24); QCOMPARE(result.copies[1]->length().getTicks(), 36); QCOMPARE(result.copies[0]->volume(), volume_t{85}); QCOMPARE(result.copies[0]->panning(), panning_t{-20}); QVERIFY(!result.copies[0]->selected()); QVERIFY(notes[0]->selected()); QVERIFY(linked(notes[0], notes[1])); QVERIFY(linked(result.copies[0], result.copies[1])); QVERIFY(!helper->isLinkedToModel(result.copies[0]->detuning().get())); QCOMPARE(result.copies[0]->detuning()->automationClip()->valueAt(12), curve->valueAt(12)); QVERIFY(result.copies[0]->detuning()->controllerConnection()->getController() != controller);
	}
	void firstStepRulePreservesRegularOffGridAndDuplicateNotes()
	{
		seed(); midi()->clearNotes(); Note low(12, 0, 60); low.setType(Note::Type::Step); auto first = midi()->addNote(low, false); Note high(24, 0, 80); high.setType(Note::Type::Step); auto chosen = midi()->addNote(high, false); auto regular = midi()->addNote(Note(48, 24, 70), false); Note offGrid(12, 1, 72); offGrid.setType(Note::Type::Step); auto off = midi()->addNote(offGrid, false); MidiClip::StepCloneResult result; QString error; QVERIFY(midi()->cloneSteps(result, error)); QCOMPARE(result.copies.size(), std::size_t{1}); QCOMPARE(result.sources[0], chosen); QCOMPARE(result.copies[0]->key(), 80); QCOMPARE(result.copies[0]->length().getTicks(), 24); QCOMPARE(midi()->notes().size(), std::size_t{5}); QCOMPARE(first->key(), 60); QCOMPARE(regular->pos().getTicks(), 24); QCOMPARE(off->pos().getTicks(), 1); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan));
	}
	void nativeGridMappingDoesNotAssumeAnAffineOffset()
	{
		const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); TimePos::setTicksPerBar(50); auto track = new InstrumentTrack(Engine::getSong()); auto clip = static_cast<MidiClip*>(track->createClip(0)); setCount(clip, 3); clip->addStepNote(1); clip->addStepNote(2); MidiClip::StepCloneResult result; QString error; QVERIFY2(clip->cloneSteps(result, error), qPrintable(error)); QCOMPARE(result.fromCount, 3); QCOMPARE(clip->stepCount(), 6); QCOMPARE(result.steps[0], 1); QCOMPARE(result.steps[1], 2); QCOMPARE(result.copies[0]->pos().getTicks(), 50); QCOMPARE(result.copies[1]->pos().getTicks(), 62);
	}
	void emptyAndRegularOnlyRowsStillDouble_data()
	{ QTest::addColumn<bool>("regular"); QTest::newRow("empty") << false; QTest::newRow("regular-only") << true; }
	void emptyAndRegularOnlyRowsStillDouble()
	{
		QFETCH(bool, regular); seed(); midi()->clearNotes(); auto note = regular ? midi()->addNote(Note(48, 24, 70), false) : nullptr; auto journal = Engine::projectJournal(); journal->setJournalling(true); MidiClip::StepCloneResult result; QString error; QVERIFY(midi()->cloneSteps(result, error)); QCOMPARE(result.fromCount, 16); QCOMPARE(midi()->stepCount(), 32); QVERIFY(result.sources.empty()); QVERIFY(result.copies.empty()); QVERIFY(result.steps.empty()); QCOMPARE(midi()->notes().size(), static_cast<std::size_t>(regular ? 1 : 0)); if (note) { QCOMPARE(midi()->notes()[0], note); } QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(midi()->stepCount(), 16);
	}
	void nativeBoundsCollisionsAndCoverageRefuseBeforeCommit_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("negative-count") << 0; QTest::newRow("integer-count-overflow") << 1; QTest::newRow("song-span") << 2; QTest::newRow("tool-count-limit") << 3; QTest::newRow("negative-limit") << 4; QTest::newRow("zero-meter") << 5; QTest::newRow("native-product-overflow") << 6; QTest::newRow("destination-step-collision") << 7; QTest::newRow("copied-endpoint") << 8; QTest::newRow("copy-group-outside") << 9; QTest::newRow("clip-history-outside") << 10; }
	void nativeBoundsCollisionsAndCoverageRefuseBeforeCommit()
	{
		QFETCH(int, failure); seed(); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); int limit = 0; if (failure <= 3 || failure == 6) { midi()->clearNotes(); const int count = failure == 0 ? -1 : failure == 1 ? std::numeric_limits<int>::max() : failure == 2 ? 160000 : failure == 3 ? 2056 : 1000; setCount(midi(), count); } if (failure == 3) { limit = 4096; } if (failure == 4) { limit = -1; } if (failure == 5) { TimePos::setTicksPerBar(0); } if (failure == 6) { TimePos::setTicksPerBar(MaxSongLength); } if (failure == 7) { midi()->addStepNote(16); } if (failure == 8) { midi()->notes()[0]->setLength(MaxSongLength); } if (failure == 9) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(midi(1)->mutedModel())); } if (failure == 10) { QVERIFY(midi()->mutedModel()->linkToModel(midi(1)->mutedModel())); Engine::projectJournal()->setJournalling(true); } const int count = midi()->stepCount(); const auto size = midi()->notes().size(); MidiClip::StepCloneResult result; QString error; QVERIFY(!midi()->cloneSteps(result, error, limit)); QVERIFY(!error.isEmpty()); QCOMPARE(midi()->stepCount(), count); QCOMPARE(midi()->notes().size(), size); QVERIFY(result.sources.empty()); QVERIFY(result.copies.empty()); QVERIFY(result.steps.empty());
	}
	void legacyNativeHistoryPreservesCountAndBothGraphs()
	{
		seed(); const QPointer<MidiClip> same(midi()); auto journal = Engine::projectJournal(); journal->setJournalling(true); midi()->cloneSteps(); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3])); journal->undo(); QCOMPARE(same.data(), midi()); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); journal->redo(); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->notes().size(), std::size_t{4}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3])); QVERIFY(!Engine::getSong()->hasErrors());
	}
	void ownershipRecordingAndCooperativeChangesRefuseAdmission_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("source-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("meter-changed") << 5; QTest::newRow("count-changed") << 6; }
	void ownershipRecordingAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto first = clip->notes()[0]; const QPointer<MidiClip> weak(clip); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); MidiClip::StepCloneResult result; bool success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; });
		if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->cloneSteps(result, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->cloneSteps(result, error); }); worker.join(); }
		else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(first); } if (failure == 4) { delete clip; } if (failure == 5) { TimePos::setTicksPerBar(bar + 12); } if (failure == 6) { clip->addSteps(); } } return failure == 2; }, false); success = clip->cloneSteps(result, error); }
		QVERIFY(!success); QVERIFY(!timer); QVERIFY(!error.isEmpty()); QVERIFY(result.copies.empty()); if (weak) { QCOMPARE(weak->stepCount(), failure == 6 ? 32 : 16); }
	}
	void invalidStructuredRequestsRefuseMutation_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("missing-track") << 0; QTest::newRow("missing-clip") << 1; QTest::newRow("container") << 2; QTest::newRow("wrong-type") << 3; QTest::newRow("unknown-count") << 4; QTest::newRow("count-limit") << 5; QTest::newRow("fractional-track") << 6; }
	void invalidStructuredRequestsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); if (failure == 0) { args.remove("track"); } if (failure == 1) { args.remove("clip"); } if (failure == 2) { args.insert("container", "other"); } if (failure == 3) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 4) { args.insert("count", 32); } if (failure == 5) { midi()->clearNotes(); setCount(midi(), 4096); } if (failure == 6) { args.insert("track", .5); } const int count = midi()->stepCount(); const auto size = midi()->notes().size(); QVERIFY(!StudioCapabilities::execute("step.clone", args).value("ok").toBool()); QCOMPARE(midi()->stepCount(), count); QCOMPARE(midi()->notes().size(), size);
	}
	void sourceStepOrderReceiptsWorkInSongAndPattern_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void sourceStepOrderReceiptsWorkInSongAndPattern()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); const auto response = StudioCapabilities::execute("step.clone", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("fromCount").toInt(), 16); QCOMPARE(result.value("count").toInt(), 32); QCOMPARE(result.value("copied").toInt(), 2); const auto receipt = result.value("notes").toArray(); QCOMPARE(receipt[0].toObject().value("sourceStep").toInt(), 0); QCOMPARE(receipt[0].toObject().value("step").toInt(), 16); QCOMPARE(receipt[0].toObject().value("note").toInt(), 2); QCOMPARE(receipt[1].toObject().value("sourceStep").toInt(), 4); QCOMPARE(receipt[1].toObject().value("step").toInt(), 20); QCOMPARE(receipt[1].toObject().value("position").toInt(), 240); QVERIFY(linked(clip->notes()[0], clip->notes()[1])); QVERIFY(linked(clip->notes()[2], clip->notes()[3]));
	}
	void runtimeHistoryReplayAndReopeningKeepStepCountsAndGraphs()
	{
		QTemporaryDir directory; QString error; const auto command = request("clone", "step.clone", arguments()); QJsonObject receipt;
		{ StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); VERIFY_NATIVE_REPLAY(runtime.process(command), receipt); QCOMPARE(midi()->stepCount(), 32); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->notes().size(), std::size_t{4}); }
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); VERIFY_NATIVE_REPLAY(reopened.process(command), receipt); QCOMPARE(midi()->stepCount(), 32); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(linked(midi()->notes()[2], midi()->notes()[3]));
	}
	void runtimeRecoversReturnedNotificationsAndRetirements_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("throw") << 0; QTest::newRow("copy-retired") << 1; QTest::newRow("original-retired") << 2; }
	void runtimeRecoversReturnedNotificationsAndRetirements()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto connection = QObject::connect(clip, &MidiClip::dataChanged, this, [&] { if (done) { return; } done = true; if (failure) { clip->removeNote(clip->notes()[failure == 1 ? 2 : 0]); } else { throw std::runtime_error("fixture step clone notification"); } }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const auto command = request("reject", "step.clone", arguments()); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); VERIFY_NATIVE_REPLAY(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsCreateAStepCloneAndInspectFutureCopy_data() { protocols(); }
	void scriptedAgentsCreateAStepCloneAndInspectFutureCopy()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Repeat the beat"}, {"steps", QJsonArray{"Add a step", "Clone the row", "Inspect a new copy"}}, {"decisions", QJsonArray{}}}), call("add", "step.set", {{"track", 0}, {"clip", 0}, {"step", 1}, {"enabled", true}}), call("clone", "step.clone", arguments(), catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 5}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native beat repeated"}, {"evidence", QJsonArray{"add", "clone", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Add the second step, repeat this row and inspect the copied detuning."}})).value("ok").toBool()); for (int index = 0; index < 8; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "clone").value("ok").toBool()); QCOMPARE(record(directory, "clone").value("result").toObject().value("copied").toInt(), 3); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->notes().size(), std::size_t{6});
	}
	void scriptedAgentsSkipReplacedRowAndReplan_data() { protocols(); }
	void scriptedAgentsSkipReplacedRowAndReplan()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("remove", "clip.remove", {{"track", 0}, {"clip", 0}}), call("clone", "step.clone", arguments(), catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old row"}, {"evidence", QJsonArray{"remove"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed retired row"}, {"evidence", QJsonArray{"remove"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Repeat the row after its clip changes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "remove").value("ok").toBool()); QVERIFY(!record(directory, "clone").value("ok").toBool()); QVERIFY(record(directory, "clone").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void largeNativeRowRetainsIndependentControlGroups()
	{
		seed(); midi()->clearNotes(); setCount(midi(), 2048); for (int index = 0; index < 1050; ++index) { auto note = midi()->addStepNote(index); note->createDetuning(); if (index) { QVERIFY(midi()->notes()[0]->detuning()->linkToModel(note->detuning().get())); } } const auto first = midi()->notes()[0]; const auto identity = first->selectionIdentity(); const auto response = StudioCapabilities::execute("step.clone", arguments()); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto result = response.value("result").toObject(); QCOMPARE(result.value("count").toInt(), 4096); QCOMPARE(result.value("copied").toInt(), 1050); QCOMPARE(result.value("notes").toArray().size(), 1050); QCOMPARE(midi()->notes().size(), std::size_t{2100}); QCOMPARE(first->selectionIdentity(), identity); QCOMPARE(first->detuning()->countLinks(), std::size_t{1049}); QCOMPARE(midi()->notes()[1050]->detuning()->countLinks(), std::size_t{1049}); QVERIFY(!first->detuning()->isLinkedToModel(midi()->notes()[1050]->detuning().get()));
	}
	void schemaAndToolRoutesRetainDeclarationBudget()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "step.clone") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); QCOMPARE(schema.value("inputSchema").toObject().value("properties").toObject().size(), 3);
		for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "step_clone"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } QJsonObject args{{"op", "step.clone"}, {"args", arguments()}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("step.clone")); QCOMPARE(args, arguments());
	}
};
QTEST_GUILESS_MAIN(StudioStepCloneTest)
#include "StudioStepCloneTest.moc"
