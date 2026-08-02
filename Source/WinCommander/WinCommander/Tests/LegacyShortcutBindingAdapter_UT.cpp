// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include <WinCommander/Core/Commands/FileOpenCommand.h>
#include <WinCommander/Core/Commands/LegacyShortcutBindingAdapter.h>
#include <WinCommander/Core/Commands/PaneNavigationCommand.h>
#include <algorithm>
#include <array>
#include <ranges>
#include <string>
#include <vector>

namespace {

using nc::core::CommandDescriptor;
using nc::core::CommandId;
using nc::core::LegacyCommandMetadata;
using nc::core::LegacyShortcutBindingAdapter;
using nc::utility::ActionShortcut;
using nc::utility::ActionsShortcutsManager;

class FakeActionsShortcutsManager final : public ActionsShortcutsManager
{
public:
    struct Binding {
        std::string action;
        int tag = 0;
        Shortcuts defaults;
        Shortcuts current;
    };

    void Add(std::string _action, const int _tag, Shortcuts _shortcuts)
    {
        m_Bindings.emplace_back(Binding{
            .action = std::move(_action),
            .tag = _tag,
            .defaults = _shortcuts,
            .current = std::move(_shortcuts),
        });
    }

    [[nodiscard]] std::optional<int> TagFromAction(const std::string_view _action) const noexcept override
    {
        const Binding *const binding = FindByAction(_action);
        return binding ? std::optional{binding->tag} : std::nullopt;
    }

    [[nodiscard]] std::optional<std::string_view> ActionFromTag(const int _tag) const noexcept override
    {
        const Binding *const binding = FindByTag(_tag);
        return binding ? std::optional{std::string_view{binding->action}} : std::nullopt;
    }

    [[nodiscard]] std::optional<Shortcuts> ShortcutsFromAction(const std::string_view _action) const noexcept override
    {
        const Binding *const binding = FindByAction(_action);
        return binding ? std::optional{binding->current} : std::nullopt;
    }

    [[nodiscard]] std::optional<Shortcuts> ShortcutsFromTag(const int _tag) const noexcept override
    {
        const Binding *const binding = FindByTag(_tag);
        return binding ? std::optional{binding->current} : std::nullopt;
    }

    [[nodiscard]] std::optional<Shortcuts> DefaultShortcutsFromTag(const int _tag) const noexcept override
    {
        const Binding *const binding = FindByTag(_tag);
        return binding ? std::optional{binding->defaults} : std::nullopt;
    }

    [[nodiscard]] std::optional<ActionTags>
    ActionTagsFromShortcut(const Shortcut _shortcut, const std::string_view _in_domain = {}) const noexcept override
    {
        ActionTags tags;
        for( const Binding &binding : m_Bindings ) {
            if( !_in_domain.empty() && !binding.action.starts_with(_in_domain) )
                continue;
            if( std::ranges::find(binding.current, _shortcut) == binding.current.end() )
                continue;
            if( std::ranges::find(tags, binding.tag) == tags.end() )
                tags.emplace_back(binding.tag);
        }
        return tags.empty() ? std::nullopt : std::optional{std::move(tags)};
    }

    [[nodiscard]] std::optional<int>
    FirstOfActionTagsFromShortcut(const std::span<const int> _of_tags,
                                  const Shortcut _shortcut,
                                  const std::string_view _in_domain = {}) const noexcept override
    {
        const auto tags = ActionTagsFromShortcut(_shortcut, _in_domain);
        if( !tags )
            return std::nullopt;
        for( const int requested_tag : _of_tags )
            if( std::ranges::find(*tags, requested_tag) != tags->end() )
                return requested_tag;
        return std::nullopt;
    }

    [[nodiscard]] std::vector<std::pair<std::string, int>> AllShortcuts() const override
    {
        std::vector<std::pair<std::string, int>> result;
        result.reserve(m_Bindings.size());
        for( const Binding &binding : m_Bindings )
            result.emplace_back(binding.action, binding.tag);
        return result;
    }

    void RevertToDefaults() override
    {
        for( Binding &binding : m_Bindings )
            binding.current = binding.defaults;
    }

    bool SetShortcutOverride(const std::string_view _action, const Shortcut _shortcut) override
    {
        return SetShortcutsOverride(_action, std::span<const Shortcut>{&_shortcut, 1});
    }

    bool SetShortcutsOverride(const std::string_view _action, const std::span<const Shortcut> _shortcuts) override
    {
        Binding *const binding = FindByAction(_action);
        if( !binding )
            return false;
        const Shortcuts replacement{_shortcuts.begin(), _shortcuts.end()};
        if( binding->current == replacement )
            return false;
        binding->current = replacement;
        return true;
    }

private:
    [[nodiscard]] Binding *FindByAction(const std::string_view _action) noexcept
    {
        const auto iterator = std::ranges::find(m_Bindings, _action, &Binding::action);
        return iterator == m_Bindings.end() ? nullptr : &*iterator;
    }

    [[nodiscard]] const Binding *FindByAction(const std::string_view _action) const noexcept
    {
        const auto iterator = std::ranges::find(m_Bindings, _action, &Binding::action);
        return iterator == m_Bindings.end() ? nullptr : &*iterator;
    }

    [[nodiscard]] const Binding *FindByTag(const int _tag) const noexcept
    {
        const auto iterator = std::ranges::find(m_Bindings, _tag, &Binding::tag);
        return iterator == m_Bindings.end() ? nullptr : &*iterator;
    }

    std::vector<Binding> m_Bindings;
};

CommandDescriptor Descriptor(const std::string_view _id,
                             std::vector<std::string> _action_names,
                             const std::optional<int> _tag = std::nullopt)
{
    CommandDescriptor descriptor;
    descriptor.id = CommandId{_id};
    descriptor.legacy = LegacyCommandMetadata{
        .shortcut_action_names = std::move(_action_names),
        .shortcut_tag = _tag,
    };
    return descriptor;
}

std::vector<std::string> ActionNames(const LegacyShortcutBindingAdapter &_adapter, const std::string_view _id)
{
    const auto names = _adapter.LegacyActionNames(CommandId{_id});
    return {names.begin(), names.end()};
}

std::vector<std::string> IdValues(const std::vector<CommandId> &_ids)
{
    std::vector<std::string> values;
    values.reserve(_ids.size());
    for( const CommandId &id : _ids )
        values.emplace_back(id.Value());
    return values;
}

} // namespace

#define PREFIX "LegacyShortcutBindingAdapter "

TEST_CASE(PREFIX "indexes stable ids and de-duplicates legacy action and tag bindings")
{
    FakeActionsShortcutsManager manager;
    manager.Add("menu.edit.copy", 12'000, {ActionShortcut{"⌘c"}});
    manager.Add("panel.copy", 100'001, {ActionShortcut{"⌘k"}});

    const std::array descriptors{
        Descriptor("file.copy", {"menu.edit.copy", "menu.edit.copy"}, 12'000),
        Descriptor("file.copy", {"panel.copy"}, 100'001),
        Descriptor("file.unbound", {}),
    };
    const LegacyShortcutBindingAdapter adapter{descriptors, manager};

    CHECK(ActionNames(adapter, "file.copy") == std::vector<std::string>{"menu.edit.copy", "panel.copy"});
    CHECK(adapter.CurrentShortcuts(CommandId{"file.copy"}) ==
          LegacyShortcutBindingAdapter::Shortcuts{ActionShortcut{"⌘c"}, ActionShortcut{"⌘k"}});
    CHECK(adapter.CurrentShortcuts(CommandId{"file.unbound"}).empty());
    CHECK(adapter.LegacyActionNames(CommandId{"file.unknown"}).empty());

    const auto copy = adapter.Resolve(ActionShortcut{"⌘c"});
    CHECK(copy.status == LegacyShortcutBindingAdapter::ResolveStatus::Resolved);
    CHECK(copy.command_id == CommandId{"file.copy"});
    CHECK(copy.ambiguous_command_ids.empty());
}

TEST_CASE(PREFIX "reads current overrides and empty bindings without rebuilding its index")
{
    FakeActionsShortcutsManager manager;
    manager.Add("menu.edit.copy", 12'000, {ActionShortcut{"⌘c"}});
    const std::array descriptors{Descriptor("file.copy", {"menu.edit.copy"}, 12'000)};
    const LegacyShortcutBindingAdapter adapter{descriptors, manager};

    REQUIRE(manager.SetShortcutOverride("menu.edit.copy", ActionShortcut{"⌘j"}));
    CHECK(adapter.Resolve(ActionShortcut{"⌘c"}).status == LegacyShortcutBindingAdapter::ResolveStatus::NotFound);
    CHECK(adapter.Resolve(ActionShortcut{"⌘j"}).command_id == CommandId{"file.copy"});
    CHECK(adapter.CurrentShortcuts(CommandId{"file.copy"}) ==
          LegacyShortcutBindingAdapter::Shortcuts{ActionShortcut{"⌘j"}});

    REQUIRE(manager.SetShortcutsOverride("menu.edit.copy", {}));
    CHECK(adapter.CurrentShortcuts(CommandId{"file.copy"}).empty());
    CHECK(adapter.Resolve(ActionShortcut{"⌘j"}).status == LegacyShortcutBindingAdapter::ResolveStatus::NotFound);
    CHECK(adapter.Resolve(ActionShortcut{}).status == LegacyShortcutBindingAdapter::ResolveStatus::NotFound);
}

TEST_CASE(PREFIX "reports duplicate legacy actions and tags as deterministic ambiguity")
{
    FakeActionsShortcutsManager manager;
    manager.Add("menu.edit.copy", 12'000, {ActionShortcut{"⌘c"}});
    const std::array descriptors{
        Descriptor("file.duplicate", {"menu.edit.copy"}, 12'000),
        Descriptor("file.copy", {"menu.edit.copy"}, 12'000),
    };
    const LegacyShortcutBindingAdapter adapter{descriptors, manager};

    const auto result = adapter.Resolve(ActionShortcut{"⌘c"});
    CHECK(result.status == LegacyShortcutBindingAdapter::ResolveStatus::Ambiguous);
    CHECK_FALSE(result.command_id);
    CHECK(IdValues(result.ambiguous_command_ids) == std::vector<std::string>{"file.copy", "file.duplicate"});
}

TEST_CASE(PREFIX "reports colliding current shortcuts from distinct legacy actions as ambiguity")
{
    FakeActionsShortcutsManager manager;
    manager.Add("menu.edit.copy", 12'000, {ActionShortcut{"⌘c"}});
    manager.Add("menu.file.duplicate", 11'150, {ActionShortcut{"⌘d"}});
    const std::array descriptors{
        Descriptor("file.duplicate", {"menu.file.duplicate"}, 11'150),
        Descriptor("file.copy", {"menu.edit.copy"}, 12'000),
    };
    const LegacyShortcutBindingAdapter adapter{descriptors, manager};

    REQUIRE(manager.SetShortcutOverride("menu.file.duplicate", ActionShortcut{"⌘c"}));
    const auto result = adapter.Resolve(ActionShortcut{"⌘c"});
    CHECK(result.status == LegacyShortcutBindingAdapter::ResolveStatus::Ambiguous);
    CHECK_FALSE(result.command_id);
    CHECK(IdValues(result.ambiguous_command_ids) == std::vector<std::string>{"file.copy", "file.duplicate"});
}

TEST_CASE(PREFIX "derives the legacy persistence key for tag-only metadata")
{
    FakeActionsShortcutsManager manager;
    manager.Add("menu.edit.copy", 12'000, {ActionShortcut{"⌘c"}});
    const std::array descriptors{Descriptor("file.copy", {}, 12'000)};
    const LegacyShortcutBindingAdapter adapter{descriptors, manager};

    CHECK(ActionNames(adapter, "file.copy") == std::vector<std::string>{"menu.edit.copy"});
    CHECK(adapter.Resolve(ActionShortcut{"⌘c"}).command_id == CommandId{"file.copy"});
}

TEST_CASE(PREFIX "resolves pane navigation aliases to their stable command ids")
{
    FakeActionsShortcutsManager manager;
    manager.Add("menu.go.enclosing_folder", 14'020, {ActionShortcut{"⌘\uF700"}});
    manager.Add("panel.go_into_enclosing_folder", 100'120, {ActionShortcut{"\u007f"}});
    manager.Add("menu.view.refresh", 13'040, {ActionShortcut{"⌘r"}});

    const auto up = nc::core::MakeNavigationUpCommand([](void *) { return true; });
    const auto refresh = nc::core::MakeNavigationRefreshCommand([](void *) { return true; });
    const std::array descriptors{up.descriptor, refresh.descriptor};
    const LegacyShortcutBindingAdapter adapter{descriptors, manager};

    struct Expected {
        ActionShortcut shortcut;
        std::string_view command_id;
    };
    const std::array expected{
        Expected{ActionShortcut{"⌘\uF700"}, "navigation.up"},
        Expected{ActionShortcut{"\u007f"}, "navigation.up"},
        Expected{ActionShortcut{"⌘r"}, "navigation.refresh"},
    };
    for( const Expected &value : expected ) {
        CAPTURE(value.command_id);
        const auto result = adapter.Resolve(value.shortcut);
        CHECK(result.status == LegacyShortcutBindingAdapter::ResolveStatus::Resolved);
        CHECK(result.command_id == CommandId{value.command_id});
        CHECK(result.ambiguous_command_ids.empty());
    }
}

TEST_CASE(PREFIX "resolves the persisted file open binding to its stable command id")
{
    FakeActionsShortcutsManager manager;
    manager.Add("menu.file.open", 11'020, {ActionShortcut{"⇧\\r"}});

    const auto open = nc::core::MakeFileOpenCommand([](void *, std::span<const nc::vfs::ListingItem>) {
        return true;
    });
    const std::array descriptors{open.descriptor};
    const LegacyShortcutBindingAdapter adapter{descriptors, manager};

    CHECK(ActionNames(adapter, "file.open") == std::vector<std::string>{"menu.file.open"});
    const auto result = adapter.Resolve(ActionShortcut{"⇧\\r"});
    CHECK(result.status == LegacyShortcutBindingAdapter::ResolveStatus::Resolved);
    CHECK(result.command_id == CommandId{"file.open"});
    CHECK(result.ambiguous_command_ids.empty());
}
