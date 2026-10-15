/* Native whole-project linked rings and checkpoint restoration.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QTemporaryDir>
#include <memory>
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "JournallingObject.h"
#include "LfoController.h"
#include "Mixer.h"
#include "NativeLinkedControls.h"
#include "NotePlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
InstrumentTrack* track() { return static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); }
InstrumentTrack* pair()
{
	auto native = new InstrumentTrack(Engine::getSong()); native->volumeModel()->setInitValue(25); native->panningModel()->setInitValue(25);
	if (!native->volumeModel()->linkToModel(native->panningModel())) { return nullptr; } return native;
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes) { QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QDomElement content(QDomDocument& document) { return document.documentElement().firstChildElement("song"); }
QDomElement graph(QDomDocument& document) { return content(document).firstChildElement("nativeLinkedControls"); }
QDomElement instrument(QDomDocument& document) { return document.elementsByTagName("instrumenttrack").at(0).toElement(); }
bool load(const QString& path) { const bool result = Engine::getSong()->loadProject(path); Engine::audioEngine()->audioDev()->stopProcessing(); return result; }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
}

class LinkedProjectPersistenceTest : public QObject
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

	void wholeProjectRestoresCrossOwnerRingsAndStoredValues_data()
	{ QTest::addColumn<QString>("kind"); QTest::newRow("tracks") << QString("tracks"); QTest::newRow("mixer-route") << QString("mixer"); QTest::newRow("header") << QString("header"); }
	void wholeProjectRestoresCrossOwnerRingsAndStoredValues()
	{
		QFETCH(QString, kind); auto song = Engine::getSong(); QTemporaryDir directory; QVERIFY(directory.isValid()); AutomatableModel *a, *b;
		if (kind == "tracks")
		{ auto first = new InstrumentTrack(song), second = new InstrumentTrack(song); a = first->volumeModel(); b = second->volumeModel(); b->setValue(37); }
		else if (kind == "mixer")
		{ auto mixer = Engine::mixer(); const auto channel = mixer->createChannel(); a = &mixer->mixerChannel(channel)->m_volumeModel; b = mixer->createChannelSend(channel, 0, .5f)->amount(); }
		else { a = &song->tempoModel(); b = &Engine::mixer()->mixerChannel(0)->m_volumeModel; }
		QVERIFY(a->linkToModel(b)); const float av = a->storedValue(), bv = b->storedValue(); const auto path = directory.filePath("linked.mmp"); QVERIFY(song->saveProjectFile(path));
		QDomDocument document; QVERIFY(document.setContent(read(path))); QVERIFY(!graph(document).isNull()); QCOMPARE(graph(document).attribute("version"), QString("1"));
		QVERIFY(load(path)); QVERIFY2(!song->hasErrors(), qPrintable(song->errorSummary()));
		if (kind == "tracks") { a = track()->volumeModel(); b = static_cast<InstrumentTrack*>(song->tracks().at(1))->volumeModel(); }
		else if (kind == "mixer") { a = &Engine::mixer()->mixerChannel(1)->m_volumeModel; b = Engine::mixer()->channelSendModel(1, 0); }
		else { a = &song->tempoModel(); b = &Engine::mixer()->mixerChannel(0)->m_volumeModel; }
		QVERIFY(a->isLinkedToModel(b)); QCOMPARE(a->storedValue(), av); QCOMPARE(b->storedValue(), bv); QCOMPARE(a->countLinks(), std::size_t{1});
	}

	void scopedRestoreUsesNewModelsWhileJournalIdentitiesAreKept()
	{
		QTemporaryDir directory; QVERIFY(pair()); const auto path = directory.filePath("identity.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(path)); DataFile document(path);
		const auto state = instrument(document); FloatModel volume(0, 0, 200, 0, nullptr), pan(0, -100, 100, 0, nullptr); const auto id = volume.id();
		NativeLinkedControls::RestoreScope scope(document); JournallingObject::KeepIdentityScope identity;
		volume.loadSettings(state, "vol"); pan.loadSettings(state, "pan"); QString error; QVERIFY2(scope.finish(document.content(), error), qPrintable(error));
		QCOMPARE(volume.id(), id); QVERIFY(volume.isLinkedToModel(&pan)); QVERIFY(!volume.isLinkedToModel(track()->volumeModel()));
		QVERIFY(track()->volumeModel()->isLinkedToModel(track()->panningModel())); QCOMPARE(volume.storedValue(), 25.f); QCOMPARE(pan.storedValue(), 25.f);
	}

	void orderedRingPreservesInheritedControllerPrecedence()
	{
		QTemporaryDir directory; auto song = Engine::getSong(); auto a = new InstrumentTrack(song), c = new InstrumentTrack(song);
		QVERIFY(a->volumeModel()->linkToModel(a->panningModel())); QVERIFY(a->panningModel()->linkToModel(c->volumeModel()));
		for (int index = 0; index < 2; ++index)
		{
			auto controller = new LfoController(song); song->addController(controller); QDomDocument document; auto settings = document.createElement("lfocontroller");
			settings.setAttribute("type", 1); settings.setAttribute("base", index ? .8 : .2); settings.setAttribute("amount", 0); settings.setAttribute("speed", 2);
			controller->loadSettings(settings); (index ? a->panningModel() : a->volumeModel())->setControllerConnection(new ControllerConnection(controller));
		}
		const auto source = c->volumeModel()->nextLinkedModel(); QCOMPARE(source, static_cast<const AutomatableModel*>(a->panningModel()));
		const float expected = c->volumeModel()->value(); const auto path = directory.filePath("ordered.mmp"); QVERIFY(song->saveProjectFile(path));
		QVERIFY(load(path)); QVERIFY2(!song->hasErrors(), qPrintable(song->errorSummary()));
		a = track(); c = static_cast<InstrumentTrack*>(song->tracks().at(1)); QCOMPARE(c->volumeModel()->nextLinkedModel(), static_cast<const AutomatableModel*>(a->panningModel()));
		QCOMPARE(c->volumeModel()->value(), expected); QCOMPARE(a->volumeModel()->countLinks(), std::size_t{2});
	}

	void malformedGraphsDoNotInstallPartialRings_data()
	{
		QTest::addColumn<QString>("damage"); QTest::newRow("version") << QString("version"); QTest::newRow("missing-graph") << QString("missing");
		QTest::newRow("duplicate-member") << QString("duplicate"); QTest::newRow("dangling-ref") << QString("dangling");
		QTest::newRow("duplicate-model-ref") << QString("model"); QTest::newRow("singleton") << QString("singleton");
		QTest::newRow("foreign-marker") << QString("foreign"); QTest::newRow("duplicate-graph") << QString("graphs");
	}
	void malformedGraphsDoNotInstallPartialRings()
	{
		QFETCH(QString, damage); QTemporaryDir directory; QVERIFY(pair()); const auto path = directory.filePath("damaged.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(path));
		QDomDocument document; QVERIFY(document.setContent(read(path))); auto definition = graph(document); auto group = definition.firstChildElement("group"); auto member = group.firstChildElement("member");
		if (damage == "version") { definition.setAttribute("version", 2); }
		else if (damage == "missing") { content(document).removeChild(definition); }
		else if (damage == "duplicate") { group.appendChild(member.cloneNode(true)); }
		else if (damage == "dangling") { member.setAttribute("ref", 2147483647); }
		else if (damage == "model") { auto state = instrument(document); state.firstChildElement("pan").setAttribute("native_link_ref", state.firstChildElement("vol").attribute("native_link_ref")); }
		else if (damage == "singleton") { group.removeChild(member.nextSiblingElement()); }
		else if (damage == "foreign") { auto extra = document.createElement("unrestored"); extra.setAttribute("native_link_ref", member.attribute("ref")); content(document).appendChild(extra); }
		else { content(document).appendChild(definition.cloneNode(true)); }
		QVERIFY(write(path, document.toByteArray())); QVERIFY(load(path)); QVERIFY(Engine::getSong()->hasErrors());
		QVERIFY(!track()->volumeModel()->isLinked()); QVERIFY(!track()->panningModel()->isLinked());
	}

	void restoredLinksCannotIntroduceControllerFeedback()
	{
		QTemporaryDir directory; auto native = pair(); QVERIFY(native); auto controller = new LfoController(Engine::getSong()); Engine::getSong()->addController(controller);
		native->volumeModel()->setControllerConnection(new ControllerConnection(controller)); const auto child = controller->findChildren<AutomatableModel*>().front(); const auto savedId = ProjectJournal::idToSave(child->id());
		const auto path = directory.filePath("feedback.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(path)); QDomDocument document; QVERIFY(document.setContent(read(path)));
		instrument(document).firstChildElement("pan").removeAttribute("native_link_ref"); auto lfo = document.elementsByTagName("lfocontroller").at(0).toElement();
		auto base = document.createElement("base"); base.setAttribute("value", .5); base.setAttribute("id", savedId); base.setAttribute("native_link_ref", 3); lfo.appendChild(base);
		graph(document).firstChildElement("group").firstChildElement("member").nextSiblingElement().setAttribute("ref", 3);
		QVERIFY(write(path, document.toByteArray())); QVERIFY(load(path)); QVERIFY(Engine::getSong()->hasErrors()); QVERIFY(!track()->volumeModel()->isLinked());
		QVERIFY(track()->volumeModel()->controllerConnection());
	}

	void nativeCheckpointUndoRedoAndReopeningPreserveGroups()
	{
		QTemporaryDir directory; QString error; QJsonObject edit;
		{
			StudioRuntime runtime(directory.path()); QVERIFY2(runtime.open(error), qPrintable(error)); QVERIFY(pair());
			QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			edit = request("edit", "parameter.set", {{"parameter", static_cast<int>(track()->volumeModel()->id())}, {"value", 77}});
			QVERIFY(runtime.process(edit).value("ok").toBool()); QCOMPARE(track()->panningModel()->storedValue(), 77.f);
			QVERIFY(runtime.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			QVERIFY(track()->volumeModel()->isLinkedToModel(track()->panningModel())); QCOMPARE(track()->volumeModel()->storedValue(), 25.f);
			QVERIFY(runtime.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
			QVERIFY(track()->volumeModel()->isLinkedToModel(track()->panningModel())); QCOMPARE(track()->volumeModel()->storedValue(), 77.f);
		}
		StudioRuntime reopened(directory.path()); QVERIFY2(reopened.open(error), qPrintable(error)); QVERIFY(track()->volumeModel()->isLinkedToModel(track()->panningModel()));
		QCOMPARE(track()->volumeModel()->storedValue(), 77.f); track()->volumeModel()->setValue(33); const auto replay = reopened.process(edit); QVERIFY(replay.value("replayed").toBool());
		QCOMPARE(track()->volumeModel()->storedValue(), 33.f); QCOMPARE(track()->panningModel()->storedValue(), 33.f); QVERIFY(!reopened.stopping());
	}

	void legacyLoadClearsLinksOnReusedHeaderModels()
	{
		QTemporaryDir directory; const auto path = directory.filePath("legacy.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(path));
		auto tempo = &Engine::getSong()->tempoModel(); auto volume = &Engine::mixer()->mixerChannel(0)->m_volumeModel; QVERIFY(tempo->linkToModel(volume));
		QVERIFY(load(path)); QVERIFY(!Engine::getSong()->hasErrors()); QVERIFY(!Engine::getSong()->tempoModel().isLinked()); QVERIFY(!Engine::mixer()->mixerChannel(0)->m_volumeModel.isLinked());
	}

	void failedStructuredLoadRollsBackThePreviousNativeGroup()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.path()); QString error; QVERIFY(runtime.open(error)); QVERIFY(pair());
		QVERIFY(runtime.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		const auto path = directory.filePath("bad.mmp"); QVERIFY(Engine::getSong()->saveProjectFile(path)); QDomDocument document; QVERIFY(document.setContent(read(path)));
		graph(document).setAttribute("version", 99); QVERIFY(write(path, document.toByteArray()));
		const auto rejected = runtime.process(request("bad-load", "project.load", {{"path", path}, {"summary", true}})); QVERIFY(!rejected.value("ok").toBool());
		QVERIFY(rejected.value("nativeWarnings").toString().contains("linked controls")); QVERIFY(!Engine::getSong()->hasErrors());
		QVERIFY(track()->volumeModel()->isLinkedToModel(track()->panningModel())); QCOMPARE(track()->volumeModel()->storedValue(), 25.f); QCOMPARE(Engine::getSong()->getTempo(), 123);
		QVERIFY(!runtime.stopping());
	}

	void unsupportedMemberRefusesPublicationAndKeepsExistingFile()
	{
		QTemporaryDir directory; auto native = new InstrumentTrack(Engine::getSong()); FloatModel external(42, 0, 200, 0, nullptr); QVERIFY(native->volumeModel()->linkToModel(&external));
		const auto path = directory.filePath("preserved.mmp"); QVERIFY(write(path, "Existing project bytes"));
		QVERIFY(!Engine::getSong()->saveProjectFile(path)); QCOMPARE(read(path), QByteArray("Existing project bytes")); QVERIFY(Engine::getSong()->hasErrors());
		QVERIFY(native->volumeModel()->isLinkedToModel(&external));
	}

	void repeatedSaveAfterUnlinkDoesNotRepublishAnOldGraph()
	{
		QTemporaryDir directory; QVERIFY(pair()); const auto first = directory.filePath("first.mmp"), second = directory.filePath("second.mmp");
		QVERIFY(Engine::getSong()->saveProjectFile(first)); QVERIFY(load(first)); track()->volumeModel()->unlink(); QVERIFY(Engine::getSong()->saveProjectFile(second));
		QDomDocument document; QVERIFY(document.setContent(read(second))); QVERIFY(graph(document).isNull()); QVERIFY(load(second)); QVERIFY(!track()->volumeModel()->isLinked()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void cancellationDoesNotPublishOrChangeTheLiveGroup()
	{
		QTemporaryDir directory; QVERIFY(pair()); const auto path = directory.filePath("cancelled.mmp"); QVERIFY(write(path, "Prior bytes"));
		{ FileResourceIO::Scope work([] { return true; }); QVERIFY(!Engine::getSong()->saveProjectFile(path)); }
		QCOMPARE(read(path), QByteArray("Prior bytes")); QVERIFY(track()->volumeModel()->isLinkedToModel(track()->panningModel()));
	}
};
QTEST_GUILESS_MAIN(LinkedProjectPersistenceTest)
#include "LinkedProjectPersistenceTest.moc"
