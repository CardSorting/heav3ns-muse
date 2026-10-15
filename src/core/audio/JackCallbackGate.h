/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_JACK_CALLBACK_GATE_H
#define LMMS_JACK_CALLBACK_GATE_H

#include <atomic>
#include <cstdint>
#include <QThread>

namespace lmms::detail
{
// AudioDevice's public running flag changes before the backend's start/stop
// hooks. This independent gate keeps a previous callback from entering native
// project/DSP state while the control thread is joining or replacing a device.
class JackCallbackGate
{
public:
	class Lease
	{
	public:
		explicit Lease(JackCallbackGate& gate) noexcept : m_gate(gate.enter() ? &gate : nullptr) { }
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;
		~Lease() { if (m_gate) { m_gate->m_state.fetch_sub(1, std::memory_order_release); } }
		explicit operator bool() const noexcept { return m_gate != nullptr; }
	private:
		JackCallbackGate* m_gate;
	};

	// Control thread only. Wait outside the native engine's model lock; an
	// admitted callback may need that lock before it can finish.
	void pause() noexcept { m_state.fetch_or(Paused, std::memory_order_acq_rel); }
	void pauseAndWait() noexcept
	{
		pause();
		while (m_state.load(std::memory_order_acquire) != Paused) { QThread::msleep(1); }
	}
	void resume() noexcept { m_state.store(0, std::memory_order_release); }
	bool paused() const noexcept { return (m_state.load(std::memory_order_acquire) & Paused) != 0; }

private:
	bool enter() noexcept
	{
		auto state = m_state.load(std::memory_order_acquire);
		do { if (state & Paused) { return false; } }
		while (!m_state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel));
		return true;
	}
	static constexpr std::uint32_t Paused = std::uint32_t{1} << 31;
	static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
	std::atomic<std::uint32_t> m_state{Paused};
};
}
#endif
