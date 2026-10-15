/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioStorage.h"
#include "FileResourceIO.h"

#include <algorithm>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>

namespace lmms
{
namespace
{
QMap<QString, QString> retainedFiles(const QJsonObject& state)
{
	QMap<QString, QString> result;
	for (const auto& value : state.value("history").toArray())
	{
		const auto entry = value.toObject(); result.insert(entry.value("path").toString(), entry.value("sha256").toString());
	}
	for (const auto& value : state.value("requests").toArray())
	{
		const auto entry = value.toObject().value("archive").toObject();
		if (!entry.isEmpty()) { result.insert(entry.value("path").toString(), entry.value("sha256").toString()); }
	}
	return result;
}

bool validReference(const QJsonObject& reference)
{
	static const QString uuid = "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}";
	static const QRegularExpression path("^(?:checkpoints/" + uuid + "\\.mmp|receipts/" + uuid + "\\.json)$");
	static const QRegularExpression hash("^[0-9a-f]{64}$");
	return reference.size() == 2 && path.match(reference.value("path").toString()).hasMatch()
		&& hash.match(reference.value("sha256").toString()).hasMatch();
}

bool contained(const QDir& directory, const QString& root, const QString& relative)
{
	// QDir may cache filesystem metadata across passes; inspect the root afresh.
	if (root.isEmpty() || QFileInfo(directory.absolutePath()).canonicalFilePath() != root) { return false; }
	const auto parent = relative.section('/', 0, 0);
	const QFileInfo parentInfo(directory.filePath(parent));
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
	if (parentInfo.isJunction()) { return false; }
#endif
	return !parentInfo.isSymLink() && parentInfo.isDir()
		&& parentInfo.canonicalFilePath() == QDir::cleanPath(root + "/" + parent);
}
}

bool StudioStorage::validRetirements(const QJsonObject& state)
{
	if (!state.contains("retiredFiles")) { return true; }
	if (!state.value("retiredFiles").isArray()) { return false; }
	const auto retired = state.value("retiredFiles").toArray();
	if (retired.size() > MaximumRetiredFiles) { return false; }
	const auto retained = retainedFiles(state); QSet<QString> seen;
	for (const auto& value : retired)
	{
		const auto reference = value.toObject(); const auto path = reference.value("path").toString();
		if (!value.isObject() || !validReference(reference) || seen.contains(path) || retained.contains(path)) { return false; }
		seen.insert(path);
	}
	return true;
}

bool StudioStorage::prepare(const QJsonObject& previous, QJsonObject& next,
	const QSet<QString>& collected, QString& error)
{
	if (!validRetirements(previous)) { error = "Invalid committed file retirement queue"; return false; }
	const auto retained = retainedFiles(next); QJsonArray retired; QSet<QString> seen;
	for (const auto& value : previous.value("retiredFiles").toArray())
	{
		const auto path = value.toObject().value("path").toString();
		if (retained.contains(path)) { error = "A retired session file cannot become current again"; return false; }
		if (!collected.contains(path)) { retired.append(value); seen.insert(path); }
	}
	const auto oldFiles = retainedFiles(previous);
	for (auto entry = oldFiles.begin(); entry != oldFiles.end(); ++entry)
	{
		if (retained.contains(entry.key()))
		{
			if (retained.value(entry.key()) != entry.value()) { error = "An immutable session file changed identity"; return false; }
			continue;
		}
		const QJsonObject reference{{"path", entry.key()}, {"sha256", entry.value()}};
		if (!validReference(reference)) { error = "Cannot retire an invalid session file reference"; return false; }
		if (!seen.contains(entry.key())) { retired.append(reference); seen.insert(entry.key()); }
	}
	if (retired.size() > MaximumRetiredFiles) { error = "Session file retirement queue exceeds the 4096-file limit"; return false; }
	if (retired.isEmpty()) { next.remove("retiredFiles"); } else { next.insert("retiredFiles", retired); }
	error.clear(); return true;
}

QJsonObject StudioStorage::collect(const QDir& directory, const QString& canonicalRoot,
	const QJsonObject& committed, QSet<QString>& collected, int& cursor,
	const std::function<bool()>& cancelled)
{
	if (!validRetirements(committed)) { return {{"error", "Invalid committed file retirement queue"}}; }
	// A commit is already published. Cleanup must not report it as uncommitted,
	// reenter the runtime, inherit an operation's timeout, or prevent its rollback.
	FileResourceIO::Suspension outer; FileResourceIO::Scope work(cancelled);
	const auto retired = committed.value("retiredFiles").toArray(); QJsonArray errors;
	int removed = 0, absent = 0, attempted = 0; double removedBytes = 0;
	const int count = retired.size(), limit = std::min(count, 16);
	if (count) { cursor %= count; } else { cursor = 0; }
	for (int scanned = 0; scanned < limit; ++scanned)
	{
		QString error; if (!FileResourceIO::check(error)) { errors.append(QJsonObject{{"error", error}}); break; }
		const auto reference = retired.at(cursor).toObject(); cursor = (cursor + 1) % count;
		const auto relative = reference.value("path").toString(); if (collected.contains(relative)) { continue; }
		++attempted;
		const auto defer = [&](const QString& reason) { errors.append(QJsonObject{{"path", relative}, {"error", reason}}); };
		if (!contained(directory, canonicalRoot, relative)) { defer("Retired file directory is unavailable or outside its owner"); continue; }
		const auto path = directory.filePath(relative); const QFileInfo before(path);
		if (before.isSymLink()) { defer("Retired file is a symbolic link"); continue; }
		if (!before.exists()) { collected.insert(relative); ++absent; continue; }
		if (!before.isFile()) { defer("Retired file is not a regular file"); continue; }
		const auto expectedBytes = before.size(); const auto expectedModified = before.lastModified();
		QString digest;
		if (!FileResourceIO::hashFile(path, digest, error)) { defer(error); continue; }
		if (digest != reference.value("sha256").toString()) { defer("Retired file was altered; retained for inspection"); continue; }
		const QFileInfo verified(path);
		if (!FileResourceIO::check(error)) { defer(error); break; }
		if (!contained(directory, canonicalRoot, relative) || verified.isSymLink() || !verified.isFile()
			|| verified.size() != expectedBytes || verified.lastModified() != expectedModified
			|| verified.canonicalFilePath() != QDir::cleanPath(canonicalRoot + "/" + relative))
		{
			defer("Retired file changed during verification"); continue;
		}
		if (!QFile::remove(path)) { defer("Could not remove verified retired file"); continue; }
		collected.insert(relative); ++removed; removedBytes += static_cast<double>(expectedBytes);
	}
	int pending = 0;
	for (const auto& value : retired) { if (!collected.contains(value.toObject().value("path").toString())) { ++pending; } }
	return {{"attempted", attempted}, {"removed", removed}, {"alreadyAbsent", absent}, {"removedBytes", removedBytes},
		{"pending", pending}, {"errors", errors}};
}
} // namespace lmms
