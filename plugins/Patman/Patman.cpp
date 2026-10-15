/*
 * Patman.cpp - a GUS-compatible patch instrument plugin
 *
 * Copyright (c) 2007-2008 Javier Serrano Polo <jasp00/at/users.sourceforge.net>
 * Copyright (c) 2009-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "Patman.h"

#include <algorithm>
#include <limits>
#include <new>
#include <QFile>
#include <QFileInfo>
#include <QDragEnterEvent>
#include <QPainter>
#include <QDomElement>
#include <QtEndian>

#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "Engine.h"
#include "FileDialog.h"
#include "FileResourceSnapshot.h"
#include "FontHelper.h"
#include "InstrumentTrack.h"
#include "NotePlayHandle.h"
#include "PathUtil.h"
#include "PixmapButton.h"
#include "Song.h"
#include "StringPairDrag.h"
#include "Clipboard.h"

#include "embed.h"

#include "plugin_export.h"

namespace lmms
{


extern "C"
{

Plugin::Descriptor PLUGIN_EXPORT patman_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"PatMan",
	QT_TRANSLATE_NOOP( "PluginBrowser",
				"GUS-compatible patch instrument" ),
	"Javier Serrano Polo <jasp00/at/users.sourceforge.net>",
	0x0100,
	Plugin::Type::Instrument,
	new PluginPixmapLoader( "logo" ),
	"pat",
	nullptr,
} ;


// necessary for getting instance out of shared lib
PLUGIN_EXPORT Plugin * lmms_plugin_main( Model *m, void * )
{
	return new PatmanInstrument( static_cast<InstrumentTrack *>( m ) );
}

}




PatmanInstrument::PatmanInstrument( InstrumentTrack * _instrument_track ) :
	Instrument( _instrument_track, &patman_plugin_descriptor ),
	m_loopedModel(true, this, tr("Loop sample")),
	m_tunedModel(true, this, tr("Tune sample"))
{
}




PatmanInstrument::~PatmanInstrument()
{
	unloadCurrentPatch();
}




void PatmanInstrument::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	_this.setAttribute( "src", m_patchFile );
	if (m_patchSnapshot) { DataFile::registerResourceSnapshot(_doc, _this, "src", m_patchSnapshot); }
	m_loopedModel.saveSettings( _doc, _this, "looped" );
	m_tunedModel.saveSettings( _doc, _this, "tuned" );
}




void PatmanInstrument::loadSettings( const QDomElement & _this )
{
	{
		DataFile::ResourceReferenceScope resource(_this, "src");
		setFile( _this.attribute( "src" ), false );
	}
	m_loopedModel.loadSettings( _this, "looped" );
	m_tunedModel.loadSettings( _this, "tuned" );
}




void PatmanInstrument::loadFile( const QString & _file )
{
	setFile( _file );
}




QString PatmanInstrument::nodeName() const
{
	return( patman_plugin_descriptor.name );
}




void PatmanInstrument::playNote( NotePlayHandle * _n,
						SampleFrame* _working_buffer )
{
	if( m_patchFile == "" )
	{
		return;
	}

	const f_cnt_t frames = _n->framesLeftForCurrentPeriod();
	const f_cnt_t offset = _n->noteOffset();

	if (!_n->m_pluginData)
	{
		selectSample( _n );
	}
	auto hdata = (handle_data*)_n->m_pluginData;

	float play_freq = hdata->tuned ? _n->frequency() :
						hdata->sample->frequency();

	if (hdata->sample->play(_working_buffer + offset, hdata->state, frames,
			m_loopedModel.value() ? Sample::Loop::On : Sample::Loop::Off, DefaultBaseFreq / play_freq))
	{
		applyRelease( _working_buffer, _n );
	}
	else
	{
		zeroSampleFrames(_working_buffer, frames + offset);
	}
}




void PatmanInstrument::deleteNotePluginData( NotePlayHandle * _n )
{
	auto hdata = (handle_data*)_n->m_pluginData;
	delete hdata->state;
	delete hdata;
}




std::vector<Plugin::FileResource> PatmanInstrument::fileResources() const
{
	return {{"patch", tr("GUS patch"), m_patchFile, {"pat"}}};
}

bool PatmanInstrument::setFileResource(const QString& key, const QString& path, QString& error)
{
	if (key != "patch") { error = tr("Unknown Patman resource"); return false; }
	QVector<std::shared_ptr<Sample>> candidate;
	std::shared_ptr<FileResourceSnapshot> snapshot;
	QString relativePath;
	if (!path.isEmpty())
	{
		bool invalid = false;
		const auto absolute = PathUtil::toAbsolute(path, &invalid);
		const QFileInfo file(FileResourceSnapshot::readableSourcePath(absolute));
		if (invalid || !file.isFile() || !file.isReadable()) { error = tr("Patch path is not a readable file: %1").arg(path); return false; }
		snapshot = FileResourceSnapshot::capture(absolute, error);
		if (!snapshot || !loadPatch(snapshot->path(), candidate, error)) { return false; }
		relativePath = PathUtil::toShortestRelative(absolute);
	}
	{
		auto guard = Engine::audioEngine()->requestChangesGuard();
		m_patchSamples.swap(candidate);
		m_patchSnapshot.swap(snapshot);
		m_patchFile = relativePath;
	}
	emit fileChanged();
	error.clear(); return true;
}

void PatmanInstrument::setFile(const QString& path, bool rename)
{
	const bool renameTrack = rename && !path.isEmpty()
		&& (m_patchFile.isEmpty() || instrumentTrack()->name() == QFileInfo(m_patchFile).fileName());
	QString error;
	if (!setFileResource("patch", path, error)) { collectErrorForUI(error); return; }
	if (renameTrack) { instrumentTrack()->setName(PathUtil::cleanName(path)); }
}

bool PatmanInstrument::loadPatch(const QString& filename, QVector<std::shared_ptr<Sample>>& samples, QString& error)
{
	// QFile opens native Unicode paths on every supported platform and owns its handle.
	QFile file(filename);
	if (!file.open(QIODevice::ReadOnly)) { error = tr("Could not open GUS patch: %1").arg(file.errorString()); return false; }
	try
	{
		const auto header = file.read(239);
		if (header.size() != 239 || (!header.startsWith(QByteArray("GF1PATCH110\0ID#000002", 22))
			&& !header.startsWith(QByteArray("GF1PATCH100\0ID#000002", 22))))
		{
			error = tr("File is not a complete native GUS patch"); return false;
		}
		const auto instrumentCount = static_cast<unsigned char>(header[82]);
		const auto layerCount = static_cast<unsigned char>(header[151]);
		if (instrumentCount > 1) { error = tr("Patman does not support multi-instrument GUS patches"); return false; }
		if (layerCount > 1) { error = tr("Patman does not support multi-layer GUS patches"); return false; }
		const auto sampleCount = static_cast<unsigned char>(header[198]);
		if (sampleCount == 0) { error = tr("GUS patch contains no samples"); return false; }
		QVector<std::shared_ptr<Sample>> decoded;
		decoded.reserve(sampleCount);
		for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
		{
			const auto waveHeader = file.read(96);
			if (waveHeader.size() != 96) { error = tr("Truncated GUS sample header %1").arg(sampleIndex); return false; }
			const auto dataLength = qFromLittleEndian<quint32>(waveHeader.constData() + 8);
			const auto loopStart = qFromLittleEndian<quint32>(waveHeader.constData() + 12);
			const auto loopEnd = qFromLittleEndian<quint32>(waveHeader.constData() + 16);
			const auto sampleRate = qFromLittleEndian<quint16>(waveHeader.constData() + 20);
			const auto rootFrequency = qFromLittleEndian<quint32>(waveHeader.constData() + 30);
			const auto modes = static_cast<unsigned char>(waveHeader[55]);
			const quint32 sampleBytes = (modes & MODES_16BIT) ? 2 : 1;
			const quint32 frames = dataLength / sampleBytes;
			// Validate native frame coordinates and remaining bytes before any PCM allocation.
			if (dataLength == 0 || dataLength % sampleBytes != 0 || static_cast<qint64>(dataLength) > file.bytesAvailable()
				|| frames > static_cast<quint32>(std::numeric_limits<int>::max()) || frames > std::vector<SampleFrame>().max_size()
				|| sampleRate == 0 || rootFrequency == 0)
			{
				error = tr("Invalid or truncated GUS sample metadata %1").arg(sampleIndex); return false;
			}
			const auto loopStartFrame = loopStart / sampleBytes, loopEndFrame = loopEnd / sampleBytes;
			if ((modes & MODES_LOOPING) && (loopStartFrame >= loopEndFrame || loopEnd > dataLength))
			{
				error = tr("GUS sample %1 has invalid loop bounds").arg(sampleIndex); return false;
			}
			std::vector<SampleFrame> data(frames);
			quint32 written = 0;
			while (written < frames)
			{
				const auto count = std::min<quint32>(8192, frames - written);
				const auto block = file.read(static_cast<qint64>(count) * sampleBytes);
				if (block.size() != static_cast<qint64>(count) * sampleBytes)
				{
					error = tr("Incomplete GUS sample data %1").arg(sampleIndex); return false;
				}
				for (quint32 index = 0; index < count; ++index)
				{
					int value;
					float denominator;
					if (sampleBytes == 2)
					{
						const int bits = qFromLittleEndian<quint16>(block.constData() + index * 2);
						value = (modes & MODES_UNSIGNED) ? bits - 32768 : (bits >= 32768 ? bits - 65536 : bits);
						denominator = 32767.f;
					}
					else
					{
						const int bits = static_cast<unsigned char>(block[index]);
						value = (modes & MODES_UNSIGNED) ? bits - 128 : (bits >= 128 ? bits - 256 : bits);
						denominator = 127.f;
					}
					data[written + index] = SampleFrame(value / denominator);
				}
				written += count;
			}
			auto buffer = std::make_shared<const SampleBuffer>(std::move(data), sampleRate);
			auto sample = std::make_shared<Sample>(std::move(buffer));
			sample->setFrequency(rootFrequency / 1000.f);
			if (modes & MODES_LOOPING)
			{
				sample->setLoopStartFrame(static_cast<int>(loopStartFrame));
				sample->setLoopEndFrame(static_cast<int>(loopEndFrame));
			}
			decoded.push_back(std::move(sample));
		}
		samples.swap(decoded);
	}
	catch (const std::bad_alloc&) { error = tr("Insufficient memory to decode the native GUS patch"); return false; }
	error.clear(); return true;
}


void PatmanInstrument::unloadCurrentPatch()
{
	while( !m_patchSamples.empty() )
	{
		m_patchSamples.pop_back();
	}
	m_patchSnapshot.reset();
}




void PatmanInstrument::selectSample( NotePlayHandle * _n )
{
	const float freq = _n->frequency();

	float min_dist = HUGE_VALF;
	std::shared_ptr<Sample> sample = nullptr;

	for (const auto& patchSample : m_patchSamples)
	{
		float patch_freq = patchSample->frequency();
		float dist = freq >= patch_freq ? freq / patch_freq :
							patch_freq / freq;

		if( dist < min_dist )
		{
			min_dist = dist;
			sample = patchSample;
		}
	}

	auto hdata = new handle_data;
	hdata->tuned = m_tunedModel.value();
	hdata->sample = sample ? sample : std::make_shared<Sample>();
	hdata->state = new Sample::PlaybackState(AudioResampler::Mode::Linear);

	_n->m_pluginData = hdata;
}




gui::PluginView * PatmanInstrument::instantiateView( QWidget * _parent )
{
	return( new gui::PatmanView( this, _parent ) );
}






namespace gui
{


PatmanView::PatmanView( Instrument * _instrument, QWidget * _parent ) :
	InstrumentViewFixedSize( _instrument, _parent ),
	m_pi(castModel<PatmanInstrument>())
{
	setAutoFillBackground( true );
	QPalette pal;
	pal.setBrush( backgroundRole(),
				PLUGIN_NAME::getIconPixmap( "artwork" ) );
	setPalette( pal );


	m_openFileButton = new PixmapButton( this, nullptr );
	m_openFileButton->setObjectName( "openFileButton" );
	m_openFileButton->setCursor(Qt::PointingHandCursor);
	m_openFileButton->move( 227, 86 );
	m_openFileButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
							"select_file_on" ) );
	m_openFileButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
							"select_file" ) );
	connect( m_openFileButton, SIGNAL( clicked() ),
				this, SLOT( openFile() ) );
	m_openFileButton->setToolTip(tr("Open patch"));

	m_loopButton = new PixmapButton( this, tr( "Loop" ) );
	m_loopButton->setObjectName("loopButton");
	m_loopButton->setCheckable( true );
	m_loopButton->move( 195, 138 );
	m_loopButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"loop_on" ) );
	m_loopButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
								"loop_off" ) );
	m_loopButton->setToolTip(tr("Loop mode"));

	m_tuneButton = new PixmapButton( this, tr( "Tune" ) );
	m_tuneButton->setObjectName("tuneButton");
	m_tuneButton->setCheckable( true );
	m_tuneButton->move( 223, 138 );
	m_tuneButton->setActiveGraphic( PLUGIN_NAME::getIconPixmap(
								"tune_on" ) );
	m_tuneButton->setInactiveGraphic( PLUGIN_NAME::getIconPixmap(
								"tune_off" ) );
	m_tuneButton->setToolTip(tr("Tune mode"));


	if (m_pi->m_patchFile.isEmpty())
	{
		m_displayFilename = tr("No file selected");
	}
	else
	{
		updateFilename();
	}

	setAcceptDrops( true );
}




void PatmanView::openFile()
{
	FileDialog ofd( nullptr, tr( "Open patch file" ) );
	ofd.setFileMode( FileDialog::ExistingFiles );

	QStringList types;
	types << tr( "Patch-Files (*.pat)" );
	ofd.setNameFilters( types );

	if( m_pi->m_patchFile == "" )
	{
		if( QDir( "/usr/share/midi/freepats" ).exists() )
		{
			ofd.setDirectory( "/usr/share/midi/freepats" );
		}
		else
		{
			ofd.setDirectory(
				ConfigManager::inst()->userSamplesDir() );
		}
	}
	else if( QFileInfo( m_pi->m_patchFile ).isRelative() )
	{
		QString f = ConfigManager::inst()->userSamplesDir()
							+ m_pi->m_patchFile;
		if( QFileInfo( f ).exists() == false )
		{
			f = ConfigManager::inst()->factorySamplesDir()
							+ m_pi->m_patchFile;
		}

		ofd.selectFile( f );
	}
	else
	{
		ofd.selectFile( m_pi->m_patchFile );
	}

	if( ofd.exec() == QDialog::Accepted && !ofd.selectedFiles().isEmpty() )
	{
		QString f = ofd.selectedFiles()[0];
		if( f != "" )
		{
			m_pi->setFile( f );
			Engine::getSong()->setModified();
		}
	}
}




void PatmanView::updateFilename()
{
 	m_displayFilename = "";
	int idx = m_pi->m_patchFile.length();

	QFontMetrics fm(adjustedToPixelSize(font(), SMALL_FONT_SIZE));

	// simple algorithm for creating a text from the filename that
	// matches in the white rectangle
	while( idx > 0 && fm.size( Qt::TextSingleLine,
				m_displayFilename + "..." ).width() < 225 )
	{
		m_displayFilename = m_pi->m_patchFile[--idx] +
							m_displayFilename;
	}

	if( idx > 0 )
	{
		m_displayFilename = "..." + m_displayFilename;
	}

	update();
}




void PatmanView::dragEnterEvent( QDragEnterEvent * _dee )
{
	// For mimeType() and MimeType enum class
	using namespace Clipboard;

	if( _dee->mimeData()->hasFormat( mimeType( MimeType::StringPair ) ) )
	{
		QString txt = _dee->mimeData()->data(
						mimeType( MimeType::StringPair ) );
		if( txt.section( ':', 0, 0 ) == "samplefile" )
		{
			_dee->acceptProposedAction();
		}
		else
		{
			_dee->ignore();
		}
	}
	else
	{
		_dee->ignore();
	}
}




void PatmanView::dropEvent( QDropEvent * _de )
{
	QString type = StringPairDrag::decodeKey( _de );
	QString value = StringPairDrag::decodeValue( _de );
	if( type == "samplefile" )
	{
		m_pi->setFile( value );
		_de->accept();
		return;
	}

	_de->ignore();
}




void PatmanView::paintEvent( QPaintEvent * )
{
	QPainter p( this );

	p.setFont(adjustedToPixelSize(font(), SMALL_FONT_SIZE));
	p.drawText( 8, 116, 235, 16,
			Qt::AlignLeft | Qt::TextSingleLine | Qt::AlignVCenter,
			m_displayFilename );
}




void PatmanView::modelChanged()
{
	m_pi = castModel<PatmanInstrument>();
	m_loopButton->setModel( &m_pi->m_loopedModel );
	m_tuneButton->setModel( &m_pi->m_tunedModel );
	connect( m_pi, SIGNAL( fileChanged() ),
			this, SLOT( updateFilename() ) );
}


} // namespace gui

} // namespace lmms
