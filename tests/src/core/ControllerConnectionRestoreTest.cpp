/* Native persisted controller indices and unresolved links.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QPointer>
#include <QScopeGuard>
#include <QTemporaryDir>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiController.h"
#include "NotePlayHandle.h"
#include "Song.h"

using namespace lmms;
namespace
{
Controller* rack(const QString& name)
{
	auto song = Engine::getSong(); auto controller = Controller::create(Controller::ControllerType::Lfo, song);
	controller->setName(name); song->addController(controller); return controller;
}
QDomElement settings(QDomDocument& document, const QString& attribute, const QString& value)
{
	auto element = document.createElement("connection");
	if (!value.isNull()) { element.setAttribute(attribute, value); }
	return element;
}
bool read(const QString& path, QDomDocument& document)
{ QFile file(path); return file.open(QIODevice::ReadOnly) && document.setContent(file.readAll()); }
bool write(const QString& path, const QDomDocument& document)
{
	QFile file(path); const auto bytes = document.toByteArray(2);
	return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
QDomElement controllers(QDomDocument& document)
{ return document.documentElement().firstChildElement("song").firstChildElement("controllers"); }
bool load(const QString& path)
{
	const bool result = Engine::getSong()->loadProject(path); Engine::audioEngine()->audioDev()->stopProcessing(); return result;
}
InstrumentTrack* instrument()
{ return dynamic_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0)); }
void finalize()
{
	const auto changes = Engine::audioEngine()->requestChangesGuard(); ControllerConnection::finalizeConnections();
}
}

class ControllerConnectionRestoreTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none");
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false); Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stop(); Engine::getSong()->clearProject();
		Engine::getSong()->clearErrors(); Engine::getSong()->getSaveOptions().setDefaultOptions();
	}
	void cleanup() { Engine::getSong()->stop(); Engine::getSong()->clearProject(); }

	void nonElementProjectNodesDoNotShiftNativeBindings_data()
	{
		QTest::addColumn<int>("position");
		QTest::newRow("leading-comment") << 0; QTest::newRow("between-comment") << 1;
		QTest::newRow("trailing-comment") << 2; QTest::newRow("mixed-non-elements") << 3;
	}
	void nonElementProjectNodesDoNotShiftNativeBindings()
	{
		QFETCH(int, position); QTemporaryDir directory; QVERIFY(directory.isValid()); auto song = Engine::getSong();
		auto track = new InstrumentTrack(song); auto first = rack("First"); auto second = rack("Second");
		track->volumeModel()->setControllerConnection(new ControllerConnection(first)); track->panningModel()->setControllerConnection(new ControllerConnection(second));
		const auto path = directory.filePath("comments.mmp"); QVERIFY(song->saveProjectFile(path)); QDomDocument document; QVERIFY(read(path, document));
		auto list = controllers(document); QVERIFY(!list.isNull()); const auto a = list.firstChildElement(), b = a.nextSiblingElement(); QVERIFY(!b.isNull());
		const auto comment = document.createComment("Native controller indices ignore this annotation");
		if (position == 0) { list.insertBefore(comment, a); }
		else if (position == 1) { list.insertBefore(comment, b); }
		else if (position == 2) { list.appendChild(comment); }
		else
		{
			list.insertBefore(comment, a); list.insertBefore(document.createTextNode("\n\t"), b);
			list.insertBefore(document.createProcessingInstruction("studio", "annotation"), b); list.appendChild(document.createComment("tail"));
		}
		QVERIFY(write(path, document)); QVERIFY(load(path)); QCOMPARE(song->controllers().size(), std::size_t{2}); QVERIFY(instrument());
		auto volume = instrument()->volumeModel()->controllerConnection(), pan = instrument()->panningModel()->controllerConnection(); QVERIFY(volume); QVERIFY(pan);
		QCOMPARE(volume->getController(), song->controllers().at(0)); QCOMPARE(pan->getController(), song->controllers().at(1));
		QCOMPARE(volume->getController()->name(), QString("First")); QCOMPARE(pan->getController()->name(), QString("Second"));
	}

	void invalidPersistedIdsCannotAliasControllerZero_data()
	{
		QTest::addColumn<QString>("id"); QTest::newRow("missing") << QString{}; QTest::newRow("empty") << QStringLiteral("");
		QTest::newRow("text") << QString("unavailable"); QTest::newRow("suffix") << QString("0suffix");
		QTest::newRow("fraction") << QString("0.0"); QTest::newRow("exponent") << QString("1e0");
		QTest::newRow("overflow") << QString("2147483648"); QTest::newRow("underflow") << QString("-2147483649");
		QTest::newRow("negative") << QString("-1"); QTest::newRow("outside-rack") << QString("1"); QTest::newRow("maximum-int") << QString("2147483647");
	}
	void invalidPersistedIdsCannotAliasControllerZero()
	{
		QFETCH(QString, id); auto first = rack("Controller zero"); FloatModel model(42, 0, 100, 1, nullptr);
		auto connection = new ControllerConnection(nullptr); model.setControllerConnection(connection); const QPointer<ControllerConnection> retired(connection);
		QDomDocument document; connection->loadSettings(settings(document, "id", id));
		QCOMPARE(connection->getController()->type(), Controller::ControllerType::Dummy); QCOMPARE(first->connectionCount(), 0);
		finalize(); QVERIFY(!retired); QVERIFY(!model.controllerConnection()); QCOMPARE(model.value(), 42.f);
		QVERIFY(Engine::getSong()->hasErrors()); QCOMPARE(first->connectionCount(), 0);
	}

	void validLegacyIdsRetainNativeIntegerSyntax_data()
	{
		QTest::addColumn<QString>("id"); QTest::newRow("zero") << QString("0"); QTest::newRow("leading-zero") << QString("00");
		QTest::newRow("positive-sign") << QString("+0"); QTest::newRow("whitespace") << QString(" 0 "); QTest::newRow("second-slot") << QString("1");
	}
	void validLegacyIdsRetainNativeIntegerSyntax()
	{
		QFETCH(QString, id); rack("First"); rack("Second"); FloatModel model(42, 0, 100, 1, nullptr);
		auto connection = new ControllerConnection(nullptr); model.setControllerConnection(connection); QDomDocument document;
		connection->loadSettings(settings(document, "id", id)); finalize(); QCOMPARE(model.controllerConnection(), connection);
		QCOMPARE(connection->getController(), Engine::getSong()->controllers().at(id.toInt())); QVERIFY(connection->isFinalized()); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void malformedControllerTypesDoNotCreateDummyInstances_data()
	{
		QTest::addColumn<QString>("type"); QTest::newRow("missing") << QString{}; QTest::newRow("empty") << QStringLiteral("");
		QTest::newRow("text") << QString("lfo"); QTest::newRow("suffix") << QString("1suffix"); QTest::newRow("fraction") << QString("1.0");
		QTest::newRow("exponent") << QString("1e0"); QTest::newRow("overflow") << QString("2147483648");
		QTest::newRow("negative") << QString("-1"); QTest::newRow("unknown") << QString("4");
	}
	void malformedControllerTypesDoNotCreateDummyInstances()
	{
		QFETCH(QString, type); QDomDocument document; auto controller = Controller::create(settings(document, "type", type), Engine::getSong());
		QVERIFY(!controller); QVERIFY(Engine::getSong()->controllers().empty());
	}

	void unavailableProjectSlotsDetachOnlyTheirOwnLinks_data()
	{
		QTest::addColumn<QString>("type"); QTest::newRow("unknown") << QString("999"); QTest::newRow("malformed") << QString("lfo");
		QTest::newRow("missing") << QString{}; QTest::newRow("explicit-dummy") << QString("0"); QTest::newRow("overflow") << QString("2147483648");
	}
	void unavailableProjectSlotsDetachOnlyTheirOwnLinks()
	{
		QFETCH(QString, type); QTemporaryDir directory; QVERIFY(directory.isValid()); auto song = Engine::getSong(); auto track = new InstrumentTrack(song);
		track->volumeModel()->setValue(42); track->panningModel()->setValue(13); auto first = rack("Unavailable"); auto second = rack("Preserved");
		track->volumeModel()->setControllerConnection(new ControllerConnection(first)); track->panningModel()->setControllerConnection(new ControllerConnection(second));
		const auto path = directory.filePath("unavailable.mmp"); QVERIFY(song->saveProjectFile(path)); QDomDocument document; QVERIFY(read(path, document));
		auto list = controllers(document); QVERIFY(!list.isNull()); auto unavailable = list.firstChildElement(); QVERIFY(!unavailable.isNull());
		if (type.isNull()) { unavailable.removeAttribute("type"); } else { unavailable.setAttribute("type", type); }
		QVERIFY(write(path, document)); QVERIFY(load(path)); QCOMPARE(song->controllers().size(), std::size_t{1}); QVERIFY(instrument());
		QVERIFY(!instrument()->volumeModel()->controllerConnection()); QCOMPARE(instrument()->volumeModel()->value(), 42.f);
		auto connection = instrument()->panningModel()->controllerConnection(); QVERIFY(connection); QCOMPARE(connection->getController(), song->controllers().at(0));
		QCOMPARE(connection->getController()->name(), QString("Preserved")); QVERIFY(song->hasErrors());
		// Saving must remap the surviving native controller to its new rack index.
		instrument()->volumeModel()->setValue(57); const auto repaired = directory.filePath("repaired.mmp"); QVERIFY(song->saveProjectFile(repaired));
		QVERIFY(load(repaired)); QVERIFY(instrument()); QCOMPARE(instrument()->volumeModel()->value(), 57.f); QVERIFY(!instrument()->volumeModel()->controllerConnection());
		connection = instrument()->panningModel()->controllerConnection(); QVERIFY(connection); QCOMPARE(connection->getController(), song->controllers().at(0));
		QCOMPARE(connection->getController()->name(), QString("Preserved")); QCOMPARE(instrument()->panningModel()->storedValue(), 13.f);
	}

	void embeddedMidiControllerRetainsOwnedNativeSettings()
	{
		FloatModel original(42, 0, 100, 1, nullptr); auto midi = new MidiController(nullptr); midi->midiPort()->setInputChannel(16); midi->midiPort()->setInputController(74);
		original.setControllerConnection(new ControllerConnection(midi)); QDomDocument document; auto element = document.createElement("volume");
		original.controllerConnection()->saveSettings(document, element); FloatModel restored(42, 0, 100, 1, nullptr); auto connection = new ControllerConnection(nullptr);
		restored.setControllerConnection(connection); connection->loadSettings(element); finalize(); QVERIFY(restored.controllerConnection());
		auto owned = dynamic_cast<MidiController*>(connection->getController()); QVERIFY(owned); const QPointer<Controller> retired(owned);
		QCOMPARE(owned->midiPort()->inputChannel(), 16); QCOMPARE(owned->midiPort()->inputController(), 74); QVERIFY(Engine::getSong()->controllers().empty());
		owned->processInEvent(MidiEvent(MidiControlChange, 15, 74, 127), {}); Controller::triggerFrameCounter(); owned->currentValue(0); Controller::triggerFrameCounter();
		QCOMPARE(restored.value(), 100.f); delete connection; QVERIFY(!retired); QVERIFY(!restored.controllerConnection()); QCOMPARE(restored.value(), 42.f);
	}

	void finalizationCallbackRetirementKeepsOtherPendingLinks()
	{
		auto controller = rack("Native LFO"); FloatModel first(42, 0, 100, 1, nullptr), middle(42, 0, 100, 1, nullptr), last(42, 0, 100, 1, nullptr);
		auto a = new ControllerConnection(0), b = new ControllerConnection(0), c = new ControllerConnection(0);
		first.setControllerConnection(a); middle.setControllerConnection(b); last.setControllerConnection(c); const QPointer<ControllerConnection> removed(a);
		// Retire an already-processed entry while resolving the next one. An
		// index walk then skips the final entry after the registry shifts left.
		const auto connection = connect(b, &ControllerConnection::valueChanged, this, [&] { if (removed) { delete removed.data(); } }, Qt::DirectConnection);
		const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });
		finalize(); QVERIFY(!removed); QVERIFY(!first.controllerConnection());
		QVERIFY(middle.controllerConnection()); QVERIFY(last.controllerConnection());
		QCOMPARE(middle.controllerConnection()->getController(), controller); QCOMPARE(last.controllerConnection()->getController(), controller);
		QCOMPARE(controller->connectionCount(), 2); QVERIFY(!Engine::getSong()->hasErrors());
	}

	void multipleExplicitDummySlotsKeepLaterProjectIndices()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); auto song = Engine::getSong(); auto track = new InstrumentTrack(song);
		rack("Unavailable zero"); rack("Unavailable one"); auto third = rack("Third survives");
		track->volumeModel()->setControllerConnection(new ControllerConnection(third));
		const auto path = directory.filePath("two-dummies.mmp"); QVERIFY(song->saveProjectFile(path)); QDomDocument document; QVERIFY(read(path, document));
		auto list = controllers(document); QVERIFY(!list.isNull()); auto first = list.firstChildElement(), second = first.nextSiblingElement(); QVERIFY(!second.isNull());
		first.setAttribute("type", 0); second.setAttribute("type", 0); QVERIFY(write(path, document)); QVERIFY(load(path));
		QCOMPARE(song->controllers().size(), std::size_t{1}); QVERIFY(instrument()); auto connection = instrument()->volumeModel()->controllerConnection(); QVERIFY(connection);
		QCOMPARE(connection->getController(), song->controllers().front()); QCOMPARE(connection->getController()->name(), QString("Third survives")); QVERIFY(song->hasErrors());
	}
};
QTEST_GUILESS_MAIN(ControllerConnectionRestoreTest)
#include "ControllerConnectionRestoreTest.moc"
