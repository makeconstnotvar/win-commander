// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include <VFS/Host.h>

namespace nc::vfs {

/** Test-only provider mint. Production callers cannot construct conditional transaction authority. */
struct ProviderConditionalCopyTransactionTestAccess final {
    [[nodiscard]] static ProviderConditionalCopyReviewedAuthority
    MakeAuthority(ProviderConditionalCopyReviewedClaims _claims)
    {
        return ProviderConditionalCopyReviewedAuthority{
            std::move(_claims), std::make_shared<const int>(1)};
    }

    [[nodiscard]] static ProviderConditionalCopyReviewedAuthority
    MakeUnsealedAuthority(ProviderConditionalCopyReviewedClaims _claims) noexcept
    {
        return ProviderConditionalCopyReviewedAuthority{std::move(_claims), {}};
    }

    [[nodiscard]] static std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
                                       ProviderConditionalCopyTransactionBeginError>
    Mint(const Host &_provider,
         ProviderConditionalCopyReviewedAuthority _authority,
         ProviderConditionalCopyTransaction::CommitHandler _commit,
         ProviderConditionalCopyTransaction::AbortHandler _abort) noexcept
    {
        return ProviderConditionalCopyTransaction::Mint(
            _provider, std::move(_authority), std::move(_commit), std::move(_abort));
    }

    [[nodiscard]] static ProviderConditionalMoveReviewedAuthority
    MakeMoveAuthority(ProviderConditionalMoveReviewedClaims _claims)
    {
        return ProviderConditionalMoveReviewedAuthority{std::move(_claims), std::make_shared<const int>(1)};
    }

    [[nodiscard]] static ProviderConditionalMoveReviewedAuthority
    MakeUnsealedMoveAuthority(ProviderConditionalMoveReviewedClaims _claims) noexcept
    {
        return ProviderConditionalMoveReviewedAuthority{std::move(_claims), {}};
    }

    [[nodiscard]] static std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
                                       ProviderConditionalMoveTransactionBeginError>
    MintForMove(const Host &_provider,
                ProviderConditionalMoveReviewedAuthority _authority,
                ProviderConditionalCopyTransaction::CommitHandler _commit,
                ProviderConditionalCopyTransaction::AbortHandler _abort) noexcept
    {
        return ProviderConditionalCopyTransaction::MintForMove(
            _provider, std::move(_authority), std::move(_commit), std::move(_abort));
    }

    [[nodiscard]] static ProviderConditionalDeleteReviewedAuthority
    MakeDeleteAuthority(ProviderConditionalDeleteReviewedClaims _claims)
    {
        return ProviderConditionalDeleteReviewedAuthority{std::move(_claims), std::make_shared<const int>(1)};
    }

    [[nodiscard]] static ProviderConditionalDeleteReviewedAuthority
    MakeUnsealedDeleteAuthority(ProviderConditionalDeleteReviewedClaims _claims) noexcept
    {
        return ProviderConditionalDeleteReviewedAuthority{std::move(_claims), {}};
    }

    [[nodiscard]] static std::expected<std::unique_ptr<ProviderConditionalCopyTransaction>,
                                       ProviderConditionalDeleteTransactionBeginError>
    MintForDelete(const Host &_provider,
                  ProviderConditionalDeleteReviewedAuthority _authority,
                  ProviderConditionalCopyTransaction::CommitHandler _commit,
                  ProviderConditionalCopyTransaction::AbortHandler _abort) noexcept
    {
        return ProviderConditionalCopyTransaction::MintForDelete(
            _provider, std::move(_authority), std::move(_commit), std::move(_abort));
    }
};

} // namespace nc::vfs
