// Copyright (C) 2018-2023 Michael Kazakov. Subject to GNU General Public License version 3.
#define CATCH_CONFIG_ENABLE_BENCHMARKING
#include <catch2/catch_all.hpp>

#define GTEST_DONT_DEFINE_FAIL 1
#define GTEST_DONT_DEFINE_SUCCEED 1
#include <gmock/gmock.h>

#include "Tests.h"
#include <Base/CommonPaths.h>
#include <Base/SysLocale.h>
#include <ftw.h>
#include <memory>

#include <spdlog/sinks/stdout_sinks.h>
#include <VFS/Log.h>
#include <VFS/NetSFTP.h>

static auto g_TestDirPrefix = "_nc__vfs__test_";

[[clang::no_destroy]] static auto g_LogSink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
[[clang::no_destroy]] static auto g_Log = std::make_shared<spdlog::logger>("vfs", g_LogSink);

namespace {

/**
 * Accepts every host key.
 *
 * SFTP connections refuse to authenticate unless a host-key policy is installed, and these tests are
 * about transfer behaviour rather than trust decisions - the policy has its own tests, which do not
 * need a server. Installed here, in one visible place, rather than left implicit.
 */
struct AcceptAnyHostKey final : nc::vfs::sftp::HostKeyVerifier {
    bool VerifyHostKey(const nc::vfs::sftp::HostKeyPresentation &) override { return true; }
};

} // namespace

int main(int argc, char *argv[])
{
    nc::vfs::sftp::SetHostKeyVerifier(std::make_shared<AcceptAnyHostKey>());
    //    g_Log->set_level(spdlog::level::trace);
    //    nc::vfs::Log::Set(g_Log);
    nc::base::SetSystemLocaleAsCLocale();
    ::testing::GTEST_FLAG(throw_on_failure) = true;
    ::testing::InitGoogleMock(&argc, argv);
    const int result = Catch::Session().run(argc, argv);
    return result;
}

TestDir::TestDir()
{
    directory = MakeTempFilesStorage();
}

TestDir::~TestDir()
{
    std::filesystem::remove_all(directory);
}

std::string TestDir::MakeTempFilesStorage()
{
    const auto &base_path = nc::base::CommonPaths::AppTemporaryDirectory();
    const auto tmp_path = base_path + g_TestDirPrefix + "/";
    if( access(tmp_path.c_str(), F_OK) == 0 )
        std::filesystem::remove_all(tmp_path);
    if( mkdir(tmp_path.c_str(), S_IRWXU) != 0 )
        throw std::runtime_error("mkdir failed");
    return tmp_path;
}
