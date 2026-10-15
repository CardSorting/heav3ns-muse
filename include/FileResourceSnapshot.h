/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_FILE_RESOURCE_SNAPSHOT_H
#define LMMS_FILE_RESOURCE_SNAPSHOT_H

#include <memory>
#include <QList>
#include <QObject>
#include <QString>
#include <QTemporaryDir>

#include "lmms_export.h"

namespace lmms
{

class SampleBuffer;

// Native loaders consume this owned copy, so persistence can use the same bytes
// even when the user's original file is replaced or removed after loading.
class LMMS_EXPORT FileResourceSnapshot : public QObject
{
public:
	struct Member { QString name; QString sha256; qint64 bytes; };
	// A native settings loader selects one exact serialized resource reference.
	// Nested references mask the outer one, even when they have no accepted data.
	class LMMS_EXPORT ReadScope
	{
	public:
		ReadScope(QString source, std::shared_ptr<const FileResourceSnapshot> snapshot,
			std::shared_ptr<const SampleBuffer> sample = {});
		~ReadScope();
		ReadScope(const ReadScope&) = delete;
		ReadScope& operator=(const ReadScope&) = delete;
	private:
		friend class FileResourceSnapshot;
		ReadScope* m_previous;
		QString m_source;
		std::shared_ptr<const FileResourceSnapshot> m_snapshot;
		std::shared_ptr<const SampleBuffer> m_sample;
	};
	static QString readableSourcePath(const QString& source);
	// Distinguish an accepted binding even when its declared path already names
	// the retained file. No filesystem work or cooperative callback is performed.
	static bool hasRetainedSource(const QString& source) { return static_cast<bool>(retainedSnapshot(source)); }
	static std::shared_ptr<const SampleBuffer> retainedSample(const QString& source);
	~FileResourceSnapshot() override;
	static std::shared_ptr<FileResourceSnapshot> capture(const QString& source, QString& error);
	// Bound the opened file before copying; used by fixed-size native file formats.
	static std::shared_ptr<FileResourceSnapshot> captureBounded(const QString& source, qint64 maximumBytes, QString& error);
	// Copy the root before reading its xfil/doxf declarations, then capture every
	// declared companion. Native libgig reads only this privately owned layout.
	static std::shared_ptr<FileResourceSnapshot> captureGig(const QString& source, QString& error);
	// A missing companion is recoverable only for an unbound source after all
	// declarations and present members have passed structural/integrity checks.
	enum class GigCaptureFailure { None, Other, MissingCompanion };
	static std::shared_ptr<FileResourceSnapshot> captureGig(const QString& source, QString& error, GigCaptureFailure& failure);
	bool isGroup() const { return !m_members.isEmpty(); }
	const QList<Member>& members() const { return m_members; }
	QString groupDigest() const { return memberDigest(m_members); }
	static bool safeRelativeName(const QString& name);
	static QString memberDigest(const QList<Member>& members);
	static bool verifyMembers(const QString& directory, const QList<Member>& members, QString& error);
	bool copyGroup(const QString& directory, QString& error) const;
	// After workers stop, release queued snapshots even if the application event
	// loop has already exited. Only the application thread may drain the queue.
	static bool drainDeferredRemovals();
	const QString& path() const { return m_path; }
	const QString& sha256() const { return m_sha256; }
	qint64 bytes() const { return m_bytes; }
	bool verify(QString& error) const { return verifyCopy(m_path, error); }
	// Compare a saved copy against the fingerprint established before loading.
	bool verifyCopy(const QString& path, QString& error) const;
	// Lazy native readers must release their file before their owner calls this.
	// An unsuccessful removal leaves the object available for a later retry.
	bool remove() { return m_directory.remove(); }

private:
	FileResourceSnapshot();
	static thread_local ReadScope* s_readScope;
	static std::shared_ptr<const FileResourceSnapshot> retainedSnapshot(const QString& source);
	static std::shared_ptr<FileResourceSnapshot> captureRoot(const QString& source, bool retainName, QString& error, qint64 maximumBytes = -1);
	static bool captureMember(const QString& source, const QString& destination, Member& member, QString& error);
	static void release(FileResourceSnapshot* owned);
	QTemporaryDir m_directory;
	QString m_path;
	QString m_sha256;
	qint64 m_bytes = -1;
	QList<Member> m_members;
	FileResourceSnapshot* m_removalPrevious = nullptr;
	FileResourceSnapshot* m_removalNext = nullptr;
	bool m_removalQueued = false;
};

} // namespace lmms

#endif // LMMS_FILE_RESOURCE_SNAPSHOT_H
