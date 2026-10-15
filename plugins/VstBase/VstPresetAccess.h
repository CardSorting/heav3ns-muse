/* Native VST2 state callbacks. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PRESET_ACCESS_H
#define LMMS_VST_PRESET_ACCESS_H

#include "VstPreset.h"
#include "VstPrograms.h"
#include <exception>
#include <functional>

namespace lmms
{

struct VstPresetAccess
{
	std::uint32_t pluginId = 0, pluginVersion = 0;
	int parameterCount = 0, programCount = 0;
	bool usesChunks = false;
	std::function<int()> current;
	std::function<void(int)> select;
	std::function<std::string()> name;
	std::function<void(const std::string&)> setName;
	std::function<float(int)> parameter;
	std::function<void(int, float)> setParameter;
	std::function<std::vector<std::uint8_t>(bool)> chunk;
	std::function<void(bool, const std::vector<std::uint8_t>&)> setChunk;
};

inline void validateVstPresetTarget(const VstPreset& preset, const VstPresetAccess& target)
{
	using vst_preset_detail::require;
	// Also checks programmatic callers; no callback can precede complete validation.
	vst_preset_detail::encodedSize(preset);
	require(target.parameterCount >= 0 && target.programCount >= 0, "Invalid native VST layout");
	require(preset.pluginId == target.pluginId, "VST preset plugin ID does not match the loaded plugin");
	if (preset.opaque())
	{
		require(target.usesChunks, "The loaded VST does not accept opaque preset chunks");
		require(!preset.chunk.empty(), "The VST preset chunk is empty");
	}
	else if (preset.bank())
	{
		require(preset.programCount <= static_cast<std::uint32_t>(target.programCount), "VST bank has too many programs for the loaded plugin");
		for (const auto& program : preset.programs) { validateVstPresetTarget(program, target); }
	}
	else
	{
		require(preset.parameters.size() == static_cast<std::size_t>(target.parameterCount), "VST preset parameter count does not match the loaded plugin");
	}
	if (preset.currentProgram)
	{ require(*preset.currentProgram < target.programCount, "VST bank current program is outside the loaded plugin"); }
}

inline void applyVstPresetProgram(const VstPreset& preset, const VstPresetAccess& target)
{
	if (preset.opaque()) { target.setChunk(true, preset.chunk); }
	else
	{
		for (std::size_t index = 0; index < preset.parameters.size(); ++index)
		{ target.setParameter(static_cast<int>(index), preset.parameters[index]); }
	}
	target.setName(preset.name);
}

inline VstPreset captureVstPreset(const VstPresetAccess& target, bool bank);

inline void applyVstPreset(const VstPreset& preset, const VstPresetAccess& target)
{
	validateVstPresetTarget(preset, target);
	if (!preset.bank()) { applyVstPresetProgram(preset, target); return; }
	const int original = target.programCount ? target.current() : -1;
	vst_preset_detail::require(original == -1 ? !target.programCount : original >= 0 && original < target.programCount,
		"Invalid current native VST program");
	std::optional<VstPreset> untouched;
	if (!preset.opaque() && original >= 0 && static_cast<std::size_t>(original) >= preset.programs.size() && !preset.programs.empty())
	{ untouched = captureVstPreset(target, false); }
	try
	{
		if (preset.opaque()) { target.setChunk(false, preset.chunk); }
		else
		{
			for (std::size_t index = 0; index < preset.programs.size(); ++index)
			{
				selectVstProgram(target.programCount, static_cast<int>(index), target.select, target.current);
				applyVstPresetProgram(preset.programs[index], target);
			}
		}
		// Version 1 has no stored selection; preserve the original program index.
		const int finalProgram = preset.currentProgram.value_or(original);
		const bool switched = finalProgram >= 0 && target.current() != finalProgram;
		if (switched) { selectVstProgram(target.programCount, finalProgram, target.select, target.current); }
		// Some plugins reset their edit buffer even on reselecting the same index.
		if (!preset.opaque() && switched && static_cast<std::size_t>(finalProgram) < preset.programs.size())
		{ applyVstPresetProgram(preset.programs[finalProgram], target); }
		else if (switched && untouched && finalProgram == original) { applyVstPresetProgram(*untouched, target); }
	}
	catch (...)
	{
		const auto failure = std::current_exception();
		if (original >= 0)
		{
			try { selectVstProgram(target.programCount, original, target.select, target.current); }
			catch (...) { throw std::runtime_error("VST bank load failed and its program selection could not be restored"); }
		}
		std::rethrow_exception(failure);
	}
}

inline VstPreset captureVstProgram(const VstPresetAccess& target)
{
	VstPreset program;
	program.pluginId = target.pluginId; program.pluginVersion = target.pluginVersion;
	program.name = target.name();
	program.parameters.reserve(target.parameterCount);
	for (int index = 0; index < target.parameterCount; ++index) { program.parameters.push_back(target.parameter(index)); }
	vst_preset_detail::encodedSize(program);
	return program;
}

inline VstPreset captureVstPreset(const VstPresetAccess& target, bool bank)
{
	using vst_preset_detail::require;
	require(target.parameterCount >= 0 && target.programCount >= 0, "Invalid native VST layout");
	require(static_cast<std::size_t>(target.parameterCount) <= (VstPresetMaxBytes - 56) / 4,
		"Native VST preset exceeds the file size limit");
	VstPreset result;
	result.pluginId = target.pluginId; result.pluginVersion = target.pluginVersion;
	result.programCount = target.programCount;
	if (bank)
	{
		result.version = 2;
		if (target.programCount)
		{
			const int current = target.current();
			require(current >= 0 && current < target.programCount, "Invalid current native VST program");
			result.currentProgram = current;
		}
	}
	if (target.usesChunks)
	{
		result.kind = bank ? VstPresetKind::BankChunk : VstPresetKind::ProgramChunk;
		if (!bank) { result.name = target.name(); }
		result.chunk = target.chunk(!bank);
		require(!result.chunk.empty(), "The VST did not return a preset chunk");
	}
	else if (!bank) { result = captureVstProgram(target); }
	else
	{
		result.kind = VstPresetKind::Bank;
		const auto recordBytes = 56 + std::size_t{4} * target.parameterCount;
		require(static_cast<std::size_t>(target.programCount) <= (VstPresetMaxBytes - 156) / recordBytes,
			"Native VST bank exceeds the file size limit");
		require(static_cast<std::size_t>(target.programCount) <= VstPresetMaxBytes / (sizeof(VstPreset) + std::size_t{4} * target.parameterCount),
			"Native VST bank exceeds the decoded memory limit");
		if (target.programCount)
		{
			// A program switch may discard the current edit buffer. Keep both its name
			// and values, and put that unsaved state into the exported bank as well.
			const auto original = captureVstProgram(target);
			const int current = *result.currentProgram;
			const auto restore = [&] {
				selectVstProgram(target.programCount, current, target.select, target.current);
				applyVstPresetProgram(original, target);
			};
			try
			{
				for (int index = 0; index < target.programCount; ++index)
				{
					if (index == current) { result.programs.push_back(original); }
					else
					{
						selectVstProgram(target.programCount, index, target.select, target.current);
						result.programs.push_back(captureVstProgram(target));
					}
				}
			}
			catch (...)
			{
				const auto failure = std::current_exception();
				try { restore(); }
				catch (...) { throw std::runtime_error("VST bank export failed and the active program could not be restored"); }
				std::rethrow_exception(failure);
			}
			restore();
		}
	}
	vst_preset_detail::encodedSize(result);
	return result;
}

} // namespace lmms
#endif
