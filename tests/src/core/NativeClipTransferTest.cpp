/* Native clipboard/drag selection state and linked control recovery.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QScopeGuard>
#include <memory>
#include <vector>
#include <stdexcept>
#include <thread>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "LfoController.h"
#include "MidiClip.h"
#include "NativeClipTransfer.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "PatternStore.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "SampleClip.h"
#include "SampleBuffer.h"
#include "Song.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
Track::Type kind(int index)
{ return index == 0 ? Track::Type::Instrument : index == 1 ? Track::Type::Sample : index == 2 ? Track::Type::Automation : Track::Type::Pattern; }
QList<Clip*> pair(int type = 0)
{
	auto first = Track::create(kind(type), Engine::getSong())->createClip(0), second = Track::create(kind(type), Engine::getSong())->createClip(192);
	if (type == 1)
	{
		for (auto clip : {first, second})
		{
			std::vector<SampleFrame> frames(512); for (std::size_t index = 0; index < frames.size(); ++index) { frames[index][0] = index / 1024.f; frames[index][1] = -frames[index][0]; }
			static_cast<SampleClip*>(clip)->setSampleBuffer(std::make_shared<SampleBuffer>(std::move(frames), 24000));
		}
	}
	if (!first->mutedModel()->linkToModel(second->soloModel())) { return {}; } first->setMuted(true); return {first, second};
}
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
}
class TransferFaultSampleTrack : public SampleTrack
{
public:
	using SampleTrack::SampleTrack;
	int fault = 0;
	Clip* createClip(const TimePos& pos) override
	{
		if (fault == 1) { throw std::runtime_error("before transfer construction"); }
		if (fault == 2) { throw 42; }
		auto clip = SampleTrack::createClip(pos);
		if (fault == 3) { throw std::runtime_error("after transfer construction"); }
		if (fault == 4) { throw 42; }
		return clip;
	}
};

class NativeClipTransferTest : public QObject
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
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void selectionsRestoreCrossClipRingsWithIndependentIdentities_data()
	{
		QTest::addColumn<int>("type"); QTest::addColumn<bool>("bytes");
		QTest::newRow("midi-native") << 0 << false; QTest::newRow("midi-bytes") << 0 << true;
		QTest::newRow("sample-native") << 1 << false; QTest::newRow("sample-bytes") << 1 << true;
		QTest::newRow("automation-native") << 2 << false; QTest::newRow("automation-bytes") << 2 << true;
		QTest::newRow("pattern-native") << 3 << false; QTest::newRow("pattern-bytes") << 3 << true;
	}
	void selectionsRestoreCrossClipRingsWithIndependentIdentities()
	{
		QFETCH(int, type); QFETCH(bool, bytes); auto selected = pair(type); QCOMPARE(selected.size(), 2);
		if (auto midi = dynamic_cast<MidiClip*>(selected.first())) { midi->clearNotes(); midi->addNote(Note(48, 0, 63), false); }
		const auto id = selected.first()->mutedModel()->id(); DataFile document(DataFile::Type::DragNDropData); QString error;
		QVERIFY2(NativeClipTransfer::save(selected, selected.first(), document, error), qPrintable(error));
		QCOMPARE(document.content().firstChildElement("copyMetadata").attribute("initialTrackIndex").toInt(), 0);
		QVERIFY(!document.content().firstChildElement("nativeLinkedControls").isNull());
		const auto incoming = bytes ? DataFile(document.toString().toUtf8()) : document;
		auto first = Track::create(kind(type), Engine::getSong()), second = Track::create(kind(type), Engine::getSong()); QList<Clip*> copied;
		QVERIFY2(NativeClipTransfer::load(incoming, {{first, TimePos(384)}, {second, TimePos(576)}}, copied, error), qPrintable(error)); QCOMPARE(copied.size(), 2);
		QVERIFY(copied.first()->mutedModel()->isLinkedToModel(copied.last()->soloModel())); QVERIFY(!copied.first()->mutedModel()->isLinkedToModel(selected.first()->mutedModel()));
		QVERIFY(copied.first()->mutedModel()->id() != id); QCOMPARE(selected.first()->mutedModel()->id(), id);
		QCOMPARE(copied.first()->startPosition().getTicks(), 384); QCOMPARE(copied.last()->startPosition().getTicks(), 576);
		copied.first()->setMuted(false); QVERIFY(!copied.last()->isSolo()); QVERIFY(selected.first()->isMuted());
		if (auto midi = dynamic_cast<MidiClip*>(copied.first())) { QCOMPARE(midi->notes().size(), std::size_t{1}); QCOMPARE(midi->notes().front()->key(), 63); }
		if (auto sample = dynamic_cast<SampleClip*>(copied.first())) { QCOMPARE(sample->sample().sampleRate(), 24000); QCOMPARE(sample->sample().sampleSize(), std::size_t{512}); QCOMPARE(sample->sample().data()[37][0], 37 / 1024.f); }
	}

	void omittedMembersBlockPublicationWithoutChangingTheSelection_data()
	{ QTest::addColumn<int>("failure"); QTest::newRow("outside-selection") << 0; QTest::newRow("omitted-owned-field") << 1; QTest::newRow("unregistered-controller") << 2; }
	void omittedMembersBlockPublicationWithoutChangingTheSelection()
	{
		QFETCH(int, failure); auto selected = pair(); QCOMPARE(selected.size(), 2); FloatModel omitted(1, 0, 1, 1, failure == 1 ? static_cast<Model*>(selected.first()) : static_cast<Model*>(Engine::getSong()));
		std::unique_ptr<LfoController> unregistered;
		if (failure == 2) { unregistered = std::make_unique<LfoController>(Engine::getSong()); selected.first()->mutedModel()->setControllerConnection(new ControllerConnection(unregistered.get())); }
		else { QVERIFY(selected.first()->mutedModel()->linkToModel(&omitted)); }
		DataFile document(DataFile::Type::DragNDropData); QString error;
		QVERIFY(!NativeClipTransfer::save(selected, selected.first(), document, error)); QVERIFY(!document.serializationError().isEmpty());
		QTemporaryDir directory; const auto path = directory.filePath("prior.xml"); { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("Prior transfer bytes"), qint64{20}); }
		QVERIFY(!document.writeFile(path)); QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArray("Prior transfer bytes"));
		if (failure != 2) { QVERIFY(selected.first()->mutedModel()->isLinkedToModel(&omitted)); }
		QVERIFY(selected.first()->mutedModel()->isLinkedToModel(selected.last()->soloModel())); QCOMPARE(selected.first()->getTrack()->numOfClips(), 1);
	}

	void rejectedDocumentsRemoveEveryPreparedClip_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("version") << 0; QTest::newRow("duplicate-graph") << 1; QTest::newRow("missing-member") << 2;
		QTest::newRow("unrestored-marker") << 3; QTest::newRow("duplicate-container") << 4; QTest::newRow("wrong-type") << 5;
		QTest::newRow("wrong-node") << 6; QTest::newRow("wrong-count") << 7; QTest::newRow("position-bound") << 8;
		QTest::newRow("unresolved-controller") << 9;
	}
	void rejectedDocumentsRemoveEveryPreparedClip()
	{
		QFETCH(int, failure); auto selected = pair(); QCOMPARE(selected.size(), 2); DataFile document(DataFile::Type::DragNDropData); QString error; QVERIFY(NativeClipTransfer::save(selected, selected.first(), document, error));
		auto graph = document.content().firstChildElement("nativeLinkedControls"), parent = document.content().firstChildElement("clips"), wrapper = parent.firstChildElement();
		if (failure == 0) { graph.setAttribute("version", 99); }
		if (failure == 1) { document.content().appendChild(graph.cloneNode(true)); }
		if (failure == 2) { graph.firstChildElement().firstChildElement().setAttribute("ref", 99999); }
		if (failure == 3) { auto node = document.createElement("extra"); node.setAttribute("metadata", 1); node.setAttribute("native_link_ref", 99999); wrapper.firstChildElement().appendChild(node); }
		if (failure == 4) { document.content().appendChild(parent.cloneNode(true)); }
		if (failure == 5) { wrapper.setAttribute("trackType", static_cast<int>(Track::Type::Sample)); }
		if (failure == 6) { wrapper.firstChildElement().setTagName("sampleclip"); }
		if (failure == 9) { auto connection = document.createElement("connection"), model = document.createElement("muted"); model.setAttribute("id", 99999); connection.appendChild(model); wrapper.firstChildElement().appendChild(connection); }
		QList<NativeClipTransfer::Target> targets{{selected.first()->getTrack(), TimePos(384)}, {selected.last()->getTrack(), TimePos(576)}};
		if (failure == 7) { targets.removeLast(); } if (failure == 8) { targets[0].position = TimePos(MaxSongLength + 1); }
		QList<Clip*> copied; QVERIFY(!NativeClipTransfer::load(document, targets, copied, error)); QVERIFY(copied.isEmpty()); QVERIFY(!error.isEmpty());
		QCOMPARE(selected.first()->getTrack()->numOfClips(), 1); QCOMPARE(selected.last()->getTrack()->numOfClips(), 1);
		QVERIFY(selected.first()->mutedModel()->isLinkedToModel(selected.last()->soloModel()));
	}

	void constructorExceptionsCleanClipsAttachedBeforeReturning_data()
	{ QTest::addColumn<int>("fault"); QTest::newRow("before-standard") << 1; QTest::newRow("before-nonstandard") << 2; QTest::newRow("after-standard") << 3; QTest::newRow("after-nonstandard") << 4; }
	void constructorExceptionsCleanClipsAttachedBeforeReturning()
	{
		QFETCH(int, fault); auto selected = pair(1); QCOMPARE(selected.size(), 2); DataFile document(DataFile::Type::DragNDropData); QString error; QVERIFY(NativeClipTransfer::save(selected, selected.first(), document, error));
		auto first = new TransferFaultSampleTrack(Engine::getSong()), second = new TransferFaultSampleTrack(Engine::getSong()); auto sentinel = second->createClip(0); second->fault = fault;
		const auto journal = Engine::projectJournal(); const auto previous = journal->isJournalling();
		const auto restore = qScopeGuard([&] { journal->setJournalling(previous); journal->clearJournal(); });
		journal->clearJournal(); journal->setJournalling(true); first->getMutedModel()->setValue(true); QVERIFY(journal->canUndo());
		QList<Clip*> copied; QVERIFY(!NativeClipTransfer::load(document, {{first, TimePos(384)}, {second, TimePos(576)}}, copied, error)); QVERIFY(copied.isEmpty());
		QCOMPARE(first->numOfClips(), 0); QCOMPARE(second->numOfClips(), 1); QCOMPARE(second->getClip(0), sentinel);
		QVERIFY(journal->isJournalling()); QVERIFY(journal->canUndo()); QVERIFY(!journal->canRedo());
	}

	void cancellationAfterPartialConstructionDoesNotDispatchEvents()
	{
		auto selected = pair(); QCOMPARE(selected.size(), 2); DataFile document(DataFile::Type::DragNDropData); QString error; QVERIFY(NativeClipTransfer::save(selected, selected.first(), document, error));
		auto target = new InstrumentTrack(Engine::getSong()); bool dispatched = false; QTimer::singleShot(0, this, [&] { dispatched = true; }); QList<Clip*> copied;
		{ FileResourceIO::Scope work([&] { return target->numOfClips() > 0; }, true); QVERIFY(!NativeClipTransfer::load(document, {{target, TimePos(384)}, {target, TimePos(576)}}, copied, error)); }
		QVERIFY(!dispatched); QVERIFY(copied.isEmpty()); QCOMPARE(target->numOfClips(), 0); QVERIFY(selected.first()->mutedModel()->isLinkedToModel(selected.last()->soloModel()));
		QCoreApplication::processEvents(); QVERIFY(dispatched);
	}

	void legacyUnmarkedTransfersRetainNativeValuesAndNewIdentities()
	{
		auto track = new InstrumentTrack(Engine::getSong()); auto source = track->createClip(0); source->setMuted(true); DataFile document(DataFile::Type::DragNDropData); QString error;
		QVERIFY(NativeClipTransfer::save({source}, source, document, error)); document.content().removeChild(document.content().firstChildElement("nativeLinkedControls"));
		auto state = document.content().firstChildElement("clips").firstChildElement().firstChildElement(); state.removeAttribute("solo"); QList<Clip*> copied;
		QVERIFY2(NativeClipTransfer::load(document, {{track, TimePos(384)}}, copied, error), qPrintable(error)); QCOMPARE(copied.size(), 1); QVERIFY(copied.first()->isMuted()); QVERIFY(!copied.first()->isSolo());
		QVERIFY(copied.first()->mutedModel()->id() != source->mutedModel()->id()); QVERIFY(!copied.first()->mutedModel()->isLinked());
	}

	void sourceAndDestinationRecordingOwnershipRejectsTransfer()
	{
		auto selected = pair(1); QCOMPARE(selected.size(), 2); DataFile document(DataFile::Type::DragNDropData); QString error;
		auto source = static_cast<SampleClip*>(selected.first()); source->setRecord(true); QVERIFY(!NativeClipTransfer::save(selected, source, document, error)); source->setRecord(false);
		DataFile accepted(DataFile::Type::DragNDropData); QVERIFY(NativeClipTransfer::save(selected, source, accepted, error)); auto target = new SampleTrack(Engine::getSong()); auto recording = static_cast<SampleClip*>(target->createClip(0)); recording->setRecord(true); QList<Clip*> copied;
		QVERIFY(!NativeClipTransfer::load(accepted, {{target, TimePos(384)}, {target, TimePos(576)}}, copied, error)); QCOMPARE(target->numOfClips(), 1); QVERIFY(recording->isRecord()); recording->setRecord(false);
	}

	void foreignThreadCannotSerializeOrPublishNativeClips_data()
	{ QTest::addColumn<bool>("load"); QTest::newRow("save") << false; QTest::newRow("load") << true; }
	void foreignThreadCannotSerializeOrPublishNativeClips()
	{
		QFETCH(bool, load); auto selected = pair(); QCOMPARE(selected.size(), 2); DataFile document(DataFile::Type::DragNDropData); QString error; QVERIFY(NativeClipTransfer::save(selected, selected.first(), document, error)); bool accepted = true;
		std::thread worker([&] { QString failure; if (load) { QList<Clip*> clips; accepted = NativeClipTransfer::load(document, {{selected.first()->getTrack(), TimePos(384)}, {selected.last()->getTrack(), TimePos(576)}}, clips, failure); }
			else { DataFile output(DataFile::Type::DragNDropData); accepted = NativeClipTransfer::save(selected, selected.first(), output, failure); } }); worker.join();
		QVERIFY(!accepted); QCOMPARE(selected.first()->getTrack()->numOfClips(), 1); QCOMPARE(selected.last()->getTrack()->numOfClips(), 1);
	}

	void unrelatedSharedRackGroupsDoNotBlockSelectionSerialization()
	{
		auto selected = pair(); QCOMPARE(selected.size(), 2); auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller);
		const auto fields = controller->findChildren<AutomatableModel*>(); QVERIFY(fields.size() >= 2); QVERIFY(fields.at(0)->linkToModel(fields.at(1)));
		selected.first()->mutedModel()->setControllerConnection(new ControllerConnection(controller)); DataFile document(DataFile::Type::DragNDropData); QString error; QVERIFY2(NativeClipTransfer::save(selected, selected.first(), document, error), qPrintable(error));
		auto target = new InstrumentTrack(Engine::getSong()); QList<Clip*> copied; QVERIFY2(NativeClipTransfer::load(document, {{target, TimePos(384)}, {target, TimePos(576)}}, copied, error), qPrintable(error));
		QVERIFY(copied.first()->mutedModel()->isLinkedToModel(copied.last()->soloModel())); QCOMPARE(copied.first()->mutedModel()->controllerConnection()->getController(), static_cast<Controller*>(controller));
	}

	void transferredSelectionsSurviveRuntimeCheckpointReopening()
	{
		QTemporaryDir directory; QString error;
		{
			StudioRuntime runtime(directory.path()); QVERIFY(runtime.open(error)); auto selected = pair(); QCOMPARE(selected.size(), 2); DataFile document(DataFile::Type::DragNDropData); QVERIFY(NativeClipTransfer::save(selected, selected.first(), document, error));
			auto target = new InstrumentTrack(Engine::getSong()); QList<Clip*> copied; QVERIFY(NativeClipTransfer::load(document, {{target, TimePos(384)}, {target, TimePos(576)}}, copied, error));
			QVERIFY(runtime.process(request("preserve", "song.set", {{"tempo", 123}})).value("ok").toBool());
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); auto target = Engine::getSong()->tracks().at(2); QCOMPARE(target->numOfClips(), 2);
		QVERIFY(target->getClip(0)->mutedModel()->isLinkedToModel(target->getClip(1)->soloModel())); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void invalidSelectionsCannotProducePublishableDocuments_data()
	{
		QTest::addColumn<int>("failure"); QTest::newRow("empty") << 0; QTest::newRow("null-anchor") << 1; QTest::newRow("unselected-anchor") << 2;
		QTest::newRow("duplicate-clip") << 3; QTest::newRow("foreign-container") << 4; QTest::newRow("nonempty-document") << 5;
	}
	void invalidSelectionsCannotProducePublishableDocuments()
	{
		QFETCH(int, failure); auto selected = pair(); QCOMPARE(selected.size(), 2); auto anchor = selected.first(); DataFile document(DataFile::Type::DragNDropData); QString error;
		if (failure == 0) { selected.clear(); } if (failure == 1) { anchor = nullptr; } if (failure == 2) { anchor = selected.last()->getTrack()->createClip(384); }
		if (failure == 3) { selected.append(selected.first()); }
		if (failure == 4) { Track::create(Track::Type::Pattern, Engine::getSong()); selected.append(Track::create(Track::Type::Instrument, Engine::patternStore())->getClip(0)); }
		if (failure == 5) { document.content().appendChild(document.createElement("existing")); }
		QVERIFY(!NativeClipTransfer::save(selected, anchor, document, error)); QVERIFY(!document.serializationError().isEmpty()); QVERIFY(!error.isEmpty());
	}
};
QTEST_GUILESS_MAIN(NativeClipTransferTest)
#include "NativeClipTransferTest.moc"
