/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "FileResourceIO.h"

#include <algorithm>
#include <array>
#include <utility>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QScopedValueRollback>
#include <QThread>

namespace lmms
{
namespace
{
thread_local FileResourceIO::Scope* currentScope = nullptr;
thread_local bool pollingEvents = false;
thread_local bool eventsSuspended = false;
}

FileResourceIO::Scope::Scope(Cancelled cancelled, bool pollEvents)
	: m_previous(currentScope), m_cancelled(std::move(cancelled)), m_pollEvents(pollEvents)
{
	m_elapsed.start(); currentScope = this;
}

FileResourceIO::Scope::~Scope() { currentScope = m_previous; }
FileResourceIO::Suspension::Suspension() : m_previous(currentScope) { currentScope = nullptr; }
FileResourceIO::Suspension::~Suspension() { currentScope = m_previous; }
FileResourceIO::EventSuspension::EventSuspension() : m_previous(eventsSuspended) { eventsSuspended = true; }
FileResourceIO::EventSuspension::~EventSuspension() { eventsSuspended = m_previous; }

bool FileResourceIO::check(QString& error)
{
	if (!currentScope) { error.clear(); return true; }
	const auto stopped = [] {
		for (auto scope = currentScope; scope; scope = scope->m_previous)
		{
			if (scope->m_cancelled && scope->m_cancelled()) { return true; }
		}
		return false;
	};
	if (stopped()) { error = "Native asset operation cancelled or exceeded its execution budget"; return false; }
	const auto application = QCoreApplication::instance();
	if (!eventsSuspended && !pollingEvents && application && QThread::currentThread() == application->thread())
	{
		for (auto scope = currentScope; scope; scope = scope->m_previous)
		{
			if (!scope->m_pollEvents) { continue; }
			const auto elapsed = scope->m_elapsed.elapsed();
			if (elapsed - scope->m_lastPoll >= 10)
			{
				scope->m_lastPoll = elapsed;
				QScopedValueRollback<bool> guard(pollingEvents, true);
				QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 1);
			}
			break;
		}
	}
	if (stopped()) { error = "Native asset operation cancelled or exceeded its execution budget"; return false; }
	error.clear(); return true;
}

bool FileResourceIO::stream(QFile& input, QIODevice* output, QCryptographicHash* hash, qint64 bytes, QString& error)
{
	if (!check(error)) { return false; }
	if (bytes < 0 || input.size() != bytes) { error = "Native asset size is invalid or changed"; return false; }
	std::array<char, 64 * 1024> buffer;
	qint64 copied = 0;
	while (copied < bytes)
	{
		if (!check(error)) { return false; }
		const auto count = input.read(buffer.data(), std::min<qint64>(buffer.size(), bytes - copied));
		if (count <= 0 || (output && output->write(buffer.data(), count) != count))
		{
			error = "Could not transfer the complete native asset"; return false;
		}
		if (hash) { hash->addData(buffer.data(), static_cast<int>(count)); }
		copied += count;
	}
	if (input.size() != bytes || input.error() != QFileDevice::NoError)
	{
		error = "Native asset changed or could not be completely read"; return false;
	}
	return check(error);
}

bool FileResourceIO::hashFile(const QString& path, QString& checksum, QString& error)
{
	checksum.clear();
	if (!check(error)) { return false; }
	QFile input(path);
	if (!input.open(QIODevice::ReadOnly)) { error = "Could not read native asset for verification"; return false; }
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!stream(input, nullptr, &hash, input.size(), error)) { return false; }
	checksum = QString::fromLatin1(hash.result().toHex()); return true;
}

bool FileResourceIO::copyFile(const QString& source, const QString& destination, QString& error)
{
	if (!check(error)) { return false; }
	QFile input(source), output(destination);
	if (!input.open(QIODevice::ReadOnly)) { error = "Could not read native asset source"; return false; }
	if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { error = "Could not create native asset copy"; return false; }
	struct Cleanup
	{
		QFile& output;
		bool complete = false;
		~Cleanup() { if (!complete) { const auto path = output.fileName(); output.close(); QFile::remove(path); } }
	} cleanup{output};
	if (!stream(input, &output, nullptr, input.size(), error)) { return false; }
	if (!output.flush()) { error = "Could not finalize native asset copy"; return false; }
	if (!check(error)) { return false; }
	if (!output.setPermissions(input.permissions())) { error = "Could not preserve native asset permissions"; return false; }
	output.close(); cleanup.complete = true; return true;
}

} // namespace lmms
