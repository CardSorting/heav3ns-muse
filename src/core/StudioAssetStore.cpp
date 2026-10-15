/* Native checkpoint media storage. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioAssetStore.h"
#include "FileResourceSnapshot.h"
#include "FileResourceIO.h"

#include <cmath>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryFile>
#include <QTemporaryDir>

namespace lmms
{
namespace
{

constexpr double MaxExactBytes = 9007199254740991.;

bool matches(const QString& path, const QString& hash, qint64 bytes, QString* error = nullptr)
{
	QString reason;
	if (!FileResourceIO::check(reason)) { if (error) { *error = reason; } return false; }
	const QFileInfo file(path);
	if (!file.isFile() || file.isSymLink() || file.size() != bytes) { return false; }
	QString checksum;
	if (!FileResourceIO::hashFile(path, checksum, reason)) { if (error) { *error = reason; } return false; }
	return checksum == hash;
}

bool validBytes(const QJsonValue& value)
{
	return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0
		&& value.toDouble() <= MaxExactBytes && value.toDouble() == static_cast<qint64>(value.toDouble());
}

QList<FileResourceSnapshot::Member> groupMembers(const QJsonObject& reference)
{
	QList<FileResourceSnapshot::Member> members;
	for (const auto& value : reference.value("members").toArray())
	{
		const auto item = value.toObject();
		members.append({item.value("name").toString(), item.value("sha256").toString(), static_cast<qint64>(item.value("bytes").toDouble())});
	}
	return members;
}

bool preserveGroup(const QString& directory, QJsonObject& reference, QString& error, const FileResourceSnapshot& accepted)
{
	if (!accepted.verify(error)) { return false; }
	QJsonArray members;
	for (const auto& member : accepted.members())
	{
		if (member.bytes < 0 || static_cast<double>(member.bytes) > MaxExactBytes)
		{
			error = "Native resource group member size cannot be represented exactly"; return false;
		}
		members.append(QJsonObject{{"name", member.name}, {"sha256", member.sha256}, {"bytes", static_cast<double>(member.bytes)}});
	}
	const auto digest = accepted.groupDigest();
	const auto target = QDir(directory).filePath(digest);
	if (!QFileInfo::exists(target))
	{
		QTemporaryDir staging(QDir(directory).filePath(".group-XXXXXX"));
		if (!staging.isValid() || !accepted.copyGroup(staging.path(), error))
		{
			if (error.isEmpty()) { error = "Could not stage native resource group"; } return false;
		}
		if (!FileResourceIO::check(error)) { return false; }
		if (QDir().rename(staging.path(), target)) { staging.setAutoRemove(false); }
		else if (!QFileInfo::exists(target)) { error = "Could not publish native resource group"; return false; }
	}
	if (QDir(target).canonicalPath() != QDir::cleanPath(QDir(directory).canonicalPath() + "/" + digest)
		|| !FileResourceSnapshot::verifyMembers(target, accepted.members(), error))
	{
		if (error.isEmpty()) { error = "Native resource group is outside its asset directory"; } return false;
	}
	reference = {{"path", "assets/" + digest + "/" + accepted.members().first().name},
		{"sha256", accepted.sha256()}, {"bytes", static_cast<double>(accepted.bytes())}, {"members", members}};
	error.clear(); return true;
}

} // namespace

bool StudioAssetStore::preserve(const QString& source, const QString& directory, const QString& suffix,
	QJsonObject& reference, QString& error, const FileResourceSnapshot* accepted)
{
	reference = {};
	if (!FileResourceIO::check(error)) { return false; }
	if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir())
	{
		error = "Checkpoint asset storage is unavailable"; return false;
	}
	if (accepted && accepted->isGroup())
	{
		// Group layouts are copied from the accepted owner, never rediscovered from
		// a mutable caller path after the native loader has accepted them.
		if (source != accepted->path()) { error = "Native resource group source does not match its accepted owner"; return false; }
		return preserveGroup(directory, reference, error, *accepted);
	}
	QFile input(source);
	if (!input.open(QIODevice::ReadOnly)) { error = "Could not read checkpoint asset source"; return false; }
	const qint64 expected = input.size();
	if (expected < 0 || static_cast<double>(expected) > MaxExactBytes)
	{
		error = "Checkpoint asset size cannot be represented exactly"; return false;
	}
	if (accepted && expected != accepted->bytes())
	{
		error = "Accepted native resource size changed before checkpointing"; return false;
	}
	QTemporaryFile staging(QDir(directory).filePath(".staging-XXXXXX"));
	if (!staging.open()) { error = "Could not create checkpoint asset staging file"; return false; }
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!FileResourceIO::stream(input, &staging, &hash, expected, error)) { return false; }
	if (!staging.flush())
	{
		error = "Checkpoint asset changed or could not be finalized"; return false;
	}
	const auto checksum = QString::fromLatin1(hash.result().toHex());
	if (accepted && checksum != accepted->sha256())
	{
		error = "Accepted native resource bytes changed before checkpointing"; return false;
	}
	static const QRegularExpression validSuffix("^[a-z0-9]{1,16}$");
	const auto extension = validSuffix.match(suffix.toLower()).hasMatch() ? suffix.toLower() : QString("bin");
	const auto name = checksum + "." + extension;
	const auto target = QDir(directory).filePath(name);
	staging.close();
	if (!FileResourceIO::check(error)) { return false; }
	const bool existing = QFileInfo::exists(target);
	if (!existing && staging.rename(target)) { staging.setAutoRemove(false); }
	else if (!existing && !matches(target, checksum, expected, &error))
	{
		if (error.isEmpty()) { error = "Could not publish checkpoint asset"; } return false;
	}
	if (!matches(target, checksum, expected, &error))
	{
		if (error.isEmpty()) { error = existing ? "Existing checkpoint asset is altered" : "Published checkpoint asset failed verification"; }
		return false;
	}
	reference = {{"path", "assets/" + name}, {"sha256", checksum}, {"bytes", static_cast<double>(expected)}};
	error.clear(); return true;
}

bool StudioAssetStore::validReferences(const QJsonArray& references)
{
	static const QRegularExpression pathPattern("^assets/([0-9a-f]{64})\\.[a-z0-9]{1,16}$");
	static const QRegularExpression groupPattern("^assets/([0-9a-f]{64})/([^/]+)$");
	static const QRegularExpression hashPattern("^[0-9a-f]{64}$");
	QSet<QString> paths;
	for (const auto& value : references)
	{
		if (!value.isObject()) { return false; }
		const auto item = value.toObject();
		const auto path = item.value("path").toString();
		const auto match = pathPattern.match(path);
		const auto bytes = item.value("bytes");
		if (!validBytes(bytes) || paths.contains(path)) { return false; }
		if (item.contains("members"))
		{
			const auto group = groupPattern.match(path);
			const auto values = item.value("members").toArray();
			if (item.size() != 4 || !group.hasMatch() || !item.value("members").isArray() || values.isEmpty() || values.size() > 4098) { return false; }
			QSet<QString> names;
			QString previous;
			for (int index = 0; index < values.size(); ++index)
			{
				if (!values[index].isObject()) { return false; }
				const auto member = values[index].toObject();
				const auto name = member.value("name").toString();
				if (member.size() != 3 || !FileResourceSnapshot::safeRelativeName(name) || names.contains(name.toCaseFolded())
					|| !hashPattern.match(member.value("sha256").toString()).hasMatch() || !validBytes(member.value("bytes"))) { return false; }
				if (index == 0)
				{
					if (name != group.captured(2) || member.value("sha256") != item.value("sha256") || member.value("bytes") != bytes) { return false; }
				}
				else if (index > 1 && QString::compare(previous, name, Qt::CaseSensitive) >= 0) { return false; }
				names.insert(name.toCaseFolded()); previous = name;
			}
			if (FileResourceSnapshot::memberDigest(groupMembers(item)) != group.captured(1)) { return false; }
		}
		else if (item.size() != 3 || !match.hasMatch() || item.value("sha256").toString() != match.captured(1)) { return false; }
		paths.insert(path);
	}
	return true;
}

bool StudioAssetStore::verify(const QString& checkpointDirectory, const QJsonArray& references, QString& error)
{
	if (!FileResourceIO::check(error)) { return false; }
	if (!validReferences(references)) { error = "Checkpoint asset references are invalid"; return false; }
	const QDir root(checkpointDirectory);
	const auto directory = root.filePath("assets");
	if (!references.isEmpty() && (QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir()
		|| QDir(directory).canonicalPath() != QDir::cleanPath(root.canonicalPath() + "/assets")))
	{
		error = "Checkpoint asset storage is outside its owning directory"; return false;
	}
	for (const auto& value : references)
	{
		const auto item = value.toObject();
		if (item.contains("members"))
		{
			const auto groupDirectory = QFileInfo(root.filePath(item.value("path").toString())).absolutePath();
			if (QDir(groupDirectory).canonicalPath() != QDir::cleanPath(QDir(directory).canonicalPath() + "/" + QFileInfo(groupDirectory).fileName())
				|| !FileResourceSnapshot::verifyMembers(groupDirectory, groupMembers(item), error))
			{
				if (error.isEmpty()) { error = "Checkpoint resource group is outside its owning directory"; } return false;
			}
			continue;
		}
		if (!matches(root.filePath(item.value("path").toString()), item.value("sha256").toString(),
			static_cast<qint64>(item.value("bytes").toDouble()), &error))
		{
			if (error.isEmpty()) { error = "Checkpoint asset is missing or altered; existing project was preserved"; } return false;
		}
	}
	error.clear(); return true;
}

} // namespace lmms
