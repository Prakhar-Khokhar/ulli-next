// tests/test_filecopier.cpp
//
// Tests for the copy that fills the boot partition with the live ISO.
//
// The tests work on temporary directories with synthetic files: no disk
// is touched, no ISO is mounted and no network is used.

#include "core/FileCopier.h"

#include <QtTest/QtTest>

#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using ulli::core::CopyLimits;
using ulli::core::CopyPlan;
using ulli::core::CopyProgressCallback;

namespace {

// Creates a file with the given content.
void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

}  // namespace

class TestFileCopier : public QObject {
    Q_OBJECT

private slots:
    void nestedAndEmptyDirectoriesAreCopied();
    void hiddenFilesAreCopied();
    void thePlanHoldsTheSizeAndTheFileCount();
    void anOversizedFileIsRejected();
    void anInsufficientDestinationIsRejected();
    void cancellationStopsTheEnumeration();
    void cancellationStopsTheCopy();
    void aFailingCopyReportsTheRelativePath();
    void theCopyReportsProgress();
    void aMissingSourceIsReported();
    void aSymlinkToAFileIsCopiedByContent();
    void aSymlinkToADirectoryIsNotTraversed();
};

void TestFileCopier::aSymlinkToAFileIsCopiedByContent() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "real.bin", "payload");
    std::error_code ec;
    std::filesystem::create_symlink(source / "real.bin", source / "link.bin", ec);
    if (ec) {
        QSKIP("symbolic links cannot be created in this environment");
    }

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());
    // The link is copied with the content of its target, like robocopy.
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(stats.ok());
    QVERIFY(std::filesystem::exists(destination / "link.bin"));
    QCOMPARE(readFile(destination / "link.bin"), std::string("payload"));
}

void TestFileCopier::aSymlinkToADirectoryIsNotTraversed() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "real" / "file.bin", "payload");
    std::error_code ec;
    std::filesystem::create_symlink(source / "real", source / "linkdir", ec);
    if (ec) {
        QSKIP("symbolic links cannot be created in this environment");
    }

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());
    // The link to the directory is not traversed, so its content is not
    // copied a second time.
    QCOMPARE(plan.value().fileCount, std::uint64_t(1));
    QCOMPARE(plan.value().skipped.size(), std::size_t(1));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(stats.ok());
    QVERIFY(std::filesystem::exists(destination / "real" / "file.bin"));
    QVERIFY(!std::filesystem::exists(destination / "linkdir" / "file.bin"));
}

void TestFileCopier::nestedAndEmptyDirectoriesAreCopied() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "casper" / "vmlinuz", "kernel");
    writeFile(source / "EFI" / "BOOT" / "BOOTx64.EFI", "loader");
    std::error_code ec;
    std::filesystem::create_directories(source / "empty" / "nested", ec);
    QVERIFY(!ec);

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(stats.ok());
    QCOMPARE(stats.value().filesCopied, std::uint64_t(2));

    QVERIFY(std::filesystem::exists(destination / "casper" / "vmlinuz"));
    QVERIFY(std::filesystem::exists(destination / "EFI" / "BOOT" / "BOOTx64.EFI"));
    QVERIFY(std::filesystem::is_directory(destination / "empty" / "nested"));
    QCOMPARE(readFile(destination / "casper" / "vmlinuz"), std::string("kernel"));
}

void TestFileCopier::hiddenFilesAreCopied() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    // A leading dot marks a hidden file on Unix; on Windows the file is
    // hidden by its attribute. Both are ordinary entries for the copy.
    writeFile(source / ".hidden", "secret");
    writeFile(source / "visible.txt", "public");

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(stats.ok());
    QVERIFY(std::filesystem::exists(destination / ".hidden"));
    QCOMPARE(readFile(destination / ".hidden"), std::string("secret"));
}

void TestFileCopier::thePlanHoldsTheSizeAndTheFileCount() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";

    writeFile(source / "a.bin", std::string(1000, 'a'));
    writeFile(source / "sub" / "b.bin", std::string(2000, 'b'));

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));
    QCOMPARE(plan.value().totalBytes, std::uint64_t(3000));
    QCOMPARE(plan.value().largestFileBytes, std::uint64_t(2000));
    QCOMPARE(plan.value().directories.size(), std::size_t(1));
}

void TestFileCopier::anOversizedFileIsRejected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "big.bin", "too big for fat32");

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());

    CopyLimits limits;
    limits.maxFileSizeBytes = 4;  // smaller than the file
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0, limits);
    QVERIFY(!stats.ok());
    QVERIFY(stats.error().message().find("4 GiB") != std::string::npos);
    // Nothing may be written when the check fails.
    QVERIFY(!std::filesystem::exists(destination / "big.bin"));
}

void TestFileCopier::anInsufficientDestinationIsRejected() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "payload.bin", std::string(10000, 'x'));

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());

    // The destination has less space than the payload needs.
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 100);
    QVERIFY(!stats.ok());
    QVERIFY(stats.error().message().find("Insufficient space") != std::string::npos);
    QVERIFY(!std::filesystem::exists(destination / "payload.bin"));

    // With enough space the copy runs.
    auto ok = ulli::core::copyCopyPlan(plan.value(), destination, 100000);
    QVERIFY(ok.ok());
    QVERIFY(std::filesystem::exists(destination / "payload.bin"));
}

void TestFileCopier::cancellationStopsTheEnumeration() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    for (int i = 0; i < 20; ++i) {
        writeFile(source / ("file" + std::to_string(i) + ".bin"), "data");
    }

    auto plan = ulli::core::enumerateCopyPlan(source, []() { return true; });
    QVERIFY(!plan.ok());
    QCOMPARE(plan.error().kind, ulli::core::Error::Kind::Cancelled);
}

void TestFileCopier::cancellationStopsTheCopy() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";
    for (int i = 0; i < 10; ++i) {
        writeFile(source / ("file" + std::to_string(i) + ".bin"), "data");
    }

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0,
                                            CopyLimits(), []() { return true; });
    QVERIFY(!stats.ok());
    QCOMPARE(stats.error().kind, ulli::core::Error::Kind::Cancelled);
}

void TestFileCopier::aFailingCopyReportsTheRelativePath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "sub" / "broken.bin", "data");

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(plan.ok());

    // A destination that cannot be written makes the copy fail.
    std::error_code ec;
    std::filesystem::create_directories(destination, ec);
    QVERIFY(!ec);
    // A file where the destination directory has to be: the copy of the
    // directory fails with a useful message.
    writeFile(destination / "sub", "not a directory");

    CopyLimits limits;
    limits.attempts = 1;  // do not wait between the attempts
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0, limits);
    QVERIFY(!stats.ok());
    QVERIFY(stats.error().message().find("broken.bin") != std::string::npos);
    QVERIFY(stats.error().message().find("Failed to copy") != std::string::npos);
}

void TestFileCopier::theCopyReportsProgress() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    for (int i = 0; i < 5; ++i) {
        writeFile(source / ("file" + std::to_string(i) + ".bin"),
                  std::string(1000, 'x'));
    }

    std::vector<std::string> phases;
    int lastPercent = -1;
    bool monotonic = true;
    CopyProgressCallback progress = [&](int percent, const std::string& phase,
                                         const std::string& detail) {
        if (phases.empty() || phases.back() != phase) phases.push_back(phase);
        if (percent < lastPercent) monotonic = false;
        lastPercent = percent;
        Q_UNUSED(detail);
    };

    auto plan = ulli::core::enumerateCopyPlan(source, nullptr, progress);
    QVERIFY(plan.ok());
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0,
                                           CopyLimits(), nullptr, progress);
    QVERIFY(stats.ok());

    QVERIFY(phases.size() >= 2);
    QCOMPARE(phases.front(), std::string("Enumerating ISO contents"));
    QVERIFY(phases.back().find("Copying") != std::string::npos);
    QVERIFY(monotonic);
    QCOMPARE(lastPercent, 100);
}

void TestFileCopier::aMissingSourceIsReported() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto missing = std::filesystem::path(dir.path().toStdString()) / "nope";

    auto plan = ulli::core::enumerateCopyPlan(missing);
    QVERIFY(!plan.ok());
    QCOMPARE(plan.error().kind, ulli::core::Error::Kind::NotFound);
}

QTEST_MAIN(TestFileCopier)
#include "test_filecopier.moc"
