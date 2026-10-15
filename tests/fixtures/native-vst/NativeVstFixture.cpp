/* Deterministic native VST2 test plugin. SPDX-License-Identifier: GPL-2.0-or-later */
// Loaded by the actual RemoteVstPlugin executable. No Qt or LMMS engine is linked.
#include "aeffectx.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#include <utility>

#ifndef LMMS_VST_FIXTURE_KIND
#define LMMS_VST_FIXTURE_KIND 0
#endif

namespace
{
constexpr bool ChunkState = LMMS_VST_FIXTURE_KIND == 1;
constexpr int ProgramCount = LMMS_VST_FIXTURE_KIND >= 2 ? 0 : 700;
constexpr int ParameterCount = LMMS_VST_FIXTURE_KIND == 3 ? 65537 : 2;
struct Program
{
	std::array<char, 24> name{};
	std::vector<float> parameters = std::vector<float>(ParameterCount, 0.f);
};
void appendWord(std::vector<std::uint8_t>& bytes, std::uint32_t word)
{
	for (int shift = 0; shift < 32; shift += 8) { bytes.push_back(static_cast<std::uint8_t>(word >> shift)); }
}
std::uint32_t readWord(std::span<const std::uint8_t> bytes, std::size_t offset)
{
	return bytes[offset] | (std::uint32_t{bytes[offset + 1]} << 8)
		| (std::uint32_t{bytes[offset + 2]} << 16) | (std::uint32_t{bytes[offset + 3]} << 24);
}
void nameTo(void* destination, const char* text, std::size_t capacity)
{
	if (destination && capacity)
	{
		auto output = static_cast<char*>(destination);
		const auto length = std::min(std::strlen(text), capacity - 1);
		std::memcpy(output, text, length); output[length] = 0;
	}
}

class Fixture
{
public:
	AEffect effect{};
	std::mutex mutex;
	std::vector<Program> programs = std::vector<Program>(std::max(1, ProgramCount));
	std::vector<std::uint8_t> chunk;
	int current = 0;
	const VstEvents* pendingMidi = nullptr;
	int midiCount = 0;
	bool midiOrdered = true;

	Fixture()
	{
		for (int index = 0; index < static_cast<int>(programs.size()); ++index)
		{
			const auto name = "P" + std::to_string(index) + " | factory: gain";
			nameTo(programs[index].name.data(), name.c_str(), programs[index].name.size());
			programs[index].parameters[0] = .25f + .125f * (index % 5);
		}
		effect.magic = kEffectMagic; effect.dispatcher = dispatch;
		effect.process = process; effect.processReplacing = process;
		effect.setParameter = setParameter; effect.getParameter = getParameter;
		effect.numPrograms = ProgramCount; effect.numParams = ParameterCount; effect.numInputs = effect.numOutputs = 2;
		// An editor is advertised to verify that headless hosting does not open it.
		effect.flags = effFlagsCanReplacing | effFlagsHasEditor | (ChunkState ? (1 << 5) : 0);
		effect.uniqueID = 0x48565030 + LMMS_VST_FIXTURE_KIND; effect.user = this;
	}
	static Fixture& instance(AEffect* effect) { return *static_cast<Fixture*>(effect->user); }
	static void VST_CALL_CONV setParameter(AEffect* effect, int32_t index, float value)
	{
		auto& self = instance(effect); std::lock_guard guard(self.mutex);
		if (index >= 0 && index < ParameterCount && std::isfinite(value) && value >= 0.f && value <= 1.f)
		{ self.programs[self.current].parameters[index] = value; }
	}
	static float VST_CALL_CONV getParameter(AEffect* effect, int32_t index)
	{
		auto& self = instance(effect); std::lock_guard guard(self.mutex);
		return index >= 0 && index < ParameterCount ? self.programs[self.current].parameters[index] : 0.f;
	}
	static void VST_CALL_CONV process(AEffect* effect, float** input, float** output, int32_t frames)
	{
		auto& self = instance(effect); std::lock_guard guard(self.mutex);
		const float gain = self.programs[self.current].parameters[0];
		if (self.pendingMidi)
		{
			self.midiCount = self.pendingMidi->numEvents; self.midiOrdered = true; int previousFrame = -1;
			for (int index = 0; index < self.midiCount; ++index)
			{
				VstEvent* pointer = nullptr;
				std::memcpy(&pointer, reinterpret_cast<const std::byte*>(self.pendingMidi) + offsetof(VstEvents, events) + index * sizeof(pointer), sizeof(pointer));
				const auto event = reinterpret_cast<const VstMidiEvent*>(pointer);
				if (!event || event->type != kVstMidiType || event->deltaFrames < previousFrame || event->deltaFrames >= frames)
				{ self.midiOrdered = false; break; }
				previousFrame = event->deltaFrames;
			}
			self.pendingMidi = nullptr;
		}
		for (int channel = 0; channel < 2; ++channel)
		{
			for (int frame = 0; frame < frames; ++frame) { output[channel][frame] = input[channel][frame] * gain; }
		}
	}
	std::intptr_t getChunk(bool program, void* output)
	{
		if (!ChunkState || !output || programs[current].parameters[1] == 1.f) { return 0; }
		chunk = {'L', 'M', 'T', program ? std::uint8_t{'P'} : std::uint8_t{'B'}};
		appendWord(chunk, program ? 1 : static_cast<std::uint32_t>(programs.size()));
		appendWord(chunk, program ? 0 : current);
		const auto append = [&](const Program& item) {
			chunk.insert(chunk.end(), item.name.begin(), item.name.end());
			for (const auto value : item.parameters) { appendWord(chunk, std::bit_cast<std::uint32_t>(value)); }
		};
		if (program) { append(programs[current]); }
		else { for (const auto& item : programs) { append(item); } }
		*static_cast<void**>(output) = chunk.data(); return static_cast<std::intptr_t>(chunk.size());
	}
	std::intptr_t setChunk(bool program, std::intptr_t length, void* input)
	{
		if (!ChunkState || length < 12 || !input) { return 0; }
		const auto bytes = std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(input), static_cast<std::size_t>(length));
		const std::size_t count = program ? 1 : programs.size();
		if (bytes.size() != 12 + 32 * count || bytes[0] != 'L' || bytes[1] != 'M' || bytes[2] != 'T'
			|| bytes[3] != (program ? 'P' : 'B') || readWord(bytes, 4) != count || readWord(bytes, 8) >= count) { return 0; }
		std::vector<Program> staged(count);
		for (std::size_t index = 0; index < count; ++index)
		{
			std::memcpy(staged[index].name.data(), bytes.data() + 12 + index * 32, 24); staged[index].name.back() = 0;
			for (int parameter = 0; parameter < 2; ++parameter)
			{
				const float value = std::bit_cast<float>(readWord(bytes, 12 + index * 32 + 24 + parameter * 4));
				if (!std::isfinite(value) || value < 0.f || value > 1.f) { return 0; }
				staged[index].parameters[parameter] = value;
			}
		}
		if (program) { programs[current] = staged.front(); }
		else { programs = std::move(staged); current = static_cast<int>(readWord(bytes, 8)); }
		return 0; // Deliberately conventional: effSetChunk has no reliable success status.
	}
	static std::intptr_t VST_CALL_CONV dispatch(AEffect* effect, int32_t opcode, int32_t index, std::intptr_t value, void* pointer, float)
	{
		if (opcode == effClose) { delete &instance(effect); return 0; }
		if (opcode == effEditOpen) { std::_Exit(81); } // Opening an editor fails the headless test.
		auto& self = instance(effect); std::lock_guard guard(self.mutex);
		switch (opcode)
		{
			case effSetProgram: if (value >= 0 && value < ProgramCount) { self.current = static_cast<int>(value); } return 0;
			case effGetProgram: return self.current;
			case effSetProgramName: if (pointer) { nameTo(self.programs[self.current].name.data(), static_cast<const char*>(pointer), 24); } return 0;
			case effGetProgramName: nameTo(pointer, self.programs[self.current].name.data(), 24); return 1;
			case effGetProgramNameIndexed:
				if (index < 0 || index >= ProgramCount || index == 7) { return 0; }
				nameTo(pointer, self.programs[index].name.data(), 24); return 1;
			case effGetParamName:
				nameTo(pointer, index == 0 ? "Gain:x" : index == 1 ? "Fault:x" : ("P" + std::to_string(index)).c_str(), 8); return 1;
			case effGetParamLabel:
				if (index >= 2) { nameTo(pointer, "12345678", 9); return 1; }
				if (self.programs[self.current].parameters[0] == 0.f) { return 0; }
				nameTo(pointer, index == 0 ? "\xc2\xb5s" : "dB", 8); return 1;
			case effGetParamDisplay:
				if (index >= 2) { nameTo(pointer, "87654321", 9); return 1; }
				if (self.programs[self.current].parameters[0] == 0.f) { return 0; }
				nameTo(pointer, index == 0 ? "\xc2\xb5V" : (self.midiOrdered ? std::to_string(self.midiCount).c_str() : "bad"), 8); return 1;
			case effGetVendorString: nameTo(pointer, "LMMS native tests", 64); return 1;
			case effGetProductString: case effGetEffectName: nameTo(pointer, "Native VST fixture", 64); return 1;
			case effGetVendorVersion: return 9;
			case effGetVstVersion: return 2400;
			case effGetChunk: return self.getChunk(index != 0, pointer);
			case effSetChunk: return self.setChunk(index != 0, value, pointer);
			case effProcessEvents: self.pendingMidi = static_cast<const VstEvents*>(pointer); return 1;
			default: return 0;
		}
	}
};
}

#ifdef _WIN32
#define LMMS_FIXTURE_EXPORT __declspec(dllexport)
#else
#define LMMS_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" LMMS_FIXTURE_EXPORT AEffect* VST_CALL_CONV VSTPluginMain(audioMasterCallback host)
{
	if (!host || !host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.f)) { return nullptr; }
	return &(new Fixture())->effect;
}
