// core/Sha256.cpp
#include "core/Sha256.h"

#include <QCryptographicHash>
#include <QFile>
#include <QString>

#include <array>

namespace ulli::core {

namespace {
// 64 KiB: large enough to keep the syscall rate low, small enough that
// the memory footprint stays flat for multi-gigabyte ISOs.
constexpr std::size_t kChunkSize = 64 * 1024;
}  // namespace

Result<std::string> Sha256::hashFile(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) {
        return makeError<std::string>(Error::Kind::NotFound,
                                      "ISO file does not exist: " + file.string());
    }

    QFile f(QString::fromStdString(file.string()));
    if (!f.open(QIODevice::ReadOnly)) {
        return makeError<std::string>(Error::Kind::Io,
                                      "Cannot open file for reading: " + file.string());
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, kChunkSize> buffer{};
    while (!f.atEnd()) {
        const qint64 bytesRead = f.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (bytesRead < 0) {
            f.close();
            return makeError<std::string>(Error::Kind::Io,
                                          "Failed to read file: " + file.string());
        }
        if (bytesRead == 0) continue;
        hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(bytesRead)));
    }
    f.close();

    // toHex() is ASCII, so the bytes are the hex string.
    return makeOk(std::string(hash.result().toHex().toLower().constData()));
}

Result<void> Sha256::verifyFile(const std::filesystem::path& file,
                                 const std::string& expectedHex) {
    const std::string expected = [&expectedHex] {
        QString s = QString::fromStdString(expectedHex).toLower().trimmed();
        return s.toStdString();
    }();
    if (expected.empty()) {
        return makeError(Error::Kind::InvalidInput,
                         "No expected SHA-256 digest available - the file "
                         "cannot be claimed to be verified");
    }

    auto actual = hashFile(file);
    if (!actual) return makeError(actual.error().kind(), actual.error().message());

    if (actual.value() != expected) {
        return makeError(Error::Kind::Platform,
                         "SHA-256 mismatch:\n  expected: " + expected +
                         "\n  actual:   " + actual.value());
    }
    return makeOk();
}

}  // namespace ulli::core
