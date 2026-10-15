/* Native VST parameter ownership. SPDX-License-Identifier: GPL-2.0-or-later */
#include "VstParameterModels.h"

#include <QDomDocument>
#include <QScopedValueRollback>
#include <QThread>
#include <cmath>
#include <stdexcept>
#include <utility>
#include "AutomationClip.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "ControllerConnection.h"
#include "FileResourceIO.h"
#include "LocaleHelper.h"

namespace lmms
{

VstParameterModels::VstParameterModels(Model* owner, Writer writer) :
	Model(owner, tr("VST parameters")), m_writer(std::move(writer)) {}

std::vector<VstParameterModels::Parameter> VstParameterModels::decode(
	const QMap<QString, QString>& dump, bool allowPlainValues)
{
	FileResourceIO::EventSuspension events;
	std::vector<Parameter> result;
	result.reserve(dump.size());
	for (int index = 0; index < dump.size(); ++index)
	{
		QString error;
		if ((index % 64 == 0) && !FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		const auto entry = dump.constFind("param" + QString::number(index));
		if (entry == dump.cend()) { throw std::runtime_error("VST parameter indices must be contiguous"); }
		const auto& record = entry.value();
		const auto first = record.indexOf(':');
		const auto last = record.lastIndexOf(':');
		bool indexValid = false;
		const int nativeIndex = record.left(first).toInt(&indexValid);
		QString name;
		QString valueText;
		if (allowPlainValues && first < 0) { valueText = record; }
		else
		{
			if (first < 1 || last <= first || !indexValid || nativeIndex != index)
			{ throw std::runtime_error("Malformed VST parameter record"); }
			name = record.mid(first + 1, last - first - 1);
			valueText = record.mid(last + 1);
		}
		bool valid = false;
		const auto value = LocaleHelper::toFloat(valueText, &valid);
		if (!valid || !std::isfinite(value) || value < 0.f || value > 1.f)
		{ throw std::runtime_error("VST parameter values must be finite and within 0..1"); }
		result.push_back({index, name, value});
	}
	return result;
}

void VstParameterModels::checkOwner() const
{
	if (QThread::currentThread() != thread() || m_synchronizing)
	{ throw std::runtime_error("Cannot refresh VST parameters outside their owner thread or during synchronization"); }
}

void VstParameterModels::ensureModels(const std::vector<Parameter>& parameters)
{
	if (m_initialized && parameters.size() != m_models.size())
	{ throw std::runtime_error("VST parameter layout changed; reload the plugin before replacing automation targets"); }
	if (m_initialized) { return; }
	std::vector<std::unique_ptr<FloatModel>> staged;
	staged.reserve(parameters.size());
	for (const auto& parameter : parameters)
	{
		QString error;
		if ((parameter.index % 64 == 0) && !FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		auto model = std::make_unique<FloatModel>(parameter.value, 0.f, 1.f, 0.01f, this,
			QString::number(parameter.index) + ": " + parameter.name);
		const auto pointer = model.get();
		connect(pointer, &FloatModel::dataChanged, this, [this, pointer, index = parameter.index] {
			// Automation can deliver this on the audio thread. The writer uses
			// the native remote-plugin lock, not an editor or an event queue.
			if (!m_synchronizing) { m_writer(index, pointer->value()); }
		}, Qt::DirectConnection);
		staged.push_back(std::move(model));
	}
	m_models = std::move(staged);
	m_initialized = true;
}

FloatModel* VstParameterModels::at(int index) const
{
	if (index < 0 || index >= size()) { throw std::out_of_range("VST parameter index is outside the loaded plugin"); }
	return m_models[index].get();
}

void VstParameterModels::synchronize(const QMap<QString, QString>& dump)
{
	checkOwner();
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QScopedValueRollback<bool> syncing(m_synchronizing, true);
	const auto parameters = decode(dump); // Validate the complete reply before publishing any value.
	ensureModels(parameters);
	for (const auto& parameter : parameters)
	{
		auto model = at(parameter.index);
		model->setDisplayName(QString::number(parameter.index) + ": " + parameter.name);
		if (!model->isAutomatedOrControlled() && !AutomationClip::isConnected(model) && model->countLinks() == 0)
		{ model->setInitValue(parameter.value); }
	}
}

void VstParameterModels::restore(const QMap<QString, QString>& dump, const QDomElement& settings)
{
	checkOwner();
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QScopedValueRollback<bool> syncing(m_synchronizing, true);
	const auto parameters = decode(dump);
	// Validate model values too, before touching existing IDs/connections.
	for (const auto& parameter : parameters)
	{
		const auto key = "param" + QString::number(parameter.index);
		const auto node = settings.firstChildElement(key);
		const auto value = node.isNull() ? settings.attribute(key) : node.attribute("value");
		if (!node.isNull() || (!value.isEmpty() && !value.contains(':')))
		{
			bool valid = false;
			const auto number = LocaleHelper::toFloat(value, &valid);
			if (!valid || !std::isfinite(number) || number < 0.f || number > 1.f)
			{ throw std::runtime_error("Invalid saved VST automation value"); }
		}
	}
	ensureModels(parameters);
	// The legacy XML uses paramN both for VST dump attributes and LMMS
	// automation metadata. Never feed an index:name:value record to a float.
	QDomDocument copy;
	auto nativeSettings = copy.importNode(settings, true).toElement();
	copy.appendChild(nativeSettings);
	for (const auto& parameter : parameters)
	{
		const auto key = "param" + QString::number(parameter.index);
		if (nativeSettings.attribute(key).contains(':')) { nativeSettings.removeAttribute(key); }
		auto model = at(parameter.index);
		model->setInitValue(parameter.value);
		delete model->controllerConnection();
		model->loadSettings(nativeSettings, key);
	}
}

void VstParameterModels::saveSettings(QDomDocument& document, QDomElement& settings)
{
	checkOwner();
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QScopedValueRollback<bool> syncing(m_synchronizing, true);
	for (int index = 0; index < size(); ++index)
	{
		auto model = at(index);
		if (!model->isAutomatedOrControlled() && !AutomationClip::isConnected(model)
			&& model->scaleType() == AutomatableModel::ScaleType::Linear) { continue; }
		const auto key = "param" + QString::number(index);
		const bool hasDump = settings.hasAttribute(key);
		const auto dump = settings.attribute(key);
		model->saveSettings(document, settings, key);
		// A controller alone is serialized as a scalar attribute. Keep the
		// native parameter dump that shares this key intact for non-chunk VSTs.
		if (hasDump) { settings.setAttribute(key, dump); }
	}
}

void VstParameterModels::clear()
{
	checkOwner();
	const auto guard = Engine::audioEngine()->requestChangesGuard();
	QScopedValueRollback<bool> syncing(m_synchronizing, true);
	m_models.clear();
	m_initialized = false;
}

} // namespace lmms
