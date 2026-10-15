/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_WINMM_MIDI_API_H
#define LMMS_WINMM_MIDI_API_H
#include "lmmsconfig.h"
#ifdef LMMS_HAVE_WINMM
#include <windows.h>
#include <mmsystem.h>
namespace lmms::detail
{
// Private native call table; production uses the platform library directly.
struct WinMmMidiApi
{
	decltype(&::midiInGetNumDevs) inputCount = &::midiInGetNumDevs;
	decltype(&::midiInGetDevCapsW) inputCapabilities = &::midiInGetDevCapsW;
	decltype(&::midiInOpen) inputOpen = &::midiInOpen;
	decltype(&::midiInStart) inputStart = &::midiInStart;
	decltype(&::midiInReset) inputReset = &::midiInReset;
	decltype(&::midiInClose) inputClose = &::midiInClose;
	decltype(&::midiOutGetNumDevs) outputCount = &::midiOutGetNumDevs;
	decltype(&::midiOutGetDevCapsW) outputCapabilities = &::midiOutGetDevCapsW;
	decltype(&::midiOutOpen) outputOpen = &::midiOutOpen;
	decltype(&::midiOutShortMsg) outputShortMessage = &::midiOutShortMsg;
	decltype(&::midiOutReset) outputReset = &::midiOutReset;
	decltype(&::midiOutClose) outputClose = &::midiOutClose;
	bool operator==(const WinMmMidiApi&) const = default;
};
}
#endif
#endif
