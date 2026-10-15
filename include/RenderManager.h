/*
 * RenderManager.h - Provides a uniform interface for rendering the project or
 *                   individual tracks for the CLI and GUI.
 *
 * Copyright (c) 2015 Ryan Roden-Corrent <ryan/at/rcorre.net>
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

#ifndef LMMS_RENDER_MANAGER_H
#define LMMS_RENDER_MANAGER_H

#include <memory>
#include <QPointer>

#include "ProjectRenderer.h"
#include "OutputSettings.h"


namespace lmms
{

class TrackRenderScope;
class NativeRenderOutput;


class RenderManager : public QObject
{
	Q_OBJECT
public:
	RenderManager(const OutputSettings& outputSettings, ProjectRenderer::ExportFileFormat fmt, QString outputPath);

	~RenderManager() override;

	/// Export all unmuted tracks into a single file
	void renderProject();

	/// Export all unmuted tracks into individual file
	void renderTracks();

	void abortProcessing();
	const QString& errorString() const { return m_error; }
	bool isStarting() const { return m_starting; }

protected:
	// Return a newly owned, unstarted native renderer for this output. Failures
	// are contained by the common CLI/GUI render path before further stems run.
	virtual std::shared_ptr<ProjectRenderer> createRenderer(const OutputSettings& settings,
		ProjectRenderer::ExportFileFormat format, const QString& outputPath);

signals:
	void progressChanged( int );
	void finished();

private slots:
	void renderNextTrack();
	void updateConsoleProgress();

private:
	QString pathForTrack( const Track *track, int num );
	void clearTrackSelection();

	void render( QString outputPath );
	bool prepareRender();

	const OutputSettings m_outputSettings;
	ProjectRenderer::ExportFileFormat m_format;
	QString m_outputPath;
	QString m_activeOutputPath;
	QString m_error;
	const QString m_rendererFailureMessage;
	bool m_deviceStored = false;
	bool m_starting = false;
	bool m_cancelled = false;
	bool m_finishing = false;
	bool m_retiring = false;
	bool m_outputFinalized = false;
	bool m_setupFailed = false;
	std::uint64_t m_deviceReservation = 0;

	std::shared_ptr<ProjectRenderer> m_activeRenderer;
	std::shared_ptr<NativeRenderOutput> m_outputTransaction;
	std::unique_ptr<TrackRenderScope> m_trackSelection;

	std::vector<QPointer<Track>> m_tracksToRender;
	std::vector<QPointer<Track>> m_exportTracks;
	friend class RenderManagerPreparationTestAccess;
	friend class RenderManagerLifecycleTestAccess;
	friend class RenderManagerConstructionTestAccess;
	friend class RenderManagerPublicationTestAccess;
} ;


} // namespace lmms

#endif // LMMS_RENDER_MANAGER_H
