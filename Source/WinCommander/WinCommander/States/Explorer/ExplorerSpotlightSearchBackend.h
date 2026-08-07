// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#pragma once

#include "ExplorerDirectSearchBackend.h"

#include <expected>
#include <variant>

namespace nc::explorer {

enum class ExplorerSpotlightQueryScope : uint8_t {
    LocalComputer
};

struct ExplorerSpotlightUnixTime final {
    int64_t seconds = 0;

    constexpr bool operator==(const ExplorerSpotlightUnixTime &) const noexcept = default;
};

using ExplorerSpotlightPredicateArgument = std::variant<std::string, uint64_t, bool, ExplorerSpotlightUnixTime>;

/**
 * Value-only NSMetadataQuery input. The format is assembled exclusively from constant fragments;
 * every request value remains a separate predicate argument.
 */
struct ExplorerSpotlightQueryPlan final {
    std::string predicate_format;
    std::vector<ExplorerSpotlightPredicateArgument> predicate_arguments;
    ExplorerSpotlightQueryScope scope = ExplorerSpotlightQueryScope::LocalComputer;

    bool operator==(const ExplorerSpotlightQueryPlan &) const noexcept = default;
};

enum class ExplorerSpotlightQueryPlanFailure : uint8_t {
    InvalidPlan,
    WrongBackend
};

enum class ExplorerSpotlightQueryAvailability : uint8_t {
    Started,
    SpotlightUnavailable,
    IndexUnavailable
};

struct ExplorerSpotlightMetadataQueryCallbacks final {
    std::function<void()> started;
    /** Each update is an owned snapshot; the backend performs exact-path deduplication. */
    std::function<void(std::vector<std::string>)> updated;
    std::function<void(std::vector<std::string>)> finished;
    std::function<void(ExplorerSpotlightQueryAvailability)> unavailable;
};

/** Injectable, narrow NSMetadataQuery lifecycle seam. */
class ExplorerSpotlightMetadataQuery
{
public:
    virtual ~ExplorerSpotlightMetadataQuery() = default;

    [[nodiscard]] virtual ExplorerSpotlightQueryAvailability
    Start(const ExplorerSpotlightQueryPlan &_plan, ExplorerSpotlightMetadataQueryCallbacks _callbacks) = 0;
    virtual void Stop() noexcept = 0;
};

/** Asynchronous Spotlight implementation of the generic Explorer Search Mode backend seam. */
class ExplorerSpotlightSearchBackend final : public ExplorerSearchBackend
{
public:
    using MetadataQueryFactory = std::function<std::unique_ptr<ExplorerSpotlightMetadataQuery>()>;

    ExplorerSpotlightSearchBackend();
    explicit ExplorerSpotlightSearchBackend(MetadataQueryFactory _query_factory);

    [[nodiscard]] static std::expected<ExplorerSpotlightQueryPlan, ExplorerSpotlightQueryPlanFailure>
    BuildQueryPlan(const core::SearchPlan &_plan);

    [[nodiscard]] std::shared_ptr<ExplorerSearchBackendRun>
    Start(ExplorerSearchBackendInput _input, ProgressCallback _progress, CompletionCallback _completion) override;

private:
    MetadataQueryFactory m_QueryFactory;
};

} // namespace nc::explorer
