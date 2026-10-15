/* Owned archive I/O through native studio transactions. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioAgent.h"
#include "StudioRuntime.h"
#include "Track.h"

using namespace lmms;
namespace
{
QJsonObject request(const QString& id, const QString& op, const QJsonObject& args = {})
{ return {{"id", id}, {"op", op}, {"args", args}}; }
QByteArray read(const QString& path)
{ QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
bool write(const QString& path, const QByteArray& bytes)
{ QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size(); }
QJsonObject manifest(const QTemporaryDir& dir)
{ return QJsonDocument::fromJson(read(dir.filePath("session.json"))).object(); }
QJsonObject largeReceipt()
{ return request("large-receipt", "session.memory.set", {{"memory", QJsonObject{{"preference", QString(20000, 'm')}}}}); }
QJsonObject decision(const QJsonObject&, const QJsonObject&, const StudioInference::Cancelled&)
{
	return {{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"},
		{"call_id", "observe"}, {"name", "project_inspect"}, {"arguments", "{}"}}}}};
}
bool dialogue(const QTemporaryDir& dir, int count, QString& error)
{
	{ StudioRuntime studio(dir.path()); if (!studio.open(error)) { return false; } }
	QJsonArray messages;
	for (int i = 0; i < count; ++i)
	{
		messages.append(QJsonObject{{"role", i % 2 ? "assistant" : "user"}, {"content", QString::number(i)},
			{"job", "legacy-fixture"}, {"at", "2026-10-14T12:00:00Z"}});
	}
	auto state = manifest(dir); auto agent = state.value("agent").toObject(); agent.insert("transcript", messages); state.insert("agent", agent);
	return StudioAgent::validState(agent) && write(dir.filePath("session.json"), QJsonDocument(state).toJson(QJsonDocument::Compact));
}
bool setup(StudioRuntime& studio, const QString& family, QString& error)
{
	if (!studio.open(error)) { return false; }
	if (family == "receipts" || family == "conversations") { return true; }
	const auto run = [&](const QJsonObject& message) {
		const auto response = studio.process(message); if (response.value("ok").toBool()) { return true; }
		error = response.value("error").toString(); return false;
	};
	if (!run(request("track", "track.add", {{"type", "sample"}, {"name", family == "tool-results" ? QString(300000, 'p') : QString("Native archive fixture")}}))
		|| !run(request("provider", "session.provider.set", {{"role", "reasoning"}, {"configuration", QJsonObject{
			{"protocol", "responses"}, {"endpoint", "http://127.0.0.1:12345/test"}, {"model", "archive-path-fixture"}}}}))
		|| !run(request("chat", "session.chat", {{"text", "Preserve the native composition and inspect its inventory."}}))) { return false; }
	if (family == "jobs") { return run(request("cancel", "agent.cancel")); }
	studio.advanceAgent();
	if (family == "contexts") { studio.advanceAgent(); }
	if (studio.stopping()) { error = "Fixture could not prepare its native tool exchange"; return false; }
	return true;
}
QJsonObject publish(StudioRuntime& studio, const QString& family)
{
	if (family == "receipts") { return studio.process(largeReceipt()); }
	if (family == "contexts") { return studio.process(request("compact", "agent.context.compact", {{"summary", "Keep the exact native inventory."}})); }
	if (family == "jobs" || family == "conversations") { return studio.process(request("next", "session.chat", {{"text", "Continue the composition."}})); }
	return studio.advanceAgent();
}
bool seed(const QTemporaryDir& dir, const QString& family, QString& error)
{
	if (family == "conversations" && !dialogue(dir, 140, error)) { return false; }
	StudioRuntime studio(dir.path(), decision); if (!setup(studio, family, error)) { return false; }
	if (family == "conversations") { return true; }
	const auto result = publish(studio, family);
	const bool ok = family == "tool-results" ? result.value("event") == QJsonValue("agent") && !studio.stopping() : result.value("ok").toBool();
	if (!ok) { error = result.value("error").toString(); }
	return ok;
}
QString archivePath(const QJsonObject& state, const QString& family)
{
	const auto agent = state.value("agent").toObject(), job = agent.value("job").toObject();
	const auto firstPath = [](const QJsonArray& entries) { return entries.isEmpty() ? QString{} : entries.first().toObject().value("path").toString(); };
	if (family == "conversations") { return firstPath(agent.value("transcriptArchives").toArray()); }
	if (family == "contexts") { return firstPath(job.value("pastInputs").toArray()); }
	if (family == "jobs") { return firstPath(agent.value("archives").toArray()); }
	for (const auto& value : family == "receipts" ? state.value("requests").toArray() : job.value("records").toArray())
	{
		const auto path = value.toObject().value("archive").toObject().value("path").toString(); if (!path.isEmpty()) { return path; }
	}
	return {};
}
QJsonObject query(const QJsonObject& state, const QString& family, const QString& id)
{
	if (family == "receipts") { return largeReceipt(); }
	if (family == "conversations") { return request(id, "conversation.history", {{"limit", 1}}); }
	if (family == "jobs") { return request(id, "agent.history.list", {{"job", QFileInfo(archivePath(state, family)).completeBaseName()}}); }
	QJsonObject args{{"job", state.value("agent").toObject().value("job").toObject().value("id")}, {"limit", 512}};
	if (family == "contexts") { args.insert("context", 0); } else { args.insert("call", "observe"); }
	return request(id, "agent.history.read", args);
}
void pathFaultRows()
{
	QTest::addColumn<QString>("family"); QTest::addColumn<int>("fault");
	for (const auto& family : {"conversations", "tool-results", "receipts", "jobs", "contexts"})
	{
		QTest::newRow(qPrintable(QString(family) + "-target-directory")) << QString(family) << 0;
#ifndef Q_OS_WIN
		QTest::newRow(qPrintable(QString(family) + "-target-link")) << QString(family) << 1;
		QTest::newRow(qPrintable(QString(family) + "-parent-link")) << QString(family) << 2;
#endif
	}
}
bool replaceArchive(const QTemporaryDir& dir, const QTemporaryDir& outside, const QString& relative, int fault)
{
	const auto path = dir.filePath(relative), parent = QFileInfo(path).absolutePath();
	const auto target = outside.filePath(QFileInfo(path).fileName());
	if (!write(target, read(path))) { return false; }
	if (fault == 2)
	{
#ifndef Q_OS_WIN
		return QDir().rename(parent, parent + "-owned") && QFile::link(outside.path(), parent);
#else
		return false;
#endif
	}
	if (!QFile::rename(path, path + ".owned")) { return false; }
	if (fault == 0) { return QDir().mkdir(path); }
#ifndef Q_OS_WIN
	return QFile::link(target, path);
#else
	return false;
#endif
}
void removeLink(const QTemporaryDir& dir, const QString& relative, int fault)
{
#ifndef Q_OS_WIN
	if (fault == 1) { QFile::remove(dir.filePath(relative)); }
	if (fault == 2) { QFile::remove(QFileInfo(dir.filePath(relative)).absolutePath()); }
#else
	Q_UNUSED(dir); Q_UNUSED(relative); Q_UNUSED(fault);
#endif
}
}

class StudioArchivePathTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init()
	{
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->stop(); Engine::getSong()->clearProject();
		Engine::getSong()->tempoModel().setInitValue(DefaultTempo); Engine::getSong()->clearErrors();
	}
	void redirectedArchivesRefuseStartupBeforeNativeReplacement_data() { pathFaultRows(); }
	void redirectedArchivesRefuseStartupBeforeNativeReplacement()
	{
		QFETCH(QString, family); QFETCH(int, fault); QTemporaryDir dir, outside; QString error; QVERIFY2(seed(dir, family, error), qPrintable(error));
		const auto relative = archivePath(manifest(dir), family); QVERIFY(!relative.isEmpty()); const auto original = read(dir.filePath(relative)); QVERIFY(!original.isEmpty());
		const auto unlink = qScopeGuard([&] { removeLink(dir, relative, fault); }); QVERIFY(replaceArchive(dir, outside, relative, fault));
		const auto before = read(dir.filePath("session.json")); Engine::getSong()->clearProject();
		Track::create(Track::Type::Sample, Engine::getSong())->setName("Current local composition"); Engine::getSong()->setTempo(77);
		StudioRuntime failed(dir.path()); QVERIFY(!failed.open(error)); QVERIFY(error.contains(family));
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(dir.filePath("session.json")), before);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0]->name(), QString("Current local composition"));
		QCOMPARE(read(outside.filePath(QFileInfo(relative).fileName())), original);
	}
	void freshReadsRefuseRedirectedArchives_data() { pathFaultRows(); }
	void freshReadsRefuseRedirectedArchives()
	{
		QFETCH(QString, family); QFETCH(int, fault); QTemporaryDir dir, outside; QString error; QVERIFY2(seed(dir, family, error), qPrintable(error));
		StudioRuntime studio(dir.path()); QVERIFY2(studio.open(error), qPrintable(error));
		const auto state = manifest(dir); const auto relative = archivePath(state, family); QVERIFY(!relative.isEmpty());
		const auto baseline = studio.process(query(state, family, "baseline")); QVERIFY2(baseline.value("ok").toBool(), qPrintable(baseline.value("error").toString()));
		const auto before = manifest(dir); const auto original = read(dir.filePath(relative));
		const auto unlink = qScopeGuard([&] { removeLink(dir, relative, fault); }); QVERIFY(replaceArchive(dir, outside, relative, fault));
		const auto failed = studio.process(query(state, family, "fresh")); QVERIFY(!failed.value("ok").toBool()); QVERIFY(failed.value("error").toString().contains(family));
		QVERIFY(!failed.contains("result")); QVERIFY(!studio.stopping());
		const auto after = manifest(dir); QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("revision"), before.value("revision"));
		QCOMPARE(after.value("agent"), before.value("agent")); QCOMPARE(after.value("memory"), before.value("memory"));
		QCOMPARE(read(outside.filePath(QFileInfo(relative).fileName())), original);
	}
	void refusedPublicationPreservesCommittedState_data()
	{
		QTest::addColumn<QString>("family"); QTest::addColumn<bool>("link");
		for (const auto& family : {"conversations", "tool-results", "receipts", "jobs", "contexts"})
		{
			QTest::newRow(qPrintable(QString(family) + "-parent-file")) << QString(family) << false;
#ifndef Q_OS_WIN
			QTest::newRow(qPrintable(QString(family) + "-parent-link")) << QString(family) << true;
#endif
		}
	}
	void refusedPublicationPreservesCommittedState()
	{
		QFETCH(QString, family); QFETCH(bool, link); QTemporaryDir dir, outside; QString error;
		if (family == "conversations") { QVERIFY2(dialogue(dir, 128, error), qPrintable(error)); }
		StudioRuntime studio(dir.path(), decision); QVERIFY2(setup(studio, family, error), qPrintable(error));
		const auto parent = dir.filePath(family); QVERIFY(!QFileInfo::exists(parent)); QVERIFY(write(outside.filePath("keep.json"), "outside sentinel"));
		const auto unlink = qScopeGuard([&] { if (link) { QFile::remove(parent); } });
#ifndef Q_OS_WIN
		if (link) { QVERIFY(QFile::link(outside.path(), parent)); }
#endif
		if (!link) { QVERIFY(write(parent, "parent obstruction")); }
		const auto beforeBytes = read(dir.filePath("session.json")); const auto before = manifest(dir); const auto tempo = Engine::getSong()->getTempo();
		const auto failed = publish(studio, family); QVERIFY(!failed.value("ok").toBool()); QVERIFY(failed.value("error").toString().contains(family));
		const auto after = manifest(dir); QCOMPARE(after.value("agent"), before.value("agent")); QCOMPARE(after.value("memory"), before.value("memory"));
		QCOMPARE(after.value("history"), before.value("history")); QCOMPARE(after.value("revision"), before.value("revision")); QCOMPARE(Engine::getSong()->getTempo(), tempo);
		// Job publication is refused inside request preparation and may durably acknowledge its error receipt.
		if (family == "jobs") { QVERIFY(!studio.stopping()); QVERIFY(studio.process(request("next", "session.chat", {{"text", "Continue the composition."}})).value("replayed").toBool()); }
		else { QVERIFY(studio.stopping()); QCOMPARE(read(dir.filePath("session.json")), beforeBytes); }
		QCOMPARE(QDir(outside.path()).entryList(QDir::Files), QStringList{"keep.json"}); QCOMPARE(read(outside.filePath("keep.json")), QByteArray("outside sentinel"));
		if (!link) { QCOMPARE(read(parent), QByteArray("parent obstruction")); }
	}
	void occupiedToolTargetRefusesLegacyPublication_data()
	{
		QTest::addColumn<int>("fault"); QTest::newRow("target-directory") << 0;
#ifndef Q_OS_WIN
		QTest::newRow("target-link") << 1;
#endif
	}
	void occupiedToolTargetRefusesLegacyPublication()
	{
		QFETCH(int, fault); QTemporaryDir dir, outside; QString error; QVERIFY2(seed(dir, "tool-results", error), qPrintable(error));
		auto state = manifest(dir), agent = state.value("agent").toObject(), job = agent.value("job").toObject();
		const auto relative = archivePath(state, "tool-results"); QVERIFY(!relative.isEmpty()); const auto originalBytes = read(dir.filePath(relative));
		const auto original = QJsonDocument::fromJson(originalBytes).object(); QVERIFY(StudioAgent::validToolRecord(original));
		auto records = job.value("records").toArray(), input = job.value("input").toArray(); QVERIFY(!records.isEmpty()); QVERIFY(!input.isEmpty());
		records[records.size() - 1] = original; auto output = input.last().toObject(); QCOMPARE(output.value("type").toString(), QString("function_call_output"));
		output.insert("output", QString::fromUtf8(QJsonDocument(original.value("response").toObject()).toJson(QJsonDocument::Compact))); input[input.size() - 1] = output;
		job.insert("records", records); job.insert("input", input); agent.insert("job", job); state.insert("agent", agent); QVERIFY(StudioAgent::validState(agent));
		const auto before = QJsonDocument(state).toJson(QJsonDocument::Compact); QVERIFY(write(dir.filePath("session.json"), before));
		const auto unlink = qScopeGuard([&] { removeLink(dir, relative, fault); }); QVERIFY(replaceArchive(dir, outside, relative, fault));
		Engine::getSong()->clearProject(); Track::create(Track::Type::Sample, Engine::getSong())->setName("Current local composition");
		Engine::getSong()->setTempo(77); StudioRuntime failed(dir.path()); QVERIFY(!failed.open(error)); QVERIFY(error.contains("tool-results"));
		QCOMPARE(Engine::getSong()->getTempo(), 77); QCOMPARE(read(dir.filePath("session.json")), before);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1)); QCOMPARE(Engine::getSong()->tracks()[0]->name(), QString("Current local composition"));
		QCOMPARE(read(outside.filePath(QFileInfo(relative).fileName())), originalBytes); QCOMPARE(read(dir.filePath(relative + ".owned")), originalBytes);
		QVERIFY(fault == 0 ? QFileInfo(dir.filePath(relative)).isDir() : QFileInfo(dir.filePath(relative)).isSymLink());
	}
#ifndef Q_OS_WIN
	void selectedRootLinkPreservesOwnedArchiveIo()
	{
		QTemporaryDir dir, aliases; QString error; QVERIFY2(seed(dir, "receipts", error), qPrintable(error));
		const auto alias = aliases.filePath("session"); QVERIFY(QFile::link(dir.path(), alias)); const auto unlink = qScopeGuard([&] { QFile::remove(alias); });
		StudioRuntime studio(alias); QVERIFY2(studio.open(error), qPrintable(error)); QVERIFY(studio.process(largeReceipt()).value("replayed").toBool());
		auto next = largeReceipt(); next.insert("id", "next-large-receipt"); QVERIFY(studio.process(next).value("ok").toBool());
		QCOMPARE(QDir(dir.filePath("receipts")).entryList({"*.json"}, QDir::Files).size(), 2);
	}
	void retargetedRootRefusesArchiveReadAndPublication()
	{
		QTemporaryDir dir, aliases, outside; QString error; QVERIFY2(seed(dir, "receipts", error), qPrintable(error));
		const auto alias = aliases.filePath("session"); QVERIFY(QFile::link(dir.path(), alias)); const auto unlink = qScopeGuard([&] { QFile::remove(alias); });
		StudioRuntime studio(alias); QVERIFY2(studio.open(error), qPrintable(error));
		const auto relative = archivePath(manifest(dir), "receipts"); const auto original = read(dir.filePath(relative)); const auto before = read(dir.filePath("session.json"));
		QVERIFY(QDir().mkpath(outside.filePath("receipts"))); QVERIFY(write(outside.filePath(relative), original));
		QVERIFY(QFile::remove(alias)); QVERIFY(QFile::link(outside.path(), alias));
		const auto failed = studio.process(largeReceipt()); QVERIFY(!failed.value("ok").toBool()); QVERIFY(failed.value("error").toString().contains("session root"));
		QVERIFY(!failed.value("replayed").toBool()); QVERIFY(!studio.stopping()); QCOMPARE(read(dir.filePath("session.json")), before);
		QCOMPARE(read(dir.filePath(relative)), original); QCOMPARE(read(outside.filePath(relative)), original); QVERIFY(!QFileInfo::exists(outside.filePath("session.json")));
		auto next = largeReceipt(); next.insert("id", "next-large-receipt"); const auto refused = studio.process(next);
		QVERIFY(!refused.value("ok").toBool()); QVERIFY(refused.value("error").toString().contains("session root")); QVERIFY(studio.stopping());
		QCOMPARE(read(dir.filePath("session.json")), before); QCOMPARE(read(outside.filePath(relative)), original);
		QVERIFY(!QFileInfo::exists(outside.filePath("session.json"))); QCOMPARE(QDir(outside.filePath("receipts")).entryList({"*.json"}, QDir::Files).size(), 1);
	}
#endif
};
QTEST_GUILESS_MAIN(StudioArchivePathTest)
#include "StudioArchivePathTest.moc"
