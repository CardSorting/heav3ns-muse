/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_FILE_RESOURCE_IO_H
#define LMMS_FILE_RESOURCE_IO_H

#include <functional>
#include <QElapsedTimer>
#include <QString>
#include "lmms_export.h"

class QFile;
class QIODevice;
class QCryptographicHash;

namespace lmms
{

// Native control-thread file work. Checks are cooperative between 64 KiB chunks;
// they cannot interrupt an operating-system I/O call that is already blocked.
class LMMS_EXPORT FileResourceIO
{
public:
	using Cancelled = std::function<bool()>;
	class LMMS_EXPORT Scope
	{
	public:
		explicit Scope(Cancelled cancelled, bool pollEvents = false);
		~Scope();
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;
	private:
		friend class FileResourceIO;
		Scope* m_previous;
		Cancelled m_cancelled;
		bool m_pollEvents;
		QElapsedTimer m_elapsed;
		qint64 m_lastPoll = -10;
	};
	class LMMS_EXPORT Suspension
	{
	public:
		Suspension();
		~Suspension();
		Suspension(const Suspension&) = delete;
		Suspension& operator=(const Suspension&) = delete;
	private:
		Scope* m_previous;
	};
	// Keep cancellation/budget checks active, but prevent nested asset loaders
	// from dispatching events inside a partially constructed model transaction.
	class LMMS_EXPORT EventSuspension
	{
	public:
		EventSuspension();
		~EventSuspension();
		EventSuspension(const EventSuspension&) = delete;
		EventSuspension& operator=(const EventSuspension&) = delete;
	private:
		bool m_previous;
	};

	static bool check(QString& error);
	static bool stream(QFile& input, QIODevice* output, QCryptographicHash* hash, qint64 bytes, QString& error);
	static bool hashFile(const QString& path, QString& checksum, QString& error);
	static bool copyFile(const QString& source, const QString& destination, QString& error);
};

} // namespace lmms
#endif // LMMS_FILE_RESOURCE_IO_H
