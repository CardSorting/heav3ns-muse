/*
 * ProjectRenderer.cpp - ProjectRenderer-class for easily rendering projects
 *
 * Copyright (c) 2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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


#include <QFile>

#include "ProjectRenderer.h"
#include "Song.h"
#include "PerfLog.h"
#include "RemotePlugin.h"

#include "AudioFileWave.h"
#include "AudioFileOgg.h"
#include "AudioFileMP3.h"
#include "AudioFileFlac.h"


namespace lmms
{


const std::array<ProjectRenderer::FileEncodeDevice, 5> ProjectRenderer::fileEncodeDevices
{

	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Wave,
		QT_TRANSLATE_NOOP( "ProjectRenderer", "WAV (*.wav)" ),
					".wav", &AudioFileWave::getInst },
	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Flac,
		QT_TRANSLATE_NOOP("ProjectRenderer", "FLAC (*.flac)"),
		".flac",
		&AudioFileFlac::getInst
	},
	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Ogg,
		QT_TRANSLATE_NOOP( "ProjectRenderer", "OGG (*.ogg)" ),
					".ogg",
#ifdef LMMS_HAVE_OGGVORBIS
					&AudioFileOgg::getInst
#else
					nullptr
#endif
									},
	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::MP3,
		QT_TRANSLATE_NOOP( "ProjectRenderer", "MP3 (*.mp3)" ),
					".mp3",
#ifdef LMMS_HAVE_MP3LAME
					&AudioFileMP3::getInst
#else
					nullptr
#endif
									},
	// Insert your own file-encoder infos here.
	// Maybe one day the user can add own encoders inside the program.

	FileEncodeDevice{ ProjectRenderer::ExportFileFormat::Count, nullptr, nullptr, nullptr }

} ;

ProjectRenderer::ProjectRenderer(
	const OutputSettings& outputSettings, ExportFileFormat exportFileFormat, const QString& outputFilename)
	: QThread(Engine::audioEngine())
	, m_fileDev(nullptr)
	, m_progress(0)
	, m_abort(false)
	, m_remotePluginFailureCount(RemotePlugin::failureCount())
	, m_dspFailureCount(Engine::audioEngine()->dspStatus().failures)
{
	const auto formatIndex = static_cast<std::size_t>(exportFileFormat);
	if (formatIndex >= NumFileFormats) { return; }
	AudioFileDeviceInstantiaton audioEncoderFactory = fileEncodeDevices[formatIndex].m_getDevInst;

	if (audioEncoderFactory)
	{
		bool successful = false;

		m_fileDev = audioEncoderFactory(
					outputFilename, outputSettings, DEFAULT_CHANNELS,
					Engine::audioEngine(), successful );
		if( !successful )
		{
			delete m_fileDev;
			m_fileDev = nullptr;
		}
		else { m_unattachedFileDev.reset(m_fileDev); }
	}
}

ProjectRenderer::~ProjectRenderer()
{
	if (isRunning()) { abortProcessing(); }
	RemotePlugin::endRenderContext(m_remoteRenderContext.exchange(0));
}

bool ProjectRenderer::hasDspFailure() const
{
	const auto engine = Engine::audioEngine();
	if (!engine) { return true; }
	const auto state = engine->dspStatus();
	return m_dspFailed || state.failure != AudioEngine::DspFailure::None || state.failures != m_dspFailureCount;
}



// Little help function for getting file format from a file extension
// (only for registered file-encoders).
ProjectRenderer::ExportFileFormat ProjectRenderer::getFileFormatFromExtension(
							const QString & _ext )
{
	int idx = 0;
	while( fileEncodeDevices[idx].m_fileFormat != ExportFileFormat::Count )
	{
		if( QString( fileEncodeDevices[idx].m_extension ) == _ext )
		{
			return( fileEncodeDevices[idx].m_fileFormat );
		}
		++idx;
	}

	return( ExportFileFormat::Wave ); // Default.
}




QString ProjectRenderer::getFileExtensionFromFormat(
		ExportFileFormat fmt )
{
	const auto index = static_cast<std::size_t>(fmt);
	return index < NumFileFormats ? QString::fromLatin1(fileEncodeDevices[index].m_extension) : QString{};
}




bool ProjectRenderer::startProcessing()
{
	if (!isReady() || m_abort || m_started || isRunning()) { return false; }
	m_encodingFailed = m_encodingFailed || m_fileDev->hasWriteError();
	if (m_encodingFailed) { return false; }
	const auto dsp = Engine::audioEngine()->dspStatus();
	m_dspFailed = dsp.failure != AudioEngine::DspFailure::None || dsp.failures != m_dspFailureCount;
	if (m_dspFailed) { return false; }
	m_remoteRenderContext = RemotePlugin::beginRenderContext(m_executionTimeout);
	if (!m_remoteRenderContext) { return false; }
	struct SetupContext
	{
		std::atomic<std::uint64_t>& context;
		bool release = true;
		~SetupContext() { if (release) { RemotePlugin::endRenderContext(context.exchange(0)); } }
	} setup{m_remoteRenderContext};
	// Device setup has main-thread affinity and may wait for remote plugins.
	try { Engine::audioEngine()->setAudioDevice(m_fileDev, false); }
	catch (...)
	{
		// Signal handlers can fail after the engine has taken ownership.
		if (Engine::audioEngine()->audioDev() == m_fileDev) { m_unattachedFileDev.release(); }
		m_executionTimedOut = RemotePlugin::renderContextDeadlineExpired(m_remoteRenderContext);
		m_executionFailed = true; return false;
	}
	m_unattachedFileDev.release();
	m_encodingFailed = m_encodingFailed || m_fileDev->hasWriteError();
	if (RemotePlugin::renderContextCancelled(m_remoteRenderContext)) { m_abort = true; }
	// Capture the authoritative budget after cancellation observes it, before
	// failed setup retires the context or a Qt timer can lose the reporting race.
	m_executionTimedOut = RemotePlugin::renderContextDeadlineExpired(m_remoteRenderContext);
	m_remotePluginFailed = RemotePlugin::failureCount() != m_remotePluginFailureCount;
	if (m_abort || m_remotePluginFailed || m_encodingFailed) { return false; }
	m_started = true;

	start(
#ifndef LMMS_BUILD_WIN32
			QThread::HighPriority
#endif
						);
	setup.release = false;
	return true;
}


void ProjectRenderer::run()
{
	struct Execution
	{
		std::uint64_t context;
		~Execution() { RemotePlugin::finishRenderExecution(context); }
	} execution{m_remoteRenderContext.load()};
	const auto checkFailures = [&]
	{
		if (RemotePlugin::renderContextCancelled(m_remoteRenderContext)) { m_abort = true; }
		m_executionTimedOut = m_executionTimedOut || RemotePlugin::renderContextDeadlineExpired(m_remoteRenderContext);
		m_remotePluginFailed = m_remotePluginFailed || RemotePlugin::failureCount() != m_remotePluginFailureCount;
		const auto dsp = Engine::audioEngine()->dspStatus();
		m_dspFailed = m_dspFailed || dsp.failure != AudioEngine::DspFailure::None || dsp.failures != m_dspFailureCount;
		m_encodingFailed = m_encodingFailed || m_fileDev->hasWriteError();
	};
	checkFailures();
	if (m_abort || m_dspFailed || m_remotePluginFailed || m_encodingFailed) { return; }
	PerfLogTimer perfLog("Project Render");
	try
	{
		Engine::getSong()->startExport();
		Engine::audioEngine()->renderNextPeriod();
		checkFailures();
		m_progress = 0;
		if (!m_abort && !m_remotePluginFailed && !m_dspFailed && !m_encodingFailed) { Engine::audioEngine()->startProcessing(); }
		while (!Engine::getSong()->isExportDone() && !m_abort && !m_remotePluginFailed && !m_dspFailed && !m_encodingFailed)
		{
			checkFailures();
			if (m_abort || m_dspFailed || m_remotePluginFailed || m_encodingFailed) { break; }
			const auto buffer = Engine::audioEngine()->renderNextPeriod();
			checkFailures();
			if (m_abort || m_dspFailed || m_remotePluginFailed || m_encodingFailed) { break; }
			m_fileDev->writeBuffer(buffer.data(), buffer.size());
			checkFailures();
			if (m_abort || m_dspFailed || m_remotePluginFailed || m_encodingFailed) { break; }
			const int progress = Engine::getSong()->getExportProgress();
			if (m_progress != progress) { m_progress = progress; emit progressChanged(m_progress); }
		}
	}
	catch (...) { m_executionFailed = true; }
	// Cleanup remains required after a returned DSP/export exception. A cleanup
	// exception is also a failed export, never a successful encoded artifact.
	try { Engine::audioEngine()->stopProcessing(); } catch (...) { m_executionFailed = true; }
	try { Engine::getSong()->stopExport(); } catch (...) { m_executionFailed = true; }
	perfLog.end();
	if (m_abort) { QFile(m_fileDev->outputFile()).remove(); }
}




void ProjectRenderer::abortProcessing()
{
	m_abort = true;
	RemotePlugin::cancelRenderContext(m_remoteRenderContext);
	wait();
}

bool ProjectRenderer::finalizeOutput()
{
	if (isRunning() || !m_fileDev || m_abort || !m_started || m_executionFailed
		|| Engine::audioEngine()->audioDev() != m_fileDev) { return false; }
	const auto dsp = Engine::audioEngine()->dspStatus();
	m_dspFailed = m_dspFailed || dsp.failure != AudioEngine::DspFailure::None || dsp.failures != m_dspFailureCount;
	if (m_dspFailed) { return false; }
	if (RemotePlugin::renderContextCancelled(m_remoteRenderContext)) { m_abort = true; return false; }
	m_remotePluginFailed = m_remotePluginFailed || RemotePlugin::failureCount() != m_remotePluginFailureCount;
	if (m_remotePluginFailed) { return false; }
	const auto encoded = m_fileDev->finalizeOutput();
	m_encodingFailed = m_encodingFailed || !encoded || m_fileDev->hasWriteError();
	if (RemotePlugin::renderContextCancelled(m_remoteRenderContext)) { m_abort = true; }
	m_remotePluginFailed = RemotePlugin::failureCount() != m_remotePluginFailureCount;
	const auto completedDsp = Engine::audioEngine()->dspStatus();
	m_dspFailed = m_dspFailed || completedDsp.failure != AudioEngine::DspFailure::None || completedDsp.failures != m_dspFailureCount;
	return encoded && !m_abort && !m_remotePluginFailed && !m_dspFailed && !m_executionFailed && !m_encodingFailed;
}



void ProjectRenderer::updateConsoleProgress()
{
	constexpr int cols = 50;
	static int rot = 0;
	auto buf = std::array<char, 80>{};
	auto prog = std::array<char, cols + 1>{};

	for( int i = 0; i < cols; ++i )
	{
		prog[i] = ( i*100/cols <= m_progress ? '-' : ' ' );
	}
	prog[cols] = 0;

	const auto activity = "|/-\\";
	std::fill(buf.begin(), buf.end(), 0);
	std::snprintf(buf.data(), buf.size(), "\r|%s|    %3d%%   %c  ", prog.data(), m_progress.load(),
							activity[rot] );
	rot = ( rot+1 ) % 4;

	fprintf( stderr, "%s", buf.data() );
	fflush( stderr );
}


} // namespace lmms
