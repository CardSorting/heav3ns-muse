/* Native step toggles, prepared insertion/removal, history and agent recovery.
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
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-step-set-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory) { QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id) { for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
void protocols() { QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
}
class StudioStepSetTest : public QObject
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

	void enablingInsertsNativeDefaultsAndPublishesOnce()
	{
		seed(); const auto original = midi()->notes(); int notifications = 0; bool complete = true; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; complete = complete && midi()->stepCount() == 16 && midi()->notes().size() == 3 && midi()->noteAtStep(2); }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); MidiClip::StepSetResult result; QString error; QVERIFY2(midi()->setStep(2, true, result, error), qPrintable(error)); QVERIFY(result.changed); QCOMPARE(result.removed, 0); QCOMPARE(result.note, midi()->noteAtStep(2)); QCOMPARE(result.note->pos().getTicks(), 24); QCOMPARE(result.note->length().getTicks(), DefaultTicksPerBar / 16); QCOMPARE(result.note->key(), DefaultKey); QCOMPARE(result.note->volume(), DefaultVolume); QCOMPARE(result.note->panning(), DefaultPanning); QCOMPARE(result.note->type(), Note::Type::Step); QVERIFY(!result.note->selected()); QVERIFY(!result.note->detuning()); QCOMPARE(notifications, 1); QVERIFY(complete); QCOMPARE(midi()->notes()[0], original[0]); QCOMPARE(midi()->notes()[2], original[1]); QVERIFY(linked(original[0], original[1])); QVERIFY(std::is_sorted(midi()->notes().begin(), midi()->notes().end(), Note::lessThan));
	}
	void enablingExistingCellRetainsFirstNoteAndAllDuplicateContent()
	{
		seed(); auto original = midi()->notes()[0]; original->setSelected(true); const auto identity = original->selectionIdentity(); auto helper = original->detuning().get(); auto curve = helper->automationClip(); Note duplicate(36, 0, 80); duplicate.setType(Note::Type::Step); auto first = midi()->addNote(duplicate, false); const auto roster = midi()->notes(); Engine::projectJournal()->setJournalling(true); MidiClip::StepSetResult result; QString error; QVERIFY(midi()->setStep(0, true, result, error)); QCOMPARE(result.note, first); QVERIFY(!result.changed); QCOMPARE(result.removed, 0); QCOMPARE(midi()->notes(), roster); QCOMPARE(original->selectionIdentity(), identity); QCOMPARE(original->detuning().get(), helper); QCOMPARE(helper->automationClip(), curve); QVERIFY(original->selected()); QVERIFY(linked(original, midi()->noteAtStep(4))); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void disablingClearsAllStepDuplicatesButRetainsRegularAndOffGrid()
	{
		seed(); auto survivor = midi()->notes()[0]; const auto identity = survivor->selectionIdentity(); Note duplicate(36, 48, 80); duplicate.setType(Note::Type::Step); auto extra = midi()->addNote(duplicate, false); extra->createDetuning(); QVERIFY(survivor->detuning()->linkToModel(extra->detuning().get())); auto regular = midi()->addNote(Note(12, 48, 70), false); Note off(12, 49, 72); off.setType(Note::Type::Step); auto offGrid = midi()->addNote(off, false); int notifications = 0; bool complete = true; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; complete = complete && midi()->notes().size() == 3 && !midi()->noteAtStep(4); }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); MidiClip::StepSetResult result; QString error; QVERIFY(midi()->setStep(4, false, result, error)); QVERIFY(result.changed); QCOMPARE(result.removed, 2); QVERIFY(!result.note); QCOMPARE(notifications, 1); QVERIFY(complete); QCOMPARE(survivor->selectionIdentity(), identity); QCOMPARE(survivor->detuning()->countLinks(), std::size_t{0}); QVERIFY(std::find(midi()->notes().begin(), midi()->notes().end(), regular) != midi()->notes().end()); QVERIFY(std::find(midi()->notes().begin(), midi()->notes().end(), offGrid) != midi()->notes().end());
	}
	void nativeNoopsAvoidHistoryAndNotifications_data()
	{ QTest::addColumn<bool>("enabled"); QTest::newRow("already-enabled") << true; QTest::newRow("already-disabled") << false; }
	void nativeNoopsAvoidHistoryAndNotifications()
	{
		QFETCH(bool, enabled); seed(); Engine::projectJournal()->setJournalling(true); int notifications = 0; const auto connection = QObject::connect(midi(), &MidiClip::dataChanged, this, [&] { ++notifications; }); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); MidiClip::StepSetResult result; QString error; QVERIFY(midi()->setStep(enabled ? 0 : 1, enabled, result, error)); QVERIFY(!result.changed); QCOMPARE(result.removed, 0); QCOMPARE(notifications, 0); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void nativeUndoRedoPreservesCountAndCompleteControlGraphs_data()
	{ QTest::addColumn<bool>("enabled"); QTest::newRow("enable") << true; QTest::newRow("disable") << false; }
	void nativeUndoRedoPreservesCountAndCompleteControlGraphs()
	{
		QFETCH(bool, enabled); seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); MidiClip::StepSetResult result; QString error; QVERIFY(midi()->setStep(enabled ? 2 : 0, enabled, result, error)); const auto size = enabled ? std::size_t{3} : std::size_t{1}; QCOMPARE(midi()->notes().size(), size); QVERIFY(journal->canUndo()); journal->undo(); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(!journal->canUndo()); journal->redo(); QCOMPARE(midi()->notes().size(), size); QCOMPARE(midi()->stepCount(), 16); if (enabled) { QVERIFY(linked(midi()->notes()[0], midi()->notes()[2])); } else { QCOMPARE(midi()->notes()[0]->detuning()->countLinks(), std::size_t{0}); }
	}
	void legacyToggleDelegatesToCheckedHistory()
	{
		seed(); auto journal = Engine::projectJournal(); journal->setJournalling(true); midi()->setStep(2, true); QCOMPARE(midi()->notes().size(), std::size_t{3}); journal->undo(); QCOMPARE(midi()->notes().size(), std::size_t{2}); journal->redo(); midi()->setStep(2, false); QCOMPARE(midi()->notes().size(), std::size_t{2}); journal->undo(); QCOMPARE(midi()->notes().size(), std::size_t{3});
	}
	void nonintegralGridUsesExactNativeCellPosition()
	{
		const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); TimePos::setTicksPerBar(50); auto clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::getSong()))->createClip(0)); MidiClip::StepSetResult result; QString error; QVERIFY(clip->setStep(3, true, result, error)); QCOMPARE(result.note->pos().getTicks(), 37); QCOMPARE(clip->stepCount(), 4); QVERIFY(clip->setStep(3, false, result, error)); QCOMPARE(result.removed, 1); QVERIFY(clip->notes().empty());
	}
	void nativeBoundsAndHistoryCoverageRefuseBeforeWriting_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("negative-step") << 0; QTest::newRow("step-past-end") << 1; QTest::newRow("maximum-index") << 2; QTest::newRow("bad-source-count") << 3; QTest::newRow("zero-meter") << 4; QTest::newRow("negative-meter") << 5; QTest::newRow("tool-row-limit") << 6; QTest::newRow("negative-limit") << 7; QTest::newRow("native-product-overflow") << 8; QTest::newRow("invalid-note-end") << 9; QTest::newRow("outside-history-group") << 10; QTest::newRow("song-span") << 11;
	}
	void nativeBoundsAndHistoryCoverageRefuseBeforeWriting()
	{
		QFETCH(int, failure); seed(); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); int step = 2, limit = 4096; if (failure == 0) { step = -1; } if (failure == 1) { step = 16; } if (failure == 2) { step = std::numeric_limits<int>::max(); } if (failure == 3 || failure == 6 || failure == 8 || failure == 11) { midi()->clearNotes(); setCount(midi(), failure == 3 ? -1 : failure == 6 ? 4097 : failure == 8 ? std::numeric_limits<int>::max() : 160000); } if (failure == 4) { TimePos::setTicksPerBar(0); } if (failure == 5) { TimePos::setTicksPerBar(-1); } if (failure == 7) { limit = -1; } if (failure == 8 || failure == 11) { limit = 0; } if (failure == 9) { midi()->notes()[0]->setLength(MaxSongLength); midi()->notes()[0]->setPos(1); } if (failure == 10) { auto outside = midi(1)->addStepNote(0); outside->createDetuning(); QVERIFY(midi()->notes()[0]->detuning()->linkToModel(outside->detuning().get())); Engine::projectJournal()->setJournalling(true); } const auto roster = midi()->notes(); const int count = midi()->stepCount(); MidiClip::StepSetResult result; result.note = roster.empty() ? nullptr : roster[0]; result.changed = true; result.removed = 77; QString error; QVERIFY(!midi()->setStep(step, true, result, error, limit)); QVERIFY(!error.isEmpty()); QCOMPARE(midi()->notes(), roster); QCOMPARE(midi()->stepCount(), count); QVERIFY(!result.note); QCOMPARE(result.removed, 0); QVERIFY(!result.changed); QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void ownershipAndCooperativeChangesRefuseAdmission_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("recording") << 0; QTest::newRow("foreign-thread") << 1; QTest::newRow("cancel") << 2; QTest::newRow("source-retired") << 3; QTest::newRow("clip-retired") << 4; QTest::newRow("meter-changed") << 5; QTest::newRow("scalar-changed") << 6; QTest::newRow("count-changed") << 7;
	}
	void ownershipAndCooperativeChangesRefuseAdmission()
	{
		QFETCH(int, failure); seed(); auto clip = midi(); const auto first = clip->notes()[0]; const QPointer<MidiClip> weak(clip); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); MidiClip::StepSetResult result; bool success = true, invoked = false, timer = false; QString error; int capture; QObject owner; QTimer::singleShot(0, &owner, [&] { timer = true; }); if (failure == 0) { QVERIFY(clip->getTrack()->beginNoteRecording(&capture)); success = clip->setStep(2, true, result, error); clip->getTrack()->endNoteRecording(&capture); } else if (failure == 1) { std::thread worker([&] { success = clip->setStep(2, true, result, error); }); worker.join(); } else { FileResourceIO::Scope scope([&] { if (!invoked) { invoked = true; if (failure == 3) { clip->removeNote(first); } if (failure == 4) { delete clip; } if (failure == 5) { TimePos::setTicksPerBar(bar + 12); } if (failure == 6) { first->setSelected(!first->selected()); } if (failure == 7) { clip->addSteps(); } } return failure == 2; }, false); success = clip->setStep(2, true, result, error); } QVERIFY(!success); QVERIFY(!timer); QVERIFY(!result.note); QVERIFY(!result.changed); QVERIFY(!error.isEmpty()); if (weak) { QCOMPARE(weak->stepCount(), failure == 7 ? 32 : 16); QVERIFY(!weak->noteAtStep(2)); }
	}
	void malformedStructuredArgumentsRefuseMutation_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("missing-track") << 0; QTest::newRow("missing-clip") << 1; QTest::newRow("fractional-step") << 2; QTest::newRow("negative-step") << 3; QTest::newRow("past-end") << 4; QTest::newRow("non-bool-enabled") << 5; QTest::newRow("unknown-field") << 6; QTest::newRow("sample-track") << 7; QTest::newRow("zero-meter") << 8;
	}
	void malformedStructuredArgumentsRefuseMutation()
	{
		QFETCH(int, failure); seed(); auto args = arguments(); args.insert("step", 2); args.insert("enabled", true); const int bar = TimePos::ticksPerBar(); const auto reset = qScopeGuard([&] { TimePos::setTicksPerBar(bar); }); if (failure == 0) { args.remove("track"); } if (failure == 1) { args.remove("clip"); } if (failure == 2) { args.insert("step", 2.5); } if (failure == 3) { args.insert("step", -1); } if (failure == 4) { args.insert("step", 16); } if (failure == 5) { args.insert("enabled", 1); } if (failure == 6) { args.insert("extra", true); } if (failure == 7) { (new SampleTrack(Engine::getSong()))->createClip(0); args.insert("track", 2); } if (failure == 8) { TimePos::setTicksPerBar(0); } const auto roster = midi()->notes(); QVERIFY(!StudioCapabilities::execute("step.set", args).value("ok").toBool()); QCOMPARE(midi()->notes(), roster); QCOMPARE(midi()->stepCount(), 16);
	}
	void songAndPatternReceiptsExposeCurrentEnabledNote_data()
	{ QTest::addColumn<bool>("pattern"); QTest::newRow("song") << false; QTest::newRow("pattern") << true; }
	void songAndPatternReceiptsExposeCurrentEnabledNote()
	{
		QFETCH(bool, pattern); MidiClip* clip; if (pattern) { new PatternTrack(Engine::getSong()); clip = static_cast<MidiClip*>((new InstrumentTrack(Engine::patternStore()))->getClip(0)); populate(clip); } else { seed(); clip = midi(); } auto args = arguments(); args.insert("container", pattern ? "pattern" : "song"); args.insert("step", 2); args.insert("enabled", true); auto response = StudioCapabilities::execute("step.set", args); QVERIFY(response.value("ok").toBool()); auto result = response.value("result").toObject(); QVERIFY(result.value("changed").toBool()); QCOMPARE(result.value("removed").toInt(), 0); QCOMPARE(result.value("note").toInt(), 1); QCOMPARE(result.value("position").toInt(), 24); response = StudioCapabilities::execute("step.set", args); QVERIFY(response.value("ok").toBool()); QVERIFY(!response.value("result").toObject().value("changed").toBool()); args.insert("enabled", false); response = StudioCapabilities::execute("step.set", args); QVERIFY(response.value("ok").toBool()); result = response.value("result").toObject(); QCOMPARE(result.value("removed").toInt(), 1); QVERIFY(!result.contains("note")); QVERIFY(!result.contains("position")); QCOMPARE(clip->notes().size(), std::size_t{2});
	}
	void runtimeReplayHistoryAndReopeningPreserveToggle_data()
	{ QTest::addColumn<bool>("enabled"); QTest::newRow("enable") << true; QTest::newRow("disable") << false; }
	void runtimeReplayHistoryAndReopeningPreserveToggle()
	{
		QFETCH(bool, enabled); QTemporaryDir directory; QString error; auto args = arguments(); args.insert("step", enabled ? 2 : 0); args.insert("enabled", enabled); const auto command = request("toggle", "step.set", args); QJsonObject receipt; const auto size = enabled ? std::size_t{3} : std::size_t{1}; { StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); receipt = runtime.process(command); QVERIFY(receipt.value("ok").toBool()); QCOMPARE(runtime.process(command), receipt); QCOMPARE(midi()->notes().size(), size); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(midi()->notes().size(), size); } StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(reopened.process(command), receipt); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), size);
	}
	void runtimeRecoversReturnedInsertionRemovalAndRetirementFailures_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("enable-throw") << 0; QTest::newRow("disable-throw") << 1; QTest::newRow("new-note-retired") << 2; QTest::newRow("survivor-retired") << 3; }
	void runtimeRecoversReturnedInsertionRemovalAndRetirementFailures()
	{
		QFETCH(int, failure); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); auto clip = midi(); bool done = false; const auto connection = QObject::connect(clip, &MidiClip::dataChanged, this, [&] { if (done) { return; } done = true; if (failure < 2) { throw std::runtime_error("fixture step toggle notification"); } clip->removeNote(clip->notes()[failure == 2 ? 1 : 0]); }, Qt::DirectConnection); const auto cleanup = qScopeGuard([&] { QObject::disconnect(connection); }); const bool enabled = failure == 0 || failure == 2; auto args = arguments(); args.insert("step", enabled ? 2 : 0); args.insert("enabled", enabled); const auto command = request("reject", "step.set", args); const auto response = runtime.process(command); QVERIFY(!response.value("ok").toBool()); QVERIFY(done); QCOMPARE(midi()->stepCount(), 16); QCOMPARE(midi()->notes().size(), std::size_t{2}); QVERIFY(linked(midi()->notes()[0], midi()->notes()[1])); QCOMPARE(runtime.process(command), response); QVERIFY(!runtime.stopping());
	}
	void scriptedAgentsEnableNewCellsAfterEarlierResize_data() { protocols(); }
	void scriptedAgentsEnableNewCellsAfterEarlierResize()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Extend the beat"}, {"steps", QJsonArray{"Grow the row", "Enable a new cell", "Inspect its note"}}}), call("grow", "step.resize", {{"track", 0}, {"clip", 0}, {"count", 32}}), call("enable", "step.set", {{"track", 0}, {"clip", 0}, {"step", 20}, {"enabled", true}}, catalog), call("inspect", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 2}}), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native beat extended"}, {"evidence", QJsonArray{"grow", "enable", "inspect"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Extend this row and enable step 21, then inspect its new note."}})).value("ok").toBool()); for (int index = 0; index < 8; ++index) { runtime.advanceAgent(); } QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "enable").value("ok").toBool()); QCOMPARE(record(directory, "enable").value("result").toObject().value("position").toInt(), 240); QVERIFY(record(directory, "inspect").value("ok").toBool()); QCOMPARE(midi()->stepCount(), 32); QCOMPARE(midi()->notes().size(), std::size_t{3});
	}
	void scriptedAgentsSkipReplacedRows_data() { protocols(); }
	void scriptedAgentsSkipReplacedRows()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("remove", "clip.remove", {{"track", 0}, {"clip", 0}}), call("enable", "step.set", {{"track", 0}, {"clip", 0}, {"step", 2}, {"enabled", true}}, catalog), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old row"}, {"evidence", QJsonArray{"remove"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed retired row"}, {"evidence", QJsonArray{"remove"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Toggle a step after its row changes."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "remove").value("ok").toBool()); QVERIFY(!record(directory, "enable").value("ok").toBool()); QVERIFY(record(directory, "enable").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete"));
	}
	void scriptedAgentsSkipNotesRetiredByDisable_data() { protocols(); }
	void scriptedAgentsSkipNotesRetiredByDisable()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error; bool inferred = false; StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) { if (!inferred) { inferred = true; return decision(protocol, {call("disable", "step.set", {{"track", 0}, {"clip", 0}, {"step", 4}, {"enabled", false}}, catalog), call("old-note", "note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 1}}), call("old-finish", "agent.finish", {{"status", "complete"}, {"summary", "Old note"}, {"evidence", QJsonArray{"disable"}}})}); } return decision(protocol, {call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Observed removed note"}, {"evidence", QJsonArray{"disable"}}})}); }); QVERIFY(runtime.open(error)); seed(); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Disable step five then inspect its old note."}})).value("ok").toBool()); for (int index = 0; index < 6; ++index) { runtime.advanceAgent(); } QVERIFY(record(directory, "disable").value("ok").toBool()); QVERIFY(!record(directory, "old-note").value("ok").toBool()); QVERIFY(record(directory, "old-note").value("error").toString().contains("changed a queued native selection")); QCOMPARE(job(directory).value("status").toString(), QString("complete")); QCOMPARE(midi()->notes().size(), std::size_t{1});
	}
	void largeDuplicateCellClearsEveryNoteAndDetachesLinks()
	{
		seed(); midi()->clearNotes(); auto survivor = midi()->addStepNote(0); survivor->createDetuning(); const auto anchor = survivor->detuning().get(); for (int index = 0; index < 1050; ++index) { Note source(12, 48, 60 + index % 2); source.setType(Note::Type::Step); auto note = midi()->addNote(source, false); note->createDetuning(); QVERIFY(anchor->linkToModel(note->detuning().get())); } auto args = arguments(); args.insert("step", 4); args.insert("enabled", false); const auto response = StudioCapabilities::execute("step.set", args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); QCOMPARE(response.value("result").toObject().value("removed").toInt(), 1050); QCOMPARE(midi()->notes().size(), std::size_t{1}); QCOMPARE(midi()->notes()[0], survivor); QCOMPARE(anchor->countLinks(), std::size_t{0});
	}
	void schemaAndDirectCatalogRoutesStayAvailable()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "step.set") { schema = value.toObject(); } } QVERIFY(schema.value("mutatesProject").toBool()); QCOMPARE(schema.value("inputSchema").toObject().value("properties").toObject().size(), 5); for (const auto protocol : {QString("chat"), QString("responses")}) { const auto tools = StudioAgent::tools(protocol); QVERIFY(tools.size() <= StudioAgent::ToolDeclarationLimit); bool direct = false, catalog = false; for (const auto& value : tools) { const auto tool = protocol == "chat" ? value.toObject().value("function").toObject() : value.toObject(); direct = direct || tool.value("name") == "step_set"; catalog = catalog || tool.value("name") == "studio_execute"; } QVERIFY(direct); QVERIFY(catalog); } auto args = arguments(); args.insert("step", 2); args.insert("enabled", true); QJsonObject catalog{{"op", "step.set"}, {"args", args}}; QCOMPARE(StudioAgent::operation("studio_execute", catalog), QString("step.set")); QCOMPARE(catalog, args);
	}
};
QTEST_GUILESS_MAIN(StudioStepSetTest)
#include "StudioStepSetTest.moc"
