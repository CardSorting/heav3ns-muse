/* LMMS-owned durable studio session. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioRuntime.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <QDateTime>
#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QQueue>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include "AudioEngine.h"
#include "Engine.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioAgent.h"
#include "StudioAudio.h"
#include "StudioStdio.h"
#include "StudioAssetStore.h"
#include "StudioStorage.h"
#include "StudioResources.h"
#include "DataFile.h"
#include "FileResourceIO.h"

namespace lmms
{
namespace
{

constexpr int MaxInputBytes = 1024 * 1024;
constexpr int MaxManifestBytes = 16 * 1024 * 1024;
constexpr int MaxHistory = 256;
constexpr int MaxRequests = 128;
constexpr int MaxInlineReceiptBytes = 8192;
constexpr int MaxInlineToolResponseBytes = 256 * 1024;
constexpr int ActiveToolArchiveThresholdBytes = 4 * 1024 * 1024;
constexpr int MaxRecentTranscriptBytes = 256 * 1024;
constexpr int TranscriptPageBytes = 1024 * 1024;

// Every studio uses the same native Engine. Directory locks alone do not keep
// two in-process sessions from replacing each other's authoritative project.
std::atomic<StudioRuntime*> engineSession{nullptr};

QString digestFile(const QString& path)
{
	QString checksum, error;
	return FileResourceIO::hashFile(path, checksum, error) ? checksum : QString{};
}

QJsonObject failure(const QString& message)
{
	return {{"ok", false}, {"error", message}};
}

bool onApplicationThread()
{
	const auto application = QCoreApplication::instance();
	return application && QThread::currentThread() == application->thread();
}

bool nonnegativeInteger(const QJsonValue& value)
{
	return value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= 2147483646
		&& value.toDouble() == value.toInt(-1);
}

int historyInteger(const QJsonObject& arguments, const QString& key, int fallback, int minimum, int maximum)
{
	if (!arguments.contains(key)) { return fallback; }
	const auto value = arguments.value(key);
	if (!nonnegativeInteger(value) || value.toInt() < minimum || value.toInt() > maximum)
	{
		throw std::invalid_argument(("Invalid history " + key).toStdString());
	}
	return value.toInt();
}

QString historyPreview(const QString& text)
{
	auto prefix = text.left(1024);
	if (prefix.size() < text.size() && !prefix.isEmpty() && prefix.at(prefix.size() - 1).isHighSurrogate()) { prefix.chop(1); }
	auto bytes = prefix.toUtf8();
	if (bytes.size() <= 1024) { return prefix; }
	int end = 1024;
	while ((static_cast<unsigned char>(bytes.at(end)) & 0xc0) == 0x80) { --end; }
	return QString::fromUtf8(bytes.constData(), end);
}

bool readHistoryArchive(const QString& path, const QString& expectedDigest, QByteArray& bytes,
	QJsonObject& object, QString& error)
{
	bytes.clear(); object = {};
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > MaxManifestBytes)
	{
		error = "History archive is missing or exceeds the 16 MiB limit"; return false;
	}
	QBuffer output(&bytes); output.open(QIODevice::WriteOnly); QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!FileResourceIO::stream(file, &output, &hash, file.size(), error)) { bytes.clear(); return false; }
	const auto document = QJsonDocument::fromJson(bytes);
	if (QString::fromLatin1(hash.result().toHex()) != expectedDigest || !document.isObject()
		|| QString::fromUtf8(bytes).toUtf8() != bytes)
	{
		bytes.clear(); error = "History archive is altered or invalid"; return false;
	}
	object = document.object(); error.clear(); return true;
}

bool validCheckpointReference(const QJsonObject& snapshot)
{
	static const QRegularExpression validPath(
		"^checkpoints/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.mmp$");
	static const QRegularExpression validHash("^[0-9a-f]{64}$");
	const bool assets = snapshot.contains("assets");
	return snapshot.size() == (assets ? 3 : 2) && validPath.match(snapshot.value("path").toString()).hasMatch()
		&& validHash.match(snapshot.value("sha256").toString()).hasMatch()
		&& (!assets || (snapshot.value("assets").isArray() && StudioAssetStore::validReferences(snapshot.value("assets").toArray())));
}

bool validLedger(const QJsonArray& entries, int revision)
{
	if (entries.size() > MaxRequests) { return false; }
	QSet<QString> ids;
	for (const auto& entry : entries)
	{
		if (!entry.isObject()) { return false; }
		const auto cached = entry.toObject();
		const auto id = cached.value("id").toString();
		if (id.isEmpty() || id.size() > 128 || ids.contains(id)) { return false; }
		if (cached.contains("archive"))
		{
			static const QRegularExpression path("^receipts/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.json$");
			static const QRegularExpression digest("^[0-9a-f]{64}$");
			const auto archive = cached.value("archive").toObject();
			if (cached.size() != 3 || archive.size() != 3 || !path.match(archive.value("path").toString()).hasMatch()
				|| !digest.match(archive.value("sha256").toString()).hasMatch() || !digest.match(cached.value("sha256").toString()).hasMatch()
				|| !nonnegativeInteger(archive.value("bytes")) || archive.value("bytes").toInt() > MaxManifestBytes) { return false; }
			ids.insert(id); continue;
		}
		const auto request = cached.value("request").toObject();
		const auto response = cached.value("response").toObject();
		const auto hash = QString::fromLatin1(QCryptographicHash::hash(
			QJsonDocument(request).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
		if (cached.size() != 4
			|| !cached.value("request").isObject() || !cached.value("response").isObject()
			|| request.value("id").toString() != id || response.value("id").toString() != id
			|| request.value("op").toString().isEmpty() || !request.value("args").isObject()
			|| !response.value("ok").isBool() || !nonnegativeInteger(response.value("revision"))
			|| response.value("revision").toInt() > revision || cached.value("sha256").toString() != hash)
		{
			return false;
		}
		ids.insert(id);
	}
	return true;
}

} // namespace

StudioRuntime::StudioRuntime(const QString& sessionDirectory, const StudioInference::Transport& transport, std::FILE* outputStream,
	const StudioInference::Cancelled& interrupted, const std::function<QJsonObject()>& resourceSampler)
	: m_directory(QFileInfo(sessionDirectory).absoluteFilePath()), m_transport(transport), m_outputStream(outputStream), m_interrupted(interrupted), m_resourceSampler(resourceSampler)
{
}

QJsonObject StudioRuntime::sampleResources() const noexcept
{
	try { return m_resourceSampler ? m_resourceSampler() : StudioResources::sample(); }
	catch (...) { return {}; }
}

void StudioRuntime::output(const QJsonObject& message)
{
	const auto bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
	if (!m_outputStream || std::fwrite(bytes.constData(), 1, static_cast<std::size_t>(bytes.size()), m_outputStream)
		!= static_cast<std::size_t>(bytes.size()) || std::fflush(m_outputStream) != 0)
	{
		std::fprintf(stderr, "Native studio protocol output failed\n"); m_failed = m_stopping = true;
	}
}

StudioRuntime::~StudioRuntime()
{
	auto expected = this;
	engineSession.compare_exchange_strong(expected, nullptr);
}

QJsonObject StudioRuntime::checkpoint()
{
	const QString relative = "checkpoints/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".mmp";
	const QString path = m_directory.filePath(relative);
	auto guard = Engine::audioEngine()->requestChangesGuard();
	QJsonArray assets;
	if (!Engine::getSong()->saveProjectFile(path, false, true, &assets))
	{
		throw std::runtime_error("Could not save the native project checkpoint");
	}
	const auto hash = digestFile(path);
	if (hash.isEmpty()) { throw std::runtime_error("Could not verify the native checkpoint"); }
	return {{"path", relative}, {"sha256", hash}, {"assets", assets}};
}

bool StudioRuntime::validateCheckpoint(const QJsonObject& snapshot, QString& error) const
{
	const auto relative = snapshot.value("path").toString();
	const auto hash = snapshot.value("sha256").toString();
	if (!validCheckpointReference(snapshot)
		|| digestFile(m_directory.filePath(relative)) != hash)
	{
		error = "Checkpoint reference or digest is invalid; existing session was preserved";
		return false;
	}
	if (snapshot.contains("assets"))
	{
		const auto assets = snapshot.value("assets").toArray();
		DataFile project(m_directory.filePath(relative));
		if (!project.matchesAssetReferences(assets))
		{
			error = "Checkpoint resource paths do not match its asset manifest; existing project was preserved"; return false;
		}
		if (!StudioAssetStore::verify(m_directory.filePath("checkpoints"), assets, error)) { return false; }
	}
	error.clear(); return true;
}

bool StudioRuntime::restore(const QJsonObject& snapshot, QString& error, bool* loadAttempted)
{
	if (loadAttempted) { *loadAttempted = false; }
	try
	{
		if (!validateCheckpoint(snapshot, error)) { return false; }
		if (loadAttempted) { *loadAttempted = true; }
		if (!Engine::getSong()->loadProject(m_directory.filePath(snapshot.value("path").toString())))
		{
			error = "LMMS rejected the native session checkpoint"; return false;
		}
		if (Engine::getSong()->hasFatalErrors())
		{
			error = "Native session checkpoint did not restore completely: " + Engine::getSong()->errorSummary(); return false;
		}
		error.clear(); return true;
	}
	catch (const std::exception& exception)
	{
		error = "Native checkpoint restoration threw: " + QString::fromUtf8(exception.what());
	}
	catch (...) { error = "Native checkpoint restoration threw a nonstandard C++ exception"; }
	return false;
}

bool StudioRuntime::recoverNativeProject(const QJsonObject& snapshot, QString& error)
{
	// Cancellation stops new work, but must not prevent mandatory native rollback.
	FileResourceIO::Suspension recovery;
	return restore(snapshot, error);
}

QString StudioRuntime::archiveFilePath(const QString& relative, bool createParent, QString& error) const
{
	// Match committed archive ownership before opening a payload or creating its parent.
	// Canonical path checks do not make concurrent external replacement race-free.
	static const QRegularExpression shape("^(conversations|tool-results|receipts|jobs|contexts)/"
		"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.json\\z");
	const auto match = shape.match(relative);
	if (!match.hasMatch()) { error = "Invalid owned archive path"; return {}; }
	const auto parent = match.captured(1);
	if (m_canonicalDirectory.isEmpty() || QFileInfo(m_directory.absolutePath()).canonicalFilePath() != m_canonicalDirectory)
	{ error = parent + " archive session root changed or is unavailable"; return {}; }
	const auto redirected = [](const QFileInfo& info)
	{
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
		if (info.isJunction()) { return true; }
#endif
		return info.isSymLink();
	};
	const QFileInfo before(m_directory.filePath(parent));
	if (redirected(before)) { error = parent + " archive directory is redirected"; return {}; }
	if (createParent && !m_directory.mkpath(parent)) { error = "Could not create " + parent + " archive directory"; return {}; }
	const QFileInfo directory(m_directory.filePath(parent));
	if (redirected(directory) || !directory.isDir()
		|| directory.canonicalFilePath() != QDir::cleanPath(m_canonicalDirectory + "/" + parent))
	{ error = parent + " archive directory is outside its session or unavailable"; return {}; }
	const auto path = m_directory.filePath(relative); const QFileInfo target(path);
	if (redirected(target) || (target.exists() && (!target.isFile()
		|| target.canonicalFilePath() != QDir::cleanPath(m_canonicalDirectory + "/" + relative))))
	{ error = parent + " archive target is redirected or is not a regular owned file"; return {}; }
	error.clear(); return path;
}

bool StudioRuntime::commit(const QJsonObject& state, QString& error)
{
	try
	{
		auto prepared = state;
		if (!archiveToolResults(prepared, error)) { return false; }
		if (!archiveTranscript(prepared, error)) { return false; }
		if (!archiveReceipts(prepared, error)) { return false; }
		if (!StudioStorage::prepare(m_state, prepared, m_collectedFiles, error)) { return false; }
		const auto bytes = QJsonDocument(prepared).toJson(QJsonDocument::Compact);
		if (bytes.size() > MaxManifestBytes) { error = "Session metadata exceeds the 16 MiB limit"; return false; }
		QSaveFile file(m_directory.filePath("session.json"));
		if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
		{
			error = "Could not commit session manifest: " + file.errorString(); return false;
		}
		m_state = prepared; m_collectedFiles.clear();
		collectRetiredFiles(); error.clear(); return true;
	}
	catch (const std::exception& exception) { error = "Session commit threw: " + QString::fromUtf8(exception.what()); }
	catch (...) { error = "Session commit threw a nonstandard C++ exception"; }
	return false;
}

void StudioRuntime::collectRetiredFiles() noexcept
{
	try
	{
		m_storageReport = StudioStorage::collect(m_directory, m_canonicalDirectory, m_state,
			m_collectedFiles, m_storageCursor, [&] { return m_stopping || interruptionRequested(); });
	}
	catch (...)
	{
		// Cleanup cannot turn an already-published commit into an unacknowledged
		// operation. The durable queue remains available for a later retry.
		m_storageReport = {};
	}
}

bool StudioRuntime::archiveTranscript(QJsonObject& state, QString& error)
{
	if (!state.contains("agent")) { return true; }
	auto agent = state.value("agent").toObject();
	if (!StudioAgent::validState(agent)) { error = "Cannot publish an invalid conversation state"; return false; }
	const auto transcript = agent.value("transcript").toArray();
	if (transcript.size() <= 128 && QJsonDocument(transcript).toJson(QJsonDocument::Compact).size() <= MaxRecentTranscriptBytes) { return true; }
	int keep = 0, recentBytes = 2;
	for (int index = transcript.size() - 1; index >= 0 && keep < 64; --index)
	{
		const int bytes = QJsonDocument(transcript.at(index).toObject()).toJson(QJsonDocument::Compact).size() + 1;
		// Retain the newest message intact even when it alone exceeds the target.
		if (keep && recentBytes + bytes > MaxRecentTranscriptBytes) { break; }
		recentBytes += bytes; ++keep;
	}
	const int remove = transcript.size() - keep;
	if (!remove) { return true; }
	auto archives = agent.value("transcriptArchives").toArray(); int offset = agent.value("transcriptOffset").toInt();
	for (int start = 0; start < remove;)
	{
		QJsonArray messages;
		while (start < remove && messages.size() < 64) { messages.append(transcript.at(start++)); }
		const QJsonObject document{{"format", 1}, {"offset", offset}, {"messages", messages}};
		const auto bytes = QJsonDocument(document).toJson(QJsonDocument::Compact);
		if (bytes.size() > MaxManifestBytes) { error = "Conversation archive exceeds the 16 MiB limit"; return false; }
		const auto path = "conversations/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
		const auto filename = archiveFilePath(path, true, error); if (filename.isEmpty()) { return false; }
		QSaveFile file(filename);
		if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
		{
			error = "Could not publish the conversation archive: " + file.errorString(); return false;
		}
		archives.append(QJsonObject{{"path", path}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
			{"offset", offset}, {"count", messages.size()}});
		offset += messages.size();
	}
	QJsonArray recent;
	for (int index = remove; index < transcript.size(); ++index) { recent.append(transcript.at(index)); }
	agent.insert("transcript", recent); agent.insert("transcriptOffset", offset); agent.insert("transcriptArchives", archives);
	state.insert("agent", agent); error.clear(); return true;
}

bool StudioRuntime::readTranscript(const QJsonObject& reference, QJsonArray& messages, QString& error) const
{
	messages = {};
	if (!StudioAgent::validTranscriptArchive(reference)) { error = "Invalid conversation archive reference"; return false; }
	QJsonObject document; QByteArray bytes;
	const auto filename = archiveFilePath(reference.value("path").toString(), false, error); if (filename.isEmpty()) { return false; }
	if (!readHistoryArchive(filename, reference.value("sha256").toString(), bytes, document, error))
	{
		error = "Conversation archive: " + error; return false;
	}
	const auto items = document.value("messages").toArray();
	if (document.size() != 3 || document.value("format") != QJsonValue(1) || document.value("offset") != reference.value("offset")
		|| !document.value("messages").isArray() || items.size() != reference.value("count").toInt())
	{
		error = "Conversation archive contents are inconsistent"; return false;
	}
	for (const auto& item : items)
	{
		if (!item.isObject() || !StudioAgent::validTranscriptMessage(item.toObject())) { error = "Conversation archive contains an invalid message"; return false; }
	}
	messages = items; error.clear(); return true;
}

QJsonObject StudioRuntime::conversationHistory(const QJsonObject& arguments) const
{
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
	{
		if (it.key() != "offset" && it.key() != "limit") { throw std::invalid_argument("Unknown conversation history argument"); }
	}
	const int offset = historyInteger(arguments, "offset", 0, 0, 2147483646), limit = historyInteger(arguments, "limit", 64, 1, 256);
	const auto agent = m_state.value("agent").toObject(); const auto recent = agent.value("transcript").toArray();
	const int recentOffset = agent.value("transcriptOffset").toInt(), total = recentOffset + recent.size();
	QJsonArray messages; int messageBytes = 2;
	const auto append = [&](const QJsonValue& message) {
		const int bytes = QJsonDocument(message.toObject()).toJson(QJsonDocument::Compact).size() + 1;
		if (messages.size() >= limit || (!messages.isEmpty() && messageBytes + bytes > TranscriptPageBytes)) { return false; }
		messages.append(message); messageBytes += bytes; return true;
	};
	bool full = false;
	for (const auto& value : agent.value("transcriptArchives").toArray())
	{
		const auto reference = value.toObject(); const int start = reference.value("offset").toInt(), end = start + reference.value("count").toInt();
		if (end <= offset) { continue; }
		if (messages.size() >= limit || messageBytes >= TranscriptPageBytes) { full = true; break; }
		QJsonArray archived; QString error;
		if (!readTranscript(reference, archived, error)) { throw std::runtime_error(error.toStdString()); }
		for (int index = std::max(0, offset - start); index < archived.size(); ++index)
		{
			if (!append(archived.at(index))) { full = true; break; }
		}
		if (full) { break; }
	}
	if (!full)
	{
		for (int index = std::max(0, offset - recentOffset); index < recent.size(); ++index) { if (!append(recent.at(index))) { break; } }
	}
	return {{"messages", messages}, {"total", total}, {"offset", offset}, {"nextOffset", offset + messages.size()}, {"complete", offset + messages.size() >= total}};
}

bool StudioRuntime::archiveToolResults(QJsonObject& state, QString& error)
{
	// Checked reuse reads must not dispatch nested requests while a manifest
	// snapshot and its provider projections are being prepared.
	const FileResourceIO::EventSuspension noReentry;
	// As with atomic publication, a checked reuse read belongs to mandatory
	// metadata durability, not an interruptible native operation callback.
	const FileResourceIO::Suspension publication;
	auto agent = state.value("agent").toObject(); auto job = agent.value("job").toObject();
	if (job.isEmpty()) { error.clear(); return true; }
	if (!StudioAgent::validState(agent)) { error = "Cannot preserve invalid native agent tool results"; return false; }
	auto input = job.value("input").toArray(), records = job.value("records").toArray();
	struct OutputReference { int index; QString field; };
	QHash<QString, std::vector<OutputReference>> outputs;
	for (int index = 0; index < input.size(); ++index)
	{
		const auto item = input.at(index).toObject();
		if (item.value("type").toString() == "function_call_output")
		{ outputs[item.value("call_id").toString()].push_back({index, "output"}); }
		else if (item.value("role").toString() == "tool")
		{ outputs[item.value("tool_call_id").toString()].push_back({index, "content"}); }
	}
	// Check the entire inline batch before publishing any payload. A late
	// inconsistent output must not leave archives for earlier valid records.
	for (const auto& value : records)
	{
		const auto record = value.toObject();
		if (record.value("kind").toString() != "tool" || record.contains("archive")) { continue; }
		const auto call = record.value("call").toObject(), response = record.value("response").toObject();
		const auto found = outputs.constFind(call.value("id").toString());
		if (found == outputs.cend()) { continue; }
		for (const auto& output : found.value())
		{
			const auto previous = QJsonDocument::fromJson(input.at(output.index).toObject().value(output.field).toString().toUtf8());
			if (!previous.isObject() || previous.object() != response)
			{ error = "Native tool output differs from its durable record"; return false; }
		}
	}
	const bool pressure = static_cast<qint64>(QJsonDocument(input).toJson(QJsonDocument::Compact).size())
		+ QJsonDocument(records).toJson(QJsonDocument::Compact).size() > ActiveToolArchiveThresholdBytes;
	int lastTool = -1;
	for (int i = 0; i < records.size(); ++i) { if (records.at(i).toObject().value("kind").toString() == "tool") { lastTool = i; } }
	bool changed = false;
	for (int i = 0; i < records.size(); ++i)
	{
		auto record = records.at(i).toObject();
		if (record.value("kind").toString() != "tool" || record.contains("archive")) { continue; }
		const auto response = record.value("response").toObject();
		const auto encoded = QJsonDocument(response).toJson(QJsonDocument::Compact);
		// Keep a concise latest output directly usable, especially history chunks.
		// Under pressure, older moderate outputs also leave the active exchange.
		if (encoded.size() <= MaxInlineToolResponseBytes && (!pressure || i == lastTool || encoded.size() <= 8192)) { continue; }
		const auto bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
		if (bytes.size() > MaxManifestBytes) { error = "Native tool record exceeds the 16 MiB archive limit"; return false; }
		const auto digestBytes = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
		const auto digest = QString::fromLatin1(digestBytes.toHex());
		const auto path = "tool-results/" + QUuid::fromRfc4122(digestBytes.left(16)).toString(QUuid::WithoutBraces) + ".json";
		const auto filename = archiveFilePath(path, true, error); if (filename.isEmpty()) { return false; }
		const QFileInfo existing(filename);
		if (existing.exists() || existing.isSymLink())
		{
			if (existing.isSymLink() || !existing.isFile())
			{ error = "Existing native tool result is not a regular immutable file"; return false; }
			QByteArray previous; QJsonObject original;
			if (!readHistoryArchive(filename, digest, previous, original, error))
			{ error = "Existing native tool result cannot be reused safely: " + error; return false; }
			if (previous != bytes || original != record)
			{ error = "Existing native tool result has a conflicting immutable identity"; return false; }
		}
		else
		{
			QSaveFile file(filename);
			if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
			{ error = "Could not preserve the native tool result: " + file.errorString(); return false; }
		}
		const QJsonObject archive{{"path", path}, {"bytes", bytes.size()},
			{"sha256", digest}};
		const auto call = record.value("call").toObject();
		const auto stored = StudioAgent::storedToolResponse(call, response, archive, job.value("id").toString());
		const auto projected = QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact));
		const auto found = outputs.constFind(call.value("id").toString());
		if (found != outputs.cend())
		{
			for (const auto& output : found.value())
			{ auto item = input.at(output.index).toObject(); item.insert(output.field, projected); input[output.index] = item; }
		}
		record.insert("response", stored); record.insert("archive", archive); records[i] = record; changed = true;
	}
	if (changed)
	{
		job.insert("input", input); job.insert("records", records); agent.insert("job", job);
		if (!StudioAgent::validState(agent)) { error = "Stored native tool results produced invalid agent state"; return false; }
		state.insert("agent", agent);
	}
	error.clear(); return true;
}

bool StudioRuntime::readToolRecord(const QJsonObject& record, const QString& job, QByteArray& bytes, QString& error) const
{
	bytes.clear();
	if (!StudioAgent::validToolRecord(record)) { error = "Invalid native tool record"; return false; }
	if (!record.contains("archive"))
	{
		bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
		if (bytes.size() > MaxManifestBytes) { bytes.clear(); error = "Native tool record exceeds the 16 MiB limit"; return false; }
		error.clear(); return true;
	}
	const auto archive = record.value("archive").toObject(); QJsonObject original;
	if (record.value("response").toObject().value("storedResult").toObject().value("job") != QJsonValue(job))
	{ error = "Native tool result belongs to a different job"; return false; }
	const auto filename = archiveFilePath(archive.value("path").toString(), false, error); if (filename.isEmpty()) { return false; }
	if (!readHistoryArchive(filename, archive.value("sha256").toString(), bytes, original, error)) { return false; }
	if (bytes.size() != archive.value("bytes").toInt() || original.contains("archive") || !StudioAgent::validToolRecord(original)
		|| original.value("call") != record.value("call")
		|| StudioAgent::storedToolResponse(original.value("call").toObject(), original.value("response").toObject(), archive, job) != record.value("response").toObject())
	{ bytes.clear(); error = "Native tool result archive does not match its committed record"; return false; }
	error.clear(); return true;
}

bool StudioRuntime::archiveReceipts(QJsonObject& state, QString& error)
{
	auto entries = state.value("requests").toArray();
	if (!validLedger(entries, state.value("revision").toInt())) { error = "Cannot publish an invalid request ledger"; return false; }
	for (int index = 0; index < entries.size(); ++index)
	{
		const auto receipt = entries.at(index).toObject();
		if (receipt.contains("archive")) { continue; }
		const auto bytes = QJsonDocument(receipt).toJson(QJsonDocument::Compact);
		if (bytes.size() <= MaxInlineReceiptBytes) { continue; }
		if (bytes.size() > MaxManifestBytes) { error = "Request receipt exceeds the 16 MiB archive limit"; return false; }
		const auto path = "receipts/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
		const auto filename = archiveFilePath(path, true, error); if (filename.isEmpty()) { return false; }
		QSaveFile file(filename);
		// Publication is synchronous: nested control requests cannot change the
		// manifest snapshot between receipt publication and its manifest commit.
		if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
		{
			error = "Could not publish the request receipt: " + file.errorString(); return false;
		}
		entries[index] = QJsonObject{{"id", receipt.value("id")}, {"sha256", receipt.value("sha256")},
			{"archive", QJsonObject{{"path", path}, {"bytes", bytes.size()},
				{"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}}};
	}
	state.insert("requests", entries); error.clear(); return true;
}

bool StudioRuntime::readReceipt(const QJsonObject& reference, int revision, QJsonObject& receipt, QString& error) const
{
	receipt = {};
	if (!validLedger(QJsonArray{reference}, revision)) { error = "Invalid request receipt reference"; return false; }
	if (!reference.contains("archive")) { receipt = reference; error.clear(); return true; }
	const auto archive = reference.value("archive").toObject(); QByteArray bytes;
	const auto filename = archiveFilePath(archive.value("path").toString(), false, error); if (filename.isEmpty()) { return false; }
	if (!readHistoryArchive(filename, archive.value("sha256").toString(), bytes, receipt, error))
	{
		error = "Request receipt: " + error; return false;
	}
	if (bytes.size() != archive.value("bytes").toInt() || receipt.contains("archive")
		|| !validLedger(QJsonArray{receipt}, revision) || receipt.value("id") != reference.value("id")
		|| receipt.value("sha256") != reference.value("sha256"))
	{
		receipt = {}; error = "Request receipt content does not match its committed reference"; return false;
	}
	error.clear(); return true;
}

bool StudioRuntime::open(QString& error)
{
	if (!onApplicationThread()) { error = "Studio sessions must open on the application thread"; return false; }
	if (!Engine::getSong() || !Engine::audioEngine()) { error = "Native LMMS engine is not initialized"; return false; }
	if (m_open) { error = "Session is already open"; return false; }
	StudioRuntime* expected = nullptr;
	if (!engineSession.compare_exchange_strong(expected, this))
	{
		error = "Native engine is already owned by a studio session"; return false;
	}
	const auto releaseFailedOpen = qScopeGuard([this] {
		if (!m_open)
		{
			auto expected = this;
			engineSession.compare_exchange_strong(expected, nullptr);
		}
	});
	if (!m_directory.mkpath("checkpoints")) { error = "Could not create the session directory"; return false; }
	m_lock = std::make_unique<QLockFile>(m_directory.filePath("session.lock"));
	m_lock->setStaleLockTime(0);
	if (!m_lock->tryLock(0)) { error = "Studio session is locked by another process"; return false; }
	m_canonicalDirectory = m_directory.canonicalPath();
	QFile manifest(m_directory.filePath("session.json"));
	if (manifest.exists())
	{
		if (!manifest.open(QIODevice::ReadOnly)) { error = manifest.errorString(); return false; }
		QJsonParseError parse;
		const auto bytes = manifest.read(MaxManifestBytes + 1);
		const auto document = QJsonDocument::fromJson(bytes, &parse);
		auto state = document.object();
		const auto history = state.value("history").toArray();
		const int cursor = state.value("cursor").toInt(-1);
		if (bytes.size() > MaxManifestBytes || parse.error != QJsonParseError::NoError
			|| !document.isObject() || state.value("format") != QJsonValue(1)
			|| !nonnegativeInteger(state.value("revision")) || history.isEmpty() || history.size() > MaxHistory
			|| !nonnegativeInteger(state.value("cursor")) || cursor >= history.size()
			|| !state.value("memory").isObject() || !state.value("requests").isArray()
			|| !validLedger(state.value("requests").toArray(), state.value("revision").toInt()) || !StudioStorage::validRetirements(state))
		{
			error = "Session manifest is invalid or unsupported; refusing to overwrite it";
			return false;
		}
		for (const auto& entry : history)
		{
			if (!entry.isObject() || !validCheckpointReference(entry.toObject()))
			{
				error = "Session history is invalid; refusing to overwrite it";
				return false;
			}
		}
		try
		{
			for (const auto& entry : state.value("requests").toArray())
			{
				QJsonObject receipt;
				if (!readReceipt(entry.toObject(), state.value("revision").toInt(), receipt, error)) { return false; }
			}
			if (state.contains("agent") && (!state.value("agent").isObject() || !StudioAgent::validState(state.value("agent").toObject())))
			{
				error = "Persisted agent state is invalid; refusing to overwrite it";
				return false;
			}
			if (state.contains("providers") && !state.value("providers").isObject())
			{
				error = "Persisted provider configuration is invalid";
				return false;
			}
			for (const auto& reference : state.value("agent").toObject().value("transcriptArchives").toArray())
			{
				QJsonArray messages;
				if (!readTranscript(reference.toObject(), messages, error)) { return false; }
			}
			if (!validateJobContexts(state.value("agent").toObject().value("job").toObject(), error)) { return false; }
			for (const auto& value : state.value("agent").toObject().value("archives").toArray())
			{
				const auto archive = value.toObject();
				QJsonObject archived; QByteArray archivedBytes;
				if (!readArchivedJob(archive, archived, archivedBytes, error) || !validateJobContexts(archived, error)) { return false; }
			}
			const auto providers = state.value("providers").toObject();
			for (auto it = providers.begin(); it != providers.end(); ++it)
			{
				if ((it.key() != "reasoning" && it.key() != "audio") || !it.value().isObject()
					|| StudioInference::configuration(it.value().toObject(), it.key() == "audio") != it.value().toObject())
				{
					error = "Persisted provider configuration is invalid";
					return false;
				}
			}
			// Migrate old large inline receipts before replacing native state.
			if (!archiveToolResults(state, error)) { return false; }
			if (!archiveTranscript(state, error)) { return false; }
			if (!archiveReceipts(state, error)) { return false; }
		}
		catch (const std::exception& exception) { error = exception.what(); return false; }
		catch (...) { error = "Session archive verification threw a nonstandard C++ exception"; return false; }
		if (!restore(history.at(cursor).toObject(), error)) { return false; }
		m_state = state;
		if (!m_state.contains("agent")) { m_state.insert("agent", StudioAgent::initialState()); }
		if (!m_state.contains("providers")) { m_state.insert("providers", QJsonObject{}); }
		if (m_state.value("revision").toInt() >= 2147483646) { error = "Native revision counter exhausted during recovery"; return false; }
		auto recovered = m_state;
		recovered.insert("revision", recovered.value("revision").toInt() + 1);
		auto agent = recovered.value("agent").toObject();
		auto job = agent.value("job").toObject();
		if (StudioAgent::active(job))
		{
			// Recovery replaces native objects. Keep executed results, but invalidate
			// decisions that still depend on handles from the previous engine.
			for (const auto& call : job.value("pending").toArray())
			{
				StudioAgent::appendToolOutput(job, call.toObject(), failure("Engine restarted; rediscover native state and parameter handles before editing"));
			}
			job.insert("pending", QJsonArray{});
			job.insert("status", "queued");
			job.insert("expectedRevision", recovered.value("revision"));
			agent.insert("job", job);
			recovered.insert("agent", agent);
		}
		if (!commit(recovered, error)) { return false; }
	}
	else
	{
		try
		{
			Engine::getSong()->createEmptyProject();
			if (!commit({{"format", 1}, {"revision", 0}, {"cursor", 0}, {"history", QJsonArray{checkpoint()}},
				{"memory", QJsonObject{}}, {"requests", QJsonArray{}}, {"providers", QJsonObject{}},
				{"agent", StudioAgent::initialState()}}, error)) { return false; }
		}
		catch (const std::exception& exception) { error = exception.what(); return false; }
		catch (...) { error = "Native session initialization threw a nonstandard C++ exception"; return false; }
	}
	m_open = true;
	return true;
}

QJsonObject StudioRuntime::sessionInfo() const
{
	return {{"directory", m_directory.absolutePath()}, {"revision", m_state.value("revision")},
		{"historyCount", m_state.value("history").toArray().size()}, {"cursor", m_state.value("cursor")},
		{"memory", m_state.value("memory")}, {"providers", m_state.value("providers")},
		{"agent", StudioAgent::inspect(m_state.value("agent").toObject())},
		{"cachedRequests", m_state.value("requests").toArray().size()},
		{"storage", QJsonObject{{"retiredFileReferences", m_state.value("retiredFiles").toArray().size()},
			{"retirementLimit", StudioStorage::MaximumRetiredFiles}, {"lastCleanup", m_storageReport}}}};
}

QJsonObject StudioRuntime::process(const QJsonObject& request)
{
	if (!onApplicationThread())
	{
		auto response = failure("Studio requests must execute on the application thread");
		response.insert("id", request.value("id")); return response;
	}
	const auto op = request.value("op").toString();
	if (request.value("id").toString().startsWith("@agent/"))
	{
		auto response = failure("The @agent/ request namespace belongs to native job execution");
		response.insert("id", request.value("id"));
		return response;
	}
	if ((!m_activeRequestIds.isEmpty() || m_advancingAgent) && op != "session.inspect" && op != "agent.inspect"
		&& op != "studio.resources.inspect" && op != "agent.cancel" && op != "session.shutdown")
	{
		auto response = failure("Studio is executing an operation; inspect or cancel the current job before submitting more work");
		response.insert("id", request.value("id"));
		response.insert("revision", m_state.value("revision"));
		return response;
	}
	return performRequest(request);
}

QJsonObject StudioRuntime::performRequest(const QJsonObject& request,
	const std::function<void(QJsonObject&, const QJsonObject&)>& update)
{
	const auto id = request.value("id").toString();
	const auto reject = [&](const QString& message)
	{
		auto response = failure(message);
		if (!id.isEmpty()) { response.insert("id", id); }
		if (m_open) { response.insert("revision", m_state.value("revision")); }
		return response;
	};
	if (!m_open || m_stopping) { return reject("Studio session is not accepting requests"); }
	const auto op = request.value("op").toString();
	if (id.isEmpty() || id.size() > 128 || op.isEmpty() || !request.value("args").isObject())
	{
		return reject("Request requires a nonempty id (up to 128 characters), op, and an args object");
	}
	for (auto it = request.begin(); it != request.end(); ++it)
	{
		if (it.key() != "id" && it.key() != "op" && it.key() != "args" && it.key() != "expectedRevision")
		{
			return reject("Unknown request field: " + it.key());
		}
	}
	const auto bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxInputBytes) { return reject("Request exceeds the 1 MiB limit"); }
	if (m_activeRequestIds.contains(id)) { return reject("Request id is still executing; wait for its acknowledged response"); }
	// Reserve before receipt I/O as well as native execution: cooperative reads
	// may process nested control requests, even when replaying an acknowledged ID.
	m_activeRequestIds.insert(id);
	const auto releaseRequestId = qScopeGuard([this, &id] { m_activeRequestIds.remove(id); });
	QElapsedTimer operationTime;
	operationTime.start();
	const auto executingJob = m_state.value("agent").toObject().value("job").toObject();
	const int operationBudget = executingJob.value("maximumWallTimeMs").toInt() - executingJob.value("elapsedMs").toInt();
	const auto cancelled = [&] {
		return m_stopping || interruptionRequested() || (m_advancingAgent && (operationTime.elapsed() >= operationBudget
			|| !StudioAgent::active(m_state.value("agent").toObject().value("job").toObject())
			|| (m_resourceBudget && m_resourceBudget(false))));
	};
	FileResourceIO::Scope assetWork(cancelled, true);
	const auto hash = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	for (const auto entry : m_state.value("requests").toArray())
	{
		const auto cached = entry.toObject();
		if (cached.value("id").toString() != id) { continue; }
		if (cached.value("sha256").toString() != hash) { return reject("Request id was already used for different content"); }
		try
		{
			QJsonObject receipt; QString error;
			if (!readReceipt(cached, m_state.value("revision").toInt(), receipt, error)) { return reject(error); }
			auto response = receipt.value("response").toObject(); response.insert("replayed", true); return response;
		}
		catch (const std::exception& exception) { return reject("Request receipt read threw: " + QString::fromUtf8(exception.what())); }
		catch (...) { return reject("Request receipt read threw a nonstandard C++ exception"); }
	}
	if (request.contains("expectedRevision")
		&& (!nonnegativeInteger(request.value("expectedRevision"))
			|| request.value("expectedRevision") != m_state.value("revision")))
	{
		auto response = failure("Project revision changed; inspect current state before editing");
		response.insert("id", id);
		response.insert("revision", m_state.value("revision"));
		return response;
	}
	const auto args = request.value("args").toObject();
	const auto stateBefore = m_state;
	auto next = m_state;
	auto history = next.value("history").toArray();
	int cursor = next.value("cursor").toInt();
	const auto original = history.at(cursor).toObject();
	QJsonObject response;
	bool restored = false;
	bool mutated = false;
	bool shutdown = false;
	QString compactSummary;
	try
	{
		if (op == "session.inspect" || op == "session.shutdown"
			|| op == "agent.inspect" || op == "agent.cancel")
		{
			if (!args.isEmpty()) { throw std::invalid_argument("Session operation takes no arguments"); }
		}
		if (op == "session.inspect") { response = {{"ok", true}, {"result", sessionInfo()}}; }
		else if (op == "studio.capabilities")
		{
			if (!args.isEmpty()) { throw std::invalid_argument("Capability discovery takes no arguments"); }
			response = {{"ok", true}, {"result", QJsonObject{{"capabilities", StudioAgent::schemas()},
				{"sessionOperations", QJsonArray{"session.inspect", "session.provider.set", "session.chat", "conversation.history",
					"agent.inspect", "agent.cancel", "agent.resume", "session.undo", "session.redo", "session.shutdown"}}}}};
		}
		else if (op == "session.shutdown")
		{
			auto agent = next.value("agent").toObject();
			auto job = agent.value("job").toObject();
			if (StudioAgent::active(job)) { job.insert("status", "paused"); job.insert("lastError", "Session shut down"); agent.insert("job", job); }
			next.insert("agent", agent);
			response = {{"ok", true}, {"result", QJsonObject{{"shutdown", true}}}}; shutdown = true;
		}
		else if (op == "session.provider.set")
		{
			const auto role = args.value("role").toString();
			if (args.size() != 2 || (role != "reasoning" && role != "audio") || !args.value("configuration").isObject())
			{
				throw std::invalid_argument("Provider operation requires role (reasoning or audio) and configuration");
			}
			const auto configuration = StudioInference::configuration(args.value("configuration").toObject(), role == "audio");
			auto providers = next.value("providers").toObject();
			providers.insert(role, configuration);
			next.insert("providers", providers);
			response = {{"ok", true}, {"result", providers}};
		}
		else if (op == "agent.inspect") { response = {{"ok", true}, {"result", StudioAgent::inspect(next.value("agent").toObject())}}; }
		else if (op == "session.chat")
		{
			for (auto it = args.begin(); it != args.end(); ++it)
			{
				if (it.key() != "text" && it.key() != "maxRounds" && it.key() != "maxOperations" && it.key() != "maximumWallTimeMs"
					&& it.key() != "maximumCpuTimeMs" && it.key() != "maximumResidentMiB")
				{
					throw std::invalid_argument("Unknown conversation argument");
				}
			}
			auto agent = next.value("agent").toObject();
			const auto oldJob = agent.value("job").toObject();
			if (StudioAgent::active(oldJob)) { throw std::invalid_argument("A conversation job is active; cancel it before submitting another intent"); }
			const auto job = StudioAgent::job(args.value("text").toString(), args,
				next.value("providers").toObject().value("reasoning").toObject(), next.value("revision").toInt());
			if (!oldJob.isEmpty())
			{
				auto archives = agent.value("archives").toArray();
				archives.append(archiveJob(oldJob));
				agent.insert("archives", archives);
			}
			auto transcript = agent.value("transcript").toArray();
			transcript.append(QJsonObject{{"role", "user"}, {"content", job.value("intent")}, {"job", job.value("id")},
				{"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
			agent.insert("transcript", transcript);
			agent.insert("job", job);
			next.insert("agent", agent);
			response = {{"ok", true}, {"result", StudioAgent::inspect(agent)}};
		}
		else if (op == "conversation.history")
		{
			response = {{"ok", true}, {"result", conversationHistory(args)}};
		}
		else if (op == "agent.cancel" || op == "agent.resume")
		{
			auto agent = next.value("agent").toObject();
			auto job = agent.value("job").toObject();
			if (job.isEmpty() || job.value("status").toString() == "complete") { throw std::invalid_argument("There is no resumable conversation job"); }
			if (op == "agent.cancel") { job.insert("status", "cancelled"); }
			else
			{
				for (auto it = args.begin(); it != args.end(); ++it)
				{
					const bool resource = it.key() == "maximumCpuTimeMs" || it.key() == "maximumResidentMiB";
					const int maximum = it.key() == "maxRounds" ? 1000 : it.key() == "maxOperations" ? 100000 : it.key() == "maximumResidentMiB" ? 1048576 : 86400000;
					if ((it.key() != "maxRounds" && it.key() != "maxOperations" && it.key() != "maximumWallTimeMs")
						&& !resource) { throw std::invalid_argument("Unknown resume budget"); }
					if (!nonnegativeInteger(it.value()) || it.value().toInt() < (resource ? 0 : 1) || it.value().toInt() > maximum)
					{
						throw std::invalid_argument("Resume budget is outside its supported integer range");
					}
					job.insert(it.key(), it.value());
				}
				if (job.value("maximumWallTimeMs").toInt() < 1000) { throw std::invalid_argument("The elapsed-time budget must be at least 1000 ms"); }
				const auto provider = next.value("providers").toObject().value("reasoning").toObject();
				const bool replanning = job.value("expectedRevision") != next.value("revision") || job.value("provider") != QJsonValue(provider);
				if (replanning)
				{
					// Close the old provider exchange before archiving or switching its
					// protocol. These decisions were never executed, but their IDs and
					// arguments still belong to this job and cannot be reused as evidence.
					StudioAgent::skipPending(job, "Conversation replanned after a project or reasoning-provider change; queued call was not executed");
					const auto previous = job.value("input").toArray();
					if (!previous.isEmpty())
					{
						auto past = job.value("pastInputs").toArray();
						past.append(archiveContext(job));
						job.insert("pastInputs", past);
						StudioAgent::archiveToolRecords(job, past.size() - 1);
					}
					job.insert("input", QJsonArray{});
				}
				job.insert("provider", provider);
				job.insert("expectedRevision", next.value("revision"));
				job.insert("status", "queued");
				job.remove("lastError");
				job.remove("outcome");
			}
			agent.insert("job", job);
			next.insert("agent", agent);
			response = {{"ok", true}, {"result", StudioAgent::inspect(agent)}};
		}
		else if (op == "agent.plan.set")
		{
			if (args.size() != 3 || args.value("goal").toString().trimmed().isEmpty() || args.value("goal").toString().toUtf8().size() > 65536
				|| !args.value("steps").isArray() || !args.value("decisions").isArray())
			{
				throw std::invalid_argument("A musical plan requires goal, steps and decisions");
			}
			for (const auto& field : {QString("steps"), QString("decisions")})
			{
				if (args.value(field).toArray().size() > 128) { throw std::invalid_argument("Plan exceeds 128 items"); }
				for (const auto& item : args.value(field).toArray())
				{
					if (!item.isString() || item.toString().size() > 2048) { throw std::invalid_argument("Plan items must be bounded strings"); }
				}
			}
			auto agent = next.value("agent").toObject();
			auto job = agent.value("job").toObject();
			if (!StudioAgent::active(job)) { throw std::invalid_argument("There is no active musical production job"); }
			job.insert("plan", args);
			agent.insert("job", job);
			next.insert("agent", agent);
			response = {{"ok", true}, {"result", args}};
		}
		else if (op == "agent.history.list" || op == "agent.history.read")
		{
			response = {{"ok", true}, {"result", queryHistory(op, args)}};
		}
		else if (op == "agent.context.compact")
		{
			const auto summary = args.value("summary").toString();
			const auto job = next.value("agent").toObject().value("job").toObject();
			const auto pending = job.value("pending").toArray();
			if (args.size() != 1 || !args.value("summary").isString() || summary.trimmed().isEmpty() || summary.toUtf8().size() > 65536)
			{
				throw std::invalid_argument("Context compaction requires one nonempty summary of at most 64 KiB");
			}
			if (!StudioAgent::active(job) || job.value("input").toArray().isEmpty()
				|| pending.size() > 1 || (!pending.isEmpty() && !update))
			{
				throw std::invalid_argument("Context compaction requires an active exchange with no remaining dependent calls");
			}
			compactSummary = summary;
			response = {{"ok", true}, {"result", QJsonObject{{"summary", summary}, {"context", job.value("pastInputs").toArray().size()}}}};
		}
		else if (op == "agent.finish")
		{
			const auto status = args.value("status").toString();
			const auto summary = args.value("summary").toString();
			if (args.size() != 3 || (status != "complete" && status != "needs_input") || summary.trimmed().isEmpty()
				|| summary.toUtf8().size() > 65536 || !args.value("evidence").isArray())
			{
				throw std::invalid_argument("Task outcome requires status, bounded summary and successful tool-call evidence");
			}
			auto agent = next.value("agent").toObject();
			auto job = agent.value("job").toObject();
			if (!StudioAgent::active(job) || job.value("pending").toArray().size() > 1)
			{
				throw std::invalid_argument("Finish requires an active job with no remaining dependent calls");
			}
			QSet<int> verifiedContexts;
			for (const auto& evidence : args.value("evidence").toArray())
			{
				bool found = false;
				for (const auto& value : job.value("records").toArray())
				{
					const auto record = value.toObject();
					if (evidence.isString() && record.value("kind").toString() == "tool"
						&& record.value("call").toObject().value("id") == evidence
						&& record.value("response").toObject().value("ok").toBool())
					{
						QByteArray bytes; QString error;
						if (!readToolRecord(record, job.value("id").toString(), bytes, error)) { throw std::runtime_error(error.toStdString()); }
						found = true; break;
					}
					if (evidence.isString() && record.value("kind").toString() == "archivedTool"
						&& record.value("id") == evidence && record.value("ok").toBool())
					{
						const int contextIndex = record.value("context").toInt();
						if (!verifiedContexts.contains(contextIndex))
						{
							QByteArray bytes; QString error;
							const auto context = job.value("pastInputs").toArray().at(contextIndex).toObject();
							if (!readContext(context, bytes, error)) { throw std::runtime_error(error.toStdString()); }
							const auto records = QJsonDocument::fromJson(bytes).object().value("records").toArray();
							// Verify all indexes into this context, then release its full
							// payload. Many cited contexts must not accumulate in memory.
							for (const auto& item : job.value("records").toArray())
							{
								const auto indexed = item.toObject();
								if (indexed.value("kind").toString() != "archivedTool" || indexed.value("context").toInt() != contextIndex) { continue; }
								const int position = indexed.value("record").toInt();
								if (position < 0 || position >= records.size() || !StudioAgent::matchesArchivedTool(indexed, records.at(position).toObject()))
								{
									throw std::runtime_error("Archived tool evidence does not match the committed index");
								}
								QByteArray payload;
								if (!readToolRecord(records.at(position).toObject(), job.value("id").toString(), payload, error))
								{ throw std::runtime_error(error.toStdString()); }
							}
							verifiedContexts.insert(contextIndex);
						}
						found = true; break;
					}
				}
				if (!found) { throw std::invalid_argument("Task outcome cites unavailable or failed tool evidence"); }
			}
			job.insert("status", status == "complete" ? "complete" : "paused");
			job.insert("outcome", args);
			if (status == "needs_input") { job.insert("lastError", "The production task requires user input"); }
			agent.insert("job", job);
			auto transcript = agent.value("transcript").toArray();
			transcript.append(QJsonObject{{"role", "assistant"}, {"content", summary}, {"job", job.value("id")},
				{"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
			agent.insert("transcript", transcript);
			next.insert("agent", agent);
			response = {{"ok", true}, {"result", args}};
		}
		else if (op == "audio.listen") { response = {{"ok", true}, {"result", listen(args)}}; }
		else if (op == "session.memory.set")
		{
			if (args.size() != 1 || !args.value("memory").isObject())
			{
				throw std::invalid_argument("Memory operation requires exactly one memory object");
			}
			next.insert("memory", args.value("memory"));
			response = {{"ok", true}, {"result", args}};
		}
		else if (op == "session.undo" || op == "session.redo")
		{
			if (!args.isEmpty() && (args.size() != 1 || !args.value("summary").isBool()))
			{ throw std::invalid_argument("History operations accept only an optional boolean summary"); }
			const int target = cursor + (op == "session.undo" ? -1 : 1);
			if (target < 0 || target >= history.size()) { throw std::invalid_argument("No revision in that direction"); }
			QString error;
			if (!restore(history.at(target).toObject(), error, &restored)) { throw std::runtime_error(error.toStdString()); }
			next.insert("cursor", target);
			response = {{"ok", true}, {"result", args.value("summary").toBool() ? StudioCapabilities::inspectSummary() : StudioCapabilities::inspect()}};
		}
		else
		{
			if (StudioCapabilities::mutatesProject(op))
			{
				QString error;
				if (!validateCheckpoint(original, error)) { throw std::runtime_error(error.toStdString()); }
				mutated = true;
			}
			response = StudioCapabilities::execute(op, args, cancelled);
			if (mutated && response.value("ok").toBool())
			{
				const auto snapshot = checkpoint();
				while (history.size() > cursor + 1) { history.removeLast(); }
				history.append(snapshot);
				while (history.size() > MaxHistory) { history.removeFirst(); }
				next.insert("history", history);
				next.insert("cursor", history.size() - 1);
			}
		}
		// Check again at the transaction boundary, even if the operation was too
		// short to trigger a periodic probe. Mandatory rollback ignores cancellation.
		if (m_advancingAgent && m_resourceBudget && op != "session.inspect" && op != "agent.inspect"
			&& op != "studio.resources.inspect" && op != "agent.cancel" && op != "session.shutdown" && m_resourceBudget(true))
		{
			throw std::runtime_error("Native execution interrupted by the conversation resource budget");
		}
	}
	catch (const std::exception& exception) { response = failure(exception.what()); }
	catch (...) { response = failure("Native studio request threw a nonstandard C++ exception"); }
	// A failed action must not commit a partially prepared cursor/history or
	// metadata update, even if its native rollback succeeds. Nested cancellation
	// and observation metadata is merged from m_state below.
	if (!response.value("ok").toBool()) { next = stateBefore; compactSummary.clear(); }
	if ((mutated || restored) && !response.value("ok").toBool())
	{
		QString error;
		if (!recoverNativeProject(original, error))
		{
			m_stopping = true;
			m_failed = true;
			return reject("Operation failed: " + response.value("error").toString() + "; native rollback failed: " + error);
		}
		restored = true;
	}
	if (mutated || restored)
	{
		// Restoring replaces native objects and invalidates parameter handles,
		// even if a rejected mutation did not commit a musical change.
		if (next.value("revision").toInt() >= 2147483646)
		{
			m_stopping = m_failed = true;
			QString error;
			recoverNativeProject(original, error);
			return reject("Native revision counter exhausted" + (error.isEmpty() ? QString() : "; rollback: " + error));
		}
		next.insert("revision", next.value("revision").toInt() + 1);
	}
	response.insert("id", id);
	response.insert("revision", next.value("revision"));
	if (m_state != stateBefore)
	{
		// Only observation, cancellation and shutdown can commit during a nested
		// event loop. Their newer metadata must survive the outer operation.
		next.insert("agent", m_state.value("agent"));
		next.insert("requests", m_state.value("requests"));
	}
	if (update || !compactSummary.isEmpty())
	{
		try
		{
			if (update) { update(next, response); }
			if (!compactSummary.isEmpty())
			{
				auto agent = next.value("agent").toObject();
				auto job = agent.value("job").toObject();
				// Include the compaction tool's output before sealing its exchange.
				// The next provider request must never begin with an unmatched output.
				auto past = job.value("pastInputs").toArray();
				past.append(archiveContext(job));
				job.insert("pastInputs", past); job.insert("input", QJsonArray{});
				StudioAgent::archiveToolRecords(job, past.size() - 1);
				job.insert("contextSummary", compactSummary); agent.insert("job", job); next.insert("agent", agent);
			}
		}
		catch (...)
		{
			QString reason;
			try { throw; }
			catch (const std::exception& exception) { reason = QString::fromUtf8(exception.what()); }
			catch (...) { reason = "nonstandard C++ exception"; }
			m_stopping = m_failed = true;
			QString rollbackError;
			if (mutated || restored) { recoverNativeProject(original, rollbackError); }
			return reject("Agent execution bookkeeping failed: " + reason
				+ (rollbackError.isEmpty() ? QString() : "; rollback: " + rollbackError));
		}
	}
	auto ledger = next.value("requests").toArray();
	ledger.append(QJsonObject{{"id", id}, {"sha256", hash}, {"request", request}, {"response", response}});
	while (ledger.size() > MaxRequests) { ledger.removeFirst(); }
	next.insert("requests", ledger);
	QString error;
	if (!commit(next, error))
	{
		// Stop before rollback: even its native callbacks must not accept new
		// requests after the durable commit has been lost.
		m_stopping = m_failed = true;
		QString rollbackError;
		if (mutated || restored) { recoverNativeProject(original, rollbackError); }
		return reject("Session commit failed; operation was not acknowledged: " + error
			+ (rollbackError.isEmpty() ? QString() : "; rollback: " + rollbackError));
	}
	if (shutdown) { m_stopping = true; }
	return response;
}

QJsonObject StudioRuntime::archiveJob(const QJsonObject& job)
{
	auto archived = job;
	StudioAgent::skipPending(archived, "A new conversation replaced this job; queued call was not executed");
	auto validation = StudioAgent::initialState(); validation.insert("job", archived);
	if (!StudioAgent::validState(validation)) { throw std::runtime_error("Cannot archive an invalid conversation job"); }
	const auto bytes = QJsonDocument(archived).toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxManifestBytes) { throw std::runtime_error("Conversation job exceeds the 16 MiB archive limit"); }
	const auto relative = "jobs/" + job.value("id").toString() + ".json";
	QString error; const auto filename = archiveFilePath(relative, true, error);
	if (filename.isEmpty()) { throw std::runtime_error(error.toStdString()); }
	QSaveFile file(filename);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
	{
		throw std::runtime_error("Could not preserve the conversation job");
	}
	return {{"path", relative}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
		{"status", job.value("status")}, {"intent", job.value("intent")}};
}

QJsonObject StudioRuntime::archiveContext(const QJsonObject& job)
{
	QJsonArray records;
	for (const auto& value : job.value("records").toArray())
	{
		if (value.toObject().value("kind").toString() == "tool") { records.append(value); }
	}
	const QJsonObject context{{"protocol", job.value("provider").toObject().value("protocol")}, {"input", job.value("input")}, {"records", records}};
	if (!StudioAgent::validPastInput(context)) { throw std::runtime_error("Cannot archive an invalid inference exchange"); }
	const auto bytes = QJsonDocument(context).toJson(QJsonDocument::Compact);
	if (bytes.size() > MaxManifestBytes) { throw std::runtime_error("Inference exchange exceeds the 16 MiB archive limit"); }
	const auto relative = "contexts/" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json";
	QString error; const auto filename = archiveFilePath(relative, true, error);
	if (filename.isEmpty()) { throw std::runtime_error(error.toStdString()); }
	QSaveFile file(filename);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
	{
		throw std::runtime_error("Could not preserve the inference exchange");
	}
	return {{"protocol", context.value("protocol")}, {"path", relative}, {"items", context.value("input").toArray().size()}, {"recordItems", records.size()},
		{"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
}

bool StudioRuntime::readContext(const QJsonObject& reference, QByteArray& bytes, QString& error) const
{
	bytes.clear();
	if (!StudioAgent::validPastInput(reference)) { error = "Invalid archived inference context reference"; return false; }
	if (reference.contains("input"))
	{
		bytes = QJsonDocument(reference).toJson(QJsonDocument::Compact);
		if (bytes.size() > MaxManifestBytes) { bytes.clear(); error = "Inline inference context exceeds the archive limit"; return false; }
		error.clear(); return true;
	}
	QJsonObject context;
	const auto filename = archiveFilePath(reference.value("path").toString(), false, error); if (filename.isEmpty()) { return false; }
	if (!readHistoryArchive(filename, reference.value("sha256").toString(), bytes, context, error))
	{
		error = "Inference context: " + error; return false;
	}
	if (!context.value("input").isArray() || !StudioAgent::validPastInput(context)
		|| context.value("protocol") != reference.value("protocol") || context.value("input").toArray().size() != reference.value("items").toInt()
		|| context.contains("records") != reference.contains("recordItems")
		|| context.value("records").toArray().size() != reference.value("recordItems").toInt())
	{
		bytes.clear(); error = "Inference context archive is inconsistent"; return false;
	}
	error.clear(); return true;
}

bool StudioRuntime::readContextRecord(const QJsonObject& context, int record, const QString& job, QByteArray& bytes, QString& error) const
{
	if (!readContext(context, bytes, error)) { return false; }
	const auto records = QJsonDocument::fromJson(bytes).object().value("records").toArray();
	if (record < 0 || record >= records.size()) { bytes.clear(); error = "No archived tool record has that index"; return false; }
	return readToolRecord(records.at(record).toObject(), job, bytes, error);
}

bool StudioRuntime::readArchivedJob(const QJsonObject& reference, QJsonObject& job, QByteArray& bytes, QString& error) const
{
	job = {}; bytes.clear();
	auto validation = StudioAgent::initialState(); validation.insert("archives", QJsonArray{reference});
	if (!StudioAgent::validState(validation)) { error = "Invalid conversation archive reference"; return false; }
	const auto filename = archiveFilePath(reference.value("path").toString(), false, error); if (filename.isEmpty()) { return false; }
	if (!readHistoryArchive(filename, reference.value("sha256").toString(), bytes, job, error)) { return false; }
	validation.insert("job", job);
	if (job.isEmpty() || !StudioAgent::validState(validation)
		|| job.value("id").toString() != QFileInfo(reference.value("path").toString()).completeBaseName()
		|| job.value("status") != reference.value("status") || job.value("intent") != reference.value("intent"))
	{
		job = {}; bytes.clear(); error = "Conversation archive identity or state is inconsistent"; return false;
	}
	error.clear(); return true;
}

QJsonObject StudioRuntime::queryHistory(const QString& operation, const QJsonObject& arguments) const
{
	const bool reading = operation == "agent.history.read";
	const QSet<QString> fields = reading ? QSet<QString>{"job", "call", "context", "record", "offset", "limit", "sha256"}
		: QSet<QString>{"job", "context", "offset", "limit"};
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
	{
		if (!fields.contains(it.key())) { throw std::invalid_argument("Unknown history argument"); }
	}
	if ((reading || arguments.contains("job") || arguments.contains("context")) && (!arguments.value("job").isString() || arguments.value("job").toString().isEmpty()))
	{
		throw std::invalid_argument("History selection requires a committed job ID");
	}
	if (arguments.contains("record") && !arguments.contains("context")) { throw std::invalid_argument("A tool record requires a frozen context index"); }
	if (arguments.contains("call") && (!arguments.value("call").isString() || arguments.value("call").toString().isEmpty()
		|| arguments.value("call").toString().size() > 128 || arguments.contains("context") || arguments.contains("record")))
	{ throw std::invalid_argument("Select a tool call by its ID, without context or record selectors"); }
	const auto agent = m_state.value("agent").toObject();
	const auto current = agent.value("job").toObject();
	const auto archives = agent.value("archives").toArray();
	if (!arguments.contains("job"))
	{
		const int currentCount = current.isEmpty() ? 0 : 1;
		const int total = currentCount + archives.size();
		const int offset = historyInteger(arguments, "offset", 0, 0, total), limit = historyInteger(arguments, "limit", 16, 1, 32);
		QJsonArray jobs;
		for (int index = offset; index < total && jobs.size() < limit; ++index)
		{
			if (currentCount && index == 0)
			{
				jobs.append(QJsonObject{{"id", current.value("id")}, {"archived", false}, {"status", current.value("status")},
					{"intentPreview", historyPreview(current.value("intent").toString())}});
			}
			else
			{
				const auto reference = archives.at(archives.size() - 1 - (index - currentCount)).toObject();
				jobs.append(QJsonObject{{"id", QFileInfo(reference.value("path").toString()).completeBaseName()}, {"archived", true},
					{"status", historyPreview(reference.value("status").toString())}, {"intentPreview", historyPreview(reference.value("intent").toString())}});
			}
		}
		return {{"jobs", jobs}, {"total", total}, {"offset", offset}, {"nextOffset", offset + jobs.size()}, {"complete", offset + jobs.size() == total}};
	}
	const auto id = arguments.value("job").toString();
	const bool live = !current.isEmpty() && id == current.value("id").toString();
	QJsonObject selected; QByteArray jobBytes; QString error;
	if (live) { selected = current; }
	else
	{
		for (const auto& value : archives)
		{
			const auto reference = value.toObject();
			if (QFileInfo(reference.value("path").toString()).completeBaseName() != id) { continue; }
			if (!readArchivedJob(reference, selected, jobBytes, error)) { throw std::runtime_error(error.toStdString()); }
			break;
		}
	}
	if (selected.isEmpty()) { throw std::invalid_argument("No committed conversation job has that ID"); }
	const auto contexts = selected.value("pastInputs").toArray();
	const bool finalInput = !live && !selected.value("input").toArray().isEmpty();
	const int total = contexts.size() + (finalInput ? 1 : 0);
	const auto checkWork = [&] { if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } };
	QJsonArray finalRecords;
	if (finalInput)
	{
		int scanned = 0;
		for (const auto& value : selected.value("records").toArray())
		{
			if (++scanned % 128 == 0) { checkWork(); }
			if (value.toObject().value("kind").toString() == "tool") { finalRecords.append(value); }
		}
	}
	const auto contextAt = [&](int index) {
		// Preserve the historical bytes/digest of final whole-context reads.
		// Its tool records remain in the job archive and are selected separately.
		return index < contexts.size() ? contexts.at(index).toObject() : QJsonObject{
			{"protocol", selected.value("provider").toObject().value("protocol")}, {"input", selected.value("input")}};
	};
	const auto recordsAt = [&](int index) {
		checkWork();
		if (index == contexts.size()) { return finalRecords; }
		auto context = contextAt(index);
		if (!context.contains("input"))
		{
			QByteArray bytes;
			if (!readContext(context, bytes, error)) { throw std::runtime_error(error.toStdString()); }
			context = QJsonDocument::fromJson(bytes).object();
		}
		else if (!StudioAgent::validPastInput(context)) { throw std::runtime_error("Invalid inline inference context"); }
		const auto records = context.value("records").toArray();
		int scanned = 0;
		for (const auto& value : selected.value("records").toArray())
		{
			if (++scanned % 128 == 0) { checkWork(); }
			const auto reference = value.toObject();
			if (reference.value("kind").toString() != "archivedTool" || reference.value("context").toInt() != index) { continue; }
			const int position = reference.value("record").toInt(-1);
			if (position < 0 || position >= records.size() || !StudioAgent::matchesArchivedTool(reference, records.at(position).toObject()))
			{ throw std::runtime_error("Archived context tool record does not match the committed index"); }
		}
		checkWork(); return records;
	};
	if (!reading && arguments.contains("context"))
	{
		const int contextIndex = historyInteger(arguments, "context", 0, 0, total - 1);
		const auto records = recordsAt(contextIndex);
		const int offset = historyInteger(arguments, "offset", 0, 0, records.size()), limit = historyInteger(arguments, "limit", 16, 1, 32);
		QJsonArray entries; qint64 bytes = 2;
		for (int index = offset; index < records.size() && entries.size() < limit; ++index)
		{
			checkWork();
			const auto record = records.at(index).toObject(), call = record.value("call").toObject(), response = record.value("response").toObject();
			QJsonObject entry{{"record", index}, {"call", call.value("id")}, {"name", call.value("name")}, {"ok", response.value("ok")},
				{"responseStorage", record.contains("archive") ? "toolResultArchive" : "inline"}};
			if (nonnegativeInteger(response.value("revision"))) { entry.insert("revision", response.value("revision")); }
			const auto errorText = response.contains("error") ? response.value("error").toString() : response.value("errorPreview").toString();
			if (!errorText.isEmpty()) { entry.insert("errorPreview", historyPreview(errorText)); }
			if (record.contains("archive")) { entry.insert("payloadBytes", record.value("archive").toObject().value("bytes")); }
			const auto length = QJsonDocument(entry).toJson(QJsonDocument::Compact).size() + 1;
			if (bytes + length > 64 * 1024) { break; }
			entries.append(entry); bytes += length;
		}
		checkWork();
		return {{"job", id}, {"context", contextIndex}, {"protocol", contextAt(contextIndex).value("protocol")}, {"records", entries},
			{"total", records.size()}, {"offset", offset}, {"nextOffset", offset + entries.size()}, {"complete", offset + entries.size() == records.size()}};
	}
	if (!reading)
	{
		const int offset = historyInteger(arguments, "offset", 0, 0, total), limit = historyInteger(arguments, "limit", 16, 1, 32);
		QJsonArray entries;
		for (int index = offset; index < total && entries.size() < limit; ++index)
		{
			const auto context = contextAt(index);
			entries.append(QJsonObject{{"context", index}, {"protocol", context.value("protocol")},
				{"items", context.contains("input") ? context.value("input").toArray().size() : context.value("items").toInt()},
				{"recordItems", index == contexts.size() ? finalRecords.size() : context.contains("input") ? context.value("records").toArray().size() : context.value("recordItems").toInt()},
				{"storage", index == contexts.size() ? "jobArchive" : context.contains("input") ? "inline" : "contextArchive"}});
		}
		return {{"job", id}, {"archived", !live}, {"status", selected.value("status")}, {"intentPreview", historyPreview(selected.value("intent").toString())},
			{"summaryPreview", historyPreview(selected.value("contextSummary").toString())}, {"contexts", entries}, {"total", total},
			{"offset", offset}, {"nextOffset", offset + entries.size()}, {"complete", offset + entries.size() == total}};
	}
	QByteArray bytes; QString protocol;
	int contextIndex = -1, recordIndex = -1;
	if (arguments.contains("call"))
	{
		bool found = false;
		for (const auto& value : selected.value("records").toArray())
		{
			const auto record = value.toObject();
			if (record.value("kind").toString() == "tool" && record.value("call").toObject().value("id") == arguments.value("call"))
			{
				if (!readToolRecord(record, id, bytes, error)) { throw std::runtime_error(error.toStdString()); }
				protocol = selected.value("provider").toObject().value("protocol").toString();
				found = true; break;
			}
			if (record.value("kind").toString() == "archivedTool" && record.value("id") == arguments.value("call"))
			{
				contextIndex = record.value("context").toInt(); recordIndex = record.value("record").toInt();
				const auto context = contexts.at(contextIndex).toObject(); protocol = context.value("protocol").toString();
				if (!readContextRecord(context, recordIndex, id, bytes, error)
					|| !StudioAgent::matchesArchivedTool(record, QJsonDocument::fromJson(bytes).object()))
				{ throw std::runtime_error(error.isEmpty() ? std::string("Archived tool call differs from its committed index") : error.toStdString()); }
				found = true; break;
			}
		}
		if (!found) { throw std::invalid_argument("No committed tool call has that ID"); }
	}
	else if (arguments.contains("context"))
	{
		contextIndex = historyInteger(arguments, "context", 0, 0, total - 1);
		const auto context = contextAt(contextIndex);
		if (arguments.contains("record"))
		{
			const auto records = recordsAt(contextIndex);
			recordIndex = historyInteger(arguments, "record", 0, 0, records.size() - 1);
			if (!readToolRecord(records.at(recordIndex).toObject(), id, bytes, error)) { throw std::runtime_error(error.toStdString()); }
		}
		else if (!readContext(context, bytes, error)) { throw std::runtime_error(error.toStdString()); }
		protocol = context.value("protocol").toString();
	}
	else
	{
		if (live) { throw std::invalid_argument("The current job is live; select one of its frozen contexts"); }
		bytes = jobBytes;
	}
	const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	if (arguments.contains("sha256") && (!arguments.value("sha256").isString() || arguments.value("sha256").toString() != digest))
	{
		throw std::invalid_argument("History digest does not match the requested continuation");
	}
	const int offset = historyInteger(arguments, "offset", 0, 0, bytes.size()), limit = historyInteger(arguments, "limit", 8192, 4, 65536);
	const auto continuationByte = [&](int index) { return (static_cast<unsigned char>(bytes.at(index)) & 0xc0) == 0x80; };
	if (offset < bytes.size() && continuationByte(offset)) { throw std::invalid_argument("History offset is inside a UTF-8 character"); }
	int end = std::min<int>(bytes.size(), offset + limit);
	while (end < bytes.size() && continuationByte(end)) { --end; }
	QJsonObject result{{"job", id}, {"context", contextIndex}, {"record", recordIndex}, {"protocol", protocol}, {"encoding", "UTF-8 JSON"}, {"sha256", digest},
		{"totalBytes", bytes.size()}, {"offset", offset}, {"nextOffset", end}, {"complete", end == bytes.size()},
		{"text", QString::fromUtf8(bytes.constData() + offset, end - offset)}};
	if (arguments.contains("call")) { result.insert("call", arguments.value("call")); }
	return result;
}

bool StudioRuntime::validateJobContexts(const QJsonObject& job, QString& error) const
{
	if (!job.isEmpty() && !job.value("pastInputs").isArray()) { error = "Invalid archived inference context list"; return false; }
	QHash<int, QJsonArray> indexedRecords;
	for (const auto& value : job.value("records").toArray())
	{
		const auto record = value.toObject();
		if (record.value("kind").toString() == "archivedTool") { indexedRecords[record.value("context").toInt()].append(record); }
		else if (record.value("kind").toString() == "tool" && record.contains("archive"))
		{
			QByteArray bytes;
			if (!readToolRecord(record, job.value("id").toString(), bytes, error)) { return false; }
		}
	}
	const auto contexts = job.value("pastInputs").toArray();
	for (int index = 0; index < contexts.size(); ++index)
	{
		const auto value = contexts.at(index);
		const auto reference = value.toObject();
		if (!value.isObject() || !StudioAgent::validPastInput(reference)) { error = "Invalid archived inference context reference"; return false; }
		QByteArray bytes;
		if (!readContext(reference, bytes, error)) { return false; }
		const auto records = QJsonDocument::fromJson(bytes).object().value("records").toArray();
		for (const auto& value : records)
		{
			const auto record = value.toObject();
			if (!record.contains("archive")) { continue; }
			QByteArray payload;
			if (!readToolRecord(record, job.value("id").toString(), payload, error)) { return false; }
		}
		for (const auto& archived : indexedRecords.value(index))
		{
			const auto record = archived.toObject(); const int position = record.value("record").toInt(-1);
			if (position < 0 || position >= records.size() || !StudioAgent::matchesArchivedTool(record, records.at(position).toObject()))
			{
				error = "Archived context tool record does not match the committed index"; return false;
			}
		}
	}
	error.clear(); return true;
}

bool StudioRuntime::storeAgent(const QJsonObject& agent, QString& error)
{
	if (!StudioAgent::validState(agent))
	{
		error = "Native agent bookkeeping produced an invalid state; previous commit was preserved";
		m_stopping = m_failed = true;
		return false;
	}
	auto next = m_state;
	next.insert("agent", agent);
	if (!commit(next, error)) { m_stopping = m_failed = true; return false; }
	return true;
}

QJsonObject StudioRuntime::listen(const QJsonObject& arguments)
{
	QElapsedTimer elapsed;
	elapsed.start();
	const auto executingJob = m_state.value("agent").toObject().value("job").toObject();
	const int remainingMs = executingJob.value("maximumWallTimeMs").toInt() - executingJob.value("elapsedMs").toInt();
	for (auto it = arguments.begin(); it != arguments.end(); ++it)
	{
		if (it.key() != "path" && it.key() != "prompt" && it.key() != "offsetSeconds" && it.key() != "durationSeconds" && it.key() != "sourceSha256")
		{
			throw std::invalid_argument("Unknown listening argument");
		}
	}
	const auto prompt = arguments.value("prompt").toString();
	if (prompt.trimmed().isEmpty() || prompt.toUtf8().size() > 65536) { throw std::invalid_argument("Listening requires a bounded evaluation prompt"); }
	auto provider = m_state.value("providers").toObject().value("audio").toObject();
	if (provider.isEmpty()) { throw std::runtime_error("Perceptual audio understanding is unavailable: no audio provider is configured"); }
	if (m_advancingAgent)
	{
		provider.insert("timeoutMs", std::max(100, std::min(provider.value("timeoutMs").toInt(), remainingMs)));
	}
	const auto cancelled = [&]
	{
		return m_stopping || interruptionRequested() || (m_advancingAgent && (elapsed.elapsed() >= remainingMs
			|| !StudioAgent::active(m_state.value("agent").toObject().value("job").toObject())
			|| (m_resourceBudget && m_resourceBudget(false))));
	};
	const auto excerpt = StudioAudio::excerpt(arguments, cancelled);
	if (m_advancingAgent)
	{
		const int afterDecode = remainingMs - static_cast<int>(elapsed.elapsed());
		if (afterDecode < 100) { throw std::runtime_error("Listening reached the conversation elapsed-time budget"); }
		provider.insert("timeoutMs", std::min(provider.value("timeoutMs").toInt(), afterDecode));
	}
	const auto scopedPrompt = "Native listening excerpt metadata (data):\n"
		+ QString::fromUtf8(QJsonDocument(excerpt.observation).toJson(QJsonDocument::Compact))
		+ "\nPCM16 transcoding clips source samples outside full scale; distinguish that from source-file measurements.\nEvaluation request:\n" + prompt;
	auto result = StudioInference::listen(provider, excerpt.wave, scopedPrompt, cancelled, m_transport);
	result.insert("excerpt", excerpt.observation);
	result.insert("audioSubmitted", true);
	result.insert("scope", "configured provider's judgment of the submitted excerpt");
	return result;
}

QJsonObject StudioRuntime::advanceAgent()
{
	if (!onApplicationThread()) { return failure("Studio agents must advance on the application thread"); }
	if (!m_open || m_stopping || m_advancingAgent || !m_activeRequestIds.isEmpty()) { return {}; }
	auto agent = m_state.value("agent").toObject();
	auto job = agent.value("job").toObject();
	if (!StudioAgent::active(job)) { return {}; }
	m_advancingAgent = true;
	struct AdvancingRestore { bool& flag; ~AdvancingRestore() { flag = false; } } advancingRestore{m_advancingAgent};
	const auto event = [&]
	{
		auto state = StudioAgent::inspect(m_state.value("agent").toObject());
		state.insert("event", "agent");
		state.insert("revision", m_state.value("revision"));
		return state;
	};
	const auto pause = [&](const QString& reason)
	{
		auto current = m_state.value("agent").toObject();
		auto currentJob = current.value("job").toObject();
		if (StudioAgent::active(currentJob)) { currentJob.insert("status", "paused"); currentJob.insert("lastError", reason); }
		current.insert("job", currentJob);
		QString error;
		if (!storeAgent(current, error)) { return failure(error); }
		return event();
	};
	if (job.value("expectedRevision") != m_state.value("revision"))
	{
		return pause("Native project changed outside this job; resume to replan against the current revision");
	}
	const int remainingMs = job.value("maximumWallTimeMs").toInt() - job.value("elapsedMs").toInt();
	if (remainingMs < 100) { return pause("Conversation execution reached its elapsed-time budget"); }
	QElapsedTimer elapsed;
	elapsed.start();
	QJsonObject resourcesBefore;
	const auto resourceJob = job;
	QString resourceError;
	QElapsedTimer resourceProbe;
	const auto releaseResourceBudget = qScopeGuard([this] { m_resourceBudget = {}; });
	const auto measureResources = [&](QJsonObject& target) {
		try
		{
			const auto usage = StudioResources::accumulate(target.value("resourceUsage").toObject(), resourcesBefore, sampleResources());
			if (!usage.isEmpty()) { target.insert("resourceUsage", usage); }
		}
		catch (...) {} // Diagnostic failure cannot reverse the native operation.
	};
	const auto failedAdvance = [&](const QString& reason)
	{
		// A stopped session must retain its last acknowledged manifest. In
		// particular, do not save a resumable job over a failed native rollback.
		if (m_stopping) { return failure(reason); }
		auto current = m_state.value("agent").toObject();
		auto currentJob = current.value("job").toObject();
		currentJob.insert("elapsedMs", currentJob.value("elapsedMs").toInt() + static_cast<int>(elapsed.elapsed()));
		measureResources(currentJob);
		if (StudioAgent::active(currentJob)) { currentJob.insert("status", "paused"); currentJob.insert("lastError", resourceError.isEmpty() ? reason : resourceError); }
		current.insert("job", currentJob);
		QString error;
		if (!storeAgent(current, error)) { return failure(error); }
		return event();
	};
	try
	{
		resourcesBefore = sampleResources(); resourceProbe.start();
		resourceError = StudioResources::budgetError(resourceJob, resourcesBefore, resourcesBefore);
		if (!resourceError.isEmpty()) { return pause(resourceError); }
		m_resourceBudget = [&](bool force) {
			if (!resourceError.isEmpty()) { return true; }
			if (!resourceJob.value("maximumCpuTimeMs").toInt() && !resourceJob.value("maximumResidentMiB").toInt()) { return false; }
			if (!force && resourceProbe.elapsed() < 50) { return false; }
			resourceProbe.restart(); resourceError = StudioResources::budgetError(resourceJob, resourcesBefore, sampleResources());
			return !resourceError.isEmpty();
		};
		auto pending = job.value("pending").toArray();
		if (!pending.isEmpty())
		{
			if (job.value("operations").toInt() >= job.value("maxOperations").toInt())
			{
				return pause("Conversation execution reached its native-operation budget");
			}
			const auto call = pending.at(0).toObject();
			auto arguments = call.value("args").toObject();
			const auto operation = StudioAgent::operation(call.value("name").toString(), arguments);
			if (operation == "audio.render")
			{
				const int requested = arguments.contains("timeoutMs") ? arguments.value("timeoutMs").toInt() : 300000;
				if (requested > 0) { arguments.insert("timeoutMs", std::min(requested, remainingMs)); }
			}
			const QJsonObject request{{"id", "@agent/" + job.value("id").toString() + "/" + QString::number(job.value("operations").toInt())},
				{"op", operation.isEmpty() ? QString("unknown.native.function") : operation}, {"args", arguments},
				{"expectedRevision", job.value("expectedRevision")}};
			std::vector<StudioCapabilities::Selection> selections;
			selections.reserve(pending.size() - 1);
			for (int index = 1; index < pending.size(); ++index)
			{
				const auto queued = pending.at(index).toObject(); auto args = queued.value("args").toObject();
				const auto op = StudioAgent::operation(queued.value("name").toString(), args);
				selections.push_back(StudioCapabilities::selection(op, args));
			}
			bool recorded = false;
			const auto response = performRequest(request, [&](QJsonObject& next, const QJsonObject& result)
			{
				auto current = next.value("agent").toObject();
				auto currentJob = current.value("job").toObject();
				StudioAgent::appendToolOutput(currentJob, call, result);
				auto remaining = currentJob.value("pending").toArray();
				remaining.removeFirst();
				const bool projectChanged = operation == "session.undo" || operation == "session.redo"
					|| operation == "project.new" || operation == "project.load" || operation == "project.import";
				// Hosted plugin slots have no native lifetime identity in PluginHost.
				// Reordering/removal must therefore end this inference batch too.
				bool selectionChanged = operation == "host.plugin.remove" || operation == "host.plugin.swap";
				if (result.value("ok").toBool() && !projectChanged && !selectionChanged)
				{
					for (int index = 0; index < remaining.size(); ++index)
					{
						const auto queued = remaining.at(index).toObject(); auto args = queued.value("args").toObject();
						const auto op = StudioAgent::operation(queued.value("name").toString(), args);
						if (selections.at(index).changed(StudioCapabilities::selection(op, args))) { selectionChanged = true; break; }
					}
				}
				if (!result.value("ok").toBool() || projectChanged || selectionChanged)
				{
					// A successful restoration can retarget an existing numeric index,
					// just as failure rollback invalidates handles. Project import also
					// needs observation of its native layout before planned work resumes.
					// Persist every skipped output in the same commit as this result;
					// do not execute or count those calls as native operations.
					const auto reason = !result.value("ok").toBool()
						? "Previous native call failed; rediscover state before retrying"
						: projectChanged ? "Previous native call restored, replaced or imported the project; rediscover state before retrying"
						: "Previous native call changed a queued native selection or hosted plugin layout; rediscover state before retrying";
					for (const auto& skipped : remaining)
					{
						StudioAgent::appendToolOutput(currentJob, skipped.toObject(), failure(reason));
					}
					remaining = {};
				}
				currentJob.insert("pending", remaining);
				currentJob.insert("operations", currentJob.value("operations").toInt() + 1);
				currentJob.insert("expectedRevision", result.value("revision"));
				currentJob.insert("elapsedMs", currentJob.value("elapsedMs").toInt() + static_cast<int>(elapsed.elapsed()));
				measureResources(currentJob);
				if (!resourceError.isEmpty() && StudioAgent::active(currentJob))
				{
					currentJob.insert("status", "paused"); currentJob.insert("lastError", resourceError);
				}
				current.insert("job", currentJob);
				next.insert("agent", current);
				recorded = true;
			});
			if (!response.value("ok").toBool() && m_stopping) { return response; }
			if (!recorded)
			{
				return pause("Native request was rejected before job execution bookkeeping: " + response.value("error").toString());
			}
			return event();
		}
		const auto provider = job.value("provider").toObject();
		if (provider.isEmpty()) { return pause("No reasoning provider is configured; configure one and resume this persisted conversation"); }
		if (job.value("rounds").toInt() >= job.value("maxRounds").toInt())
		{
			return pause("Conversation execution reached its inference-round budget");
		}
		auto input = job.value("input").toArray();
		if (input.isEmpty())
		{
			if (!job.value("contextSummary").toString().isEmpty())
			{
				// The working summary is supplied in the native observation below.
				// Replaying the old transcript here would undo context compaction.
				input.append(QJsonObject{{"role", "user"}, {"content", job.value("intent")}});
			}
			else
			{
				if (agent.value("transcriptOffset").toInt() > 0)
				{
					input.append(QJsonObject{{"role", "user"}, {"content", "Native conversation metadata (data): "
						+ QString::number(agent.value("transcriptOffset").toInt()) + " earlier messages are archived; use conversation_history when their details are needed."}});
				}
				const auto transcript = agent.value("transcript").toArray();
				for (int i = std::max(0, static_cast<int>(transcript.size()) - 64); i < transcript.size(); ++i)
				{
					const auto message = transcript.at(i).toObject();
					input.append(QJsonObject{{"role", message.value("role")}, {"content", message.value("content")}});
				}
			}
			job.insert("input", input);
		}
		job.insert("rounds", job.value("rounds").toInt() + 1);
		job.insert("status", "running");
		agent.insert("job", job);
		QString error;
		// Reserve the round before external inference. Crash recovery may repeat
		// inference, but cannot evade the budget or replay an uncommitted native edit.
		if (!storeAgent(agent, error)) { return failure(error); }
		// Committing may move large native outputs to immutable archives. Use the
		// committed projection for inference, not the pre-publication local copy.
		job = m_state.value("agent").toObject().value("job").toObject();
		input = job.value("input").toArray();
		const auto cancelled = [&]
		{
			return m_stopping || interruptionRequested() || elapsed.elapsed() >= remainingMs
				|| !StudioAgent::active(m_state.value("agent").toObject().value("job").toObject())
				|| (m_resourceBudget && m_resourceBudget(false));
		};
		QJsonObject inspected;
		{
			FileResourceIO::Scope observationWork(cancelled);
			inspected = StudioCapabilities::execute("project.inspect", {{"section", "summary"}});
		}
		if (!inspected.value("ok").toBool()) { throw std::runtime_error("Could not observe the native project for reasoning"); }
		input.append(StudioAgent::observation(inspected.value("result").toObject(), m_state.value("memory").toObject(), job,
			!m_state.value("providers").toObject().value("audio").toObject().isEmpty(), sampleResources()));
		auto boundedProvider = provider;
		boundedProvider.insert("timeoutMs", std::min(provider.value("timeoutMs").toInt(), remainingMs));
		const auto decision = StudioInference::generate(boundedProvider, input,
			StudioAgent::tools(provider.value("protocol").toString()), StudioAgent::instructions(), cancelled, m_transport);
		if (m_resourceBudget(true)) { throw std::runtime_error(resourceError.toStdString()); }
		agent = m_state.value("agent").toObject();
		job = agent.value("job").toObject();
		if (!StudioAgent::active(job)) { return event(); }
		QSet<QString> priorCalls;
		for (const auto& value : job.value("records").toArray())
		{
			const auto record = value.toObject();
			if (record.value("kind").toString() == "tool") { priorCalls.insert(record.value("call").toObject().value("id").toString()); }
			else if (record.value("kind").toString() == "archivedTool") { priorCalls.insert(record.value("id").toString()); }
		}
		for (const auto& value : decision.value("calls").toArray())
		{
			if (priorCalls.contains(value.toObject().value("id").toString()))
			{
				throw std::runtime_error("Provider reused a function-call identifier from this job");
			}
		}
		const int responseStart = input.size();
		for (const auto& item : decision.value("continuation").toArray()) { input.append(item); }
		job.insert("input", input);
		job.insert("pending", decision.value("calls"));
		job.insert("elapsedMs", job.value("elapsedMs").toInt() + static_cast<int>(elapsed.elapsed()));
		measureResources(job);
		auto records = job.value("records").toArray();
		records.append(QJsonObject{{"kind", "inference"}, {"round", job.value("rounds")}, {"revision", job.value("expectedRevision")},
			{"context", job.value("pastInputs").toArray().size()}, {"responseStart", responseStart}, {"responseEnd", input.size()}, {"usage", decision.value("usage")}});
		job.insert("records", records);
		const auto text = decision.value("text").toString();
		if (!text.isEmpty())
		{
			auto transcript = agent.value("transcript").toArray();
			transcript.append(QJsonObject{{"role", "assistant"}, {"content", text}, {"job", job.value("id")},
				{"at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}});
			agent.insert("transcript", transcript);
		}
		if (decision.value("calls").toArray().isEmpty())
		{
			job.insert("status", "paused");
			job.insert("lastError", "Provider ended without declaring a task outcome; its text is retained, but production completion is unproven");
		}
		agent.insert("job", job);
		if (!storeAgent(agent, error)) { return failure(error); }
		return event();
	}
	catch (const std::exception& exception)
	{
		return failedAdvance(QString::fromUtf8(exception.what()));
	}
	catch (...) { return failedAdvance("Native agent transition threw a nonstandard C++ exception"); }
}

bool StudioRuntime::interruptionRequested() const
{
	return m_interrupted && m_interrupted();
}

void StudioRuntime::pollInterruption()
{
	if (!m_open || m_stopping || !interruptionRequested()) { return; }
	auto response = process({{"id", "@interrupt/" + QUuid::createUuid().toString(QUuid::WithoutBraces)},
		{"op", "session.shutdown"}, {"args", QJsonObject{}}});
	response.insert("event", "shutdown"); response.insert("reason", "process interrupt");
	output(response);
	// A rejected shutdown must not admit further work while the interrupt is set.
	if (!response.value("ok").toBool()) { m_stopping = m_failed = true; }
}

int StudioRuntime::run()
{
	if (!onApplicationThread()) { return EXIT_FAILURE; }
	QString error;
	{
		FileResourceIO::Scope startup([&] { return interruptionRequested(); });
		if (interruptionRequested() || !open(error))
		{
			output(failure(error.isEmpty() ? QString("Studio startup interrupted") : error)); return EXIT_FAILURE;
		}
	}
	auto ready = sessionInfo();
	ready.insert("event", "ready");
	ready.insert("applicationType", QCoreApplication::instance()->metaObject()->className());
	ready.insert("transport", StudioCapabilities::execute("transport.inspect", {}).value("result"));
	output(ready);
	if (m_failed) { return EXIT_FAILURE; }
	pollInterruption();
	if (m_stopping) { return m_failed ? EXIT_FAILURE : EXIT_SUCCESS; }
	StudioStdio input;
	QQueue<StudioStdio::Frame> frames;
	QEventLoop loop;
	QTimer inputTimer;
	QTimer agentTimer;
	int dispatching = 0;
	const auto finishIfReady = [&]
	{
		pollInterruption();
		if (dispatching == 0 && !m_advancingAgent && m_activeRequestIds.isEmpty() && (m_stopping
			|| (input.ended() && frames.isEmpty() && !StudioAgent::active(m_state.value("agent").toObject().value("job").toObject())))) { loop.quit(); }
	};
	QObject::connect(&inputTimer, &QTimer::timeout, &loop, [&]
	{
		pollInterruption();
		if (m_stopping) { finishIfReady(); return; }
		if (frames.size() < 64)
		{
			for (const auto& frame : input.readAvailable(64 - frames.size())) { frames.enqueue(frame); }
		}
		if (!input.error().isEmpty())
		{
			output(failure(input.error()));
			m_stopping = m_failed = true;
			loop.quit();
			return;
		}
		if (!frames.isEmpty() && !m_stopping)
		{
			const auto frame = frames.dequeue();
			++dispatching;
			// Return from the polling callback before execution. The same timer can
			// then receive control requests while a native render runs a Qt loop.
			QTimer::singleShot(0, &loop, [&, frame] {
				pollInterruption();
				if (!m_stopping)
				{
					if (frame.oversized) { output(failure("Request exceeds the 1 MiB limit")); }
					else if (!frame.bytes.trimmed().isEmpty())
					{
						QJsonParseError parse;
						const auto document = QJsonDocument::fromJson(frame.bytes, &parse);
						if (parse.error == QJsonParseError::NoError && document.isObject()) { output(process(document.object())); }
						else if (frame.bytes.trimmed().startsWith('{') || frame.bytes.trimmed().startsWith('['))
						{
							output(failure("Structured input must be one valid JSON request object per line"));
						}
						else
						{
							const auto text = QString::fromUtf8(frame.bytes);
							if (text.toUtf8() != frame.bytes) { output(failure("Conversation input must be valid UTF-8")); }
							else
							{
								output(process({{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"op", "session.chat"},
									{"args", QJsonObject{{"text", text}}}}));
							}
						}
					}
				}
				--dispatching;
				finishIfReady();
			});
		}
		finishIfReady();
	});
	QObject::connect(&agentTimer, &QTimer::timeout, &loop, [&]
	{
		pollInterruption();
		if (!m_stopping && frames.isEmpty() && dispatching == 0)
		{
			const auto event = advanceAgent();
			if (!event.isEmpty()) { output(event); }
		}
		finishIfReady();
	});
	inputTimer.start(10);
	agentTimer.start(25);
	loop.exec();
	return m_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

} // namespace lmms
