#include "platform/windows/Sha256.h"

#include "core/Sha256.h"

namespace ulli::platform::windows {

core::Result<void> Sha256::verifyFile(
    const std::filesystem::path& file,
    const std::string& expectedHex) {
    return core::Sha256::verifyFile(file, expectedHex);
}

}  // namespace ulli::platform::windows
