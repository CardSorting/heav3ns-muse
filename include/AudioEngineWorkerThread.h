/*
 * AudioEngineWorkerThread.h - declaration of class AudioEngineWorkerThread
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

#ifndef LMMS_AUDIO_ENGINE_WORKER_THREAD_H
#define LMMS_AUDIO_ENGINE_WORKER_THREAD_H

#include <QThread>
#include <QList>

#include <algorithm>
#include <atomic>
#include <cstddef>

namespace lmms
{

class AudioEngine;
class ThreadableJob;

class AudioEngineWorkerThread : public QThread
{
	Q_OBJECT
public:
	// Consumers may run concurrently. Reset and job lifetime are caller-
	// serialized; dynamic producers must be part of an outstanding job, so
	// completion cannot precede future publication.
	class JobQueue
	{
	public:
		enum class Failure { None, ProcessingException, QueueOverflow };
		enum class OperationMode
		{
			Static,	// no jobs added while processing queue
			Dynamic	// jobs can be added while processing queue
		} ;

		static constexpr size_t JOB_QUEUE_SIZE = 8192;

		JobQueue() :
			m_items(),
			m_writeIndex( 0 ),
			m_itemsDone( 0 ),
			m_opMode( OperationMode::Static )
		{
			std::fill(m_items, m_items + JOB_QUEUE_SIZE, nullptr);
		}

		void reset( OperationMode _opMode );

		void addJob( ThreadableJob * _job );

		void run();
		void wait();
		Failure failure() const { return m_failure.load(std::memory_order_acquire); }

	private:
		void fail(Failure reason)
		{
			auto expected = Failure::None;
			m_failure.compare_exchange_strong(expected, reason, std::memory_order_acq_rel);
		}
		std::atomic<Failure> m_failure{Failure::None};
		std::atomic<ThreadableJob*> m_items[JOB_QUEUE_SIZE];
		std::atomic_size_t m_writeIndex;
		std::atomic_size_t m_itemsDone;
		OperationMode m_opMode;
	} ;


	AudioEngineWorkerThread( AudioEngine* audioEngine );
	~AudioEngineWorkerThread() override;

	virtual void quit();

	static void resetJobQueue(JobQueue::OperationMode mode = JobQueue::OperationMode::Static);

	static void addJob( ThreadableJob * _job )
	{
		globalJobQueue.addJob( _job );
	}

	// a convenient helper function allowing to pass a container with pointers
	// to ThreadableJob objects
	template<typename T>
	static void fillJobQueue( const T & _vec,
							JobQueue::OperationMode _opMode = JobQueue::OperationMode::Static )
	{
		resetJobQueue( _opMode );
		for (const auto& job : _vec)
		{
			addJob(job);
		}
	}

	static void startAndWaitForJobs();
	static JobQueue::Failure jobQueueFailure() { return globalJobQueue.failure(); }
	struct SchedulerStatus
	{
		int registered = 0;
		int ready = 0;
		int queued = 0;
		int scanning = 0;
		bool batchActive = false;
	};
	static SchedulerStatus schedulerStatus();


private:
	void run() override;

	static JobQueue globalJobQueue;
	static QList<AudioEngineWorkerThread *> workerThreads;

	// Guarded by the shared scheduler mutex, including quit/wake predicates.
	bool m_quit = false;
	bool m_ready = false;
	bool m_workPending = false;
	bool m_workActive = false;
} ;

} // namespace lmms

#endif // LMMS_AUDIO_ENGINE_WORKER_THREAD_H
