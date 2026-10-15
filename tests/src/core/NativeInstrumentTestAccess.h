/* Raw instrument settings fixtures. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_INSTRUMENT_TEST_ACCESS_H
#define LMMS_NATIVE_INSTRUMENT_TEST_ACCESS_H
#include "Instrument.h"
namespace lmms
{
class NativeInstrumentTestAccess
{
public:
	static void save(Instrument* instrument, QDomDocument& document, QDomElement& element)
	{ instrument->saveSettings(document, element); }
	static void load(Instrument* instrument, const QDomElement& element)
	{ instrument->loadSettings(element); }
};
}
#endif
