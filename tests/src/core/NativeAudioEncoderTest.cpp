/* Production audio encoder input, storage and closure contracts. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>
#include <sndfile.h>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AudioFileDevice.h"
#include "AudioFileWave.h"
#include "AudioFileFlac.h"
#include "AudioFileMP3.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "NotePlayHandle.h"
#include "ProjectRenderer.h"
#include "RenderManager.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "lmmsconfig.h"
using namespace lmms;
namespace lmms
{
class NativeAudioEncoderTestAccess
{
public:
	struct Storage { const void* samples = nullptr; std::size_t sampleCapacity = 0; const void* encoded = nullptr; std::size_t encodedCapacity = 0; };
	static Storage storage(AudioFileDevice* device, bool integer)
	{
		if (auto wave = dynamic_cast<AudioFileWave*>(device)) { return integer ? Storage{wave->m_integerBuffer.data(), wave->m_integerBuffer.capacity()} : Storage{wave->m_floatBuffer.data(), wave->m_floatBuffer.capacity()}; }
		if (auto flac = dynamic_cast<AudioFileFlac*>(device)) { return integer ? Storage{flac->m_integerBuffer.data(), flac->m_integerBuffer.capacity()} : Storage{flac->m_floatBuffer.data(), flac->m_floatBuffer.capacity()}; }
#ifdef LMMS_HAVE_MP3LAME
		if (auto mp3 = dynamic_cast<AudioFileMP3*>(device)) { return {mp3->m_interleavedData.data(), mp3->m_interleavedData.capacity(), mp3->m_encodingBuffer.data(), mp3->m_encodingBuffer.capacity()}; }
#endif
		return {};
	}
};
}
namespace
{
using Format = ProjectRenderer::ExportFileFormat;
constexpr std::array<Format, 4> formats{Format::Wave, Format::Flac, Format::Ogg, Format::MP3};
bool enabled(Format format) { return ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(format)).isAvailable(); }
QString extension(Format format) { return ProjectRenderer::getFileExtensionFromFormat(format); }
OutputSettings settings(OutputSettings::BitDepth depth = OutputSettings::BitDepth::Depth24Bit, sample_rate_t rate = 44100) { return OutputSettings(rate, 160, depth, OutputSettings::StereoMode::Stereo); }
std::unique_ptr<AudioFileDevice> create(Format format, const QString& path, bool& good, ch_cnt_t channels = 2, OutputSettings config = settings()) { return std::unique_ptr<AudioFileDevice>(ProjectRenderer::fileEncodeDevices.at(static_cast<std::size_t>(format)).m_getDevInst(path, config, channels, Engine::audioEngine(), good)); }
void codecRows() { QTest::addColumn<int>("format"); for (const auto format : formats) { if (enabled(format)) { QTest::newRow(qPrintable(extension(format))) << int(format); } } }
std::vector<SampleFrame> signal()
{
	std::vector<SampleFrame> result(44100); for (std::size_t frame = 0; frame < result.size(); ++frame) { result[frame][0] = .4f * std::sin(6.283185307179586 * 440 * frame / 44100); result[frame][1] = .2f * std::sin(6.283185307179586 * 660 * frame / 44100); } return result;
}
void feed(AudioFileDevice* device, const std::vector<SampleFrame>& pcm)
{
	device->writeBuffer(nullptr, 0); for (std::size_t offset = 0; offset < pcm.size(); offset += 512) { const auto count = std::min<std::size_t>(512, pcm.size() - offset); device->writeBuffer(pcm.data() + offset, count); device->writeBuffer(nullptr, 0); }
}
struct Measurement { bool valid = false; int channels = 0, rate = 0; sf_count_t frames = 0; std::vector<double> rms, pitch; };
Measurement decode(const QString& path)
{
	QFile file(path); if (!file.open(QIODevice::ReadOnly)) { return {}; } SF_INFO info{}; std::unique_ptr<SNDFILE, decltype(&sf_close)> audio(sf_open_fd(file.handle(), SFM_READ, &info, false), &sf_close); if (!audio || info.channels < 1 || info.channels > 8 || info.samplerate < 1 || info.frames < info.samplerate * 3 / 4) { return {}; }
	const auto count = info.samplerate / 2, begin = info.samplerate / 5; if (sf_seek(audio.get(), begin, SEEK_SET) != begin) { return {}; } std::vector<float> pcm(count * info.channels); if (sf_readf_float(audio.get(), pcm.data(), count) != count || sf_error(audio.get())) { return {}; }
	Measurement out{true, info.channels, info.samplerate, info.frames, std::vector<double>(info.channels), std::vector<double>(info.channels)};
	for (int channel = 0; channel < info.channels; ++channel) { std::vector<int> crossings; double square = 0; for (int frame = 0; frame < count; ++frame) { const auto value = pcm[frame * info.channels + channel]; if (!std::isfinite(value)) { return {}; } square += double(value) * value; if (frame && pcm[(frame - 1) * info.channels + channel] <= 0 && value > 0) { crossings.push_back(frame); } } out.rms[channel] = std::sqrt(square / count); out.pitch[channel] = crossings.size() > 1 ? double(info.samplerate) * (crossings.size() - 1) / (crossings.back() - crossings.front()) : 0.; }
	return out;
}
QByteArray read(const QString& path) { QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{}; }
class ConversionDevice final : public AudioDevice
{
public:
	ConversionDevice() : AudioDevice(4, Engine::audioEngine()) { }
	int convert(const SampleFrame* input, int_sample_t* output, bool swap) { return convertToS16(input, 2, output, swap); }
private:
	void startProcessingImpl() override { }
	void stopProcessingImpl() override { }
};
}
class NativeAudioEncoderTest : public QObject
{
	Q_OBJECT
	QTemporaryDir configuration;
	void stereo(const Measurement& pcm)
	{ QVERIFY2(pcm.valid, "Enabled native encoder output could not be independently decoded; no PCM proof"); QCOMPARE(pcm.channels, 2); QCOMPARE(pcm.rate, 44100); QVERIFY(pcm.rms[0] > .1); QVERIFY(std::abs(pcm.pitch[0] - 440) < 7); QVERIFY(std::abs(pcm.pitch[1] - 660) < 9); QVERIFY(std::abs(pcm.rms[1] / pcm.rms[0] - .5) < .2); }
private slots:
	void initTestCase() { QVERIFY(configuration.isValid()); ConfigManager::inst()->setWorkingDir(configuration.filePath("workspace")); ConfigManager::inst()->loadConfigFile(configuration.filePath("config.xml")); ConfigManager::inst()->setValue("midi", "midiautoassign", "none"); NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); QVERIFY(enabled(Format::Wave)); QVERIFY(enabled(Format::Flac)); }
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); Engine::getSong()->setExportLoop(false); Engine::getSong()->setRenderBetweenMarkers(false); Engine::getSong()->setLoopRenderCount(1); }
	void cleanup() { Engine::getSong()->stopExport(); QVERIFY(Engine::audioEngine()->restoreAudioDevice()); Engine::getSong()->clearProject(); }
	void finiteStereoRoundTripIncludesZeroFrameNoOpsAndClosure_data() { codecRows(); }
	void finiteStereoRoundTripIncludesZeroFrameNoOpsAndClosure()
	{
		QFETCH(int, format); const auto kind = static_cast<Format>(format); QTemporaryDir output; const auto path = output.filePath("native" + extension(kind)), moved = output.filePath("closed" + extension(kind)); bool good = false; auto device = create(kind, path, good); QVERIFY(good); feed(device.get(), signal()); QVERIFY(!device->hasWriteError()); QVERIFY(device->finalizeOutput()); const auto accepted = read(path); QVERIFY(device->finalizeOutput()); QCOMPARE(read(path), accepted); QVERIFY(QFile::rename(path, moved)); stereo(decode(moved)); QVERIFY(device->finalizeOutput());
	}
	void invalidInputRefusesBeforeNativeEncoding_data()
	{ QTest::addColumn<int>("format"); QTest::addColumn<int>("failure"); for (const auto kind : formats) { if (!enabled(kind)) { continue; } for (int failure = 0; failure < 5; ++failure) { QTest::newRow(qPrintable(extension(kind) + QString::number(failure))) << int(kind) << failure; } } }
	void invalidInputRefusesBeforeNativeEncoding()
	{
		QFETCH(int, format); QFETCH(int, failure); const auto kind = static_cast<Format>(format); QTemporaryDir output; bool good = false; auto device = create(kind, output.filePath("invalid" + extension(kind)), good); QVERIFY(good); SampleFrame frame(.1f); f_cnt_t count = 1; const SampleFrame* data = &frame; if (failure == 0) { data = nullptr; } if (failure == 1) { count = std::numeric_limits<f_cnt_t>::max(); } if (failure == 2) { frame[0] = std::numeric_limits<float>::quiet_NaN(); } if (failure == 3) { frame[1] = std::numeric_limits<float>::infinity(); } if (failure == 4) { count = static_cast<f_cnt_t>(std::numeric_limits<int>::max()) / 2 / sizeof(float) + 1; } device->writeBuffer(data, count); QVERIFY(device->hasWriteError()); QVERIFY(!device->finalizeOutput()); QVERIFY(!device->finalizeOutput());
	}
	void writesAfterFinalizationCannotChangeAcceptedBytes_data() { codecRows(); }
	void writesAfterFinalizationCannotChangeAcceptedBytes()
	{
		QFETCH(int, format); const auto kind = static_cast<Format>(format); QTemporaryDir output; const auto path = output.filePath("native" + extension(kind)); bool good = false; auto device = create(kind, path, good); QVERIFY(good); feed(device.get(), signal()); QVERIFY(device->finalizeOutput()); const auto accepted = read(path); QVERIFY(!accepted.isEmpty()); const SampleFrame frame(.2f); device->writeBuffer(&frame, 1); QVERIFY(device->hasWriteError()); QVERIFY(!device->finalizeOutput()); QCOMPARE(read(path), accepted); device.reset(); QCOMPARE(read(path), accepted);
	}
	void multichannelEncodingPadsBeyondTheStereoSource_data()
	{ QTest::addColumn<int>("format"); QTest::addColumn<int>("depth"); for (const auto kind : {Format::Wave, Format::Flac, Format::Ogg}) { if (!enabled(kind)) { continue; } const int count = kind == Format::Wave ? 3 : kind == Format::Flac ? 2 : 1; for (int depth = 0; depth < count; ++depth) { QTest::newRow(qPrintable(extension(kind) + QString::number(depth))) << int(kind) << depth; } } }
	void multichannelEncodingPadsBeyondTheStereoSource()
	{
		QFETCH(int, format); QFETCH(int, depth); const auto kind = static_cast<Format>(format); QTemporaryDir output; const auto path = output.filePath("four-channel" + extension(kind)); bool good = false; auto device = create(kind, path, good, 4, settings(static_cast<OutputSettings::BitDepth>(depth))); QVERIFY(good); feed(device.get(), signal()); QVERIFY(device->finalizeOutput()); const auto pcm = decode(path); QVERIFY(pcm.valid); QCOMPARE(pcm.channels, 4); QVERIFY(std::abs(pcm.pitch[0] - 440) < 7); QVERIFY(std::abs(pcm.pitch[1] - 660) < 9); QVERIFY(pcm.rms[2] < .001); QVERIFY(pcm.rms[3] < .001);
	}
	void integerConversionPadsBothEndianModes_data()
	{ QTest::addColumn<bool>("swap"); QTest::newRow("native") << false; QTest::newRow("swapped") << true; }
	void integerConversionPadsBothEndianModes()
	{
		QFETCH(bool, swap); ConversionDevice device; const std::array<SampleFrame, 2> input{SampleFrame(.25f, -.5f), SampleFrame(-.75f, .125f)}; std::array<int_sample_t, 8> output; output.fill(17); QCOMPARE(device.convert(input.data(), output.data(), swap), 16); for (const auto index : {2, 3, 6, 7}) { QCOMPARE(output[index], int_sample_t(0)); } for (const auto index : {0, 1, 4, 5}) { QVERIFY(output[index] != 0); }
	}
	void packingStorageIsReusedForEqualAndSmallerPeriods_data()
	{ QTest::addColumn<int>("format"); QTest::addColumn<bool>("integer"); for (const auto kind : {Format::Wave, Format::Flac, Format::MP3}) { if (!enabled(kind)) { continue; } QTest::newRow(qPrintable(extension(kind) + "-float")) << int(kind) << false; if (kind != Format::MP3) { QTest::newRow(qPrintable(extension(kind) + "-integer")) << int(kind) << true; } } }
	void packingStorageIsReusedForEqualAndSmallerPeriods()
	{
		QFETCH(int, format); QFETCH(bool, integer); const auto kind = static_cast<Format>(format); QTemporaryDir output; bool good = false; auto device = create(kind, output.filePath("buffers" + extension(kind)), good, 2, settings(integer ? OutputSettings::BitDepth::Depth16Bit : OutputSettings::BitDepth::Depth24Bit)); QVERIFY(good); const std::vector<SampleFrame> pcm(128, SampleFrame(.25f)); device->writeBuffer(pcm.data(), pcm.size()); const auto first = NativeAudioEncoderTestAccess::storage(device.get(), integer); QVERIFY(first.samples); QVERIFY(first.sampleCapacity >= 256); device->writeBuffer(pcm.data(), pcm.size()); device->writeBuffer(pcm.data(), 64); const auto after = NativeAudioEncoderTestAccess::storage(device.get(), integer); QCOMPARE(after.samples, first.samples); QCOMPARE(after.sampleCapacity, first.sampleCapacity); QCOMPARE(after.encoded, first.encoded); QCOMPARE(after.encodedCapacity, first.encodedCapacity); QVERIFY(device->finalizeOutput());
	}
	void failedFileInitializationRejectsWrites_data() { codecRows(); }
	void failedFileInitializationRejectsWrites()
	{
		QFETCH(int, format); const auto kind = static_cast<Format>(format); QTemporaryDir output; bool good = true; auto device = create(kind, output.filePath("missing/native" + extension(kind)), good); QVERIFY(!good); QVERIFY(device); const SampleFrame frame(.1f); device->writeBuffer(&frame, 1); QVERIFY(device->hasWriteError()); QVERIFY(!device->finalizeOutput()); QVERIFY(!device->finalizeOutput());
	}
#ifdef LMMS_HAVE_OGGVORBIS
	void oggConstructionDoesNotReseedTheProcessCRandomSequence()
	{
		std::srand(3781); const auto first = std::rand(), second = std::rand(), third = std::rand(); std::srand(3781); QCOMPARE(std::rand(), first); QTemporaryDir output; bool good = false; auto device = create(Format::Ogg, output.filePath("random.ogg"), good); QVERIFY(good); QCOMPARE(std::rand(), second); QVERIFY(device->finalizeOutput()); device.reset(); QCOMPARE(std::rand(), third);
	}
#endif
	void nativeManagerPublishesEachEnabledCodec_data() { codecRows(); }
	void nativeManagerPublishesEachEnabledCodec()
	{
		QFETCH(int, format); const auto kind = static_cast<Format>(format); const auto pcm = signal(); Engine::getSong()->setTempo(240); Engine::getSong()->setMasterVolume(100); auto track = new SampleTrack(Engine::getSong()); auto clip = static_cast<SampleClip*>(track->createClip(TimePos())); clip->setSampleBuffer(std::make_shared<SampleBuffer>(pcm, 44100)); clip->changeLength(TimePos(192)); QTemporaryDir output; const auto path = output.filePath("published" + extension(kind)); QFile prior(path); QVERIFY(prior.open(QIODevice::WriteOnly)); QCOMPARE(prior.write("old"), qint64(3)); prior.close(); RenderManager manager(settings(), kind, path); QEventLoop loop; QTimer timer; timer.setSingleShot(true); bool done = false; connect(&manager, &RenderManager::finished, &loop, [&] { done = true; loop.quit(); }); connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit); timer.start(30000); manager.renderProject(); if (!done) { loop.exec(); } if (!done) { manager.abortProcessing(); } QVERIFY(done); QVERIFY2(manager.errorString().isEmpty(), qPrintable(manager.errorString())); stereo(decode(path)); QVERIFY(QDir(output.path()).entryList({".lmms-export-*"}, QDir::Files | QDir::Hidden).isEmpty());
	}
	void invalidInitializationParametersRefuseBeforeOpeningOutput_data()
	{ QTest::addColumn<int>("format"); QTest::addColumn<int>("failure"); for (const auto kind : formats) { if (!enabled(kind)) { continue; } for (int failure = 0; failure < 3; ++failure) { QTest::newRow(qPrintable(extension(kind) + QString::number(failure))) << int(kind) << failure; } } }
	void invalidInitializationParametersRefuseBeforeOpeningOutput()
	{
		QFETCH(int, format); QFETCH(int, failure); const auto kind = static_cast<Format>(format); QTemporaryDir output; const auto path = output.filePath("existing" + extension(kind)); QFile prior(path); QVERIFY(prior.open(QIODevice::WriteOnly)); QCOMPARE(prior.write("old"), qint64(3)); prior.close(); bool good = true; const auto rate = failure == 1 ? sample_rate_t(0) : failure == 2 ? std::numeric_limits<sample_rate_t>::max() : sample_rate_t(44100); auto device = create(kind, path, good, failure == 0 ? 0 : 2, settings(OutputSettings::BitDepth::Depth24Bit, rate)); QVERIFY(!good); QVERIFY(device); QVERIFY(device->hasWriteError()); QCOMPARE(read(path), QByteArray("old")); QVERIFY(!device->finalizeOutput());
	}
};
QTEST_GUILESS_MAIN(NativeAudioEncoderTest)
#include "NativeAudioEncoderTest.moc"
