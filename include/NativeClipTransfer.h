/* Native selection serialization shared by headless callers and GUI adapters.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_CLIP_TRANSFER_H
#define LMMS_NATIVE_CLIP_TRANSFER_H
#include <QList>
#include <QString>
#include "TimePos.h"
#include "lmms_export.h"
namespace lmms
{
class Clip;
class DataFile;
class Track;
class LMMS_EXPORT NativeClipTransfer
{
public:
	struct Target { Track* track; TimePos position; Clip* replacement = nullptr; };
	static bool save(const QList<Clip*>& clips, Clip* anchor, DataFile& document, QString& error);
	// Targets follow the document's clip order. Clips attach while preparing
	// under native model ownership; caller results publish after all references
	// restore. Targets append unless an explicit pattern replacement is supplied.
	static bool load(const DataFile& document, const QList<Target>& targets, QList<Clip*>& clips, QString& error);
	// Direct native constructors retain resident content; only controls use XML.
	static bool copy(const QList<Clip*>& sources, const QList<Target>& targets, QList<Clip*>& clips, QString& error);
private:
	static bool capture(const QList<Clip*>& clips, Clip* anchor, DataFile& document, QString& error, bool controlsOnly);
	static bool restore(const DataFile& document, const QList<Target>& targets, QList<Clip*>& clips, QString& error, const QList<Clip*>* sources);
};
}
#endif
