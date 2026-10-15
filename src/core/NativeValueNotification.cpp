/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "NativeValueNotification.h"
#include <utility>
namespace lmms
{
thread_local NativeValueNotification* NativeValueNotification::s_current = nullptr;
NativeValueNotification::NativeValueNotification() noexcept : m_previous(s_current) { s_current = this; }
NativeValueNotification::~NativeValueNotification() { s_current = m_previous; }
bool NativeValueNotification::report(std::exception_ptr failure) noexcept
{
	if (!s_current || !failure) { return false; }
	if (!s_current->m_failure) { s_current->m_failure = std::move(failure); }
	return true;
}
void NativeValueNotification::rethrow() const
{
	if (m_failure) { std::rethrow_exception(m_failure); }
}
}
