/* Native agent selection duplication, receiver identities and recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDataStream>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QScopeGuard>
#include <memory>
#include <vector>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
Track::Type kind(int index)
{ return index == 0 ? Track::Type::Instrument : index == 1 ? Track::Type::Sample : index == 2 ? Track::Type::Automation : Track::Type::Pattern; }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QJsonArray items()
{ return {QJsonObject{{"track", 0}, {"clip", 0}, {"targetTrack", 1}, {"position", 384}}, QJsonObject{{"track", 0}, {"clip", 1}, {"targetTrack", 1}, {"position", 576}}}; }
QList<Clip*> seed(int type = 0)
{
	auto source = Track::create(kind(type), Engine::getSong()); auto first = source->createClip(0), second = source->createClip(192);
	Track::create(kind(type), Engine::getSong()); if (!first->mutedModel()->linkToModel(second->soloModel())) { return {}; } first->setMuted(true); return {first, second};
}
QJsonObject call(const QString& id, const QString& op, const QJsonObject& args, bool catalog = false)
{
	if (catalog) { return {{"id", id}, {"name", "studio_execute"}, {"args", QJsonObject{{"op", op}, {"args", args}}}}; }
	auto name = op; name.replace('.', '_'); return {{"id", id}, {"name", name}, {"args", args}};
}
QJsonObject decision(const QString& protocol, const QJsonArray& calls)
{
	QJsonArray output;
	for (const auto& value : calls)
	{
		const auto item = value.toObject(); const auto args = QString::fromUtf8(QJsonDocument(item.value("args").toObject()).toJson(QJsonDocument::Compact));
		if (protocol == "responses") { output.append(QJsonObject{{"type", "function_call"}, {"call_id", item.value("id")}, {"name", item.value("name")}, {"arguments", args}}); }
		else { output.append(QJsonObject{{"id", item.value("id")}, {"type", "function"}, {"function", QJsonObject{{"name", item.value("name")}, {"arguments", args}}}}); }
	}
	if (protocol == "responses") { return {{"status", "completed"}, {"output", output}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", output}}}}}}};
}
QJsonObject provider(const QString& protocol) { return {{"role", "reasoning"}, {"configuration", QJsonObject{{"endpoint", "http://127.0.0.1:1/fixture"}, {"model", "native-selection-fixture"}, {"protocol", protocol}}}}; }
QJsonObject job(const QTemporaryDir& directory)
{ QFile file(directory.filePath("session.json")); if (!file.open(QIODevice::ReadOnly)) { return {}; } return QJsonDocument::fromJson(file.readAll()).object().value("agent").toObject().value("job").toObject(); }
QJsonObject record(const QTemporaryDir& directory, const QString& id)
{ for (const auto& value : job(directory).value("records").toArray()) { const auto item = value.toObject(); if (item.value("call").toObject().value("id") == id) { return item.value("response").toObject(); } } return {}; }
bool wave(const QString& path)
{
	QByteArray data; QDataStream stream(&data, QIODevice::WriteOnly); stream.setByteOrder(QDataStream::LittleEndian);
	stream.writeRawData("RIFF", 4); stream << quint32(36 + 512 * 4); stream.writeRawData("WAVEfmt ", 8);
	stream << quint32(16) << quint16(1) << quint16(2) << quint32(24000) << quint32(96000) << quint16(4) << quint16(16);
	stream.writeRawData("data", 4); stream << quint32(512 * 4); for (int index = 0; index < 512; ++index) { stream << qint16(index * 8) << qint16(-index * 8); }
	QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
}

class StudioClipSelectionCopyTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void nativeSelectionCopiesRetainContentAndIndependentGroups_data()
	{ QTest::addColumn<int>("type"); QTest::newRow("midi") << 0; QTest::newRow("sample") << 1; QTest::newRow("automation") << 2; QTest::newRow("pattern-reference") << 3; }
	void nativeSelectionCopiesRetainContentAndIndependentGroups()
	{
		QFETCH(int, type); auto selected = seed(type); QCOMPARE(selected.size(), 2); const auto id = selected.first()->mutedModel()->id();
		if (auto midi = dynamic_cast<MidiClip*>(selected.first())) { midi->clearNotes(); midi->addNote(Note(48, 0, 63), false); }
		if (auto sample = dynamic_cast<SampleClip*>(selected.first())) { sample->setSampleBuffer(std::make_shared<SampleBuffer>(std::vector<SampleFrame>(512), 24000)); sample->sample().setAmplification(.375f); }
		if (auto curve = dynamic_cast<AutomationClip*>(selected.first())) { curve->putValue(0, 25, false); curve->putValue(96, 70, false); }
		const auto result = StudioCapabilities::execute("clip.selection.copy", {{"items", items()}}); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		const auto receipts = result.value("result").toObject().value("clips").toArray(); QCOMPARE(receipts.size(), 2); QCOMPARE(receipts.first().toObject().value("clip").toInt(), 0); QCOMPARE(receipts.last().toObject().value("clip").toInt(), 1);
		auto target = Engine::getSong()->tracks().at(1); auto first = target->getClip(0), second = target->getClip(1);
		QVERIFY(first->mutedModel()->isLinkedToModel(second->soloModel())); QVERIFY(!first->mutedModel()->isLinkedToModel(selected.first()->mutedModel())); QVERIFY(first->mutedModel()->id() != id);
		QCOMPARE(selected.first()->mutedModel()->id(), id); first->setMuted(false); QVERIFY(selected.first()->isMuted());
		if (auto midi = dynamic_cast<MidiClip*>(first)) { QCOMPARE(midi->notes().size(), std::size_t{1}); QCOMPARE(midi->notes().front()->key(), 63); }
		if (auto sample = dynamic_cast<SampleClip*>(first)) { QCOMPARE(sample->sample().buffer().get(), static_cast<SampleClip*>(selected.first())->sample().buffer().get()); QCOMPARE(sample->sample().amplification(), .375f); }
		if (auto curve = dynamic_cast<AutomationClip*>(first)) { curve->putValue(96, 90, false); QCOMPARE(static_cast<AutomationClip*>(selected.first())->valueAt(96), 70.f); }
		if (auto pattern = dynamic_cast<PatternClip*>(first)) { QCOMPARE(pattern->patternIndex(), 1); }
	}

	void patternCellsReplaceTogetherAfterGraphRestoration()
	{
		for (int index = 0; index < 4; ++index) { Track::create(Track::Type::Pattern, Engine::getSong()); }
		auto row = Track::create(Track::Type::Instrument, Engine::patternStore()); auto first = row->getClip(0), second = row->getClip(1);
		QVERIFY(first->mutedModel()->linkToModel(second->soloModel())); first->setMuted(true); const QPointer<Clip> oldA(row->getClip(2)), oldB(row->getClip(3));
		QJsonArray entries{QJsonObject{{"container", "pattern"}, {"track", 0}, {"clip", 0}, {"targetClip", 2}, {"replace", true}},
			QJsonObject{{"container", "pattern"}, {"track", 0}, {"clip", 1}, {"targetClip", 3}, {"replace", true}}};
		const auto result = StudioCapabilities::execute("clip.selection.copy", {{"items", entries}}); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		QVERIFY(!oldA); QVERIFY(!oldB); QCOMPARE(row->numOfClips(), 4); QCOMPARE(row->getClip(0), first); QCOMPARE(row->getClip(1), second);
		QVERIFY(row->getClip(2)->mutedModel()->isLinkedToModel(row->getClip(3)->soloModel())); QVERIFY(!row->getClip(2)->mutedModel()->isLinkedToModel(first->mutedModel()));
		QCOMPARE(row->getClip(2)->startPosition().getTicks(), TimePos(2, 0).getTicks()); QCOMPARE(row->getClip(3)->startPosition().getTicks(), TimePos(3, 0).getTicks());
	}

	void mixedContainerSourcesRetainTheirNativeOwnersAndMidiControllers()
	{
		auto song = Engine::getSong(); auto track = new InstrumentTrack(song); auto first = track->createClip(0); Track::create(Track::Type::Pattern, song);
		auto row = Track::create(Track::Type::Instrument, Engine::patternStore()); auto second = row->getClip(0); auto target = new InstrumentTrack(song);
		QVERIFY(first->mutedModel()->linkToModel(second->soloModel())); auto midi = new MidiController(nullptr); midi->midiPort()->setInputController(74); first->mutedModel()->setControllerConnection(new ControllerConnection(midi));
		const auto result = StudioCapabilities::execute("clip.selection.copy", {{"items", QJsonArray{QJsonObject{{"track", 0}, {"clip", 0}, {"targetTrack", 2}, {"position", 384}},
			QJsonObject{{"container", "pattern"}, {"track", 0}, {"clip", 0}, {"targetContainer", "song"}, {"targetTrack", 2}, {"position", 576}}}}});
		QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString())); QCOMPARE(target->numOfClips(), 2); QVERIFY(target->getClip(0)->mutedModel()->isLinkedToModel(target->getClip(1)->soloModel()));
		const auto copied = dynamic_cast<MidiController*>(target->getClip(0)->mutedModel()->controllerConnection()->getController()); QVERIFY(copied); QVERIFY(copied != midi); QCOMPARE(copied->midiPort()->inputController(), 74);
		QCOMPARE(static_cast<MidiClip*>(target->getClip(1))->instrumentTrack(), target); QCOMPARE(row->numOfClips(), 1);
	}

	void invalidItemsLeaveNativeSourceAndDestinationClipsIntact_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("not-array") << 0; QTest::newRow("empty") << 1; QTest::newRow("not-object") << 2;
		QTest::newRow("unknown-field") << 3; QTest::newRow("missing-track") << 4; QTest::newRow("fractional-track") << 5; QTest::newRow("container") << 6;
		QTest::newRow("target-container-without-track") << 7; QTest::newRow("type") << 8; QTest::newRow("missing-position") << 9; QTest::newRow("end-overflow") << 10;
		QTest::newRow("pattern-no-replace") << 11; QTest::newRow("pattern-replace-false") << 12; QTest::newRow("pattern-position") << 13; QTest::newRow("source-replacement") << 14;
		QTest::newRow("duplicate-source") << 15; QTest::newRow("duplicate-destination-cell") << 16; QTest::newRow("missing-cell") << 17; QTest::newRow("outside-group") << 18; QTest::newRow("too-many") << 19;
	}
	void invalidItemsLeaveNativeSourceAndDestinationClipsIntact()
	{
		QFETCH(int, failure); auto selected = seed(); QCOMPARE(selected.size(), 2); Track::create(Track::Type::Sample, Engine::getSong()); Track::create(Track::Type::Pattern, Engine::getSong());
		auto row = Track::create(Track::Type::Instrument, Engine::patternStore()); auto cell = row->getClip(0); auto entries = items(); auto first = entries.first().toObject();
		if (failure == 3) { first.insert("unknown", 1); } if (failure == 4) { first.remove("track"); } if (failure == 5) { first.insert("track", .5); } if (failure == 6) { first.insert("container", "bad"); }
		if (failure == 7) { first.remove("targetTrack"); first.insert("targetContainer", "song"); } if (failure == 8) { first.insert("targetTrack", 2); } if (failure == 9) { first.remove("position"); } if (failure == 10) { first.insert("position", MaxSongLength); }
		if (failure >= 11 && failure <= 13) { first.insert("targetContainer", "pattern"); first.insert("targetTrack", 0); first.insert("targetClip", 0); if (failure != 13) { first.remove("position"); } if (failure != 11) { first.insert("replace", failure != 12); } }
		if (failure == 14) { entries = QJsonArray{QJsonObject{{"container", "pattern"}, {"track", 0}, {"clip", 0}, {"targetClip", 0}, {"replace", true}}}; }
		else if (failure == 16) { first = QJsonObject{{"track", 0}, {"clip", 0}, {"targetContainer", "pattern"}, {"targetTrack", 0}, {"targetClip", 0}, {"replace", true}}; auto last = first; last.insert("clip", 1); entries[1] = last; entries[0] = first; }
		else { entries[0] = first; }
		if (failure == 15) { entries[1] = entries[0]; } if (failure == 17) { first.insert("targetContainer", "pattern"); first.insert("targetTrack", 0); first.insert("targetClip", 9); first.insert("replace", true); first.remove("position"); entries[0] = first; }
		if (failure == 18) { auto outside = selected.first()->getTrack()->createClip(768); QVERIFY(selected.first()->mutedModel()->linkToModel(outside->soloModel())); }
		QJsonObject args{{"items", entries}}; if (failure == 0) { args.insert("items", "bad"); } if (failure == 1) { args.insert("items", QJsonArray{}); } if (failure == 2) { args.insert("items", QJsonArray{1}); }
		if (failure == 19) { QJsonArray oversized; for (int index = 0; index < 4097; ++index) { oversized.append(first); } args.insert("items", oversized); }
		const auto result = StudioCapabilities::execute("clip.selection.copy", args); QVERIFY(!result.value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().at(1)->numOfClips(), 0); QCOMPARE(row->numOfClips(), 1); QCOMPARE(row->getClip(0), cell);
		QVERIFY(selected.first()->mutedModel()->isLinkedToModel(selected.last()->soloModel()));
	}

	void acceptedResidentAudioCopiesAfterOriginalFileRemoval()
	{
		QTemporaryDir directory; const auto path = directory.filePath("accepted.wav"); QVERIFY(wave(path)); auto selected = seed(1); QCOMPARE(selected.size(), 2);
		auto first = static_cast<SampleClip*>(selected.first()), second = static_cast<SampleClip*>(selected.last()); first->setSampleFile(path); second->setSampleFile(path);
		const auto buffer = first->sample().buffer(); QVERIFY(buffer->size() > 0); QVERIFY(first->setSampleWindow(5, 200)); first->sample().setAmplification(.25f); QVERIFY(QFile::remove(path));
		const auto result = StudioCapabilities::execute("clip.selection.copy", {{"items", items()}}); QVERIFY2(result.value("ok").toBool(), qPrintable(result.value("error").toString()));
		auto copied = static_cast<SampleClip*>(Engine::getSong()->tracks().at(1)->getClip(0)); QCOMPARE(copied->sample().buffer().get(), buffer.get()); QCOMPARE(copied->sample().startFrame(), 5); QCOMPARE(copied->sample().endFrame(), 200); QCOMPARE(copied->sample().amplification(), .25f);
		QCOMPARE(copied->sample().data()[37][0], buffer->data()[37][0]); QCOMPARE(copied->sampleFile(), first->sampleFile()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void selectionsSurviveHistoryReopeningAndRefusalRollback()
	{
		QTemporaryDir directory; QString error; const auto copy = request("copy", "clip.selection.copy", {{"items", items()}}); QJsonObject receipt;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); QCOMPARE(seed().size(), 2); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			receipt = runtime.process(copy); QVERIFY2(receipt.value("ok").toBool(), qPrintable(receipt.value("error").toString())); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().at(1)->numOfClips(), 0);
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().at(1)->numOfClips(), 2);
			auto source = Engine::getSong()->tracks().at(0); auto outside = source->createClip(768); QVERIFY(source->getClip(0)->mutedModel()->linkToModel(outside->soloModel()));
			QVERIFY(!runtime.process(request("bad", "clip.selection.copy", {{"items", items()}})).value("ok").toBool()); source = Engine::getSong()->tracks().at(0);
			QCOMPARE(source->numOfClips(), 3); QVERIFY(source->getClip(0)->mutedModel()->isLinkedToModel(source->getClip(2)->soloModel())); QVERIFY(!runtime.stopping()); QVERIFY(!Engine::getSong()->hasErrors());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QCOMPARE(reopened.process(copy), receipt); auto target = Engine::getSong()->tracks().at(1); QCOMPARE(target->numOfClips(), 2); QVERIFY(target->getClip(0)->mutedModel()->isLinkedToModel(target->getClip(1)->soloModel()));
	}

	void nestedSelectorsFollowEverySourceAndDestination_data()
	{ QTest::addColumn<int>("change"); QTest::newRow("source-track") << 0; QTest::newRow("source-clip") << 1; QTest::newRow("destination-track") << 2; QTest::newRow("destination-cell") << 3; }
	void nestedSelectorsFollowEverySourceAndDestination()
	{
		QFETCH(int, change); QCOMPARE(seed().size(), 2); auto entries = items();
		if (change == 3) { Track::create(Track::Type::Pattern, Engine::getSong()); auto row = Track::create(Track::Type::Instrument, Engine::patternStore()); entries = QJsonArray{QJsonObject{{"track", 0}, {"clip", 0}, {"targetContainer", "pattern"}, {"targetTrack", 0}, {"targetClip", 0}, {"replace", true}}}; QVERIFY(row->getClip(0)); }
		const QJsonObject args{{"items", entries}}; const auto before = StudioCapabilities::selection("clip.selection.copy", args);
		if (change == 0) { QVERIFY(StudioCapabilities::execute("track.edit", {{"track", 0}, {"to", 1}}).value("ok").toBool()); }
		if (change == 1) { delete Engine::getSong()->tracks().at(0)->getClip(0); }
		if (change == 2) { delete Engine::getSong()->tracks().at(1); }
		if (change == 3) { auto row = Engine::patternStore()->tracks().at(0); delete row->getClip(0); row->createClip(0); }
		QVERIFY(before.changed(StudioCapabilities::selection("clip.selection.copy", args)));
	}

	void scriptedNativeAgentPlansAndCopiesSelections_data()
	{ QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::newRow("chat-direct") << QString("chat") << false; QTest::newRow("chat-catalog") << QString("chat") << true; QTest::newRow("responses-direct") << QString("responses") << false; QTest::newRow("responses-catalog") << QString("responses") << true; }
	void scriptedNativeAgentPlansAndCopiesSelections()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QTemporaryDir directory; QString error;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			return decision(protocol, {call("plan", "agent.plan.set", {{"goal", "Duplicate the linked arrangement"}, {"steps", QJsonArray{"Copy both clips together"}}, {"decisions", QJsonArray{"Retain native control relationships"}}}),
				call("copy", "clip.selection.copy", {{"items", items()}}, catalog), call("finish", "agent.finish", {{"status", "complete"}, {"summary", "Native selection copied"}, {"evidence", QJsonArray{"copy"}}})});
		});
		QVERIFY(runtime.open(error)); QCOMPARE(seed().size(), 2); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Copy the linked clips together."}})).value("ok").toBool());
		for (int index = 0; index < 4; ++index) { runtime.advanceAgent(); }
		QCOMPARE(job(directory).value("status").toString(), QString("complete")); QVERIFY(record(directory, "copy").value("ok").toBool()); auto target = Engine::getSong()->tracks().at(1); QCOMPARE(target->numOfClips(), 2); QVERIFY(target->getClip(0)->mutedModel()->isLinkedToModel(target->getClip(1)->soloModel()));
	}

	void partialPreparationCancellationRetainsPatternCells()
	{
		for (int index = 0; index < 4; ++index) { Track::create(Track::Type::Pattern, Engine::getSong()); } auto row = Track::create(Track::Type::Instrument, Engine::patternStore());
		QVERIFY(row->getClip(0)->mutedModel()->linkToModel(row->getClip(1)->soloModel())); const QPointer<Clip> oldA(row->getClip(2)), oldB(row->getClip(3)); bool dispatched = false; QTimer::singleShot(0, this, [&] { dispatched = true; });
		const QJsonArray entries{QJsonObject{{"container", "pattern"}, {"track", 0}, {"clip", 0}, {"targetClip", 2}, {"replace", true}}, QJsonObject{{"container", "pattern"}, {"track", 0}, {"clip", 1}, {"targetClip", 3}, {"replace", true}}};
		const auto result = StudioCapabilities::execute("clip.selection.copy", {{"items", entries}}, [&] { return row->numOfClips() > 4; }); QVERIFY(!result.value("ok").toBool()); QVERIFY(!dispatched); QCOMPARE(row->numOfClips(), 4); QCOMPARE(row->getClip(2), oldA.data()); QCOMPARE(row->getClip(3), oldB.data()); QCoreApplication::processEvents(); QVERIFY(dispatched);
	}

	void queuedSelectionCopiesAreSkippedAfterSourceOrDestinationChanges_data()
	{
		QTest::addColumn<QString>("protocol"); QTest::addColumn<bool>("catalog"); QTest::addColumn<bool>("source");
		for (const auto protocol : {QString("chat"), QString("responses")}) { for (const auto catalog : {false, true}) { for (const auto source : {false, true})
		{ QTest::newRow(qPrintable(protocol + QString("-%1-%2").arg(catalog).arg(source))) << protocol << catalog << source; } } }
	}
	void queuedSelectionCopiesAreSkippedAfterSourceOrDestinationChanges()
	{
		QFETCH(QString, protocol); QFETCH(bool, catalog); QFETCH(bool, source); QTemporaryDir directory; QString error; bool inferred = false;
		StudioRuntime runtime(directory.path(), [&](const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&) {
			if (!inferred)
			{
				inferred = true; const auto change = source ? call("change", "clip.remove", {{"track", 0}, {"clip", 0}}) : call("change", "track.remove", {{"track", 1}});
				return decision(protocol, {change, call("copy", "clip.selection.copy", {{"items", items()}}, catalog), call("premature", "agent.finish", {{"status", "complete"}, {"summary", "Old batch"}, {"evidence", QJsonArray{"change"}}})});
			}
			return decision(protocol, {call("done", "agent.finish", {{"status", "complete"}, {"summary", "Observed changed selection"}, {"evidence", QJsonArray{"change"}}})});
		});
		QVERIFY(runtime.open(error)); QCOMPARE(seed().size(), 2); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(runtime.process(request("provider", "session.provider.set", provider(protocol))).value("ok").toBool()); QVERIFY(runtime.process(request("chat", "session.chat", {{"text", "Handle the changed native selection."}})).value("ok").toBool());
		for (int index = 0; index < 4; ++index) { runtime.advanceAgent(); }
		QVERIFY(record(directory, "change").value("ok").toBool()); const auto skipped = record(directory, "copy"); QVERIFY(!skipped.value("ok").toBool()); QVERIFY(skipped.value("error").toString().contains("changed a queued native selection"));
		QCOMPARE(job(directory).value("operations").toInt(), 2); QCOMPARE(job(directory).value("status").toString(), QString("complete")); if (source) { QCOMPARE(Engine::getSong()->tracks().at(1)->numOfClips(), 0); }
	}

	void nativeConstructorNotificationFailureLeavesNoRegisteredClip()
	{
		QCOMPARE(seed().size(), 2); auto target = Engine::getSong()->tracks().at(1); int notifications = 0;
		const auto connection = QObject::connect(target, &Track::clipAdded, this, [&](Clip*) { if (++notifications == 2) { throw std::runtime_error("fixture clip registration callback"); } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		const auto result = StudioCapabilities::execute("clip.selection.copy", {{"items", items()}}); QVERIFY(!result.value("ok").toBool()); QCOMPARE(target->numOfClips(), 0); QVERIFY(!target->getClips().size());
		QVERIFY(Engine::getSong()->tracks().at(0)->getClip(0)->mutedModel()->isLinkedToModel(Engine::getSong()->tracks().at(0)->getClip(1)->soloModel()));
	}

	void schemaAndToolCatalogDescribeNestedSelectionArguments()
	{
		QJsonObject schema; for (const auto& value : StudioCapabilities::schema()) { if (value.toObject().value("name") == "clip.selection.copy") { schema = value.toObject(); } }
		QVERIFY(schema.value("mutatesProject").toBool()); const auto field = schema.value("inputSchema").toObject().value("properties").toObject().value("items").toObject(); QCOMPARE(field.value("type").toString(), QString("array")); QCOMPARE(field.value("maxItems").toInt(), 4096); QVERIFY(!field.value("items").toObject().value("additionalProperties").toBool());
		for (const auto protocol : {QString("chat"), QString("responses")}) { QVERIFY(StudioAgent::tools(protocol).size() <= StudioAgent::ToolDeclarationLimit); }
		QJsonObject args{{"items", items()}}; QCOMPARE(StudioAgent::operation("clip_selection_copy", args), QString("clip.selection.copy"));
		args = {{"op", "clip.selection.copy"}, {"args", QJsonObject{{"items", items()}}}}; QCOMPARE(StudioAgent::operation("studio_execute", args), QString("clip.selection.copy")); QVERIFY(args.value("items").isArray());
	}
};
QTEST_GUILESS_MAIN(StudioClipSelectionCopyTest)
#include "StudioClipSelectionCopyTest.moc"
