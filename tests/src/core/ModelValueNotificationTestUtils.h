/* Native value-notification fault boundary after Qt returns. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_TEST_MODEL_VALUE_NOTIFICATION_UTILS_H
#define LMMS_TEST_MODEL_VALUE_NOTIFICATION_UTILS_H
#include <QPointer>
#include <functional>
#include <utility>
#include "AutomatableModel.h"
namespace lmms
{
class ModelValueNotificationTestAccess
{
public:
	using Probe = AutomatableModel::ValueNotificationProbe;
	static const Probe* exchange(const Probe* probe) { return AutomatableModel::exchangeValueNotificationProbe(probe); }
};
// Install and reset only on the same thread in lexical scope order. The target
// is weak, and each native dispatch owns a copy of the installed callable.
class ModelValueNotificationProbe
{
public:
	ModelValueNotificationProbe(AutomatableModel* target, std::function<void()> callback, bool includeUnchanged = false)
		: m_probe([target = QPointer<AutomatableModel>(target), callback = std::move(callback), includeUnchanged](AutomatableModel* model, bool changed) {
			if (target && target.data() == model && (changed || includeUnchanged)) { callback(); }
		}), m_previous(ModelValueNotificationTestAccess::exchange(&m_probe)) { }
	~ModelValueNotificationProbe() { reset(); }
	ModelValueNotificationProbe(const ModelValueNotificationProbe&) = delete;
	ModelValueNotificationProbe& operator=(const ModelValueNotificationProbe&) = delete;
	void reset() { if (m_installed) { ModelValueNotificationTestAccess::exchange(m_previous); m_installed = false; } }
private:
	ModelValueNotificationTestAccess::Probe m_probe;
	const ModelValueNotificationTestAccess::Probe* m_previous;
	bool m_installed = true;
};
}
#endif
