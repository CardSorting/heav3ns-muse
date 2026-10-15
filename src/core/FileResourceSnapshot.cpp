/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "FileResourceSnapshot.h"
#include "FileResourceIO.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QRegularExpression>
#include <QThread>
#include <QSet>
#include <QHash>
#include <QStringList>
#include <QtEndian>

namespace lmms
{
namespace
{

struct DeferredRemovals
{
	QMutex mutex;
	FileResourceSnapshot* first = nullptr;
};

DeferredRemovals& deferredRemovals()
{
	static DeferredRemovals queue;
	return queue;
}

quint32 riffInteger(const char* data, bool bigEndian)
{
	const auto bytes = reinterpret_cast<const uchar*>(data);
	return bigEndian ? qFromBigEndian<quint32>(bytes) : qFromLittleEndian<quint32>(bytes);
}

quint64 riffSize(const char* data, int offsetBytes, bool bigEndian)
{
	if (offsetBytes == 4) { return riffInteger(data, bigEndian); }
	const auto bytes = reinterpret_cast<const uchar*>(data);
	return bigEndian ? qFromBigEndian<quint64>(bytes) : qFromLittleEndian<quint64>(bytes);
}

QByteArray riffIdentifier(QByteArray bytes, bool bigEndian)
{
	if (bigEndian && bytes.size() == 16)
	{
		std::reverse(bytes.begin(), bytes.begin() + 4);
		std::reverse(bytes.begin() + 4, bytes.begin() + 6);
		std::reverse(bytes.begin() + 6, bytes.begin() + 8);
	}
	return bytes;
}

} // namespace

FileResourceSnapshot::FileResourceSnapshot()
	: m_directory(QDir::tempPath() + "/lmms-resource-XXXXXX")
{
	// Initialize on capture, before any audio voice can release this resource.
	(void)deferredRemovals();
	if (auto application = QCoreApplication::instance()) { moveToThread(application->thread()); }
}

FileResourceSnapshot::~FileResourceSnapshot()
{
	auto& queue = deferredRemovals();
	QMutexLocker guard(&queue.mutex);
	if (!m_removalQueued) { return; }
	if (m_removalPrevious) { m_removalPrevious->m_removalNext = m_removalNext; }
	else { queue.first = m_removalNext; }
	if (m_removalNext) { m_removalNext->m_removalPrevious = m_removalPrevious; }
	// Member file cleanup follows after the queue lock has been released.
}

void FileResourceSnapshot::release(FileResourceSnapshot* owned)
{
	if (!QCoreApplication::instance() || QThread::currentThread() == owned->thread()) { delete owned; return; }
	auto& queue = deferredRemovals();
	QMutexLocker guard(&queue.mutex);
	// Intrusive links avoid a second allocation merely to remember pending cleanup.
	owned->m_removalNext = queue.first;
	if (queue.first) { queue.first->m_removalPrevious = owned; }
	queue.first = owned; owned->m_removalQueued = true;
	// Keep posting and queue publication under one lock. A concurrent application
	// deletion cannot destroy the object before this worker finishes posting.
	owned->deleteLater();
}

bool FileResourceSnapshot::drainDeferredRemovals()
{
	const auto application = QCoreApplication::instance();
	if (!application || QThread::currentThread() != application->thread()) { return false; }
	auto& queue = deferredRemovals();
	for (;;)
	{
		FileResourceSnapshot* owned;
		{
			QMutexLocker guard(&queue.mutex);
			owned = queue.first;
		}
		if (!owned) { return true; }
		// Deletion unlinks under its own short lock and removes the posted Qt event.
		// Active owners never enter this queue; their files remain available.
		delete owned;
	}
}

thread_local FileResourceSnapshot::ReadScope* FileResourceSnapshot::s_readScope = nullptr;

FileResourceSnapshot::ReadScope::ReadScope(QString source,
	std::shared_ptr<const FileResourceSnapshot> snapshot, std::shared_ptr<const SampleBuffer> sample)
	: m_previous(s_readScope), m_source(QDir::cleanPath(source)), m_snapshot(std::move(snapshot)), m_sample(std::move(sample))
{
	QString error;
	if (m_snapshot && !m_snapshot->verify(error)) { throw std::runtime_error(error.toStdString()); }
	s_readScope = this;
}

FileResourceSnapshot::ReadScope::~ReadScope()
{
	s_readScope = m_previous;
}

std::shared_ptr<const FileResourceSnapshot> FileResourceSnapshot::retainedSnapshot(const QString& source)
{
	return s_readScope && s_readScope->m_source == QDir::cleanPath(source) ? s_readScope->m_snapshot : nullptr;
}

QString FileResourceSnapshot::readableSourcePath(const QString& source)
{
	const auto retained = retainedSnapshot(source);
	return retained ? retained->path() : source;
}

std::shared_ptr<const SampleBuffer> FileResourceSnapshot::retainedSample(const QString& source)
{
	return retainedSnapshot(source) ? s_readScope->m_sample : nullptr;
}

bool FileResourceSnapshot::verifyCopy(const QString& path, QString& error) const
{
	if (isGroup())
	{
		if (QFileInfo(path).fileName() != m_members.first().name)
		{
			error = QObject::tr("Native resource group root was renamed"); return false;
		}
		return verifyMembers(QFileInfo(path).absolutePath(), m_members, error);
	}
	if (!FileResourceIO::check(error)) { return false; }
	const QFileInfo info(path);
	QFile input(path);
	if (m_bytes < 0 || m_sha256.size() != 64 || !info.isFile() || info.isSymLink() || info.size() != m_bytes
		|| !input.open(QIODevice::ReadOnly) || input.size() != m_bytes)
	{
		error = QObject::tr("Accepted native resource is missing or has an altered size"); return false;
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!FileResourceIO::stream(input, nullptr, &hash, m_bytes, error)) { return false; }
	if (QString::fromLatin1(hash.result().toHex()) != m_sha256)
	{
		error = QObject::tr("Accepted native resource bytes are altered"); return false;
	}
	error.clear(); return true;
}

std::shared_ptr<FileResourceSnapshot> FileResourceSnapshot::capture(const QString& source, QString& error)
{
	const auto retained = retainedSnapshot(source);
	if (retained && retained->isGroup()) { return captureGig(source, error); }
	auto snapshot = captureRoot(retained ? retained->path() : source, false, error);
	if (snapshot && retained && !retained->verifyCopy(snapshot->path(), error)) { return {}; }
	return snapshot;
}

std::shared_ptr<FileResourceSnapshot> FileResourceSnapshot::captureBounded(const QString& source, qint64 maximumBytes, QString& error)
{
	if (maximumBytes < 0) { error = "Bounded native resource capture requires a nonnegative byte limit"; return {}; }
	const auto retained = retainedSnapshot(source);
	if (retained && retained->isGroup()) { error = "Bounded native resource capture requires a single file"; return {}; }
	auto snapshot = captureRoot(retained ? retained->path() : source, false, error, maximumBytes);
	if (snapshot && retained && !retained->verifyCopy(snapshot->path(), error)) { return {}; }
	return snapshot;
}

std::shared_ptr<FileResourceSnapshot> FileResourceSnapshot::captureRoot(const QString& source, bool retainName, QString& error, qint64 maximumBytes)
{
	if (!FileResourceIO::check(error)) { return {}; }
	QFile input(source);
	if (!QFileInfo(source).isFile() || !input.open(QIODevice::ReadOnly))
	{
		error = QObject::tr("Could not read the native resource source"); return {};
	}
	const qint64 expected = input.size();
	if (expected < 0) { error = QObject::tr("Could not determine the native resource size"); return {}; }
	if (maximumBytes >= 0 && expected > maximumBytes) { error = "Native resource exceeds the accepted file size limit"; return {}; }
	auto snapshot = std::shared_ptr<FileResourceSnapshot>(new FileResourceSnapshot(), &FileResourceSnapshot::release);
	if (!snapshot->m_directory.isValid())
	{
		error = QObject::tr("Could not create temporary native resource storage"); return {};
	}
	static const QRegularExpression validSuffix("^[a-zA-Z0-9]{1,16}$");
	const auto suffix = QFileInfo(source).suffix();
	const auto name = retainName ? QFileInfo(source).fileName() : "resource."
		+ (validSuffix.match(suffix).hasMatch() ? suffix : QString("bin"));
	if (!safeRelativeName(name)) { error = QObject::tr("Native resource root has an unsafe filename"); return {}; }
	snapshot->m_path = snapshot->m_directory.filePath(name);
	QFile output(snapshot->m_path);
	if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
	{
		error = QObject::tr("Could not open temporary native resource storage"); return {};
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!FileResourceIO::stream(input, &output, &hash, expected, error)) { return {}; }
	if (!output.flush())
	{
		error = QObject::tr("The native resource changed or temporary storage could not be finalized"); return {};
	}
	// QFile releases its actual descriptor; QTemporaryFile::close() would retain
	// a descriptor internally for every loaded resource and retired snapshot.
	output.close();
	snapshot->m_bytes = expected;
	snapshot->m_sha256 = QString::fromLatin1(hash.result().toHex());
	if (!snapshot->verify(error)) { return {}; }
	error.clear(); return snapshot;
}

bool FileResourceSnapshot::safeRelativeName(const QString& name)
{
	if (name.isEmpty() || name.size() > 1024 || QString::fromUtf8(name.toUtf8()) != name
		|| QDir::isAbsolutePath(name) || name.contains('\\')) { return false; }
	for (const auto& part : name.split('/'))
	{
		if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')) { return false; }
		for (const auto character : part)
		{
			if (character.unicode() < 32 || QString(":*?\"<>|").contains(character)) { return false; }
		}
		// These names cannot form a portable native bank on Windows.
		static const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption);
		if (reserved.match(part).hasMatch()) { return false; }
	}
	return true;
}

QString FileResourceSnapshot::memberDigest(const QList<Member>& members)
{
	if (members.isEmpty()) { return {}; }
	QCryptographicHash hash(QCryptographicHash::Sha256);
	hash.addData("lmms-resource-group-v1\n");
	// Length prefixes make Unicode names and arbitrary layouts unambiguous. The
	// first member identifies the root; companions are ordered by accepted name.
	for (const auto& member : members)
	{
		const auto name = member.name.toUtf8();
		hash.addData(QByteArray::number(name.size()) + ":" + name + ":"
			+ QByteArray::number(member.bytes) + ":" + member.sha256.toLatin1() + "\n");
	}
	return QString::fromLatin1(hash.result().toHex());
}

bool FileResourceSnapshot::verifyMembers(const QString& directory, const QList<Member>& members, QString& error)
{
	const QDir root(directory);
	const auto canonical = root.canonicalPath();
	if (members.isEmpty() || canonical.isEmpty() || QFileInfo(directory).isSymLink())
	{
		error = QObject::tr("Native resource group storage is unavailable"); return false;
	}
	for (const auto& member : members)
	{
		if (!FileResourceIO::check(error)) { return false; }
		const auto path = root.filePath(member.name);
		const QFileInfo info(path);
		if (!safeRelativeName(member.name) || !info.isFile() || info.isSymLink() || info.size() != member.bytes
			|| info.canonicalFilePath() != QDir::cleanPath(canonical + "/" + member.name))
		{
			error = QObject::tr("Native resource group member is missing, altered or outside its directory: %1").arg(member.name); return false;
		}
		QString hash;
		if (!FileResourceIO::hashFile(path, hash, error)) { return false; }
		if (hash != member.sha256) { error = QObject::tr("Native resource group member bytes are altered: %1").arg(member.name); return false; }
	}
	error.clear(); return true;
}

bool FileResourceSnapshot::captureMember(const QString& source, const QString& destination, Member& member, QString& error)
{
	QFile input(source), output(destination);
	if (!QFileInfo(source).isFile() || !input.open(QIODevice::ReadOnly) || input.size() < 0)
	{
		error = QObject::tr("Could not read declared native resource member: %1").arg(member.name); return false;
	}
	member.bytes = input.size();
	if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
	{
		error = QObject::tr("Could not stage declared native resource member: %1").arg(member.name); return false;
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!FileResourceIO::stream(input, &output, &hash, member.bytes, error) || !output.flush())
	{
		if (error.isEmpty()) { error = QObject::tr("Could not finalize declared native resource member"); } return false;
	}
	output.close(); member.sha256 = QString::fromLatin1(hash.result().toHex()); return true;
}

bool FileResourceSnapshot::copyGroup(const QString& directory, QString& error) const
{
	if (!isGroup() || !verify(error)) { return false; }
	const QDir source(QFileInfo(m_path).absolutePath()), target(directory);
	if (!QDir().mkpath(directory) || QFileInfo(directory).isSymLink())
	{
		error = QObject::tr("Could not create native resource group copy"); return false;
	}
	for (const auto& member : m_members)
	{
		const auto destination = target.filePath(member.name);
		if (!QDir().mkpath(QFileInfo(destination).absolutePath())
			|| !FileResourceIO::copyFile(source.filePath(member.name), destination, error)) { return false; }
	}
	return verifyMembers(directory, m_members, error);
}

std::shared_ptr<FileResourceSnapshot> FileResourceSnapshot::captureGig(const QString& source, QString& error)
{
	GigCaptureFailure failure; return captureGig(source, error, failure);
}

std::shared_ptr<FileResourceSnapshot> FileResourceSnapshot::captureGig(const QString& source, QString& error, GigCaptureFailure& failure)
{
	failure = GigCaptureFailure::Other;
	const auto retained = retainedSnapshot(source);
	const auto physicalSource = retained ? retained->path() : source;
	auto snapshot = captureRoot(physicalSource, true, error);
	if (!snapshot) { return {}; }
	QFile root(snapshot->path());
	if (!root.open(QIODevice::ReadOnly)) { error = QObject::tr("Could not inspect accepted GIG root"); return {}; }
	// Match libgig's automatic file-offset width: each file chooses 64-bit
	// chunk lengths at 4 GiB, independently of the root/other companions.
	const int offsetBytes = (quint64(snapshot->bytes()) >> 32) ? 8 : 4;
	const int rootHeaderBytes = 8 + offsetBytes, chunkHeaderBytes = 4 + offsetBytes;
	const auto header = root.read(rootHeaderBytes);
	const bool bigEndian = header.left(4) == "RIFX";
	if (header.size() != rootHeaderBytes || (!bigEndian && header.left(4) != "RIFF") || header.mid(4 + offsetBytes, 4) != "DLS ")
	{
		error = QObject::tr("GIG resource requires a RIFF/RIFX DLS root"); return {};
	}
	if (riffSize(header.constData() + 4, offsetBytes, bigEndian) != quint64(snapshot->bytes() - chunkHeaderBytes))
	{
		error = QObject::tr("GIG root size does not match libgig's automatic RIFF layout"); return {};
	}
	QStringList names;
	QHash<QString, QByteArray> expectedIds;
	bool xfil = false, doxf = false;
	int chunks = 0;
	qint64 position = rootHeaderBytes;
	while (position < snapshot->bytes())
	{
		if (!FileResourceIO::check(error)) { return {}; }
		if (++chunks > 100000 || snapshot->bytes() - position < chunkHeaderBytes || !root.seek(position))
		{
			error = QObject::tr("GIG root chunk table is truncated or exceeds its inspection limit"); return {};
		}
		const auto chunk = root.read(chunkHeaderBytes);
		if (chunk.size() != chunkHeaderBytes) { error = QObject::tr("Could not read GIG root chunk header"); return {}; }
		const auto size = riffSize(chunk.constData() + 4, offsetBytes, bigEndian);
		if (size > static_cast<quint64>(snapshot->bytes() - position - chunkHeaderBytes))
		{
			error = QObject::tr("GIG root chunk exceeds the accepted file"); return {};
		}
		if (chunk.left(4) == "xfil")
		{
			if (xfil || size < 4) { error = QObject::tr("GIG extension table is invalid"); return {}; }
			xfil = true; const auto countBytes = root.read(4);
			if (countBytes.size() != 4) { error = QObject::tr("Could not read GIG extension count"); return {}; }
			const auto count = riffInteger(countBytes.constData(), bigEndian);
			if (count > 4096 || quint64(count) * 144 + 4 > size)
			{
				error = QObject::tr("GIG extension table is truncated or exceeds 4096 members"); return {};
			}
			for (quint32 index = 0; index < count; ++index)
			{
				if (!FileResourceIO::check(error)) { return {}; }
				const auto record = root.read(144);
				if (record.size() != 144) { error = QObject::tr("Could not read GIG extension record"); return {}; }
				auto encoded = record.left(128); const int terminator = encoded.indexOf('\0');
				if (terminator >= 0) { encoded.truncate(terminator); }
				const auto name = QString::fromUtf8(encoded);
				if (name.toUtf8() != encoded || !safeRelativeName(name))
				{
					error = QObject::tr("GIG extension filename is unsafe or is not UTF-8"); return {};
				}
				names.append(name);
				expectedIds.insert(name, riffIdentifier(record.mid(128, 16), bigEndian));
			}
		}
		else if (chunk.left(4) == "doxf")
		{
			if (doxf || size < 148) { error = QObject::tr("GIG convolution extension declaration is invalid"); return {}; }
			doxf = true;
			const auto declaration = root.read(148);
			if (declaration.size() != 148) { error = QObject::tr("Could not read GIG convolution extension record"); return {}; }
			const auto name = QFileInfo(physicalSource).completeBaseName() + ".gx99";
			names.append(name); expectedIds.insert(name, riffIdentifier(declaration.mid(132, 16), bigEndian));
		}
		// Include the pad byte in bounds before signed arithmetic.
		if (size + (size & 1) > quint64(snapshot->bytes() - position - chunkHeaderBytes))
		{
			error = QObject::tr("GIG root padding is truncated"); return {};
		}
		position += chunkHeaderBytes + static_cast<qint64>(size + (size & 1));
	}
	if (position != snapshot->bytes()) { error = QObject::tr("GIG root padding is truncated"); return {}; }
	root.close();
	const auto rootName = QFileInfo(snapshot->path()).fileName();
	snapshot->m_members.append({rootName, snapshot->sha256(), snapshot->bytes()});
	QSet<QString> used{rootName.toCaseFolded()};
	names.sort(Qt::CaseSensitive);
	const auto sourceDirectory = QFileInfo(physicalSource).absoluteDir();
	QStringList missing;
	for (const auto& name : names)
	{
		if (!safeRelativeName(name) || used.contains(name.toCaseFolded()))
		{
			error = QObject::tr("GIG member filenames collide or are unsafe: %1").arg(name); return {};
		}
		used.insert(name.toCaseFolded());
		if (!FileResourceIO::check(error)) { return {}; }
		const QFileInfo sourceMember(sourceDirectory.filePath(name));
		if (!sourceMember.exists() && !sourceMember.isSymLink()) { missing.append(name); continue; }
		Member member{name, {}, -1};
		if (!captureMember(sourceDirectory.filePath(name), snapshot->m_directory.filePath(name), member, error)) { return {}; }
		// libgig advances over the declaration's ID only when this member has a
		// dlid chunk. Require it and compare before libgig sees the table, so a
		// malformed companion cannot shift native parsing onto undeclared paths.
		QFile companion(snapshot->m_directory.filePath(name));
		if (!companion.open(QIODevice::ReadOnly))
		{
			error = QObject::tr("Could not inspect accepted GIG companion: %1").arg(name); return {};
		}
		const int memberOffsetBytes = (quint64(member.bytes) >> 32) ? 8 : 4;
		const int memberHeaderBytes = 8 + memberOffsetBytes, memberChunkHeaderBytes = 4 + memberOffsetBytes;
		const auto memberHeader = companion.read(memberHeaderBytes);
		const bool memberBigEndian = memberHeader.left(4) == "RIFX";
		if (memberHeader.size() != memberHeaderBytes || (!memberBigEndian && memberHeader.left(4) != "RIFF")
			|| riffSize(memberHeader.constData() + 4, memberOffsetBytes, memberBigEndian) != quint64(member.bytes - memberChunkHeaderBytes))
		{
			error = QObject::tr("GIG companion is not a supported RIFF file: %1").arg(name); return {};
		}
		qint64 offset = memberHeaderBytes; int memberChunks = 0; bool foundId = false;
		while (offset < member.bytes)
		{
			if (!FileResourceIO::check(error)) { return {}; }
			if (++memberChunks > 100000 || member.bytes - offset < memberChunkHeaderBytes || !companion.seek(offset))
			{
				error = QObject::tr("GIG companion chunk table is invalid: %1").arg(name); return {};
			}
			const auto chunk = companion.read(memberChunkHeaderBytes);
			if (chunk.size() != memberChunkHeaderBytes) { error = QObject::tr("Could not read GIG companion chunk"); return {}; }
			const auto size = riffSize(chunk.constData() + 4, memberOffsetBytes, memberBigEndian);
			if (size > quint64(member.bytes - offset - memberChunkHeaderBytes))
			{
				error = QObject::tr("GIG companion chunk exceeds its accepted file: %1").arg(name); return {};
			}
			if (chunk.left(4) == "dlid")
			{
				if (foundId || size != 16 || riffIdentifier(companion.read(16), memberBigEndian) != expectedIds.value(name))
				{
					error = QObject::tr("GIG companion identifier is invalid or mismatched: %1").arg(name); return {};
				}
				foundId = true;
			}
			if (size + (size & 1) > quint64(member.bytes - offset - memberChunkHeaderBytes))
			{
				error = QObject::tr("GIG companion padding is truncated: %1").arg(name); return {};
			}
			offset += memberChunkHeaderBytes + qint64(size + (size & 1));
		}
		if (!foundId || offset != member.bytes)
		{
			error = QObject::tr("GIG companion has no identifier or has truncated padding: %1").arg(name); return {};
		}
		snapshot->m_members.append(member);
	}
	if (!snapshot->verify(error)) { return {}; }
	if (!missing.isEmpty())
	{
		// Every present member and all declarations were checked before reporting
		// an incomplete group. Accepted bindings never become recoverable holes.
		error = QObject::tr("GIG resource has missing declared companions: %1").arg(missing.join(", "));
		if (!retained) { failure = GigCaptureFailure::MissingCompanion; }
		return {};
	}
	if (retained && !retained->verifyCopy(snapshot->path(), error)) { return {}; }
	failure = GigCaptureFailure::None; error.clear(); return snapshot;
}

} // namespace lmms
