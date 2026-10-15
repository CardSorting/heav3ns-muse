/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QElapsedTimer>
#include <QScopeGuard>
#include <QSemaphore>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "AudioEngineWorkerThread.h"
#include "ThreadableJob.h"

namespace lmms
{
namespace
{
using Worker = AudioEngineWorkerThread;
using Mode = Worker::JobQueue::OperationMode;
using Failure = Worker::JobQueue::Failure;

class Job final : public ThreadableJob
{
public:
	std::atomic<int> calls{0};
	std::function<void()> action;
	bool requiresProcessing() const override { return true; }
protected:
	void doProcessing() override
	{
		++calls;
		if (action) { action(); }
	}
};

class Pool
{
public:
	explicit Pool(int count)
	{
		for (int i = 0; i < count; ++i)
		{
			auto worker = std::make_unique<Worker>(nullptr);
			worker->start(); workers.push_back(std::move(worker));
		}
	}
	~Pool()
	{
		for (const auto& worker : workers) { worker->quit(); }
		for (const auto& worker : workers) { worker->wait(); }
	}
	std::vector<std::unique_ptr<Worker>> workers;
};

// The release guard in each concurrent test runs before joining the dispatcher
// or destroying jobs/workers, including when a Qt assertion returns early.
struct Gate
{
	QSemaphore arrived;
	QSemaphore release;
	void enter() { arrived.release(); release.acquire(); }
};
}

class AudioEngineWorkerTest : public QObject
{
	Q_OBJECT
private slots:
	void init()
	{
		const auto status = Worker::schedulerStatus();
		QCOMPARE(status.registered, 0); QVERIFY(!status.batchActive);
		Worker::resetJobQueue();
	}
	void cleanup()
	{
		const auto status = Worker::schedulerStatus();
		QCOMPARE(status.registered, 0); QCOMPARE(status.queued, 0);
		QCOMPARE(status.scanning, 0); QVERIFY(!status.batchActive);
	}
	void coordinatorProcessesWithoutWorkersAndBeforeWorkerStartup()
	{
		Job first; Worker::addJob(&first); Worker::startAndWaitForJobs();
		QCOMPARE(first.calls.load(), 1); QCOMPARE(first.state(), ThreadableJob::ProcessingState::Done);
		Worker unstarted(nullptr); Worker::resetJobQueue();
		Job second; Worker::addJob(&second); Worker::startAndWaitForJobs();
		QCOMPARE(second.calls.load(), 1); QCOMPARE(Worker::schedulerStatus().ready, 0);
		// An early quit remains a predicate when run() begins; no wake is needed.
		unstarted.quit(); unstarted.start(); QVERIFY(unstarted.wait(5000));
		QCOMPARE(Worker::schedulerStatus().ready, 0);
	}
	void readyWorkersFinishRepeatedStaticAndDynamicBatches_data()
	{
		QTest::addColumn<int>("workerCount");
		QTest::newRow("one-worker") << 1; QTest::newRow("three-workers") << 3; QTest::newRow("nine-workers") << 9;
	}
	void readyWorkersFinishRepeatedStaticAndDynamicBatches()
	{
		QFETCH(int, workerCount); Pool pool(workerCount);
		QTRY_COMPARE(Worker::schedulerStatus().ready, workerCount);
		std::array<Job, 37> jobs;
		for (int iteration = 0; iteration < 256; ++iteration)
		{
			Worker::resetJobQueue(iteration % 2 ? Mode::Dynamic : Mode::Static);
			for (auto& job : jobs) { job.reset(); Worker::addJob(&job); }
			Worker::startAndWaitForJobs();
			for (const auto& job : jobs)
			{
				QCOMPARE(job.calls.load(), iteration + 1);
				QCOMPARE(job.state(), ThreadableJob::ProcessingState::Done);
			}
			const auto status = Worker::schedulerStatus();
			QCOMPARE(status.ready, workerCount); QCOMPARE(status.queued, 0);
			QCOMPARE(status.scanning, 0); QVERIFY(!status.batchActive);
			QCOMPARE(Worker::jobQueueFailure(), Failure::None);
			// Empty dispatches also have to retire every admitted scanner.
			Worker::resetJobQueue(); Worker::startAndWaitForJobs();
			QVERIFY(!Worker::schedulerStatus().batchActive);
		}
	}
	void dynamicDependenciesRunExactlyOnce_data()
	{
		QTest::addColumn<int>("workerCount");
		QTest::newRow("coordinator") << 0; QTest::newRow("one-worker") << 1; QTest::newRow("three-workers") << 3; QTest::newRow("nine-workers") << 9;
	}
	void backgroundDispatchRetiresScansWhileStatusIsObserved()
	{
		Pool pool(9); QTRY_COMPARE(Worker::schedulerStatus().ready, 9);
		Job job; std::atomic_bool finished{false}; std::atomic<int> batches{0};
		std::jthread dispatcher([&] {
			for (int iteration = 0; iteration < 4096; ++iteration)
			{
				Worker::resetJobQueue(iteration % 4 < 2 ? Mode::Static : Mode::Dynamic);
				job.reset(); if (iteration % 2 == 0) { Worker::addJob(&job); }
				Worker::startAndWaitForJobs(); ++batches;
			}
			finished = true;
		});
		bool consistent = true;
		while (!finished.load())
		{
			const auto status = Worker::schedulerStatus();
			consistent &= status.registered == 9 && status.ready == 9 && status.queued + status.scanning <= 9;
			std::this_thread::yield();
		}
		dispatcher.join(); QVERIFY(consistent); QCOMPARE(batches.load(), 4096); QCOMPARE(job.calls.load(), 2048);
		const auto status = Worker::schedulerStatus(); QVERIFY(!status.batchActive); QCOMPARE(status.queued, 0); QCOMPARE(status.scanning, 0);
	}
	void dynamicDependenciesRunExactlyOnce()
	{
		QFETCH(int, workerCount); Pool pool(workerCount);
		QTRY_COMPARE(Worker::schedulerStatus().ready, workerCount);
		std::array<Job, 31> jobs;
		for (std::size_t index = 0; index < 15; ++index)
		{
			jobs[index].action = [&, index] {
				Worker::addJob(&jobs[index * 2 + 1]);
				std::this_thread::yield();
				Worker::addJob(&jobs[index * 2 + 2]);
			};
		}
		for (int iteration = 0; iteration < 64; ++iteration)
		{
			Worker::resetJobQueue(Mode::Dynamic);
			for (auto& job : jobs) { job.reset(); }
			Worker::addJob(&jobs[0]); Worker::startAndWaitForJobs();
			for (const auto& job : jobs)
			{
				QCOMPARE(job.calls.load(), iteration + 1);
				QCOMPARE(job.state(), ThreadableJob::ProcessingState::Done);
			}
			QCOMPARE(Worker::jobQueueFailure(), Failure::None);
			QCOMPARE(Worker::schedulerStatus().scanning, 0);
		}
	}
	void exceptionsDrainTheBatchAndAllowRetry_data()
	{
		QTest::addColumn<int>("workerCount"); QTest::addColumn<bool>("nonstandard");
		QTest::newRow("coordinator-standard") << 0 << false;
		QTest::newRow("coordinator-nonstandard") << 0 << true;
		QTest::newRow("workers-standard") << 3 << false;
		QTest::newRow("workers-nonstandard") << 3 << true;
	}
	void exceptionsDrainTheBatchAndAllowRetry()
	{
		QFETCH(int, workerCount); QFETCH(bool, nonstandard); Pool pool(workerCount);
		QTRY_COMPARE(Worker::schedulerStatus().ready, workerCount);
		Job root; std::array<Job, 32> children;
		root.action = [&] {
			for (auto& child : children) { Worker::addJob(&child); }
			if (nonstandard) { throw 7; }
			throw std::runtime_error("Native worker fixture exception");
		};
		Worker::resetJobQueue(Mode::Dynamic); Worker::addJob(&root); Worker::startAndWaitForJobs();
		QCOMPARE(root.calls.load(), 1); QCOMPARE(root.state(), ThreadableJob::ProcessingState::Done);
		QCOMPARE(Worker::jobQueueFailure(), Failure::ProcessingException);
		for (const auto& child : children)
		{
			QVERIFY(child.calls.load() <= 1); QCOMPARE(child.state(), ThreadableJob::ProcessingState::Done);
		}
		Worker::resetJobQueue(); Job healthy; Worker::addJob(&healthy); Worker::startAndWaitForJobs();
		QCOMPARE(healthy.calls.load(), 1); QCOMPARE(Worker::jobQueueFailure(), Failure::None);
		QCOMPARE(Worker::schedulerStatus().scanning, 0);
	}
	void activeDispatchRejectsResetReentryAndRosterMutation()
	{
		Pool pool(1); QTRY_COMPARE(Worker::schedulerStatus().ready, 1);
		Gate gate; std::array<Job, 2> jobs;
		for (auto& job : jobs) { job.action = [&] { gate.enter(); }; Worker::addJob(&job); }
		std::jthread dispatcher([] { Worker::startAndWaitForJobs(); });
		const auto release = qScopeGuard([&] { gate.release.release(2); });
		QVERIFY(gate.arrived.tryAcquire(2, 5000));
		const auto active = Worker::schedulerStatus();
		QVERIFY(active.batchActive); QCOMPARE(active.scanning, 1); QCOMPARE(active.queued, 0);
		QVERIFY_EXCEPTION_THROWN(Worker::resetJobQueue(Mode::Dynamic), std::logic_error);
		QVERIFY_EXCEPTION_THROWN(Worker::startAndWaitForJobs(), std::logic_error);
		QVERIFY_EXCEPTION_THROWN(Worker(nullptr), std::logic_error);
		gate.release.release(2); dispatcher.join();
		for (const auto& job : jobs) { QCOMPARE(job.calls.load(), 1); }
		Worker::resetJobQueue(); Worker::startAndWaitForJobs();
		QCOMPARE(Worker::schedulerStatus().registered, 1);
	}
	void quitWaitsForAdmittedWorkAndDoesNotEraseTheBatch()
	{
		Pool pool(1); QTRY_COMPARE(Worker::schedulerStatus().ready, 1);
		Gate gate; std::array<Job, 2> jobs;
		for (auto& job : jobs) { job.action = [&] { gate.enter(); }; Worker::addJob(&job); }
		std::atomic_bool finished{false};
		std::jthread dispatcher([&] { Worker::startAndWaitForJobs(); finished = true; });
		const auto release = qScopeGuard([&] { gate.release.release(2); });
		QVERIFY(gate.arrived.tryAcquire(2, 5000));
		pool.workers.front()->quit(); pool.workers.front()->quit();
		const auto quitting = Worker::schedulerStatus();
		QVERIFY(quitting.batchActive); QCOMPARE(quitting.ready, 0); QCOMPARE(quitting.scanning, 1);
		QVERIFY(!finished.load()); QVERIFY(!pool.workers.front()->wait(1));
		gate.release.release(2); dispatcher.join(); QVERIFY(pool.workers.front()->wait(5000));
		for (const auto& job : jobs)
		{
			QCOMPARE(job.calls.load(), 1); QCOMPARE(job.state(), ThreadableJob::ProcessingState::Done);
		}
		QVERIFY(finished.load()); QCOMPARE(Worker::schedulerStatus().scanning, 0);
		Worker::resetJobQueue(); Job afterQuit; Worker::addJob(&afterQuit); Worker::startAndWaitForJobs();
		QCOMPARE(afterQuit.calls.load(), 1); QCOMPARE(Worker::jobQueueFailure(), Failure::None);
	}
	void workerDestructionJoinsItsActiveNativeProcessor()
	{
		auto worker = std::make_unique<Worker>(nullptr); worker->start();
		QTRY_COMPARE(Worker::schedulerStatus().ready, 1);
		QSemaphore entered, permitExit; Job job;
		job.action = [&] { entered.release(); permitExit.acquire(); };
		// Both jobs use the same gate: whichever is on the worker must
		// remain active until a helper observes quit through the production status.
		Job coordinator;
		coordinator.action = job.action;
		Worker::addJob(&coordinator); Worker::addJob(&job);
		std::jthread dispatcher([] { Worker::startAndWaitForJobs(); });
		const auto release = qScopeGuard([&] { permitExit.release(2); });
		QVERIFY(entered.tryAcquire(2, 5000));
		std::atomic_bool observedQuit{false};
		std::jthread unblock([&] {
			QElapsedTimer timer; timer.start();
			while (Worker::schedulerStatus().ready != 0 && timer.elapsed() < 5000) { std::this_thread::yield(); }
			const auto status = Worker::schedulerStatus();
			observedQuit = status.ready == 0 && status.scanning == 1 && status.batchActive;
			permitExit.release(2);
		});
		worker.reset(); unblock.join(); dispatcher.join();
		QVERIFY(observedQuit.load()); QCOMPARE(Worker::schedulerStatus().registered, 0);
		QCOMPARE(job.calls.load(), 1); QCOMPARE(coordinator.calls.load(), 1);
		QCOMPARE(job.state(), ThreadableJob::ProcessingState::Done);
		QCOMPARE(coordinator.state(), ThreadableJob::ProcessingState::Done);
	}
	void quittingRacesWithAdmissionWithoutLosingOrRepeatingJobs()
	{
		// This repeated race permits either pending cancellation or an active
		// scanner. Both outcomes must drain the batch before its next reset.
		for (int iteration = 0; iteration < 64; ++iteration)
		{
			Pool pool(2); QTRY_COMPARE(Worker::schedulerStatus().ready, 2);
			std::array<Job, 23> jobs;
			Worker::resetJobQueue(iteration % 2 ? Mode::Dynamic : Mode::Static);
			for (auto& job : jobs) { Worker::addJob(&job); }
			std::jthread dispatcher([] { Worker::startAndWaitForJobs(); });
			for (const auto& worker : pool.workers) { worker->quit(); }
			dispatcher.join();
			for (const auto& worker : pool.workers) { QVERIFY(worker->wait(5000)); }
			for (const auto& job : jobs)
			{
				QCOMPARE(job.calls.load(), 1); QCOMPARE(job.state(), ThreadableJob::ProcessingState::Done);
			}
			const auto status = Worker::schedulerStatus();
			QCOMPARE(status.queued, 0); QCOMPARE(status.scanning, 0); QVERIFY(!status.batchActive);
			QCOMPARE(Worker::jobQueueFailure(), Failure::None);
		}
	}
	void destroyingAnIdlePoolAllowsFreshWorkersToTakeOver()
	{
		for (int iteration = 0; iteration < 16; ++iteration)
		{
			{
				Pool pool(2); QTRY_COMPARE(Worker::schedulerStatus().ready, 2);
				Job job; Worker::addJob(&job); Worker::startAndWaitForJobs();
				QCOMPARE(job.calls.load(), 1);
			}
			const auto status = Worker::schedulerStatus();
			QCOMPARE(status.registered, 0); QCOMPARE(status.ready, 0);
			QCOMPARE(status.scanning, 0); QCOMPARE(status.queued, 0); QVERIFY(!status.batchActive);
		}
	}
};
} // namespace lmms

QTEST_GUILESS_MAIN(lmms::AudioEngineWorkerTest)
#include "AudioEngineWorkerTest.moc"
