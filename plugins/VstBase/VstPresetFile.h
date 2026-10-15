/* Checked worker file I/O. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PRESET_FILE_H
#define LMMS_VST_PRESET_FILE_H

#include "VstPreset.h"
#include <array>
#include <cstdio>
#include <memory>

namespace lmms
{
// The caller opens UTF-8 paths with the platform's existing fopenUtf8 helper.
// These functions take ownership, including when reading/writing throws.
inline std::vector<std::uint8_t> readVstPresetFile(FILE* opened)
{
	std::unique_ptr<FILE, decltype(&std::fclose)> file(opened, &std::fclose);
	vst_preset_detail::require(file != nullptr, "Cannot open VST preset for reading");
	std::vector<std::uint8_t> bytes;
	std::array<std::uint8_t, 65536> block;
	for (;;)
	{
		const auto length = std::fread(block.data(), 1, block.size(), file.get());
		vst_preset_detail::require(length <= VstPresetMaxBytes - bytes.size(), "VST preset exceeds the file size limit");
		bytes.insert(bytes.end(), block.begin(), block.begin() + length);
		if (length < block.size())
		{
			vst_preset_detail::require(!std::ferror(file.get()) && std::feof(file.get()), "Cannot read the complete VST preset");
			break;
		}
	}
	vst_preset_detail::require(std::fclose(file.release()) == 0, "Cannot close VST preset after reading");
	return bytes;
}

inline void writeVstPresetFile(FILE* opened, std::span<const std::uint8_t> bytes)
{
	std::unique_ptr<FILE, decltype(&std::fclose)> file(opened, &std::fclose);
	vst_preset_detail::require(file != nullptr, "Cannot open VST preset for writing");
	vst_preset_detail::require(bytes.size() <= VstPresetMaxBytes, "VST preset exceeds the file size limit");
	vst_preset_detail::require(std::fwrite(bytes.data(), 1, bytes.size(), file.get()) == bytes.size(), "Cannot write the complete VST preset");
	vst_preset_detail::require(std::fclose(file.release()) == 0, "Cannot close VST preset after writing");
}
} // namespace lmms
#endif
