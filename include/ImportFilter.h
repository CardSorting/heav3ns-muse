/*
 * ImportFilter.h - declaration of class ImportFilter, the base-class for all
 *                  file import filters
 *
 * Copyright (c) 2006-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_IMPORT_FILTER_H
#define LMMS_IMPORT_FILTER_H

#include <QFile>
#include <functional>
#include <memory>

#include "Plugin.h"

namespace lmms
{


class TrackContainer;


class LMMS_EXPORT ImportFilter : public Plugin
{
public:
	ImportFilter( const QString & _file_name,
					const Descriptor * _descriptor );
	~ImportFilter() override = default;


	// tries to import given file to given track-container by having all
	// available import-filters to try to import the file
	// PatternStore: -1 chooses its current column (0 when empty); an explicit
	// index chooses an existing column without changing the current selection.
	// Dispatch guards project transitions and checks returned filter stages.
	// Failure can leave partial native changes; callers own checkpoint recovery.
	static bool import( const QString & _file_to_import,
						TrackContainer* tc, int pattern = -1 );


protected:
	class LMMS_EXPORT WorkScope
	{
	public:
		WorkScope(ImportFilter* filter, TrackContainer* target, int pattern);
		~WorkScope();
		WorkScope(const WorkScope&) = delete;
		WorkScope& operator=(const WorkScope&) = delete;
		void check();
		void checkContext();
	private:
		struct State;
		std::unique_ptr<State> m_state;
	};
	virtual bool tryImport( TrackContainer* tc ) = 0;
	// Explicit PatternStore destinations require a filter that supports them.
	// -1 retains the ordinary song-container dispatch for existing filters.
	virtual bool tryImport(TrackContainer* tc, int pattern)
	{ return pattern == -1 && tryImport(tc); }
	QFile& file() { return m_file; }

	const QFile & file() const
	{
		return m_file;
	}

	bool openFile();
	static bool openFile(QFile& input);

	inline void closeFile()
	{
		m_file.close();
	}

	inline int readByte()
	{
		unsigned char c;
		if( m_file.getChar( (char*) &c ) )
		{
			return static_cast<int>( c );
		}
		return -1;
	}

	inline int readBlock( char * _data, int _len )
	{
		return m_file.read( _data, _len );
	}

	inline QByteArray readAllData()
	{
		m_file.seek(0);
		return m_file.readAll();
	}

	inline void ungetChar( char _ch )
	{
		m_file.ungetChar( _ch );
	}

	void saveSettings( QDomDocument &, QDomElement & ) override
	{
	}

	void loadSettings( const QDomElement & ) override
	{
	}

	QString nodeName() const override
	{
		return "import_filter";
	}


private:
	class Session;
	using Instantiate = std::function<Plugin*(const QString&, void*)>;
	static bool importWithFilters(const QString& path, TrackContainer* target, int pattern,
		QStringList filters, Instantiate instantiate);
	friend class NativeImportTestAccess;
	QFile m_file;
	bool m_working = false;

} ;


} // namespace lmms

#endif // LMMS_IMPORT_FILTER_H
