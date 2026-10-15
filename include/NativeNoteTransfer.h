/* Native selected-note transfer with no GUI dependency.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_NOTE_TRANSFER_H
#define LMMS_NATIVE_NOTE_TRANSFER_H
#include "Note.h"
#include <functional>
#include "lmms_export.h"
namespace lmms
{
class MidiClip;
class DataFile;
class LMMS_EXPORT NativeNoteTransfer
{
public:
	static bool save(MidiClip* source, const NoteVector& notes, TimePos origin, DataFile& document, QString& error);
	static bool load(const DataFile& document, MidiClip* target, TimePos position, NoteVector& inserted, QString& error);
	static bool copy(MidiClip* source, const NoteVector& notes, TimePos origin, MidiClip* target, TimePos position, NoteVector& inserted, QString& error);
	// One result per input: a new right fragment, or nullptr if no split occurred.
	static bool split(MidiClip* source, const NoteVector& notes, TimePos position, NoteVector& rightFragments, QString& error);
	static bool knife(MidiClip* source, const NoteVector& notes, TimePos from, int fromKey, TimePos to, int toKey, bool keepLongest,
		NoteVector& rightFragments, std::vector<int>& cutPositions, QString& error);
	static bool checkpoint(MidiClip* clip, QString& error);
private:
	friend class MidiClip;
	struct CutOptions { TimePos from, to; int fromKey = 0, toKey = 0; bool line = false, keepLongest = false; };
	static bool cut(MidiClip* source, const NoteVector& notes, const CutOptions& options, NoteVector& rightFragments, std::vector<int>& cutPositions, QString& error);
	static bool capture(MidiClip* source, const NoteVector& notes, TimePos origin, DataFile& document, QString& error, bool controlsOnly);
	static bool restore(const DataFile& document, MidiClip* target, TimePos position, NoteVector& inserted, QString& error, MidiClip* source, const NoteVector* sources, int stepCount = -1, const std::function<void()>& validate = {});
};
}
#endif
