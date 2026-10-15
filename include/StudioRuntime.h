/* LMMS-owned durable studio session. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_RUNTIME_H
#define LMMS_STUDIO_RUNTIME_H

#include <memory>
#include <cstdio>
#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include "lmms_export.h"
#include "StudioInference.h"

class QLockFile;

namespace lmms
{

class LMMS_EXPORT StudioRuntime
{
public:
	// Entry points execute on the application thread. One open runtime owns the
	// process-wide native project until destruction, including after shutdown.
	explicit StudioRuntime(const QString& sessionDirectory, const StudioInference::Transport& transport = {},
		std::FILE* outputStream = stdout, const StudioInference::Cancelled& interrupted = {},
		const std::function<QJsonObject()>& resourceSampler = {});
	~StudioRuntime();
	bool open(QString& error);
	QJsonObject process(const QJsonObject& request);
	int run();
	bool stopping() const { return m_stopping; }
	// One bounded inference or native execution transition, always on the app thread.
	QJsonObject advanceAgent();

private:
	void output(const QJsonObject& message);
	bool interruptionRequested() const;
	void pollInterruption();
	QJsonObject sampleResources() const noexcept;
	QJsonObject checkpoint();
	bool validateCheckpoint(const QJsonObject& snapshot, QString& error) const;
	bool restore(const QJsonObject& snapshot, QString& error, bool* loadAttempted = nullptr);
	bool recoverNativeProject(const QJsonObject& snapshot, QString& error);
	bool commit(const QJsonObject& state, QString& error);
	QString archiveFilePath(const QString& relative, bool createParent, QString& error) const;
	void collectRetiredFiles() noexcept;
	bool archiveReceipts(QJsonObject& state, QString& error);
	bool archiveToolResults(QJsonObject& state, QString& error);
	bool readToolRecord(const QJsonObject& record, const QString& job, QByteArray& bytes, QString& error) const;
	bool readReceipt(const QJsonObject& reference, int revision, QJsonObject& receipt, QString& error) const;
	bool archiveTranscript(QJsonObject& state, QString& error);
	bool readTranscript(const QJsonObject& reference, QJsonArray& messages, QString& error) const;
	QJsonObject conversationHistory(const QJsonObject& arguments) const;
	QJsonObject sessionInfo() const;
	QJsonObject performRequest(const QJsonObject& request,
		const std::function<void(QJsonObject&, const QJsonObject&)>& update = {});
	QJsonObject archiveJob(const QJsonObject& job);
	QJsonObject archiveContext(const QJsonObject& job);
	bool readContext(const QJsonObject& reference, QByteArray& bytes, QString& error) const;
	bool readContextRecord(const QJsonObject& context, int record, const QString& job, QByteArray& bytes, QString& error) const;
	bool readArchivedJob(const QJsonObject& reference, QJsonObject& job, QByteArray& bytes, QString& error) const;
	QJsonObject queryHistory(const QString& operation, const QJsonObject& arguments) const;
	bool validateJobContexts(const QJsonObject& job, QString& error) const;
	bool storeAgent(const QJsonObject& agent, QString& error);
	QJsonObject listen(const QJsonObject& arguments);

	QDir m_directory;
	std::unique_ptr<QLockFile> m_lock;
	QJsonObject m_state;
	bool m_open = false;
	bool m_stopping = false;
	bool m_failed = false;
	QSet<QString> m_activeRequestIds;
	bool m_advancingAgent = false;
	StudioInference::Transport m_transport;
	std::FILE* m_outputStream;
	StudioInference::Cancelled m_interrupted;
	std::function<QJsonObject()> m_resourceSampler;
	std::function<bool(bool)> m_resourceBudget;
	QString m_canonicalDirectory;
	QSet<QString> m_collectedFiles;
	QJsonObject m_storageReport;
	int m_storageCursor = 0;
};

} // namespace lmms
#endif
