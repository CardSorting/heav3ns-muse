/*
 * VstPlugin.h - declaration of VstPlugin class
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef _VST_PLUGIN_H
#define _VST_PLUGIN_H

#include <QMap>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "Knob.h"
#include "JournallingObject.h"
#include "RemotePlugin.h"
#include "Plugin.h"

#include "vstbase_export.h"

namespace lmms
{


class VSTBASE_EXPORT VstPlugin : public RemotePlugin, public JournallingObject
{
	Q_OBJECT
public:
	VstPlugin( const QString & _plugin );
	~VstPlugin() override;

	void tryLoad( const QString &remoteVstPluginExecutable );

	bool processMessage( const message & _m ) override;

	inline bool hasEditor() const
	{
		return m_pluginWindowID != 0;
	}

	/// Same as pluginWidget(), but can be overwritten in sub-classes to modify
	/// behavior the UI. This is used in VstInstrumentPlugin to wrap the VST UI
	/// in a QMdiSubWindow
	virtual QWidget* editor();

	inline const QString & name() const
	{
		return m_name;
	}

	inline int version() const
	{
		return m_version;
	}
	
	inline const QString & vendorString() const
	{
		return m_vendorString;
	}

	inline const QString & productString() const
	{
		return m_productString;
	}

	inline const QString& currentProgramName() const
	{
		return m_currentProgramName;
	}

	inline const QStringList& allProgramNames() const
	{
		return m_allProgramNames;
	}

	const std::vector<QString>& allParameterLabels() const
	{
		return m_allParameterLabels;
	}

	const std::vector<QString>& allParameterDisplays() const
	{
		return m_allParameterDisplays;
	}

	int currentProgram();
	Plugin::NativeProgramPage programPage(int offset, int limit);
	Plugin::NativePresetInfo inspectPresetFile(const QString& path);
	Plugin::NativePresetInfo loadPresetFile(const QString& path);
	Plugin::NativePresetInfo savePresetFile(const QString& path, bool bank);

	const QMap<QString, QString> & parameterDump();
	void setParameterDump( const QMap<QString, QString> & _pdump );


	QWidget * pluginWidget();

	void loadSettings( const QDomElement & _this ) override;
	void saveSettings( QDomDocument & _doc, QDomElement & _this ) override;

	QString nodeName() const override
	{
		return "vstplugin";
	}


	virtual void createUI(QWidget *parent);
	bool eventFilter(QObject *obj, QEvent *event) override;

	QString embedMethod() const;

public slots:
	void setTempo( lmms::bpm_t _bpm );
	void updateSampleRate();
	QString presetOpenPath();
	void setProgram( int index );
	void rotateProgram( int offset );
	void loadProgramNames();
	void loadParameterLabels();
	void loadParameterDisplays();
	void updateParameterLabel(int index);
	void updateParameterDisplay(int index);
	QString presetSavePath();
	void setParam( int i, float f );
	void idleUpdate();

	void showUI() override;
	void hideUI() override;
	void toggleUI() override;

	void handleClientEmbed();

private:
	void requestParameterText(int messageId, int index = -1);
	void presetFileRequest(const QString& path, const char* operation);
	Plugin::NativePresetInfo readPresetFile(const QString& path, bool apply);
	void loadChunk( const QByteArray & _chunk );
	QByteArray saveChunk();
	QByteArray saveNativePresetState();
	void loadNativePresetState(const QByteArray& bytes);

	void toggleEditorVisibility(int visible = -1);

	QString m_plugin;
	QPointer<QWidget> m_pluginWidget;
	int m_pluginWindowID;
	QSize m_pluginGeometry;
	const QString m_embedMethod;

	QString m_name;
	int m_version;
	QString m_vendorString;
	QString m_productString;
	QString m_currentProgramName;
	QStringList m_allProgramNames;
	std::vector<QString> m_allParameterLabels;
	std::vector<QString> m_allParameterDisplays;

	QString p_name;

	QMap<QString, QString> m_parameterDump;
	int m_parameterCount = -1;

	int m_currentProgram;

	QTimer m_idleTimer;

} ;

namespace gui {

class VSTBASE_EXPORT VstPluginKnob : public Knob
{
public:
	VstPluginKnob(VstPlugin* plugin, int paramIndex, const QString& name, QWidget* parent);
	~VstPluginKnob() override = default;

private:
	void timerEvent(QTimerEvent* event) override;

	auto currentValueToText() -> QString override;
	auto currentValueToTextUpdate() -> std::optional<QString> override;

	auto getParameterText() const -> QString;

	VstPlugin* m_plugin = nullptr;
	int m_paramIndex = 0;

	int m_rateLimitTimerId = 0;
	bool m_updateNow = false;
};

} // namespace gui
} // namespace lmms

#endif
