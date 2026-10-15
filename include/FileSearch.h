/* Native filename search shared by presentation and studio code. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_FILE_SEARCH_H
#define LMMS_FILE_SEARCH_H
#include <functional>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include "lmms_export.h"
namespace lmms
{
class LMMS_EXPORT FileSearch
{
public:
	struct Task
	{
		QString filter;
		QStringList paths;
		QStringList extensions;
		QFlags<QDir::Filter> dirFilters = QDir::AllEntries | QDir::NoDotAndDotDot;
		bool recursive = true, followSymlinks = true, includeFiles = true, includeDirectories = true;
		qint64 maximumEntries = 0; // Zero preserves the existing GUI's unlimited traversal.
	};
	struct Result { qint64 entries = 0, matches = 0, unavailableRoots = 0, unreadableDirectories = 0; bool cancelled = false, limited = false; };
	struct Callbacks
	{
		std::function<bool()> cancelled;
		// Local metadata is valid during the callback; retain owned copies if needed.
		std::function<void(const QFileInfo&, bool)> entry;
		std::function<void(const QString&)> match;
	};
	static QStringList tokens(const QString& filter);
	static bool matches(const Task& task, const QStringList& tokens, const QFileInfo& file);
	static Result scan(Task task, Callbacks callbacks);
};
}
#endif
