/* Host-owned preset file acceptance and publication. SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef LMMS_VST_PRESET_STORAGE_H
#define LMMS_VST_PRESET_STORAGE_H

#include "VstPreset.h"
#include "Plugin.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <QTemporaryDir>
#include <functional>

namespace lmms
{
using VstPresetRequest = std::function<void(const QString&, const char*)>;

namespace vst_preset_storage_detail
{
inline QByteArray readPresetBytes(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error(file.errorString().toStdString()); }
	if (file.size() < 0 || file.size() > static_cast<qint64>(VstPresetMaxBytes))
	{ throw std::runtime_error("VST preset exceeds the 256 MiB file size limit"); }
	QByteArray bytes;
	QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
	QString error;
	if (!FileResourceIO::stream(file, &buffer, nullptr, file.size(), error)) { throw std::runtime_error(error.toStdString()); }
	return bytes;
}

inline Plugin::NativePresetInfo presetInfo(const QByteArray& bytes)
{
	const auto preset = decodeVstPreset(std::span<const std::uint8_t>(
		reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())));
	Plugin::NativePresetInfo info;
	info.format = preset.bank() ? "fxb" : "fxp";
	info.encoding = preset.opaque() ? "chunk" : "parameters";
	if (preset.bank() && !preset.opaque())
	{
		if (std::any_of(preset.programs.begin(), preset.programs.end(), [](const auto& program) { return program.opaque(); }))
		{ info.encoding = "programs"; }
	}
	info.name = QString::fromLatin1(preset.name.data(), static_cast<int>(preset.name.size()));
	info.pluginId = QString::number(preset.pluginId, 16).rightJustified(8, '0');
	info.pluginVersion = preset.pluginVersion; info.formatVersion = static_cast<int>(preset.version);
	info.programCount = preset.bank() || preset.opaque() ? static_cast<int>(preset.programCount) : 1;
	info.parameterCount = preset.kind == VstPresetKind::Parameters ? static_cast<int>(preset.parameters.size()) : -1;
	info.currentProgram = preset.currentProgram.value_or(-1);
	info.bytes = bytes.size();
	info.sha256 = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
	return info;
}

inline void checkPresetIO()
{
	QString error;
	if (!FileResourceIO::check(error)) { throw std::runtime_error(error.toStdString()); }
}
}

inline QByteArray decodeVstChunkState(const QString& encoded)
{
	// Legacy projects may wrap Base64 or omit its final padding. Preserve those
	// byte-equivalent forms, but never discard corruption and apply different data.
	constexpr auto maximum = ((VstPresetMaxBytes + 2) / 3) * 4;
	QByteArray text;
	std::size_t visited = 0;
	for (const auto character : encoded)
	{
		if ((visited++ % 65536) == 0) { vst_preset_storage_detail::checkPresetIO(); }
		const auto value = character.unicode();
		if (value == ' ' || value == '\t' || value == '\r' || value == '\n') { continue; }
		if (text.size() >= static_cast<qint64>(maximum))
		{ throw std::runtime_error("Saved native VST chunk exceeds the file size limit"); }
		if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
			|| (value >= '0' && value <= '9') || value == '+' || value == '/' || value == '='))
		{ throw std::runtime_error("Saved native VST chunk has invalid Base64 data"); }
		text.append(static_cast<char>(value));
	}
	const auto bytes = QByteArray::fromBase64(text);
	if (bytes.isEmpty() || bytes.size() > static_cast<qint64>(VstPresetMaxBytes))
	{ throw std::runtime_error("Invalid saved native VST chunk length"); }
	const auto canonical = bytes.toBase64();
	const auto padding = canonical.indexOf('=');
	if (text != canonical && (padding < 0 || text != canonical.left(padding)))
	{ throw std::runtime_error("Saved native VST chunk has invalid Base64 data"); }
	return bytes;
}

inline QByteArray decodeVstPresetState(const QString& encoded)
{
	// This attribute is new and always written in canonical form.
	if (static_cast<quint64>(encoded.size()) > ((VstPresetMaxBytes + 2) / 3) * 4)
	{ throw std::runtime_error("Saved native VST preset exceeds the file size limit"); }
	const auto text = encoded.toLatin1();
	const auto bytes = QByteArray::fromBase64(text);
	if (QString::fromLatin1(text) != encoded || bytes.toBase64() != text)
	{ throw std::runtime_error("Saved native VST preset has invalid Base64 data"); }
	vst_preset_storage_detail::presetInfo(bytes);
	return bytes;
}

inline Plugin::NativePresetInfo readVstPresetStorage(const QString& path, bool apply, const VstPresetRequest& request)
{
	FileResourceIO::EventSuspension events;
	using namespace vst_preset_storage_detail;
	if (path.isEmpty() || path.contains(QChar(0))) { throw std::runtime_error("VST preset requires a valid file path"); }
	QString error;
	const auto accepted = FileResourceSnapshot::captureBounded(path, VstPresetMaxBytes, error);
	if (!accepted) { throw std::runtime_error(error.toStdString()); }
	const auto info = presetInfo(readPresetBytes(accepted->path()));
	if (info.sha256 != accepted->sha256()) { throw std::runtime_error("Accepted VST preset changed while being read"); }
	checkPresetIO();
	request(accepted->path(), apply ? "load" : "inspect");
	if (!accepted->verify(error)) { throw std::runtime_error(error.toStdString()); }
	return info;
}
inline Plugin::NativePresetInfo saveVstPresetStorage(const QString& path, bool bank, const VstPresetRequest& request)
{
	FileResourceIO::EventSuspension events;
	using namespace vst_preset_storage_detail;
	if (path.isEmpty() || path.contains(QChar(0))) { throw std::runtime_error("VST preset requires a valid file path"); }
	checkPresetIO();
	QTemporaryDir temporary;
	if (!temporary.isValid()) { throw std::runtime_error(temporary.errorString().toStdString()); }
	const auto privatePath = temporary.filePath("preset.bin");
	request(privatePath, bank ? "save-bank" : "save-program");
	const auto info = presetInfo(readPresetBytes(privatePath));
	if (info.format != (bank ? "fxb" : "fxp")) { throw std::runtime_error("VST worker exported the wrong preset kind"); }
	checkPresetIO();
	QFile input(privatePath);
	QSaveFile output(path);
	output.setDirectWriteFallback(false);
	if (!input.open(QIODevice::ReadOnly)) { throw std::runtime_error(input.errorString().toStdString()); }
	if (!output.open(QIODevice::WriteOnly)) { throw std::runtime_error(output.errorString().toStdString()); }
	QString error;
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!FileResourceIO::stream(input, &output, &hash, info.bytes, error)) { throw std::runtime_error(error.toStdString()); }
	if (QString::fromLatin1(hash.result().toHex()) != info.sha256) { throw std::runtime_error("Exported VST preset changed before publication"); }
	checkPresetIO();
	if (!output.commit()) { throw std::runtime_error(output.errorString().toStdString()); }
	return info;
}

} // namespace lmms
#endif
