// core/StagingMount.cpp
#include "core/StagingMount.h"

namespace ulli::core {

std::string stagingMountRoot(char driveLetter) {
    if (driveLetter < 'A' || driveLetter > 'Z') {
        return std::string();
    }
    return std::string(1, driveLetter) + ":\\";
}

std::string stagingMountErrorMessage(int diskNumber, int partitionNumber,
                                     char attemptedLetter,
                                     const std::string& commandOutput) {
    std::string message =
        "Could not establish a drive letter for the staging partition on "
        "disk " + std::to_string(diskNumber) + " (partition " +
        std::to_string(partitionNumber) + ")";
    if (attemptedLetter >= 'A' && attemptedLetter <= 'Z') {
        message += "; the letter '" + std::string(1, attemptedLetter) +
                   "' was not usable";
    }
    if (!commandOutput.empty()) {
        message += "; command output: " + commandOutput;
    }
    message +=
        ". The installation cannot continue without a mounted staging "
        "partition.";
    return message;
}

StagingMount decideStagingMount(int diskNumber, int partitionNumber,
                                char verifiedLetter, char attemptedLetter,
                                const std::string& commandOutput) {
    StagingMount result;

    const std::string root = stagingMountRoot(verifiedLetter);
    if (!root.empty()) {
        result.ok = true;
        result.root = root;
        return result;
    }

    result.ok = false;
    result.errorMessage = stagingMountErrorMessage(diskNumber, partitionNumber,
                                                  attemptedLetter, commandOutput);
    return result;
}

}  // namespace ulli::core
