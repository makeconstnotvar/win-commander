// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "NativeCreateCopy.h"
#include "../Job.h"

namespace nc::ops {

class NativeCreateCopyJob final : public Job
{
public:
    NativeCreateCopyJob(NativeCreateCopyCapsule _capsule, std::shared_ptr<NativeCreateCopyIO> _io);
    ~NativeCreateCopyJob() override;

    NativeCreateCopyOutcome Outcome() const noexcept;

private:
    struct MetadataSnapshot final {
        mode_t mode{0};
        struct timespec access_time {};
        struct timespec modification_time {};
        struct timespec birth_time {};
        uint32_t flags{0};
        std::vector<std::byte> acl;
        bool captured{false};
    };

    struct FileSystemSyncResult final {
        int error_number{0};
        bool confirmed{false};
    };

    void Perform() override;
    bool OnStopRequested() noexcept override;

    bool ValidateSource();
    bool ValidateDestinationParent();
    bool ValidateDestinationAbsent();
    bool CaptureSourceMetadata();
    bool CreateTemp();
    bool ClearTempACL();
    bool CopyContents(std::vector<std::byte> &_buffer);
    bool CopyPrepublishMetadata();
    bool ApplyPublishedMetadata();
    bool VerifyPublishedMetadata();
    bool SyncTemp();
    bool CaptureTempSeal();
    bool RevalidateTempSeal();
    bool TryBeginCommit() noexcept;
    void ReleaseCommitForFailure() noexcept;
    bool Publish();
    bool ValidatePublishedDestination();
    void ClosePublishedDescriptor() noexcept;
    FileSystemSyncResult FinalizePublishedStorage() noexcept;
    void CompletePublished();
    bool Checkpoint(NativeCreateCopyCheckpoint _checkpoint);
    void Fail(NativeCreateCopyOutcomeCode _code, int _error_number = 0);
    void Succeed();
    bool AbandonTempForRecovery() noexcept;
    void StoreOutcome(NativeCreateCopyOutcome _outcome) noexcept;

    NativeCreateCopyCapsule m_Capsule;
    std::shared_ptr<NativeCreateCopyIO> m_IO;
    std::string m_TempName;
    int m_TempFD{-1};
    NativeCreateCopyIdentity m_TempIdentity;
    NativeCreateCopyIdentity m_TempSeal;
    MetadataSnapshot m_SourceMetadata;
    bool m_TempSealValid{false};
    uint64_t m_BytesCopied{0};
    bool m_Published{false};

    mutable std::mutex m_CommitMutex;
    bool m_StopAccepted{false};
    bool m_CommitStarted{false};

    mutable std::mutex m_OutcomeMutex;
    NativeCreateCopyOutcome m_Outcome;
};

} // namespace nc::ops
