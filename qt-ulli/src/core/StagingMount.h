// core/StagingMount.h
//
// The mount root of the staging partition.
//
// The drive letter of the staging partition is never guessed. Windows
// picks a free letter when the partition gets an access path, and the
// letter that is really assigned is read back from the partition and
// its volume. These helpers turn that verified letter into the root path
// the file copy needs, and produce the diagnostic for a partition that
// has no usable letter.

#pragma once

#include <string>

namespace ulli::core {

// The mount root of the staging partition.
struct StagingMount {
    bool ok = false;
    std::string root;          // "D:\\" when ok
    std::string errorMessage;  // the diagnostic when not ok
};

// The root path of a volume, for example "D:\\" for the letter 'D'.
// Empty when the letter is not a usable drive letter.
std::string stagingMountRoot(char driveLetter);

// The diagnostic for a staging partition without a usable drive letter.
// It names the disk, the partition, the letter that was attempted and
// the output of the command that was supposed to establish it.
std::string stagingMountErrorMessage(int diskNumber, int partitionNumber,
                                     char attemptedLetter,
                                     const std::string& commandOutput);

// Decides the mount root of the staging partition.
//
// `verifiedLetter` is the letter the partition really has, read back
// from the partition and its volume; it is empty when the verification
// did not yield one. `attemptedLetter` is the letter that was assigned
// or requested, and `commandOutput` is the output of the command that
// was supposed to establish it.
//
// Only a verified letter produces a root: a letter that was merely
// assumed is never returned.
StagingMount decideStagingMount(int diskNumber, int partitionNumber,
                                char verifiedLetter, char attemptedLetter,
                                const std::string& commandOutput);

}  // namespace ulli::core
