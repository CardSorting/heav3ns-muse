/* Bounded native VST parameter transfers. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PARAMETER_TRANSFER_H
#define LMMS_VST_PARAMETER_TRANSFER_H

#include "VstParameterWire.h"
#include <algorithm>
#include <span>
#include <utility>

namespace lmms
{

// This bounds a message, not the number of parameters a plugin can expose.
inline constexpr int VstParameterPageSize = 256;
struct VstParameterRange { int total, offset, count; };

inline VstParameterRange vstParameterRange(int total, int offset, int limit)
{
	if (total < 0 || offset < 0 || offset > total || limit < 1 || limit > VstParameterPageSize)
	{ throw std::runtime_error("Invalid VST parameter page bounds"); }
	return {total, offset, std::min(limit, total - offset)};
}

template<class Field>
VstParameterRange validateVstParameterPage(int total, int offset, std::size_t fields,
	int fieldsPerEntry, const Field& field)
{
	if (fields == 2 && field(0) == "0")
	{ throw std::runtime_error("VST parameter page failed: " + field(1)); }
	const auto range = vstParameterRange(total, offset, VstParameterPageSize);
	if (fields != 4 + static_cast<std::size_t>(fieldsPerEntry) * range.count || field(0) != "1"
		|| vstParameterInteger(field(1)) != total || vstParameterInteger(field(2)) != offset
		|| vstParameterInteger(field(3)) != range.count)
	{ throw std::runtime_error("VST parameter page does not match the native layout"); }
	return range;
}

struct VstParameterEntry { int index; std::string name; float value; };
template<class Field>
std::vector<VstParameterEntry> readVstParameterPage(int total, int offset, std::size_t fields, const Field& field)
{
	const auto range = validateVstParameterPage(total, offset, fields, 3, field);
	std::vector<VstParameterEntry> entries;
	entries.reserve(range.count);
	for (int relative = 0; relative < range.count; ++relative)
	{
		const int position = 4 + 3 * relative;
		const int index = vstParameterInteger(field(position));
		auto name = field(position + 1);
		if (index != offset + relative || name.size() > 255)
		{ throw std::runtime_error("Malformed VST parameter page entry"); }
		entries.push_back({index, std::move(name), vstParameterValue(field(position + 2))});
	}
	return entries;
}

template<class Field>
std::vector<std::string> readVstParameterTextPage(int total, int offset, std::size_t fields, const Field& field)
{
	const auto range = validateVstParameterPage(total, offset, fields, 2, field);
	std::vector<std::string> entries;
	entries.reserve(range.count);
	for (int relative = 0; relative < range.count; ++relative)
	{
		const int position = 4 + 2 * relative;
		auto text = field(position + 1);
		if (vstParameterInteger(field(position)) != offset + relative || text.size() > 8)
		{ throw std::runtime_error("Malformed VST parameter text page entry"); }
		entries.push_back(std::move(text));
	}
	return entries;
}

class VstParameterRestore
{
public:
	void begin(int nativeCount, int declaredCount)
	{
		if (m_active || nativeCount < 0 || declaredCount != nativeCount)
		{ reject("VST parameter restore does not match the native layout or is already active"); }
		std::vector<float> staged;
		staged.reserve(nativeCount);
		m_values = std::move(staged); m_count = nativeCount; m_active = true;
	}

	void append(int nativeCount, int offset, std::span<const float> values)
	{
		if (!m_active || nativeCount != m_count || offset < 0 || static_cast<std::size_t>(offset) != m_values.size()
			|| values.empty() || values.size() > VstParameterPageSize
			|| values.size() > static_cast<std::size_t>(m_count) - m_values.size())
		{ reject("VST parameter restore page is incomplete, out of order or outside the native layout"); }
		for (const auto value : values)
		{
			if (!std::isfinite(value) || value < 0.f || value > 1.f)
			{ reject("Invalid normalized VST parameter restore value"); }
		}
		m_values.insert(m_values.end(), values.begin(), values.end());
	}

	template<class Writer>
	void commit(int nativeCount, const Writer& write)
	{
		if (!m_active || nativeCount != m_count || m_values.size() != static_cast<std::size_t>(m_count))
		{ reject("Cannot commit an incomplete VST parameter restore"); }
		auto values = std::move(m_values);
		abort(); // A throwing native setter must not leave a transaction available for replay.
		for (int index = 0; index < nativeCount; ++index) { write(index, values[index]); }
	}

	void abort() noexcept
	{
		m_active = false; m_count = 0;
		std::vector<float>().swap(m_values);
	}

private:
	[[noreturn]] void reject(const char* error) { abort(); throw std::runtime_error(error); }
	bool m_active = false;
	int m_count = 0;
	std::vector<float> m_values;
};

} // namespace lmms
#endif
