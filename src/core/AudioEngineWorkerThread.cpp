/*
 * AudioEngineWorkerThread.cpp - implementation of AudioEngineWorkerThread
 *
 * Copyright (c) 2009-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "AudioEngineWorkerThread.h"

#include <QScopedValueRollback>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#include "AudioEngine.h"
#include "Hardware.h"
#include "ThreadableJob.h"


namespace lmms
{

AudioEngineWorkerThread::JobQueue AudioEngineWorkerThread::globalJobQueue;
QList<AudioEngineWorkerThread *> AudioEngineWorkerThread::workerThreads;

namespace
{
struct Scheduler
{
	std::mutex mutex;
	std::condition_variable workReady;
	std::condition_variable scansFinished;
	std::size_t pendingWorkers = 0;
	bool batchActive = false;
};
Scheduler& scheduler() { static Scheduler value; return value; }
}

// implementation of internal JobQueue
void AudioEngineWorkerThread::JobQueue::reset( OperationMode _opMode )
{
	m_writeIndex = 0;
	m_itemsDone = 0;
	m_failure = Failure::None;
	m_opMode = _opMode;
}




void AudioEngineWorkerThread::JobQueue::addJob( ThreadableJob * _job )
{
	if( _job->requiresProcessing() )
	{
		// update job state
		_job->queue();
		// actually queue the job via atomic operations
		auto index = m_writeIndex++;
		if (index < JOB_QUEUE_SIZE) {
			m_items[index] = _job;
		} else {
			_job->done();
			fail(Failure::QueueOverflow);
			++m_itemsDone;
		}
	}
}



void AudioEngineWorkerThread::JobQueue::run()
{
	bool processedJob = true;
	while (processedJob && m_itemsDone < m_writeIndex)
	{
		processedJob = false;
		for (auto i = std::size_t{0}; i < m_writeIndex && i < JOB_QUEUE_SIZE; ++i)
		{
			ThreadableJob * job = m_items[i].exchange(nullptr);
			if( job )
			{
				if (failure() != Failure::None) { job->done(); }
				else
				{
					try { job->process(); }
					catch (...)
					{
						job->done(); fail(Failure::ProcessingException);
					}
				}
				processedJob = true;
				++m_itemsDone;
			}
		}
		// always exit loop if we're not in dynamic mode
		processedJob = processedJob && ( m_opMode == OperationMode::Dynamic );
	}
}




void AudioEngineWorkerThread::JobQueue::wait()
{
	while (m_itemsDone < m_writeIndex) { busyWaitHint(); }
}





// implementation of worker threads

AudioEngineWorkerThread::AudioEngineWorkerThread(AudioEngine* audioEngine)
	: QThread(audioEngine)
{
	auto& state = scheduler(); std::unique_lock guard(state.mutex);
	if (state.batchActive) { throw std::logic_error("Cannot construct an audio worker during dispatch"); }
	if (workerThreads.isEmpty()) { globalJobQueue.reset(JobQueue::OperationMode::Static); }
	workerThreads << this;
}

AudioEngineWorkerThread::~AudioEngineWorkerThread()
{
	quit();
	// Never destroy a QThread whose native processor is still using this pool.
	// A hung processor can still block shutdown; safe hard interruption needs
	// process supervision rather than deleting a live worker after a timeout.
	Q_ASSERT(QThread::currentThread() != this);
	wait();
	auto& state = scheduler(); std::unique_lock guard(state.mutex);
	workerThreads.removeAll(this);
}

void AudioEngineWorkerThread::resetJobQueue(JobQueue::OperationMode mode)
{
	auto& state = scheduler(); std::unique_lock guard(state.mutex);
	if (state.batchActive || state.pendingWorkers) { throw std::logic_error("Cannot reset an active native audio job queue"); }
	globalJobQueue.reset(mode);
}

void AudioEngineWorkerThread::quit()
{
	auto& state = scheduler(); std::unique_lock guard(state.mutex);
	m_quit = true;
	if (m_workPending)
	{
		m_workPending = false;
		Q_ASSERT(state.pendingWorkers > 0);
		--state.pendingWorkers; state.scansFinished.notify_all();
	}
	state.workReady.notify_all();
}

void AudioEngineWorkerThread::startAndWaitForJobs()
{
	auto& state = scheduler();
	{
		std::unique_lock guard(state.mutex);
		if (state.batchActive) { throw std::logic_error("Native audio dispatch cannot overlap or reenter itself"); }
		state.batchActive = true;
		for (const auto worker : workerThreads)
		{
			if (worker->m_ready && !worker->m_quit)
			{
				Q_ASSERT(!worker->m_workPending && !worker->m_workActive);
				worker->m_workPending = true; ++state.pendingWorkers;
			}
		}
		state.workReady.notify_all();
	}
	// The coordinating thread also consumes jobs. Not-yet-ready workers need
	// not participate: the coordinator can complete the queue on its own.
	globalJobQueue.run();
	globalJobQueue.wait();
	std::unique_lock guard(state.mutex);
	state.scansFinished.wait(guard, [&state] { return state.pendingWorkers == 0; });
	// Job completion alone did not prove worker scans had returned. This barrier
	// now makes the following reset safe for mode, counters and queue slots.
	state.batchActive = false;
}

AudioEngineWorkerThread::SchedulerStatus AudioEngineWorkerThread::schedulerStatus()
{
	auto& state = scheduler(); std::unique_lock guard(state.mutex);
	SchedulerStatus result;
	result.registered = static_cast<int>(workerThreads.size());
	result.batchActive = state.batchActive;
	for (const auto worker : workerThreads)
	{
		result.ready += worker->m_ready && !worker->m_quit;
		result.queued += worker->m_workPending;
		result.scanning += worker->m_workActive;
	}
	return result;
}

void AudioEngineWorkerThread::run()
{
	disableDenormals();
	auto& state = scheduler(); std::unique_lock guard(state.mutex);
	m_ready = true;
	for (;;)
	{
		state.workReady.wait(guard, [this] { return m_quit || m_workPending; });
		if (m_quit) { break; }
		m_workPending = false; m_workActive = true;
		guard.unlock();
		{
			// The coordinator retains model ownership until every worker returns.
			// Note/MIDI jobs share that render admission instead of trying to lock
			// the coordinator's mutex from another thread.
			QScopedValueRollback<bool> rendering(AudioEngine::s_renderingThread, true);
			globalJobQueue.run();
		}
		guard.lock();
		m_workActive = false;
		Q_ASSERT(state.pendingWorkers > 0);
		--state.pendingWorkers; state.scansFinished.notify_all();
	}
	m_ready = false; state.scansFinished.notify_all();
}

} // namespace lmms
