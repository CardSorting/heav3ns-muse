/*
 * MidiSndio.h - base-class that implements sndio midi support
 *
 * Copyright (c) 2010-2016 jackmsr@openbsd.net
 * Copyright (c) 2016-2017 David Carlier <devnexen@gmail.com>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef LMMS_MIDI_SNDIO_H
#define LMMS_MIDI_SNDIO_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_SNDIO

#include <QThread>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <poll.h>

#include "MidiClient.h"


struct mio_hdl;

namespace lmms
{

namespace detail { struct SndioMidiApi; class MidiIoWakeup; class RawMidiOutput; }


class MidiSndio : public QThread, public MidiClientRaw
{
	Q_OBJECT
public:
	MidiSndio();
	~MidiSndio() override;
	InputBackendStatus inputBackendStatus() const override;
	bool isAvailable() const;
	OutputBackendStatus outputBackendStatus() const override;
	void addPort(MidiPort* port) override;
	void removePort(MidiPort* port) override;
	void cancelOutputEvents(const MidiPort* port) override;

	static QString probeDevice();

	inline static QString name()
	{
		return QT_TRANSLATE_NOOP("MidiSetupWidget", "sndio MIDI");
	}

	inline static QString configSection()
	{
		return "MidiSndio";
	}


protected:
	void sendByte(const unsigned char c) override;
	void run() override;
	void processOutEvent(const MidiEvent& event, const TimePos& time, const MidiPort* port) override;

private:
	friend class RawMidiIoTestAccess;
	explicit MidiSndio(const detail::SndioMidiApi& api);
	void closeResources() noexcept;
	void inputFailed(int error) noexcept;
	void finishPortTransferOutput(const std::function<bool()>& cancelled) override;
	void drainOutput(const std::function<bool()>& cancelled);
	int writeOutput(const std::uint8_t* bytes, std::size_t size);
	const detail::SndioMidiApi& m_api;
	std::unique_ptr<detail::MidiIoWakeup> m_wakeup;
	std::unique_ptr<detail::RawMidiOutput> m_outputQueue;
	std::mutex m_ioMutex;
	mio_hdl* m_hdl = nullptr;
	static constexpr int MaximumDescriptors = 64;
	std::array<pollfd, MaximumDescriptors + 1> m_descriptors{};
	std::atomic_bool m_quit{false}, m_active{false};
	std::atomic_int m_nativeError{0};
} ;


} // namespace lmms

#endif // LMMS_HAVE_SNDIO

#endif // LMMS_MIDI_SNDIO_H
