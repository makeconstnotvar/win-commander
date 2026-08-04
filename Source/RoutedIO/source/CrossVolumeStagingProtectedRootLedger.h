// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <RoutedIO/CrossVolumeStagingProtocol.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string_view>

namespace nc::routedio::cross_volume_staging::helper {

/** A helper-owned identifier.  It never crosses the V1 XPC boundary. */
struct ArtifactID final {
    std::array<uint8_t, 32> bytes{};

    bool operator==(const ArtifactID &) const noexcept = default;
};

enum class ArtifactRole : uint8_t {
    SourceSnapshot,
    DestinationStage,
};

/**
 * Bounded root-FD-only record store for future helper artifacts.  A reservation creates a durable record, not an
 * artifact or a user-visible destination.  The narrow materializer may durably create one empty private artifact
 * and its append-only sealed companion manifest.  Restart reconciliation has read-only authority over that pair.
 */
class ProtectedRootLedger final
{
public:
    static constexpr size_t kMaximumReservations = 16;

    enum class Error : uint8_t {
        InvalidRoot,
        RootBusy,
        RootRegistryFull,
        InvalidHeader,
        InvalidRole,
        CapacityExceeded,
        DuplicateCorrelation,
        RandomFailed,
        RecordCreateFailed,
        RecordWriteFailed,
        RecordSyncFailed,
        RootSyncFailed,
        RecordCloseFailed,
        RecordRemoveFailed,
        ArtifactCreateFailed,
        ArtifactValidationFailed,
        ArtifactSyncFailed,
        ArtifactCloseFailed,
        SealManifestCreateFailed,
        SealManifestWriteFailed,
        SealManifestSyncFailed,
        SealManifestCloseFailed,
        PostSealValidationFailed,
        UnknownReservation,
        Busy,
    };

    struct ReconcileResult final {
        size_t removed_reservations{0};
        size_t retained_records{0};
        size_t ignored_entries{0};
        /** Sealed records are classified read-only; cleanup authority for them is a later separately reviewed slice. */
        size_t inspected_sealed_artifacts{0};
        size_t exact_sealed_artifacts{0};
        /** Includes a reservation whose deterministic private artifact name is already occupied. */
        size_t retained_incomplete_artifacts{0};
    };

    class Reservation final
    {
    public:
        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        Reservation(Reservation &&) noexcept = default;
        Reservation &operator=(Reservation &&) noexcept = default;
        ~Reservation() noexcept = default;

        [[nodiscard]] const Header &Correlation() const noexcept { return m_Header; }
        [[nodiscard]] ArtifactRole Role() const noexcept { return m_Role; }
        [[nodiscard]] const ArtifactID &ID() const noexcept { return m_ID; }

    private:
        Reservation(Header _header, ArtifactRole _role, ArtifactID _id) noexcept
            : m_Header{_header}, m_Role{_role}, m_ID{_id}
        {
        }

        Header m_Header;
        ArtifactRole m_Role;
        ArtifactID m_ID;

        friend class ProtectedRootLedger;
    };

    [[nodiscard]] static std::expected<ProtectedRootLedger, Error> Open(int _borrowed_root_fd) noexcept;

    ProtectedRootLedger(const ProtectedRootLedger &) = delete;
    ProtectedRootLedger &operator=(const ProtectedRootLedger &) = delete;
    ProtectedRootLedger(ProtectedRootLedger &&_rhs) noexcept;
    ProtectedRootLedger &operator=(ProtectedRootLedger &&_rhs) noexcept;
    ~ProtectedRootLedger() noexcept;

    [[nodiscard]] std::expected<Reservation, Error> Reserve(const Header &_header, ArtifactRole _role) noexcept;

    /**
     * Creates exactly one empty root-private `0600` artifact from a live reservation, durably seals it and appends a
     * separate sealed companion manifest.  The reservation remains occupied until a later explicitly authorized
     * cleanup authority exists.  Every interrupted or failed state stays retained for read-only reconciliation.
     */
    [[nodiscard]] std::expected<void, Error> MaterializeEmptyAndSeal(Reservation &&_reservation) noexcept;

    /** Removes exactly the still-live reservation record and frees its bounded slot after directory synchronization. */
    [[nodiscard]] std::expected<void, Error> Release(Reservation &&_reservation) noexcept;

    /**
     * Removes only an exact reservation record with no private artifact name present.  Sealed-artifact records are
     * parsed and identity-classified read-only; corrupt, incomplete and unsafe records stay retained.
     */
    [[nodiscard]] std::expected<ReconcileResult, Error> Reconcile() noexcept;

    [[nodiscard]] size_t ActiveReservationCount() const noexcept;

private:
    ProtectedRootLedger(int _root_fd, uint64_t _device, uint64_t _inode, bool _registered_root) noexcept;

    [[nodiscard]] bool HasValidRootUnlocked() const noexcept;
    [[nodiscard]] size_t ActiveReservationCountUnlocked() const noexcept;

    struct ActiveReservation final {
        Header header;
        ArtifactRole role;
        ArtifactID id;
    };

    mutable std::mutex m_Mutex;
    int m_RootFD{-1};
    uint64_t m_RootDevice{0};
    uint64_t m_RootInode{0};
    bool m_RegisteredRoot{false};
    std::array<std::optional<ActiveReservation>, kMaximumReservations> m_ActiveReservations;
};

} // namespace nc::routedio::cross_volume_staging::helper
