/* Native tuning text and file IO. SPDX-License-Identifier: GPL-2.0-or-later */
#include "TuningFile.h"

#include <cmath>
#include <limits>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include "Keymap.h"
#include "Note.h"
#include "Scale.h"

namespace lmms
{
namespace
{
bool reject(QString& error, const QString& message) { error = message; return false; }

bool validName(const QString& name, QString& error)
{
	if (name.toUtf8().size() > TuningFile::MaximumTextBytes || name.trimmed().startsWith('!')
		|| name.contains('\n') || name.contains('\r') || name.contains(QChar(0)))
	{
		return reject(error, "A tuning description must fit one line and cannot start with !");
	}
	return true;
}

QStringList lines(QString text)
{
	if (text.startsWith(QChar(0xfeff))) { text.remove(0, 1); }
	text.replace("\r\n", "\n"); text.replace('\r', '\n');
	return text.split('\n');
}

QString token(const QString& line) { return line.trimmed().section(QRegularExpression("\\s+"), 0, 0); }

bool boundedText(const QString& text, QString& error)
{
	return text.toUtf8().size() <= TuningFile::MaximumTextBytes && !text.contains(QChar(0))
		? true : reject(error, "Tuning text exceeds 1 MiB or contains a null character");
}

bool readFile(const QString& path, QString& text, QString& error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) { return reject(error, file.errorString()); }
	if (file.size() > TuningFile::MaximumTextBytes) { return reject(error, "Tuning file exceeds 1 MiB"); }
	auto bytes = file.read(TuningFile::MaximumTextBytes + 1);
	if (file.error() != QFileDevice::NoError || bytes.size() > TuningFile::MaximumTextBytes) { return reject(error, "Tuning file read failed or exceeded its size bound"); }
	if (bytes.startsWith(QByteArray::fromHex("efbbbf"))) { bytes.remove(0, 3); }
	text = QString::fromUtf8(bytes);
	if (text.toUtf8() != bytes) { text = QString::fromLatin1(bytes); }
	return boundedText(text, error);
}

bool writeFile(const QString& path, const QString& text, QString& error)
{
	const auto bytes = text.toUtf8();
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
	{
		return reject(error, file.errorString());
	}
	error.clear(); return true;
}

bool parseInterval(const QString& line, Interval& result, QString& error)
{
	const auto first = token(line);
	if (first.contains('.'))
	{
		static const QRegularExpression cents("^[+-]?(?:[0-9]+\\.[0-9]*|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?");
		const auto match = cents.match(first);
		bool ok = false;
		const auto value = match.captured(0).toFloat(&ok);
		if (!match.hasMatch() || !ok || !std::isfinite(value)) { return reject(error, "Interval cents are not a finite native number"); }
		result = Interval(value);
	}
	else
	{
		static const QRegularExpression ratio("^\\s*([+]?[0-9]+)(?:\\s*/\\s*([+]?[0-9]+))?");
		const auto match = ratio.match(line);
		bool numeratorOk = false, denominatorOk = true;
		const auto numerator = match.captured(1).toULongLong(&numeratorOk);
		const auto denominator = match.captured(2).isEmpty() ? 1 : match.captured(2).toULongLong(&denominatorOk);
		if (!match.hasMatch() || !numeratorOk || !denominatorOk || numerator == 0 || denominator == 0
			|| numerator > std::numeric_limits<uint32_t>::max() || denominator > std::numeric_limits<uint32_t>::max()
			|| first.count('/') > 1
			|| line.mid(match.capturedLength()).trimmed().startsWith('/')) { return reject(error, "An interval ratio requires positive native numerator and denominator"); }
		result = Interval(static_cast<uint32_t>(numerator), static_cast<uint32_t>(denominator));
	}
	if (!std::isfinite(result.getRatio()) || result.getRatio() <= 0) { return reject(error, "Interval ratio is outside finite positive native bounds"); }
	return true;
}

QString numberText(float value) { return QString::number(value, 'g', std::numeric_limits<float>::max_digits10); }
}

bool TuningFile::validateScale(const Scale& scale, QString& error)
{
	if (!validName(scale.getDescription(), error)) { return false; }
	const auto& intervals = scale.getIntervals();
	if (intervals.empty() || intervals.size() > MaximumEntries + 1 || intervals.front().getRatio() != 1)
	{
		return reject(error, "A native scale needs implicit 1/1 and at most 4096 additional intervals");
	}
	for (const auto& interval : intervals)
	{
		if (!std::isfinite(interval.getRatio()) || interval.getRatio() <= 0) { return reject(error, "Scale contains an invalid native interval ratio"); }
	}
	error.clear(); return true;
}

bool TuningFile::validateKeymap(const Keymap& keymap, QString& error)
{
	if (!validName(keymap.getDescription(), error)) { return false; }
	if (keymap.getSize() > MaximumEntries || keymap.getFirstKey() < 0 || keymap.getLastKey() >= NumKeys
		|| keymap.getFirstKey() > keymap.getLastKey() || keymap.getMiddleKey() < 0 || keymap.getMiddleKey() >= NumKeys
		|| keymap.getBaseKey() < 0 || keymap.getBaseKey() >= NumKeys || !std::isfinite(keymap.getBaseFreq()) || keymap.getBaseFreq() <= 0)
	{
		return reject(error, "Keymap requires ordered MIDI bounds, valid middle/reference keys and finite positive frequency");
	}
	error.clear(); return true;
}

bool TuningFile::parseScale(const QString& name, const QString& text, std::shared_ptr<Scale>& result, QString& error)
{
	if (!validName(name, error) || !boundedText(text, error)) { return false; }
	std::vector<Interval> intervals{Interval(1, 1)};
	for (const auto& line : lines(text))
	{
		if (line.trimmed().isEmpty() || line.trimmed().startsWith('!')) { continue; }
		if (intervals.size() > MaximumEntries) { return reject(error, "Scale exceeds 4096 intervals"); }
		Interval interval;
		if (!parseInterval(line, interval, error)) { return false; }
		intervals.push_back(interval);
	}
	auto parsed = std::make_shared<Scale>(name, std::move(intervals));
	if (!validateScale(*parsed, error)) { return false; }
	result = std::move(parsed); return true;
}

bool TuningFile::parseKeymap(const QString& name, const QString& text, int first, int last, int middle,
	int reference, double frequency, int octaveDegree, std::shared_ptr<Keymap>& result, QString& error, bool linearFromMiddle)
{
	if (!validName(name, error) || !boundedText(text, error)) { return false; }
	if (!std::isfinite(frequency) || frequency <= 0 || frequency > std::numeric_limits<float>::max())
	{
		return reject(error, "Reference frequency must fit a finite positive native float");
	}
	std::vector<int> degrees;
	std::vector<bool> mapped;
	for (const auto& line : lines(text))
	{
		if (line.trimmed().isEmpty() || line.trimmed().startsWith('!')) { continue; }
		if (degrees.size() >= MaximumEntries) { return reject(error, "Keymap exceeds 4096 entries"); }
		const auto value = token(line);
		bool ok = value == "x";
		const int degree = ok ? -1 : value.toInt(&ok);
		if (!ok) { return reject(error, "A keyboard entry must be a native signed integer or x"); }
		degrees.push_back(degree); mapped.push_back(value != "x");
	}
	auto parsed = std::make_shared<Keymap>(name, std::move(degrees), first, last, middle, reference,
		static_cast<float>(frequency), octaveDegree, std::move(mapped), linearFromMiddle);
	if (!validateKeymap(*parsed, error)) { return false; }
	result = std::move(parsed); return true;
}

bool TuningFile::loadScale(const QString& path, std::shared_ptr<Scale>& result, QString& error)
{
	QString text;
	if (!readFile(path, text, error)) { return false; }
	QString name, intervals; int count = -1, phase = 0, received = 0;
	for (const auto& line : lines(text))
	{
		if (line.trimmed().startsWith('!')) { continue; }
		if (phase == 0) { name = line; phase = 1; continue; }
		if (line.trimmed().isEmpty()) { continue; }
		if (phase == 1)
		{
			bool ok = false; count = token(line).toInt(&ok);
			if (!ok || count < 0 || count > MaximumEntries) { return reject(error, "Invalid Scala scale degree count"); }
			phase = 2; continue;
		}
		if (received < count) { intervals += line + '\n'; ++received; }
	}
	if (phase != 2 || received != count) { return reject(error, "Scala scale is missing its header or declared intervals"); }
	return parseScale(name, intervals, result, error);
}

bool TuningFile::loadKeymap(const QString& path, std::shared_ptr<Keymap>& result, QString& error)
{
	QString text;
	if (!readFile(path, text, error)) { return false; }
	QString name = QFileInfo(path).completeBaseName(); QStringList data;
	for (const auto& line : lines(text))
	{
		const auto trimmed = line.trimmed();
		if (data.isEmpty() && trimmed.startsWith("!!")) { name = trimmed.mid(2); }
		if (trimmed.isEmpty() || trimmed.startsWith('!')) { continue; }
		data.append(line);
	}
	if (data.size() < 7) { return reject(error, "Scala keyboard mapping requires seven header fields"); }
	bool ok = false; const auto count = token(data[0]).toInt(&ok);
	if (!ok || count < 0 || count > MaximumEntries) { return reject(error, "Invalid Scala keyboard map size"); }
	int header[5];
	for (int i = 0; i < 4; ++i)
	{
		header[i] = token(data[i + 1]).toInt(&ok);
		if (!ok) { return reject(error, "Invalid Scala keyboard MIDI header field"); }
	}
	header[4] = token(data[6]).toInt(&ok);
	if (!ok) { return reject(error, "Invalid Scala formal octave degree"); }
	const double frequency = token(data[5]).toDouble(&ok);
	if (!ok) { return reject(error, "Invalid Scala reference frequency"); }
	QString mapping;
	for (int i = 0; i < count; ++i) { mapping += (i + 7 < data.size() ? data[i + 7] : QString("x")) + '\n'; }
	std::shared_ptr<Keymap> parsed;
	if (!parseKeymap(name, mapping, header[0], header[1], header[2], header[3], frequency, header[4], parsed, error, true)) { return false; }
	if (!parsed->isMapped(parsed->getBaseKey())) { return reject(error, "Scala reference key is unmapped"); }
	result = std::move(parsed); return true;
}

bool TuningFile::saveScale(const QString& path, const Scale& scale, QString& error)
{
	if (!validateScale(scale, error)) { return false; }
	QString text = "! Exported from LMMS\n" + scale.getDescription() + '\n' + QString::number(static_cast<qulonglong>(scale.getIntervals().size() - 1)) + '\n';
	for (std::size_t i = 1; i < scale.getIntervals().size(); ++i) { text += scale.getIntervals()[i].getString() + '\n'; }
	return writeFile(path, text, error);
}

bool TuningFile::saveKeymap(const QString& path, const Keymap& keymap, QString& error)
{
	if (!validateKeymap(keymap, error)) { return false; }
	if (!keymap.isMapped(keymap.getBaseKey())) { return reject(error, "Cannot export a Scala mapping with an unmapped reference key"); }
	// Legacy empty LMMS maps are absolute-key mappings. A zero Scala middle note
	// preserves those frequencies on exchange; native XML keeps the original metadata.
	const int middle = keymap.getSize() == 0 && !keymap.linearFromMiddle() ? 0 : keymap.getMiddleKey();
	QString text = "! Exported from LMMS\n!!" + keymap.getDescription() + '\n' + QString::number(static_cast<qulonglong>(keymap.getSize())) + '\n'
		+ QString::number(keymap.getFirstKey()) + '\n' + QString::number(keymap.getLastKey()) + '\n' + QString::number(middle) + '\n'
		+ QString::number(keymap.getBaseKey()) + '\n' + numberText(keymap.getBaseFreq()) + '\n' + QString::number(keymap.octaveDegree()) + '\n';
	for (std::size_t i = 0; i < keymap.getSize(); ++i) { text += (keymap.isDegreeMapped(i) ? QString::number(keymap.getMap()[i]) : QString("x")) + '\n'; }
	return writeFile(path, text, error);
}
}
