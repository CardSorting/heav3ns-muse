/* Native asset root and filename observations. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioAssets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
#include <utility>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QThread>
#include <QUuid>
#include "AudioEngine.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "FileSearch.h"
#include "PathUtil.h"
#include "Song.h"
namespace lmms
{
namespace
{
[[noreturn]] void fail(const char* reason) { throw std::invalid_argument(reason); }
void work() { QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); } }
int integer(const QJsonObject& args, const char* key, int fallback, int minimum, int maximum)
{ if (!args.contains(key)) { return fallback; } const auto value = args.value(key); const double number = value.toDouble(-1); if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum) { fail("Asset search page/budget is outside its native bounds"); } return static_cast<int>(number); }
bool boolean(const QJsonObject& args, const char* key, bool fallback)
{ if (!args.contains(key)) { return fallback; } if (!args.value(key).isBool()) { fail("Asset search flag must be boolean"); } return args.value(key).toBool(); }
QString string(const QJsonObject& args, const char* key, const QString& fallback)
{ if (!args.contains(key)) { return fallback; } if (!args.value(key).isString()) { fail("Asset search text must be a string"); } const auto value = args.value(key).toString(); if (value.contains(QChar::Null)) { fail("Asset search text cannot contain a null character"); } return value; }
bool bounded(const QString& value, int maximum)
{ int points = 0; for (decltype(value.size()) index = 0; index < value.size(); ++index) { if (++points > maximum) { return false; } if (value.at(index).isHighSurrogate() && index + 1 < value.size() && value.at(index + 1).isLowSurrogate()) { ++index; } } return true; }
void text(QJsonObject& target, const QString& key, const QString& value, bool names)
{ if (names || value.size() <= 256) { target.insert(key, value); return; } auto shortValue = value.left(256); if (shortValue.at(shortValue.size() - 1).isHighSurrogate()) { shortValue.chop(1); } target.insert(key + "Preview", shortValue); target.insert(key + "Length", static_cast<double>(value.size())); target.insert(key + "Truncated", true); }
void fields(const QJsonObject& args, const QStringList& allowed)
{ for (auto it = args.begin(); it != args.end(); ++it) { if (!allowed.contains(it.key())) { fail("Unknown native asset discovery argument"); } } }
struct Root { QString id, prefix, path; bool resolved = false; bool operator==(const Root&) const = default; };
std::vector<Root> definitions()
{
	using Base = PathUtil::Base;
	const auto bases = std::array{Base::UserSample, Base::FactorySample, Base::Preset, Base::FactoryPresets, Base::ProjectDir, Base::FactoryProjects, Base::UserSoundfont, Base::DefaultSoundfont, Base::UserGIG, Base::DefaultGIG, Base::UserVST, Base::UserLADSPA, Base::DefaultLADSPA, Base::LocalDir};
	std::vector<Root> result;
	for (auto base : bases)
	{
		bool error = false; const auto location = PathUtil::baseLocation(base, &error); const auto prefix = PathUtil::basePrefix(base); const auto path = error ? QString{} : QDir::cleanPath(QFileInfo(location).absoluteFilePath());
		result.push_back({prefix.left(prefix.size() - 1), prefix, path, !error && !path.isEmpty()});
	}
	return result;
}
class Context
{
public:
	Context()
	{
		if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() || !Engine::audioEngine() || !Engine::getSong()) { fail("Asset discovery requires an initialized native engine on the application thread"); }
		if (!Engine::audioEngine()->canJoinBackendCallbacks()) { fail("Asset discovery must start outside native model changes and audio callbacks"); }
		const auto changes = Engine::audioEngine()->requestChangesGuard(); m_engine = Engine::audioEngine(); m_song = Engine::getSong(); m_project = m_song->replacementGeneration(); m_filename = m_song->projectFileName(); m_roots = definitions(); check();
	}
	void check() const
	{ if (!m_engine || Engine::audioEngine() != m_engine.data() || !m_song || Engine::getSong() != m_song.data() || m_song->thread() != QThread::currentThread() || m_song->replacementGeneration() != m_project || m_song->projectFileName() != m_filename) { fail("Native project changed during asset discovery"); } }
	void poll() const { work(); check(); if (!m_engine->canJoinBackendCallbacks()) { fail("Asset discovery cannot continue inside a native model edit"); } }
	void finish() const
	{ check(); if (!m_engine->canJoinBackendCallbacks()) { fail("Asset discovery cannot finish inside a native model edit"); } const auto changes = m_engine->requestChangesGuard(); if (m_roots != definitions()) { fail("Native asset base paths changed during discovery"); } check(); }
	const std::vector<Root>& roots() const { return m_roots; }
	Root resolve(const QString& selector) const
	{
		if (selector.isEmpty()) { fail("Choose a native asset root or an explicit absolute/prefixed directory"); }
		for (const auto& root : m_roots) { if (selector == root.id || selector == root.prefix) { if (!root.resolved) { fail("Native asset base is unavailable; choose another root"); } return root; } }
		if (!QDir::isAbsolutePath(selector) && PathUtil::baseLookup(selector) == PathUtil::Base::Absolute) { fail("Unknown asset root; use asset.roots or an absolute/prefixed directory"); }
		bool error = false; const auto path = PathUtil::toAbsolute(selector, &error); if (error || path.isEmpty()) { fail("Asset search directory could not be resolved"); }
		return {"explicit", {}, QDir::cleanPath(QFileInfo(path).absoluteFilePath()), true};
	}
	QString projectFile() const { return m_filename; }
private:
	QPointer<AudioEngine> m_engine; QPointer<Song> m_song; std::uint64_t m_project = 0; QString m_filename; std::vector<Root> m_roots;
};
QJsonObject rootObject(const Root& root, bool names)
{
	QJsonObject result{{"id", root.id}, {"prefix", root.prefix}, {"resolved", root.resolved}};
	if (!root.resolved) { result.insert("status", "unresolvedBase"); result.insert("path", QJsonValue::Null); return result; }
	const QFileInfo file(root.path); result.insert("status", !file.exists() ? "missing" : !file.isDir() ? "notDirectory" : !file.isReadable() ? "unreadable" : "readableDirectory"); result.insert("symbolicLink", file.isSymLink());
	text(result, "path", root.path, names); text(result, "canonical", file.canonicalFilePath(), names); return result;
}
QJsonObject entryObject(const QFileInfo& file)
{
	const bool regular = file.isFile(); const auto size = regular ? file.size() : -1;
	return {{"path", file.absoluteFilePath()}, {"name", file.fileName()}, {"kind", file.isDir() ? "directory" : regular ? "file" : "other"}, {"readable", file.isReadable()}, {"symbolicLink", file.isSymLink()},
		{"bytes", size >= 0 && size <= 9007199254740991LL ? QJsonValue(static_cast<double>(size)) : QJsonValue(QJsonValue::Null)}, {"bytesText", size >= 0 ? QJsonValue(QString::number(size)) : QJsonValue(QJsonValue::Null)}, {"modified", file.lastModified().toUTC().toString(Qt::ISODateWithMs)}};
}
void hashText(QCryptographicHash& hash, const QString& value, const Context& context)
{
	hash.addData(QByteArray::number(static_cast<qulonglong>(value.size())) + ':'); const auto data = reinterpret_cast<const char*>(value.constData()); const auto bytes = static_cast<std::size_t>(value.size()) * sizeof(QChar);
	for (std::size_t offset = 0; offset < bytes;) { context.poll(); const auto count = std::min<std::size_t>(8192, bytes - offset); hash.addData(QByteArray::fromRawData(data + offset, static_cast<int>(count))); offset += count; }
}
}

QJsonObject StudioAssets::roots(const QJsonObject& arguments)
{
	fields(arguments, {"names"}); const bool names = boolean(arguments, "names", true); FileResourceIO::EventSuspension events; Context context; QJsonArray roots;
	for (const auto& root : context.roots()) { context.poll(); roots.append(rootObject(root, names)); }
	context.finish(); QJsonObject result{{"roots", roots}, {"scope", "current native PathUtil bases; discovery does not create directories or verify asset content"}}; text(result, "projectFile", context.projectFile(), names);
	if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 512 * 1024) { fail("Native asset root metadata exceeds 512 KiB; request names=false"); } return result;
}

QJsonObject StudioAssets::search(const QJsonObject& arguments)
{
	fields(arguments, {"root", "query", "extensions", "kind", "recursive", "followSymlinks", "hidden", "maximumEntries", "offset", "limit", "names", "searchGeneration"});
	const QString selected = string(arguments, "root", {}), query = string(arguments, "query", {}), kind = string(arguments, "kind", "files"), expected = string(arguments, "searchGeneration", {});
	if (selected.isEmpty()) { fail("Choose a native asset root or an explicit absolute/prefixed directory"); }
	if (!bounded(query, 4096)) { fail("Asset filename query exceeds 4096 Unicode characters"); }
	if (kind != "files" && kind != "directories" && kind != "all") { fail("Asset search kind must be files, directories or all"); }
	if (arguments.contains("searchGeneration") && !QRegularExpression("^[0-9a-f]{64}$").match(expected).hasMatch()) { fail("Invalid native asset search observation token"); }
	const int offset = integer(arguments, "offset", 0, 0, std::numeric_limits<int>::max()), limit = integer(arguments, "limit", 128, 1, 1024), maximum = integer(arguments, "maximumEntries", 100000, 1, 1000000);
	const bool recursive = boolean(arguments, "recursive", true), follow = boolean(arguments, "followSymlinks", false), hidden = boolean(arguments, "hidden", false), names = boolean(arguments, "names", true);
	QStringList extensions{"*"};
	if (arguments.contains("extensions"))
	{
		if (!arguments.value("extensions").isArray()) { fail("Asset extensions must be an array"); } const auto input = arguments.value("extensions").toArray(); if (input.isEmpty() || input.size() > 64) { fail("Asset extension lists require 1..64 entries"); } extensions.clear();
		for (const auto& value : input)
		{
			if (!value.isString() || !bounded(value.toString(), 64)) { fail("Asset extensions must be strings of at most 64 Unicode characters"); } auto extension = value.toString().toLower();
			if (extension == "*") { extensions.append(extension); continue; } if (extension.startsWith("*.")) { extension.remove(0, 2); } else if (extension.startsWith('.')) { extension.remove(0, 1); }
			if (extension.isEmpty()) { fail("An asset extension cannot be empty"); } for (const auto character : extension) { if (character.isSpace() || character == '/' || character == '\\' || character == '*' || character == '?' || character == ':' || character == QChar::Null) { fail("Use literal suffixes or * in asset extensions"); } } extensions.append("*." + extension);
		}
	}
	FileResourceIO::EventSuspension events; Context context; context.poll(); const auto root = context.resolve(selected); const QFileInfo directory(root.path);
	if (!directory.isDir() || !directory.isReadable()) { fail("Asset search root is not a readable directory"); }
	const auto rootObservation = rootObject(root, true);
	FileSearch::Task task; task.filter = query; task.paths = {root.path}; task.extensions = extensions; task.recursive = recursive; task.followSymlinks = follow; task.includeFiles = kind != "directories"; task.includeDirectories = kind != "files"; task.maximumEntries = maximum; if (hidden) { task.dirFilters |= QDir::Hidden; }
	static const auto process = QUuid::createUuid().toString(QUuid::WithoutBraces); QCryptographicHash digest(QCryptographicHash::Sha256);
	hashText(digest, process, context); hashText(digest, selected, context); hashText(digest, root.path, context); hashText(digest, query, context); for (const auto& extension : extensions) { hashText(digest, extension, context); }
	digest.addData(QJsonDocument(QJsonObject{{"kind", kind}, {"recursive", recursive}, {"followSymlinks", follow}, {"hidden", hidden}, {"maximumEntries", maximum}, {"root", rootObservation}}).toJson(QJsonDocument::Compact));
	QJsonArray matches; qint64 bytes = 2; int matched = 0; bool full = false;
	FileSearch::Callbacks callbacks; callbacks.cancelled = [&] { context.poll(); return false; };
	callbacks.entry = [&](const QFileInfo& file, bool match) {
		context.check(); const auto observed = entryObject(file); digest.addData(QJsonDocument(observed).toJson(QJsonDocument::Compact));
		if (!match) { return; } const int index = matched++;
		if (index < offset || matches.size() >= limit || full) { return; }
		auto row = observed; const auto path = row.take("path").toString(), name = row.take("name").toString(); text(row, "path", path, names); text(row, "name", name, names); text(row, "relative", QDir(root.path).relativeFilePath(path), names); row.insert("index", index);
		const qint64 length = static_cast<qint64>(QJsonDocument(row).toJson(QJsonDocument::Compact).size()) + 1; if (bytes + length > 512 * 1024) { if (matches.isEmpty()) { fail("An asset match exceeds 512 KiB; request names=false"); } full = true; return; } matches.append(row); bytes += length;
	};
	const auto scanned = FileSearch::scan(std::move(task), std::move(callbacks)); context.poll(); context.finish();
	digest.addData(QJsonDocument(QJsonObject{{"entries", static_cast<double>(scanned.entries)}, {"matches", static_cast<double>(scanned.matches)}, {"limited", scanned.limited}, {"unavailableRoots", static_cast<double>(scanned.unavailableRoots)}, {"unreadableDirectories", static_cast<double>(scanned.unreadableDirectories)}}).toJson(QJsonDocument::Compact));
	const auto generation = QString::fromLatin1(digest.result().toHex()); if (!expected.isEmpty() && expected != generation) { fail("Asset search observation changed; restart paging without searchGeneration"); }
	const bool complete = !scanned.cancelled && !scanned.limited && scanned.unavailableRoots == 0 && scanned.unreadableDirectories == 0;
	auto projectedRoot = rootObservation; if (!names) { for (const auto& key : {QString("path"), QString("canonical")}) { const auto value = projectedRoot.take(key).toString(); text(projectedRoot, key, value, false); } }
	return {{"root", projectedRoot}, {"query", query}, {"matches", matches}, {"total", matched}, {"offset", offset}, {"limit", limit}, {"nextOffset", static_cast<double>(static_cast<qint64>(offset) + matches.size())}, {"hasMore", static_cast<qint64>(offset) + matches.size() < matched},
		{"searchGeneration", generation}, {"entriesObserved", static_cast<double>(scanned.entries)}, {"maximumEntries", maximum}, {"entryLimitReached", scanned.limited}, {"scanComplete", complete}, {"unavailableRoots", static_cast<double>(scanned.unavailableRoots)}, {"unreadableDirectories", static_cast<double>(scanned.unreadableDirectories)},
		{"scope", "native filename tokens and suffixes within the selected directory; native iterator order, no byte hashing or decoding, no content-equivalence or musical judgment"}, {"continuation", "keep root/query/filters/budget fixed with searchGeneration while paging; incomplete scans require a narrower directory or another entry budget; filesystem observations can change"}, {"completedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}
}
