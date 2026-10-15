/* Native VST parameter ownership. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PARAMETER_MODELS_H
#define LMMS_VST_PARAMETER_MODELS_H

#include <QMap>
#include <functional>
#include <memory>
#include <vector>
#include "AutomatableModel.h"
#include "vstbase_export.h"

namespace lmms
{

// One instance per loaded plugin. Editors borrow these models; closing an
// editor must never remove automation targets or disconnect the native DSP.
class VSTBASE_EXPORT VstParameterModels : public Model
{
public:
	struct Parameter { int index; QString name; float value; };
	using Writer = std::function<void(int, float)>;
	explicit VstParameterModels(Model* owner, Writer writer);
	static std::vector<Parameter> decode(const QMap<QString, QString>& dump, bool allowPlainValues = false);
	void synchronize(const QMap<QString, QString>& dump);
	void restore(const QMap<QString, QString>& dump, const QDomElement& settings);
	void saveSettings(QDomDocument& document, QDomElement& settings);
	void clear();
	// Call before acquiring an adapter's mutex so synchronous callbacks fail
	// before they can reenter that non-recursive lock.
	void checkOwner() const;
	int size() const { return static_cast<int>(m_models.size()); }
	FloatModel* at(int index) const;

private:
	void ensureModels(const std::vector<Parameter>& parameters);
	Writer m_writer;
	std::vector<std::unique_ptr<FloatModel>> m_models;
	bool m_initialized = false;
	bool m_synchronizing = false;
};

} // namespace lmms
#endif
