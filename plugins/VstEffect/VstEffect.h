/*
 * VstEffect.h - class for handling VST effect plugins
 *
 * Copyright (c) 2006-2009 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef _VST_EFFECT_H
#define _VST_EFFECT_H

#include <QMutex>
#include <QSharedPointer>

#include "Effect.h"
#include "VstEffectControls.h"

namespace lmms
{


class VstPlugin;


class VstEffect : public Effect
{
public:
	VstEffect( Model * _parent,
			const Descriptor::SubPluginFeatures::Key * _key );
	~VstEffect() override = default;

	ProcessStatus processImpl(SampleFrame* buf, const f_cnt_t frames) override;
	void refreshParameterModels() override { m_vstControls.refreshParameterModels(); }
	bool supportsNativePrograms() const override { return m_plugin != nullptr; }
	NativeProgramPage nativePrograms(int offset, int limit) override { return m_vstControls.nativePrograms(offset, limit); }
	bool selectNativeProgram(int index, QString& error) override { return m_vstControls.selectNativeProgram(index, error); }
	bool supportsNativePresets() const override { return m_plugin != nullptr; }
	NativePresetInfo inspectNativePreset(const QString& path) override { return m_vstControls.inspectNativePreset(path); }
	NativePresetInfo loadNativePreset(const QString& path) override { return m_vstControls.loadNativePreset(path); }
	NativePresetInfo saveNativePreset(const QString& path, bool bank) override { return m_vstControls.saveNativePreset(path, bank); }

	EffectControls * controls() override
	{
		return &m_vstControls;
	}


private:
	//! Returns true if plugin was loaded (m_plugin != nullptr)
	bool openPlugin(const QString& plugin);
	void closePlugin();

	QSharedPointer<VstPlugin> m_plugin;
	QMutex m_pluginMutex;
	EffectKey m_key;

	VstEffectControls m_vstControls;


	friend class VstEffectControls;
	friend class gui::VstEffectControlDialog;
	friend class gui::ManageVSTEffectView;

} ;


} // namespace lmms

#endif
