/* Native inline note graphs in projects and whole-clip copies.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QPointer>
#include <QTemporaryDir>
#include <memory>
#include <stdexcept>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "MidiController.h"
#include "MidiEvent.h"
#include "NativeClipTransfer.h"
#include "NativeLinkedControls.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
void notes(MidiClip* clip)
{
	for (int index = 0; index < 2; ++index) { auto note = clip->addNote(Note(48, index * 48, 60 + index), false); note->createDetuning(); note->detuning()->automationClip()->putValue(0, .5f, false); note->detuning()->automationClip()->putValue(24, -.75f, false); }
}
MidiClip* populated(InstrumentTrack* track) { auto clip = static_cast<MidiClip*>(track->createClip(0)); notes(clip); return clip; }
bool linked(MidiClip* clip)
{
	auto a = clip->notes().at(0)->detuning().get(), b = clip->notes().at(1)->detuning().get(); if (!a->linkToModel(b)) { return false; } a->setValue(3.25f);
	if (!a->automationClip()->mutedModel()->linkToModel(b->automationClip()->soloModel())) { return false; } a->automationClip()->setMuted(true); return true;
}
MidiClip* current(int index = 0) { return static_cast<MidiClip*>(Engine::getSong()->tracks().front()->getClip(index)); }
}
class LinkedInlinePersistenceTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	bool journalling = false;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing(); journalling = Engine::projectJournal()->isJournalling(); Engine::projectJournal()->setJournalling(false);
	}
	void cleanupTestCase() { Engine::projectJournal()->setJournalling(journalling); Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::projectJournal()->setJournalling(false); }
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }
	void fileWriterRetainsNativeControlsAndDropsJournalMetadata_data()
	{
		QTest::addColumn<int>("type");
		QTest::newRow("project") << int(DataFile::Type::SongProject);
		QTest::newRow("template") << int(DataFile::Type::SongProjectTemplate);
		QTest::newRow("preset") << int(DataFile::Type::InstrumentTrackSettings);
	}
	void fileWriterRetainsNativeControlsAndDropsJournalMetadata()
	{
		QFETCH(int, type); DataFile document(static_cast<DataFile::Type>(type)); auto root = document.content();
		for (const auto& tag : {QString("inlinecontrol"), QString("muted"), QString("solo"), QString("connection"), QString("nativeInlineControls")})
		{
			auto state = document.createElement(tag); state.setAttribute("metadata", 1); state.setAttribute("native_state", 1); state.setAttribute("value", 3);
			auto journal = document.createElement("journal"); journal.setAttribute("metadata", 1); state.appendChild(journal); root.appendChild(state);
		}
		auto temporary = document.createElement("temporary"); temporary.setAttribute("metadata", 1); root.appendChild(temporary);
		QString xml; QTextStream stream(&xml); document.write(stream); stream.flush(); QDomDocument restored; QVERIFY(restored.setContent(xml));
		const auto content = restored.documentElement().firstChildElement(root.tagName()); QVERIFY(!content.isNull()); QCOMPARE(content.childNodes().size(), 5);
		for (auto state = content.firstChildElement(); !state.isNull(); state = state.nextSiblingElement())
		{ QCOMPARE(state.attribute("value"), QString("3")); QVERIFY(state.firstChild().isNull()); }
	}

	void nativeProjectRoundTripIncludesUnparentedHelpersAndCurves()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto source = populated(track); QVERIFY(linked(source)); auto helper = source->notes().front()->detuning().get(); auto curve = helper->automationClip();
		QVERIFY(NativeLinkedControls::projectModels().contains(helper)); QVERIFY(NativeLinkedControls::clipModels(source).contains(helper)); QVERIFY(NativeLinkedControls::clipModels(source).contains(curve->mutedModel())); QVERIFY(!NativeLinkedControls::trackModels(track).contains(helper)); QVERIFY(NativeLinkedControls::trackModels(track, true).contains(helper));
		QTemporaryDir directory; const auto path = directory.filePath("inline.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(path)); const QPointer<DetuningHelper> old(helper); QVERIFY(Engine::getSong()->loadProject(path)); Engine::audioEngine()->audioDev()->stopProcessing(); QVERIFY(!old);
		auto a = current()->notes().at(0)->detuning().get(), b = current()->notes().at(1)->detuning().get(); QVERIFY(a->isLinkedToModel(b)); QCOMPARE(a->storedValue(), 3.25f); QVERIFY(a->automationClip()->mutedModel()->isLinkedToModel(b->automationClip()->soloModel())); QVERIFY(a->automationClip()->isMuted()); QCOMPARE(a->automationClip()->valueAt(24), -.75f); QCOMPARE(a->automationClip()->objects().size(), std::size_t{1}); QCOMPARE(a->automationClip()->objects().front().data(), static_cast<AutomatableModel*>(a)); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void wholeClipCopyPathsRetainIndependentInlineGraphs_data()
	{ QTest::addColumn<int>("path"); QTest::newRow("clone") << 0; QTest::newRow("copy-to") << 1; QTest::newRow("state-copy") << 2; QTest::newRow("selection-copy") << 3; QTest::newRow("clipboard") << 4; }
	void wholeClipCopyPathsRetainIndependentInlineGraphs()
	{
		QFETCH(int, path); auto sourceTrack = new InstrumentTrack(Engine::getSong()), targetTrack = new InstrumentTrack(Engine::getSong()); auto source = populated(sourceTrack); QVERIFY(linked(source)); MidiClip* copy = nullptr; QString error;
		if (path == 0) { copy = source->clone(); } else if (path == 1) { copy = static_cast<MidiClip*>(source->copyTo(targetTrack)); }
		else if (path == 2) { copy = static_cast<MidiClip*>(targetTrack->createClip(192)); Clip::copyStateTo(source, copy); QCOMPARE(copy->startPosition().getTicks(), 192); }
		else { QList<Clip*> results; const QList<NativeClipTransfer::Target> targets{{targetTrack, 192}}; if (path == 3) { QVERIFY2(NativeClipTransfer::copy({source}, targets, results, error), qPrintable(error)); } else { DataFile document(DataFile::Type::ClipboardData); QVERIFY(NativeClipTransfer::save({source}, source, document, error)); QVERIFY2(NativeClipTransfer::load(document, targets, results, error), qPrintable(error)); } copy = static_cast<MidiClip*>(results.front()); }
		QVERIFY(copy); auto a = copy->notes().at(0)->detuning().get(), b = copy->notes().at(1)->detuning().get(), original = source->notes().at(0)->detuning().get(); QVERIFY(a != original); QVERIFY(a->id() != original->id()); QVERIFY(a->isLinkedToModel(b)); QVERIFY(!a->isLinkedToModel(original)); QCOMPARE(a->storedValue(), 3.25f); QVERIFY(a->automationClip()->mutedModel()->isLinkedToModel(b->automationClip()->soloModel())); QVERIFY(a->automationClip()->isMuted()); QCOMPARE(a->automationClip()->valueAt(24), -.75f); QCOMPARE(a->automationClip()->objects().size(), std::size_t{1}); QCOMPARE(a->automationClip()->objects().front().data(), static_cast<AutomatableModel*>(a)); a->automationClip()->putValue(24, 1.f, false); QCOMPARE(original->automationClip()->valueAt(24), -.75f); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void externalInlineLinksPersistInProjectsAndRefusePartialCopies()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto source = populated(track); QVERIFY(source->notes().front()->detuning()->linkToModel(track->volumeModel())); QTemporaryDir directory; QVERIFY(Engine::getSong()->saveProjectFile(directory.filePath("external.mmp"))); const auto count = track->numOfClips(); QVERIFY(!source->clone()); QCOMPARE(track->numOfClips(), count); QVERIFY(source->notes().front()->detuning()->isLinkedToModel(track->volumeModel()));
	}

	void jointSelectionCanCopyGroupsSpanningTwoMidiClips()
	{
		auto track = new InstrumentTrack(Engine::getSong()), target = new InstrumentTrack(Engine::getSong()); auto first = populated(track), second = populated(track); QVERIFY(first->notes().front()->detuning()->linkToModel(second->notes().front()->detuning().get())); QList<Clip*> results; QString error; QVERIFY2(NativeClipTransfer::copy({first, second}, {{target, 192}, {target, 384}}, results, error), qPrintable(error)); auto a = static_cast<MidiClip*>(results.at(0))->notes().front()->detuning().get(), b = static_cast<MidiClip*>(results.at(1))->notes().front()->detuning().get(); QVERIFY(a->isLinkedToModel(b)); QVERIFY(!a->isLinkedToModel(first->notes().front()->detuning().get()));
	}

	void ownedMidiBindingCopiesItsStateWithoutBakingItsLiveValue()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto source = populated(track); auto helper = source->notes().front()->detuning().get(); helper->setValue(2); auto midi = new MidiController(nullptr); midi->midiPort()->setInputController(74); midi->processInEvent(MidiEvent(MidiControlChange, 0, 74, 80), {}); helper->setControllerConnection(new ControllerConnection(midi)); QVERIFY(helper->value() != helper->storedValue()); auto copy = source->clone(); QVERIFY(copy); auto copied = copy->notes().front()->detuning().get(); QCOMPARE(copied->storedValue(), 2.f); QVERIFY(copied->controllerConnection()); QVERIFY(copied->controllerConnection()->getController() != midi); QVERIFY(dynamic_cast<MidiController*>(copied->controllerConnection()->getController()));
	}

	void legacyLazyEmptyAndZeroLengthNotesRetainTheirNativeCurveState_data()
	{ QTest::addColumn<int>("kind"); QTest::newRow("legacy") << 0; QTest::newRow("lazy") << 1; QTest::newRow("empty-curve") << 2; QTest::newRow("zero-length") << 3; }
	void legacyLazyEmptyAndZeroLengthNotesRetainTheirNativeCurveState()
	{
		QFETCH(int, kind); Note source(kind == 3 ? 0 : 48, 0, 60); if (kind == 1) { source = Note(48, 0, 60, 100, 0, std::make_shared<DetuningHelper>()); } else { source.createDetuning(); source.detuning()->automationClip()->putValue(24, -.75f, false); if (kind == 2) { source.detuning()->automationClip()->clear(); } }
		QDomDocument document; auto root = document.createElement("notes"); document.appendChild(root); auto state = source.saveState(document, root); if (kind == 0) { state.removeChild(state.firstChildElement("inlinecontrol")); } Note restored; restored.restoreState(state); QVERIFY(restored.detuning());
		if (kind == 1) { QVERIFY(!restored.detuning()->existingAutomationClip()); } else if (kind == 2) { QVERIFY(restored.detuning()->automationClip()->getTimeMap().isEmpty()); } else { QCOMPARE(restored.detuning()->automationClip()->valueAt(24), -.75f); QCOMPARE(restored.detuning()->automationClip()->objects().front().data(), static_cast<AutomatableModel*>(restored.detuning().get())); }
	}

	void inspectionProvidesCurrentInlineHandlesWithoutCreatingLazyCurves()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto clip = static_cast<MidiClip*>(track->createClip(0)); auto note = clip->addNote(Note(48, 0, 60), false); const QJsonObject args{{"track", 0}, {"clip", 0}, {"note", 0}}; auto result = StudioCapabilities::execute("note.detuning.inspect", args); QVERIFY(result.value("ok").toBool()); QVERIFY(!note->detuning()); note->createDetuning(); result = StudioCapabilities::execute("note.detuning.inspect", args); QVERIFY(result.value("ok").toBool()); QCOMPARE(result.value("result").toObject().value("parameter").toInt(), static_cast<int>(note->detuning()->id())); result = StudioCapabilities::execute("parameter.inspect", {{"parameter", static_cast<int>(note->detuning()->id())}, {"choices", false}}); QVERIFY(result.value("ok").toBool()); auto location = result.value("result").toObject().value("location").toObject(); QCOMPARE(location.value("track").toInt(-1), 0); QCOMPARE(location.value("clip").toInt(-1), 0); QCOMPARE(location.value("note").toInt(-1), 0);
	}

	void malformedInlineMetadataRefusesBeforeReplacingHelperValues_data()
	{ QTest::addColumn<int>("damage"); QTest::newRow("version") << 0; QTest::newRow("duplicate") << 1; QTest::newRow("curve-flag") << 2; QTest::newRow("missing-curve") << 3; QTest::newRow("unexpected-curve") << 4; }
	void malformedInlineMetadataRefusesBeforeReplacingHelperValues()
	{
		QFETCH(int, damage); Note source(48, 0, 60); source.createDetuning(); QDomDocument document; auto root = document.createElement("note"); document.appendChild(root); source.detuning()->saveSettings(document, root); auto state = root.firstChildElement("inlinecontrol"); if (damage == 0) { state.setAttribute("version", 99); } else if (damage == 1) { root.appendChild(state.cloneNode()); } else if (damage == 2) { state.setAttribute("hasCurve", 4); } else if (damage == 3) { root.removeChild(root.firstChildElement(AutomationClip::classNodeName())); } else { state.setAttribute("hasCurve", 0); }
		DetuningHelper target; target.setRange(-MaxDetuning, MaxDetuning, .5); target.setValue(4); bool failed = false; try { target.loadSettings(root); } catch (const std::invalid_argument&) { failed = true; } QVERIFY(failed); QCOMPARE(target.storedValue(), 4.f); QVERIFY(!target.existingAutomationClip());
	}

	void runtimeHistoryAndReopeningPreserveInlineGroups()
	{
		QTemporaryDir directory; QString error; { StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); auto track = new InstrumentTrack(Engine::getSong()); QVERIFY(linked(populated(track))); QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool()); QVERIFY(runtime.process(request("copy", "clip.clone", {{"track", 0}, {"clip", 0}, {"position", 192}})).value("ok").toBool()); QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool()); QCOMPARE(Engine::getSong()->tracks().front()->numOfClips(), 1); QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool()); }
		StudioRuntime restored(directory.path()); QVERIFY2(restored.open(error), qPrintable(error)); auto copied = current(1); QVERIFY(copied->notes().at(0)->detuning()->isLinkedToModel(copied->notes().at(1)->detuning().get())); QVERIFY(!copied->notes().at(0)->detuning()->isLinkedToModel(current()->notes().at(0)->detuning().get())); QVERIFY(!restored.stopping());
	}

	void ordinaryNoteCopiesStillShareDetuningAndDeepClonesRebindEmptyCurves()
	{
		Note source(48, 0, 60); source.createDetuning(); source.detuning()->automationClip()->clear(); Note playback(source); QCOMPARE(playback.detuning(), source.detuning()); std::unique_ptr<Note> copied(source.clone()); QVERIFY(copied->detuning() != source.detuning()); QVERIFY(copied->detuning()->automationClip()->getTimeMap().isEmpty()); QCOMPARE(copied->detuning()->automationClip()->objects().front().data(), static_cast<AutomatableModel*>(copied->detuning().get()));
	}

	void inlineParametersCanUseStructuredNativeLinkOperations()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto clip = populated(track); auto a = clip->notes().at(0)->detuning().get(), b = clip->notes().at(1)->detuning().get(); QVERIFY(StudioCapabilities::execute("parameter.link", {{"parameter", static_cast<int>(a->id())}, {"peer", static_cast<int>(b->id())}}).value("ok").toBool()); QVERIFY(a->isLinkedToModel(b)); QVERIFY(StudioCapabilities::execute("parameter.unlink", {{"parameter", static_cast<int>(a->id())}}).value("ok").toBool()); QVERIFY(!a->isLinked());
	}

	void cancelledInlineSelectionCopyPublishesNoDestination()
	{
		auto track = new InstrumentTrack(Engine::getSong()), target = new InstrumentTrack(Engine::getSong()); auto source = populated(track); QVERIFY(linked(source)); const auto before = target->numOfClips(); QList<Clip*> results; QString error; FileResourceIO::Scope cancel([] { return true; }); QVERIFY(!NativeClipTransfer::copy({source}, {{target, 192}}, results, error)); QVERIFY(results.isEmpty()); QCOMPARE(target->numOfClips(), before); QVERIFY(source->notes().at(0)->detuning()->isLinkedToModel(source->notes().at(1)->detuning().get()));
	}

	void patternStoreInlineGroupsCopyBackToSong()
	{
		new PatternTrack(Engine::getSong()); auto track = new InstrumentTrack(Engine::patternStore()); auto source = static_cast<MidiClip*>(track->getClip(0)); notes(source); DetuningHelper* first = nullptr; DetuningHelper* second = nullptr; for (auto note : source->notes()) { if (note->detuning()) { if (!first) { first = note->detuning().get(); } else { second = note->detuning().get(); break; } } } QVERIFY(first); QVERIFY(second); QVERIFY(first->linkToModel(second));
		auto target = new InstrumentTrack(Engine::getSong()); auto copied = static_cast<MidiClip*>(source->copyTo(target)); DetuningHelper* a = nullptr; DetuningHelper* b = nullptr; for (auto note : copied->notes()) { if (note->detuning()) { if (!a) { a = note->detuning().get(); } else { b = note->detuning().get(); break; } } } QVERIFY(a); QVERIFY(b); QVERIFY(a->isLinkedToModel(b)); QVERIFY(!a->isLinkedToModel(first));
	}

	void sharedEditableHelperWithLinksRefusesPublication()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto clip = populated(track); const auto shared = clip->notes().front()->detuning(); *clip->notes().at(1) = Note(48, 48, 61, 100, 0, shared); QVERIFY(shared->linkToModel(shared->automationClip()->mutedModel()));
		QTemporaryDir directory; QVERIFY(!Engine::getSong()->saveProjectFile(directory.filePath("ambiguous.mmp"))); const auto count = track->numOfClips(); QVERIFY(!clip->clone()); QCOMPARE(track->numOfClips(), count); QVERIFY(shared->isLinkedToModel(shared->automationClip()->mutedModel()));
	}
};
QTEST_GUILESS_MAIN(LinkedInlinePersistenceTest)
#include "LinkedInlinePersistenceTest.moc"
