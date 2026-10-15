/*
 * ProjectRenderer.h - ProjectRenderer class for easily rendering projects
 *
 * Copyright (c) 2008-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_PROJECT_RENDERER_H
#define LMMS_PROJECT_RENDERER_H

#include <atomic>
#include <cstdint>
#include <memory>

#include "AudioFileDevice.h"
#include "AudioEngine.h"
#include "OutputSettings.h"

#include "lmms_export.h"

namespace lmms
{


class LMMS_EXPORT ProjectRenderer : public QThread
{
	Q_OBJECT
public:
	enum class ExportFileFormat : int
	{
		Wave,
		Flac,
		Ogg,
		MP3,
		Count
	} ;
	constexpr static auto NumFileFormats = static_cast<std::size_t>(ExportFileFormat::Count);

	struct FileEncodeDevice
	{
		bool isAvailable() const { return m_getDevInst != nullptr; }

		ExportFileFormat m_fileFormat;
		const char * m_description;
		const char * m_extension;
		AudioFileDeviceInstantiaton m_getDevInst;
	} ;

	ProjectRenderer(const OutputSettings& _os, ExportFileFormat _file_format, const QString& _out_file);
	~ProjectRenderer() override;

	bool isReady() const
	{
		return m_fileDev != nullptr;
	}

	//! Complete codec output before the engine restores/deletes its file device.
	bool finalizeOutput();
	bool hasPluginFailure() const { return m_remotePluginFailed; }
	bool hasDspFailure() const;
	bool hasExecutionFailure() const { return m_executionFailed; }
	bool hasEncodingFailure() const { return m_encodingFailed; }
	bool wasAborted() const { return m_abort; }
	void setExecutionTimeout(int timeoutMs) { m_executionTimeout = timeoutMs > 0 ? timeoutMs : 0; }

	static ExportFileFormat getFileFormatFromExtension(
							const QString & _ext );

	static QString getFileExtensionFromFormat( ExportFileFormat fmt );

	static const std::array<FileEncodeDevice, 5> fileEncodeDevices;

public slots:
	bool startProcessing();
	void abortProcessing();

	void updateConsoleProgress();


signals:
	void progressChanged( int );


private:
	void run() override;

	AudioFileDevice * m_fileDev;
	// Keep ownership until the audio engine installs the device. An export
	// cancelled before setup must still close its codec and file descriptor.
	std::unique_ptr<AudioFileDevice> m_unattachedFileDev;

	std::atomic<int> m_progress;
	std::atomic<bool> m_abort;
	std::uint64_t m_remotePluginFailureCount;
	std::atomic_bool m_remotePluginFailed{false};
	std::uint64_t m_dspFailureCount;
	std::atomic_bool m_dspFailed{false};
	std::atomic_bool m_executionFailed{false};
	std::atomic_bool m_encodingFailed{false};
	std::atomic<std::uint64_t> m_remoteRenderContext{0};
	std::atomic_int m_executionTimeout{0};
	std::atomic_bool m_started{false};
	friend class ProjectRendererFailureTestAccess;

} ;


} // namespace lmms

#endif // LMMS_PROJECT_RENDERER_H
