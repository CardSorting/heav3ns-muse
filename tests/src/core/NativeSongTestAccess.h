/* Native test setup without widening Song's mutation API. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_SONG_TEST_ACCESS_H
#define LMMS_NATIVE_SONG_TEST_ACCESS_H
#include "Song.h"
namespace lmms
{
class NativeSongTestAccess
{
public:
	static void setProjectFileName(Song* song, const QString& path) { song->setProjectFileName(path); }
	static void setModified(Song* song, bool value) { song->setModified(value); }
	static bool isCreating(const Song* song) { return song->m_creatingProject; }
	static bool permitsCreationLoad(const Song* song) { return song->m_permitCreationLoad; }
	static bool isImporting(const Song* song) { return song->m_importingProject; }
};
}
#endif
