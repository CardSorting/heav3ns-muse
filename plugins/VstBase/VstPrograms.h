/* Read-only native VST program queries. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PROGRAMS_H
#define LMMS_VST_PROGRAMS_H

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace lmms
{

struct VstProgramEntry { int index; std::string name; bool nameAvailable; };
struct VstProgramPage { int total; int current; int offset; std::vector<VstProgramEntry> programs; };

// Query callbacks must not select programs. Some VSTs cannot name unselected
// programs; exposing that absence preserves the user's unsaved sound state.
inline VstProgramPage readVstPrograms(int count, int current, int offset, int limit,
	const std::function<std::optional<std::string>(int)>& indexedName,
	const std::function<std::string()>& currentName)
{
	if (count < 0 || offset < 0 || limit < 1 || limit > 1024
		|| (count > 0 && (current < 0 || current >= count)))
	{ throw std::runtime_error("Invalid native VST program query or selection"); }
	VstProgramPage page{count, count ? current : -1, offset, {}};
	const int length = offset >= count ? 0 : std::min(limit, count - offset);
	page.programs.reserve(length);
	for (int relative = 0; relative < length; ++relative)
	{
		const int index = offset + relative;
		auto name = index == current ? std::optional<std::string>{currentName()} : indexedName(index);
		page.programs.push_back({index, name.value_or(std::string{}), name.has_value()});
	}
	return page;
}

inline int selectVstProgram(int count, int index, const std::function<void(int)>& select,
	const std::function<int()>& current)
{
	if (count <= 0 || index < 0 || index >= count) { throw std::out_of_range("VST program index is outside the loaded plugin"); }
	select(index);
	const int actual = current();
	if (actual != index) { throw std::runtime_error("VST plugin did not select the requested program"); }
	return actual;
}

} // namespace lmms
#endif
