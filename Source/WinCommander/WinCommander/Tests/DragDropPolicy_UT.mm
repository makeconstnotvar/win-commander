// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <WinCommander/States/FilePanels/DragDropPolicy.h>
#include <array>
#include <string_view>
#include <utility>

namespace {

using nc::panel::DragDropDestinationFacts;
using nc::panel::DragDropFailure;
using nc::panel::DragDropItemKind;
using nc::panel::DragDropModifierIntent;
using nc::panel::DragDropModifierState;
using nc::panel::DragDropOperation;
using nc::panel::DragDropPathCapabilities;
using nc::panel::DragDropPolicyInput;
using nc::panel::DragDropSourceFacts;
using nc::panel::EvaluateDragDropPolicy;
using nc::panel::MapDragDropModifiers;

DragDropPathCapabilities FullCapabilities()
{
    return DragDropPathCapabilities{
        .can_read = true,
        .can_write = true,
        .can_create_file = true,
        .can_create_folder = true,
        .can_create_symlink = true,
        .can_rename = true,
        .can_delete_permanently = true,
    };
}

DragDropPolicyInput Input(const bool _native = false)
{
    return DragDropPolicyInput{
        .modifiers = {},
        .sources = {DragDropSourceFacts{
            .provider_identity = 11,
            .volume_identity = _native ? 101U : 0U,
            .native_namespace = _native,
            .kind = DragDropItemKind::RegularFile,
            .path = "/source/report.txt",
            .directory = "/source/",
            .capabilities = FullCapabilities(),
        }},
        .destination = DragDropDestinationFacts{
            .provider_identity = 11,
            .volume_identity = _native ? 101U : 0U,
            .native_namespace = _native,
            .directory = "/destination/",
            .capabilities = FullCapabilities(),
        },
    };
}

struct ExpectedDecision final {
    DragDropOperation operation;
    DragDropFailure failure;
};

void CheckDecision(DragDropPolicyInput _input, const ExpectedDecision _expected)
{
    const auto decision = EvaluateDragDropPolicy(std::move(_input));
    CHECK(decision.Operation() == _expected.operation);
    CHECK(decision.Failure() == _expected.failure);
    CHECK(decision.Allowed() == (_expected.operation != DragDropOperation::Forbidden));
}

} // namespace

#define PREFIX "nc::panel::DragDropPolicy "

TEST_CASE(PREFIX "maps normalized modifiers including explicit Move")
{
    struct Case final {
        DragDropModifierState state;
        DragDropModifierIntent expected;
    };
    constexpr std::array cases{
        Case{{}, DragDropModifierIntent::Automatic},
        Case{{.force_copy = true}, DragDropModifierIntent::Copy},
        Case{{.force_move = true}, DragDropModifierIntent::Move},
        Case{{.force_link = true}, DragDropModifierIntent::Link},
        Case{{.force_copy = true, .force_move = true}, DragDropModifierIntent::Invalid},
        Case{{.force_copy = true, .force_link = true}, DragDropModifierIntent::Invalid},
        Case{{.force_move = true, .force_link = true}, DragDropModifierIntent::Invalid},
    };

    for( const Case &test : cases )
        CHECK(MapDragDropModifiers(test.state) == test.expected);
}

TEST_CASE(PREFIX "selects automatic Copy or Move only from exact provider volume and writable evidence")
{
    struct Case final {
        bool native;
        uintptr_t source_provider;
        uintptr_t destination_provider;
        uintptr_t source_volume;
        uintptr_t destination_volume;
        bool source_writable;
        ExpectedDecision expected;
    };
    constexpr std::array cases{
        Case{false, 11, 11, 0, 0, true, {DragDropOperation::Move, DragDropFailure::None}},
        Case{false, 11, 11, 0, 0, false, {DragDropOperation::Copy, DragDropFailure::None}},
        Case{false, 11, 12, 0, 0, true, {DragDropOperation::Copy, DragDropFailure::None}},
        Case{true, 11, 11, 101, 101, true, {DragDropOperation::Move, DragDropFailure::None}},
        Case{true, 11, 11, 101, 202, true, {DragDropOperation::Copy, DragDropFailure::None}},
        Case{true, 11, 11, 0, 101, true, {DragDropOperation::Forbidden, DragDropFailure::UnknownVolumeIdentity}},
    };

    for( const Case &test : cases ) {
        auto input = Input(test.native);
        input.sources[0].provider_identity = test.source_provider;
        input.destination.provider_identity = test.destination_provider;
        input.sources[0].volume_identity = test.source_volume;
        input.destination.volume_identity = test.destination_volume;
        input.sources[0].capabilities.can_write = test.source_writable;
        CheckDecision(std::move(input), test.expected);
    }

    auto no_move_capability = Input();
    no_move_capability.sources[0].capabilities.can_rename = false;
    no_move_capability.sources[0].capabilities.can_delete_permanently = false;
    CheckDecision(std::move(no_move_capability), {DragDropOperation::Copy, DragDropFailure::None});

    auto rename_without_read = Input();
    rename_without_read.sources[0].capabilities.can_read = false;
    CheckDecision(std::move(rename_without_read), {DragDropOperation::Move, DragDropFailure::None});
}

TEST_CASE(PREFIX "honours explicit Copy Move and native-only Link")
{
    {
        auto input = Input();
        input.modifiers.force_copy = true;
        CheckDecision(std::move(input), {DragDropOperation::Copy, DragDropFailure::None});
    }
    {
        auto input = Input();
        input.modifiers.force_move = true;
        input.destination.provider_identity = 22;
        CheckDecision(std::move(input), {DragDropOperation::Move, DragDropFailure::None});
    }
    {
        auto input = Input(true);
        input.modifiers.force_link = true;
        input.destination.provider_identity = 22;
        CheckDecision(std::move(input), {DragDropOperation::Link, DragDropFailure::None});
    }
    {
        auto input = Input();
        input.modifiers.force_link = true;
        CheckDecision(std::move(input), {DragDropOperation::Forbidden, DragDropFailure::LinkUnsupported});
    }
}

TEST_CASE(PREFIX "uses path-scoped source and destination capabilities")
{
    struct Case final {
        DragDropFailure expected;
        void (*mutate)(DragDropPolicyInput &);
    };
    const std::array cases{
        Case{DragDropFailure::SourceUnreadable,
             +[](DragDropPolicyInput &_input) {
                 _input.modifiers.force_copy = true;
                 _input.sources[0].capabilities.can_read = false;
             }},
        Case{DragDropFailure::DestinationReadOnly,
             +[](DragDropPolicyInput &_input) { _input.destination.capabilities.can_write = false; }},
        Case{DragDropFailure::DestinationUnsupported,
             +[](DragDropPolicyInput &_input) {
                 _input.modifiers.force_copy = true;
                 _input.destination.capabilities.can_create_file = false;
             }},
        Case{DragDropFailure::SourceReadOnly, +[](DragDropPolicyInput &_input) {
                 _input.modifiers.force_move = true;
                 _input.sources[0].capabilities.can_write = false;
             }},
        Case{DragDropFailure::SourceMutationUnsupported, +[](DragDropPolicyInput &_input) {
                 _input.modifiers.force_move = true;
                 _input.sources[0].capabilities.can_rename = false;
                 _input.sources[0].capabilities.can_delete_permanently = false;
             }},
        Case{DragDropFailure::LinkUnsupported, +[](DragDropPolicyInput &_input) {
                 _input.modifiers.force_link = true;
                 _input.destination.capabilities.can_create_symlink = false;
             }},
    };

    for( const Case &test : cases ) {
        auto input = test.expected == DragDropFailure::LinkUnsupported ? Input(true) : Input();
        test.mutate(input);
        CheckDecision(std::move(input), {DragDropOperation::Forbidden, test.expected});
    }
}

TEST_CASE(PREFIX "rejects same-folder recursive malformed and duplicate sources component-wise")
{
    struct Case final {
        std::string_view source_path;
        std::string_view source_directory;
        std::string_view destination;
        DragDropItemKind kind;
        DragDropFailure expected;
    };
    constexpr std::array cases{
        Case{"/same/item", "/same/", "/same/", DragDropItemKind::RegularFile, DragDropFailure::SameFolder},
        Case{"/tree", "/", "/tree/child/", DragDropItemKind::Directory, DragDropFailure::RecursiveDestination},
        Case{"/tree", "/", "/tree/", DragDropItemKind::Directory, DragDropFailure::RecursiveDestination},
        Case{"/tree/file", "/wrong/", "/destination/", DragDropItemKind::RegularFile, DragDropFailure::InvalidPath},
        Case{"/tree/../file", "/tree/../", "/destination/", DragDropItemKind::RegularFile, DragDropFailure::InvalidPath},
    };

    for( const Case &test : cases ) {
        auto input = Input();
        input.sources[0].path = test.source_path;
        input.sources[0].directory = test.source_directory;
        input.sources[0].kind = test.kind;
        input.destination.directory = test.destination;
        CheckDecision(std::move(input), {DragDropOperation::Forbidden, test.expected});
    }

    auto duplicate = Input();
    duplicate.sources.emplace_back(duplicate.sources.front());
    CheckDecision(std::move(duplicate), {DragDropOperation::Forbidden, DragDropFailure::DuplicateSource});
}

TEST_CASE(PREFIX "does not confuse component prefixes with recursive destinations")
{
    auto input = Input();
    input.sources[0].path = "/tree";
    input.sources[0].directory = "/";
    input.sources[0].kind = DragDropItemKind::Directory;
    input.destination.directory = "/treehouse/";

    CheckDecision(std::move(input), {DragDropOperation::Move, DragDropFailure::None});

    auto other_provider = Input();
    other_provider.sources[0].path = "/same/item";
    other_provider.sources[0].directory = "/same/";
    other_provider.destination.directory = "/same/";
    other_provider.destination.provider_identity = 22;
    CheckDecision(std::move(other_provider), {DragDropOperation::Copy, DragDropFailure::None});
}

TEST_CASE(PREFIX "fails closed on unknown or inconsistent exact provider identity")
{
    {
        auto input = Input();
        input.sources[0].provider_identity = 0;
        CheckDecision(std::move(input),
                      {DragDropOperation::Forbidden, DragDropFailure::UnknownProviderIdentity});
    }
    {
        auto input = Input();
        input.destination.provider_identity = 0;
        CheckDecision(std::move(input),
                      {DragDropOperation::Forbidden, DragDropFailure::UnknownProviderIdentity});
    }
    {
        auto input = Input();
        input.sources[0].native_namespace = true;
        CheckDecision(std::move(input),
                      {DragDropOperation::Forbidden, DragDropFailure::InconsistentProviderFacts});
    }
}

TEST_CASE(PREFIX "retains the exact immutable facts used for the decision")
{
    const DragDropPolicyInput input = Input(true);
    const auto decision = EvaluateDragDropPolicy(input);

    REQUIRE(decision.Allowed());
    CHECK(decision.Input() == input);
    CHECK(decision.Input().sources[0].provider_identity == 11);
    CHECK(decision.Input().sources[0].volume_identity == 101);
    CHECK(decision.Input().destination.directory == "/destination/");
}
