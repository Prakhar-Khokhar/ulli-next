// tests/test_filecopier.cpp
//
// Tests for the copy that fills the boot partition with the live ISO.
//
// The tests work on temporary directories with synthetic files: no disk
// is touched, no ISO is mounted and no network is used.

#include "core/FileCopier.h"

#include <QtTest/QtTest>

#include <QTemporaryDir>

#include <cstdio>
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

// The QTest report goes to stdout, which the Windows CI runner does not
// keep in the log. These breadcrumbs go to stderr and make a failing test
// visible there.
void trace(const std::string& what) {
    std::fprintf(stderr, "[filecopier] %s\n", what.c_str());
    std::fflush(stderr);
}

// Checks a result and reports the reason on stderr. Without it a failed
// check would be followed by an exception on value() and the report of
// the test would be lost.
template <typename T>
bool requireOk(const ulli::core::Result<T>& result, const char* what) {
    if (result.isOk()) {
        return true;
    }
    trace(std::string(what) + " failed: " + result.error().message());
    return false;
}

// Dumps a copy plan to stderr, so a failing check can be diagnosed from
// the CI log.
void dumpPlan(const char* what, const ulli::core::CopyPlan& plan) {
    std::string text = std::string("[filecopier] ") + what + ": files=" +
        std::to_string(plan.fileCount) + " bytes=" +
        std::to_string(plan.totalBytes) + " dirs=" +
        std::to_string(plan.directories.size()) + " skipped=";
    for (const std::string& entry : plan.skipped) {
        text += "{" + entry + "}";
    }
    text += " entries=";
    for (const auto& entry : plan.files) {
        text += "{" + entry.relative.string() + "}";
    }
    trace(text);
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
    trace("aSymlinkToAFileIsCopiedByContent");
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
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));
    dumpPlan("symlinkFile", plan.value());
    // The link is copied with the content of its target, like robocopy.
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(requireOk(stats, "copyCopyPlan"));
    QVERIFY(std::filesystem::exists(destination / "link.bin"));
    QCOMPARE(readFile(destination / "link.bin"), std::string("payload"));
}

void TestFileCopier::aSymlinkToADirectoryIsNotTraversed() {
    trace("aSymlinkToADirectoryIsNotTraversed");
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
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));
    dumpPlan("symlinkDir", plan.value());
    // The link to the directory is not traversed, so its content is not
    // copied a second time.
    QCOMPARE(plan.value().fileCount, std::uint64_t(1));
    QCOMPARE(plan.value().skipped.size(), std::size_t(1));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(requireOk(stats, "copyCopyPlan"));
    trace(std::string("[filecopier] destination has real/file.bin: ") +
          (std::filesystem::exists(destination / "real" / "file.bin") ? "yes" : "no") +
          " has linkdir/file.bin: " +
          (std::filesystem::exists(destination / "linkdir" / "file.bin") ? "yes" : "no"));
    QVERIFY(std::filesystem::exists(destination / "real" / "file.bin"));
    QVERIFY(!std::filesystem::exists(destination / "linkdir" / "file.bin"));
}

void TestFileCopier::nestedAndEmptyDirectoriesAreCopied() {
    trace("nestedAndEmptyDirectoriesAreCopied");
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
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(requireOk(stats, "copyCopyPlan"));
    QCOMPARE(stats.value().filesCopied, std::uint64_t(2));

    QVERIFY(std::filesystem::exists(destination / "casper" / "vmlinuz"));
    QVERIFY(std::filesystem::exists(destination / "EFI" / "BOOT" / "BOOTx64.EFI"));
    QVERIFY(std::filesystem::is_directory(destination / "empty" / "nested"));
    QCOMPARE(readFile(destination / "casper" / "vmlinuz"), std::string("kernel"));
}

void TestFileCopier::hiddenFilesAreCopied() {
    trace("hiddenFilesAreCopied");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    // A leading dot marks a hidden file on Unix; on Windows the file is
    // hidden by its attribute. Both are ordinary entries for the copy.
    writeFile(source / ".hidden", "secret");
    writeFile(source / "visible.txt", "public");

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination);
    QVERIFY(requireOk(stats, "copyCopyPlan"));
    QVERIFY(std::filesystem::exists(destination / ".hidden"));
    QCOMPARE(readFile(destination / ".hidden"), std::string("secret"));
}

void TestFileCopier::thePlanHoldsTheSizeAndTheFileCount() {
    trace("thePlanHoldsTheSizeAndTheFileCount");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";

    writeFile(source / "a.bin", std::string(1000, 'a'));
    writeFile(source / "sub" / "b.bin", std::string(2000, 'b'));

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));
    QCOMPARE(plan.value().fileCount, std::uint64_t(2));
    QCOMPARE(plan.value().totalBytes, std::uint64_t(3000));
    QCOMPARE(plan.value().largestFileBytes, std::uint64_t(2000));
    QCOMPARE(plan.value().directories.size(), std::size_t(1));
}

void TestFileCopier::anOversizedFileIsRejected() {
    trace("anOversizedFileIsRejected");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "big.bin", "too big for fat32");

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));

    CopyLimits limits;
    limits.maxFileSizeBytes = 4;  // smaller than the file
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0, limits);
    QVERIFY(!stats.isOk());
    QVERIFY(stats.error().message().find("4 GiB") != std::string::npos);
    // Nothing may be written when the check fails.
    QVERIFY(!std::filesystem::exists(destination / "big.bin"));
}

void TestFileCopier::anInsufficientDestinationIsRejected() {
    trace("anInsufficientDestinationIsRejected");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "payload.bin", std::string(10000, 'x'));

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));

    // The destination has less space than the payload needs.
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 100);
    QVERIFY(!stats.isOk());
    QVERIFY(stats.error().message().find("Insufficient space") != std::string::npos);
    QVERIFY(!std::filesystem::exists(destination / "payload.bin"));

    // With enough space the copy runs.
    auto ok = ulli::core::copyCopyPlan(plan.value(), destination, 100000);
    QVERIFY(requireOk(ok, "copyCopyPlan"));
    QVERIFY(std::filesystem::exists(destination / "payload.bin"));
}

void TestFileCopier::cancellationStopsTheEnumeration() {
    trace("cancellationStopsTheEnumeration");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    for (int i = 0; i < 20; ++i) {
        writeFile(source / ("file" + std::to_string(i) + ".bin"), "data");
    }

    auto plan = ulli::core::enumerateCopyPlan(source, []() { return true; });
    QVERIFY(!plan.isOk());
    QCOMPARE(plan.error().kind(), ulli::core::Error::Kind::Cancelled);
}

void TestFileCopier::cancellationStopsTheCopy() {
    trace("cancellationStopsTheCopy");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";
    for (int i = 0; i < 10; ++i) {
        writeFile(source / ("file" + std::to_string(i) + ".bin"), "data");
    }

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));

    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0,
                                            CopyLimits(), []() { return true; });
    QVERIFY(!stats.isOk());
    QCOMPARE(stats.error().kind(), ulli::core::Error::Kind::Cancelled);
}

void TestFileCopier::aFailingCopyReportsTheRelativePath() {
    trace("aFailingCopyReportsTheRelativePath");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    writeFile(source / "sub" / "broken.bin", "data");

    auto plan = ulli::core::enumerateCopyPlan(source);
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));

    // A directory where the file has to be: the copy of the file fails
    // and the error names the file and the destination.
    std::error_code ec;
    std::filesystem::create_directories(destination / "sub" / "broken.bin", ec);
    QVERIFY(!ec);

    CopyLimits limits;
    limits.attempts = 1;  // do not wait between the attempts
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0, limits);
    QVERIFY(!stats.isOk());
    QVERIFY(stats.error().message().find("broken.bin") != std::string::npos);
    QVERIFY(stats.error().message().find("Failed to copy") != std::string::npos);
}

void TestFileCopier::theCopyReportsProgress() {
    trace("theCopyReportsProgress");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto source = std::filesystem::path(dir.path().toStdString()) / "iso";
    const auto destination = std::filesystem::path(dir.path().toStdString()) / "staging";

    for (int i = 0; i < 5; ++i) {
        writeFile(source / ("file" + std::to_string(i) + ".bin"),
                  std::string(1000, 'x'));
    }

    std::vector<std::string> phases;
    std::vector<int> copyPercents;
    CopyProgressCallback progress = [&](int percent, const std::string& phase,
                                         const std::string& detail) {
        if (phases.empty() || phases.back() != phase) phases.push_back(phase);
        if (phase.find("Copying") != std::string::npos) {
            copyPercents.push_back(percent);
        }
        Q_UNUSED(detail);
    };

    auto plan = ulli::core::enumerateCopyPlan(source, nullptr, progress);
    QVERIFY(requireOk(plan, "enumerateCopyPlan"));
    auto stats = ulli::core::copyCopyPlan(plan.value(), destination, 0,
                                           CopyLimits(), nullptr, progress);
    QVERIFY(requireOk(stats, "copyCopyPlan"));

    // The enumeration, the copy and the finalizing are reported as
    // separate phases, and the copy reports a growing percentage that ends
    // at 100.
    QVERIFY(phases.size() >= 3);
    QCOMPARE(phases.front(), std::string("Enumerating ISO contents"));
    QVERIFY(phases.back().find("Finalizing") != std::string::npos);
    QVERIFY(copyPercents.size() >= 2);
    QCOMPARE(copyPercents.back(), 100);
    for (std::size_t i = 1; i < copyPercents.size(); ++i) {
        QVERIFY(copyPercents[i] >= copyPercents[i - 1]);
    }
}

void TestFileCopier::aMissingSourceIsReported() {
    trace("aMissingSourceIsReported");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto missing = std::filesystem::path(dir.path().toStdString()) / "nope";

    auto plan = ulli::core::enumerateCopyPlan(missing);
    QVERIFY(!plan.isOk());
    QCOMPARE(plan.error().kind(), ulli::core::Error::Kind::NotFound);
}

QTEST_MAIN(TestFileCopier)
#include "test_filecopier.moc"
