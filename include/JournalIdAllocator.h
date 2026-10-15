/* Native journal identity allocation. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_JOURNAL_ID_ALLOCATOR_H
#define LMMS_JOURNAL_ID_ALLOCATOR_H

#include <cstdint>
#include <stdexcept>

namespace lmms
{

// The flag separates live and serialized aliases of the same native identity.
// Keep a cursor across allocations instead of restarting in a small random range.
template<class Reserved>
std::uint32_t nextJournalId(std::uint32_t& cursor, std::uint32_t flag, const Reserved& reserved)
{
	if (!flag || (flag & (flag - 1)) || cursor >= flag)
	{ throw std::invalid_argument("Invalid native journal ID namespace or allocation cursor"); }
	for (std::uint32_t attempted = 0; attempted < flag; ++attempted)
	{
		const auto saved = cursor;
		cursor = (cursor + 1) & (flag - 1);
		const auto live = saved | flag;
		if (!reserved(live) && !reserved(saved)) { return live; }
	}
	throw std::overflow_error("Native journal object ID namespace is exhausted");
}

} // namespace lmms
#endif
