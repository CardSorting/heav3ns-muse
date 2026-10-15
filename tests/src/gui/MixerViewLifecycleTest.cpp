/* Native mixer presentation ownership tests. SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QGraphicsView>
#include <QLineEdit>
#include <QPointer>
#include <QScopeGuard>
#include <QStackedLayout>
#include <QTemporaryDir>
#include <array>
#include <memory>
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "DummyEffect.h"
#include "EffectRackView.h"
#include "EffectView.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "Mixer.h"
#include "MixerChannelView.h"
#include "MixerView.h"
#include "NotePlayHandle.h"
#include "Song.h"

using namespace lmms;
using namespace lmms::gui;

class MixerViewLifecycleTest : public QObject
{
	Q_OBJECT
	QTemporaryDir m_configuration;
	void channels(int count) { while (Engine::mixer()->numChannels() <= count) { Engine::mixer()->createChannel(); } }
	EffectRackView* rack(MixerView& view, int index)
	{
		for (const auto candidate : view.findChildren<EffectRackView*>())
		{
			if (candidate->model() == &Engine::mixer()->mixerChannel(index)->m_fxChain) { return candidate; }
		}
		return nullptr;
	}
	QStackedLayout* racks(MixerView& view)
	{
		const auto master = rack(view, 0);
		return master ? qobject_cast<QStackedLayout*>(master->parentWidget()->layout()) : nullptr;
	}
	static void drainDeferredDeletes() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); }
private slots:
	void initTestCase()
	{
		QVERIFY(m_configuration.isValid()); ConfigManager::inst()->setWorkingDir(m_configuration.filePath("workspace"));
		ConfigManager::inst()->loadConfigFile(m_configuration.filePath("config.xml")); NotePlayHandleManager::init(); Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing(); Engine::getSong()->setLoadOnLaunch(false); QVERIFY(!getGUI());
	}
	void cleanupTestCase() { Engine::destroy(); NotePlayHandleManager::free(); }
	void init() { Engine::getSong()->clearProject(); Engine::getSong()->clearErrors(); }
	void cleanup() { drainDeferredDeletes(); Engine::getSong()->clearProject(); }

	void existingChannelsHaveSelectableRacks()
	{
		channels(3); MixerView view(Engine::mixer());
		QCOMPARE(view.findChildren<EffectRackView*>().size(), 4);
		const auto layout = racks(view); QVERIFY(layout); QCOMPARE(layout->count(), 4);
		for (int index = 0; index < 4; ++index)
		{
			const auto expected = rack(view, index); QVERIFY(expected); view.setCurrentMixerChannel(index);
			QCOMPARE(layout->currentWidget(), static_cast<QWidget*>(expected));
		}
	}

	void deletionRetiresRackAndRenameSceneAndPreservesSelection_data()
	{
		QTest::addColumn<int>("selected"); QTest::addColumn<int>("removed"); QTest::addColumn<int>("expected");
		QTest::newRow("earlier-channel") << 3 << 1 << 2;
		QTest::newRow("later-channel") << 1 << 3 << 1;
		QTest::newRow("selected-middle") << 2 << 2 << 2;
		QTest::newRow("selected-last") << 3 << 3 << 2;
		QTest::newRow("master-selected") << 0 << 1 << 0;
	}
	void deletionRetiresRackAndRenameSceneAndPreservesSelection()
	{
		QFETCH(int, selected); QFETCH(int, removed); QFETCH(int, expected); channels(3); MixerView view(Engine::mixer());
		view.setCurrentMixerChannel(selected); const auto selectedView = view.currentMixerChannel();
		QPointer<MixerChannelView> oldView = view.channelView(removed); QPointer<EffectRackView> oldRack = rack(view, removed); QVERIFY(oldRack);
		const auto graphics = oldView->findChild<QGraphicsView*>(); QVERIFY(graphics); QPointer<QGraphicsScene> scene = graphics->scene(); QVERIFY(scene);
		QLineEdit* editor = nullptr;
		for (const auto item : scene->items())
		{
			if (const auto proxy = qgraphicsitem_cast<QGraphicsProxyWidget*>(item)) { editor = qobject_cast<QLineEdit*>(proxy->widget()); if (editor) { break; } }
		}
		QVERIFY(editor); editor->setText("Retired channel editor");
		view.deleteChannel(removed); QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(3));
		QVERIFY(QMetaObject::invokeMethod(editor, "editingFinished", Qt::DirectConnection));
		for (int index = 0; index < 3; ++index) { QVERIFY(Engine::mixer()->mixerChannel(index)->m_name != "Retired channel editor"); }
		QCOMPARE(view.currentMixerChannel()->channelIndex(), expected);
		if (selected != removed) { QCOMPARE(view.currentMixerChannel(), selectedView); }
		QVERIFY(oldRack->isHidden()); QVERIFY(!oldRack->isEnabled());
		const auto layout = racks(view); QVERIFY(layout); QCOMPARE(layout->count(), 3);
		drainDeferredDeletes(); QVERIFY(oldView.isNull()); QVERIFY(oldRack.isNull()); QVERIFY(scene.isNull());
		QCOMPARE(view.findChildren<EffectRackView*>().size(), 3);
		QCOMPARE(layout->currentWidget(), static_cast<QWidget*>(rack(view, expected)));
	}

	void repeatedRefreshRetiresSeparatelyParentedRacks()
	{
		channels(3); MixerView view(Engine::mixer()); const auto layout = racks(view); QVERIFY(layout);
		for (int pass = 0; pass < 4; ++pass)
		{
			QPointer<EffectRackView> previous = rack(view, 2); QVERIFY(previous);
			view.refreshDisplay(); QVERIFY(previous.isNull()); QCOMPARE(layout->count(), 4);
			QCOMPARE(view.findChildren<EffectRackView*>().size(), 4);
			view.setCurrentMixerChannel(2); QCOMPARE(layout->currentWidget(), static_cast<QWidget*>(rack(view, 2)));
		}
	}

	void refreshAfterNativeDeletionDoesNotIndexOldModels()
	{
		channels(3); MixerView view(Engine::mixer()); view.setCurrentMixerChannel(3);
		QPointer<EffectRackView> previous = rack(view, 3); QVERIFY(previous);
		QVERIFY(Engine::mixer()->deleteChannel(3)); QVERIFY(Engine::mixer()->deleteChannel(2));
		QVERIFY(previous); QVERIFY(!previous->model());
		QVERIFY(QMetaObject::invokeMethod(previous.data(), "update", Qt::DirectConnection));
		view.refreshDisplay(); QVERIFY(previous.isNull()); QCOMPARE(view.findChildren<EffectRackView*>().size(), 2);
		QCOMPARE(view.currentMixerChannel()->channelIndex(), 0); view.setCurrentMixerChannel(1); QVERIFY(rack(view, 1));
	}

	void rackParentMayBeDestroyedBeforeChannelViews()
	{
		channels(2); auto view = std::make_unique<MixerView>(Engine::mixer());
		QPointer<EffectRackView> selected = rack(*view, 1); QVERIFY(selected);
		QPointer<MixerChannelView> channel = view->channelView(1);
		delete selected->parentWidget(); QVERIFY(selected.isNull()); QVERIFY(channel);
		view.reset(); QVERIFY(channel.isNull()); QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(3));
	}

	void queuedEffectRemovalCannotDeleteReplacementViews_data()
	{
		QTest::addColumn<QString>("transition");
		QTest::newRow("reset") << QString("reset"); QTest::newRow("delete-channel") << QString("delete");
		QTest::newRow("refresh") << QString("refresh");
	}
	void queuedEffectRemovalCannotDeleteReplacementViews()
	{
		QFETCH(QString, transition); channels(2); MixerView view(Engine::mixer());
		QDomDocument document; auto original = document.createElement("effect"); original.setAttribute("name", "missing-fixture-effect"); document.appendChild(original);
		const auto chain = &Engine::mixer()->mixerChannel(1)->m_fxChain;
		chain->appendEffect(new DummyEffect(chain, original));
		const auto originalRack = rack(view, 1); QVERIFY(originalRack);
		QPointer<EffectView> previous = originalRack->findChild<EffectView*>(); QVERIFY(previous);
		previous->deletePlugin(); // Queues a request carrying the old view identity.
		if (transition == "delete") { view.deleteChannel(1); }
		else if (transition == "reset")
		{
			QVERIFY(Engine::mixer()->clearChannel(1)); chain->appendEffect(new DummyEffect(chain, original));
		}
		else { view.refreshDisplay(); }
		QVERIFY(previous.isNull());
		QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall); drainDeferredDeletes();
		if (transition == "delete") { QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(2)); }
		else
		{
			QCOMPARE(chain->effects().size(), std::size_t(1));
			QCOMPARE(rack(view, 1)->findChildren<EffectView*>().size(), 1);
		}
	}

	void bulkViewClearGuardsMasterCallbacks()
	{
		channels(3); MixerView view(Engine::mixer()); bool observed = false;
		const auto connection = connect(&Engine::mixer()->mixerChannel(0)->m_fxChain, &EffectChain::aboutToClear, this, [&] {
			observed = true; QCOMPARE(view.addNewChannel(), -1); view.clear(); view.refreshDisplay();
		}, Qt::DirectConnection);
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); });
		view.clear(); QVERIFY(observed); disconnect(connection); drainDeferredDeletes();
		QCOMPARE(Engine::mixer()->numChannels(), mix_ch_t(1)); QCOMPARE(view.findChildren<EffectRackView*>().size(), 1);
		QCOMPARE(view.currentMixerChannel()->channelIndex(), 0); QCOMPARE(view.addNewChannel(), 1);
	}

	void destructionCallbacksCannotReenterViewStructuralEdits()
	{
		channels(3); MixerView view(Engine::mixer()); view.setCurrentMixerChannel(3); const auto selected = view.currentMixerChannel();
		bool observed = false;
		const auto connection = connect(&Engine::mixer()->mixerChannel(1)->m_fxChain, &EffectChain::aboutToClear, this, [&] {
			observed = true; QCOMPARE(view.addNewChannel(), -1); view.deleteChannel(2); view.refreshDisplay();
			QCOMPARE(view.currentMixerChannel(), selected);
		}, Qt::DirectConnection);
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); });
		view.deleteChannel(1); QVERIFY(observed); drainDeferredDeletes();
		QCOMPARE(view.currentMixerChannel(), selected); QCOMPARE(selected->channelIndex(), 2);
		QCOMPARE(view.findChildren<EffectRackView*>().size(), 3); QCOMPARE(view.addNewChannel(), 3);
	}

	void effectViewsFollowNativeAndGuiMovesWithoutReplacement()
	{
		channels(1); MixerView view(Engine::mixer()); auto& chain = Engine::mixer()->mixerChannel(1)->m_fxChain;
		const auto effectsRack = rack(view, 1); QVERIFY(effectsRack); std::array<QPointer<EffectView>, 3> views;
		std::array<Effect*, 3> effects{};
		for (int index = 0; index < 3; ++index)
		{
			QDomDocument document; const auto node = document.createElement("effect"); effects[index] = new DummyEffect(&chain, node); QVERIFY(chain.appendEffect(effects[index]));
			for (const auto candidate : effectsRack->findChildren<EffectView*>()) { if (candidate->effect() == effects[index]) { views[index] = candidate; } }
			QVERIFY(views[index]);
		}
		const auto verifyOrder = [&](const QString& order) {
			QCOMPARE(effectsRack->findChildren<EffectView*>().size(), 3); int previousY = -1;
			for (int index = 0; index < 3; ++index)
			{
				const auto original = order.at(index).digitValue(); QVERIFY(views[original]); QCOMPARE(chain.effects()[index], effects[original]);
				QCOMPARE(views[original]->effect(), effects[original]); QVERIFY(views[original]->y() > previousY); previousY = views[original]->y();
			}
		};
		verifyOrder("012"); QVERIFY(chain.moveEffect(0, 2)); verifyOrder("120"); views[0]->moveUp(); verifyOrder("102"); views[1]->moveDown(); verifyOrder("012");
		views[0]->moveUp(); views[2]->moveDown(); verifyOrder("012");
		QPointer<Effect> removed = effects[1]; views[1]->deletePlugin(); QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
		QVERIFY(views[1].isNull()); QCOMPARE(chain.effects().size(), std::size_t(2)); QVERIFY(views[0]); QVERIFY(views[2]);
		drainDeferredDeletes(); QVERIFY(removed.isNull()); QVERIFY(views[0]->y() < views[2]->y());
	}

	void refusedEffectEditsKeepTheExistingViewsAndModels()
	{
		channels(1); MixerView view(Engine::mixer()); auto& chain = Engine::mixer()->mixerChannel(1)->m_fxChain;
		const auto effectsRack = rack(view, 1); QVERIFY(effectsRack);
		effectsRack->moveUp(nullptr); effectsRack->moveDown(nullptr); effectsRack->deletePlugin(nullptr);
		QDomDocument document; const auto node = document.createElement("effect"); QVERIFY(chain.appendEffect(new DummyEffect(&chain, node)));
		QPointer<EffectView> first = effectsRack->findChild<EffectView*>(); QVERIFY(first); QPointer<Effect> effect = first->effect(); bool observed = false;
		const auto connection = connect(&chain, &EffectChain::dataChanged, this, [&] {
			observed = true; const auto before = chain.effects(); effectsRack->deletePlugin(first); effectsRack->moveDown(first); effectsRack->moveUp(first);
			QVERIFY(first); QVERIFY(effect); QVERIFY(chain.effects() == before); QCOMPARE(effectsRack->findChildren<EffectView*>().size(), 2);
		}, Qt::DirectConnection);
		const auto cleanupConnection = qScopeGuard([&] { disconnect(connection); });
		QVERIFY(chain.appendEffect(new DummyEffect(&chain, node))); QVERIFY(observed); disconnect(connection);
		QVERIFY(first); QCOMPARE(first->effect(), effect.data()); effectsRack->deletePlugin(first); QVERIFY(first.isNull());
		drainDeferredDeletes(); QVERIFY(effect.isNull()); QCOMPARE(chain.effects().size(), std::size_t(1));
	}
};

QTEST_MAIN(MixerViewLifecycleTest)
#include "MixerViewLifecycleTest.moc"
