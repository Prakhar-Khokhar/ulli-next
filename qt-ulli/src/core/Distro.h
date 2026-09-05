// core/Distro.h
//
// One Linux distribution. Mirrors the schema in /distros.json (snake_case
// keys) but exposes a clean C++ API. Windows-specific metadata
// (Keyword, IsHybrid) lives alongside Linux-specific metadata
// (live_path, hybrid) — only the relevant fields are used per platform.

#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

namespace ulli::core {

class Distro {
public:
    Distro() = default;
    Distro(std::string key, std::string label, std::string isoFilename,
           std::string sha256, std::vector<std::string> mirrors,
           std::string downloadPage = {},
           std::string keyword = {},
           std::string validationFile = {},
           bool isHybrid = false,
           std::string livePath = {});

    const std::string& key() const noexcept { return key_; }
    const std::string& label() const noexcept { return label_; }
    const std::string& isoFilename() const noexcept { return isoFilename_; }
    const std::string& sha256() const noexcept { return sha256_; }
    const std::vector<std::string>& mirrors() const noexcept { return mirrors_; }
    const std::string& downloadPage() const noexcept { return downloadPage_; }
    const std::string& keyword() const noexcept { return keyword_; }
    const std::string& validationFile() const noexcept { return validationFile_; }
    bool isHybrid() const noexcept { return isHybrid_; }
    const std::string& livePath() const noexcept { return livePath_; }

    QString qlabel() const { return QString::fromStdString(label_); }
    QStringList qmirrors() const;

    bool valid() const { return !key_.empty() && !isoFilename_.empty(); }

private:
    std::string key_;
    std::string label_;
    std::string isoFilename_;
    std::string sha256_;
    std::vector<std::string> mirrors_;
    std::string downloadPage_;
    std::string keyword_;
    std::string validationFile_;
    bool isHybrid_ = false;
    std::string livePath_;
};

}  // namespace ulli::core
