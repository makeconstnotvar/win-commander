// Copyright (C) 2026 Michael Kazakov. Subject to GNU General Public License version 3.
#include "Tests.h"

#include <Operations/ArchiveCreationFormat.h>

#include <set>
#include <string>

namespace {

using nc::ops::ArchiveCreationFormat;
using nc::ops::ArchiveCreationFormatForFilename;
using nc::ops::DescribeArchiveCreationFormat;
using nc::ops::ArchiveCreationRequestVerdict;
using nc::ops::EvaluateArchiveCreationRequest;
using nc::ops::SupportedArchiveCreationFormats;

} // namespace

#define PREFIX "nc::ops::ArchiveCreationFormat "

TEST_CASE(PREFIX "resolves a compound extension to the whole thing, not its tail")
{
    // The failure this prevents: creating backup.tar.gz as a bare gzip stream produces a file named
    // like a tarball that is not one - it opens, yields a single unnamed blob, and the directory
    // structure the user thought they archived is silently absent.
    CHECK(ArchiveCreationFormatForFilename("backup.tar.gz") == ArchiveCreationFormat::TarGzip);
    CHECK(ArchiveCreationFormatForFilename("backup.tar.bz2") == ArchiveCreationFormat::TarBzip2);
    CHECK(ArchiveCreationFormatForFilename("backup.tar") == ArchiveCreationFormat::Tar);
    CHECK(ArchiveCreationFormatForFilename("backup.zip") == ArchiveCreationFormat::Zip);
}

TEST_CASE(PREFIX "matches case-insensitively")
{
    CHECK(ArchiveCreationFormatForFilename("BACKUP.ZIP") == ArchiveCreationFormat::Zip);
    CHECK(ArchiveCreationFormatForFilename("Backup.Tar.Gz") == ArchiveCreationFormat::TarGzip);
}

TEST_CASE(PREFIX "does not read a dotfile as an archive")
{
    // ".zip" is a file called ".zip", not a zip archive with an empty name.
    CHECK_FALSE(ArchiveCreationFormatForFilename(".zip"));
    CHECK_FALSE(ArchiveCreationFormatForFilename(".tar.gz"));
    // A leading dot elsewhere in the name is fine.
    CHECK(ArchiveCreationFormatForFilename(".hidden.zip") == ArchiveCreationFormat::Zip);
}

TEST_CASE(PREFIX "refuses names that only look like an archive")
{
    CHECK_FALSE(ArchiveCreationFormatForFilename(""));
    CHECK_FALSE(ArchiveCreationFormatForFilename("zip"));          // no separating dot
    CHECK_FALSE(ArchiveCreationFormatForFilename("archive.zipx")); // different format
    CHECK_FALSE(ArchiveCreationFormatForFilename("archive.rar"));  // extractable, not creatable
    CHECK_FALSE(ArchiveCreationFormatForFilename("archive.7z"));   // not in the creatable set
    CHECK_FALSE(ArchiveCreationFormatForFilename("notes.txt"));
}

TEST_CASE(PREFIX "every creatable format is described exactly once and round-trips")
{
    const auto formats = SupportedArchiveCreationFormats();
    REQUIRE_FALSE(formats.empty());

    std::set<std::string> extensions;
    for( const auto &info : formats ) {
        INFO("extension: " << std::string{info.extension});
        // A duplicate extension would make resolution order-dependent and the picker ambiguous.
        CHECK(extensions.emplace(info.extension).second);
        // The description must agree with the table it came from.
        CHECK(DescribeArchiveCreationFormat(info.format) == info);
        // Every advertised extension must resolve back to its own format.
        CHECK(ArchiveCreationFormatForFilename("name." + std::string{info.extension}) == info.format);
    }
}

TEST_CASE(PREFIX "records the metadata trade-off a picker has to explain")
{
    // tar-based formats carry full POSIX ownership and permissions; zip does not. That is the
    // reason to offer more than one format at all, so the model states it rather than leaving each
    // surface to fold it into a tooltip.
    CHECK(DescribeArchiveCreationFormat(ArchiveCreationFormat::Tar).preserves_posix_metadata);
    CHECK(DescribeArchiveCreationFormat(ArchiveCreationFormat::TarGzip).preserves_posix_metadata);
    CHECK_FALSE(DescribeArchiveCreationFormat(ArchiveCreationFormat::Zip).preserves_posix_metadata);

    // Plain tar is the one that does not compress.
    CHECK_FALSE(DescribeArchiveCreationFormat(ArchiveCreationFormat::Tar).compresses);
    CHECK(DescribeArchiveCreationFormat(ArchiveCreationFormat::TarGzip).compresses);
    CHECK(DescribeArchiveCreationFormat(ArchiveCreationFormat::Zip).compresses);
}

TEST_CASE(PREFIX "records that only one format can carry a passphrase")
{
    // A surface that forgot this would let a user ask for protection, watch an archive appear, and
    // never learn it is not protected.
    CHECK(DescribeArchiveCreationFormat(ArchiveCreationFormat::Zip).supports_encryption);
    CHECK_FALSE(DescribeArchiveCreationFormat(ArchiveCreationFormat::Tar).supports_encryption);
    CHECK_FALSE(DescribeArchiveCreationFormat(ArchiveCreationFormat::TarGzip).supports_encryption);
    CHECK_FALSE(DescribeArchiveCreationFormat(ArchiveCreationFormat::TarBzip2).supports_encryption);
}

TEST_CASE(PREFIX "judges a create request before anything is written")
{
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::Zip, true, false, false) ==
          ArchiveCreationRequestVerdict::Submittable);
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::Zip, true, true, true) ==
          ArchiveCreationRequestVerdict::Submittable);
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::TarGzip, true, false, false) ==
          ArchiveCreationRequestVerdict::Submittable);

    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::Zip, false, false, false) ==
          ArchiveCreationRequestVerdict::DestinationMissing);
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::Zip, true, true, false) ==
          ArchiveCreationRequestVerdict::PasswordMissing);

    // Protection asked of a format that cannot carry it is reported as unsupported rather than as a
    // missing password: typing one would not help, and asking for it is the more misleading answer.
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::Tar, true, true, false) ==
          ArchiveCreationRequestVerdict::PasswordUnsupported);
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::TarBzip2, true, true, true) ==
          ArchiveCreationRequestVerdict::PasswordUnsupported);

    // A password left over in a field nobody is reading does not make a request unsubmittable - only
    // asking for protection does.
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::Tar, true, false, true) ==
          ArchiveCreationRequestVerdict::Submittable);

    // The missing destination outranks everything: there is nowhere to put an archive either way.
    CHECK(EvaluateArchiveCreationRequest(ArchiveCreationFormat::Tar, false, true, false) ==
          ArchiveCreationRequestVerdict::DestinationMissing);
}

#undef PREFIX
