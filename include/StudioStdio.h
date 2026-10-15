/* Nonblocking native text transport. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_STUDIO_STDIO_H
#define LMMS_STUDIO_STDIO_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <cstdio>

namespace lmms
{

// Reserve the inherited output for protocol frames, and route ordinary native
// stdout diagnostics to stderr before starting any engine or plugin threads.
class StudioProtocolOutput
{
public:
	explicit StudioProtocolOutput(bool restoreOutput = true) : m_restoreOutput(restoreOutput) { }
	~StudioProtocolOutput();
	StudioProtocolOutput(const StudioProtocolOutput&) = delete;
	StudioProtocolOutput& operator=(const StudioProtocolOutput&) = delete;
	bool open(QString& error);
	std::FILE* stream() const { return m_stream; }

private:
	std::FILE* m_stream = nullptr;
	bool m_redirected = false;
	bool m_restoreOutput;
	int m_originalOutputMode = -1;
};

class StudioStdio
{
public:
	struct Frame { QByteArray bytes; bool oversized = false; };
	QList<Frame> readAvailable(int maximumFrames = 64);
	bool ended() const { return m_ended && m_buffer.isEmpty() && m_line.isEmpty(); }
	QString error() const { return m_error; }

private:
	QByteArray read();
	QByteArray m_buffer;
	QByteArray m_line;
	bool m_oversized = false;
	bool m_ended = false;
	bool m_consoleInput = false;
	QString m_error;
#ifdef Q_OS_WIN
	ushort m_highSurrogate = 0;
#endif
};

} // namespace lmms
#endif
