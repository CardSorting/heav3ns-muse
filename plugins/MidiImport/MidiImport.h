/*
 * MidiImport.h - support for importing MIDI files
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef _MIDI_IMPORT_H
#define _MIDI_IMPORT_H

#include <cstdint>
#include <QString>

#include "MidiEvent.h"
#include "ImportFilter.h"

namespace lmms
{

class MidiImportState;

class MidiImport : public ImportFilter
{
	Q_OBJECT
public:
	MidiImport(const QString& file);
	~MidiImport() override = default;
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }

private:
	bool tryImport(TrackContainer* tc) override;
	bool tryImport(TrackContainer* tc, int pattern) override;
	static bool readSMF(TrackContainer* tc, const QByteArray& bytes, int pattern, MidiImportState& state);
	friend class MidiImportTest;
};


} // namespace lmms

#endif
