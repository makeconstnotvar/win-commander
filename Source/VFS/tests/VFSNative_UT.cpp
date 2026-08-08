// Copyright (C) 2020-2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"
#include "TestEnv.h"
#include <NativeSpecialDirectories.h>
#include <Base/algo.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <unordered_set>

#define PREFIX "VFSNative "

namespace VFSNativeTests {

using namespace nc;
using namespace nc::vfs;
using namespace nc::vfs::native;

static VFSNativeHost &host()
{
    return *TestEnv().vfs_native;
}

static bool ListingHas(const Listing &listing, const std::string &_filename)
{
    return std::ranges::any_of(listing, [&](auto &item) { return item.Filename() == _filename; });
};

static bool ListingHas(const VFSListingPtr &listing, const std::string &_filename)
{
    return ListingHas(*listing, _filename);
};

static void CheckListingsEqual(const Listing &_lhs, const Listing &_rhs)
{
    REQUIRE(_lhs.Count() == _rhs.Count());
    for( unsigned index = 0; index != _lhs.Count(); ++index ) {
        CHECK(_lhs.Host(index) == _rhs.Host(index));
        CHECK(_lhs.Directory(index) == _rhs.Directory(index));
        CHECK(_lhs.Filename(index) == _rhs.Filename(index));
        CHECK(_lhs.UnixMode(index) == _rhs.UnixMode(index));
        CHECK(_lhs.UnixType(index) == _rhs.UnixType(index));

#define CHECK_OPTIONAL_FIELD(_has, _value)                                                                             \
    CHECK(_lhs._has(index) == _rhs._has(index));                                                                       \
    if( _lhs._has(index) && _rhs._has(index) )                                                                         \
    CHECK(_lhs._value(index) == _rhs._value(index))

        CHECK_OPTIONAL_FIELD(HasDisplayFilename, DisplayFilename);
        CHECK_OPTIONAL_FIELD(HasSize, Size);
        CHECK_OPTIONAL_FIELD(HasInode, Inode);
        CHECK_OPTIONAL_FIELD(HasATime, ATime);
        CHECK_OPTIONAL_FIELD(HasMTime, MTime);
        CHECK_OPTIONAL_FIELD(HasCTime, CTime);
        CHECK_OPTIONAL_FIELD(HasBTime, BTime);
        CHECK_OPTIONAL_FIELD(HasAddTime, AddTime);
        CHECK_OPTIONAL_FIELD(HasUID, UID);
        CHECK_OPTIONAL_FIELD(HasGID, GID);
        CHECK_OPTIONAL_FIELD(HasUnixFlags, UnixFlags);
        CHECK_OPTIONAL_FIELD(HasSymlink, Symlink);
#undef CHECK_OPTIONAL_FIELD

        CHECK(_lhs.HasTags(index) == _rhs.HasTags(index));
        if( _lhs.HasTags(index) && _rhs.HasTags(index) )
            CHECK(std::ranges::equal(_lhs.Tags(index), _rhs.Tags(index)));
    }
}

TEST_CASE(PREFIX "Does produces unified Application directory")
{
    const auto marker_path = "/Applications/__nc_fetch_probe__";
    auto rm_marker = [&] { unlink(marker_path); };
    rm_marker();
    REQUIRE(close(creat(marker_path, 0755)) == 0);
    auto marker_cleanup = at_scope_end([&] { rm_marker(); });

    std::expected<VFSListingPtr, Error> listing;
    SECTION("No ..")
    {
        listing = FetchUnifiedApplicationsListing(host(), Flags::F_NoDotDot);
    }
    SECTION("With ..")
    {
        listing = FetchUnifiedApplicationsListing(host(), Flags::None);
    }

    REQUIRE(listing);
    REQUIRE(*listing != nullptr);
    REQUIRE((*listing)->IsUniform() == false);
    CHECK(ListingHas(*listing, "..") == false);
    CHECK(ListingHas(*listing, "Mail.app"));
    CHECK(ListingHas(*listing, "__nc_fetch_probe__"));
    CHECK(ListingHas(*listing, "some_meaningless_rubbish_that_nobody_would_every_have") == false);
}

TEST_CASE(PREFIX "FetchUnifiedListing fetches contents from both directories")
{
    const TestDir test_dir_holder;
    const std::string test_dir = test_dir_holder.directory;

    REQUIRE(mkdir((test_dir + "A").c_str(), 0755) == 0);
    REQUIRE(mkdir((test_dir + "B").c_str(), 0755) == 0);
    REQUIRE(close(creat((test_dir + "A/a").c_str(), 0755)) == 0);
    REQUIRE(close(creat((test_dir + "B/b").c_str(), 0755)) == 0);

    const std::expected<VFSListingPtr, Error> listing =
        FetchUnifiedListing(host(), test_dir + "A", test_dir + "B", VFSFlags::None);
    REQUIRE(listing);
    REQUIRE(*listing != nullptr);
    REQUIRE((*listing)->IsUniform() == false);
    CHECK((*listing)->Count() == 2);
    CHECK(ListingHas(*listing, "a"));
    CHECK(ListingHas(*listing, "b"));
}

TEST_CASE(PREFIX "FetchUnifiedListing succeeds when user directory doesn't exist")
{
    const TestDir test_dir_holder;
    const std::string test_dir = test_dir_holder.directory;

    REQUIRE(mkdir((test_dir + "A").c_str(), 0755) == 0);
    REQUIRE(close(creat((test_dir + "A/a").c_str(), 0755)) == 0);

    const std::expected<VFSListingPtr, Error> listing =
        FetchUnifiedListing(host(), test_dir + "A", test_dir + "B", VFSFlags::None);
    REQUIRE(listing);
    REQUIRE(*listing != nullptr);
    REQUIRE((*listing)->IsUniform() == true);
    CHECK((*listing)->Count() == 1);
    CHECK(ListingHas(*listing, "a"));
}

TEST_CASE(PREFIX "Loading tags")
{
    using Color = utility::Tags::Color;

    unsigned char xattr_bytes_green[] = {0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xa1, 0x01, 0x57, 0x47, 0x72,
                                         0x65, 0x65, 0x6e, 0x0a, 0x32, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12};
    unsigned char xattr_bytes_blue[] = {0x62, 0x70, 0x6c, 0x69, 0x73, 0x74, 0x30, 0x30, 0xa1, 0x01, 0x56, 0x42, 0x6c,
                                        0x75, 0x65, 0x0a, 0x34, 0x08, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
                                        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11};

    const TestDir test_dir_holder;
    auto test_dir = test_dir_holder.directory;
    REQUIRE(close(creat((test_dir / "1.txt").c_str(), 0755)) == 0);
    REQUIRE(setxattr((test_dir / "1.txt").c_str(),
                     "com.apple.metadata:_kMDItemUserTags",
                     xattr_bytes_green,
                     sizeof(xattr_bytes_green),
                     0,
                     0) == 0);
    REQUIRE(close(creat((test_dir / "2.txt").c_str(), 0755)) == 0);
    REQUIRE(setxattr((test_dir / "2.txt").c_str(),
                     "com.apple.metadata:_kMDItemUserTags",
                     xattr_bytes_blue,
                     sizeof(xattr_bytes_blue),
                     0,
                     0) == 0);

    {
        const std::expected<VFSListingPtr, Error> exp_listing =
            host().FetchDirectoryListing(test_dir.c_str(), Flags::F_NoDotDot | Flags::F_LoadTags);
        REQUIRE(exp_listing);
        REQUIRE(*exp_listing);
        const VFSListing &listing = **exp_listing;
        REQUIRE(listing.Count() == 2);
        REQUIRE(listing.HasTags(0));
        REQUIRE(listing.Tags(0).size() == 1);
        REQUIRE(listing.Tags(0)[0].Label() == (listing.Filename(0) == "1.txt" ? "Green" : "Blue"));
        REQUIRE(listing.Tags(0)[0].Color() == (listing.Filename(0) == "1.txt" ? Color::Green : Color::Blue));
        REQUIRE(listing.HasTags(1));
        REQUIRE(listing.Tags(1).size() == 1);
        REQUIRE(listing.Tags(1)[0].Label() == (listing.Filename(1) == "1.txt" ? "Green" : "Blue"));
        REQUIRE(listing.Tags(1)[0].Color() == (listing.Filename(1) == "1.txt" ? Color::Green : Color::Blue));
    }
    {
        const std::expected<VFSListingPtr, Error> exp_listing =
            host().FetchDirectoryListing(test_dir.c_str(), Flags::F_NoDotDot);
        REQUIRE(exp_listing);
        REQUIRE(*exp_listing);
        const VFSListing &listing = **exp_listing;
        REQUIRE(listing.Count() == 2);
        REQUIRE(!listing.HasTags(0));
        REQUIRE(!listing.HasTags(1));
    }
    {
        const std::expected<VFSListingPtr, Error> exp_listing =
            host().FetchSingleItemListing((test_dir / "1.txt").c_str(), Flags::F_NoDotDot | Flags::F_LoadTags).value();
        REQUIRE(exp_listing);
        REQUIRE(*exp_listing);
        const VFSListing &listing = **exp_listing;
        REQUIRE(listing.Count() == 1);
        REQUIRE(listing.HasTags(0));
        REQUIRE(listing.Tags(0).size() == 1);
        REQUIRE(listing.Tags(0)[0].Label() == "Green");
        REQUIRE(listing.Tags(0)[0].Color() == Color::Green);
    }
    {
        const std::expected<VFSListingPtr, Error> exp_listing =
            host().FetchSingleItemListing((test_dir / "1.txt").c_str(), Flags::F_NoDotDot).value();
        REQUIRE(exp_listing);
        REQUIRE(*exp_listing);
        const VFSListing &listing = **exp_listing;
        REQUIRE(listing.Count() == 1);
        REQUIRE(!listing.HasTags(0));
    }
}

TEST_CASE(PREFIX "FetchUsers")
{
    const std::expected<std::vector<VFSUser>, Error> users = host().FetchUsers();
    REQUIRE(users);
    REQUIRE(std::ranges::contains(users.value(), VFSUser{0, "root", "System Administrator"}));
    REQUIRE(std::ranges::contains(users.value(), VFSUser{1, "daemon", "System Services"}));
}

TEST_CASE(PREFIX "FetchGroups")
{
    const std::expected<std::vector<VFSGroup>, Error> groups = host().FetchGroups();
    REQUIRE(groups);
    REQUIRE(std::ranges::contains(groups.value(), VFSGroup{0, "wheel", "System Group"}));
    REQUIRE(std::ranges::contains(groups.value(), VFSGroup{20, "staff", "Staff"}));
}

TEST_CASE(PREFIX "FetchDirectoryListing reads symlink value and resolves target type")
{
    const TestDir test_dir_holder;
    const std::filesystem::path test_dir = test_dir_holder.directory;

    // Create a regular file with known content
    const std::filesystem::path target_path = test_dir / "file.txt";
    std::ofstream{target_path} << "Hello, world!";

    // Generate 50 random, unique symlink names and create them pointing to the file (relative target)
    std::mt19937 rng(123456);
    const std::string_view alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<int> len_dist(6, 16);
    std::uniform_int_distribution<int> ch_dist(0, static_cast<int>(alphabet.length() - 1));

    std::unordered_set<std::string> link_names;
    while( link_names.size() < 50 ) {
        const int len = len_dist(rng);
        std::string name;
        for( int i = 0; i < len; ++i )
            name += alphabet[ch_dist(rng)];
        name += ".lnk";
        if( name == "file.txt" )
            continue; // just in case
        link_names.insert(std::move(name));
    }

    for( const auto &name : link_names )
        REQUIRE_NOTHROW(std::filesystem::create_symlink("file.txt", test_dir / name));

    const auto listing_exp = host().FetchDirectoryListing(test_dir.c_str(), Flags::F_NoDotDot);
    REQUIRE(listing_exp);
    REQUIRE(*listing_exp);
    const VFSListing &listing = **listing_exp;
    REQUIRE(listing.Count() == 1 + link_names.size());

    int idx_file = -1;
    for( unsigned i = 0; i != listing.Count(); ++i )
        if( listing.Filename(i) == "file.txt" ) {
            idx_file = static_cast<int>(i);
            break;
        }
    REQUIRE(idx_file >= 0);

    // Verify the regular file entry
    CHECK(listing.IsReg(idx_file));
    CHECK(!listing.IsSymlink(idx_file));
    CHECK(!listing.HasSymlink(idx_file));
    const uint64_t file_size = listing.Size(idx_file);

    // Verify that all other entries are the generated symlinks and resolve to the target type/size
    for( unsigned i = 0; i != listing.Count(); ++i ) {
        if( static_cast<int>(i) == idx_file )
            continue;
        CHECK(link_names.contains(listing.Filename(i)));
        CHECK(listing.IsSymlink(i));
        CHECK(listing.HasSymlink(i));
        CHECK(listing.Symlink(i) == "file.txt");
        CHECK(listing.IsReg(i));
        CHECK(listing.Size(i) == file_size);
    }
}

TEST_CASE(PREFIX "progressive directory batches are enriched immutable deltas matching the final listing")
{
    const TestDir test_dir_holder;
    const std::filesystem::path test_dir = test_dir_holder.directory;

    constexpr size_t regular_files_count = 1024;
    for( size_t index = 0; index != regular_files_count; ++index )
        REQUIRE(close(creat((test_dir / fmt::format("regular-{:04}", index)).c_str(), 0600)) == 0);

    const std::filesystem::path target_path = test_dir / "target.txt";
    std::ofstream{target_path} << "progressive-target";
    REQUIRE_NOTHROW(std::filesystem::create_symlink("target.txt", test_dir / "target-link"));

    const std::filesystem::path tagged_path = test_dir / "tagged.txt";
    REQUIRE(close(creat(tagged_path.c_str(), 0600)) == 0);
    const std::vector<utility::Tags::Tag> expected_tags{
        {utility::Tags::Tag::Internalize("Progressive"), utility::Tags::Color::Green}};
    REQUIRE(utility::Tags::WriteTags(tagged_path, expected_tags));

    std::vector<VFSListingPtr> batches;
    size_t next_batch_index = 0;
    bool saw_dot_dot = false;
    bool saw_enriched_symlink = false;
    bool saw_enriched_tags = false;
    const auto result = host().FetchDirectoryListingProgressively(
        test_dir.c_str(),
        Flags::F_LoadTags,
        [&](DirectoryListingBatch &&_batch) {
            REQUIRE(_batch.entries);
            CHECK(_batch.first_index == next_batch_index);
            if( _batch.first_index == 0 ) {
                REQUIRE(_batch.entries->Count() > 0);
                CHECK(_batch.entries->Filename(0) == "..");
                saw_dot_dot = _batch.entries->Filename(0) == "..";
            }

            for( unsigned index = 0; index != _batch.entries->Count(); ++index ) {
                if( _batch.entries->Filename(index) == "target-link" ) {
                    CHECK(_batch.entries->IsSymlink(index));
                    CHECK(_batch.entries->HasSymlink(index));
                    CHECK(_batch.entries->Symlink(index) == "target.txt");
                    CHECK(_batch.entries->IsReg(index));
                    CHECK(_batch.entries->Size(index) == std::string_view("progressive-target").size());
                    saw_enriched_symlink = true;
                }
                if( _batch.entries->Filename(index) == "tagged.txt" ) {
                    REQUIRE(_batch.entries->HasTags(index));
                    CHECK(std::ranges::equal(_batch.entries->Tags(index), expected_tags));
                    saw_enriched_tags = true;
                }
            }

            next_batch_index += _batch.entries->Count();
            batches.emplace_back(std::move(_batch.entries));
            return DirectoryListingBatchDisposition::Continue;
        });

    REQUIRE(result);
    REQUIRE(*result);
    CHECK(batches.size() > 1);
    CHECK(saw_dot_dot);
    CHECK(saw_enriched_symlink);
    CHECK(saw_enriched_tags);
    CHECK(next_batch_index == (*result)->Count());

    const VFSListingPtr concatenated = VFSListing::Build(VFSListing::Compose(batches));
    REQUIRE(concatenated);
    CheckListingsEqual(*concatenated, **result);
}

TEST_CASE(PREFIX "progressive directory callback cancellation fails closed")
{
    const TestDir test_dir_holder;
    const std::filesystem::path test_dir = test_dir_holder.directory;
    for( size_t index = 0; index != 600; ++index )
        REQUIRE(close(creat((test_dir / fmt::format("entry-{:04}", index)).c_str(), 0600)) == 0);

    size_t callback_count = 0;
    const auto result = host().FetchDirectoryListingProgressively(
        test_dir.c_str(),
        Flags::F_NoDotDot,
        [&](DirectoryListingBatch &&) {
            ++callback_count;
            return DirectoryListingBatchDisposition::Cancel;
        });

    REQUIRE_FALSE(result);
    CHECK(result.error() == Error{Error::POSIX, ECANCELED});
    CHECK(callback_count == 1);
}

TEST_CASE(PREFIX "progressive directory callback can stop publishing while the final fetch continues")
{
    const TestDir test_dir_holder;
    const std::filesystem::path test_dir = test_dir_holder.directory;
    constexpr size_t files_count = 1024;
    for( size_t index = 0; index != files_count; ++index )
        REQUIRE(close(creat((test_dir / fmt::format("entry-{:04}", index)).c_str(), 0600)) == 0);

    size_t callback_count = 0;
    size_t preview_count = 0;
    const auto result = host().FetchDirectoryListingProgressively(
        test_dir.c_str(),
        Flags::F_NoDotDot,
        [&](DirectoryListingBatch &&_batch) {
            ++callback_count;
            preview_count = _batch.entries->Count();
            return DirectoryListingBatchDisposition::StopPublishing;
        });

    REQUIRE(result);
    REQUIRE(*result);
    CHECK(callback_count == 1);
    CHECK(preview_count > 0);
    CHECK(preview_count < files_count);
    CHECK((*result)->Count() == files_count);
    for( size_t index = 0; index != files_count; ++index )
        CHECK(ListingHas(*result, fmt::format("entry-{:04}", index)));
}

TEST_CASE(PREFIX "progressive directory cancellation checker suppresses later batches and final result")
{
    const TestDir test_dir_holder;
    const std::filesystem::path test_dir = test_dir_holder.directory;
    for( size_t index = 0; index != 600; ++index )
        REQUIRE(close(creat((test_dir / fmt::format("entry-{:04}", index)).c_str(), 0600)) == 0);

    bool cancelled = false;
    size_t callback_count = 0;
    const auto result = host().FetchDirectoryListingProgressively(
        test_dir.c_str(),
        Flags::F_NoDotDot,
        [&](DirectoryListingBatch &&) {
            ++callback_count;
            cancelled = true;
            return DirectoryListingBatchDisposition::Continue;
        },
        [&] { return cancelled; });

    REQUIRE_FALSE(result);
    CHECK(result.error() == Error{Error::POSIX, ECANCELED});
    CHECK(callback_count == 1);
}

TEST_CASE(PREFIX "progressive directory callback exceptions retain their original type and message")
{
    const TestDir test_dir_holder;
    const std::filesystem::path test_dir = test_dir_holder.directory;
    for( size_t index = 0; index != 600; ++index )
        REQUIRE(close(creat((test_dir / fmt::format("entry-{:04}", index)).c_str(), 0600)) == 0);

    size_t callback_count = 0;
    bool caught_expected_exception = false;
    try {
        static_cast<void>(host().FetchDirectoryListingProgressively(
            test_dir.c_str(),
            Flags::F_NoDotDot,
            [&](DirectoryListingBatch &&) -> DirectoryListingBatchDisposition {
                ++callback_count;
                throw std::runtime_error("progressive consumer failure");
            }));
    } catch( const std::runtime_error &exception ) {
        caught_expected_exception = true;
        CHECK(std::string_view(exception.what()) == "progressive consumer failure");
    }

    CHECK(caught_expected_exception);
    CHECK(callback_count == 1);
}

TEST_CASE(PREFIX "progressive directory cancellation interrupts final enrichment after publishing stops")
{
    const TestDir test_dir_holder;
    const std::filesystem::path test_dir = test_dir_holder.directory;
    constexpr size_t files_count = 2048;
    for( size_t index = 0; index != files_count; ++index )
        REQUIRE(close(creat((test_dir / fmt::format("entry-{:04}", index)).c_str(), 0600)) == 0);

    size_t raw_batch_count = 0;
    size_t first_batch_count = 0;
    const auto probe = host().FetchDirectoryListingProgressively(
        test_dir.c_str(),
        Flags::F_NoDotDot,
        [&](DirectoryListingBatch &&_batch) {
            ++raw_batch_count;
            if( first_batch_count == 0 )
                first_batch_count = _batch.entries->Count();
            return DirectoryListingBatchDisposition::Continue;
        });
    REQUIRE(probe);
    REQUIRE(raw_batch_count > 1);
    REQUIRE(first_batch_count + 256 < files_count);

    bool stopped_publishing = false;
    size_t callback_count = 0;
    size_t cancellation_checks_after_stop = 0;
    const auto result = host().FetchDirectoryListingProgressively(
        test_dir.c_str(),
        Flags::F_NoDotDot,
        [&](DirectoryListingBatch &&) {
            ++callback_count;
            stopped_publishing = true;
            return DirectoryListingBatchDisposition::StopPublishing;
        },
        [&] {
            if( !stopped_publishing )
                return false;
            ++cancellation_checks_after_stop;
            return cancellation_checks_after_stop >= raw_batch_count + 3;
        });

    REQUIRE_FALSE(result);
    CHECK(result.error() == Error{Error::POSIX, ECANCELED});
    CHECK(callback_count == 1);
    CHECK(cancellation_checks_after_stop == raw_batch_count + 3);
}

} // namespace VFSNativeTests

#undef PREFIX
