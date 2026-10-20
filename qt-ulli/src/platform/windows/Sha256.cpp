// platform/windows/Sha256.cpp
#include "platform/windows/Sha256.h"

#include <QCryptographicHash>
#include <QFile>
#include <QIODevice>

#include <filesystem>

namespace ulli::platform::windows {

core::Result<void> Sha256::verifyFile(
    const std::filesystem::path& file,
    const std::string& expectedHex) {
    if (!std::filesystem::exists(file)) {
        return core::makeError(core::Error::Kind::NotFound,
            "ISO file does not exist: " + file.string());
    }

    QFile f(QString::fromStdString(file.string()));
    if (!f.open(QIODevice::ReadOnly)) {
        return core::makeError(core::Error::Kind::Io,
            "Cannot open file for reading: " + file.string());
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    constexpr std::size_t kBufferSize = 64 * 1024;
    char buffer[kBufferSize];
    while (!f.atEnd()) {
        qint64 bytesRead = f.read(buffer, kBufferSize);
        if (bytesRead < 0) {
            f.close();
            return core::makeError(core::Error::Kind::Io,
                "Failed to read file: " + file.string());
        }
        hash.addData(buffer, static_cast<int>(bytesRead));
    }
    f.close();

    QString actualHex = QString(hash.result().toHex()).toLower();
    QString expected = QString::fromStdString(expectedHex).toLower().trimmed();

    if (actualHex != expected) {
        return core::makeError(core::Error::Kind::Platform,
            "SHA-256 mismatch:\n  expected: " + expected.toStdString() +
            "\n  actual:   " + actualHex.toStdString());
    }

    return core::makeOk();
}

}  // namespace ulli::platform::windows
