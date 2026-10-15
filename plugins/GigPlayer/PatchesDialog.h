/*
 * PatchesDialog.h - display GIG patches (based on Sf2 patches_dialog.h)
 *
 * Copyright (c) 2008 Paul Giblock <drfaygo/at/gmail/dot/com>
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


#ifndef PATCHES_DIALOG_H
#define PATCHES_DIALOG_H

#include "ui_PatchesDialog.h"
#include "GigPlayer.h"
#include <QPointer>
#include <QVector>
#include <cstdint>
#include <memory>
#include <vector>

namespace lmms { class InstrumentTrack; class TrackContainer; class Song; class Mixer; class AudioEngine; }
namespace lmms::gui
{
class PatchesDialog : public QDialog, private Ui::PatchesDialog
{
	Q_OBJECT
public:
	PatchesDialog(QWidget* parent = nullptr, Qt::WindowFlags flags = {});
	~PatchesDialog() override = default;
	bool setup(GigInstrument* instrument, const QString& channelName);
public slots:
	void stabilizeForm();
	void bankChanged();
	void progChanged(QTreeWidgetItem* current, QTreeWidgetItem* previous);
protected slots:
	void accept() override;
	void reject() override;
private:
	class ViewGuard;
	bool actorsValid() const;
	bool contextValid(bool controls = true);
	bool fillPrograms(int bank, int preferredProgram);
	bool selectedProgram(int& bank, int& program) const;
	void invalidate();
	QPointer<QTreeWidget> m_banksView, m_programsView;
	QPointer<QPushButton> m_acceptButton;
	QPointer<GigInstrument> m_instrument;
	QPointer<InstrumentTrack> m_track;
	QPointer<TrackContainer> m_container;
	QPointer<Song> m_song;
	QPointer<Mixer> m_mixer;
	QPointer<AudioEngine> m_engine;
	QPointer<AutomatableModel> m_bankModel, m_programModel;
	std::shared_ptr<const void> m_identity;
	QString m_path, m_projectPath;
	std::vector<Instrument::Program> m_programs;
	QVector<QMetaObject::Connection> m_connections;
	std::uint64_t m_project = 0, m_routing = 0, m_epoch = 0;
	sample_rate_t m_rate = 0;
	float m_bank = 0, m_program = 0;
	bool m_invalid = true, m_closed = false, m_preparing = false, m_committing = false;
};
} // namespace lmms::gui
#endif
