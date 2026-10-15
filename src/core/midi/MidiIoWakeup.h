/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_MIDI_IO_WAKEUP_H
#define LMMS_MIDI_IO_WAKEUP_H
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace lmms::detail
{
// POSIX native MIDI readers wake before joining. Owned descriptors outlive the
// reader and close on every construction/retirement path, including failed opens.
class MidiIoWakeup
{
public:
	MidiIoWakeup() = default;
	~MidiIoWakeup() { close(); }
	MidiIoWakeup(const MidiIoWakeup&) = delete;
	MidiIoWakeup& operator=(const MidiIoWakeup&) = delete;
	bool open()
	{
		if (::pipe(m_descriptors) < 0) { return false; }
		for (const auto descriptor : m_descriptors)
		{
			const auto flags = ::fcntl(descriptor, F_GETFL);
			if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0
				|| ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
			{ const auto error = errno; close(); errno = error; return false; }
		}
		return true;
	}
	int descriptor() const { return m_descriptors[0]; }
	void drain() const noexcept
	{
		unsigned char bytes[64];
		// Producers may keep signalling while output is queued. Bound this
		// drain so native input/output and shutdown still get a turn.
		for (unsigned int attempt = 0; attempt < 64; ++attempt)
		{
			const auto count = ::read(m_descriptors[0], bytes, sizeof(bytes));
			if (count > 0 || (count < 0 && errno == EINTR)) { continue; }
			return;
		}
	}
	void signal() const noexcept
	{
		if (m_descriptors[1] < 0) { return; }
		const unsigned char byte = 1;
		while (::write(m_descriptors[1], &byte, 1) < 0 && errno == EINTR) { }
	}
	void close() noexcept
	{
		for (auto& descriptor : m_descriptors) { if (descriptor >= 0) { ::close(descriptor); descriptor = -1; } }
	}
private:
	int m_descriptors[2]{-1, -1};
};
}
#endif
