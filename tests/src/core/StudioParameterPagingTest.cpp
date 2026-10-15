/* Native studio discovery pagination. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#include <vector>
#include "AudioBusHandle.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "EffectChain.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "NotePlayHandle.h"
#include "Song.h"
#include "StudioCapabilities.h"
#include "StudioRuntime.h"

using namespace lmms;
namespace
{
const Plugin::Descriptor fixtureDescriptor{"parameter-page-fixture", "Many controls", "Native discovery fixture", "LMMS tests",
	1, Plugin::Type::Effect, nullptr, "", nullptr};
class ManyControls final : public Effect
{
	DummyEffectControls m_controls{this};
public:
	ManyControls(EffectChain* chain, int count, int nameLength = 0) : Effect(&fixtureDescriptor, chain, nullptr)
	{
		for (int index = 0; index < count; ++index)
		{
			values.push_back(std::make_unique<FloatModel>(.25f, 0.f, 1.f, .01f, this,
				"Native " + QString::number(index) + QString(nameLength, 'x')));
		}
	}
	std::vector<std::unique_ptr<FloatModel>> values;
	IntModel mode{0, 0, 512, this, "Choice"};
	EffectControls* controls() override { return &m_controls; }
protected:
	ProcessStatus processImpl(SampleFrame*, f_cnt_t) override { return ProcessStatus::Continue; }
};
QJsonObject request(const QString& id, const QString& operation, const QJsonObject& args = {})
{ return {{"id", id}, {"op", operation}, {"args", args}}; }
QJsonObject call(const QString& operation, const QJsonObject& args = {}) { return StudioCapabilities::execute(operation, args); }
QJsonObject readManifest(const QString& directory)
{
	QFile file(directory + "/session.json");
	return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
}
}

class StudioParameterPagingTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false);
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { Engine::getSong()->clearProject(); }

	void nativeControlsAreCompleteAcrossPages_data()
	{
		QTest::addColumn<QString>("operation"); QTest::newRow("parameters") << QString("parameter.list");
		QTest::newRow("instrument") << QString("instrument.inspect"); QTest::newRow("effect") << QString("effect.inspect");
	}
	void nativeControlsAreCompleteAcrossPages()
	{
		QFETCH(QString, operation);
		auto track = static_cast<InstrumentTrack*>(Track::create(Track::Type::Instrument, Engine::getSong()));
		const auto chain = track->audioBusHandle()->effects(); auto effect = new ManyControls(chain, 1025); QVERIFY(chain->appendEffect(effect));
		auto other = new ManyControls(&Engine::mixer()->mixerChannel(0)->m_fxChain, 3);
		QVERIFY(Engine::mixer()->mixerChannel(0)->m_fxChain.appendEffect(other));
		auto first = call(operation, {{"track", 0}, {"choices", false}}).value("result").toObject();
		if (operation == "effect.inspect") { first = first.value("effects").toArray().at(0).toObject(); }
		const auto defaultPage = operation == "parameter.list" ? first : first.value("parameterPage").toObject();
		QCOMPARE(defaultPage.value("limit").toInt(), 128); QCOMPARE(first.value("parameters").toArray().size(), 128); QVERIFY(defaultPage.value("hasMore").toBool());
		QSet<int> seen; int offset = 0, total = -1;
		for (int attempt = 0; attempt < 32; ++attempt)
		{
			QJsonObject args{{"track", 0}, {"choices", false}};
			args.insert(operation == "effect.inspect" ? "parameterOffset" : "offset", offset);
			args.insert(operation == "effect.inspect" ? "parameterLimit" : "limit", 113);
			if (operation != "instrument.inspect") { args.insert("effect", 0); }
			const auto response = call(operation, args); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			auto result = response.value("result").toObject();
			if (operation == "effect.inspect") { result = result.value("effects").toArray().at(0).toObject(); }
			const auto page = operation == "parameter.list" ? result : result.value("parameterPage").toObject();
			const auto items = result.value("parameters").toArray();
			if (total < 0) { total = page.value("total").toInt(); } else { QCOMPARE(page.value("total").toInt(), total); }
			QCOMPARE(page.value("offset").toInt(), offset); QVERIFY(items.size() <= 113);
			for (const auto entry : items)
			{
				const auto item = entry.toObject(); const auto id = item.value("id").toInt(); QVERIFY(!seen.contains(id)); seen.insert(id);
				QCOMPARE(item.value("location").toObject().value("track").toInt(-1), 0); QVERIFY(!item.contains("choices"));
			}
			QCOMPARE(page.value("nextOffset").toInt(), offset + items.size()); offset = page.value("nextOffset").toInt();
			if (!page.value("hasMore").toBool()) { break; }
			QVERIFY(!items.isEmpty());
		}
		QCOMPARE(seen.size(), total);
		for (const auto& model : effect->values) { QVERIFY(seen.contains(static_cast<int>(model->id()))); }
		for (const auto& model : other->values) { QVERIFY(!seen.contains(static_cast<int>(model->id()))); }
		const int handle = static_cast<int>(effect->values.back()->id());
		QVERIFY(call("parameter.set", {{"parameter", handle}, {"value", .75}}).value("ok").toBool()); QCOMPARE(effect->values.back()->value(), .75f);
	}

	void byteBoundedPagesCommitReceiptsWithoutStoppingTheSession()
	{
		QTemporaryDir directory; const auto session = directory.filePath("session"); QString error;
		StudioRuntime runtime(session); QVERIFY2(runtime.open(error), qPrintable(error));
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain;
		auto effect = new ManyControls(&chain, 2049, 8192); QVERIFY(chain.appendEffect(effect));
		const auto before = readManifest(session); QSet<int> seen; int offset = 0, total = -1; qint64 completeBytes = 0;
		for (int pageIndex = 0; pageIndex < 100; ++pageIndex)
		{
			const auto command = request("page-" + QString::number(pageIndex), "parameter.list",
				{{"channel", 0}, {"effect", 0}, {"offset", offset}, {"limit", 1024}, {"choices", false}});
			const auto response = runtime.process(command); QVERIFY2(response.value("ok").toBool(), qPrintable(response.value("error").toString()));
			QVERIFY(!runtime.stopping()); const auto result = response.value("result").toObject(); const auto items = result.value("parameters").toArray();
			QVERIFY(items.size() < 1024); QVERIFY(QJsonDocument(items).toJson(QJsonDocument::Compact).size() <= 512 * 1024);
			if (total < 0) { total = result.value("total").toInt(); } else { QCOMPARE(result.value("total").toInt(), total); }
			for (const auto item : items)
			{
				const auto id = item.toObject().value("id").toInt(); QVERIFY(!seen.contains(id)); seen.insert(id);
				completeBytes += QJsonDocument(item.toObject()).toJson(QJsonDocument::Compact).size();
			}
			if (pageIndex == 0)
			{
				const auto replay = runtime.process(command); QVERIFY(replay.value("replayed").toBool()); QCOMPARE(replay.value("result"), response.value("result"));
			}
			offset = result.value("nextOffset").toInt(); if (!result.value("hasMore").toBool()) { break; } QVERIFY(!items.isEmpty());
		}
		QCOMPARE(seen.size(), total); QVERIFY(completeBytes > 16 * 1024 * 1024);
		for (const auto& model : effect->values) { QVERIFY(seen.contains(static_cast<int>(model->id()))); }
		const auto after = readManifest(session); QCOMPARE(after.value("history"), before.value("history"));
		QCOMPARE(after.value("revision"), before.value("revision")); QVERIFY(!runtime.stopping());
		QVERIFY(runtime.process(request("invalid-page", "parameter.list", {{"limit", 0}})).value("ok").toBool() == false);
		QVERIFY(runtime.process(request("still-alive", "session.inspect")).value("ok").toBool()); QVERIFY(!runtime.stopping());
	}

	void effectPagesRetainNativeOrderingAndSelection()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain;
		for (int index = 0; index < 33; ++index) { QVERIFY(chain.appendEffect(new ManyControls(&chain, 1))); }
		int offset = 0;
		for (int page = 0; page < 3; ++page)
		{
			const auto response = call("effect.inspect", {{"channel", 0}, {"offset", offset}, {"details", false}}); QVERIFY(response.value("ok").toBool());
			const auto result = response.value("result").toObject(); const auto effects = result.value("effects").toArray();
			QCOMPARE(result.value("total").toInt(), 33); QCOMPARE(effects.size(), page == 2 ? 1 : 16);
			for (int index = 0; index < effects.size(); ++index)
			{ QCOMPARE(effects[index].toObject().value("index").toInt(), offset + index); QVERIFY(!effects[index].toObject().contains("parameters")); }
			offset = result.value("nextOffset").toInt(); QCOMPARE(result.value("hasMore").toBool(), page < 2);
		}
		const auto selected = call("effect.inspect", {{"channel", 0}, {"effect", 32}}).value("result").toObject();
		QCOMPARE(selected.value("total").toInt(), 1); QCOMPARE(selected.value("effects").toArray().at(0).toObject().value("index").toInt(), 32);
		const auto empty = call("effect.inspect", {{"channel", 0}, {"offset", 2147483647}}).value("result").toObject();
		QVERIFY(empty.value("effects").toArray().isEmpty()); QVERIFY(!empty.value("hasMore").toBool());
	}

	void choicePagesPreserveEveryNativeLabel_data()
	{
		QTest::addColumn<int>("padding"); QTest::newRow("count-bound") << 0; QTest::newRow("byte-bound") << 8192;
	}
	void choicePagesPreserveEveryNativeLabel()
	{
		QFETCH(int, padding); auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain;
		auto effect = new ManyControls(&chain, 1); QVERIFY(chain.appendEffect(effect));
		QMap<int, QString> labels; for (int index = -1; index <= 513; ++index) { labels.insert(index, "Choice " + QString::number(index) + QString(padding, 'x')); }
		effect->mode.setValueLabels(labels); const int id = static_cast<int>(effect->mode.id()); int offset = 0;
		for (int attempt = 0; attempt < 100; ++attempt)
		{
			const auto response = call("parameter.inspect", {{"parameter", id}, {"choiceOffset", offset}}); QVERIFY(response.value("ok").toBool());
			const auto result = response.value("result").toObject(); const auto choices = result.value("choices").toArray(); const auto page = result.value("choicePage").toObject();
			QCOMPARE(page.value("total").toInt(), 513); QVERIFY(choices.size() <= 128);
			QVERIFY(QJsonDocument(choices).toJson(QJsonDocument::Compact).size() <= 64 * 1024);
			for (const auto choice : choices)
			{ QCOMPARE(choice.toObject().value("value").toInt(), offset); QCOMPARE(choice.toObject().value("name").toString(), labels[offset]); ++offset; }
			QCOMPARE(page.value("nextOffset").toInt(), offset); if (!page.value("hasMore").toBool()) { break; } QVERIFY(!choices.isEmpty());
		}
		QCOMPARE(offset, 513);
		const auto concise = call("parameter.inspect", {{"parameter", id}, {"choices", false}}).value("result").toObject();
		QVERIFY(!concise.contains("choices")); QVERIFY(!concise.contains("choicePage"));
		QVERIFY(call("parameter.set", {{"parameter", id}, {"value", 512}}).value("ok").toBool()); QCOMPARE(effect->mode.value(), 512);
	}

	void invalidPageBoundsAreRefused_data()
	{
		QTest::addColumn<QString>("operation"); QTest::addColumn<QJsonObject>("args");
		QTest::newRow("negative-offset") << QString("parameter.list") << QJsonObject{{"offset", -1}};
		QTest::newRow("fractional-offset") << QString("parameter.list") << QJsonObject{{"offset", .5}};
		QTest::newRow("text-offset") << QString("parameter.list") << QJsonObject{{"offset", "0"}};
		QTest::newRow("zero-limit") << QString("parameter.list") << QJsonObject{{"limit", 0}};
		QTest::newRow("oversized-limit") << QString("parameter.list") << QJsonObject{{"limit", 1025}};
		QTest::newRow("instrument-offset") << QString("instrument.inspect") << QJsonObject{{"track", 0}, {"offset", -1}};
		QTest::newRow("instrument-limit") << QString("instrument.inspect") << QJsonObject{{"track", 0}, {"limit", 1025}};
		QTest::newRow("effect-limit") << QString("effect.inspect") << QJsonObject{{"channel", 0}, {"limit", 129}};
		QTest::newRow("nested-offset") << QString("effect.inspect") << QJsonObject{{"channel", 0}, {"parameterOffset", -1}};
		QTest::newRow("nested-limit") << QString("effect.inspect") << QJsonObject{{"channel", 0}, {"parameterLimit", 0}};
		QTest::newRow("choice-offset") << QString("parameter.inspect") << QJsonObject{{"parameter", 1}, {"choiceOffset", -1}};
		QTest::newRow("choice-limit") << QString("parameter.inspect") << QJsonObject{{"parameter", 1}, {"choiceLimit", 1025}};
	}
	void invalidPageBoundsAreRefused()
	{
		QFETCH(QString, operation); QFETCH(QJsonObject, args);
		if (operation == "instrument.inspect") { Track::create(Track::Type::Instrument, Engine::getSong()); }
		const auto before = StudioCapabilities::inspect();
		QVERIFY(!call(operation, args).value("ok").toBool()); QCOMPARE(StudioCapabilities::inspect(), before);
	}

	void oversizedChoiceReturnsAnErrorWithoutBreakingReceipts()
	{
		QTemporaryDir directory; StudioRuntime runtime(directory.filePath("session")); QString error; QVERIFY2(runtime.open(error), qPrintable(error));
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ManyControls(&chain, 1); QVERIFY(chain.appendEffect(effect));
		effect->mode.setValueLabels({{0, QString(65536, 'x')}}); const int id = static_cast<int>(effect->mode.id());
		const auto response = runtime.process(request("too-large", "parameter.inspect", {{"parameter", id}}));
		QVERIFY(!response.value("ok").toBool()); QVERIFY(response.value("error").toString().contains("page byte budget")); QVERIFY(!runtime.stopping());
		const auto concise = runtime.process(request("omit-labels", "parameter.inspect", {{"parameter", id}, {"choices", false}}));
		QVERIFY(concise.value("ok").toBool()); QVERIFY(!concise.value("result").toObject().contains("choices"));
		QVERIFY(runtime.process(request("continued", "session.inspect")).value("ok").toBool()); QVERIFY(!runtime.stopping());
	}

	void cancellationDoesNotDispatchReentrantEventsOrPublishARead()
	{
		auto& chain = Engine::mixer()->mixerChannel(0)->m_fxChain; auto effect = new ManyControls(&chain, 513); QVERIFY(chain.appendEffect(effect));
		const auto before = StudioCapabilities::inspect(); QObject timerOwner; bool eventRan = false;
		QTimer::singleShot(0, &timerOwner, [&] { eventRan = true; });
		{
			int checks = 0; FileResourceIO::Scope cancelled([&] { return ++checks >= 10; }, true);
			const auto result = call("parameter.list", {{"channel", 0}, {"effect", 0}});
			QVERIFY(!result.value("ok").toBool()); QVERIFY(result.value("error").toString().contains("cancelled")); QVERIFY(!result.contains("result"));
		}
		QVERIFY(!eventRan); QCOMPARE(StudioCapabilities::inspect(), before);
		QVERIFY(call("parameter.list", {{"channel", 0}, {"effect", 0}}).value("ok").toBool());
		QCoreApplication::processEvents(); QVERIFY(eventRan);
	}
};

QTEST_GUILESS_MAIN(StudioParameterPagingTest)
#include "StudioParameterPagingTest.moc"
