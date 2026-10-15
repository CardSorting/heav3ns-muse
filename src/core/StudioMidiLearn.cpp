/* Bounded native parameter MIDI learn. SPDX-License-Identifier: GPL-2.0-or-later */
#include "StudioMidiLearn.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPointer>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <stdexcept>
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "AutoDetectMidiController.h"
#include "ControllerConnection.h"
#include "Engine.h"
#include "FileResourceIO.h"
#include "MidiClient.h"
#include "Song.h"
#include "StudioRecording.h"

namespace lmms
{
namespace
{
bool learning = false; // Application-thread operation, independent of GUI detectors.
[[noreturn]] void fail(const QString& message) { throw std::runtime_error(message.toStdString()); }
int option(const QJsonObject& arguments, const char* key, int fallback, int minimum, int maximum)
{
	if (!arguments.contains(key)) { return fallback; }
	const auto value = arguments.value(key); const auto number = value.toDouble();
	if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum)
	{
		fail(QString("%1 is outside the supported integer range").arg(key));
	}
	return static_cast<int>(number);
}
}

QJsonObject StudioMidiLearn::learn(AutomatableModel* model, const QJsonObject& arguments, const std::function<bool()>& cancelled)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
		|| !Engine::audioEngine() || !Engine::getSong()) { fail("Native MIDI learn requires the initialized application thread"); }
	if (learning) { fail("Another native MIDI learn operation is active"); }
	if (!model || model->thread() != QThread::currentThread()) { fail("Native MIDI learn requires a parameter on the application thread"); }
	const int timeout = option(arguments, "timeoutMs", 30000, 1, 600000);
	const int channel = option(arguments, "inputChannel", 0, 0, MidiChannelCount);
	QString selectedPort;
	if (arguments.contains("port"))
	{
		if (!arguments.value("port").isString() || arguments.value("port").toString().isEmpty()) { fail("port must be a nonempty native input name"); }
		selectedPort = arguments.value("port").toString();
	}
	const auto engine = Engine::audioEngine(); const QPointer<Song> song(Engine::getSong()); const auto client = engine->midiClient();
	const QPointer<AutomatableModel> target(model);
	const auto source = [&] { const auto guard = engine->requestChangesGuard(); return target->captureControllerState(); }();
	QElapsedTimer elapsed; elapsed.start();
	FileResourceIO::Scope learnWork([&] { return elapsed.elapsed() >= timeout || (cancelled && cancelled()); }, false);
	QScopedValueRollback<bool> active(learning, true);
	QPointer<AutoDetectMidiController> detector; bool detectorExpected = false;
	const auto retireDetector = [&] { if (detector && !detector->isRetiring()) { delete detector.data(); } };
	const auto cleanupDetector = qScopeGuard(retireDetector);
	MidiClient::InputDeliveryStatus delivery;
	QStringList candidates;
	const auto owners = [&] {
		if (!target || target->thread() != QThread::currentThread() || !song || Engine::getSong() != song.data()
			|| Engine::audioEngine() != engine || song->isLoadingProject() || !client
			|| engine->midiClient() != client || engine->isChangingMidiBackend())
		{ fail("Native MIDI learn target, project or backend changed"); }
	};
	const auto check = [&](bool resources = true, bool published = false) {
		if (!published)
		{
			if (cancelled && cancelled()) { fail("Native MIDI learn cancelled"); }
			if (elapsed.elapsed() >= timeout) { fail("Native MIDI learn timed out before binding a controller"); }
			QString error; if (resources && !FileResourceIO::check(error)) { fail(error); }
		}
		owners();
		if (!target->supportsAutomation()) { fail("Native MIDI learn requires an automatable parameter"); }
		owners();
		if (target->scaleType() != AutomatableModel::ScaleType::Linear && target->scaleType() != AutomatableModel::ScaleType::Logarithmic)
		{ fail("Native controller DSP does not support this parameter scale"); }
		if (!published && !target->controllerStateMatches(*source, false)) { fail("Native MIDI learn linked parameter or prior binding changed"); }
		StudioRecording::requireIdle();
		owners();
		if (engine->hasStoredAudioDevice()) { fail("Native MIDI learn cannot run during audio-device reservation"); }
		const auto backend = client->inputBackendStatus();
		owners();
		if (!client->hasQueuedInputDelivery() || !backend.observed || !backend.active || backend.nativeErrorCode)
		{
			fail("Native MIDI learn requires observed active queued MIDI input");
		}
		if (detectorExpected)
		{
			if (!detector || detector->isRetiring()) { fail("Native MIDI learn detector retired"); }
			const auto port = detector->midiPort(); const auto current = client->inputDeliveryStatus(port);
			if (port->client() != client || !port->isInputEnabled() || !current.registered || current.failed
				|| port->inputChannel() != channel || port->inputController() != MidiController::NONE
				|| current.failureVersion != delivery.failureVersion || current.droppedEvents != delivery.droppedEvents)
			{
				fail("Native MIDI learn input stopped or lost events");
			}
			if (!selectedPort.isEmpty() && !port->readablePorts().contains(selectedPort)) { fail("Selected native MIDI learn input disappeared"); }
			if (port->requestedReadablePorts() != candidates) { fail("Native MIDI learn input subscriptions changed"); }
		}
	};
	{
		const auto guard = engine->requestChangesGuard(); check();
		if (client->isRaw() && !selectedPort.isEmpty()) { fail("Raw MIDI input does not support a named learn port"); }
		detector = new AutoDetectMidiController(nullptr); detectorExpected = true;
		if (!detector || detector->isRetiring()) { fail("Native MIDI learn detector retired during construction"); }
		detector->reset(true); owners();
		if (!detector || detector->isRetiring()) { fail("Native MIDI learn detector retired during reset"); }
		const QPointer<MidiPort> port(detector->midiPort()); port->setInputChannel(channel); owners();
		if (!detector || detector->isRetiring() || !port) { fail("Native MIDI learn detector retired during setup"); }
		if (!client->isRaw())
		{
			const auto available = port->readablePorts();
			if (!selectedPort.isEmpty() && !available.contains(selectedPort)) { fail("Native MIDI learn input is unavailable"); }
			for (auto it = available.cbegin(); it != available.cend(); ++it)
			{
				const bool selected = selectedPort.isEmpty() || it.key() == selectedPort;
				if (selected) { candidates.append(it.key()); }
				port->subscribeReadablePort(it.key(), selected); owners();
				if (!detector || detector->isRetiring() || !port) { fail("Native MIDI learn detector retired during input subscription"); }
			}
			if (candidates.isEmpty()) { fail("Native MIDI learn has no available input ports"); }
		}
		delivery = client->inputDeliveryStatus(port.data()); check(false);
	}
	for (;;)
	{
		owners();
		{
			const auto guard = engine->requestChangesGuard(); check();
			if (detector->detection().received) { break; }
		}
		QEventLoop quantum; QTimer::singleShot(10, &quantum, &QEventLoop::quit); quantum.exec(QEventLoop::ExcludeUserInputEvents);
	}
	owners(); const auto guard = engine->requestChangesGuard(); check();
	const auto detected = detector->detection();
	if (!client->isRaw() && (detected.port.isEmpty() || !candidates.contains(detected.port)
		|| !detector->midiPort()->readablePorts().contains(detected.port)))
	{
		fail("Learned MIDI source is unavailable or lacks an unambiguous native port identity");
	}
	const QStringList learnedInputs = client->isRaw() ? QStringList{} : QStringList{detected.port};
	bool changed = false; QString error;
	if (!target->assignMidiController(detected.channel, detected.controller, true, changed, error, &learnedInputs, detected.value,
		[&](bool published) { check(false, published); }, [&] { check(false); detectorExpected = false; retireDetector(); }))
	{ fail(error); }
	owners();
	const auto committed = target->captureControllerState(); const int parameterId = static_cast<int>(target->id());
	song->setModified(); check(false, true);
	if (!target->controllerStateMatches(*committed, false)) { fail("Native learned MIDI binding changed during project notification"); }
	return {{"parameter", parameterId}, {"inputChannel", detected.channel}, {"inputController", detected.controller},
		{"port", detected.port}, {"learnedValue", detected.value}, {"elapsedMs", static_cast<double>(elapsed.elapsed())},
		{"ownership", "parameter connection"}, {"connectionVerified", false}, {"changed", changed}};
}
}
