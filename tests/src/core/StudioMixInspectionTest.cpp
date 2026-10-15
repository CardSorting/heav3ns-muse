/* Bounded native mixer, controller and automation inspection. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include "AudioEngine.h"
#include "AutomationClip.h"
#include "AutomationNode.h"
#include "AutomationTrack.h"
#include "ConfigManager.h"
#include "Controller.h"
#include "DetuningHelper.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Mixer.h"
#include "Note.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"
#include "Track.h"

using namespace lmms;
namespace
{
QJsonObject call(const QString& op, const QJsonObject& args = {}) { return StudioCapabilities::execute(op, args); }
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {}) { return {{"id", id}, {"op", op}, {"args", args}}; }
QByteArray encoded(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
QJsonObject manifest(const QTemporaryDir& dir)
{ QFile file(dir.filePath("session.json")); return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{}; }
void fill(AutomationClip* curve, int count, int spacing = 17)
{
	// Seed the native map once so fixture creation does not repeatedly regenerate tangents.
	auto& points = curve->getTimeMap(); points.clear();
	for (int i = 0; i < count; ++i)
	{
		const int position = i * spacing; AutomationNode node(curve, float(i % 17) / 32, float(i % 13) / 32, position);
		node.setInTangent(.125f); node.setOutTangent(-.25f); node.setLockedTangents(true); points.insert(position, node);
	}
	curve->updateLength();
}
struct Curve
{
	QString operation; QJsonObject args; AutomationClip* native; InstrumentTrack* instrument; Note* note = nullptr;
};
Curve makeCurve(const QString& kind, bool create = true)
{
	auto instrument = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
	if (kind == "global")
	{
		auto model = instrument->pitchModel(); return {"automation.global.inspect", {{"parameter", int(model->id())}},
			create ? AutomationClip::globalAutomationClip(model) : nullptr, instrument};
	}
	if (kind == "detuning")
	{
		auto clip = static_cast<MidiClip*>(instrument->createClip(0)); clip->clearNotes();
		auto note = clip->addNote(Note(TimePos(192), TimePos(0), 60));
		if (create) { note->createDetuning(); }
		return {"note.detuning.inspect", {{"track", 0}, {"clip", 0}, {"note", 0}}, create ? note->detuning()->automationClip() : nullptr, instrument, note};
	}
	auto track = Track::create(Track::Type::Automation, Engine::getSong()); auto curve = static_cast<AutomationClip*>(track->createClip(0));
	curve->addObject(instrument->pitchModel()); return {"automation.inspect", {{"track", 1}, {"clip", 0}}, curve, instrument};
}
int globalIndex(const AutomationClip* curve)
{
	const auto& clips = Engine::getSong()->globalAutomationTrack()->getClips();
	return static_cast<int>(std::distance(clips.begin(), std::find(clips.begin(), clips.end(), curve)));
}
QJsonObject decision(const QString& protocol, const QString& id, const QString& name, const QJsonObject& args)
{
	const auto arguments = QString::fromUtf8(encoded(args));
	if (protocol == "responses") { return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{
		{"type", "function_call"}, {"call_id", id}, {"name", name}, {"arguments", arguments}}}}}; }
	return {{"choices", QJsonArray{QJsonObject{{"finish_reason", "tool_calls"}, {"message", QJsonObject{
		{"role", "assistant"}, {"content", QJsonValue::Null}, {"tool_calls", QJsonArray{QJsonObject{{"id", id}, {"type", "function"},
			{"function", QJsonObject{{"name", name}, {"arguments", arguments}}}}}}}}}}}};
}
}

class StudioMixInspectionTest : public QObject
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
	void cleanup() { Engine::getSong()->clearProject(); }

	void inventoriesPreserveNativeMixerRackAndGlobalIndices_data()
	{
		QTest::addColumn<QString>("section");
		QTest::newRow("channels") << QString("channels"); QTest::newRow("routes") << QString("routes");
		QTest::newRow("controllers") << QString("controllers"); QTest::newRow("global-curves") << QString("globalAutomations");
	}
	void inventoriesPreserveNativeMixerRackAndGlobalIndices()
	{
		QFETCH(QString, section); auto mixer = Engine::mixer(); auto song = Engine::getSong();
		for (int i = 0; i < 145; ++i)
		{
			if (section == "controllers")
			{ auto c = Controller::create(Controller::ControllerType::Lfo, song); c->setName("Controller " + QString::number(i)); song->addController(c); }
			else
			{
				const int index = mixer->createChannel(); auto channel = mixer->mixerChannel(index); channel->setName("Channel " + QString::number(i));
				channel->m_volumeModel.setValue(float(i % 7) / 8);
				if (section == "globalAutomations") { fill(AutomationClip::globalAutomationClip(&channel->m_volumeModel), 5); }
			}
		}
		const auto before = StudioCapabilities::inspect(); const auto expected = before.value(section).toArray(); int offset = 0; QSet<int> seen;
		do
		{
			const auto response = call("project.inspect", {{"section", section}, {"offset", offset}, {"limit", 23}});
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto page = response.value("result").toObject();
			QVERIFY(!page.contains("container")); QCOMPARE(page.value("total").toInt(), expected.size()); const auto items = page.value(section).toArray(); QVERIFY(!items.isEmpty());
			for (const auto& value : items)
			{
				const auto item = value.toObject(); const int index = item.value("index").toInt(); QCOMPARE(index, offset++); QVERIFY(!seen.contains(index)); seen.insert(index);
				if (section == "channels")
				{
					auto native = mixer->mixerChannel(index); QCOMPARE(item.value("name").toString(), native->m_name);
					QCOMPARE(item.value("volume").toDouble(), double(native->m_volumeModel.value())); QVERIFY(!item.contains("effects"));
					QCOMPARE(item.value("parameters").toObject().value("volume").toInt(), int(native->m_volumeModel.id()));
					QCOMPARE(item.value("sendCount").toInt(), int(native->m_sends.size()));
				}
				else if (section == "routes")
				{
					auto native = mixer->m_mixerRoutes.at(index); QCOMPARE(item.value("from").toInt(), int(native->senderIndex()));
					QCOMPARE(item.value("to").toInt(), int(native->receiverIndex())); QCOMPARE(item.value("parameter").toInt(), int(native->amount()->id()));
					QCOMPARE(item.value("amount").toDouble(), double(native->amount()->value()));
				}
				else if (section == "controllers")
				{ QCOMPARE(item.value("name").toString(), song->controllers().at(index)->name()); QVERIFY(!item.contains("parameters")); QVERIFY(item.value("userWave").isObject()); }
				else
				{
					auto native = static_cast<AutomationClip*>(song->globalAutomationTrack()->getClip(index)); const auto metadata = item.value("automation").toObject();
					QCOMPARE(metadata.value("pointCount").toInt(), native->getTimeMap().size()); QVERIFY(!metadata.contains("points")); QVERIFY(!metadata.contains("targets"));
					const auto selected = call("automation.global.inspect", {{"global", index}, {"section", "summary"}});
					QVERIFY(selected.value("ok").toBool()); QCOMPARE(selected.value("result").toObject().value("pointCount"), metadata.value("pointCount"));
				}
			}
			QCOMPARE(page.value("nextOffset").toInt(), offset); QCOMPARE(page.value("hasMore").toBool(), offset < expected.size());
		} while (offset < expected.size());
		QCOMPARE(StudioCapabilities::inspect(), before); QCOMPARE(seen.size(), expected.size());
	}

	void pointPagesKeepNativeTickOrderAndCurveValues_data()
	{
		QTest::addColumn<QString>("kind"); QTest::addColumn<int>("progression");
		for (const auto kind : {QString("arrangement"), QString("global"), QString("detuning")})
			for (int p = 0; p < 3; ++p) { QTest::newRow(qPrintable(kind + QString::number(p))) << kind << p; }
	}
	void pointPagesKeepNativeTickOrderAndCurveValues()
	{
		QFETCH(QString, kind); QFETCH(int, progression); auto fixture = makeCurve(kind); fill(fixture.native, 4097);
		fixture.native->setProgressionType(static_cast<AutomationClip::ProgressionType>(progression)); fixture.native->setTension(.75f);
		// Native loaded maps can contain signed endpoint keys; paging must not add one and overflow.
		for (int tick : {std::numeric_limits<int>::min(), -17, std::numeric_limits<int>::max()})
		{ fixture.native->getTimeMap().insert(tick, AutomationNode(fixture.native, .125f, .375f, tick)); }
		const auto full = call(fixture.operation, fixture.args); QVERIFY(full.value("ok").toBool()); const auto expected = full.value("result").toObject().value("points").toArray();
		auto args = fixture.args; args.insert("section", "points"); args.insert("limit", 97); QJsonArray collected;
		for (;;)
		{
			const auto response = call(fixture.operation, args); QVERIFY(response.value("ok").toBool()); const auto page = response.value("result").toObject();
			QCOMPARE(page.value("total").toInt(), expected.size()); QVERIFY(!page.contains("targets")); QCOMPARE(page.value("progression").toInt(), progression);
			const auto points = page.value("points").toArray(); QVERIFY(!points.isEmpty()); QVERIFY(points.size() <= 97);
			for (const auto& point : points) { collected.append(point); }
			if (!page.value("hasMore").toBool()) { QVERIFY(page.value("nextPosition").isNull()); break; }
			QVERIFY(page.value("nextPosition").toDouble() > points.last().toObject().value("position").toDouble()); args.insert("fromPosition", page.value("nextPosition"));
		}
		QCOMPARE(collected, expected); QCOMPARE(call(fixture.operation, fixture.args), full);
		args.insert("fromPosition", 1); args.insert("limit", 1); const auto between = call(fixture.operation, args).value("result").toObject();
		QCOMPARE(between.value("points").toArray().at(0).toObject().value("position").toInt(), 17);
		args.insert("fromPosition", std::numeric_limits<int>::max()); int checks = 0; QJsonObject tail;
		{ FileResourceIO::Scope work([&] { ++checks; return false; }); tail = call(fixture.operation, args).value("result").toObject(); }
		QCOMPARE(tail.value("points").toArray().size(), 1); QVERIFY(!tail.value("hasMore").toBool()); QVERIFY(checks > 0 && checks < 200);
	}

	void targetsPageWithExactNativeHandlesAndReceiptReplay()
	{
		QTemporaryDir directory; StudioRuntime studio(directory.path()); QString error; QVERIFY(studio.open(error)); auto fixture = makeCurve("arrangement");
		fixture.native->removeObject(fixture.instrument->pitchModel()); std::vector<std::unique_ptr<FloatModel>> targets;
		for (int i = 0; i < 100; ++i)
		{
			targets.push_back(std::make_unique<FloatModel>(.25f, 0.f, 1.f, .001f, Engine::getSong(), QString(200000, 'x') + QString::number(i)));
			QVERIFY(fixture.native->addObject(targets.back().get()));
		}
		fill(fixture.native, 257); const auto before = manifest(directory); int offset = 0; qint64 bytes = 0;
		while (offset < 100)
		{
			auto args = fixture.args; args.insert("section", "targets"); args.insert("offset", offset);
			const auto query = request("targets-" + QString::number(offset), fixture.operation, args); const auto response = studio.process(query);
			QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString())); const auto page = response.value("result").toObject();
			const auto items = page.value("targets").toArray(); QVERIFY(!items.isEmpty()); const auto size = QJsonDocument(items).toJson(QJsonDocument::Compact).size();
			QVERIFY(size <= 512 * 1024); bytes += size; QVERIFY(!page.contains("points"));
			for (const auto& value : items)
			{
				const auto item = value.toObject(); QCOMPARE(item.value("index").toInt(), offset); QVERIFY(item.value("available").toBool());
				QCOMPARE(item.value("parameter").toInt(), int(targets.at(offset)->id())); QCOMPARE(item.value("name").toString(), targets.at(offset)->fullDisplayName()); ++offset;
			}
			QCOMPARE(page.value("nextOffset").toInt(), offset); const auto replay = studio.process(query); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("result"), response.value("result"));
		}
		QVERIFY(bytes > 16 * 1024 * 1024); QCOMPARE(manifest(directory).value("history"), before.value("history")); QCOMPARE(manifest(directory).value("revision"), before.value("revision"));
		QVERIFY(call("parameter.set", {{"parameter", int(targets.back()->id())}, {"value", .75}}).value("ok").toBool()); QCOMPARE(targets.back()->storedValue(), .75f); QVERIFY(!studio.stopping());
	}

	void oversizedNamesUsePreviewsAndControllerCreationHasCompactAcknowledgement()
	{
		auto song = Engine::getSong(); auto mixer = Engine::mixer(); auto native = Controller::create(Controller::ControllerType::Lfo, song); song->addController(native);
		native->setName(QString(600000, 'x')); mixer->mixerChannel(0)->setName(QString(600000, 'y'));
		for (const auto section : {QString("channels"), QString("controllers")})
		{
			QVERIFY(!call("project.inspect", {{"section", section}}).value("ok").toBool());
			const auto response = call("project.inspect", {{"section", section}, {"names", false}}); QVERIFY(response.value("ok").toBool());
			const auto item = response.value("result").toObject().value(section).toArray().at(0).toObject(); QVERIFY(item.value("nameTruncated").toBool()); QCOMPARE(item.value("namePreview").toString().size(), 256);
		}
		const auto added = call("controller.add", {{"type", "lfo"}, {"name", "New modulator"}, {"summary", true}});
		QVERIFY(added.value("ok").toBool()); QVERIFY(encoded(added).size() < 4096); QCOMPARE(added.value("result").toObject().value("controller").toInt(), 1); QCOMPARE(song->controllers().size(), std::size_t(2));
		auto fixture = makeCurve("arrangement"); FloatModel large(.25f, 0.f, 1.f, .01f, song, QString(600000, 'z'));
		fixture.native->removeObject(fixture.instrument->pitchModel()); fixture.native->addObject(&large);
		auto args = fixture.args; args.insert("section", "targets"); QVERIFY(!call(fixture.operation, args).value("ok").toBool());
		args.insert("names", false); const auto response = call(fixture.operation, args); QVERIFY(response.value("ok").toBool());
		const auto target = response.value("result").toObject().value("targets").toArray().at(0).toObject(); QVERIFY(target.value("nameTruncated").toBool()); QCOMPARE(target.value("parameter").toInt(), int(large.id()));
	}

	void absentCurvesStayAbsent_data()
	{
		QTest::addColumn<QString>("kind"); QTest::addColumn<QString>("section");
		for (const auto kind : {QString("global"), QString("detuning")})
			for (const auto section : {QString("full"), QString("summary"), QString("points")}) { QTest::newRow(qPrintable(kind + section)) << kind << section; }
		QTest::newRow("global-targets") << QString("global") << QString("targets");
	}
	void absentCurvesStayAbsent()
	{
		QFETCH(QString, kind); QFETCH(QString, section); auto fixture = makeCurve(kind, false); const int before = Engine::getSong()->globalAutomationTrack()->numOfClips();
		auto args = fixture.args; args.insert("section", section); args.insert("position", 17); const auto response = call(fixture.operation, args);
		QVERIFY(response.value("ok").toBool()); const auto result = response.value("result").toObject(); QVERIFY(!result.value("hasAutomation").toBool());
		QCOMPARE(result.value("value").toDouble(), kind == "detuning" ? 0.0 : double(fixture.instrument->pitchModel()->storedValue()));
		if (section != "full") { QVERIFY(!result.value("exists").toBool()); }
		QCOMPARE(Engine::getSong()->globalAutomationTrack()->numOfClips(), before); if (fixture.note) { QVERIFY(!fixture.note->detuning()); }
	}

	void globalIndicesRetainEmptyAndUnboundCurves()
	{
		auto fixture = makeCurve("global"); fixture.native->clear(); fixture.instrument->pitchModel()->setValue(37);
		const int index = globalIndex(fixture.native); const auto before = StudioCapabilities::inspect();
		auto byParameter = fixture.args; byParameter.insert("section", "points"); byParameter.insert("position", 12);
		const auto parameterResult = call(fixture.operation, byParameter).value("result").toObject();
		const auto indexed = call(fixture.operation, {{"global", index}, {"section", "points"}, {"position", 12}});
		QVERIFY(indexed.value("ok").toBool()); const auto result = indexed.value("result").toObject();
		QVERIFY(result.value("exists").toBool()); QCOMPARE(result.value("pointCount").toInt(), 0);
		QCOMPARE(result.value("value"), parameterResult.value("value")); QCOMPARE(result.value("value").toDouble(), 37.0);
		QCOMPARE(StudioCapabilities::inspect(), before);
		QVERIFY(fixture.native->removeObject(fixture.instrument->pitchModel())); const auto unbound = StudioCapabilities::inspect();
		const auto targets = call(fixture.operation, {{"global", index}, {"section", "targets"}, {"position", 12}});
		QVERIFY(targets.value("ok").toBool()); QVERIFY(targets.value("result").toObject().value("targets").toArray().isEmpty());
		QCOMPARE(targets.value("result").toObject().value("total").toInt(), 0); QCOMPARE(StudioCapabilities::inspect(), unbound);
	}

	void invalidSelectionsCannotChangeNativeState_data()
	{
		QTest::addColumn<QString>("op"); QTest::addColumn<QJsonObject>("args");
		QTest::newRow("channel-owner") << QString("project.inspect") << QJsonObject{{"section", "channels"}, {"container", "song"}};
		QTest::newRow("route-names") << QString("project.inspect") << QJsonObject{{"section", "routes"}, {"names", false}};
		QTest::newRow("rack-track") << QString("project.inspect") << QJsonObject{{"section", "controllers"}, {"track", 0}};
		QTest::newRow("global-clip") << QString("project.inspect") << QJsonObject{{"section", "globalAutomations"}, {"clip", 0}};
		QTest::newRow("missing-global-selector") << QString("automation.global.inspect") << QJsonObject{{"section", "summary"}};
		QTest::newRow("two-global-selectors") << QString("automation.global.inspect") << QJsonObject{{"parameter", 1}, {"global", 0}};
		QTest::newRow("negative-global") << QString("automation.global.inspect") << QJsonObject{{"global", -1}};
		QTest::newRow("missing-global") << QString("automation.global.inspect") << QJsonObject{{"global", 100000}};
		QTest::newRow("unknown-section") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "unknown"}};
		QTest::newRow("full-limit") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"limit", 1}};
		QTest::newRow("summary-start") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "summary"}, {"fromPosition", 0}};
		QTest::newRow("point-offset") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "points"}, {"offset", 0}};
		QTest::newRow("point-names") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "points"}, {"names", false}};
		QTest::newRow("point-zero-limit") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "points"}, {"limit", 0}};
		QTest::newRow("point-high-limit") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "points"}, {"limit", 1025}};
		QTest::newRow("point-fractional-tick") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "points"}, {"fromPosition", .5}};
		QTest::newRow("point-overflow-tick") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "points"}, {"fromPosition", 2147483648.0}};
		QTest::newRow("target-start") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "targets"}, {"fromPosition", 0}};
		QTest::newRow("target-negative-offset") << QString("automation.global.inspect") << QJsonObject{{"global", 0}, {"section", "targets"}, {"offset", -1}};
		QTest::newRow("detuning-targets") << QString("note.detuning.inspect") << QJsonObject{{"track", 0}, {"clip", 0}, {"note", 0}, {"section", "targets"}};
		QTest::newRow("creation-summary-type") << QString("controller.add") << QJsonObject{{"type", "lfo"}, {"summary", 1}};
	}
	void invalidSelectionsCannotChangeNativeState()
	{
		QFETCH(QString, op); QFETCH(QJsonObject, args); auto fixture = makeCurve("detuning"); fill(fixture.native, 17); const auto before = StudioCapabilities::inspect();
		const auto response = call(op, args); QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QCOMPARE(StudioCapabilities::inspect(), before);
	}

	void cancellationDoesNotPublishPartialStateOrDispatchEvents_data()
	{
		QTest::addColumn<QString>("section");
		for (const auto section : {QString("channels"), QString("routes"), QString("controllers"), QString("globalAutomations"), QString("points"), QString("targets")})
		{ QTest::newRow(qPrintable(section)) << section; }
	}
	void cancellationDoesNotPublishPartialStateOrDispatchEvents()
	{
		QFETCH(QString, section); auto fixture = makeCurve("arrangement"); fill(fixture.native, 5001);
		for (int i = 0; i < 40; ++i)
		{
			const int index = Engine::mixer()->createChannel(); auto model = &Engine::mixer()->mixerChannel(index)->m_volumeModel;
			fixture.native->addObject(model); AutomationClip::globalAutomationClip(model);
			Engine::getSong()->addController(Controller::create(Controller::ControllerType::Lfo, Engine::getSong()));
		}
		const auto before = StudioCapabilities::inspect(); bool eventRan = false; QObject owner; QTimer::singleShot(0, &owner, [&] { eventRan = true; });
		int checks = 0; QJsonObject response;
		{
			FileResourceIO::Scope work([&] { return ++checks >= 8; }, true);
			auto args = section == "points" || section == "targets" ? fixture.args : QJsonObject{}; args.insert("section", section);
			response = call(section == "points" || section == "targets" ? fixture.operation : "project.inspect", args);
		}
		QVERIFY(!response.value("ok").toBool()); QVERIFY(!response.contains("result")); QVERIFY(response.value("error").toString().contains("cancelled")); QVERIFY(!eventRan);
		QCOMPARE(StudioCapabilities::inspect(), before); QCoreApplication::processEvents(); QVERIFY(eventRan);
	}

	void compactCurveEditSurvivesOversizedLegacyResultUndoReplayAndRecovery()
	{
		QTemporaryDir directory; QString error; constexpr int Count = 200001; const int removed = (Count - 1) * 2;
		{
			StudioRuntime studio(directory.path()); QVERIFY(studio.open(error)); auto fixture = makeCurve("global"); fill(fixture.native, Count, 2);
			{ const auto legacy = call(fixture.operation, fixture.args); QVERIFY(legacy.value("ok").toBool()); QVERIFY(encoded(legacy).size() > 16 * 1024 * 1024); }
			QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
			auto args = fixture.args; args.insert("position", removed); args.insert("summary", true); const auto edit = request("remove", "automation.global.removePoint", args);
			const auto response = studio.process(edit); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			QVERIFY(encoded(response).size() < 4096); QCOMPARE(response.value("result").toObject().value("pointCount").toInt(), Count - 1); QVERIFY(!fixture.native->getTimeMap().contains(removed));
			const auto after = manifest(directory); QVERIFY(studio.process(edit).value("replayed").toBool()); QCOMPARE(manifest(directory), after);
			QVERIFY(studio.process(request("undo", "session.undo", {{"summary", true}})).value("ok").toBool());
			auto restored = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
			QVERIFY(AutomationClip::globalAutomationClip(restored->pitchModel())->getTimeMap().contains(removed));
			QVERIFY(studio.process(edit).value("replayed").toBool()); QVERIFY(AutomationClip::globalAutomationClip(restored->pitchModel())->getTimeMap().contains(removed));
			QVERIFY(studio.process(request("redo", "session.redo", {{"summary", true}})).value("ok").toBool());
		}
		StudioRuntime recovered(directory.path()); QVERIFY2(recovered.open(error), qPrintable(error)); auto instrument = static_cast<InstrumentTrack*>(Engine::getSong()->tracks().at(0));
		auto curve = AutomationClip::globalAutomationClip(instrument->pitchModel()); QCOMPARE(curve->getTimeMap().size(), Count - 1); QVERIFY(!curve->getTimeMap().contains(removed));
		const auto summary = recovered.process(request("summary", "automation.global.inspect", {{"global", globalIndex(curve)}, {"section", "summary"}}));
		QVERIFY(summary.value("ok").toBool()); QCOMPARE(summary.value("result").toObject().value("pointCount").toInt(), Count - 1);
		QVERIFY(recovered.process(request("continue", "automation.global.point", {{"parameter", int(instrument->pitchModel()->id())}, {"position", removed}, {"value", .75}})).value("ok").toBool());
		QCOMPARE(curve->valueAt(removed), .75f); QVERIFY(!recovered.stopping());
	}

	void historySummaryArgumentsValidateBeforeRestoration()
	{
		QTemporaryDir directory; StudioRuntime studio(directory.path()); QString error; QVERIFY(studio.open(error));
		QVERIFY(studio.process(request("one", "song.set", {{"tempo", 101}})).value("ok").toBool());
		QVERIFY(studio.process(request("two", "song.set", {{"tempo", 102}})).value("ok").toBool());
		int id = 0;
		for (const auto op : {QString("session.undo"), QString("session.redo")})
		{
			if (op == "session.redo") { QVERIFY(studio.process(request("valid-undo", "session.undo", {{"summary", true}})).value("ok").toBool()); }
			const auto before = manifest(directory); const auto native = StudioCapabilities::inspect();
			for (const auto args : {QJsonObject{{"summary", 1}}, QJsonObject{{"summary", true}, {"unexpected", true}}})
			{
				const auto response = studio.process(request("invalid-" + QString::number(++id), op, args)); QVERIFY(!response.value("ok").toBool());
				QCOMPARE(StudioCapabilities::inspect(), native); QCOMPARE(manifest(directory).value("cursor"), before.value("cursor"));
				QCOMPARE(manifest(directory).value("revision"), before.value("revision")); QVERIFY(!studio.stopping());
			}
		}
		const auto full = studio.process(request("legacy-redo", "session.redo", {{"summary", false}})); QVERIFY(full.value("ok").toBool());
		QCOMPARE(full.value("result").toObject(), StudioCapabilities::inspect());
	}

	void compactMutationResponsesPreserveNativeCurveChanges_data()
	{
		QTest::addColumn<QString>("kind"); QTest::newRow("global") << QString("global"); QTest::newRow("detuning") << QString("detuning"); QTest::newRow("arrangement") << QString("arrangement");
	}
	void compactMutationResponsesPreserveNativeCurveChanges()
	{
		QFETCH(QString, kind); auto fixture = makeCurve(kind); fill(fixture.native, 4097); auto args = fixture.args; args.insert("summary", true);
		const auto prefix = fixture.operation.left(fixture.operation.size() - 7);
		if (kind == "arrangement")
		{
			args.insert("parameter", int(fixture.instrument->pitchModel()->id()));
			const auto disconnected = call("automation.disconnect", args); QVERIFY(disconnected.value("ok").toBool()); QVERIFY(fixture.native->objects().empty());
			QVERIFY(encoded(disconnected).size() < 4096); const auto connected = call("automation.connect", args); QVERIFY(connected.value("ok").toBool());
			QVERIFY(encoded(connected).size() < 4096); QCOMPARE(fixture.native->getTimeMap().size(), 4097); QCOMPARE(fixture.native->objects().size(), std::size_t(1));
		}
		else
		{
			auto edit = args; edit.insert("progression", 2); edit.insert("tension", .5); const auto changed = call(prefix + "edit", edit);
			QVERIFY(changed.value("ok").toBool()); QVERIFY(encoded(changed).size() < 4096); QCOMPARE(int(fixture.native->progressionType()), 2);
			auto remove = args; remove.insert("position", 17); const auto removed = call(prefix + "removePoint", remove);
			QVERIFY(removed.value("ok").toBool()); QVERIFY(encoded(removed).size() < 4096); QVERIFY(!fixture.native->getTimeMap().contains(17));
			const auto cleared = call(prefix + "clear", args); QVERIFY(cleared.value("ok").toBool()); QVERIFY(encoded(cleared).size() < 4096); QVERIFY(fixture.native->getTimeMap().isEmpty());
		}
	}

	void nativeAgentDiscoversMixerAutomationAndRevisesIt_data()
	{ QTest::addColumn<QString>("protocol"); QTest::newRow("chat") << QString("chat"); QTest::newRow("responses") << QString("responses"); }
	void nativeAgentDiscoversMixerAutomationAndRevisesIt()
	{
		QFETCH(QString, protocol); QTemporaryDir directory; QString error; int round = 0, parameter = -1, position = -1; double original = 0, revised = 0; bool historyCorrect = true;
		StudioRuntime studio(directory.path(), [&](const QJsonObject&, const QJsonObject& body, const StudioInference::Cancelled&) {
			QJsonObject output; for (const auto& value : body.value(protocol == "chat" ? "messages" : "input").toArray())
			{
				const auto item = value.toObject(); if (item.value("role").toString() == "tool" || item.value("type").toString() == "function_call_output")
				{ output = QJsonDocument::fromJson(item.value(protocol == "chat" ? "content" : "output").toString().toUtf8()).object(); }
			}
			if (++round == 1) { return decision(protocol, "channel", "project_inspect", {{"section", "channels"}, {"offset", 1}, {"limit", 1}, {"names", false}}); }
			if (round == 2 || round == 6 || round == 9)
			{
				const auto channels = output.value("result").toObject().value("channels").toArray();
				if (!output.value("ok").toBool() || channels.size() != 1) { throw std::runtime_error("Native mixer discovery failed"); }
				parameter = channels.at(0).toObject().value("parameters").toObject().value("volume").toInt(-1);
				return decision(protocol, round == 2 ? "curve" : round == 6 ? "curve_before" : "curve_after", "studio_execute", {{"op", "automation.global.inspect"},
					{"args", QJsonObject{{"parameter", parameter}, {"section", "points"}, {"fromPosition", 17408}, {"limit", 1}}}});
			}
			if (round == 3)
			{
				const auto points = output.value("result").toObject().value("points").toArray();
				if (!output.value("ok").toBool() || points.size() != 1) { throw std::runtime_error("Native curve discovery failed"); }
				const auto point = points.at(0).toObject(); position = point.value("position").toInt(); original = point.value("value").toDouble(); revised = original + .125;
				return decision(protocol, "revise", "studio_execute", {{"op", "automation.global.point"}, {"args", QJsonObject{{"parameter", parameter}, {"position", position}, {"value", revised}}}});
			}
			if (round == 4) { return decision(protocol, "undo", "session_undo", {{"summary", true}}); }
			if (round == 5 || round == 8)
			{
				historyCorrect = historyCorrect && output.value("ok").toBool() && output.value("result").toObject().value("inspection").toString() == "summary";
				return decision(protocol, round == 5 ? "channel_before" : "channel_after", "project_inspect", {{"section", "channels"}, {"offset", 1}, {"limit", 1}, {"names", false}});
			}
			const auto points = output.value("result").toObject().value("points").toArray();
			if (!output.value("ok").toBool() || points.size() != 1) { throw std::runtime_error("Native restored curve discovery failed"); }
			const auto observed = points.at(0).toObject().value("value").toDouble();
			historyCorrect = historyCorrect && output.value("ok").toBool() && observed == (round == 7 ? original : revised);
			if (round == 7) { return decision(protocol, "redo", "session_redo", {{"summary", true}}); }
			return decision(protocol, "finish", "agent_finish", {{"status", "complete"}, {"summary", "Discovered, revised and restored native mixer automation."}, {"evidence", QJsonArray{"channel", "curve", "revise", "undo", "redo", "curve_after"}}});
		});
		QVERIFY(studio.open(error)); const int index = Engine::mixer()->createChannel(); auto channel = Engine::mixer()->mixerChannel(index); channel->setName(QString(300000, 'x'));
		auto curve = AutomationClip::globalAutomationClip(&channel->m_volumeModel); fill(curve, 5001);
		QVERIFY(studio.process(request("baseline", "song.set", {{"tempo", 123}})).value("ok").toBool());
		QVERIFY(studio.process(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
			{"protocol", protocol}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "native-mix-inspection-fixture"}}}})).value("ok").toBool());
		QVERIFY(studio.process(request("chat", "session.chat", {{"text", "Inspect the mixer and revise its existing gain automation."}})).value("ok").toBool());
		for (int step = 0; step < 20; ++step) { studio.advanceAgent(); }
		QVERIFY(historyCorrect); channel = Engine::mixer()->mixerChannel(index); curve = AutomationClip::globalAutomationClip(&channel->m_volumeModel);
		QCOMPARE(parameter, int(channel->m_volumeModel.id())); QCOMPARE(position, 17408); QCOMPARE(double(curve->valueAt(position)), revised);
		QCOMPARE(manifest(directory).value("agent").toObject().value("job").toObject().value("status").toString(), QString("complete")); QVERIFY(!studio.stopping());
	}
};
QTEST_GUILESS_MAIN(StudioMixInspectionTest)
#include "StudioMixInspectionTest.moc"
