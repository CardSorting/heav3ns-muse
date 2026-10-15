/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QtTest>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QPointer>
#include <QTemporaryDir>
#include <thread>
#include <array>
#include <stdexcept>
#include <utility>
#include <QThread>
#include "DataFile.h"
#include "FileResourceIO.h"
#include "FileResourceSnapshot.h"
#include "GigResourceFixture.h"

namespace lmms
{

class FileResourceSnapshotTest : public QObject
{
	Q_OBJECT
	static bool write(const QString& path, const QByteArray& bytes)
	{
		QFile file(path); return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
	}
private slots:
	void acceptedReadsDistinguishNodesAttributesDocumentsAndThreads()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("same.wav"); QString error;
		QVERIFY(write(source, "first")); auto first = FileResourceSnapshot::capture(source, error); QVERIFY(first);
		QVERIFY(write(source, "later")); auto later = FileResourceSnapshot::capture(source, error); QVERIFY(later);
		QVERIFY(write(source, "third"));
		DataFile document(DataFile::Type::JournalData), other(DataFile::Type::JournalData);
		auto a = document.createElement("tripleoscillator"), b = document.createElement("audiofileprocessor");
		auto foreign = other.createElement("audiofileprocessor");
		a.setAttribute("userwavefile0", source); a.setAttribute("userwavefile1", source); b.setAttribute("src", source); foreign.setAttribute("src", source);
		{
			DataFile::ResourceSourceScope save(document);
			DataFile::registerResourceSnapshot(document, a, "userwavefile0", first);
			DataFile::registerResourceSnapshot(document, a, "userwavefile1", later);
			DataFile::registerResourceSnapshot(document, b, "src", later);
		}
		{
			DataFile::ResourceReadScope read(document);
			DataFile::ResourceReferenceScope outer(a, "userwavefile0");
			QCOMPARE(FileResourceSnapshot::readableSourcePath(source), first->path());
			auto copy = FileResourceSnapshot::capture(source, error); QVERIFY(copy); QCOMPARE(copy->sha256(), first->sha256()); QVERIFY(copy->path() != first->path());
			for (const auto& reference : {std::make_pair(a, QString("userwavefile1")), std::make_pair(b, QString("src"))})
			{
				DataFile::ResourceReferenceScope inner(reference.first, reference.second);
				auto nested = FileResourceSnapshot::capture(source, error); QVERIFY(nested); QCOMPARE(nested->sha256(), later->sha256());
			}
			QCOMPARE(FileResourceSnapshot::readableSourcePath(source), first->path());
			{
				DataFile::ResourceReferenceScope unbound(foreign, "src"); QCOMPARE(FileResourceSnapshot::readableSourcePath(source), source);
			}
			{
				DataFile::ResourceReadScope different(other); DataFile::ResourceReferenceScope unbound(a, "userwavefile0");
				QCOMPARE(FileResourceSnapshot::readableSourcePath(source), source);
			}
			bool isolated = false;
			std::thread worker([&] { isolated = FileResourceSnapshot::readableSourcePath(source) == source; }); worker.join(); QVERIFY(isolated);
			try { DataFile::ResourceReferenceScope inner(b, "src"); throw std::runtime_error("fixture"); } catch (const std::runtime_error&) {}
			QCOMPARE(FileResourceSnapshot::readableSourcePath(source), first->path());
		}
		DataFile::ResourceReferenceScope ordinary(a, "userwavefile0");
		QCOMPARE(FileResourceSnapshot::readableSourcePath(source), source);
		auto disk = FileResourceSnapshot::capture(source, error); QVERIFY(disk); QVERIFY(disk->sha256() != first->sha256() && disk->sha256() != later->sha256());
	}

	void changedReferencesAndDamagedAcceptedFilesNeverFallBackToTheOriginal_data()
	{
		QTest::addColumn<int>("fault");
		QTest::newRow("changed-dom") << 0; QTest::newRow("changed-accepted-bytes") << 1;
		QTest::newRow("missing-accepted-file") << 2; QTest::newRow("cancelled-verification") << 3;
	}
	void changedReferencesAndDamagedAcceptedFilesNeverFallBackToTheOriginal()
	{
		QFETCH(int, fault); QTemporaryDir directory; const auto source = directory.filePath("original.pat"); QString error;
		QVERIFY(write(source, "first")); auto accepted = FileResourceSnapshot::capture(source, error); QVERIFY(accepted);
		DataFile document(DataFile::Type::JournalData); auto node = document.createElement("patman"); node.setAttribute("src", source);
		{ DataFile::ResourceSourceScope save(document); DataFile::registerResourceSnapshot(document, node, "src", accepted); }
		QVERIFY(write(source, "newer"));
		if (fault == 0) { node.setAttribute("src", directory.filePath("different.pat")); }
		if (fault == 1) { QVERIFY(write(accepted->path(), "other")); }
		if (fault == 2) { QVERIFY(QFile::remove(accepted->path())); }
		DataFile::ResourceReadScope read(document); FileResourceIO::Scope work([&] { return fault == 3; });
		QVERIFY_EXCEPTION_THROWN(DataFile::ResourceReferenceScope(node, "src"), std::runtime_error);
		QCOMPARE(FileResourceSnapshot::readableSourcePath(source), source);
	}

	void acceptedBankCopiesOwnEveryGigMemberAndSurviveSourceRelease_data()
	{
		QTest::addColumn<bool>("gig"); QTest::newRow("single-bank") << false; QTest::newRow("gig-with-convolution") << true;
	}
	void acceptedBankCopiesOwnEveryGigMemberAndSurviveSourceRelease()
	{
		using namespace test::gigResource;
		QFETCH(bool, gig); QTemporaryDir directory; const auto source = directory.filePath(gig ? "bank.gig" : "bank.sf2"); QString error;
		QVERIFY(write(source, gig ? root({"pool.gx01"}, true) : QByteArray("single bank")));
		if (gig) { QVERIFY(write(directory.filePath("pool.gx01"), companion())); QVERIFY(write(directory.filePath("bank.gx99"), companion("impulse"))); }
		auto accepted = gig ? FileResourceSnapshot::captureGig(source, error) : FileResourceSnapshot::capture(source, error); QVERIFY2(accepted, qPrintable(error));
		QVERIFY(QFile::remove(source));
		if (gig) { QVERIFY(QFile::remove(directory.filePath("pool.gx01"))); QVERIFY(QFile::remove(directory.filePath("bank.gx99"))); }
		std::shared_ptr<FileResourceSnapshot> copy;
		{
			FileResourceSnapshot::ReadScope read(source, accepted);
			copy = gig ? FileResourceSnapshot::captureGig(source, error) : FileResourceSnapshot::capture(source, error);
			QVERIFY2(copy, qPrintable(error)); QVERIFY(copy->path() != accepted->path()); QVERIFY(accepted->verifyCopy(copy->path(), error));
			QCOMPARE(copy->groupDigest(), accepted->groupDigest());
		}
		const auto previous = accepted->path(); QVERIFY(accepted->remove()); accepted.reset(); QVERIFY(!QFile::exists(previous));
		QVERIFY(copy->verify(error));
	}

	void gigCompanionDamageDuringAnAcceptedReadCannotPublishANewBank()
	{
		using namespace test::gigResource;
		QTemporaryDir directory; const auto source = directory.filePath("bank.gig"); QString error;
		QVERIFY(write(source, root({"pool.gx01"}))); QVERIFY(write(directory.filePath("pool.gx01"), companion("first")));
		auto accepted = FileResourceSnapshot::captureGig(source, error); QVERIFY(accepted);
		FileResourceSnapshot::ReadScope read(source, accepted);
		QVERIFY(write(QFileInfo(accepted->path()).absoluteDir().filePath("pool.gx01"), companion("other")));
		QVERIFY(!FileResourceSnapshot::captureGig(source, error)); QVERIFY(!error.isEmpty());
		QVERIFY_EXCEPTION_THROWN(FileResourceSnapshot::ReadScope(source, accepted), std::runtime_error);
		QCOMPARE(FileResourceSnapshot::readableSourcePath(source), accepted->path());
	}

	void gigDeclaredLayoutSurvivesOriginalDeletionAndDetectsEveryMember_data()
	{
		QTest::addColumn<bool>("rootBigEndian"); QTest::addColumn<bool>("companionBigEndian");
		QTest::newRow("little-root-little-pool") << false << false;
		QTest::newRow("little-root-big-pool") << false << true;
		QTest::newRow("big-root-little-pool") << true << false;
		QTest::newRow("big-root-big-pool") << true << true;
	}

	void gigDeclaredLayoutSurvivesOriginalDeletionAndDetectsEveryMember()
	{
		using namespace test::gigResource;
		QFETCH(bool, rootBigEndian); QFETCH(bool, companionBigEndian);
		QTemporaryDir directory; QVERIFY(directory.isValid());
		QVERIFY(QDir().mkpath(directory.filePath("samples")));
		const auto unicode = QString("samples/") + QChar(0x97f3) + ".gx02";
		const auto source = directory.filePath("original.name.gig");
		QVERIFY(write(source, root({unicode, "different.gx01"}, true, rootBigEndian)));
		QVERIFY(write(directory.filePath(unicode), companion("second", id(), companionBigEndian)));
		QVERIFY(write(directory.filePath("different.gx01"), companion("first", id(), companionBigEndian)));
		QVERIFY(write(directory.filePath("original.name.gx99"), companion("convolution", id(), companionBigEndian)));
		QString error; auto snapshot = FileResourceSnapshot::captureGig(source, error); QVERIFY2(snapshot, qPrintable(error));
		QVERIFY(snapshot->isGroup()); QCOMPARE(snapshot->members().size(), 4);
		QCOMPARE(QFileInfo(snapshot->path()).fileName(), QString("original.name.gig"));
		QCOMPARE(snapshot->members().at(1).name, QString("different.gx01"));
		const auto digest = snapshot->groupDigest(); QVERIFY(digest != snapshot->sha256());
		for (const auto& member : snapshot->members()) { QVERIFY(QFile::remove(directory.filePath(member.name))); }
		QVERIFY(snapshot->verify(error));
		const auto destination = directory.filePath("copied"); QVERIFY(snapshot->copyGroup(destination, error));
		QVERIFY(snapshot->verifyCopy(QDir(destination).filePath("original.name.gig"), error));
		for (const auto& member : snapshot->members())
		{
			const auto copy = QDir(destination).filePath(member.name);
			QFile original(copy); QVERIFY(original.open(QIODevice::ReadOnly)); const auto bytes = original.readAll(); original.close();
			QVERIFY(write(copy, QByteArray(bytes.size(), 'x'))); QVERIFY(!snapshot->verifyCopy(QDir(destination).filePath("original.name.gig"), error));
			QVERIFY(write(copy, bytes)); QVERIFY(snapshot->verifyCopy(QDir(destination).filePath("original.name.gig"), error));
		}
		QCOMPARE(snapshot->groupDigest(), digest);
		const auto privateDirectory = QFileInfo(snapshot->path()).absolutePath();
		snapshot.reset(); QVERIFY(!QFileInfo::exists(privateDirectory));
	}

	void gigMissingUnsafeCollidingAndMismatchedCompanionsAreRejected_data()
	{
		QTest::addColumn<QString>("name"); QTest::addColumn<int>("mode");
		QTest::newRow("missing") << QString("missing.gx01") << 0;
		QTest::newRow("parent-traversal") << QString("../outside.gx01") << 0;
		QTest::newRow("absolute") << QString("/outside.gx01") << 0;
		QTest::newRow("windows-drive") << QString("C:/outside.gx01") << 0;
		QTest::newRow("backslash") << QString("samples\\outside.gx01") << 0;
		QTest::newRow("root-collision") << QString("BANK.GIG") << 0;
		QTest::newRow("member-collision") << QString("sample.gx01") << 1;
		QTest::newRow("identifier-mismatch") << QString("sample.gx01") << 2;
		QTest::newRow("missing-identifier") << QString("sample.gx01") << 3;
		QTest::newRow("truncated-table") << QString("sample.gx01") << 4;
		QTest::newRow("invalid-utf8") << QString("sample.gx01") << 5;
	}

	void gigMissingUnsafeCollidingAndMismatchedCompanionsAreRejected()
	{
		using namespace test::gigResource;
		QFETCH(QString, name); QFETCH(int, mode); QTemporaryDir directory; QVERIFY(directory.isValid());
		QStringList names{name}; if (mode == 1) { names.append("SAMPLE.gx01"); }
		auto bytes = root(names);
		if (mode == 4) { bytes.chop(1); }
		if (mode == 5) { const auto index = bytes.indexOf("sample.gx01"); QVERIFY(index >= 0); bytes[index] = char(0xff); }
		QVERIFY(write(directory.filePath("bank.gig"), bytes));
		if (mode == 1) { QVERIFY(write(directory.filePath(name), companion())); }
		if (mode == 2) { QVERIFY(write(directory.filePath(name), companion("sample", QByteArray(16, 'x')))); }
		if (mode == 3) { QVERIFY(write(directory.filePath(name), riff(chunk("JUNK", "no identifier")))); }
		QString error; QVERIFY(!FileResourceSnapshot::captureGig(directory.filePath("bank.gig"), error)); QVERIFY(!error.isEmpty());
	}

	void gigCaptureClassifiesOnlyValidatedUnboundMissingCompanions_data()
	{
		QTest::addColumn<int>("mode"); QTest::newRow("missing-extension") << 0; QTest::newRow("missing-convolution") << 1; QTest::newRow("missing-root") << 2; QTest::newRow("corrupt-root") << 3; QTest::newRow("missing-before-corrupt-member") << 4; QTest::newRow("missing-before-collision") << 5; QTest::newRow("unsafe-declaration") << 6; QTest::newRow("complete-group") << 7;
	}
	void gigCaptureClassifiesOnlyValidatedUnboundMissingCompanions()
	{
		using namespace test::gigResource; QFETCH(int, mode); QTemporaryDir directory; const auto source = directory.filePath("bank.gig"); QByteArray bytes = root({"a.gx01"});
		if (mode == 1) { bytes = root({}, true); } if (mode == 3) { bytes = "invalid GIG root"; } if (mode == 4) { bytes = root({"a.gx01", "z.gx02"}); QVERIFY(write(directory.filePath("z.gx02"), "invalid companion")); } if (mode == 5) { bytes = root({"a.gx01", "A.gx01"}); } if (mode == 6) { bytes = root({"../unsafe", "a.gx01"}); } if (mode == 7) { QVERIFY(write(directory.filePath("a.gx01"), companion())); } if (mode != 2) { QVERIFY(write(source, bytes)); }
		QString error; auto failure = FileResourceSnapshot::GigCaptureFailure::MissingCompanion; const auto accepted = FileResourceSnapshot::captureGig(source, error, failure); const bool complete = mode == 7, missing = mode < 2; QCOMPARE(static_cast<bool>(accepted), complete); QCOMPARE(failure, complete ? FileResourceSnapshot::GigCaptureFailure::None : missing ? FileResourceSnapshot::GigCaptureFailure::MissingCompanion : FileResourceSnapshot::GigCaptureFailure::Other); if (!complete) { QVERIFY(!error.isEmpty()); }
	}
	void missingAcceptedGigMemberCannotBeDowngradedToRecoverable()
	{
		using namespace test::gigResource; QTemporaryDir directory; const auto source = directory.filePath("bank.gig"); QVERIFY(write(source, root({"pool.gx01"}))); QVERIFY(write(directory.filePath("pool.gx01"), companion())); QString error; const auto accepted = FileResourceSnapshot::captureGig(source, error); QVERIFY(accepted); FileResourceSnapshot::ReadScope reading(source, accepted); QVERIFY(QFile::remove(QFileInfo(accepted->path()).absoluteDir().filePath("pool.gx01"))); auto failure = FileResourceSnapshot::GigCaptureFailure::MissingCompanion; QVERIFY(!FileResourceSnapshot::captureGig(source, error, failure)); QCOMPARE(failure, FileResourceSnapshot::GigCaptureFailure::Other);
	}
	void gigCaptureCancellationAndExceptionsCannotReportMissingCompanions_data()
	{ QTest::addColumn<bool>("exception"); QTest::newRow("cancel") << false; QTest::newRow("exception") << true; }
	void gigCaptureCancellationAndExceptionsCannotReportMissingCompanions()
	{
		using namespace test::gigResource; QFETCH(bool, exception); QTemporaryDir directory; const auto source = directory.filePath("bank.gig"); QVERIFY(write(source, root({"a.gx01"}))); QString error; auto failure = FileResourceSnapshot::GigCaptureFailure::MissingCompanion; int checks = 0; FileResourceIO::Scope work([&] { if (++checks < 4) { return false; } if (exception) { throw std::runtime_error("GIG classification fixture"); } return true; }); if (exception) { QVERIFY_EXCEPTION_THROWN(FileResourceSnapshot::captureGig(source, error, failure), std::runtime_error); } else { QVERIFY(!FileResourceSnapshot::captureGig(source, error, failure)); } QVERIFY(checks >= 4); QCOMPARE(failure, FileResourceSnapshot::GigCaptureFailure::Other);
	}

	void gigSingleFileAndConvolutionDeclarationsRetainTheirRootName()
	{
		using namespace test::gigResource;
		QTemporaryDir directory; QVERIFY(directory.isValid()); QString error;
		const auto source = directory.filePath("bank.gig"); QVERIFY(write(source, root({})));
		auto single = FileResourceSnapshot::captureGig(source, error); QVERIFY2(single, qPrintable(error));
		QCOMPARE(single->members().size(), 1); QVERIFY(single->verify(error));
		QVERIFY(write(source, root({}, true)));
		QVERIFY(!FileResourceSnapshot::captureGig(source, error));
		QVERIFY(write(directory.filePath("bank.gx99"), companion()));
		auto convolution = FileResourceSnapshot::captureGig(source, error); QVERIFY2(convolution, qPrintable(error));
		QCOMPARE(convolution->members().size(), 2); QVERIFY(convolution->verify(error));
		QVERIFY(!convolution->verifyCopy(directory.filePath("renamed.gig"), error));
	}

	void captureFingerprintSurvivesSourceReplacementAndDetectsSnapshotCorruption()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("input.wav");
		const QByteArray bytes(130007, 'a'); QVERIFY(write(source, bytes));
		QString error; auto snapshot = FileResourceSnapshot::capture(source, error); QVERIFY2(snapshot, qPrintable(error));
		const auto checksum = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
		QCOMPARE(snapshot->sha256(), checksum); QCOMPARE(snapshot->bytes(), qint64(bytes.size()));
		const auto copy = directory.filePath("copy.wav"); QVERIFY(QFile::copy(snapshot->path(), copy));
		QVERIFY(write(source, QByteArray(bytes.size(), 'b'))); QVERIFY(snapshot->verify(error));
		QVERIFY(!snapshot->verifyCopy(source, error)); QVERIFY(snapshot->verifyCopy(copy, error));
		QVERIFY(write(snapshot->path(), QByteArray(bytes.size(), 'b')));
		QVERIFY(!snapshot->verify(error)); QVERIFY(!error.isEmpty());
		QCOMPARE(snapshot->sha256(), checksum); QCOMPARE(snapshot->bytes(), qint64(bytes.size()));
		QVERIFY(snapshot->verifyCopy(copy, error));
		QVERIFY(write(snapshot->path(), bytes.left(bytes.size() - 1))); QVERIFY(!snapshot->verify(error));
		QVERIFY(QFile::remove(snapshot->path())); QVERIFY(!snapshot->verify(error));
		QCOMPARE(snapshot->sha256(), checksum); QCOMPARE(snapshot->bytes(), qint64(bytes.size()));
		QVERIFY(write(snapshot->path(), bytes)); QVERIFY(snapshot->verify(error)); QVERIFY(error.isEmpty());
	}

	void multipleCopyChunksPreserveBytesUntilTheLastOwnerReleasesThem()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath(QString(QChar(0x97f3)) + ".sf2");
		QByteArray bytes(130007, '\0');
		for (int index = 0; index < bytes.size(); ++index) { bytes[index] = static_cast<char>(index % 251); }
		QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size())); file.close();
		QString error; auto snapshot = FileResourceSnapshot::capture(source, error);
		QVERIFY2(snapshot, qPrintable(error)); QVERIFY(error.isEmpty());
		const auto path = snapshot->path(); QVERIFY(path != source); QCOMPARE(QFileInfo(path).suffix(), QString("sf2"));
		QVERIFY(QFile::remove(source));
		QFile preserved(path); QVERIFY(preserved.open(QIODevice::ReadOnly)); QCOMPARE(preserved.readAll(), bytes); preserved.close();
		auto owner = snapshot; snapshot.reset(); QVERIFY(QFile::exists(path));
		owner.reset(); QVERIFY(!QFile::exists(path));
	}

	void workerReleaseDefersFileRemovalToTheApplicationThread()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid());
		const auto source = directory.filePath("wave.wav");
		QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("Owned input"), qint64(11)); file.close();
		QString error; auto snapshot = FileResourceSnapshot::capture(source, error); QVERIFY2(snapshot, qPrintable(error));
		const auto path = snapshot->path();
		std::thread worker([owner = std::move(snapshot)]() mutable { owner.reset(); }); worker.join();
		QVERIFY(QFile::exists(path));
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QVERIFY(!QFile::exists(path)); QVERIFY(QFile::exists(source));
	}

	void shutdownDrainRemovesQueuedFilesAndLeavesActiveOwnersIntact()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("input.pat");
		QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("Complete input"), qint64(14)); file.close();
		QString error;
		auto active = FileResourceSnapshot::capture(source, error); QVERIFY2(active, qPrintable(error));
		const auto activePath = active->path();
		std::array<std::shared_ptr<FileResourceSnapshot>, 3> resources;
		std::array<QString, 3> paths;
		std::array<QPointer<FileResourceSnapshot>, 3> objects;
		QObject observer;
		QThread* destroyedOn = nullptr;
		for (std::size_t index = 0; index < resources.size(); ++index)
		{
			resources[index] = FileResourceSnapshot::capture(source, error); QVERIFY2(resources[index], qPrintable(error));
			paths[index] = resources[index]->path(); objects[index] = resources[index].get();
			connect(resources[index].get(), &QObject::destroyed, &observer, [&] { destroyedOn = QThread::currentThread(); }, Qt::DirectConnection);
		}
		std::thread worker([owners = std::move(resources)]() mutable { for (auto& owner : owners) { owner.reset(); } }); worker.join();
		for (const auto& path : paths) { QVERIFY(QFile::exists(path)); }
		// Ordinary Qt cleanup can remove the middle of the queue before shutdown.
		QCoreApplication::sendPostedEvents(objects[1].data(), QEvent::DeferredDelete);
		QVERIFY(objects[1].isNull()); QVERIFY(!QFile::exists(paths[1]));
		QVERIFY(FileResourceSnapshot::drainDeferredRemovals());
		for (std::size_t index = 0; index < paths.size(); ++index) { QVERIFY(objects[index].isNull()); QVERIFY(!QFile::exists(paths[index])); }
		QCOMPARE(destroyedOn, QCoreApplication::instance()->thread());
		QVERIFY(QFile::exists(activePath)); QVERIFY(QFile::exists(source));
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QVERIFY(FileResourceSnapshot::drainDeferredRemovals());
		active.reset(); QVERIFY(!QFile::exists(activePath));
	}

	void workerCannotDrainApplicationOwnedCleanup()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); const auto source = directory.filePath("input.wav");
		QFile file(source); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write("Input"), qint64(5)); file.close();
		QString error; auto snapshot = FileResourceSnapshot::capture(source, error); QVERIFY2(snapshot, qPrintable(error));
		const auto path = snapshot->path(); bool drained = true;
		std::thread worker([owner = std::move(snapshot), &drained]() mutable {
			owner.reset(); drained = FileResourceSnapshot::drainDeferredRemovals();
		}); worker.join();
		QVERIFY(!drained); QVERIFY(QFile::exists(path));
		QVERIFY(FileResourceSnapshot::drainDeferredRemovals()); QVERIFY(!QFile::exists(path));
	}

	void missingFilesAndDirectoriesDoNotProduceSnapshots()
	{
		QTemporaryDir directory; QVERIFY(directory.isValid()); QString error;
		QVERIFY(!FileResourceSnapshot::capture(directory.filePath("absent.wav"), error)); QVERIFY(!error.isEmpty());
		QVERIFY(!FileResourceSnapshot::capture(directory.path(), error)); QVERIFY(!error.isEmpty());
		QVERIFY(!FileResourceSnapshot::capture({}, error)); QVERIFY(!error.isEmpty());
	}
};

} // namespace lmms

QTEST_GUILESS_MAIN(lmms::FileResourceSnapshotTest)
#include "FileResourceSnapshotTest.moc"
