/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_MIDI_IMPORT_DATA_H
#define LMMS_MIDI_IMPORT_DATA_H

#include <QByteArray>
class QFile;

namespace lmms
{
// Return the exact embedded SMF bytes; an unrelated format returns empty.
// Recognized malformed MIDI/RIFF and cancellation throw before model creation.
// PortSMF remains the musical event decoder.
QByteArray readMidiImportData(QFile& input);
}
#endif
