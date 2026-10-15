/*
 * RemotePlugin.h - base class providing RPC like mechanisms
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

#ifndef LMMS_REMOTE_PLUGIN_H
#define LMMS_REMOTE_PLUGIN_H

#include <QThread>
#include <QProcess>
#include <QRecursiveMutex>
#include <cstdint>

#include "RemotePluginBase.h"
#include "SharedMemory.h"
#include "LmmsTypes.h"

namespace lmms
{

class MidiEvent;
class RemotePlugin;
class SampleFrame;

class ProcessWatcher : public QThread
{
	Q_OBJECT
public:
	ProcessWatcher( RemotePlugin * );
	~ProcessWatcher() override = default;

	void stop()
	{
		m_quit = true;
		quit();
	}

	void reset()
	{
		m_quit = false;
	}

private:
	void run() override;

	RemotePlugin * m_plugin;
	std::atomic_bool m_quit;
	friend class RemotePlugin;

} ;


class LMMS_EXPORT RemotePlugin : public QObject, public RemotePluginBase
{
	Q_OBJECT
public:
	RemotePlugin();
	~RemotePlugin() override;

	inline bool isRunning()
	{
#ifdef DEBUG_REMOTE_PLUGIN
		return true;
#else
		return m_running;
#endif // DEBUG_REMOTE_PLUGIN
	}

	bool init( const QString &pluginExecutable, bool waitForInitDoneMsg, QStringList extraArgs = {} );

	int sendMessage(const message& message) override;
	message waitForMessage(const message& message, bool busyWaiting = false) override;
	void fetchAndProcessAllMessages();
	static std::uint64_t failureCount();
	//! One native Song/AudioEngine export owns this process-wide context.
	static std::uint64_t beginRenderContext(int timeoutMs = 0);
	static void cancelRenderContext(std::uint64_t context);
	//! Keep the export reservation, but stop assigning new RPCs to its execution.
	static void finishRenderExecution(std::uint64_t context);
	static void endRenderContext(std::uint64_t context);
	static bool renderContextCancelled(std::uint64_t context);
	static bool hasActiveRenderContext();
	static bool hasRunningRenderExecution();

	inline void waitForHostInfoGotten()
	{
		waitForMessage(IdHostInfoGotten);
	}

	inline void waitForInitDone( bool _busyWaiting = true )
	{
		waitForMessage(IdInitDone, _busyWaiting);
	}

	bool processMessage( const message & _m ) override;

	bool process( const SampleFrame* _in_buf, SampleFrame* _out_buf );

	void processMidiEvent( const MidiEvent&, const f_cnt_t _offset );

	void updateSampleRate( sample_rate_t _sr )
	{
		lock();
		sendMessage( message( IdSampleRateInformation ).addInt( _sr ) );
		waitForMessage( IdInformationUpdated, true );
		unlock();
	}


	virtual void toggleUI()
	{
		lock();
		sendMessage( IdToggleUI );
		unlock();
	}

	int isUIVisible()
	{
		lock();
		sendMessage( IdIsUIVisible );
		message m = waitForMessage( IdIsUIVisible );
		unlock();
		return m.id != IdIsUIVisible ? -1 : m.getInt() ? 1 : 0;
	}

	inline bool failed() const
	{
		return m_failed;
	}

	inline void lock()
	{
		m_commMutex.lock();
	}

	inline void unlock()
	{
		m_commMutex.unlock();
	}

public slots:
	virtual void showUI();
	virtual void hideUI();

protected:
	inline void setSplittedChannels( bool _on )
	{
		m_splitChannels = _on;
	}


	std::atomic_bool m_failed;
private:
	void resizeSharedProcessingMemory();
	void recordFailure();
	void retireProcess();
	bool requestCancelled();
	class RequestDeadline;
	std::atomic<std::int64_t> m_requestDeadline{0};
	std::atomic<std::uint64_t> m_requestRenderContext{0};
	std::atomic_bool m_running{false};
	int m_timeoutMs{120000};


	QProcess m_process;
	ProcessWatcher m_watcher;

	QString m_exec;
	QStringList m_args;

	QRecursiveMutex m_commMutex;
	bool m_splitChannels;

	SharedMemory<float[]> m_audioBuffer;
	std::size_t m_audioBufferSize;

	int m_inputCount;
	int m_outputCount;

#ifndef SYNC_WITH_SHM_FIFO
	int m_server;
	QString m_socketFile;
#endif // not SYNC_WITH_SHM_FIFO

	friend class ProcessWatcher;


private slots:
	void processFinished( int exitCode, QProcess::ExitStatus exitStatus );
	void processErrored(QProcess::ProcessError err );
} ;

inline std::string QSTR_TO_STDSTR(QString const& qstr)
{
	return qstr.toStdString();
}

} // namespace lmms

#endif // LMMS_REMOTE_PLUGIN_H
