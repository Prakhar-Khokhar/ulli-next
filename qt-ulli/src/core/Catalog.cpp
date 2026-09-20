// core/Catalog.cpp
#include "core/Catalog.h"

#include "platform/Platform.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <iostream>

namespace ulli::core {

// ─── Distro ──────────────────────────────────────────────────────────────────

Distro::Distro(std::string key, std::string label, std::string isoFilename,
               std::string sha256, std::vector<std::string> mirrors,
               std::string downloadPage, std::string keyword,
               std::string validationFile, bool isHybrid, std::string livePath)
    : key_(std::move(key))
    , label_(std::move(label))
    , isoFilename_(std::move(isoFilename))
    , sha256_(std::move(sha256))
    , mirrors_(std::move(mirrors))
    , downloadPage_(std::move(downloadPage))
    , keyword_(std::move(keyword))
    , validationFile_(std::move(validationFile))
    , isHybrid_(isHybrid)
    , livePath_(std::move(livePath)) {}

QStringList Distro::qmirrors() const {
    QStringList out;
    out.reserve(static_cast<int>(mirrors_.size()));
    for (const auto& m : mirrors_) {
        out << QString::fromStdString(m);
    }
    return out;
}

// ─── Built-in fallback (mirror of /distros.json) ─────────────────────────────

const std::vector<Distro>& Catalog::fallbackDistros() {
    static const std::vector<Distro> kFallback = {
        {"mint", "Linux Mint 22.3", "linuxmint-22.3-cinnamon-64bit.iso",
         "a081ab202cfda17f6924128dbd2de8b63518ac0531bcfe3f1a1b88097c459bd4",
         {"https://mirrors.kernel.org/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso",
          "https://mirror.csclub.uwaterloo.ca/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso",
          "https://mirrors.seas.harvard.edu/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso",
          "https://mirror.arizona.edu/linuxmint/stable/22.3/linuxmint-22.3-cinnamon-64bit.iso"},
         "https://linuxmint.com/edition.php?id=326", "Mint", "casper\\vmlinuz", false,
         "casper/vmlinuz"},
        {"cachyos", "CachyOS Desktop", "cachyos-desktop-linux-260809.iso",
         "959f6577f45e25ee9fd8c220fd221b08e4ea79412c7315c0f922dd6d86d5e33c",
         {"https://cdn77.cachyos.org/ISO/desktop/260809/cachyos-desktop-linux-260809.iso"},
         "https://cachyos.org/download/", "CachyOS",
         "arch\\boot\\x86_64\\vmlinuz-linux-cachyos", true,
         "boot/vmlinuz-linux-cachyos"},
        {"ubuntu", "Ubuntu 26.04 LTS", "ubuntu-26.04-desktop-amd64.iso",
         "487f87faaf547ea30e0aba4d5b53346292571256b25333a978db1692bcee9dd2",
         {"https://gsl-syd.mm.fcix.net/ubuntu-releases/26.04/ubuntu-26.04-desktop-amd64.iso",
          "https://mirror.xenyth.net/ubuntu-releases/26.04/ubuntu-26.04-desktop-amd64.iso",
          "https://ftp.udx.icscoe.jp/Linux/ubuntu-releases/26.04/ubuntu-26.04-desktop-amd64.iso"},
         "https://ubuntu.com/download/desktop", "Ubuntu", "casper\\vmlinuz", false,
         "casper/vmlinuz"},
        {"kubuntu", "Kubuntu 26.04 LTS", "kubuntu-26.04-desktop-amd64.iso",
         "95ce9cf68f13015b9a88bd1ef86fcf7eda77c99979fda48c69e28aa0a84f88ac",
         {"https://ftp.linux.org.tr/kubuntu/26.04/release/kubuntu-26.04-desktop-amd64.iso",
          "https://www.mirrorservice.org/sites/cdimage.ubuntu.com/cdimage/kubuntu/releases/26.04/release/kubuntu-26.04-desktop-amd64.iso",
          "https://cdimage.ubuntu.com/kubuntu/releases/26.04/release/kubuntu-26.04-desktop-amd64.iso"},
         "https://kubuntu.org/getkubuntu/", "Kubuntu", "casper\\vmlinuz", false,
         "casper/vmlinuz"},
        {"linux-lite", "Linux Lite 8.0 - Xfce", "linux-lite-8.0-64bit.iso",
         "7cfc63baf597156a0a5ecac87e860aff3967279694b19fa67fb410a34802857e",
         {"https://mirror.freedif.org/LinuxLiteOS/isos/linux-lite-8.0-64bit.iso",
          "https://mirror.freedif.org/LinuxLiteOS/isos/linux-lite-8.0-64bit.iso",
          "https://www.mirrorservice.org/sites/repo.linuxliteos.com/linuxlite/isos/8.0/linux-lite-8.0-64bit.iso",
          "https://mirrors.sjtug.sjtu.edu.cn/linuxliteos/isos/8.0/linux-lite-8.0-64bit.iso"},
         "https://www.linuxliteos.com/download.php", "Linux Lite", "casper\\vmlinuz", true,
         "casper/vmlinuz"},
        {"debian", "Debian Live 13.6.0 KDE", "debian-live-13.6.0-amd64-kde.iso",
         "426984f7edf034f4cd49f6218e706a6086588359d34fa0328676451b4a679639",
         {"https://cdimage.debian.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.6.0-amd64-kde.iso",
          "https://mirrors.edge.kernel.org/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.6.0-amd64-kde.iso",
          "https://mirror.csclub.uwaterloo.ca/debian-cd/current-live/amd64/iso-hybrid/debian-live-13.6.0-amd64-kde.iso"},
         "https://www.debian.org/CD/live/", "Debian", "live\\vmlinuz", true,
         "live/vmlinuz"},
        {"fedora", "Fedora 43 KDE", "Fedora-KDE-Desktop-Live-43-1.6.x86_64.iso",
         "181fe3e265fb5850c929f5afb7bdca91bb433b570ef39ece4a7076187435fdab",
         {"https://mirror.telepoint.bg/fedora/releases/43/KDE/x86_64/iso/Fedora-KDE-Desktop-Live-43-1.6.x86_64.iso",
          "https://mirrors.netix.net/fedora/linux/releases/43/KDE/x86_64/iso/Fedora-KDE-Desktop-Live-43-1.6.x86_64.iso"},
         "https://fedoraproject.org/kde/download/", "Fedora", "LiveOS\\squashfs.img", true,
         "LiveOS/squashfs.img"},
    };
    return kFallback;
}

// ─── JSON loader ─────────────────────────────────────────────────────────────

namespace {

std::optional<Catalog> tryLoadFromFile(const std::filesystem::path& path) {
    QFile f(QString::fromStdString(path.string()));
    if (!f.exists()) return std::nullopt;
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (perr.error != QJsonParseError::NoError) {
        std::cerr << "WARN: parse error in " << path << ": "
                  << perr.errorString().toStdString() << '\n';
        return std::nullopt;
    }
    if (!doc.isObject()) {
        std::cerr << "WARN: " << path << " is not a JSON object at the top level\n";
        return std::nullopt;
    }

    Catalog c;
    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject e = it.value().toObject();
        std::vector<std::string> mirrors;
        for (const QJsonValue& v : e.value("mirrors").toArray()) {
            mirrors.push_back(v.toString().toStdString());
        }
        // Linux-style live_path is the validation_file with backslashes
        // flipped — convenient for the Linux port.
        std::string validation = e.value("validation_file").toString().toStdString();
        std::string livePath = validation;
        std::replace(livePath.begin(), livePath.end(), '\\', '/');

        Distro d(
            it.key().toStdString(),
            e.value("label").toString().toStdString(),
            e.value("filename").toString().toStdString(),
            e.value("sha256").toString().toStdString(),
            std::move(mirrors),
            e.value("download_page").toString().toStdString(),
            e.value("keyword").toString().toStdString(),
            std::move(validation),
            e.value("is_hybrid").toBool(false),
            std::move(livePath)
        );
        if (!d.valid()) {
            std::cerr << "WARN: skipping invalid entry '" << it.key().toStdString()
                      << "' in " << path << '\n';
            continue;
        }
        c.insert(std::move(d));
    }
    if (c.empty()) return std::nullopt;
    return c;
}

}  // namespace

Catalog Catalog::load(bool* fallbackUsed) {
    if (fallbackUsed) *fallbackUsed = false;

    const std::filesystem::path exeDir = platform::executableDir();
    const std::vector<std::filesystem::path> candidates = {
        exeDir / "distros.json",
        exeDir.parent_path() / "distros.json",
    };
    for (const auto& p : candidates) {
        if (auto c = tryLoadFromFile(p); c.has_value()) {
            std::cout << "Loaded " << c->distros().size()
                      << " distros from " << p << '\n';
            return std::move(*c);
        }
    }

    std::cerr << "WARN: distros.json not found; using built-in catalog\n";
    if (fallbackUsed) *fallbackUsed = true;
    return fromDistros(std::vector<Distro>(
        fallbackDistros().begin(), fallbackDistros().end()));
}

Catalog Catalog::fromDistros(std::vector<Distro> distros) {
    Catalog c;
    for (auto& d : distros) {
        c.insert(std::move(d));
    }
    return c;
}

const Distro* Catalog::find(const std::string& key) const {
    auto it = distros_.find(key);
    return it == distros_.end() ? nullptr : &it->second;
}

std::vector<std::string> Catalog::keys() const {
    std::vector<std::string> out;
    out.reserve(distros_.size());
    for (const auto& [k, _] : distros_) out.push_back(k);
    return out;
}

}  // namespace ulli::core
