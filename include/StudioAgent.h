/* Durable native agent state and tool declarations. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_AGENT_H
#define LMMS_STUDIO_AGENT_H

#include <QJsonArray>
#include <QJsonObject>
#include "lmms_export.h"

namespace lmms
{

class LMMS_EXPORT StudioAgent
{
public:
	static QJsonObject initialState();
	static bool validState(const QJsonObject& state);
	static bool validTranscriptMessage(const QJsonObject& message);
	static bool validTranscriptArchive(const QJsonObject& reference);
	static bool validPastInput(const QJsonObject& context);
	static bool validToolRecord(const QJsonObject& record);
	static QJsonObject storedToolResponse(const QJsonObject& call, const QJsonObject& response,
		const QJsonObject& archive, const QString& job);
	static bool matchesArchivedTool(const QJsonObject& reference, const QJsonObject& record);
	static void archiveToolRecords(QJsonObject& job, int context);
	static bool active(const QJsonObject& job);
	static QJsonObject job(const QString& text, const QJsonObject& options,
		const QJsonObject& provider, int revision);
	static QJsonArray schemas();
	// A local declaration budget, not a limit on the native operation catalog.
	static constexpr int ToolDeclarationLimit = 128;
	static QJsonArray tools(const QString& protocol);
	static QString operation(const QString& functionName);
	static QString operation(const QString& functionName, QJsonObject& arguments);
	static QJsonObject observation(const QJsonObject& project, const QJsonObject& memory,
		const QJsonObject& job, bool audioAvailable, const QJsonObject& resources = {});
	static QString instructions();
	static void appendToolOutput(QJsonObject& job, const QJsonObject& call, const QJsonObject& response);
	// Close queued decisions in their original provider protocol without execution.
	static void skipPending(QJsonObject& job, const QString& reason);
	static QJsonObject inspect(const QJsonObject& state);
};

} // namespace lmms
#endif
