/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_JACK_AUDIO_BUSES_H
#define LMMS_JACK_AUDIO_BUSES_H
#include "lmmsconfig.h"
#ifdef LMMS_HAVE_JACK

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>
#include <QElapsedTimer>
#include "AudioEngine.h"
#include "JackApi.h"

namespace lmms
{
class AudioJack;
class AudioBusOutput;
class AudioJackTestAccess;
namespace detail
{
struct JackBusSnapshot
{
	struct Entry
	{
		std::uint64_t id;
		std::array<jack_port_t*, 2> ports{};
	};
	std::uint32_t generation = 0;
	std::vector<Entry> entries;
	// Only the single native callback writes this preallocated scratch. The
	// owner never reads it, and destroys the snapshot only after reader exit.
	std::vector<std::array<float*, 2>> buffers;
	std::vector<AudioEngine::BusDestination> destinations;
};

class JackAudioBuses
{
public:
	explicit JackAudioBuses(AudioJack& owner) : m_owner(owner) { }
	~JackAudioBuses();
	static constexpr std::size_t PortLimit = 256; // stereo bus pairs, not master ports

	// Model hooks stage intent only. Every native call/join occurs separately
	// on the QObject owner thread, outside an AudioEngine model edit.
	void add(const std::shared_ptr<const AudioBusOutput>& source, const QString& label);
	void remove(const std::shared_ptr<const AudioBusOutput>& source);
	void rename(const std::shared_ptr<const AudioBusOutput>& source, const QString& label);
	AudioDevice::BusGraph inspect(const std::shared_ptr<const AudioBusOutput>& source, bool enabled);
	std::vector<jack_port_t*> routePorts(const std::shared_ptr<const AudioBusOutput>& source);
	void synchronize(const std::shared_ptr<const AudioBusOutput>& source, const std::function<bool()>& cancelled);
	void poll();
	void prepareClient();
	void detachClient();
	void restoreSnapshot();

	// One native JACK process callback is the sole reader. Publication and
	// hazard stores are sequentially consistent; no pointer is dereferenced
	// until publication is rechecked. Retired snapshots die on the owner.
	class Read
	{
	public:
		explicit Read(JackAudioBuses& buses) noexcept;
		~Read();
		Read(const Read&) = delete;
		Read& operator=(const Read&) = delete;
		JackBusSnapshot* get() const noexcept { return m_snapshot; }
		void confirm() noexcept { m_confirmed = true; }
	private:
		JackAudioBuses& m_buses;
		JackBusSnapshot* m_snapshot = nullptr;
		bool m_confirmed = false;
	};
	bool prepare(JackBusSnapshot* snapshot, jack_nframes_t frames);
	bool silence(JackBusSnapshot* snapshot, jack_nframes_t frames) noexcept;

private:
	friend class ::lmms::AudioJackTestAccess;
	struct Record
	{
		std::uint64_t id;
		std::shared_ptr<const AudioBusOutput> source;
		QString label, error;
		bool wanted = true, muted = false;
		std::array<jack_port_t*, 2> ports{};
		int nativeError = 0;
		QElapsedTimer retry;
	};
	Record* find(const std::shared_ptr<const AudioBusOutput>& source);
	bool pending(const Record& record) const;
	std::unique_ptr<JackBusSnapshot> snapshot(Record* adding = nullptr, Record* omitting = nullptr);
	void publish(std::unique_ptr<JackBusSnapshot> next);
	void reconcile(Record& record, const std::function<bool()>& cancelled);
	void registerPorts(Record& record, const std::function<bool()>& cancelled);
	void unregisterPorts(Record& record, const std::function<bool()>& cancelled);
	void retireUncertainClient();
	void prune();

	AudioJack& m_owner;
	std::map<const AudioBusOutput*, std::unique_ptr<Record>> m_records;
	std::uint64_t m_nextId = 1;
	std::uint32_t m_generation = 0;
	std::unique_ptr<JackBusSnapshot> m_snapshot;
	std::atomic<JackBusSnapshot*> m_published{nullptr}, m_reader{nullptr};
	std::atomic<std::uint32_t> m_observed{0};
};
}
}
#endif
#endif
