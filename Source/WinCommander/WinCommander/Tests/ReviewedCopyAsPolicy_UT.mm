// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Operations/CopyingOptions.h>
#include <VFS/VFS.h>
#include <VFS/VFSListingInput.h>
#include <WinCommander/Core/Operations/OperationSubmissionGate.h>
#include <WinCommander/States/FilePanels/Actions/CopyFile.h>

#include <chrono>
#include <future>
#include <memory>
#include <sys/dirent.h>
#include <sys/stat.h>
#include <thread>

namespace {

using nc::ops::CopyingOptions;
using nc::panel::actions::reviewed_copy_as::Select;
using nc::panel::actions::reviewed_copy_as::Selection;
using nc::vfs::ListingItem;

class ReviewedCopyAsTestHost final : public nc::vfs::Host
{
public:
    explicit ReviewedCopyAsTestHost(const bool _native,
                                    const nc::vfs::ProviderConditionalCopyPathSupport _path_support =
                                        nc::vfs::ProviderConditionalCopyPathSupport::Supported)
        : Host("/", nullptr, _native ? "reviewed_copy_as_native" : "reviewed_copy_as_remote"), m_Native(_native),
          m_PathSupport(_path_support)
    {
    }

    bool IsNativeFS() const noexcept override { return m_Native; }
    nc::vfs::ProviderConditionalCopyPathSupport ConditionalCopyPathSupport(std::string_view,
                                                                           std::string_view) const noexcept override
    {
        return m_PathSupport;
    }

private:
    bool m_Native;
    nc::vfs::ProviderConditionalCopyPathSupport m_PathSupport;
};

ListingItem Item(const std::shared_ptr<ReviewedCopyAsTestHost> &_host,
                 const mode_t _mode = S_IFREG | S_IRUSR | S_IWUSR,
                 const uint8_t _type = DT_REG)
{
    nc::vfs::ListingInput input;
    input.directories.reset(nc::base::variable_container<>::type::common);
    input.directories[0] = "/source/";
    input.hosts.reset(nc::base::variable_container<>::type::common);
    input.hosts[0] = _host;
    input.filenames.emplace_back("source.txt");
    input.unix_modes.emplace_back(_mode);
    input.unix_types.emplace_back(_type);
    return VFSListing::Build(std::move(input))->Item(0);
}

} // namespace

#define PREFIX "reviewed CopyAs policy "

TEST_CASE(PREFIX "accepts only the exact default single-file Native shape")
{
    const auto host = std::make_shared<ReviewedCopyAsTestHost>(true);
    const auto item = Item(host);
    CopyingOptions options;

    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Reviewed);

    options.verification = CopyingOptions::ChecksumVerification::WhenMoves;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Reviewed);

    const auto other_native_host = std::make_shared<ReviewedCopyAsTestHost>(true);
    CHECK(Select(item, "/source/copy.txt", other_native_host, options) == Selection::Legacy);
    CHECK(Select(item, "/other/copy.txt", host, options) == Selection::Legacy);
    CHECK(Select(item, "copy.txt", host, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "rejects provider and item shapes outside the reviewed lifecycle")
{
    const auto native = std::make_shared<ReviewedCopyAsTestHost>(true);
    const auto remote = std::make_shared<ReviewedCopyAsTestHost>(false);
    CopyingOptions options;

    CHECK(Select(Item(remote), "/source/copy.txt", remote, options) == Selection::Legacy);
    CHECK(Select(Item(native, S_IFDIR | S_IRUSR | S_IWUSR, DT_DIR), "/source/copy.txt", native, options) ==
          Selection::Legacy);
    CHECK(Select(Item(native), "/source/copy.txt", {}, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "rejects copy preferences that the clone-only product does not consume")
{
    const auto host = std::make_shared<ReviewedCopyAsTestHost>(true);
    const auto item = Item(host);
    CopyingOptions options;

    options.verification = CopyingOptions::ChecksumVerification::Always;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);

    options = {};
    options.disable_system_caches = true;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);

    options = {};
    options.copy_xattrs = false;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);

    options = {};
    options.exist_behavior = CopyingOptions::ExistBehavior::KeepBoth;
    CHECK(Select(item, "/source/copy.txt", host, options) == Selection::Legacy);
}

TEST_CASE(PREFIX "distinguishes known unsupported storage from unavailable eligibility evidence")
{
    using nc::vfs::ProviderConditionalCopyPathSupport;
    const auto unsupported =
        std::make_shared<ReviewedCopyAsTestHost>(true, ProviderConditionalCopyPathSupport::Unsupported);
    const auto unavailable =
        std::make_shared<ReviewedCopyAsTestHost>(true, ProviderConditionalCopyPathSupport::Unavailable);
    CopyingOptions options;

    CHECK(Select(Item(unsupported), "/source/copy.txt", unsupported, options) == Selection::Legacy);
    CHECK(Select(Item(unavailable), "/source/copy.txt", unavailable, options) == Selection::Reject);
}

TEST_CASE(PREFIX "submission gate accounts for every admission ticket")
{
    nc::core::OperationSubmissionGate gate;
    CHECK_FALSE(gate.HasPending());
    CHECK(gate.PendingCount() == 0);

    auto first = gate.Acquire();
    auto second = gate.Acquire();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(gate.HasPending());
    CHECK(gate.PendingCount() == 2);

    first.reset();
    CHECK(gate.PendingCount() == 1);
    second.reset();
    CHECK_FALSE(gate.HasPending());
}

TEST_CASE(PREFIX "submission gate cancels monotonically and waits for owners")
{
    using namespace std::chrono_literals;

    nc::core::OperationSubmissionGate gate;
    auto ticket = gate.Acquire();
    REQUIRE(ticket);

    auto cancellation = std::async(std::launch::async, [&gate] { gate.CancelAndWait(); });
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while( !ticket->IsCancelled() && std::chrono::steady_clock::now() < deadline )
        std::this_thread::yield();

    CHECK(ticket->IsCancelled());
    CHECK_FALSE(gate.Acquire());
    CHECK(cancellation.wait_for(0ms) == std::future_status::timeout);

    ticket.reset();
    CHECK(cancellation.wait_for(1s) == std::future_status::ready);
    cancellation.get();
    CHECK_FALSE(gate.Acquire());
}
