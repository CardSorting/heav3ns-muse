/*
 * DataFile.cpp - implementation of class DataFile
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 * Copyright (c) 2012-2013 Paul Giblock    <p/at/pgiblock.net>
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


#include "DataFile.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <limits>
#include <stdexcept>
#include <utility>

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QDir>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include "base64.h"
#include "ConfigManager.h"
#include "DeprecationHelper.h"
#include "Effect.h"
#include "FileResourceSnapshot.h"
#include "FileResourceIO.h"
#include "embed.h"
#include "GuiApplication.h"
#include "LocaleHelper.h"
#include "Note.h"
#include "PluginFactory.h"
#include "ProjectVersion.h"
#include "SongEditor.h"
#include "TextFloat.h"
#include "Track.h"
#include "PathUtil.h"
#include "StudioAssetStore.h"
#include "UpgradeExtendedNoteRange.h"

#include "lmmsversion.h"

namespace lmms
{


static void findIds(const QDomElement& elem, QList<jo_id_t>& idList);
static thread_local DataFile* resourceSourceDocument = nullptr;
static thread_local bool resourceSourceRetainSamples = false;
static thread_local const DataFile* resourceReadDocument = nullptr;

QString DataFile::serializationError(const QDomDocument& document)
{
	for (auto node = document.firstChild(); !node.isNull(); node = node.nextSibling())
	{
		if (node.isProcessingInstruction() && node.nodeName() == "lmms-serialization-error")
		{
			return node.nodeValue().isEmpty() ? QObject::tr("Native serialization failed") : node.nodeValue();
		}
	}
	return {};
}

void DataFile::recordSerializationError(QDomDocument& document, const QString& error)
{
	if (!serializationError(document).isEmpty()) { return; }
	auto message = error.isEmpty() ? QObject::tr("Native serialization failed") : error;
	message.replace("?>", "? >");
	document.appendChild(document.createProcessingInstruction("lmms-serialization-error", message));
}

QString DataFile::toString(int indent) const
{
	if (!serializationError().isEmpty()) { return {}; }
	return QDomDocument::toString(indent);
}


// QMap with the DOM elements that access file resources
const DataFile::ResourcesMap DataFile::ELEMENTS_WITH_RESOURCES = {
{ "sampleclip", {"src"} },
{ "audiofileprocessor", {"src"} },
{ "slicert", {"src"} },
{ "sf2player", {"src"} },
{ "gigplayer", {"src"} },
{ "patman", {"src"} },
{ "tripleoscillator", {"userwavefile0", "userwavefile1", "userwavefile2"} },
{ "lfocontroller", {"userwavefile"} },
{ "elvol", {"userwavefile"} },
{ "elcut", {"userwavefile"} },
{ "elres", {"userwavefile"} },
};

DataFile::ResourceSourceScope::ResourceSourceScope(DataFile& document, bool enabled, bool retainSamples)
	: m_previous(resourceSourceDocument), m_previousRetainSamples(resourceSourceRetainSamples)
{
	resourceSourceDocument = enabled ? &document : nullptr;
	resourceSourceRetainSamples = enabled && retainSamples;
}

DataFile::ResourceSourceScope::~ResourceSourceScope()
{
	resourceSourceDocument = m_previous;
	resourceSourceRetainSamples = m_previousRetainSamples;
}

DataFile::ResourceReadScope::ResourceReadScope(const DataFile& document)
	: m_previous(resourceReadDocument)
{
	resourceReadDocument = &document;
}

DataFile::ResourceReadScope::~ResourceReadScope()
{
	resourceReadDocument = m_previous;
}

DataFile::ResourceReferenceScope::ResourceReferenceScope(const QDomElement& element, const QString& attribute)
{
	const auto document = resourceReadDocument;
	if (document && element.ownerDocument() == static_cast<const QDomDocument&>(*document))
	{
		for (const auto& binding : document->m_resourceSources)
		{
			if (binding.element != element || binding.attribute != attribute) { continue; }
			if (element.attribute(attribute) != binding.declaredValue || !binding.snapshot)
			{
				throw std::runtime_error("Native resource reference changed after its settings were captured");
			}
			m_scope = std::make_unique<FileResourceSnapshot::ReadScope>(
				PathUtil::toAbsolute(binding.declaredValue), binding.snapshot, binding.sample);
			return;
		}
	}
	m_scope = std::make_unique<FileResourceSnapshot::ReadScope>(QString{}, nullptr);
}

void DataFile::registerResourceSource(const QDomDocument& document, const QDomElement& element,
	const QString& attribute, const QString& source)
{
	bindResourceSource(document, element, attribute, source, {});
}

void DataFile::registerResourceSnapshot(const QDomDocument& document, const QDomElement& element,
	const QString& attribute, std::shared_ptr<const FileResourceSnapshot> snapshot,
	std::shared_ptr<const SampleBuffer> sample)
{
	if (!snapshot) { return; }
	const auto path = snapshot->path();
	bindResourceSource(document, element, attribute, path, std::move(snapshot), std::move(sample));
}

void DataFile::bindResourceSource(const QDomDocument& document, const QDomElement& element,
	const QString& attribute, const QString& source, std::shared_ptr<const FileResourceSnapshot> snapshot,
	std::shared_ptr<const SampleBuffer> sample)
{
	auto target = resourceSourceDocument;
	if (!target || document != static_cast<const QDomDocument&>(*target) || element.ownerDocument() != document
		|| source.isEmpty()) { return; }
	const auto known = ELEMENTS_WITH_RESOURCES.find(element.tagName());
	if (known == ELEMENTS_WITH_RESOURCES.end()
		|| std::find(known->second.begin(), known->second.end(), attribute) == known->second.end()) { return; }
	if (!resourceSourceRetainSamples) { sample.reset(); }
	for (auto& binding : target->m_resourceSources)
	{
		if (binding.element == element && binding.attribute == attribute)
		{
			binding.path = source; binding.declaredValue = element.attribute(attribute);
			binding.snapshot = std::move(snapshot); binding.sample = std::move(sample); return;
		}
	}
	target->m_resourceSources.push_back({element, attribute, source, element.attribute(attribute),
		std::move(snapshot), std::move(sample)});
}

// Vector with all the upgrade methods
const std::vector<DataFile::UpgradeMethod> DataFile::UPGRADE_METHODS = {
	&DataFile::upgrade_0_2_1_20070501   ,   &DataFile::upgrade_0_2_1_20070508,
	&DataFile::upgrade_0_3_0_rc2        ,   &DataFile::upgrade_0_3_0,
	&DataFile::upgrade_0_4_0_20080104   ,   &DataFile::upgrade_0_4_0_20080118,
	&DataFile::upgrade_0_4_0_20080129   ,   &DataFile::upgrade_0_4_0_20080409,
	&DataFile::upgrade_0_4_0_20080607   ,   &DataFile::upgrade_0_4_0_20080622,
	&DataFile::upgrade_0_4_0_beta1      ,   &DataFile::upgrade_0_4_0_rc2,
	&DataFile::upgrade_1_0_99           ,   &DataFile::upgrade_1_1_0,
	&DataFile::upgrade_1_1_91           ,   &DataFile::upgrade_1_2_0_rc3,
	&DataFile::upgrade_1_3_0            ,   &DataFile::upgrade_noHiddenClipNames,
	&DataFile::upgrade_automationNodes  ,   &DataFile::upgrade_extendedNoteRange,
	&DataFile::upgrade_defaultTripleOscillatorHQ,
	&DataFile::upgrade_mixerRename      ,   &DataFile::upgrade_bbTcoRename,
	&DataFile::upgrade_sampleAndHold    ,   &DataFile::upgrade_midiCCIndexing,
	&DataFile::upgrade_loopsRename      ,   &DataFile::upgrade_noteTypes,
	&DataFile::upgrade_fixCMTDelays     ,   &DataFile::upgrade_fixBassLoopsTypo,
	&DataFile::findProblematicLadspaPlugins,
	&DataFile::upgrade_noHiddenAutomationTracks
};

// Vector of all versions that have upgrade routines.
const std::vector<ProjectVersion> DataFile::UPGRADE_VERSIONS = {
	"0.2.1-20070501"   ,   "0.2.1-20070508"   ,   "0.3.0-rc2",
	"0.3.0"            ,   "0.4.0-20080104"   ,   "0.4.0-20080118",
	"0.4.0-20080129"   ,   "0.4.0-20080409"   ,   "0.4.0-20080607",
	"0.4.0-20080622"   ,   "0.4.0-beta1"      ,   "0.4.0-rc2",
	"1.0.99-0"         ,   "1.1.0-0"          ,   "1.1.91-0",
	"1.2.0-rc3"        ,   "1.3.0"
};

namespace
{
	struct TypeDescStruct
	{
		DataFile::Type m_type;
		QString m_name;
	};

	const auto s_types = std::array{
		TypeDescStruct{ DataFile::Type::Unknown, "unknown" },
		TypeDescStruct{ DataFile::Type::SongProject, "song" },
		TypeDescStruct{ DataFile::Type::SongProjectTemplate, "songtemplate" },
		TypeDescStruct{ DataFile::Type::InstrumentTrackSettings, "instrumenttracksettings" },
		TypeDescStruct{ DataFile::Type::DragNDropData, "dnddata" },
		TypeDescStruct{ DataFile::Type::ClipboardData, "clipboard-data" },
		TypeDescStruct{ DataFile::Type::JournalData, "journaldata" },
		TypeDescStruct{ DataFile::Type::EffectSettings, "effectsettings" },
		TypeDescStruct{ DataFile::Type::MidiClip, "midiclip" }
	};
}




DataFile::DataFile( Type type ) :
	QDomDocument( "lmms-project" ),
	m_fileName(""),
	m_content(),
	m_head(),
	m_type( type ),
	m_fileVersion( UPGRADE_METHODS.size() )
{
	appendChild( createProcessingInstruction("xml", "version=\"1.0\""));
	QDomElement root = createElement( "lmms-project" );
	root.setAttribute( "version", m_fileVersion );
	root.setAttribute( "type", typeName( type ) );
	root.setAttribute( "creator", "LMMS" );
	root.setAttribute( "creatorversion", LMMS_VERSION );
	root.setAttribute("creatorplatform", QSysInfo::kernelType());
	root.setAttribute("creatorplatformtype", QSysInfo::productType());
	appendChild( root );

	m_head = createElement( "head" );
	root.appendChild( m_head );

	m_content = createElement( typeName( type ) );
	root.appendChild( m_content );

}




DataFile::DataFile( const QString & _fileName ) :
	QDomDocument(),
	m_fileName(_fileName),
	m_content(),
	m_head(),
	m_fileVersion( UPGRADE_METHODS.size() )
{
	QFile inFile( _fileName );
	if( !inFile.open( QIODevice::ReadOnly ) )
	{
		if (gui::getGUI() != nullptr)
		{
			QMessageBox::critical( nullptr,
				gui::SongEditor::tr( "Could not open file" ),
				gui::SongEditor::tr( "Could not open file %1. You probably "
						"have no permissions to read this "
						"file.\n Please make sure to have at "
						"least read permissions to the file "
						"and try again." ).arg( _fileName ) );
		}

		return;
	}

	loadData( inFile.readAll(), _fileName );
}




DataFile::DataFile( const QByteArray & _data ) :
	QDomDocument(),
	m_fileName(""),
	m_content(),
	m_head(),
	m_fileVersion( UPGRADE_METHODS.size() )
{
	loadData( _data, "<internal data>" );
}





bool DataFile::validate( QString extension )
{
	if (!serializationError().isEmpty()) { return false; }
	switch( m_type )
	{
	case Type::SongProject:
		if( extension == "mmp" || extension == "mmpz" )
		{
			return true;
		}
		break;
	case Type::SongProjectTemplate:
		if(  extension == "mpt" )
		{
			return true;
		}
		break;
	case Type::InstrumentTrackSettings:
		if ( extension == "xpf" || extension == "xml" )
		{
			return true;
		}
		break;
	case Type::MidiClip:
		if (extension == "xpt" || extension == "xptz")
		{
			return true;
		}
		break;
	case Type::Unknown:
		if (! ( extension == "mmp" || extension == "mpt" || extension == "mmpz" ||
				extension == "xpf" || extension == "xml" ||
				( extension == "xiz" && ! getPluginFactory()->pluginSupportingExtension(extension).isNull()) ||
				extension == "sf2" || extension == "sf3" || extension == "pat" || extension == "mid" ||
				extension == "dll"
#ifdef LMMS_BUILD_LINUX
				|| extension == "so"
#endif
#ifdef LMMS_HAVE_LV2
				|| extension == "lv2"
#endif
				) )
		{
			return true;
		}
		if( extension == "wav" || extension == "ogg" || extension == "ds"
#ifdef LMMS_HAVE_SNDFILE_MP3
				|| extension == "mp3"
#endif
				)
		{
			return true;
		}
		break;
	default:
		return false;
	}
	return false;
}




QString DataFile::nameWithExtension( const QString & _fn ) const
{
	const QString extension = _fn.section( '.', -1 );

	switch( type() )
	{
		case Type::SongProject:
			if( extension != "mmp" &&
					extension != "mpt" &&
					extension != "mmpz" )
			{
				if( ConfigManager::inst()->value( "app",
						"nommpz" ).toInt() == 0 )
				{
					return _fn + ".mmpz";
				}
				return _fn + ".mmp";
			}
			break;
		case Type::SongProjectTemplate:
			if( extension != "mpt" )
			{
				return _fn + ".mpt";
			}
			break;
		case Type::InstrumentTrackSettings:
			if( extension != "xpf" )
			{
				return _fn + ".xpf";
			}
			break;
		default: ;
	}
	return _fn;
}




void DataFile::write( QTextStream & _strm )
{
	if (!serializationError().isEmpty()) { _strm.setStatus(QTextStream::WriteFailed); return; }
	if( type() == Type::SongProject || type() == Type::SongProjectTemplate
					|| type() == Type::InstrumentTrackSettings )
	{
		cleanMetaNodes( documentElement() );
	}

	save(_strm, 2);
}




bool DataFile::writeFile(const QString& filename, bool withResources)
{
	return writeFile(filename, withResources, {});
}

bool DataFile::writeFile(const QString& filename, bool withResources, const std::function<void()>& beforeCommit)
{
	// Small lambda function for displaying errors
	auto showError = [](QString title, QString body){
		if (gui::getGUI() != nullptr)
		{
			QMessageBox mb;
			mb.setWindowTitle(title);
			mb.setText(body);
			mb.setIcon(QMessageBox::Warning);
			mb.setStandardButtons(QMessageBox::Ok);
			mb.exec();
		}
		else
		{
			qWarning() << body;
		}
	};

	// If we are saving without resources, filename is just the file we are
	// saving to. If we are saving with resources (project bundle), filename
	// will be used (discarding extensions) to create a folder where the
	// bundle will be saved in

	QFileInfo fInfo(filename);

	const QString bundleDir = fInfo.path() + "/" + fInfo.fileName().section('.', 0, 0);
	const QString resourcesDir = bundleDir + "/resources";
	const QString fullName = withResources
		? nameWithExtension(bundleDir + "/" + fInfo.fileName())
		: nameWithExtension(filename);
	const QString fullNameBak = fullName + ".bak";

	using gui::SongEditor;

	// If we are saving with resources, setup the bundle folder first
	if (const auto error = serializationError(); !error.isEmpty())
	{
		showError(SongEditor::tr("Could not save project data"), error); return false;
	}
	QString operationError;
	if (!FileResourceIO::check(operationError)) { showError(SongEditor::tr("Could not save project data"), operationError); return false; }
	if (withResources)
	{
		// First check if there's a bundle folder with the same name in
		// the path already. If so, warns user that we can't overwrite a
		// project bundle.
		if (QDir(bundleDir).exists())
		{
			showError(SongEditor::tr("Operation denied"),
				SongEditor::tr("A bundle folder with that name already exists on the "
				"selected path. Can't overwrite a project bundle. Please select a different "
				"name."));

			return false;
		}

		// Create bundle folder
		if (!QDir().mkdir(bundleDir))
		{
			showError(SongEditor::tr("Error"),
				SongEditor::tr("Couldn't create bundle folder."));
			return false;
		}

		// Create resources folder
		if (!QDir().mkdir(resourcesDir))
		{
			showError(SongEditor::tr("Error"),
				SongEditor::tr("Couldn't create resources folder."));
			return false;
		}

		// Copy resources to folder and update paths
		if (!copyResources(resourcesDir))
		{
			showError(SongEditor::tr("Error"),
				SongEditor::tr("Failed to copy resources."));
			return false;
		}
	}

	QSaveFile outfile(fullName);

	if (!outfile.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		showError(SongEditor::tr("Could not write file"),
			SongEditor::tr("Could not open %1 for writing. You probably are not permitted to "
				"write to this file. Please make sure you have write-access to "
				"the file and try again.").arg(fullName));

		return false;
	}

	const QString extension = fullName.section('.', -1);
	bool written = false;
	if (extension == "mmpz" || extension == "xptz")
	{
		QString xml;
		QTextStream ts( &xml );
		write( ts );
		ts.flush();
		if (ts.status() == QTextStream::Ok && FileResourceIO::check(operationError))
		{
			const auto compressed = qCompress(xml.toUtf8());
			written = !compressed.isEmpty() && outfile.write(compressed) == compressed.size();
		}
	}
	else
	{
		QTextStream ts( &outfile );
		write( ts );
		ts.flush(); written = ts.status() == QTextStream::Ok;
	}
	if (!written || outfile.error() != QFileDevice::NoError || !FileResourceIO::check(operationError))
	{
		showError(SongEditor::tr("Could not write file"), SongEditor::tr("Project data could not be written completely. The original file was preserved."));
		return false;
	}

	// Preserve the old file until the replacement commits atomically. Moving it
	// away first could lose the authoritative file, and ignored rename failures
	// previously allowed writeFile() to report success without publishing it.
	if (beforeCommit) { beforeCommit(); }
	if (!ConfigManager::inst()->value("app", "disablebackup").toInt() && QFile::exists(fullName))
	{
		QFile original(fullName);
		QSaveFile backup(fullNameBak);
		bool backedUp = original.open(QIODevice::ReadOnly) && backup.open(QIODevice::WriteOnly);
		while (backedUp && !original.atEnd())
		{
			if (!FileResourceIO::check(operationError)) { backedUp = false; break; }
			const auto block = original.read(65536);
			backedUp = !block.isEmpty() && backup.write(block) == block.size();
		}
		if (!backedUp || original.error() != QFileDevice::NoError || !FileResourceIO::check(operationError) || !backup.commit())
		{
			showError(SongEditor::tr("Could not write file"),
				SongEditor::tr("The previous project could not be backed up. The original file was preserved."));
			return false;
		}
	}

	if (!FileResourceIO::check(operationError)) { showError(SongEditor::tr("Could not write file"), operationError); return false; }
	if (beforeCommit) { beforeCommit(); }
	if (!outfile.commit())
	{
		showError(SongEditor::tr("Could not write file"),
			SongEditor::tr("An unknown error has occurred and the file could not be saved."));
		return false;
	}

	return true;
}




bool DataFile::makeLocalResourcePathsAbsolute()
{
	for (const auto& [elementName, attributes] : ELEMENTS_WITH_RESOURCES)
	{
		const auto elements = elementsByTagName(elementName);
		for (int i = 0; i < elements.size(); ++i)
		{
			auto element = elements.item(i).toElement();
			for (const auto& attribute : attributes)
			{
				const auto path = element.attribute(attribute);
				if (!path.startsWith(PathUtil::basePrefix(PathUtil::Base::LocalDir), Qt::CaseInsensitive)) { continue; }
				bool error = false;
				const auto absolute = PathUtil::toAbsolute(path, &error);
				if (error) { return false; }
				element.setAttribute(attribute, absolute);
			}
		}
	}
	return true;
}

bool DataFile::copyResources(const QString& resourcesDir, QJsonArray* checkpointAssets)
{
	if (checkpointAssets) { *checkpointAssets = {}; }
	if (!serializationError().isEmpty()) { return false; }
	QString operationError;
	if (!FileResourceIO::check(operationError)) { qWarning() << operationError; return false; }
	QSet<QString> usedNames;
	QSet<QString> recordedAssets;
	QJsonArray capturedAssets;
	QHash<QString, QString> copiedSources;
	struct Rewrite { QDomElement element; QString attribute; QString path; };
	std::vector<Rewrite> rewrites;
	const QDir destination(resourcesDir);
	for (const auto& [elementName, attributes] : ELEMENTS_WITH_RESOURCES)
	{
		const auto elements = elementsByTagName(elementName);
		for (int i = 0; i < elements.size(); ++i)
		{
			auto element = elements.item(i).toElement();
			for (const auto& attribute : attributes)
			{
				const auto original = element.attribute(attribute);
				const bool unresolvedSample = (elementName == "audiofileprocessor" || elementName == "sampleclip") && attribute == "src" && element.attribute("unresolved") == "1";
				const bool unresolvedWave = elementName == "lfocontroller" && attribute == "userwavefile" && element.attribute("userwaveunresolved") == "1";
				const bool unresolvedEnvelope = (elementName == "elvol" || elementName == "elcut" || elementName == "elres") && attribute == "userwavefile" && element.attribute("userwaveunresolved") == "1";
				const bool unresolvedOscillator = elementName == "tripleoscillator" && (attribute == "userwavefile0" || attribute == "userwavefile1" || attribute == "userwavefile2") && element.attribute("userwaveunresolved" + attribute.right(1)) == "1";
				const bool unresolvedBank = (elementName == "sf2player" || elementName == "gigplayer") && attribute == "src" && element.attribute("unresolved") == "1";
				const bool unresolved = unresolvedSample || unresolvedWave || unresolvedOscillator || unresolvedEnvelope || unresolvedBank;
				if ((unresolved && (original.isEmpty() || original.contains(QChar::Null))) || (unresolvedSample && !element.attribute(elementName == "sampleclip" ? "data" : "sampledata").isEmpty())) { return false; }
				if (original.isEmpty()) { continue; }
				bool error = false;
				QString originalPath;
				const auto prefix = PathUtil::basePrefix(PathUtil::Base::LocalDir);
				if (!m_fileName.isEmpty() && original.startsWith(prefix, Qt::CaseInsensitive))
				{
					// A loaded DataFile owns its bundle base even when another Song
					// is open in the engine.
					const auto relative = original.mid(prefix.size());
					if (QDir::isAbsolutePath(relative)) { return false; }
					originalPath = QFileInfo(m_fileName).absoluteDir().absoluteFilePath(relative);
				}
				else { originalPath = PathUtil::toAbsolute(original, &error); }
				if (error) { return false; }
				auto sourcePath = originalPath;
				bool bound = false;
				std::shared_ptr<const FileResourceSnapshot> accepted;
				for (const auto& binding : m_resourceSources)
				{
					if (binding.element == element && binding.attribute == attribute)
					{
						sourcePath = binding.path; accepted = binding.snapshot; bound = true; break;
					}
				}
				if (unresolved)
				{
					// Native checkpoints preserve deliberate silent state, even if a
					// different file later appears at the retained source path. Bundles
					// still require every dependency and never claim portable completion.
					if (!checkpointAssets || bound || !QDir::isAbsolutePath(originalPath)) { return false; }
					rewrites.push_back({element, attribute, QDir::cleanPath(originalPath)}); continue;
				}
				if (elementName == "gigplayer" && !accepted)
				{
					accepted = FileResourceSnapshot::captureGig(sourcePath, operationError);
					if (!accepted) { qWarning() << operationError; return false; }
					sourcePath = accepted->path();
				}
				const QFileInfo source(sourcePath);
				if (!source.exists() || !source.isFile()) { qWarning("ERROR: Resource source is unavailable"); return false; }
				const auto canonical = source.canonicalFilePath();
				const auto sourceKey = canonical.isEmpty() ? source.absoluteFilePath() : canonical;
				QString finalName = copiedSources.value(sourceKey);
				const bool reused = !finalName.isEmpty();
				if (finalName.isEmpty())
				{
					const QFileInfo naming(originalPath);
					if (checkpointAssets || (accepted && accepted->isGroup()))
					{
						QJsonObject reference; QString assetError;
						if (!StudioAssetStore::preserve(sourcePath, resourcesDir, naming.suffix(), reference, assetError, accepted.get()))
						{
							qWarning() << assetError; return false;
						}
						const auto path = reference.value("path").toString();
						finalName = path.mid(QString("assets/").size());
						if (checkpointAssets && !recordedAssets.contains(path)) { capturedAssets.append(reference); recordedAssets.insert(path); }
					}
					else
					{
						finalName = naming.fileName();
						if (finalName.isEmpty()) { return false; }
						const auto suffix = naming.suffix();
						const auto stem = suffix.isEmpty() ? finalName : naming.completeBaseName();
						int counter = 0;
						while (usedNames.contains(finalName) || QFileInfo::exists(destination.filePath(finalName)))
						{
							if (counter == std::numeric_limits<int>::max()) { return false; }
							finalName = stem + "-" + QString::number(++counter) + (suffix.isEmpty() ? "" : "." + suffix);
						}
						if (!FileResourceIO::copyFile(sourcePath, destination.filePath(finalName), operationError))
						{
							qWarning() << operationError; return false;
						}
						QString copyError;
						if (accepted && !accepted->verifyCopy(destination.filePath(finalName), copyError))
						{
							QFile::remove(destination.filePath(finalName));
							qWarning() << copyError; return false;
						}
						usedNames.insert(finalName);
					}
					copiedSources.insert(sourceKey, finalName);
				}
				if (reused && accepted)
				{
					QString copyError;
					if (!accepted->verifyCopy(destination.filePath(finalName), copyError)) { qWarning() << copyError; return false; }
				}
				rewrites.push_back({element, attribute,
					PathUtil::basePrefix(PathUtil::Base::LocalDir) + (checkpointAssets ? "assets/" : "resources/") + finalName});
			}
		}
	}
	// A later failure must not leave a partially rebased native document.
	if (!FileResourceIO::check(operationError)) { qWarning() << operationError; return false; }
	for (auto& rewrite : rewrites) { rewrite.element.setAttribute(rewrite.attribute, rewrite.path); }
	if (checkpointAssets) { *checkpointAssets = capturedAssets; }
	return true;
}

bool DataFile::matchesAssetReferences(const QJsonArray& assets) const
{
	if (!StudioAssetStore::validReferences(assets)) { return false; }
	QSet<QString> declared, used;
	for (const auto& value : assets) { declared.insert(value.toObject().value("path").toString()); }
	const auto prefix = PathUtil::basePrefix(PathUtil::Base::LocalDir);
	for (const auto& [tag, attributes] : ELEMENTS_WITH_RESOURCES)
	{
		const auto elements = elementsByTagName(tag);
		for (int i = 0; i < elements.size(); ++i)
		{
			const auto element = elements.item(i).toElement();
			for (const auto& attribute : attributes)
			{
				const auto path = element.attribute(attribute);
				const bool unresolvedSample = (tag == "audiofileprocessor" || tag == "sampleclip") && attribute == "src" && element.attribute("unresolved") == "1";
				const bool unresolvedWave = tag == "lfocontroller" && attribute == "userwavefile" && element.attribute("userwaveunresolved") == "1";
				const bool unresolvedEnvelope = (tag == "elvol" || tag == "elcut" || tag == "elres") && attribute == "userwavefile" && element.attribute("userwaveunresolved") == "1";
				const bool unresolvedOscillator = tag == "tripleoscillator" && (attribute == "userwavefile0" || attribute == "userwavefile1" || attribute == "userwavefile2") && element.attribute("userwaveunresolved" + attribute.right(1)) == "1";
				const bool unresolvedBank = (tag == "sf2player" || tag == "gigplayer") && attribute == "src" && element.attribute("unresolved") == "1";
				if (unresolvedSample || unresolvedWave || unresolvedOscillator || unresolvedEnvelope || unresolvedBank)
				{
					if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null) || (unresolvedSample && !element.attribute(tag == "sampleclip" ? "data" : "sampledata").isEmpty())) { return false; }
					continue;
				}
				if (path.isEmpty()) { continue; }
				if (!path.startsWith(prefix)) { return false; }
				const auto relative = path.mid(prefix.size());
				if (!declared.contains(relative)) { return false; }
				used.insert(relative);
			}
		}
	}
	return used == declared;
}




bool DataFile::hasLocalPlugins(QDomElement parent /* = QDomElement()*/, bool firstCall /* = true*/) const
{
	// If this is the first iteration of the recursion we use the root element
	if (firstCall) { parent = documentElement(); }

	auto children = parent.childNodes();
	for (int i = 0; i < children.size(); ++i)
	{
		QDomNode child = children.at(i);
		QDomElement childElement = child.toElement();

		const auto resource = ELEMENTS_WITH_RESOURCES.find(childElement.tagName());
		const auto attributes = childElement.attributes();
		for (int j = 0; j < attributes.size(); ++j)
		{
			const auto attribute = attributes.item(j).toAttr();
			if (!attribute.value().startsWith(PathUtil::basePrefix(PathUtil::Base::LocalDir), Qt::CaseInsensitive)) { continue; }
			const bool isAsset = resource != ELEMENTS_WITH_RESOURCES.end()
				&& std::find(resource->second.begin(), resource->second.end(), attribute.name()) != resource->second.end();
			if (!isAsset) { return true; }
		}

		// Now we check the children of this node (recursively)
		// and if any return true we return true.
		if (hasLocalPlugins(childElement, false))
		{
			return true;
		}
	}

	// If we got here none of the nodes had the "local:" path.
	return false;
}




DataFile::Type DataFile::type( const QString& typeName )
{
	const auto it = std::find_if(s_types.begin(), s_types.end(),
		[&typeName](const TypeDescStruct& type) { return type.m_name == typeName; });
	if (it != s_types.end()) { return it->m_type; }

	// compat code
	if( typeName == "channelsettings" )
	{
		return Type::InstrumentTrackSettings;
	}

	if (typeName == "pattern")
	{
		return Type::MidiClip;
	}

	return Type::Unknown;
}




QString DataFile::typeName( Type type )
{
	return s_types[static_cast<std::size_t>(type)].m_name;
}




void DataFile::cleanMetaNodes( QDomElement _de )
{
	QDomNode node = _de.firstChild();
	while( !node.isNull() )
	{
		if( node.isElement() )
		{
			if( node.toElement().attribute( "metadata" ).toInt() )
			{
				QDomNode ns = node.nextSibling();
				_de.removeChild( node );
				node = ns;
				continue;
			}
			if( node.hasChildNodes() )
			{
				cleanMetaNodes( node.toElement() );
			}
		}
		node = node.nextSibling();
	}
}

void DataFile::mapSrcAttributeInElementsWithResources(const QMap<QString, QString>& map)
{
	for (const auto& [elem, srcAttrs] : ELEMENTS_WITH_RESOURCES)
	{
		auto elements = elementsByTagName(elem);

		for (const auto& srcAttr : srcAttrs)
		{
			for (int i = 0; i < elements.length(); ++i)
			{
				auto item = elements.item(i).toElement();

				if (item.isNull() || !item.hasAttribute(srcAttr)) { continue; }

				const QString srcVal = item.attribute(srcAttr);

				const auto it = map.constFind(srcVal);
				if (it != map.constEnd())
				{
					item.setAttribute(srcAttr, *it);
				}
			}
		}
	}
}


void DataFile::upgrade_0_2_1_20070501()
{
	// Upgrade to version 0.2.1-20070501
	QDomNodeList list = elementsByTagName( "arpandchords" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.hasAttribute( "arpdir" ) )
		{
			int arpdir = el.attribute( "arpdir" ).toInt();
			if( arpdir > 0 )
			{
				el.setAttribute( "arpdir", arpdir - 1 );
			}
			else
			{
				el.setAttribute( "arpdisabled", "1" );
			}
		}
	}

	list = elementsByTagName( "sampletrack" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.attribute( "vol" ) != "" )
		{
			el.setAttribute( "vol", LocaleHelper::toFloat(
					el.attribute( "vol" ) ) * 100.0f );
		}
		else
		{
			QDomNode node = el.namedItem(
						"automation-pattern" );
			if( !node.isElement() ||
				!node.namedItem( "vol" ).isElement() )
			{
				el.setAttribute( "vol", 100.0f );
			}
		}
	}

	list = elementsByTagName( "ladspacontrols" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		QDomNode anode = el.namedItem( "automation-pattern" );
		QDomNode node = anode.firstChild();
		while( !node.isNull() )
		{
			if( node.isElement() )
			{
				QString name = node.nodeName();
				if( name.endsWith( "link" ) )
				{
					el.setAttribute( name,
						node.namedItem( "time" )
						.toElement()
						.attribute( "value" ) );
					QDomNode oldNode = node;
					node = node.nextSibling();
					anode.removeChild( oldNode );
					continue;
				}
			}
			node = node.nextSibling();
		}
	}

	QDomNode node = m_head.firstChild();
	while( !node.isNull() )
	{
		if( node.isElement() )
		{
			if( node.nodeName() == "bpm" )
			{
				int value = node.toElement().attribute(
						"value" ).toInt();
				if( value > 0 )
				{
					m_head.setAttribute( "bpm",
								value );
					QDomNode oldNode = node;
					node = node.nextSibling();
					m_head.removeChild( oldNode );
					continue;
				}
			}
			else if( node.nodeName() == "mastervol" )
			{
				int value = node.toElement().attribute(
						"value" ).toInt();
				if( value > 0 )
				{
					m_head.setAttribute(
						"mastervol", value );
					QDomNode oldNode = node;
					node = node.nextSibling();
					m_head.removeChild( oldNode );
					continue;
				}
			}
			else if( node.nodeName() == "masterpitch" )
			{
				m_head.setAttribute( "masterpitch",
					-node.toElement().attribute(
						"value" ).toInt() );
				QDomNode oldNode = node;
				node = node.nextSibling();
				m_head.removeChild( oldNode );
				continue;
			}
		}
		node = node.nextSibling();
	}
}


void DataFile::upgrade_0_2_1_20070508()
{
	// Upgrade to version 0.2.1-20070508 from some version greater than or equal to 0.2.1-20070501
	QDomNodeList list = elementsByTagName( "arpandchords" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.hasAttribute( "chorddisabled" ) )
		{
			el.setAttribute( "chord-enabled",
				!el.attribute( "chorddisabled" )
							.toInt() );
			el.setAttribute( "arp-enabled",
				!el.attribute( "arpdisabled" )
							.toInt() );
		}
		else if( !el.hasAttribute( "chord-enabled" ) )
		{
			el.setAttribute( "chord-enabled", true );
			el.setAttribute( "arp-enabled",
				el.attribute( "arpdir" ).toInt() != 0 );
		}
	}

	while( !( list = elementsByTagName( "channeltrack" ) ).isEmpty() )
	{
		QDomElement el = list.item( 0 ).toElement();
		el.setTagName( "instrumenttrack" );
	}

	list = elementsByTagName( "instrumenttrack" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.hasAttribute( "vol" ) )
		{
			float value = LocaleHelper::toFloat( el.attribute( "vol" ) );
			value = roundf( value * 0.585786438f );
			el.setAttribute( "vol", value );
		}
		else
		{
			QDomNodeList vol_list = el.namedItem(
						"automation-pattern" )
					.namedItem( "vol" ).toElement()
					.elementsByTagName( "time" );
			for( int j = 0; !vol_list.item( j ).isNull();
								++j )
			{
				QDomElement timeEl = list.item( j )
							.toElement();
				int value = timeEl.attribute( "value" )
							.toInt();
				value = (int)roundf( value *
							0.585786438f );
				timeEl.setAttribute( "value", value );
			}
		}
	}
}


void DataFile::upgrade_0_3_0_rc2()
{
	// Upgrade to version 0.3.0-rc2 from some version greater than or equal to 0.2.1-20070508
	QDomNodeList list = elementsByTagName( "arpandchords" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.attribute( "arpdir" ).toInt() > 0 )
		{
			el.setAttribute( "arpdir",
				el.attribute( "arpdir" ).toInt() - 1 );
		}
	}
}


void DataFile::upgrade_0_3_0()
{
	// Upgrade to version 0.3.0 (final) from some version greater than or equal to 0.3.0-rc2
	QDomNodeList list;
	while( !( list = elementsByTagName(
				"pluckedstringsynth" ) ).isEmpty() )
	{
		QDomElement el = list.item( 0 ).toElement();
		el.setTagName( "vibedstrings" );
		el.setAttribute( "active0", 1 );
	}

	while( !( list = elementsByTagName( "lb303" ) ).isEmpty() )
	{
		QDomElement el = list.item( 0 ).toElement();
		el.setTagName( "lb302" );
	}

	while( !( list = elementsByTagName( "channelsettings" ) ).
							isEmpty() )
	{
		QDomElement el = list.item( 0 ).toElement();
		el.setTagName( "instrumenttracksettings" );
	}
}


void DataFile::upgrade_0_4_0_20080104()
{
	// Upgrade to version 0.4.0-20080104 from some version greater than or equal to 0.3.0 (final)
	QDomNodeList list = elementsByTagName( "fx" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.hasAttribute( "fxdisabled" ) &&
			el.attribute( "fxdisabled" ).toInt() == 0 )
		{
			el.setAttribute( "enabled", 1 );
		}
	}
}


void DataFile::upgrade_0_4_0_20080118()
{
	// Upgrade to version 0.4.0-20080118 from some version greater than or equal to 0.4.0-20080104
	QDomNodeList list;
	while( !( list = elementsByTagName( "fx" ) ).isEmpty() )
	{
		QDomElement fxchain = list.item( 0 ).toElement();
		fxchain.setTagName( "fxchain" );
		QDomNode rack = list.item( 0 ).firstChild();
		QDomNodeList effects = rack.childNodes();
		// move items one level up
		while( effects.count() )
		{
			fxchain.appendChild( effects.at( 0 ) );
		}
		fxchain.setAttribute( "numofeffects",
			rack.toElement().attribute( "numofeffects" ) );
		fxchain.removeChild( rack );
	}
}


void DataFile::upgrade_0_4_0_20080129()
{
	// Upgrade to version 0.4.0-20080129 from some version greater than or equal to 0.4.0-20080118
	QDomNodeList list;
	while( !( list =
		elementsByTagName( "arpandchords" ) ).isEmpty() )
	{
		QDomElement aac = list.item( 0 ).toElement();
		aac.setTagName( "arpeggiator" );
		QDomNode cloned = aac.cloneNode();
		cloned.toElement().setTagName( "chordcreator" );
		aac.parentNode().appendChild( cloned );
	}
}


void DataFile::upgrade_0_4_0_20080409()
{
	// Upgrade to version 0.4.0-20080409 from some version greater than or equal to 0.4.0-20080129
	QStringList s;
	s << "note" << "pattern" << "bbtco" << "sampletco" << "time";
	for( QStringList::iterator it = s.begin(); it < s.end(); ++it )
	{
		QDomNodeList list = elementsByTagName( *it );
		for( int i = 0; !list.item( i ).isNull(); ++i )
		{
			QDomElement el = list.item( i ).toElement();
			el.setAttribute( "pos",
				el.attribute( "pos" ).toInt()*3 );
			el.setAttribute( "len",
				el.attribute( "len" ).toInt()*3 );
		}
	}
	QDomNodeList list = elementsByTagName( "timeline" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		el.setAttribute( "lp0pos",
			el.attribute( "lp0pos" ).toInt()*3 );
		el.setAttribute( "lp1pos",
			el.attribute( "lp1pos" ).toInt()*3 );
	}
}


void DataFile::upgrade_0_4_0_20080607()
{
	// Upgrade to version 0.4.0-20080607 from some version greater than or equal to 0.3.0-20080409
	QDomNodeList list;
	while( !( list = elementsByTagName( "midi" ) ).isEmpty() )
	{
		QDomElement el = list.item( 0 ).toElement();
		el.setTagName( "midiport" );
	}
}


void DataFile::upgrade_0_4_0_20080622()
{
	// Upgrade to version 0.4.0-20080622 from some version greater than or equal to 0.3.0-20080607
	QDomNodeList list;
	while( !( list = elementsByTagName(
				"automation-pattern" ) ).isEmpty() )
	{
		QDomElement el = list.item( 0 ).toElement();
		el.setTagName( "automationpattern" );
	}

	list = elementsByTagName( "bbtrack" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		QString s = el.attribute( "name" );
		s.replace(QRegularExpression("^Beat/Baseline "), "Beat/Bassline");
		el.setAttribute( "name", s );
	}
}


void DataFile::upgrade_0_4_0_beta1()
{
	// Upgrade to version 0.4.0-beta1 from some version greater than or equal to 0.4.0-20080622
	// convert binary effect-key-blobs to XML
	QDomNodeList list;
	list = elementsByTagName( "effect" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		QString k = el.attribute( "key" );
		if( !k.isEmpty() )
		{
			const QList<QVariant> l =
				base64::decode(k, QMetaType::QVariantList).toList();
			if( !l.isEmpty() )
			{
				QString name = l[0].toString();
				QVariant u = l[1];
				EffectKey::AttributeMap m;
				// VST-effect?
				if (typeId(u) == QMetaType::QString)
				{
					m["file"] = u.toString();
				}
				// LADSPA-effect?
				else if (typeId(u) == QMetaType::QStringList)
				{
					const QStringList sl = u.toStringList();
					m["plugin"] = sl.value( 0 );
					m["file"] = sl.value( 1 );
				}
				EffectKey key( nullptr, name, m );
				el.appendChild( key.saveXML( *this ) );
			}
		}
	}
}


void DataFile::upgrade_0_4_0_rc2()
{
	// Upgrade to version 0.4.0-rc2 from some version greater than or equal to 0.4.0-beta1
	QDomNodeList list = elementsByTagName( "audiofileprocessor" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		QString s = el.attribute( "src" );
		s.replace( "drumsynth/misc ", "drumsynth/misc_" );
		s.replace( "drumsynth/r&b", "drumsynth/r_n_b" );
		s.replace( "drumsynth/r_b", "drumsynth/r_n_b" );
		el.setAttribute( "src", s );
	}
	list = elementsByTagName( "lb302" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		int s = el.attribute( "shape" ).toInt();
		if( s >= 1 )
		{
			s--;
		}
		el.setAttribute( "shape", QString("%1").arg(s) );
	}
}

void DataFile::upgrade_1_0_99()
{
	jo_id_t last_assigned_id = 0;

	QList<jo_id_t> idList;
	findIds(documentElement(), idList);

	QDomNodeList list = elementsByTagName("ladspacontrols");
	for(int i = 0; !list.item(i).isNull(); ++i)
	{
		for(QDomNode node = list.item(i).firstChild(); !node.isNull();
			node = node.nextSibling())
		{
			QDomElement el = node.toElement();
			QDomNode data_child = el.namedItem("data");
			if(!data_child.isElement())
			{
				if (el.attribute("scale_type") == "log")
				{
					QDomElement me = createElement("data");
					me.setAttribute("value", el.attribute("data"));
					me.setAttribute("scale_type", "log");

					jo_id_t id;
					for(id = last_assigned_id + 1;
						idList.contains(id); id++)
					{
					}

					last_assigned_id = id;
					idList.append(id);
					me.setAttribute("id", id);
					el.appendChild(me);

				}
			}
		}
	}
}


void DataFile::upgrade_1_1_0()
{
	QDomNodeList list = elementsByTagName("fxchannel");
	for (int i = 1; !list.item(i).isNull(); ++i)
	{
		QDomElement el = list.item(i).toElement();
		QDomElement send = createElement("send");
		send.setAttribute("channel", "0");
		send.setAttribute("amount", "1");
		el.appendChild(send);
	}
}


void DataFile::upgrade_1_1_91()
{
	// Upgrade to version 1.1.91 from some version less than 1.1.91
	QDomNodeList list = elementsByTagName( "audiofileprocessor" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		QString s = el.attribute( "src" );
		s.replace(QRegularExpression("/samples/bassloopes/"), "/samples/bassloops/");
		el.setAttribute( "src", s );
	}

	list = elementsByTagName( "attribute" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.attribute( "name" ) == "plugin" && el.attribute( "value" ) == "vocoder-lmms" ) {
			el.setAttribute( "value", "vocoder" );
		}
	}

	list = elementsByTagName( "crossoevereqcontrols" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		// invert the mute LEDs
		for( int j = 1; j <= 4; ++j ){
			QString a = QString( "mute%1" ).arg( j );
			el.setAttribute( a, ( el.attribute( a ) == "0" ) ? "1" : "0" );
		}
	}

	list = elementsByTagName( "arpeggiator" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		// Swap elements ArpDirRandom and ArpDirDownAndUp
		if( el.attribute( "arpdir" ) == "3" )
		{
			el.setAttribute( "arpdir", "4" );
		}
		else if( el.attribute( "arpdir" ) == "4" )
		{
			el.setAttribute( "arpdir", "3" );
		}
	}
}


static void upgradeElement_1_2_0_rc2_42( QDomElement & el )
{
	if( el.hasAttribute( "syncmode" ) )
	{
		int syncmode = el.attribute( "syncmode" ).toInt();
		QStringList names;
		QDomNamedNodeMap atts = el.attributes();
		for (auto i = 0; i < atts.length(); i++)
		{
			QString name = atts.item( i ).nodeName();
			if( name.endsWith( "_numerator" ) )
			{
				names << name.remove( "_numerator" )
								+ "_syncmode";
			}
		}
		for( QStringList::iterator it = names.begin(); it < names.end();
									++it )
		{
			el.setAttribute( *it, syncmode );
		}
	}

	QDomElement child = el.firstChildElement();
	while ( !child.isNull() )
	{
		upgradeElement_1_2_0_rc2_42( child );
		child = child.nextSiblingElement();
	}
}


void DataFile::upgrade_1_2_0_rc3()
{
	// Upgrade from earlier bbtrack beat note behaviour of adding
	// steps if a note is placed after the last step.
	QDomNodeList bbtracks = elementsByTagName( "bbtrack" );
	for( int i = 0; !bbtracks.item( i ).isNull(); ++i )
	{
		QDomNodeList patterns = bbtracks.item( i
				).toElement().elementsByTagName(
								"pattern" );
		for( int j = 0; !patterns.item( j ).isNull(); ++j )
		{
			QDomElement el = patterns.item( j ).toElement();
			if( el.attribute( "len" ) != "" )
			{
				int patternLength = el.attribute( "len" ).toInt();
				int steps = patternLength / 12;
				el.setAttribute( "steps", steps );
			}
		}
	}

	// DataFile::upgrade_1_2_0_rc2_42
	QDomElement el = firstChildElement();
	while ( !el.isNull() )
	{
		upgradeElement_1_2_0_rc2_42( el );
		el = el.nextSiblingElement();
	}
}


/**
 *  Helper function to call a functor for all effect ports' DomElements,
 *  providing the functor with lists to add and remove DomElements. Helpful for
 *  patching port values from savefiles.
 */
template<class Ftor>
void iterate_ladspa_ports(QDomElement& effect, Ftor& ftor)
{
	// Head back up the DOM to upgrade ports
	QDomNodeList ladspacontrols = effect.elementsByTagName( "ladspacontrols" );
	for( int m = 0; !ladspacontrols.item( m ).isNull(); ++m )
	{
		QList<QDomElement> addList, removeList;
		QDomElement ladspacontrol = ladspacontrols.item( m ).toElement();
		for( QDomElement port = ladspacontrol.firstChild().toElement();
			!port.isNull(); port = port.nextSibling().toElement() )
		{
			QStringList parts = port.tagName().split("port");
			// Not a "port"
			if ( parts.size() < 2 )
			{
				continue;
			}
			int num = parts[1].toInt();

			// From Qt's docs of QDomNode:
			// * copying a QDomNode is OK, they still have the same
			//   pointer to the "internal" QDomNodePrivate.
			// * Also, they are using linked lists, which means
			//   deleting or appending QDomNode does not invalidate
			//   any other pointers.
			// => Inside ftor, you can (and should) push back the
			//    QDomElements by value, not references
			// => The loops below for adding and removing don't
			//    invalidate any other QDomElements
			ftor(port, num, addList, removeList);
		}

		// Add ports marked for adding
		for ( QDomElement e : addList )
		{
			ladspacontrol.appendChild( e );
		}
		// Remove ports marked for removal
		for ( QDomElement e : removeList )
		{
			ladspacontrol.removeChild( e );
		}
	}
}

// helper function if you need to print a QDomNode
QDebug operator<<(QDebug dbg, const QDomNode& node)
{
	QString s;
	QTextStream str(&s, QIODevice::WriteOnly);
	node.save(str, 2);
	dbg << qPrintable(s);
	return dbg;
}

void DataFile::upgrade_1_3_0()
{
	QDomNodeList list = elementsByTagName( "instrument" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement el = list.item( i ).toElement();
		if( el.attribute( "name" ) == "papu" )
		{
			el.setAttribute( "name", "freeboy" );
			QDomNodeList children = el.elementsByTagName( "papu" );
			for( int j = 0; !children.item( j ).isNull(); ++j )
			{
				QDomElement child = children.item( j ).toElement();
				child.setTagName( "freeboy" );
			}
		}
		else if( el.attribute( "name" ) == "OPL2" )
		{
			el.setAttribute( "name", "opulenz" );
			QDomNodeList children = el.elementsByTagName( "OPL2" );
			for( int j = 0; !children.item( j ).isNull(); ++j )
			{
				QDomElement child = children.item( j ).toElement();
				child.setTagName( "opulenz" );
			}
		}
	}

	list = elementsByTagName( "effect" );
	for( int i = 0; !list.item( i ).isNull(); ++i )
	{
		QDomElement effect = list.item( i ).toElement();
		if( effect.attribute( "name" ) == "ladspaeffect" )
		{
			QDomNodeList keys = effect.elementsByTagName( "key" );
			for( int j = 0; !keys.item( j ).isNull(); ++j )
			{
				QDomElement key = keys.item( j ).toElement();
				QDomNodeList attributes = key.elementsByTagName( "attribute" );
				for( int k = 0; !attributes.item( k ).isNull(); ++k )
				{
					// Effect name changes

					QDomElement attribute = attributes.item( k ).toElement();
					const QString attrName = attribute.attribute("name");
					const QString attrVal = attribute.attribute("value");
					const QString plugin = attrName == "plugin" ? attrVal : "";

					static const std::map<QString, QString> pluginNames = {
						{"Sidechaincompressor", "SidechainCompressor"},
						{"Sidechaingate", "SidechainGate"},
						{"Multibandcompressor", "MultibandCompressor"},
						{"Multibandgate", "MultibandGate"},
						{"Multibandlimiter", "MultibandLimiter"},
					};

					if (attrName == "file" && (attrVal == "calf" || attrVal == "calf.so" ))
					{
						attribute.setAttribute( "value", "veal" );
					}

					const auto newName = pluginNames.find(plugin);
					if (newName != pluginNames.end())
					{
						attribute.setAttribute("value", newName->second);
					}

					// Handle port changes

					if (plugin == "MultibandLimiter" ||	plugin == "MultibandCompressor" || plugin == "MultibandGate")
					{
						auto fn = [&](QDomElement& port, int num, QList<QDomElement>&, QList<QDomElement>& removeList)
						{
							// Mark ports for removal
							if ( num >= 18 && num <= 23 )
							{
								removeList << port;
							}
							// Bump higher ports up 6 positions
							else if ( num >= 24 )
							{
								// port01...port010, etc
								QString name( "port0" );
								name.append( QString::number( num -6 ) );
								port.setTagName( name );
							}
						};
						iterate_ladspa_ports(effect, fn);
					}

					else if (plugin == "Pulsator")
					{
						auto fn = [&](QDomElement& port, int num, QList<QDomElement>& addList, QList<QDomElement>& removeList)
						{
							switch(num)
							{
								case 16:
								{
									// old freq is now at port 25
									QDomElement portCopy = createElement("port025");
									portCopy.setAttribute("data", port.attribute("data"));
									addList << portCopy;
									// remove old freq port
									removeList << port;
									// set the "timing" port to choose port23+2=port25 (timing in Hz)
									QDomElement timing = createElement("port022");
									timing.setAttribute("data", 2);
									addList << timing;
									break;
								}
								// port 18 (modulation) => 17
								case 17:
									port.setTagName("port016");
									break;
								case 18:
								{
									// leave port 18 (offsetr), but add port 17 (offsetl)
									QDomElement offsetl = createElement("port017");
									offsetl.setAttribute("data", 0.0f);
									addList << offsetl;
									// additional: bash port 21 to 1
									QDomElement pulsewidth = createElement("port021");
									pulsewidth.setAttribute("data", 1.0f);
									addList << pulsewidth;
									break;
								}
							}


						};
						iterate_ladspa_ports(effect, fn);
					}

					else if (plugin == "VintageDelay")
					{
						auto fn = [&](QDomElement& port, int num, QList<QDomElement>& addList, QList<QDomElement>& )
						{
							switch(num)
							{
								case 4:
								{
									// BPM is now port028
									port.setTagName("port028");
									// bash timing to BPM
									QDomElement timing = createElement("port027");
									timing.setAttribute("data", 0);
									addList << timing;

									// port 5 and 6 (in, out gain) need to be bashed to 1:
									QDomElement input = createElement("port05");
									input.setAttribute("data", 1.0f);
									addList << input;
									QDomElement output = createElement("port06");
									output.setAttribute("data", 1.0f);
									addList << output;

									break;
								}
								default:
									// all other ports increase by 10
									QString name( "port0" );
									name.append( QString::number( num + 10 ) );
									port.setTagName( name );
							}


						};
						iterate_ladspa_ports(effect, fn);
					}

					else if (plugin == "Equalizer5Band" || plugin == "Equalizer8Band" || plugin == "Equalizer12Band")
					{
						// NBand equalizers got 4 q nobs inserted. We need to shift everything else...
						// HOWEVER: 5 band eq has only 2 q nobs inserted (no LS/HS filters)
						bool band5 = plugin == "Equalizer5Band";
						auto fn = [&](QDomElement& port, int num, QList<QDomElement>& addList, QList<QDomElement>& )
						{
							if(num == 4)
							{
								// don't modify port 4, but some other ones:
								int zoom_port = 0;
								if (plugin == "Equalizer5Band")
									zoom_port = 36;
								else if (plugin == "Equalizer8Band")
									zoom_port = 48;
								else // 12 band
									zoom_port = 64;
								// bash zoom to 0.25
								QString name( "port0" );
								name.append( QString::number( zoom_port ) );
								QDomElement timing = createElement(name);
								timing.setAttribute("data", 0.25f);
								addList << timing;
							}
							// the following code could be refactored, but I did careful code-reading
							// to prevent copy-paste-errors
							if(num == 18)
							{
								// 18 => 19
								port.setTagName("port019");
								// insert port 18 (q)
								QDomElement q = createElement("port018");
								q.setAttribute("data", 0.707f);
								addList << q;
							}
							else if(num >= 19 && num <= 20)
							{
								// num += 1
								QString name( "port0" );
								name.append( QString::number( num + 1 ) );
								port.setTagName( name );
							}
							else if(num == 21)
							{
								// 21 => 23
								port.setTagName("port023");
								// insert port 22 (q)
								QDomElement q = createElement("port022");
								q.setAttribute("data", 0.707f);
								addList << q;
							}
							else if(num >= 22 && (num <= 23 || band5))
							{
								// num += 2
								QString name( "port0" );
								name.append( QString::number( num + 2 ) );
								port.setTagName( name );
							}
							else if(num == 24 && !band5)
							{
								// 24 => 27
								port.setTagName("port027");
								// insert port 26 (q)
								QDomElement q = createElement("port026");
								q.setAttribute("data", 0.707f);
								addList << q;
							}
							else if(num >= 25 && num <= 26 && !band5)
							{
								// num += 3
								QString name( "port0" );
								name.append( QString::number( num + 3 ) );
								port.setTagName( name );
							}
							else if(num == 27 && !band5)
							{
								// 27 => 31
								port.setTagName("port031");
								// insert port 30 (q)
								QDomElement q = createElement("port030");
								q.setAttribute("data", 0.707f);
								addList << q;
							}
							else if(num >= 28 && !band5)
							{
								// num += 4
								QString name( "port0" );
								name.append( QString::number( num + 4 ) );
								port.setTagName( name );
							}
						};
						iterate_ladspa_ports(effect, fn);
					}

					else if (plugin == "Saturator")
					{
						auto fn = [&](QDomElement& port, int num, QList<QDomElement>&, QList<QDomElement>& )
						{
							// These ports have been shifted a bit weird...
							if( num == 7 )
							{
								port.setTagName("port015");
							}
							else if(num == 12)
							{
								port.setTagName("port016");
							}
							else if(num == 13)
							{
								port.setTagName("port017");
							}
							else if ( num >= 15 )
							{
								QString name( "port0" );
								name.append( QString::number( num + 3 ) );
								port.setTagName( name );
							}
						};
						iterate_ladspa_ports(effect, fn);
					}

					else if (plugin == "StereoTools")
					{
						auto fn = [&](QDomElement& port, int num, QList<QDomElement>&, QList<QDomElement>& )
						{
							// This effect can not be back-ported due to bugs in the old version,
							// or due to different behaviour. We thus port all parameters we can,
							// and bash all new parameters (in this case, s.level and m.level) to
							// their new defaults (both 1.0f in this case)

							if( num == 23 || num == 25 )
							{
								port.setAttribute("data", 1.0f);
							}
						};
						iterate_ladspa_ports(effect, fn);
					}

					else if (plugin == "amPitchshift")
					{
						auto fn = [&](QDomElement& port, int num, QList<QDomElement>&, QList<QDomElement>& removeList)
						{
							switch (num)
							{
							case 0:
								port.setTagName("port01");
								break;
							case 1:
								port.setTagName("port03");
								break;
							case 10:
								port.setTagName("port11");
								break;
							case 11:
								port.setTagName("port13");
								break;
							}
						};
						iterate_ladspa_ports(effect, fn);
					}
				}
			}
		}
	}
}

void DataFile::upgrade_noHiddenAutomationTracks()
{
	// convert global automation tracks to non-hidden
	QDomElement song = firstChildElement("lmms-project")
		.firstChildElement("song");
	QDomElement trackContainer = song.firstChildElement("trackcontainer");
	QDomElement globalAutomationTrack = song.firstChildElement("track");
	if (!globalAutomationTrack.isNull()
		&& globalAutomationTrack.attribute("type").toInt() == static_cast<int>(Track::Type::HiddenAutomation))
	{
		// global automation clips
		QDomNodeList automationClips = globalAutomationTrack.elementsByTagName("automationclip");
		QList<QDomNode> tracksToInsert;
		for (int i = 0; i < automationClips.length(); ++i)
		{
			QDomElement automationClip = automationClips.item(i).toElement();
			// If automationClip has time nodes, move it to trackcontainer
			// There are times when an <object> node is present without an
			// object with the same ID in the file, so we ignore that node
			if (automationClip.elementsByTagName("time").length() > 1)
			{
				QDomElement automationTrackForClip = createElement("track");
				automationTrackForClip.setAttribute("muted", QString::number(false));
				automationTrackForClip.setAttribute("solo", QString::number(false));
				automationTrackForClip.setAttribute("type",
					QString::number(static_cast<int>(Track::Type::Automation)));
				automationTrackForClip.setAttribute("name",
					automationClip.attribute("name", "Automation Track"));
				QDomElement at = createElement("automationtrack");
				automationTrackForClip.appendChild(at);
				automationTrackForClip.appendChild(automationClips.item(i).cloneNode());
				tracksToInsert.prepend(automationTrackForClip); // To preserve orders
			}
		}

		// Insert the tracks at the beginning of trackContainer, preserving their order
		for (const auto& track : tracksToInsert) {
			trackContainer.insertBefore(track, trackContainer.firstChild());
		}

		// Remove the track object just in case
		globalAutomationTrack.parentNode().removeChild(globalAutomationTrack);
	}
}

void DataFile::upgrade_noHiddenClipNames()
{
	QDomNodeList tracks = elementsByTagName("track");

	auto clearDefaultNames = [](QDomNodeList clips, QString trackName)
	{
		for (int j = 0; j < clips.size(); ++j)
		{
			QDomElement clip = clips.item(j).toElement();
			QString clipName = clip.attribute("name", "");
			if (clipName == trackName) { clip.setAttribute("name", ""); }
		}
	};

	for (int i = 0; i < tracks.size(); ++i)
	{
		QDomElement track = tracks.item(i).toElement();
		QString trackName = track.attribute("name", "");

		QDomNodeList instClips = track.elementsByTagName("pattern");
		QDomNodeList autoClips = track.elementsByTagName("automationpattern");
		QDomNodeList bbClips = track.elementsByTagName("bbtco");

		clearDefaultNames(instClips, trackName);
		clearDefaultNames(autoClips, trackName);
		clearDefaultNames(bbClips, trackName);
	}
}

void DataFile::upgrade_automationNodes()
{
	QDomNodeList autoPatterns = elementsByTagName("automationpattern");

	// Go through all automation patterns
	for (int i = 0; i < autoPatterns.size(); ++i)
	{
		QDomElement autoPattern = autoPatterns.item(i).toElement();

		// On each automation pattern, get all <time> elements
		QDomNodeList times = autoPattern.elementsByTagName("time");

		// Loop through all <time> elements and change what we need
		for (int j=0; j < times.size(); ++j)
		{
			QDomElement el = times.item(j).toElement();

			float value = LocaleHelper::toFloat(el.attribute("value"));

			// inValue will be equal to "value" and outValue will
			// be set to the same
			el.setAttribute("outValue", value);
		}
	}
}

// Convert the negative length notes to StepNotes
void DataFile::upgrade_noteTypes()
{
	const auto notes = elementsByTagName("note");

	for (int i = 0; i < notes.size(); ++i)
	{
		auto note = notes.item(i).toElement();

		const auto noteSize = note.attribute("len").toInt();
		if (noteSize < 0)
		{
			note.setAttribute("len", DefaultTicksPerBar / 16);
			note.setAttribute("type", static_cast<int>(Note::Type::Step));
		}
	}
}

void DataFile::upgrade_fixCMTDelays()
{
	static const QMap<QString, QString> nameMap {
		{ "delay_0,01s", "delay_0.01s" },
		{ "delay_0,1s", "delay_0.1s" },
		{ "fbdelay_0,01s", "fbdelay_0.01s" },
		{ "fbdelay_0,1s", "fbdelay_0.1s" }
	};

	const auto effects = elementsByTagName("effect");

	for (int i = 0; i < effects.size(); ++i)
	{
		auto effect = effects.item(i).toElement();

		// We are only interested in LADSPA plugins
		if (effect.attribute("name") != "ladspaeffect") { continue; }

		// Fetch all attributes (LMMS) beneath the LADSPA effect so that we can check the value of the plugin attribute (XML)
		auto attributes = effect.elementsByTagName("attribute");
		for (int j = 0; j < attributes.size(); ++j)
		{
			auto attribute = attributes.item(j).toElement();

			if (attribute.attribute("name") == "plugin")
			{
				const auto attributeValue = attribute.attribute("value");

				const auto it = nameMap.constFind(attributeValue);
				if (it != nameMap.constEnd())
				{
					attribute.setAttribute("value", *it);
				}
			}
		}
	}
}


void DataFile::upgrade_extendedNoteRange()
{
	auto root = documentElement();
	UpgradeExtendedNoteRange upgradeExtendedNoteRange(root);

	upgradeExtendedNoteRange.upgrade();
}


void DataFile::upgrade_defaultTripleOscillatorHQ()
{
	QDomNodeList tripleoscillators = elementsByTagName("tripleoscillator");
	for (int i = 0; !tripleoscillators.item(i).isNull(); i++)
	{
		for (int j = 1; j <= 3; j++)
		{
			// Only set the attribute if it does not exist (default template has it but reports as 1.2.0)
			if (tripleoscillators.item(i).toElement().attribute("useWaveTable" + QString::number(j)) == "")
			{
				tripleoscillators.item(i).toElement().setAttribute("useWaveTable" + QString::number(j), 0);
			}
		}
	}
}


// Remove FX prefix from mixer and related nodes
void DataFile::upgrade_mixerRename()
{
	// Change nodename <fxmixer> to <mixer>
	QDomNodeList fxmixer = elementsByTagName("fxmixer");
	for (int i = 0; i < fxmixer.length(); ++i)
	{
		auto item = fxmixer.item(i).toElement();
		if (item.isNull())
		{
			continue;
		}
		item.setTagName("mixer");
	}

	// Change nodename <fxchannel> to <mixerchannel>
	QDomNodeList fxchannel = elementsByTagName("fxchannel");
	for (int i = 0; i < fxchannel.length(); ++i)
	{
		auto item = fxchannel.item(i).toElement();
		if (item.isNull())
		{
			continue;
		}
		item.setTagName("mixerchannel");
	}

	// Change the attribute fxch of element <instrumenttrack> to mixch
	QDomNodeList fxch = elementsByTagName("instrumenttrack");
	for (int i = 0; i < fxch.length(); ++i)
	{
		auto item = fxch.item(i).toElement();
		if (item.isNull())
		{
			continue;
		}
		if (item.hasAttribute("fxch"))
		{
			item.setAttribute("mixch", item.attribute("fxch"));
			item.removeAttribute("fxch");
		}
	}
	// Change the attribute fxch of element <sampletrack> to mixch
	fxch = elementsByTagName("sampletrack");
	for (int i = 0; i < fxch.length(); ++i)
	{
		auto item = fxch.item(i).toElement();
		if (item.isNull())
		{
			continue;
		}
		if (item.hasAttribute("fxch"))
		{
			item.setAttribute("mixch", item.attribute("fxch"));
			item.removeAttribute("fxch");
		}
	}
}


// Rename BB to pattern and TCO to clip
void DataFile::upgrade_bbTcoRename()
{
	std::vector<std::pair<const char *, const char *>> names {
		{"automationpattern", "automationclip"},
		{"bbtco", "patternclip"},
		{"pattern", "midiclip"},
		{"sampletco", "sampleclip"},
		{"bbtrack", "patterntrack"},
		{"bbtrackcontainer", "patternstore"},
	};
	// Replace names of XML tags
	for (auto name : names)
	{
		QDomNodeList elements = elementsByTagName(name.first);
		for (int i = 0; !elements.item(i).isNull(); ++i)
		{
			elements.item(i).toElement().setTagName(name.second);
		}
	}
	// Replace "Beat/Bassline" with "Pattern" in track names
	QDomNodeList elements = elementsByTagName("track");
	for (int i = 0; !elements.item(i).isNull(); ++i)
	{
		auto e = elements.item(i).toElement();
		static_assert(Track::Type::Pattern == static_cast<Track::Type>(1), "Must be type=1 for backwards compatibility");
		if (static_cast<Track::Type>(e.attribute("type").toInt()) == Track::Type::Pattern)
		{
			e.setAttribute("name", e.attribute("name").replace("Beat/Bassline", "Pattern"));
		}
	}
}


// Set LFO speed to 0.01 on projects made before sample-and-hold PR
void DataFile::upgrade_sampleAndHold()
{
	QDomNodeList elements = elementsByTagName("lfocontroller");
	for (int i = 0; i < elements.length(); ++i)
	{
		if (elements.item(i).isNull()) { continue; }
		auto e = elements.item(i).toElement();
		// Correct old random wave LFO speeds
		if (e.attribute("wave").toInt() == 6)
		{
			e.setAttribute("speed", 0.01f);
		}
	}
}


static QMap<QString, QString> buildReplacementMap()
{
	static constexpr auto loopBPMs = std::array{
		std::pair{"bassloops/briff01", "140"},
		std::pair{"bassloops/briff01", "140"},
		std::pair{"bassloops/rave_bass01", "180"},
		std::pair{"bassloops/rave_bass02", "180"},
		std::pair{"bassloops/tb303_01", "123"},
		std::pair{"bassloops/techno_bass01", "140"},
		std::pair{"bassloops/techno_bass02", "140"},
		std::pair{"bassloops/techno_synth01", "140"},
		std::pair{"bassloops/techno_synth02", "140"},
		std::pair{"bassloops/techno_synth03", "130"},
		std::pair{"bassloops/techno_synth04", "140"},
		std::pair{"beats/909beat01", "122"},
		std::pair{"beats/break01", "168"},
		std::pair{"beats/break02", "141"},
		std::pair{"beats/break03", "168"},
		std::pair{"beats/electro_beat01", "120"},
		std::pair{"beats/electro_beat02", "119"},
		std::pair{"beats/house_loop01", "142"},
		std::pair{"beats/jungle01", "168"},
		std::pair{"beats/rave_hihat01", "180"},
		std::pair{"beats/rave_hihat02", "180"},
		std::pair{"beats/rave_kick01", "180"},
		std::pair{"beats/rave_kick02", "180"},
		std::pair{"beats/rave_snare01", "180"},
		std::pair{"latin/latin_brass01", "140"},
		std::pair{"latin/latin_guitar01", "126"},
		std::pair{"latin/latin_guitar02", "140"},
		std::pair{"latin/latin_guitar03", "120"},
	};

	QMap<QString, QString> namesToNamesWithBPMsMap;

	auto insertEntry = [&namesToNamesWithBPMsMap](const QString& originalName, const QString& bpm, const QString& prefix = "", const QString& extension = "ogg")
	{
		const QString original = prefix + originalName + "." + extension;
		const QString replacement = prefix + originalName + " - " + bpm + " BPM." + extension;

		namesToNamesWithBPMsMap.insert(original, replacement);
	};

	for (const auto & loopBPM : loopBPMs)
	{
		insertEntry(loopBPM.first, loopBPM.second);
		insertEntry(loopBPM.first, loopBPM.second, "factorysample:");
	}

	return namesToNamesWithBPMsMap;
}

// Change loops' filenames in <sampleclip>s
void DataFile::upgrade_loopsRename()
{
	static const QMap<QString, QString> namesToNamesWithBPMsMap = buildReplacementMap();

	mapSrcAttributeInElementsWithResources(namesToNamesWithBPMsMap);
}

void DataFile::upgrade_midiCCIndexing()
{
	static constexpr std::array attributesToUpdate{"inputcontroller", "outputcontroller"};

	QDomNodeList elements = elementsByTagName("Midicontroller");
	for(int i = 0; i < elements.length(); i++)
	{
		if (elements.item(i).isNull()) { continue; }
		auto element = elements.item(i).toElement();
		for (const char* attrName : attributesToUpdate)
		{
			if (element.hasAttribute(attrName))
			{
				int cc = element.attribute(attrName).toInt();
				element.setAttribute(attrName, cc - 1);
			}
		}
	}
}

void DataFile::findProblematicLadspaPlugins()
{
	// This is not an upgrade but a check for potentially problematic LADSPA
	// controls. See #5738 for more details.

	const QDomNodeList ladspacontrols = elementsByTagName("ladspacontrols");

	uint numberOfProblematicPlugins = 0;

	for (int i = 0; i < ladspacontrols.size(); ++i)
	{
		const QDomElement ladspacontrol = ladspacontrols.item(i).toElement();

		const auto attributes = ladspacontrol.attributes();
		for (int j = 0; j < attributes.length(); ++j)
		{
			const auto attribute = attributes.item(j);
			const auto name = attribute.nodeName();
			if (name != "ports" && name.startsWith("port"))
			{
				++numberOfProblematicPlugins;
				break;
			}
		}
	}

	if (numberOfProblematicPlugins > 0)
	{
		const auto message = QObject::tr("The project contains %1 LADSPA plugin(s) which might have not been restored correctly! Please check the project.").arg(numberOfProblematicPlugins);
		if (gui::getGUI() != nullptr) { QMessageBox::warning(nullptr, QObject::tr("LADSPA plugins"), message); }
		else { qWarning().noquote() << message; }
	}
}

void DataFile::upgrade_fixBassLoopsTypo()
{
	static const QMap<QString, QString> replacementMap = {
		{ "bassloopes/briff01.ogg", "bassloops/briff01 - 140 BPM.ogg" },
		{ "bassloopes/rave_bass01.ogg", "bassloops/rave_bass01 - 180 BPM.ogg" },
		{ "bassloopes/rave_bass02.ogg", "bassloops/rave_bass02 - 180 BPM.ogg" },
		{ "bassloopes/tb303_01.ogg","bassloops/tb303_01 - 123 BPM.ogg" },
		{ "bassloopes/techno_bass01.ogg", "bassloops/techno_bass01 - 140 BPM.ogg" },
		{ "bassloopes/techno_bass02.ogg", "bassloops/techno_bass02 - 140 BPM.ogg" },
		{ "bassloopes/techno_synth01.ogg", "bassloops/techno_synth01 - 140 BPM.ogg" },
		{ "bassloopes/techno_synth02.ogg", "bassloops/techno_synth02 - 140 BPM.ogg" },
		{ "bassloopes/techno_synth03.ogg", "bassloops/techno_synth03 - 130 BPM.ogg" },
		{ "bassloopes/techno_synth04.ogg", "bassloops/techno_synth04 - 140 BPM.ogg" }
	};

	mapSrcAttributeInElementsWithResources(replacementMap);
}

void DataFile::upgrade()
{
	// Runs all necessary upgrade methods
	std::size_t max = std::min(static_cast<std::size_t>(m_fileVersion), UPGRADE_METHODS.size());
	std::for_each( UPGRADE_METHODS.begin() + max, UPGRADE_METHODS.end(),
		[this](UpgradeMethod um)
		{
			(this->*um)();
		}
	);

	// Bump the file version (which should be the size of the upgrade methods vector)
	m_fileVersion = UPGRADE_METHODS.size();

	// update document meta data
	documentElement().setAttribute( "version", m_fileVersion );
	documentElement().setAttribute( "type", typeName( type() ) );
	documentElement().setAttribute( "creator", "LMMS" );
	documentElement().setAttribute( "creatorversion", LMMS_VERSION );
	documentElement().setAttribute("creatorplatform", QSysInfo::kernelType());
	documentElement().setAttribute("creatorplatformtype", QSysInfo::productType());

	if( type() == Type::SongProject || type() == Type::SongProjectTemplate )
	{
		// Time-signature
		if ( !m_head.hasAttribute( "timesig_numerator" ) )
		{
			m_head.setAttribute( "timesig_numerator", 4 );
			m_head.setAttribute( "timesig_denominator", 4 );
		}

		if( !m_head.hasAttribute( "mastervol" ) )
		{
			m_head.setAttribute( "mastervol", 100 );
		}
	}
}




void DataFile::loadData( const QByteArray & _data, const QString & _sourceFile )
{
	QString errorMsg;
	int line = -1, col = -1;
	if (!lmms::setContent(*this, _data, &errorMsg, &line, &col))
	{
		// parsing failed? then try to uncompress data
		QByteArray uncompressed = qUncompress( _data );
		if( !uncompressed.isEmpty() )
		{
			if (lmms::setContent(*this, uncompressed, &errorMsg, &line, &col))
			{
				line = col = -1;
			}
		}
		if( line >= 0 && col >= 0 )
		{
			using gui::SongEditor;

			qWarning() << "at line" << line << "column" << errorMsg;
			if (gui::getGUI() != nullptr)
			{
				QMessageBox::critical( nullptr,
					SongEditor::tr( "Error in file" ),
					SongEditor::tr( "The file %1 seems to contain "
							"errors and therefore can't be "
							"loaded." ).
								arg( _sourceFile ) );
			}

			return;
		}
	}

	if (const auto error = serializationError(); !error.isEmpty()) { qWarning() << error; return; }
	QDomElement root = documentElement();
	m_type = type( root.attribute( "type" ) );
	m_head = root.elementsByTagName( "head" ).item( 0 ).toElement();

	if (!root.hasAttribute("version") || root.attribute("version")=="1.0")
	{
		// The file versioning is now a unsigned int, not maj.min, so we use
		// legacyFileVersion() to retrieve the appropriate version
		m_fileVersion = legacyFileVersion();
	}
	else
	{
		bool success;
		m_fileVersion = root.attribute( "version" ).toUInt( &success );
		if( !success ) qWarning("File Version conversion failure.");
	}

	if (root.hasAttribute("creatorversion"))
	{
		using gui::SongEditor;

		// compareType defaults to All, so it doesn't have to be set here
		ProjectVersion createdWith = root.attribute("creatorversion");
		ProjectVersion openedWith = LMMS_VERSION;

		if (createdWith.setCompareType(ProjectVersion::CompareType::Minor)
		 !=  openedWith.setCompareType(ProjectVersion::CompareType::Minor)
		 && gui::getGUI() != nullptr && root.attribute("type") == "song"
		){
			auto projectType = _sourceFile.endsWith(".mpt") ?
				SongEditor::tr("template") : SongEditor::tr("project");

			gui::TextFloat::displayMessage(
				SongEditor::tr("Version difference"),
				SongEditor::tr("This %1 was created with LMMS %2")
				.arg(projectType).arg(createdWith.getVersion()),
				embed::getIconPixmap("whatsthis", 24, 24),
				2500
			);
		}
	}

	// Perform upgrade routines
	if (m_fileVersion < UPGRADE_METHODS.size()) { upgrade(); }

	m_content = root.elementsByTagName(typeName(m_type)).item(0).toElement();
}


void findIds(const QDomElement& elem, QList<jo_id_t>& idList)
{
	if(elem.hasAttribute("id"))
	{
		idList.append(elem.attribute("id").toInt());
	}
	QDomElement child = elem.firstChildElement();
	while(!child.isNull())
	{
		findIds(child, idList);
		child = child.nextSiblingElement();
	}
}

unsigned int DataFile::legacyFileVersion()
{
	// Version of LMMs that created this project
	ProjectVersion creator =
		documentElement().attribute( "creatorversion" ).
		replace( "svn", "" );

	// Get an iterator pointing at the first upgrade we need to run (or at the end if there is no such upgrade)
	auto firstRequiredUpgrade = std::upper_bound( UPGRADE_VERSIONS.begin(), UPGRADE_VERSIONS.end(), creator );

	// Convert the iterator to an index, which is our file version (starting at 0)
	return std::distance( UPGRADE_VERSIONS.begin(), firstRequiredUpgrade );
}

} // namespace lmms
