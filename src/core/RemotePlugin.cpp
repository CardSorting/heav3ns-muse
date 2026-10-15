/*
 * RemotePlugin.cpp - base class providing RPC like mechanisms
 *
 * Copyright (c) 2008-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "RemotePlugin.h"

//#define DEBUG_REMOTE_PLUGIN
#ifdef DEBUG_REMOTE_PLUGIN
#include <QDebug>
#endif

#ifdef LMMS_BUILD_WIN32
#include <windows.h>
#endif

#include "AudioEngine.h"
#include "Engine.h"
#include "MidiEvent.h"
#include "Song.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QUuid>
#include <QTimer>
#include <QMutexLocker>
#include <chrono>
#include <algorithm>
#include <limits>
#include <QElapsedTimer>

#ifndef SYNC_WITH_SHM_FIFO
#include <sys/socket.h>
#include <sys/un.h>
#endif

#ifdef LMMS_BUILD_WIN32

namespace {

HANDLE getRemotePluginJob()
{
	static const auto job = []
	{
		const auto job = CreateJobObject(nullptr, nullptr);

		auto limitInfo = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
		limitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limitInfo, sizeof(limitInfo));

		return job;
	}();

	return job;
}

} // namespace

#endif // LMMS_BUILD_WIN32

namespace lmms
{

namespace
{
std::atomic<std::uint64_t> remoteFailureCount{0};
std::mutex renderContextMutex;
std::uint64_t nextRenderContext = 1;
std::atomic<std::uint64_t> activeRenderContext{0};
std::atomic<std::uint64_t> executingRenderContext{0};
std::atomic<std::uint64_t> cancelledRenderContext{0};
std::atomic<std::int64_t> renderContextDeadline{0};

std::int64_t monotonicMilliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

class RemotePlugin::RequestDeadline
{
public:
	RequestDeadline(RemotePlugin& plugin, int timeoutMs = 0) :
		m_plugin(plugin), m_previous(plugin.m_requestDeadline.load()),
		m_previousContext(plugin.m_requestRenderContext.load())
	{
		const auto deadline = monotonicMilliseconds() + (timeoutMs ? timeoutMs : plugin.m_timeoutMs);
		plugin.m_requestDeadline = m_previous ? std::min(m_previous, deadline) : deadline;
		if (!m_previousContext) { plugin.m_requestRenderContext = executingRenderContext.load(); }
	}
	~RequestDeadline()
	{
		m_plugin.m_requestDeadline = m_previous;
		m_plugin.m_requestRenderContext = m_previousContext;
	}
private:
	RemotePlugin& m_plugin;
	std::int64_t m_previous;
	std::uint64_t m_previousContext;
};

std::uint64_t RemotePlugin::failureCount() { return remoteFailureCount.load(); }
bool RemotePlugin::hasActiveRenderContext() { return activeRenderContext.load() != 0; }
bool RemotePlugin::hasRunningRenderExecution() { return executingRenderContext.load() != 0; }

std::uint64_t RemotePlugin::beginRenderContext(int timeoutMs)
{
	const auto lock = std::lock_guard{renderContextMutex};
	if (activeRenderContext || nextRenderContext == std::numeric_limits<std::uint64_t>::max()) { return 0; }
	const auto context = nextRenderContext++;
	renderContextDeadline = timeoutMs > 0 ? monotonicMilliseconds() + timeoutMs : 0;
	executingRenderContext = context;
	activeRenderContext = context;
	return context;
}

void RemotePlugin::cancelRenderContext(std::uint64_t context)
{
	// Keep cancellation of retired contexts visible to their outstanding requests;
	// monotonically allocated IDs keep it from cancelling a later export.
	auto cancelled = cancelledRenderContext.load();
	while (context > cancelled && !cancelledRenderContext.compare_exchange_weak(cancelled, context)) { }
}

void RemotePlugin::finishRenderExecution(std::uint64_t context)
{
	const auto lock = std::lock_guard{renderContextMutex};
	if (context && activeRenderContext == context) { executingRenderContext = 0; }
}

void RemotePlugin::endRenderContext(std::uint64_t context)
{
	if (context)
	{
		const auto lock = std::lock_guard{renderContextMutex};
		if (activeRenderContext == context)
		{
			executingRenderContext = 0;
			activeRenderContext = 0;
		}
	}
}

bool RemotePlugin::renderContextCancelled(std::uint64_t context)
{
	if (!context) { return false; }
	if (context <= cancelledRenderContext.load()) { return true; }
	// Read the ID and deadline together. A watcher checking an older request
	// must not combine a retired deadline with a newly started export's ID.
	const auto lock = std::lock_guard{renderContextMutex};
	const auto deadline = renderContextDeadline.load();
	if (context == activeRenderContext.load() && deadline && monotonicMilliseconds() >= deadline)
	{
		cancelRenderContext(context);
		return true;
	}
	return false;
}

bool RemotePlugin::requestCancelled()
{
	if (!m_requestDeadline) { return false; }
	auto context = m_requestRenderContext.load();
	if (!context)
	{
		// Export setup can be stopping a hardware period whose RPC began before
		// the render context. Adopt it so the watcher can unwind that wait too.
		const auto active = executingRenderContext.load();
		m_requestRenderContext.compare_exchange_strong(context, active);
		context = m_requestRenderContext.load();
	}
	return renderContextCancelled(context);
}

void RemotePlugin::recordFailure()
{
	if (!m_failed.exchange(true)) { ++remoteFailureCount; }
}

int RemotePlugin::sendMessage(const message& m)
{
	QMutexLocker locker(&m_commMutex);
	RequestDeadline deadline(*this);
	if (requestCancelled()) { recordFailure(); invalidate(); return 0; }
	const auto bytes = RemotePluginBase::sendMessage(m);
	if (!bytes) { recordFailure(); }
	return bytes;
}

RemotePluginBase::message RemotePlugin::waitForMessage(const message& expected, bool busyWaiting)
{
	QMutexLocker locker(&m_commMutex);
	RequestDeadline deadline(*this);
	if (requestCancelled()) { recordFailure(); invalidate(); return {}; }
	const auto received = RemotePluginBase::waitForMessage(expected, busyWaiting);
	if (received.id != expected.id) { recordFailure(); invalidate(); }
	return received;
}

void RemotePlugin::fetchAndProcessAllMessages()
{
	QMutexLocker locker(&m_commMutex);
	RequestDeadline deadline(*this);
	if (requestCancelled()) { recordFailure(); invalidate(); return; }
	RemotePluginBase::fetchAndProcessAllMessages();
	if (isInvalid()) { recordFailure(); }
}

// simple helper thread monitoring our RemotePlugin - if process terminates
// unexpectedly invalidate plugin so LMMS doesn't lock up
ProcessWatcher::ProcessWatcher( RemotePlugin * _p ) :
	QThread(),
	m_plugin( _p ),
	m_quit( false )
{
}


void ProcessWatcher::run()
{
	auto& process = m_plugin->m_process;
	process.start(m_plugin->m_exec, m_plugin->m_args);
	QElapsedTimer startup;
	startup.start();
	while (process.state() == QProcess::Starting && startup.elapsed() < 30000
		&& !m_quit && !m_plugin->isInvalid() && !m_plugin->requestCancelled())
	{
		if (process.waitForStarted(25)) { break; }
	}
	if (process.state() != QProcess::Running || m_quit || m_plugin->isInvalid() || m_plugin->requestCancelled())
	{
		m_plugin->recordFailure();
		m_plugin->invalidate();
		process.kill();
		process.waitForFinished(1000);
		m_plugin->m_running = false;
		process.moveToThread(m_plugin->thread());
		return;
	}
	m_plugin->m_running = true;

#ifdef LMMS_BUILD_WIN32
	// Add the process to our job so it is killed if we crash
	if (process.state() == QProcess::Running)
	{
		if (const auto processHandle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, false, process.processId()))
		{
			// Ensure the process is still running, otherwise the handle we
			// obtained may be for a different process that happened to reuse
			// the same process id.
			// QProcess::state() alone is insufficient as it only returns a
			// cached state variable that is updated asynchronously. To query
			// the process itself, we can use QProcess::waitForFinished() with a
			// zero timeout, but that too is insufficient as it fails if the
			// process has already finished. Therefore, we check both.
			if (!process.waitForFinished(0) && process.state() == QProcess::Running)
			{
				AssignProcessToJobObject(getRemotePluginJob(), processHandle);
			}
			CloseHandle(processHandle);
		}
	}
#endif // LMMS_BUILD_WIN32

	QTimer watchdog;
	watchdog.setInterval(25);
	QObject::connect(&watchdog, &QTimer::timeout, &watchdog, [&]
	{
		if (m_quit) { quit(); return; }
		if (m_plugin->requestCancelled() && !m_plugin->isInvalid())
		{
			qWarning() << "Remote plugin request interrupted by native render cancellation" << m_plugin->m_exec;
			m_plugin->recordFailure();
			m_plugin->invalidate();
		}
		const auto deadline = m_plugin->m_requestDeadline.load();
		if (deadline && monotonicMilliseconds() >= deadline && !m_plugin->isInvalid())
		{
			qCritical() << "Remote plugin communication deadline exceeded" << m_plugin->m_exec;
			m_plugin->recordFailure();
			m_plugin->invalidate();
		}
		if (m_plugin->isInvalid()) { process.kill(); quit(); }
	});
	watchdog.start();
	if (!m_quit && process.state() == QProcess::Running) { exec(); }
	watchdog.stop();
	if (!m_quit)
	{
		m_plugin->recordFailure();
	}
	// Do not inspect/drain the FIFO after a child exits: its data lock may
	// have been abandoned. Invalidation wakes both transports without it.
	m_plugin->invalidate();
	if (process.state() != QProcess::NotRunning)
	{
		if (!m_quit || !process.waitForFinished(1000))
		{
			process.kill();
			process.waitForFinished(1000);
		}
	}
	m_plugin->m_running = process.state() != QProcess::NotRunning;
	process.moveToThread(m_plugin->thread());
}





RemotePlugin::RemotePlugin() :
	QObject(),
#ifdef SYNC_WITH_SHM_FIFO
	RemotePluginBase( new shmFifo(), new shmFifo() ),
#else
	RemotePluginBase(),
#endif
	m_failed( true ),
	m_watcher( this ),
	m_splitChannels( false ),
	m_audioBufferSize( 0 ),
	m_inputCount( DEFAULT_CHANNELS ),
	m_outputCount( DEFAULT_CHANNELS )
{
	bool validTimeout = false;
	const auto configuredTimeout = qEnvironmentVariableIntValue("LMMS_REMOTE_PLUGIN_TIMEOUT_MS", &validTimeout);
	if (validTimeout && configuredTimeout >= 1000 && configuredTimeout <= 600000) { m_timeoutMs = configuredTimeout; }
	else if (qEnvironmentVariableIsSet("LMMS_REMOTE_PLUGIN_TIMEOUT_MS"))
	{
		qWarning() << "Invalid LMMS_REMOTE_PLUGIN_TIMEOUT_MS; using 120000 ms (supported range 1000..600000)";
	}
#ifndef SYNC_WITH_SHM_FIFO
	struct sockaddr_un sa;
	sa.sun_family = AF_LOCAL;

	m_socketFile = QDir::tempPath() + QDir::separator() +
						QUuid::createUuid().toString();
	auto path = m_socketFile.toUtf8();
	size_t length = path.length();
	if ( length >= sizeof sa.sun_path )
	{
		length = sizeof sa.sun_path - 1;
		qWarning( "Socket path too long." );
	}
	memcpy(sa.sun_path, path.constData(), length );
	sa.sun_path[length] = '\0';

	m_server = socket( PF_LOCAL, SOCK_STREAM, 0 );
	if ( m_server == -1 )
	{
		qWarning( "Unable to start the server." );
	}
	remove(path.constData());
	int ret = bind( m_server, (struct sockaddr *) &sa, sizeof sa );
	if ( ret == -1 || listen( m_server, 1 ) == -1 )
	{
		qWarning( "Unable to start the server." );
	}
#endif

	connect( &m_process, SIGNAL(finished(int,QProcess::ExitStatus)),
		this, SLOT(processFinished(int,QProcess::ExitStatus)),
		Qt::DirectConnection );
	connect( &m_process, SIGNAL(errorOccurred(QProcess::ProcessError)),
			 this, SLOT(processErrored(QProcess::ProcessError)),
		Qt::DirectConnection );
	connect( &m_process, SIGNAL(finished(int,QProcess::ExitStatus)),
		&m_watcher, SLOT(quit()), Qt::DirectConnection );
	connect(&m_process, &QProcess::readyReadStandardOutput, &m_process, [this]
	{
		const auto diagnostics = m_process.readAllStandardOutput();
		std::fwrite(diagnostics.constData(), 1, static_cast<std::size_t>(diagnostics.size()), stderr);
		std::fflush(stderr);
	}, Qt::DirectConnection);
}




RemotePlugin::~RemotePlugin()
{
	retireProcess();

#ifndef SYNC_WITH_SHM_FIFO
	if ( close( m_server ) == -1)
	{
		qWarning( "Error freeing resources." );
	}
	remove( m_socketFile.toUtf8().constData() );
	if (m_socket >= 0) { close(m_socket); }
#endif
}

void RemotePlugin::retireProcess()
{
	// Mark orderly retirement before IdQuit can make QProcess emit finished.
	m_watcher.m_quit = true;
	if (m_watcher.isRunning() && m_running && !isInvalid())
	{
		RequestDeadline deadline(*this, 500);
		RemotePluginBase::sendMessage(IdQuit);
	}
	m_watcher.stop();
	m_watcher.wait();
	if (m_process.state() != QProcess::NotRunning)
	{
		m_process.kill();
		m_process.waitForFinished(1000);
	}
	m_running = false;
}




bool RemotePlugin::init(const QString &pluginExecutable,
							bool waitForInitDoneMsg , QStringList extraArgs)
{
	QMutexLocker initializationLock(&m_commMutex);
	RequestDeadline deadline(*this);
	if (requestCancelled()) { recordFailure(); invalidate(); return true; }
	retireProcess();
#ifdef SYNC_WITH_SHM_FIFO
	reset(new shmFifo(), new shmFifo());
#else
	resetConnection();
#endif
	m_failed = false;
	m_inputCount = DEFAULT_CHANNELS;
	m_outputCount = DEFAULT_CHANNELS;
	m_audioBuffer.detach();
	m_audioBufferSize = 0;
	QString exec = QFileInfo(QDir("plugins:"), pluginExecutable).absoluteFilePath();

	// We may have received a directory via a environment variable
	if (const char* env_path = std::getenv("LMMS_PLUGIN_DIR"))
			exec = QFileInfo(QDir(env_path), pluginExecutable).absoluteFilePath();

#ifdef LMMS_BUILD_APPLE
	// search current directory first
	QString curDir = QCoreApplication::applicationDirPath() + "/" + pluginExecutable;
	if( QFile( curDir ).exists() )
	{
		exec = curDir;
	}
#endif
#ifdef LMMS_BUILD_WIN32
	if( ! exec.endsWith( ".exe", Qt::CaseInsensitive ) )
	{
		exec += ".exe";
	}
#endif

	if( ! QFile( exec ).exists() )
	{
		qWarning( "Remote plugin '%s' not found",
						exec.toUtf8().constData() );
		recordFailure();
		invalidate();
		return failed();
	}

	// ensure the watcher is ready in case we're running again
	// (e.g. 32-bit VST plugins on Windows)
	m_watcher.wait();
	m_watcher.reset();

	QStringList args;
#ifdef SYNC_WITH_SHM_FIFO
	// swap in and out for bidirectional communication
	args << QString::fromStdString(out()->shmKey());
	args << QString::fromStdString(in()->shmKey());
#else
	args << m_socketFile;
#endif
	args << extraArgs;
#ifndef DEBUG_REMOTE_PLUGIN
	m_process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
	m_process.setWorkingDirectory( QCoreApplication::applicationDirPath() );
	m_exec = exec;
	m_args = args;
	// we start the process on the watcher thread to work around QTBUG-8819
	m_process.moveToThread( &m_watcher );
	m_watcher.start( QThread::LowestPriority );
#else
	qDebug() << exec << args;
#endif

#ifndef SYNC_WITH_SHM_FIFO
	struct pollfd pollin{};
	pollin.fd = m_server;
	pollin.events = POLLIN;

	QElapsedTimer connection;
	connection.start();
	while (connection.elapsed() < 30000 && !failed() && !isInvalid() && !requestCancelled())
	{
		const auto available = poll(&pollin, 1, 25);
		if (available < 0 && errno != EINTR) { qWarning("Unexpected poll error."); break; }
		if (available > 0 && (pollin.revents & POLLIN))
		{
			m_socket = accept( m_server, nullptr, nullptr );
			if ( m_socket == -1 )
			{
				qWarning( "Unexpected socket error." );
			}
			break;
		}
		if (available > 0 && (pollin.revents & (POLLERR | POLLNVAL | POLLHUP))) { break; }
		if (QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread())
		{
			QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 5);
		}
	}
	if (m_socket == -1)
	{
		recordFailure();
		invalidate();
		return true;
	}
#endif

	sendMessage(message(IdSyncKey).addString(Engine::getSong()->syncKey()));
	resizeSharedProcessingMemory();

	if( waitForInitDoneMsg )
	{
		waitForInitDone();
	}

	return failed();
}




bool RemotePlugin::process( const SampleFrame* _in_buf, SampleFrame* _out_buf )
{
	QMutexLocker processingLock(&m_commMutex);
	RequestDeadline deadline(*this);
	const f_cnt_t frames = Engine::audioEngine()->framesPerPeriod();

	if (m_failed || isInvalid() || !isRunning())
	{
		// A previously failed host used by a new export must fail that export,
		// rather than publish silence merely because the original failure is old.
		++remoteFailureCount;
		recordFailure();
		if( _out_buf != nullptr )
		{
			zeroSampleFrames(_out_buf, frames);
		}
		return false;
	}

	if (!m_audioBuffer)
	{
		// m_audioBuffer being zero means we didn't initialize everything so
		// far so process one message each time (and hope we get
		// information like SHM-key etc.) until we process messages
		// in a later stage of this procedure
		if( m_audioBufferSize == 0 )
		{
			lock();
			fetchAndProcessAllMessages();
			unlock();
		}
		if( _out_buf != nullptr )
		{
			zeroSampleFrames(_out_buf, frames);
		}
		return false;
	}

	memset( m_audioBuffer.get(), 0, m_audioBufferSize );

	ch_cnt_t inputs = std::min<ch_cnt_t>(m_inputCount, DEFAULT_CHANNELS);

	if( _in_buf != nullptr && inputs > 0 )
	{
		if( m_splitChannels )
		{
			for( ch_cnt_t ch = 0; ch < inputs; ++ch )
			{
				for( f_cnt_t frame = 0; frame < frames; ++frame )
				{
					m_audioBuffer[ch * frames + frame] =
							_in_buf[frame][ch];
				}
			}
		}
		else if( inputs == DEFAULT_CHANNELS )
		{
			auto target = m_audioBuffer.get();
			copyFromSampleFrames(target, _in_buf, frames);
		}
		else
		{
			auto o = (SampleFrame*)m_audioBuffer.get();
			for( ch_cnt_t ch = 0; ch < inputs; ++ch )
			{
				for( f_cnt_t frame = 0; frame < frames; ++frame )
				{
					o[frame][ch] = _in_buf[frame][ch];
				}
			}
		}
	}

	lock();
	sendMessage( IdStartProcessing );

	const auto completed = waitForMessage(IdProcessingDone).id == IdProcessingDone && !failed() && !isInvalid();
	unlock();
	if (!completed)
	{
		if (_out_buf) { zeroSampleFrames(_out_buf, frames); }
		return false;
	}
	if (!_out_buf || m_outputCount == 0) { return false; }

	const ch_cnt_t outputs = std::min<ch_cnt_t>(m_outputCount,
							DEFAULT_CHANNELS);
	if( m_splitChannels )
	{
		for( ch_cnt_t ch = 0; ch < outputs; ++ch )
		{
			for( f_cnt_t frame = 0; frame < frames; ++frame )
			{
				_out_buf[frame][ch] = m_audioBuffer[( m_inputCount+ch )*
								frames + frame];
			}
		}
	}
	else if( outputs == DEFAULT_CHANNELS )
	{
		auto source = m_audioBuffer.get() + m_inputCount * frames;
		copyToSampleFrames(_out_buf, source, frames);
	}
	else
	{
		auto o = (SampleFrame*)(m_audioBuffer.get() + m_inputCount * frames);
		// clear buffer, if plugin didn't fill up both channels
		zeroSampleFrames(_out_buf, frames);

		for (ch_cnt_t ch = 0; ch <
				std::min<int>(DEFAULT_CHANNELS, outputs); ++ch)
		{
			for( f_cnt_t frame = 0; frame < frames; ++frame )
			{
				_out_buf[frame][ch] = o[frame][ch];
			}
		}
	}

	return true;
}




void RemotePlugin::processMidiEvent( const MidiEvent & _e,
							const f_cnt_t _offset )
{
	message m( IdMidiEvent );
	m.addInt( _e.type() );
	m.addInt( _e.channel() );
	m.addInt( _e.param( 0 ) );
	m.addInt( _e.param( 1 ) );
	m.addInt( _offset );
	lock();
	sendMessage( m );
	unlock();
}

void RemotePlugin::showUI()
{
	lock();
	sendMessage( IdShowUI );
	unlock();
}

void RemotePlugin::hideUI()
{
	lock();
	sendMessage( IdHideUI );
	unlock();
}




void RemotePlugin::resizeSharedProcessingMemory()
{
	const auto frames = Engine::audioEngine()->framesPerPeriod();
	constexpr std::size_t maximumAudioBytes = 64 * 1024 * 1024;
	const auto maximumChannels = frames > 0 ? maximumAudioBytes / sizeof(float) / frames : 0;
	if (m_inputCount < 0 || m_outputCount < 0
		|| static_cast<std::size_t>(m_inputCount) > maximumChannels
		|| static_cast<std::size_t>(m_outputCount) > maximumChannels - m_inputCount)
	{
		qCritical() << "Remote plugin reported invalid or excessive audio channel counts";
		m_audioBuffer.detach();
		m_audioBufferSize = 0;
		recordFailure();
		invalidate();
		return;
	}
	const size_t s = (m_inputCount + m_outputCount) * Engine::audioEngine()->framesPerPeriod();
	try
	{
		m_audioBuffer.create(s);
	}
	catch (const std::runtime_error& error)
	{
		qCritical() << "Failed to allocate shared audio buffer:" << error.what();
		m_audioBuffer.detach();
		m_audioBufferSize = 0;
		recordFailure();
		invalidate();
		return;
	}
	m_audioBufferSize = s * sizeof(float);
	sendMessage(message(IdChangeSharedMemoryKey).addString(m_audioBuffer.key()));
}




void RemotePlugin::processFinished( int exitCode,
					QProcess::ExitStatus exitStatus )
{
	m_running = false;
	if (m_watcher.m_quit) { return; }
	recordFailure();
	invalidate();
	if ( exitStatus == QProcess::CrashExit )
	{
		qCritical() << "Remote plugin crashed";
	}
	else if ( exitCode )
	{
		qCritical() << "Remote plugin exit code: " << exitCode;
	}
}

void RemotePlugin::processErrored( QProcess::ProcessError err )
{
	if (m_watcher.m_quit || err == QProcess::Timedout) { return; }
	qCritical() << "Process error: " << err;
	if (err == QProcess::FailedToStart || err == QProcess::Crashed) { recordFailure(); invalidate(); }
}




bool RemotePlugin::processMessage( const message & _m )
{
	lock();
	message reply_message( _m.id );
	bool reply = false;
	switch( _m.id )
	{
		case IdUndefined:
			unlock();
			return false;

		case IdInitDone:
			reply = true;
			break;

		case IdSampleRateInformation:
			reply = true;
			reply_message.addInt( Engine::audioEngine()->outputSampleRate() );
			break;

		case IdBufferSizeInformation:
			reply = true;
			reply_message.addInt( Engine::audioEngine()->framesPerPeriod() );
			break;

		case IdChangeInputCount:
			m_inputCount = _m.getInt( 0 );
			resizeSharedProcessingMemory();
			break;

		case IdChangeOutputCount:
			m_outputCount = _m.getInt( 0 );
			resizeSharedProcessingMemory();
			break;

		case IdChangeInputOutputCount:
			m_inputCount = _m.getInt( 0 );
			m_outputCount = _m.getInt( 1 );
			resizeSharedProcessingMemory();
			break;

		case IdDebugMessage:
			fprintf( stderr, "RemotePlugin::DebugMessage: %s",
						_m.getString( 0 ).c_str() );
			break;

		case IdProcessingDone:
		case IdQuit:
		default:
			break;
	}
	if( reply )
	{
		sendMessage( reply_message );
	}
	unlock();

	return true;
}


} // namespace lmms
