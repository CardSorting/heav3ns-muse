/* Actual headless VST host/worker integration. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <bit>
#include <cmath>
#include <thread>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "NotePlayHandle.h"
#include "MidiEvent.h"
#include "Song.h"
#include "VstParameterModels.h"
#include "VstParameterWire.h"
#include "VstParameterTransfer.h"
#include "VstPlugin.h"
#include "communication.h"

using namespace lmms;
namespace
{
float nativeValue(VstPlugin& plugin, int index = 0) { return VstParameterModels::decode(plugin.parameterDump()).at(index).value; }
void setValues(VstPlugin& plugin, float gain, float fault = 0.f)
{
	plugin.setParameterDump({{"param0", QString::fromStdString(vstParameterText(gain))},
		{"param1", QString::fromStdString(vstParameterText(fault))}});
}
void writeFile(const QString& path, const QByteArray& bytes)
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.flush())
	{ throw std::runtime_error("Cannot write native worker test fixture"); }
}
QByteArray readFile(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) { throw std::runtime_error("Cannot read native worker test fixture"); }
	return file.readAll();
}
}

class VstNativeWorkerTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	QByteArray m_oldPluginDirectory, m_oldTimeout, m_oldDisplay;
	bool m_hadPluginDirectory = false, m_hadTimeout = false, m_hadDisplay = false;
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid());
		m_hadPluginDirectory = qEnvironmentVariableIsSet("LMMS_PLUGIN_DIR"); m_oldPluginDirectory = qgetenv("LMMS_PLUGIN_DIR");
		m_hadTimeout = qEnvironmentVariableIsSet("LMMS_REMOTE_PLUGIN_TIMEOUT_MS"); m_oldTimeout = qgetenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS");
		m_hadDisplay = qEnvironmentVariableIsSet("DISPLAY"); m_oldDisplay = qgetenv("DISPLAY");
		qputenv("LMMS_PLUGIN_DIR", LMMS_TEST_VST_WORKER_DIR); qputenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS", "5000"); qunsetenv("DISPLAY");
		QVERIFY(QFileInfo::exists(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)));
		QVERIFY(QFileInfo::exists(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_1)));
		QVERIFY(QFileInfo::exists(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_2)));
		QVERIFY(QFileInfo::exists(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_3)));
		ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml"));
		NotePlayHandleManager::init(); Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase()
	{
		Engine::destroy(); NotePlayHandleManager::free();
		if (m_hadPluginDirectory) { qputenv("LMMS_PLUGIN_DIR", m_oldPluginDirectory); } else { qunsetenv("LMMS_PLUGIN_DIR"); }
		if (m_hadTimeout) { qputenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS", m_oldTimeout); } else { qunsetenv("LMMS_REMOTE_PLUGIN_TIMEOUT_MS"); }
		if (m_hadDisplay) { qputenv("DISPLAY", m_oldDisplay); } else { qunsetenv("DISPLAY"); }
	}

	void nativeParametersProgramsAndAudio_data()
	{
		QTest::addColumn<QString>("path"); QTest::addColumn<int>("kind");
		QTest::newRow("parameters") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0) << 0;
		QTest::newRow("chunks") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_1) << 1;
		QTest::newRow("zero-programs") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_2) << 2;
	}
	void nativeParametersProgramsAndAudio()
	{
		QFETCH(QString, path); QFETCH(int, kind); const auto failures = RemotePlugin::failureCount();
		{
			VstPlugin plugin(path); QVERIFY(!plugin.failed()); QCOMPARE(plugin.embedMethod(), QString("headless")); QVERIFY(!plugin.editor());
			const float precise = std::nextafter(.5f, 1.f); plugin.setParam(0, precise);
			QCOMPARE(std::bit_cast<std::uint32_t>(nativeValue(plugin)), std::bit_cast<std::uint32_t>(precise));
			const auto parameters = VstParameterModels::decode(plugin.parameterDump()); QCOMPARE(parameters.size(), std::size_t{2}); QVERIFY(parameters[0].name.contains(':'));
			// Render immediately after bulk restore: this requires its worker acknowledgment.
			setValues(plugin, .375f);
			const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> input(frames), output(frames);
			for (auto& frame : input) { frame[0] = .5f; frame[1] = -.5f; }
			QVERIFY(plugin.process(input.data(), output.data()));
			for (const auto& frame : output) { QCOMPARE(frame[0], .1875f); QCOMPARE(frame[1], -.1875f); }
			bool refused = false;
			std::thread foreign([&] { try { setValues(plugin, .875f); } catch (const std::runtime_error&) { refused = true; } });
			foreign.join(); QVERIFY(refused); QCOMPARE(nativeValue(plugin), .375f);
			if (kind == 2)
			{
				const auto page = plugin.programPage(0, 4); QCOMPARE(page.total, 0); QCOMPARE(page.current, -1); QVERIFY(page.programs.empty());
				QVERIFY_EXCEPTION_THROWN(plugin.setProgram(0), std::out_of_range);
			}
			else
			{
				plugin.setProgram(512); setValues(plugin, precise);
				const auto page = plugin.programPage(510, 4); QCOMPARE(page.total, 700); QCOMPARE(page.current, 512); QCOMPARE(page.programs.size(), std::size_t{4});
				QVERIFY(page.programs[2].name.contains('|')); QCOMPARE(std::bit_cast<std::uint32_t>(nativeValue(plugin)), std::bit_cast<std::uint32_t>(precise));
				QVERIFY(!plugin.programPage(7, 1).programs.front().nameAvailable); plugin.setProgram(7); QVERIFY(plugin.programPage(7, 1).programs.front().nameAvailable);
			}
		}
		QCOMPARE(RemotePlugin::failureCount(), failures);
	}

	void presetsAndXmlAcrossWorkerRestart_data()
	{
		QTest::addColumn<QString>("path"); QTest::addColumn<int>("kind");
		QTest::newRow("parameters") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0) << 0;
		QTest::newRow("chunks") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_1) << 1;
		QTest::newRow("zero-programs") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_2) << 2;
	}
	void presetsAndXmlAcrossWorkerRestart()
	{
		QFETCH(QString, path); QFETCH(int, kind); QTemporaryDir files; QVERIFY(files.isValid());
		QDomDocument document; auto settings = document.createElement("vstplugin"); document.appendChild(settings);
		const float precise = std::nextafter(.5f, 1.f); const auto failures = RemotePlugin::failureCount();
		const auto programPath = files.filePath("program.data"), bankPath = files.filePath("bank.fxp");
		{
			VstPlugin plugin(path); QVERIFY(!plugin.failed());
			if (kind != 2) { plugin.setProgram(0); setValues(plugin, .125f); plugin.setProgram(699); setValues(plugin, .875f); plugin.setProgram(512); }
			setValues(plugin, precise);
			const auto program = plugin.savePresetFile(programPath, false); QCOMPARE(program.format, QString("fxp"));
			const auto bank = plugin.savePresetFile(bankPath, true); QCOMPARE(bank.format, QString("fxb"));
			QCOMPARE(plugin.inspectPresetFile(bankPath).sha256, bank.sha256);
			QCOMPARE(std::bit_cast<std::uint32_t>(nativeValue(plugin)), std::bit_cast<std::uint32_t>(precise));
			plugin.saveSettings(document, settings); QCOMPARE(settings.hasAttribute("chunk"), kind == 1); QCOMPARE(settings.hasAttribute("nativepreset"), kind != 1);
			if (kind != 2) { plugin.setProgram(0); }
			setValues(plugin, .75f); plugin.loadPresetFile(programPath);
			QCOMPARE(std::bit_cast<std::uint32_t>(nativeValue(plugin)), std::bit_cast<std::uint32_t>(precise));
			plugin.loadPresetFile(bankPath);
			if (kind != 2)
			{
				QCOMPARE(plugin.currentProgram(), 512); plugin.setProgram(0); QCOMPARE(nativeValue(plugin), .125f);
				plugin.setProgram(699); QCOMPARE(nativeValue(plugin), .875f); plugin.setProgram(512);
			}
			const auto invalidPath = files.filePath("truncated.fxp"); writeFile(invalidPath, readFile(programPath).left(40));
			QVERIFY_EXCEPTION_THROWN(plugin.loadPresetFile(invalidPath), std::runtime_error);
			QCOMPARE(std::bit_cast<std::uint32_t>(nativeValue(plugin)), std::bit_cast<std::uint32_t>(precise));
			auto mismatch = readFile(programPath); mismatch[16] = static_cast<char>(mismatch.at(16) ^ 1); writeFile(invalidPath, mismatch);
			QVERIFY_EXCEPTION_THROWN(plugin.inspectPresetFile(invalidPath), std::runtime_error);
			QCOMPARE(std::bit_cast<std::uint32_t>(nativeValue(plugin)), std::bit_cast<std::uint32_t>(precise));
		}
		{
			VstPlugin restarted(path); QVERIFY(!restarted.failed()); restarted.loadSettings(settings);
			QCOMPARE(restarted.currentProgram(), kind == 2 ? -1 : 512);
			QCOMPARE(std::bit_cast<std::uint32_t>(nativeValue(restarted)), std::bit_cast<std::uint32_t>(precise));
			if (kind != 2) { restarted.setProgram(0); QCOMPARE(nativeValue(restarted), .125f); restarted.setProgram(699); QCOMPARE(nativeValue(restarted), .875f); }
		}
		QCOMPARE(RemotePlugin::failureCount(), failures);
	}

	void malformedBulkWritesReturnErrorsWithoutMutation_data()
	{
		QTest::addColumn<QString>("path");
		QTest::newRow("parameters") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0);
		QTest::newRow("chunks") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_1);
		QTest::newRow("zero-programs") << QString::fromUtf8(LMMS_TEST_VST_FIXTURE_2);
	}
	void malformedBulkWritesReturnErrorsWithoutMutation()
	{
		QFETCH(QString, path); VstPlugin plugin(path); QVERIFY(!plugin.failed()); setValues(plugin, .625f);
		for (int change = 0; change < 4; ++change)
		{
			RemotePluginBase::message request(IdVstSetParameterDumpChecked);
			request.addInt(2).addInt(0).addString("").addString(".125").addString(change == 0 ? "invalid" : "1").addString("");
			if (change != 1) { request.addString(change == 2 ? "nan" : ".75"); }
			if (change == 3) { request.addString("extra"); }
			{
				plugin.lock(); const auto unlock = qScopeGuard([&] { plugin.unlock(); });
				plugin.sendMessage(request); const auto reply = plugin.waitForMessage(IdVstSetParameterDumpChecked, true);
				QCOMPARE(reply.id, static_cast<int>(IdVstSetParameterDumpChecked)); QCOMPARE(reply.getQString(0), QString("0")); QVERIFY(!reply.getQString(1).isEmpty());
			}
			QVERIFY(!plugin.failed()); QCOMPARE(nativeValue(plugin), .625f); QCOMPARE(nativeValue(plugin, 1), 0.f);
		}
		setValues(plugin, .875f); QCOMPARE(nativeValue(plugin), .875f);
	}

	void advertisedChunkFailureDoesNotDegradeOrReplaceOutput()
	{
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_1)); QVERIFY(!plugin.failed()); setValues(plugin, .375f, 1.f);
		QDomDocument document; auto settings = document.createElement("vstplugin");
		QVERIFY_EXCEPTION_THROWN(plugin.saveSettings(document, settings), std::runtime_error);
		QVERIFY(!settings.hasAttribute("numparams")); QVERIFY(!settings.hasAttribute("nativepreset"));
		QTemporaryDir files; QVERIFY(files.isValid()); const auto output = files.filePath("retained.fxb"); writeFile(output, "prior file");
		QVERIFY_EXCEPTION_THROWN(plugin.savePresetFile(output, true), std::runtime_error); QCOMPARE(readFile(output), QByteArray("prior file"));
		QVERIFY(!plugin.failed()); QCOMPARE(nativeValue(plugin), .375f);
		setValues(plugin, .375f); plugin.saveSettings(document, settings); QVERIFY(settings.hasAttribute("chunk"));
	}

	void legacyParameterXmlRestoresBeforeTheNextAudioBlock()
	{
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)); QVERIFY(!plugin.failed());
		QDomDocument document; auto settings = document.createElement("vstplugin"); settings.setAttribute("numparams", 2); settings.setAttribute("program", 0);
		settings.setAttribute("param0", "0:Gain: legacy:0.375"); settings.setAttribute("param1", "1:Fault: legacy:0");
		plugin.loadSettings(settings);
		const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> input(frames), output(frames);
		for (auto& frame : input) { frame[0] = .5f; frame[1] = -.5f; }
		QVERIFY(plugin.process(input.data(), output.data()));
		for (const auto& frame : output) { QCOMPARE(frame[0], .1875f); QCOMPARE(frame[1], -.1875f); }
	}

	void denseMidiBatchSurvivesDispatchUntilNativeAudioProcessing()
	{
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)); QVERIFY(!plugin.failed());
		const auto frames = Engine::audioEngine()->framesPerPeriod();
		for (int index = 0; index < 1500; ++index)
		{ plugin.processMidiEvent(MidiEvent(MidiNoteOn, 0, 60 + index % 12, 100), (1500 - index) % frames); }
		std::vector<SampleFrame> input(frames), output(frames);
		for (auto& frame : input) { frame[0] = .5f; frame[1] = -.5f; }
		QVERIFY(plugin.process(input.data(), output.data()));
		plugin.loadParameterDisplays(); QCOMPARE(plugin.allParameterDisplays().size(), std::size_t{2});
		QCOMPARE(plugin.allParameterDisplays()[1], QString("1500"));
		for (const auto& frame : output) { QCOMPARE(frame[0], .125f); QCOMPARE(frame[1], -.125f); }
	}

	void corruptChunkIsRejectedBeforeNativeProgramSelection()
	{
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_1)); QVERIFY(!plugin.failed());
		plugin.setProgram(512); setValues(plugin, .375f);
		QDomDocument document; auto settings = document.createElement("vstplugin"); plugin.saveSettings(document, settings);
		const auto encoded = settings.attribute("chunk"); QVERIFY(!encoded.isEmpty());
		settings.setAttribute("program", 0); settings.setAttribute("chunk", encoded + "!");
		QVERIFY_EXCEPTION_THROWN(plugin.loadSettings(settings), std::runtime_error);
		QCOMPARE(plugin.currentProgram(), 512); QCOMPARE(nativeValue(plugin), .375f); QVERIFY(!plugin.failed());
		settings.setAttribute("program", 512); settings.setAttribute("chunk", " \t" + encoded + "\r\n");
		setValues(plugin, .875f); plugin.loadSettings(settings);
		QCOMPARE(plugin.currentProgram(), 512); QCOMPARE(nativeValue(plugin), .375f);
	}

	void parameterTextUsesBytesAndClearsEmptyCallbacks()
	{
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)); QVERIFY(!plugin.failed());
		plugin.loadParameterLabels(); plugin.loadParameterDisplays();
		QCOMPARE(plugin.allParameterLabels().size(), std::size_t{2}); QCOMPARE(plugin.allParameterDisplays().size(), std::size_t{2});
		QCOMPARE(plugin.allParameterLabels()[0], QString::fromUtf8("\xc2\xb5s")); QCOMPARE(plugin.allParameterLabels()[1], QString("dB"));
		QCOMPARE(plugin.allParameterDisplays()[0], QString::fromUtf8("\xc2\xb5V")); QCOMPARE(plugin.allParameterDisplays()[1], QString("0"));
		plugin.updateParameterLabel(0); plugin.updateParameterDisplay(0);
		setValues(plugin, 0.f); plugin.updateParameterLabel(0); plugin.updateParameterDisplay(0);
		QVERIFY(plugin.allParameterLabels()[0].isEmpty()); QVERIFY(plugin.allParameterDisplays()[0].isEmpty());
		QVERIFY_EXCEPTION_THROWN(plugin.updateParameterLabel(-1), std::out_of_range);
		QVERIFY_EXCEPTION_THROWN(plugin.updateParameterDisplay(2), std::out_of_range);
		bool refused = false;
		std::thread foreign([&] { try { plugin.loadParameterLabels(); } catch (const std::runtime_error&) { refused = true; } });
		foreign.join(); QVERIFY(refused);
		setValues(plugin, .25f); plugin.loadParameterLabels(); QVERIFY(!plugin.failed());
		QCOMPARE(plugin.allParameterLabels()[0], QString::fromUtf8("\xc2\xb5s"));
	}

	void malformedParameterTextRepliesRetainCache()
	{
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)); QVERIFY(!plugin.failed());
		plugin.loadParameterLabels(); plugin.loadParameterDisplays();
		const auto labels = plugin.allParameterLabels(), displays = plugin.allParameterDisplays();
		using Message = RemotePluginBase::message;
		const std::vector<Message> replies{
			Message(IdVstLoadAllParameterLabels).addString("2dBx"),
			Message(IdVstLoadAllParameterDisplays).addString("2dB0").addString("extra"),
			Message(IdVstLoadAllParameterLabels).addInt(-1).addString("Native query failed"),
			Message(IdVstUpdateParameterLabel).addString("invalid").addString("dB"),
			Message(IdVstUpdateParameterDisplay).addInt(-2).addString("0"),
			Message(IdVstUpdateParameterLabel).addInt(2).addString("dB"),
			Message(IdVstUpdateParameterDisplay).addInt(0).addString("123456789"),
			Message(IdVstUpdateParameterLabel).addInt(0).addString("dB").addInt(1)
		};
		for (const auto& reply : replies)
		{
			QVERIFY_EXCEPTION_THROWN(plugin.processMessage(reply), std::runtime_error);
			QVERIFY(plugin.allParameterLabels() == labels); QVERIFY(plugin.allParameterDisplays() == displays);
		}
		plugin.loadParameterLabels(); plugin.loadParameterDisplays(); QVERIFY(!plugin.failed());
	}

	void malformedParameterTextRequestsReturnErrors()
	{
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)); QVERIFY(!plugin.failed());
		plugin.loadParameterLabels(); plugin.loadParameterDisplays();
		using Message = RemotePluginBase::message;
		const std::vector<Message> requests{
			Message(IdVstLoadAllParameterLabels).addInt(0),
			Message(IdVstUpdateParameterDisplay).addString("not-an-index"),
			Message(IdVstUpdateParameterLabel).addInt(-1),
			Message(IdVstUpdateParameterDisplay).addInt(2),
			Message(IdVstUpdateParameterLabel).addInt(0).addInt(1)
		};
		for (const auto& request : requests)
		{
			{
				plugin.lock(); const auto unlock = qScopeGuard([&] { plugin.unlock(); });
				plugin.sendMessage(request); QVERIFY_EXCEPTION_THROWN(plugin.waitForMessage(request.id, true), std::runtime_error);
			}
			QVERIFY(!plugin.failed()); plugin.updateParameterLabel(0);
		}
		QCOMPARE(plugin.allParameterLabels()[0], QString::fromUtf8("\xc2\xb5s"));
	}

	void largeParameterLayoutRoundTripsThroughBoundedMessages()
	{
		constexpr int count = 65537;
		const auto path = QString::fromUtf8(LMMS_TEST_VST_FIXTURE_3);
		const auto failures = RemotePlugin::failureCount(); const float precise = std::nextafter(.5f, 1.f);
		QDomDocument document; auto settings = document.createElement("vstplugin"); document.appendChild(settings);
		{
			VstPlugin plugin(path); QVERIFY(!plugin.failed());
			const auto& cached = plugin.parameterDump(); QCOMPARE(cached.size(), count);
			QMap<QString, QString> values;
			std::size_t legacyBytes = 8 + 4 + std::to_string(count).size();
			for (int index = 0; index < count; ++index)
			{
				const auto text = vstParameterText(index == 1 ? 0.f : precise);
				values.insert("param" + QString::number(index), QString::fromStdString(text));
				legacyBytes += 12 + std::to_string(index).size() + text.size();
			}
			// Even the old restore's empty names exceed the shared-memory message limit.
			QVERIFY(legacyBytes > 512 * 1024);
			plugin.setParameterDump(values);
			const auto restored = VstParameterModels::decode(plugin.parameterDump()); QCOMPARE(restored.size(), std::size_t{count});
			for (int index = 0; index < count; ++index)
			{ QCOMPARE(std::bit_cast<std::uint32_t>(restored[index].value), std::bit_cast<std::uint32_t>(index == 1 ? 0.f : precise)); }
			plugin.loadParameterLabels(); plugin.loadParameterDisplays();
			QCOMPARE(plugin.allParameterLabels().size(), std::size_t{count}); QCOMPARE(plugin.allParameterDisplays().size(), std::size_t{count});
			std::size_t oldTextBytes = 12;
			for (int index = 0; index < count; ++index) { oldTextBytes += 1 + plugin.allParameterLabels()[index].toUtf8().size(); }
			QVERIFY(oldTextBytes > 512 * 1024);
			QCOMPARE(plugin.allParameterLabels().back(), QString("12345678")); QCOMPARE(plugin.allParameterDisplays().back(), QString("87654321"));
			plugin.setParam(count - 1, .875f);
			plugin.updateParameterLabel(count - 1); plugin.updateParameterDisplay(count - 1);
			const auto previousDump = cached; const auto previousLabels = plugin.allParameterLabels();
			{
				int checks = 0; FileResourceIO::Scope cancellation([&] { return ++checks >= 3; });
				QVERIFY_EXCEPTION_THROWN(plugin.parameterDump(), std::runtime_error);
			}
			QCOMPARE(cached, previousDump);
			{
				int checks = 0; FileResourceIO::Scope cancellation([&] { return ++checks >= 3; });
				QVERIFY_EXCEPTION_THROWN(plugin.loadParameterLabels(), std::runtime_error);
			}
			QVERIFY(plugin.allParameterLabels() == previousLabels); QVERIFY(!plugin.failed());
			const auto afterScalar = VstParameterModels::decode(plugin.parameterDump()); QCOMPARE(afterScalar.back().value, .875f);
			plugin.saveSettings(document, settings); QVERIFY(settings.hasAttribute("nativepreset"));
			const auto frames = Engine::audioEngine()->framesPerPeriod(); std::vector<SampleFrame> input(frames), output(frames);
			for (auto& frame : input) { frame[0] = .5f; frame[1] = -.5f; }
			QVERIFY(plugin.process(input.data(), output.data()));
			for (const auto& frame : output) { QCOMPARE(frame[0], .5f * precise); QCOMPARE(frame[1], -.5f * precise); }
		}
		{
			VstPlugin restarted(path); QVERIFY(!restarted.failed()); restarted.loadSettings(settings);
			const auto values = VstParameterModels::decode(restarted.parameterDump()); QCOMPARE(values.size(), std::size_t{count});
			for (int index = 0; index < count; ++index)
			{ QCOMPARE(values[index].value, index == 1 ? 0.f : index == count - 1 ? .875f : precise); }
		}
		QCOMPARE(RemotePlugin::failureCount(), failures);
	}

	void stagedParameterRestoresRejectIncompleteOrMalformedRequests()
	{
		using Message = RemotePluginBase::message;
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)); QVERIFY(!plugin.failed()); setValues(plugin, .625f);
		const auto exchange = [&](const Message& request) {
			plugin.lock(); const auto unlock = qScopeGuard([&] { plugin.unlock(); });
			plugin.sendMessage(request); return plugin.waitForMessage(IdVstParameterRestore);
		};
		const auto begin = Message(IdVstParameterRestore).addString("begin").addInt(2);
		const auto first = Message(IdVstParameterRestore).addString("append").addInt(0).addInt(1).addString(".25");
		const auto commit = Message(IdVstParameterRestore).addString("commit");
		const std::vector<Message> invalid{
			first, commit,
			Message(IdVstParameterRestore).addString("append").addInt(1).addInt(1).addString("nan"),
			Message(IdVstParameterRestore).addString("append").addInt(1).addInt(1),
			Message(IdVstParameterRestore).addString("append").addInt(1).addInt(1).addString(".75").addString("extra"),
			Message(IdVstParameterRestore).addString("append").addString("bad").addInt(1).addString(".75"),
			Message(IdVstParameterRestore).addString("append").addInt(1).addInt(257),
			Message(IdVstParameterRestore).addString("begin").addInt(3),
			Message(IdVstParameterRestore).addString("commit").addInt(0),
			Message(IdVstParameterRestore).addString("unknown")
		};
		for (const auto& request : invalid)
		{
			QCOMPARE(exchange(begin).getString(), std::string("1")); QCOMPARE(exchange(first).getString(), std::string("1"));
			QCOMPARE(nativeValue(plugin), .625f); // Accepted pages have not called a setter.
			const auto refused = exchange(request); QCOMPARE(refused.getString(), std::string("0")); QVERIFY(!refused.getString(1).empty());
			QCOMPARE(exchange(commit).getString(), std::string("0")); QCOMPARE(nativeValue(plugin), .625f); QVERIFY(!plugin.failed());
		}
		QCOMPARE(exchange(begin).getString(), std::string("1")); QCOMPARE(exchange(first).getString(), std::string("1"));
		QCOMPARE(exchange(Message(IdVstParameterRestore).addString("abort")).getString(), std::string("1"));
		QCOMPARE(exchange(commit).getString(), std::string("0")); QCOMPARE(nativeValue(plugin), .625f);
		{
			// Stop after the complete page has been staged, before the commit request.
			int checks = 0; FileResourceIO::Scope cancellation([&] { return ++checks >= 5; });
			QVERIFY_EXCEPTION_THROWN(setValues(plugin, .25f), std::runtime_error);
		}
		QCOMPARE(nativeValue(plugin), .625f); QVERIFY(!plugin.failed());
		setValues(plugin, .875f); QCOMPARE(nativeValue(plugin), .875f);
	}

	void malformedParameterPageRequestsLeaveTheWorkerUsable()
	{
		using Message = RemotePluginBase::message;
		VstPlugin plugin(QString::fromUtf8(LMMS_TEST_VST_FIXTURE_0)); QVERIFY(!plugin.failed());
		const std::vector<Message> requests{
			Message(IdVstParameterPage), Message(IdVstParameterPage).addInt(0).addInt(257),
			Message(IdVstParameterPage).addInt(-1).addInt(256), Message(IdVstParameterPage).addInt(3).addInt(256),
			Message(IdVstParameterPage).addString("bad").addInt(256), Message(IdVstParameterPage).addInt(0).addInt(256).addInt(0),
			Message(IdVstParameterTextPage).addInt(0).addInt(256),
			Message(IdVstParameterTextPage).addInt(0).addInt(256).addString("unknown"),
			Message(IdVstParameterTextPage).addInt(0).addInt(0).addString("labels")
		};
		for (const auto& request : requests)
		{
			plugin.lock(); const auto unlock = qScopeGuard([&] { plugin.unlock(); });
			plugin.sendMessage(request); const auto reply = plugin.waitForMessage(request.id);
			QCOMPARE(reply.id, request.id); QCOMPARE(reply.getString(), std::string("0")); QVERIFY(!reply.getString(1).empty()); QVERIFY(!plugin.failed());
		}
		QCOMPARE(nativeValue(plugin), .25f); plugin.loadParameterLabels(); plugin.loadParameterDisplays();
		const auto& cached = plugin.parameterDump(); const auto previous = cached;
		const auto labels = plugin.allParameterLabels(), displays = plugin.allParameterDisplays();
		setValues(plugin, 0.f);
		for (int query = 0; query < 3; ++query)
		{
			// Cancel after the final page, immediately before publishing the new cache.
			int checks = 0; FileResourceIO::Scope cancellation([&] { return ++checks >= 3; });
			const auto read = [&] {
				if (query == 0) { plugin.parameterDump(); }
				else if (query == 1) { plugin.loadParameterLabels(); }
				else { plugin.loadParameterDisplays(); }
			};
			QVERIFY_EXCEPTION_THROWN(read(), std::runtime_error);
			QCOMPARE(cached, previous); QVERIFY(plugin.allParameterLabels() == labels); QVERIFY(plugin.allParameterDisplays() == displays);
		}
		QCOMPARE(nativeValue(plugin), 0.f); plugin.loadParameterLabels(); plugin.loadParameterDisplays();
		QVERIFY(plugin.allParameterLabels().front().isEmpty()); QVERIFY(plugin.allParameterDisplays().front().isEmpty()); QVERIFY(!plugin.failed());
	}

	void malformedBinaryIsRefusedBeforeStartingWorker()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto path = directory.filePath("broken.dll");
		const auto failures = RemotePlugin::failureCount();
		QByteArray badOffset(64, '\0'); badOffset[0] = 'M'; badOffset[1] = 'Z';
		for (int index = 0x3c; index < 64; ++index) { badOffset[index] = char(0xff); }
		for (const auto& bytes : {QByteArray(), QByteArray("MZ"), badOffset})
		{
			writeFile(path, bytes); VstPlugin plugin(path); QVERIFY(plugin.failed());
		}
		VstPlugin missing(directory.filePath("missing.dll")); QVERIFY(missing.failed());
		QCOMPARE(RemotePlugin::failureCount(), failures);
	}
};

QTEST_GUILESS_MAIN(VstNativeWorkerTest)
#include "VstNativeWorkerTest.moc"
