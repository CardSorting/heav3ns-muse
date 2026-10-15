#include "LocalFileMng.h"

#include <cctype>
#include <limits>
#include <stdexcept>
#include "FileResourceIO.h"

#include <QDomDocument>
#include <QFile>
#include <QLocale>


namespace lmms
{


/* New QtXml based methods */

QString LocalFileMng::readXmlString( QDomNode node , const QString& nodeName, const QString& defaultValue, bool bCanBeEmpty, bool bShouldExists, bool tinyXmlCompatMode)
{
 	QDomElement element = node.firstChildElement( nodeName );
	
	if( !node.isNull() && !element.isNull() ){
		if(  !element.text().isEmpty() ){
			return element.text();
		} else {
			if ( !bCanBeEmpty ) {
				//_WARNINGLOG( "Using default value in " + nodeName );
			}
			return defaultValue;
		}
	} else {	
		if(  bShouldExists ){
			//_WARNINGLOG( "'" + nodeName + "' node not found" );
			
		}
		return defaultValue;
	}
}

float LocalFileMng::readXmlFloat( QDomNode node , const QString& nodeName, float defaultValue, bool bCanBeEmpty, bool bShouldExists, bool tinyXmlCompatMode)
{
	QLocale c_locale = QLocale::c();
 	QDomElement element = node.firstChildElement( nodeName );
	
	if( !node.isNull() && !element.isNull() ){
		if(  !element.text().isEmpty() ){
			return c_locale.toFloat(element.text());
		} else {
			if ( !bCanBeEmpty ) {
				//_WARNINGLOG( "Using default value in " + nodeName );
			}
			return defaultValue;
		}
	} else {	
		if(  bShouldExists ){
			//_WARNINGLOG( "'" + nodeName + "' node not found" );
		}
		return defaultValue;
	}
}

int LocalFileMng::readXmlInt( QDomNode node , const QString& nodeName, int defaultValue, bool bCanBeEmpty, bool bShouldExists, bool tinyXmlCompatMode)
{
	QLocale c_locale = QLocale::c();
 	QDomElement element = node.firstChildElement( nodeName );
	
	if( !node.isNull() && !element.isNull() ){
		if(  !element.text().isEmpty() ){
			return c_locale.toInt( element.text() );
		} else {
			if ( !bCanBeEmpty ) {
				//_WARNINGLOG( "Using default value in " + nodeName );
			}
			return defaultValue;
		}
	} else {	
		if(  bShouldExists ){
			//_WARNINGLOG( "'" + nodeName + "' node not found" );
		}
		return defaultValue;
	}
}

bool LocalFileMng::readXmlBool( QDomNode node , const QString& nodeName, bool defaultValue, bool bShouldExists, bool tinyXmlCompatMode)
{
 	QDomElement element = node.firstChildElement( nodeName );
	
	if( !node.isNull() && !element.isNull() ){
		if(  !element.text().isEmpty() ){
			if( element.text() == "true"){
				return true;
			} else {
				return false;
			}
		} else {
			//_WARNINGLOG( "Using default value in " + nodeName );
			return defaultValue;
		}
	} else {	
		if(  bShouldExists ){
			//_WARNINGLOG( "'" + nodeName + "' node not found" );
		}
		return defaultValue;
	}
}


/* Convert (in-place) an XML escape sequence into a literal byte,
 * rather than the character it actually refers to.
 */
void LocalFileMng::convertFromTinyXMLString(QByteArray* bytes)
{
	if (!bytes) { throw std::invalid_argument("Missing Hydrogen XML buffer"); }
	if (bytes->size() > std::numeric_limits<int>::max() - 64) { throw std::out_of_range("Hydrogen legacy XML exceeds the Qt input range"); }
	int cursor = 0;
	while ((cursor = bytes->indexOf("&#x", cursor)) != -1)
	{
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		if (bytes->size() - cursor >= 6 && bytes->at(cursor + 5) == ';'
			&& std::isxdigit(static_cast<unsigned char>(bytes->at(cursor + 3))) && std::isxdigit(static_cast<unsigned char>(bytes->at(cursor + 4))))
		{
			const auto digit = [](unsigned char value) { const auto lower = std::tolower(value); return lower >= 'a' ? lower - 'a' + 10 : lower - '0'; };
			const auto value = 16 * digit(static_cast<unsigned char>(bytes->at(cursor + 3))) + digit(static_cast<unsigned char>(bytes->at(cursor + 4)));
			if (value < 0x80) { cursor += 6; continue; } // Let XML decode ASCII entities, including reserved punctuation.
			(*bytes)[cursor] = static_cast<char>(value);
			bytes->remove(cursor + 1, 5); ++cursor;
		}
		else { cursor += 3; }
	}
}

bool LocalFileMng::checkTinyXMLCompatMode(const QString& filename)
{
	QFile input(filename); if (!input.open(QIODevice::ReadOnly)) { return false; }
	auto beginning = input.read(256); if (beginning.startsWith(QByteArray::fromHex("efbbbf"))) { beginning.remove(0, 3); }
	return !beginning.trimmed().startsWith("<?xml");
}

QDomDocument LocalFileMng::openXmlDocument(const QString& filename)
{
	const QString requested(filename); QFile input(requested); QDomDocument document;
	if (!input.open(QIODevice::ReadOnly)) { return document; }
	if (input.size() < 0 || input.size() > std::numeric_limits<int>::max() - 64) { throw std::out_of_range("Hydrogen XML exceeds the Qt input range"); }
	QByteArray bytes;
	while (!input.atEnd())
	{
		QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
		const auto chunk = input.read(64 * 1024);
		if (chunk.isEmpty() && !input.atEnd()) { throw std::runtime_error("Could not read Hydrogen XML completely"); }
		if (chunk.size() > std::numeric_limits<int>::max() - 64 - bytes.size()) { throw std::out_of_range("Hydrogen XML grew beyond the Qt input range"); }
		bytes.append(chunk);
	}
	if (input.error() != QFile::NoError) { throw std::runtime_error("Could not read Hydrogen XML"); }
	auto beginning = bytes.left(256); if (beginning.startsWith(QByteArray::fromHex("efbbbf"))) { beginning.remove(0, 3); }
	if (!beginning.trimmed().startsWith("<?xml")) { convertFromTinyXMLString(&bytes); }
	if (!document.setContent(bytes)) { return QDomDocument{}; }
	QString error; if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
	return document;
}

} // namespace lmms
