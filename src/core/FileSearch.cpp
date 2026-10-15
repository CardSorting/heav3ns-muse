/* Native filename search. SPDX-License-Identifier: GPL-2.0-or-later */
#include "FileSearch.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <QDirIterator>
#include <QRegularExpression>
namespace lmms
{
QStringList FileSearch::tokens(const QString& filter)
{
	static const QRegularExpression expression{R"(\"([^"]+)\"|(\S+))"}; auto matches = expression.globalMatch(filter); QStringList result;
	while (matches.hasNext()) { const auto match = matches.next(); const auto quoted = match.captured(1), plain = match.captured(2); if (!quoted.isEmpty()) { result.append(quoted); } if (!plain.isEmpty()) { result.append(plain); } }
	return result;
}
bool FileSearch::matches(const Task& task, const QStringList& tokens, const QFileInfo& file)
{
	const auto name = file.fileName(); if (!std::all_of(tokens.begin(), tokens.end(), [&](const auto& token) { return name.contains(token, Qt::CaseInsensitive); })) { return false; }
	if (file.isDir()) { return task.includeDirectories; }
	return task.includeFiles && file.isFile() && (task.extensions.contains("*") || task.extensions.contains("*." + file.completeSuffix(), Qt::CaseInsensitive) || task.extensions.contains("*." + file.suffix(), Qt::CaseInsensitive));
}
FileSearch::Result FileSearch::scan(Task task, Callbacks callbacks)
{
	if (task.maximumEntries < 0) { throw std::invalid_argument("Native search entry limit cannot be negative"); }
	const auto words = tokens(task.filter); Result result;
	const auto cancelled = [&] { if (callbacks.cancelled && callbacks.cancelled()) { result.cancelled = true; return true; } return false; };
	QDirIterator::IteratorFlags flags;
	if (task.recursive) { flags |= QDirIterator::Subdirectories; }
	if (task.followSymlinks) { flags |= QDirIterator::FollowSymlinks; }
	for (const auto& path : task.paths)
	{
		if (cancelled()) { return result; } const QFileInfo root(path);
		if (!root.isDir() || !root.isReadable()) { ++result.unavailableRoots; continue; }
		QDirIterator iterator(path, task.dirFilters, flags);
		while (!cancelled() && iterator.hasNext())
		{
			if ((task.maximumEntries && result.entries >= task.maximumEntries) || result.entries == std::numeric_limits<qint64>::max()) { result.limited = true; return result; }
			const QFileInfo file(iterator.next()); ++result.entries;
			if (task.recursive && file.isDir() && !file.isReadable()) { ++result.unreadableDirectories; }
			const bool matched = matches(task, words, file); if (callbacks.entry) { callbacks.entry(file, matched); }
			if (matched) { ++result.matches; if (callbacks.match) { callbacks.match(file.filePath()); } }
		}
		if (result.cancelled) { return result; }
	}
	return result;
}
}
