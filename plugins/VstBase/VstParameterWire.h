/* Checked VST parameter transport. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PARAMETER_WIRE_H
#define LMMS_VST_PARAMETER_WIRE_H

#include <charconv>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lmms
{

inline int vstParameterInteger(std::string_view text)
{
	if (text.empty()) { throw std::runtime_error("Missing VST parameter integer"); }
	int value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
	{ throw std::runtime_error("Malformed VST parameter integer"); }
	return value;
}

inline float vstParameterValue(std::string_view text)
{
	if (text.empty()) { throw std::runtime_error("Missing VST parameter value"); }
	float value = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size()
		|| !std::isfinite(value) || value < 0.f || value > 1.f)
	{ throw std::runtime_error("Malformed normalized VST parameter value"); }
	return value;
}

inline std::string vstParameterText(float value)
{
	if (!std::isfinite(value) || value < 0.f || value > 1.f)
	{ throw std::runtime_error("Invalid normalized VST parameter value"); }
	char bytes[64];
	const auto result = std::to_chars(bytes, bytes + sizeof(bytes), value, std::chars_format::general,
		std::numeric_limits<float>::max_digits10);
	if (result.ec != std::errc{}) { throw std::runtime_error("Could not encode native VST parameter value"); }
	return {bytes, result.ptr};
}

inline std::vector<std::string> decodeVstParameterTextList(std::string_view packed, int count)
{
	// The legacy wire format prefixes each native string with its byte length,
	// not its Unicode character count. Split bytes before decoding text on the host.
	if (count < 0 || static_cast<std::size_t>(count) > packed.size())
	{ throw std::runtime_error("VST parameter text count does not match its payload"); }
	std::vector<std::string> result;
	result.reserve(count);
	std::size_t position = 0;
	for (int index = 0; index < count; ++index)
	{
		if (position == packed.size() || packed[position] < '0' || packed[position] > '8')
		{ throw std::runtime_error("Invalid VST parameter text length"); }
		const auto length = static_cast<std::size_t>(packed[position++] - '0');
		if (length > packed.size() - position)
		{ throw std::runtime_error("Truncated VST parameter text"); }
		result.emplace_back(packed.substr(position, length));
		position += length;
	}
	if (position != packed.size()) { throw std::runtime_error("Unexpected trailing VST parameter text"); }
	return result;
}

// Validate the whole message before invoking a native setter, including the
// exact field count. A missing/non-numeric value must never become zero via atof.
inline void applyVstParameterDump(int nativeCount, std::size_t fields,
	const std::function<std::string(int)>& field, const std::function<void(int, float)>& write)
{
	if (nativeCount < 0 || nativeCount > (std::numeric_limits<int>::max() - 1) / 3
		|| fields != 1 + std::size_t{3} * nativeCount || vstParameterInteger(field(0)) != nativeCount)
	{ throw std::runtime_error("VST parameter dump does not match the native layout"); }
	std::vector<float> values;
	values.reserve(nativeCount);
	for (int index = 0; index < nativeCount; ++index)
	{
		if (vstParameterInteger(field(1 + 3 * index)) != index)
		{ throw std::runtime_error("VST parameter dump is not in native index order"); }
		values.push_back(vstParameterValue(field(3 + 3 * index)));
	}
	for (int index = 0; index < nativeCount; ++index) { write(index, values[index]); }
}

} // namespace lmms
#endif
