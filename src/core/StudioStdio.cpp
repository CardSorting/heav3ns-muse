/* Nonblocking native text transport. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioStdio.h"

#include <algorithm>
#include <array>
#include <QChar>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace lmms
{

bool StudioProtocolOutput::open(QString& error)
{
	if (m_stream) { error = "Studio protocol output is already reserved"; return false; }
	if (std::fflush(stdout) != 0) { error = "Could not flush studio startup output"; return false; }
#ifdef Q_OS_WIN
	const int outputDescriptor = _fileno(stdout); const int diagnosticDescriptor = _fileno(stderr);
	if (outputDescriptor < 0 || diagnosticDescriptor < 0) { error = "Studio requires valid output and diagnostic descriptors"; return false; }
	const auto source = _get_osfhandle(outputDescriptor);
	HANDLE saved = nullptr;
	if (source == -1 || !DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(source),
		GetCurrentProcess(), &saved, 0, FALSE, DUPLICATE_SAME_ACCESS))
	{
		error = "Could not reserve the studio output handle"; return false;
	}
	const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(saved), _O_WRONLY | _O_BINARY | _O_NOINHERIT);
	if (descriptor < 0) { CloseHandle(saved); error = "Could not reserve the studio output descriptor"; return false; }
	m_stream = _fdopen(descriptor, "wb");
	if (!m_stream) { _close(descriptor); error = "Could not open the studio output stream"; return false; }
	m_originalOutputMode = _setmode(outputDescriptor, _O_BINARY);
	if (m_originalOutputMode < 0) { std::fclose(m_stream); m_stream = nullptr; error = "Could not inspect the native output mode"; return false; }
	if (_dup2(diagnosticDescriptor, outputDescriptor) != 0)
	{
		_setmode(outputDescriptor, m_originalOutputMode);
		error = "Could not separate studio diagnostics from protocol output"; std::fclose(m_stream); m_stream = nullptr; return false;
	}
	m_redirected = true;
	if (!SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)))))
	{
		error = "Could not separate the native Windows output handle";
		_dup2(descriptor, _fileno(stdout));
		_setmode(outputDescriptor, m_originalOutputMode);
		SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
		m_redirected = false; std::fclose(m_stream); m_stream = nullptr; return false;
	}
#else
#ifdef F_DUPFD_CLOEXEC
	const int descriptor = ::fcntl(fileno(stdout), F_DUPFD_CLOEXEC, 3);
#else
	const int descriptor = ::dup(fileno(stdout));
#endif
	if (descriptor < 0) { error = "Could not reserve the studio output descriptor"; return false; }
#ifndef F_DUPFD_CLOEXEC
	if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
	{
		::close(descriptor); error = "Could not protect the studio output descriptor from child inheritance"; return false;
	}
#endif
	m_stream = ::fdopen(descriptor, "wb");
	if (!m_stream) { ::close(descriptor); error = "Could not open the studio output stream"; return false; }
	if (::dup2(fileno(stderr), fileno(stdout)) < 0)
	{
		error = "Could not separate studio diagnostics from protocol output"; std::fclose(m_stream); m_stream = nullptr; return false;
	}
	m_redirected = true;
#endif
	return true;
}

StudioProtocolOutput::~StudioProtocolOutput()
{
	if (!m_stream) { return; }
	std::fflush(stdout); std::fflush(m_stream);
	if (m_redirected && m_restoreOutput)
	{
#ifdef Q_OS_WIN
		if (_dup2(_fileno(m_stream), _fileno(stdout)) == 0)
		{
			_setmode(_fileno(stdout), m_originalOutputMode);
			SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
		}
#else
		::dup2(fileno(m_stream), fileno(stdout));
#endif
	}
	std::fclose(m_stream);
}

QByteArray StudioStdio::read()
{
	std::array<char, 65536> bytes{};
#ifdef Q_OS_WIN
	const auto input = GetStdHandle(STD_INPUT_HANDLE);
	const auto type = GetFileType(input);
	m_consoleInput = type == FILE_TYPE_CHAR;
	DWORD available = static_cast<DWORD>(bytes.size());
	if (type == FILE_TYPE_PIPE)
	{
		if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr))
		{
			if (GetLastError() != ERROR_BROKEN_PIPE) { m_error = "Could not inspect the studio input pipe"; }
			m_ended = true;
			return {};
		}
		if (available == 0) { return {}; }
	}
	else if (type == FILE_TYPE_CHAR)
	{
		DWORD events = 0;
		if (!GetNumberOfConsoleInputEvents(input, &events)) { m_error = "Could not inspect studio console input"; m_ended = true; return {}; }
		QByteArray result;
		while (events-- > 0 && result.size() < 4096)
		{
			INPUT_RECORD record{};
			DWORD count = 0;
			if (!ReadConsoleInputW(input, &record, 1, &count) || count == 0) { m_error = "Could not read studio console input"; m_ended = true; break; }
			if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) { continue; }
			const auto key = record.Event.KeyEvent;
			const auto ch = static_cast<ushort>(key.uChar.UnicodeChar);
			if (ch == 26) { m_ended = true; break; } // Ctrl-Z
			for (int repeat = 0; repeat < key.wRepeatCount; ++repeat)
			{
				if (QChar::isHighSurrogate(ch)) { m_highSurrogate = ch; continue; }
				if (m_highSurrogate)
				{
					const auto high = m_highSurrogate;
					m_highSurrogate = 0;
					if (QChar::isLowSurrogate(ch)) { result.append((QString(QChar(high)) + QChar(ch)).toUtf8()); continue; }
					result.append(QString(QChar::ReplacementCharacter).toUtf8());
				}
				if (ch == '\b') { result.append('\b'); }
				else if (ch == '\r') { result.append('\n'); }
				else if (ch) { result.append(QString(QChar(ch)).toUtf8()); }
			}
		}
		return result;
	}
	else if (type != FILE_TYPE_DISK)
	{
		m_error = "Studio stdin must be a pipe, file or console";
		m_ended = true;
		return {};
	}
	DWORD count = 0;
	if (!ReadFile(input, bytes.data(), std::min(available, static_cast<DWORD>(bytes.size())), &count, nullptr))
	{
		if (GetLastError() != ERROR_BROKEN_PIPE) { m_error = "Could not read studio stdin"; }
		m_ended = true;
		return {};
	}
	if (count == 0) { m_ended = true; }
	return QByteArray(bytes.data(), static_cast<int>(count));
#else
	m_consoleInput = ::isatty(STDIN_FILENO) != 0;
	pollfd input{STDIN_FILENO, POLLIN, 0};
	const int available = ::poll(&input, 1, 0);
	if (available < 0)
	{
		if (errno != EINTR) { m_error = "Could not poll studio stdin"; m_ended = true; }
		return {};
	}
	if (available == 0) { return {}; }
	if (input.revents & (POLLERR | POLLNVAL)) { m_error = "Studio stdin is invalid"; m_ended = true; return {}; }
	if (!(input.revents & (POLLIN | POLLHUP))) { return {}; }
	const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
	if (count < 0)
	{
		if (errno != EINTR && errno != EAGAIN) { m_error = "Could not read studio stdin"; m_ended = true; }
		return {};
	}
	if (count == 0) { m_ended = true; }
	return QByteArray(bytes.data(), static_cast<int>(count));
#endif
}

QList<StudioStdio::Frame> StudioStdio::readAvailable(int maximumFrames)
{
	QList<Frame> frames;
	maximumFrames = std::clamp(maximumFrames, 0, 64);
	if (maximumFrames == 0) { return frames; }
	if (m_buffer.isEmpty() && !m_ended) { m_buffer = read(); }
	int consumed = 0;
	for (; consumed < m_buffer.size() && frames.size() < maximumFrames; ++consumed)
	{
		const char ch = m_buffer.at(consumed);
		if (ch == '\n')
		{
			if (m_line.endsWith('\r')) { m_line.chop(1); }
			frames.append({m_line, m_oversized});
			m_line.clear();
			m_oversized = false;
		}
		else if (ch == '\b' && m_consoleInput && !m_oversized)
		{
			if (!m_line.isEmpty())
			{
				int start = m_line.size() - 1;
				while (start > 0 && (static_cast<unsigned char>(m_line.at(start)) & 0xc0) == 0x80) { --start; }
				m_line.truncate(start);
			}
		}
		else if (m_line.size() < 1024 * 1024 && !m_oversized) { m_line.append(ch); }
		else { m_oversized = true; }
	}
	m_buffer.remove(0, consumed);
	if (m_ended && m_buffer.isEmpty() && frames.size() < maximumFrames && (!m_line.isEmpty() || m_oversized))
	{
		frames.append({m_line, m_oversized});
		m_line.clear();
		m_oversized = false;
	}
	return frames;
}

} // namespace lmms
