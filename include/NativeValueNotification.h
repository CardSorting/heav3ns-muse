/* Receiver failure transport for synchronous native value notifications.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_NATIVE_VALUE_NOTIFICATION_H
#define LMMS_NATIVE_VALUE_NOTIFICATION_H
#include <exception>
#include "lmms_export.h"
namespace lmms
{
// Receivers catch locally and report before returning to Qt. The emitting
// native caller rethrows after dispatch. This stack scope has no QObject owner
// to access on unwind and applies only to the current synchronous thread.
class LMMS_EXPORT NativeValueNotification
{
public:
	NativeValueNotification() noexcept;
	~NativeValueNotification();
	NativeValueNotification(const NativeValueNotification&) = delete;
	NativeValueNotification& operator=(const NativeValueNotification&) = delete;
	static bool report(std::exception_ptr failure) noexcept;
	void rethrow() const;
private:
	static thread_local NativeValueNotification* s_current;
	NativeValueNotification* m_previous;
	std::exception_ptr m_failure;
};
}
#endif
