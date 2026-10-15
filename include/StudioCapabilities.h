/* Native studio capabilities. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_CAPABILITIES_H
#define LMMS_STUDIO_CAPABILITIES_H

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <functional>
#include <memory>
#include "lmms_export.h"

namespace lmms
{

// This is an adapter to the live LMMS models, never a second project model.
// Call on the application's thread with an initialized Engine.
// StudioRuntime provides the durable transaction and rollback boundary.
class LMMS_EXPORT StudioCapabilities
{
public:
	// Transient references to existing native targets, never serialized handles.
	// Missing targets may be created by earlier calls in a composition batch.
	class LMMS_EXPORT Selection
	{
	public:
		bool changed(const Selection& current) const;
	private:
		friend class StudioCapabilities;
		QMap<QString, QPointer<QObject>> m_objects;
		std::shared_ptr<const void> m_note;
		QMap<QString, std::shared_ptr<const void>> m_notes;
	};
	static Selection selection(const QString& operation, const QJsonObject& arguments);
	static QJsonArray schema();
	// Validate the published capability schema without touching native models.
	// Native state-dependent admission still belongs to each operation.
	static bool validateArguments(const QString& operation, const QJsonObject& arguments, QString& error,
		const std::function<bool()>& cancelled = {});
	static bool mutatesProject(const QString& operation);
	// Cancellation is cooperative at native work/publication checks. A rejected
	// direct call can have applied native effects; StudioRuntime owns rollback.
	// Export/device/capture paths retain their own publication/recovery boundaries.
	static QJsonObject execute(const QString& operation, const QJsonObject& arguments,
		const std::function<bool()>& cancelled = {});
	static QJsonObject inspect();
	static QJsonObject inspectSummary();
};

} // namespace lmms
#endif
