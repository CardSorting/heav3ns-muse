/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_GIG_PCM_H
#define LMMS_GIG_PCM_H

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include "SampleFrame.h"

namespace lmms::gigPcm
{
// libgig returns native-order signed 16-bit words and little-endian 24-bit
// bytes. Retain GigPlayer's existing half-scale gain in both formats.
inline bool decode(std::span<const std::uint8_t> input, unsigned bitDepth,
	unsigned channels, float attenuation, std::span<SampleFrame> output)
{
	std::fill(output.begin(), output.end(), SampleFrame{});
	if ((bitDepth != 16 && bitDepth != 24) || (channels != 1 && channels != 2) || !std::isfinite(attenuation)) { return false; }
	const auto wordBytes = bitDepth / 8, frameBytes = wordBytes * channels;
	if (output.size() > input.size() / frameBytes) { return false; }
	const auto value = [bitDepth, attenuation](const std::uint8_t* bytes)
	{
		if (bitDepth == 16)
		{
			std::int16_t word; std::memcpy(&word, bytes, sizeof(word));
			return float(word) * (attenuation / 65536.f);
		}
		auto bits = std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16);
		if (bits & 0x800000u) { bits |= 0xff000000u; }
		return float(std::bit_cast<std::int32_t>(bits)) * (attenuation / 16777216.f);
	};
	for (std::size_t frame = 0; frame < output.size(); ++frame)
	{
		const auto bytes = input.data() + frame * frameBytes;
		output[frame][0] = value(bytes);
		output[frame][1] = channels == 1 ? output[frame][0] : value(bytes + wordBytes);
	}
	return true;
}
} // namespace lmms::gigPcm
#endif
