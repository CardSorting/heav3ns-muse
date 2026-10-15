/* Native tuning text and file IO. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_TUNING_FILE_H
#define LMMS_TUNING_FILE_H

#include <memory>
#include <QString>
#include "lmms_export.h"

namespace lmms
{
class Scale;
class Keymap;

class LMMS_EXPORT TuningFile
{
public:
	static constexpr int MaximumEntries = 4096;
	static constexpr int MaximumTextBytes = 1024 * 1024;
	static bool parseScale(const QString& name, const QString& intervals, std::shared_ptr<Scale>& result, QString& error);
	static bool parseKeymap(const QString& name, const QString& mapping, int first, int last, int middle,
		int reference, double frequency, int octaveDegree, std::shared_ptr<Keymap>& result, QString& error, bool linearFromMiddle = false);
	static bool loadScale(const QString& path, std::shared_ptr<Scale>& result, QString& error);
	static bool loadKeymap(const QString& path, std::shared_ptr<Keymap>& result, QString& error);
	static bool saveScale(const QString& path, const Scale& scale, QString& error);
	static bool saveKeymap(const QString& path, const Keymap& keymap, QString& error);
	static bool validateScale(const Scale& scale, QString& error);
	static bool validateKeymap(const Keymap& keymap, QString& error);
};
}
#endif
