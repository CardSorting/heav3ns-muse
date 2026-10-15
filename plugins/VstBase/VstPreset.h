/* Checked native VST2 preset storage. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PRESET_H
#define LMMS_VST_PRESET_H

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lmms
{

// One bounded file is decoded in full before any native plugin state is changed.
inline constexpr std::size_t VstPresetMaxBytes = 256 * 1024 * 1024;
enum class VstPresetKind : std::uint32_t
{
	Parameters = 0x4678436b, ProgramChunk = 0x46504368,
	Bank = 0x4678426b, BankChunk = 0x46424368
};

struct VstPreset
{
	VstPresetKind kind = VstPresetKind::Parameters;
	std::uint32_t version = 1, pluginId = 0, pluginVersion = 0, programCount = 0;
	std::optional<int> currentProgram;
	std::string name;
	std::vector<float> parameters;
	std::vector<VstPreset> programs;
	std::vector<std::uint8_t> chunk;

	bool bank() const { return kind == VstPresetKind::Bank || kind == VstPresetKind::BankChunk; }
	bool opaque() const { return kind == VstPresetKind::ProgramChunk || kind == VstPresetKind::BankChunk; }
};

namespace vst_preset_detail
{
inline void require(bool condition, const char* error)
{
	if (!condition) { throw std::runtime_error(error); }
}

class Reader
{
public:
	explicit Reader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}
	std::size_t position() const { return m_position; }
	std::size_t remaining() const { return m_bytes.size() - m_position; }
	std::span<const std::uint8_t> take(std::size_t length)
	{
		require(length <= remaining(), "Truncated VST preset");
		const auto result = m_bytes.subspan(m_position, length);
		m_position += length;
		return result;
	}
	std::uint32_t word()
	{
		const auto bytes = take(4);
		return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16)
			| (std::uint32_t{bytes[2]} << 8) | bytes[3];
	}
	void account(std::size_t bytes)
	{
		require(bytes <= VstPresetMaxBytes - m_allocation, "Decoded VST preset exceeds its memory limit");
		m_allocation += bytes;
	}
	VstPreset record(bool nested = false)
	{
		account(sizeof(VstPreset));
		const auto start = position();
		require(word() == 0x43636e4b, "VST preset has no CcnK header");
		const auto declaredSize = word();
		require(declaredSize == 0 || declaredSize <= remaining(), "VST preset size exceeds available bytes");
		VstPreset result;
		result.kind = static_cast<VstPresetKind>(word());
		require(result.kind == VstPresetKind::Parameters || result.kind == VstPresetKind::ProgramChunk
			|| result.bank(), "Unsupported VST preset format");
		require(!nested || !result.bank(), "VST bank contains a nested bank");
		result.version = word();
		require(result.version == 1 || (result.bank() && result.version == 2), "Unsupported VST preset version");
		result.pluginId = word(); result.pluginVersion = word();
		const auto count = word();
		require(count <= std::uint32_t{std::numeric_limits<int>::max()}, "Invalid VST preset count");
		if (result.bank())
		{
			result.programCount = count;
			const auto current = word();
			take(124);
			if (result.version == 2)
			{
				require(current <= std::uint32_t{std::numeric_limits<int>::max()}, "Invalid VST bank current program");
				// Opaque writers can use a count of one for a whole native bank.
				// Its selected index is checked against the loaded plugin at application.
				require(result.opaque() || (count ? current < count : current == 0), "Invalid VST bank current program");
				if (count || current) { result.currentProgram = static_cast<int>(current); }
			}
		}
		else
		{
			const auto name = take(28);
			const auto end = std::find(name.begin(), name.end(), 0);
			result.name.assign(name.begin(), end);
			if (result.opaque()) { result.programCount = count; }
		}
		if (result.opaque())
		{
			const auto data = take(word());
			account(data.size());
			result.chunk.assign(data.begin(), data.end());
		}
		else if (result.bank())
		{
			require(count <= remaining() / 56, "VST bank program count exceeds available bytes");
			// Do not reserve from an untrusted count: even zero-parameter records have object overhead.
			for (std::uint32_t index = 0; index < count; ++index)
			{
				auto program = record(true);
				require(program.pluginId == result.pluginId, "VST bank contains a different plugin ID");
				result.programs.push_back(std::move(program));
			}
		}
		else
		{
			require(count <= remaining() / 4, "VST preset parameter count exceeds available bytes");
			account(static_cast<std::size_t>(count) * sizeof(float));
			result.parameters.reserve(count);
			for (std::uint32_t index = 0; index < count; ++index)
			{
				const auto value = std::bit_cast<float>(word());
				require(std::isfinite(value) && value >= 0.f && value <= 1.f, "Invalid normalized VST preset parameter");
				result.parameters.push_back(value);
			}
		}
		// Some established writers leave this field zero, including nested FXP records.
		require(declaredSize == 0 || declaredSize == position() - start - 8, "VST preset byte size does not match its contents");
		return result;
	}
private:
	std::span<const std::uint8_t> m_bytes;
	std::size_t m_position = 0;
	std::size_t m_allocation = 0;
};

inline std::size_t encodedSize(const VstPreset& preset, bool nested = false)
{
	require(preset.kind == VstPresetKind::Parameters || preset.kind == VstPresetKind::ProgramChunk
		|| preset.bank(), "Unsupported VST preset format");
	require(!nested || !preset.bank(), "VST bank contains a nested bank");
	require(preset.version == 1 || (preset.bank() && preset.version == 2), "Unsupported VST preset version");
	require(preset.programCount <= std::uint32_t{std::numeric_limits<int>::max()}, "Invalid VST preset count");
	require(preset.name.size() <= 28, "VST preset name exceeds 28 bytes");
	require(!preset.currentProgram || (preset.bank() && preset.version == 2 && *preset.currentProgram >= 0
		&& (preset.opaque() || static_cast<std::uint32_t>(*preset.currentProgram) < preset.programCount)), "Invalid VST bank current program");
	std::size_t size = preset.bank() ? 156 : 56;
	if (preset.opaque())
	{
		require(preset.parameters.empty() && preset.programs.empty(), "Conflicting VST chunk and parameter data");
		require(preset.chunk.size() <= VstPresetMaxBytes - size - 4, "VST preset exceeds the file size limit");
		size += 4 + preset.chunk.size();
	}
	else if (preset.bank())
	{
		require(preset.chunk.empty() && preset.parameters.empty() && preset.programs.size() == preset.programCount,
			"Invalid VST bank program data");
		for (const auto& program : preset.programs)
		{
			require(program.pluginId == preset.pluginId, "VST bank contains a different plugin ID");
			const auto length = encodedSize(program, true);
			require(length <= VstPresetMaxBytes - size, "VST preset exceeds the file size limit");
			size += length;
		}
	}
	else
	{
		require(preset.chunk.empty() && preset.programs.empty(), "Conflicting VST parameter and chunk data");
		require(preset.parameters.size() <= (VstPresetMaxBytes - size) / 4, "VST preset exceeds the file size limit");
		for (float value : preset.parameters)
		{ require(std::isfinite(value) && value >= 0.f && value <= 1.f, "Invalid normalized VST preset parameter"); }
		size += preset.parameters.size() * 4;
	}
	return size;
}

inline void appendWord(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (int shift = 24; shift >= 0; shift -= 8) { bytes.push_back(static_cast<std::uint8_t>(value >> shift)); }
}

inline void appendRecord(std::vector<std::uint8_t>& bytes, const VstPreset& preset)
{
	appendWord(bytes, 0x43636e4b);
	appendWord(bytes, static_cast<std::uint32_t>(encodedSize(preset) - 8));
	appendWord(bytes, static_cast<std::uint32_t>(preset.kind));
	appendWord(bytes, preset.version); appendWord(bytes, preset.pluginId); appendWord(bytes, preset.pluginVersion);
	appendWord(bytes, preset.kind == VstPresetKind::Parameters
		? static_cast<std::uint32_t>(preset.parameters.size()) : preset.programCount);
	if (preset.bank())
	{
		appendWord(bytes, static_cast<std::uint32_t>(preset.currentProgram.value_or(0)));
		bytes.insert(bytes.end(), 124, 0);
	}
	else
	{
		bytes.insert(bytes.end(), preset.name.begin(), preset.name.end());
		bytes.insert(bytes.end(), 28 - preset.name.size(), 0);
	}
	if (preset.opaque())
	{
		appendWord(bytes, static_cast<std::uint32_t>(preset.chunk.size()));
		bytes.insert(bytes.end(), preset.chunk.begin(), preset.chunk.end());
	}
	else if (preset.bank()) { for (const auto& program : preset.programs) { appendRecord(bytes, program); } }
	else { for (float value : preset.parameters) { appendWord(bytes, std::bit_cast<std::uint32_t>(value)); } }
}
} // namespace vst_preset_detail

inline VstPreset decodeVstPreset(std::span<const std::uint8_t> bytes)
{
	vst_preset_detail::require(bytes.size() <= VstPresetMaxBytes, "VST preset exceeds the file size limit");
	vst_preset_detail::Reader reader(bytes);
	auto result = reader.record();
	vst_preset_detail::require(reader.remaining() == 0, "Unexpected trailing VST preset data");
	return result;
}

inline std::vector<std::uint8_t> encodeVstPreset(const VstPreset& preset)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(vst_preset_detail::encodedSize(preset));
	vst_preset_detail::appendRecord(bytes, preset);
	return bytes;
}

} // namespace lmms
#endif
