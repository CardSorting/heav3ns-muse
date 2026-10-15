/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioSignals.h"
#include "lmmsconfig.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <QScopeGuard>
#ifdef LMMS_BUILD_WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

namespace lmms
{
namespace
{
static_assert(std::atomic<bool>::is_always_lock_free, "Studio signal handlers require a lock-free flag");
std::atomic<bool> owned{false};
std::atomic<bool> interrupted{false};
#ifdef LMMS_BUILD_WIN32
BOOL WINAPI studioConsoleControl(DWORD control)
{
	if (control != CTRL_C_EVENT && control != CTRL_BREAK_EVENT) { return FALSE; }
	interrupted.store(true, std::memory_order_relaxed);
	return TRUE;
}
#else
void studioSignal(int) noexcept
{
	interrupted.store(true, std::memory_order_relaxed);
}
#endif
}

struct StudioSignals::State
{
	bool installed = false;
#ifndef LMMS_BUILD_WIN32
	struct sigaction previousInterrupt{};
	struct sigaction previousTerminate{};
#endif
};

StudioSignals::StudioSignals() : m_state(std::make_unique<State>()) {}

bool StudioSignals::install(QString& error)
{
	if (m_state->installed) { error = "Studio interrupt handlers are already installed"; return false; }
	bool expected = false;
	if (!owned.compare_exchange_strong(expected, true)) { error = "Another studio owns the process interrupt handlers"; return false; }
	const auto releaseFailedInstall = qScopeGuard([&] { if (!m_state->installed) { owned.store(false); } });
	interrupted.store(false, std::memory_order_relaxed);
#ifdef LMMS_BUILD_WIN32
	if (!SetConsoleCtrlHandler(studioConsoleControl, TRUE))
	{
		error = "Could not install studio console interrupt handler: " + QString::number(GetLastError()); return false;
	}
#else
	struct sigaction action{};
	action.sa_handler = studioSignal;
	if (::sigemptyset(&action.sa_mask) != 0) { error = "Could not initialize studio signal mask"; return false; }
	action.sa_flags = SA_RESTART;
	if (::sigaction(SIGINT, &action, &m_state->previousInterrupt) != 0)
	{
		error = "Could not install studio SIGINT handler: " + QString::number(errno); return false;
	}
	if (::sigaction(SIGTERM, &action, &m_state->previousTerminate) != 0)
	{
		const int failure = errno; ::sigaction(SIGINT, &m_state->previousInterrupt, nullptr);
		error = "Could not install studio SIGTERM handler: " + QString::number(failure); return false;
	}
#endif
	m_state->installed = true; error.clear(); return true;
}

StudioSignals::~StudioSignals()
{
	if (!m_state->installed) { return; }
#ifdef LMMS_BUILD_WIN32
	SetConsoleCtrlHandler(studioConsoleControl, FALSE);
#else
	::sigaction(SIGINT, &m_state->previousInterrupt, nullptr);
	::sigaction(SIGTERM, &m_state->previousTerminate, nullptr);
#endif
	owned.store(false);
}

bool StudioSignals::requested() const noexcept
{
	return m_state->installed && interrupted.load(std::memory_order_relaxed);
}
} // namespace lmms
