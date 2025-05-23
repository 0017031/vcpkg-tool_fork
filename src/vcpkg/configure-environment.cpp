#include <vcpkg/base/fwd/message_sinks.h>

#include <vcpkg/base/checks.h>
#include <vcpkg/base/contractual-constants.h>
#include <vcpkg/base/downloads.h>
#include <vcpkg/base/files.h>
#include <vcpkg/base/json.h>
#include <vcpkg/base/setup-messages.h>
#include <vcpkg/base/system.debug.h>
#include <vcpkg/base/system.process.h>
#include <vcpkg/base/util.h>
#include <vcpkg/base/uuid.h>

#include <vcpkg/archives.h>
#include <vcpkg/commands.version.h>
#include <vcpkg/configure-environment.h>
#include <vcpkg/metrics.h>
#include <vcpkg/tools.h>
#include <vcpkg/vcpkgcmdarguments.h>
#include <vcpkg/vcpkgpaths.h>

using namespace vcpkg;

namespace
{
    void track_telemetry(const Filesystem& fs, const Path& telemetry_file_path)
    {
        std::error_code ec;
        auto telemetry_file = fs.read_contents(telemetry_file_path, ec);
        if (ec)
        {
            Debug::println("Telemetry file couldn't be read: " + ec.message());
            return;
        }

        auto maybe_parsed = Json::parse_object(telemetry_file, telemetry_file_path);
        auto pparsed = maybe_parsed.get();

        if (!pparsed)
        {
            Debug::println("Telemetry file couldn't be parsed: " + maybe_parsed.error().data());
            return;
        }

        if (auto acquired_artifacts = pparsed->get(JsonIdAcquiredArtifacts))
        {
            if (auto maybe_acquired_string = acquired_artifacts->maybe_string())
            {
                get_global_metrics_collector().track_string(StringMetric::AcquiredArtifacts, *maybe_acquired_string);
            }
            else
            {
                Debug::println("Acquired artifacts was not a string.");
            }
        }
        else
        {
            Debug::println("No artifacts acquired.");
        }

        if (auto activated_artifacts = pparsed->get(JsonIdActivatedArtifacts))
        {
            if (auto maybe_activated_string = activated_artifacts->maybe_string())
            {
                get_global_metrics_collector().track_string(StringMetric::ActivatedArtifacts, *maybe_activated_string);
            }
            else
            {
                Debug::println("Activated artifacts was not a string.");
            }
        }
        else
        {
            Debug::println("No artifacts activated.");
        }
    }

    constexpr const StringLiteral* ArtifactOperatingSystemsSwitchNamesStorage[] = {
        &SwitchWindows, &SwitchOsx, &SwitchLinux, &SwitchFreeBsd};
    constexpr const StringLiteral* ArtifactHostPlatformSwitchNamesStorage[] = {
        &SwitchX86, &SwitchX64, &SwitchArm, &SwitchArm64};
    constexpr const StringLiteral* ArtifactTargetPlatformSwitchNamesStorage[] = {
        &SwitchTargetX86, &SwitchTargetX64, &SwitchTargetArm, &SwitchTargetArm64};

    bool more_than_one_mapped(View<const StringLiteral*> candidates,
                              const std::set<StringLiteral, std::less<>>& switches)
    {
        bool seen = false;
        for (auto&& candidate : candidates)
        {
            if (Util::Sets::contains(switches, *candidate))
            {
                if (seen)
                {
                    return true;
                }

                seen = true;
            }
        }

        return false;
    }
} // unnamed namespace

namespace vcpkg
{
    Optional<Path> download_vcpkg_standalone_bundle(DiagnosticContext& context,
                                                    const AssetCachingSettings& asset_cache_settings,
                                                    const Filesystem& fs,
                                                    const Path& download_root)
    {
#if defined(VCPKG_STANDALONE_BUNDLE_SHA)
        static constexpr StringLiteral tarball_name = "vcpkg-standalone-bundle-" VCPKG_BASE_VERSION_AS_STRING ".tar.gz";
        const auto bundle_tarball = download_root / tarball_name;
        context.statusln(msg::format(msgDownloadingVcpkgStandaloneBundle, msg::version = VCPKG_BASE_VERSION_AS_STRING));
        const auto bundle_uri =
            "https://github.com/microsoft/vcpkg-tool/releases/download/" VCPKG_BASE_VERSION_AS_STRING
            "/vcpkg-standalone-bundle.tar.gz";
        if (!download_file_asset_cached(context,
                                        null_sink,
                                        asset_cache_settings,
                                        fs,
                                        bundle_uri,
                                        {},
                                        bundle_tarball,
                                        tarball_name,
                                        MACRO_TO_STRING(VCPKG_STANDALONE_BUNDLE_SHA)))
        {
            return nullopt;
        }
#else  // ^^^ VCPKG_STANDALONE_BUNDLE_SHA / !VCPKG_STANDALONE_BUNDLE_SHA vvv
        static constexpr StringLiteral latest_tarball_name = "vcpkg-standalone-bundle-latest.tar.gz";
        const auto bundle_tarball = download_root / latest_tarball_name;
        context.report(DiagnosticLine{DiagKind::Warning, msg::format(msgDownloadingVcpkgStandaloneBundleLatest)});
        std::error_code ec;
        fs.remove(bundle_tarball, ec);
        if (ec)
        {
            context.report_error(format_filesystem_call_error(ec, "remove", {bundle_tarball}));
            return nullopt;
        }

        const auto bundle_uri =
            "https://github.com/microsoft/vcpkg-tool/releases/latest/download/vcpkg-standalone-bundle.tar.gz";
        if (!download_file_asset_cached(context,
                                        null_sink,
                                        asset_cache_settings,
                                        fs,
                                        bundle_uri,
                                        {},
                                        bundle_tarball,
                                        latest_tarball_name,
                                        nullopt))
        {
            return nullopt;
        }
#endif // ^^^ !VCPKG_STANDALONE_BUNDLE_SHA
        return bundle_tarball;
    }

    int run_configure_environment_command(const VcpkgPaths& paths, View<std::string> args)
    {
        msg::println_warning(msgVcpkgCeIsExperimental);
        auto& fs = paths.get_filesystem();

        // if artifacts is deployed in development, with Visual Studio, or with the One Liner, it will be deployed here
        const Path exe_path = get_exe_path_of_current_process();
        Path vcpkg_artifacts_path = exe_path;
        vcpkg_artifacts_path.replace_filename("vcpkg-artifacts");
        vcpkg_artifacts_path.make_preferred();
        Path vcpkg_artifacts_main_path = vcpkg_artifacts_path / "main.js";
        // Official / Development / None
        // cross with
        // Git / OneLiner / VS
        //
        // Official Git: Check for matching version number, use if set
        // Development Git: Use development copy
        // None Git: Use latest copy, download every time
        if (paths.try_provision_vcpkg_artifacts())
        {
#if defined(VCPKG_STANDALONE_BUNDLE_SHA)
            Path vcpkg_artifacts_version_path = vcpkg_artifacts_path / "version.txt";
            bool out_of_date = fs.check_update_required(vcpkg_artifacts_version_path, VCPKG_BASE_VERSION_AS_STRING)
                                   .value_or_exit(VCPKG_LINE_INFO);
#else  // ^^^ VCPKG_STANDALONE_BUNDLE_SHA / !VCPKG_STANDALONE_BUNDLE_SHA vvv
            bool out_of_date = !fs.exists(vcpkg_artifacts_path / "artifacts-development.txt", VCPKG_LINE_INFO);
#endif // ^^^ !VCPKG_STANDALONE_BUNDLE_SHA
            if (out_of_date)
            {
                // This is racy; the reason for the temp-path-then-rename dance is to get only the
                // 'vcpkg-artifacts' directory out of the standalone bundle, while extracting
                // the standalone bundle over VCPKG_ROOT would overwrite scripts/triplets/etc.
                auto maybe_tarball = download_vcpkg_standalone_bundle(
                    console_diagnostic_context, paths.get_asset_cache_settings(), fs, paths.downloads);
                auto tarball = maybe_tarball.get();
                if (!tarball)
                {
                    Checks::exit_fail(VCPKG_LINE_INFO);
                }

                Path temp = extract_archive_to_temp_subdirectory(
                    fs, paths.get_tool_cache(), null_sink, *tarball, vcpkg_artifacts_path);
                fs.remove_all(vcpkg_artifacts_path, VCPKG_LINE_INFO);
                fs.rename_with_retry(temp / "vcpkg-artifacts", vcpkg_artifacts_path, VCPKG_LINE_INFO);
                fs.remove_all(temp, VCPKG_LINE_INFO);
                fs.remove(*tarball, VCPKG_LINE_INFO);
#if defined(VCPKG_STANDALONE_BUNDLE_SHA)
                fs.write_contents(vcpkg_artifacts_version_path, VCPKG_BASE_VERSION_AS_STRING, VCPKG_LINE_INFO);
#endif // ^^^ VCPKG_STANDALONE_BUNDLE_SHA
            }

            if (!fs.exists(vcpkg_artifacts_main_path, VCPKG_LINE_INFO))
            {
                Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgArtifactsBootstrapFailed);
            }
        }
        else if (!fs.exists(vcpkg_artifacts_path, VCPKG_LINE_INFO))
        {
            // Official OneLiner: Do nothing, should be handled by z-boostrap-standalone
            // Development OneLiner: (N/A)
            // None OneLiner: (N/A)
            //
            // Official VS: Do nothing, should be bundled by VS
            // Development VS: (N/A)
            // None VS: (N/A)
            Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgArtifactsNotInstalledReadonlyRoot);
        }

        auto temp_directory = fs.create_or_get_temp_directory(VCPKG_LINE_INFO);

        auto cmd = Command{paths.get_tool_exe(Tools::NODE, out_sink)};
        cmd.string_arg(vcpkg_artifacts_main_path);
        cmd.forwarded_args(args);
        if (Debug::g_debugging)
        {
            cmd.string_arg("--debug");
        }

        Optional<Path> maybe_telemetry_file_path;
        if (g_metrics_enabled.load())
        {
            auto& p = maybe_telemetry_file_path.emplace(temp_directory /
                                                        (generate_random_UUID() + "_artifacts_telemetry.txt"));
            cmd.string_arg("--z-telemetry-file").string_arg(p);
        }

        cmd.string_arg("--vcpkg-root").string_arg(paths.root);
        cmd.string_arg("--z-vcpkg-command").string_arg(exe_path);

        cmd.string_arg("--z-vcpkg-artifacts-root").string_arg(paths.artifacts());
        cmd.string_arg("--z-vcpkg-downloads").string_arg(paths.downloads);
        cmd.string_arg("--z-vcpkg-registries-cache").string_arg(paths.registries_cache());
        cmd.string_arg("--z-next-previous-environment")
            .string_arg(temp_directory / (generate_random_UUID() + "_previous_environment.txt"));
        cmd.string_arg("--z-global-config").string_arg(paths.global_config());

        auto maybe_file = msg::get_loaded_file();
        if (!maybe_file.empty())
        {
            auto temp_file = temp_directory / "messages.json";
            fs.write_contents(temp_file, maybe_file, VCPKG_LINE_INFO);
            cmd.string_arg("--language").string_arg(temp_file);
        }

        ProcessLaunchSettings settings;
        settings.working_directory = paths.original_cwd;
        const auto node_result = cmd_execute(cmd, settings).value_or_exit(VCPKG_LINE_INFO);
        if (auto telemetry_file_path = maybe_telemetry_file_path.get())
        {
            track_telemetry(fs, *telemetry_file_path);
        }

        if constexpr (std::is_signed_v<decltype(node_result)>)
        {
            // workaround some systems which only keep the lower 7 bits
            if (node_result < 0 || node_result > 127)
            {
                return 1;
            }

            return node_result;
        }
        else
        {
            return static_cast<int>(node_result);
        }
    }

    void forward_common_artifacts_arguments(std::vector<std::string>& appended_to, const ParsedArguments& parsed)
    {
        auto&& switches = parsed.switches;
        for (auto&& parsed_switch : switches)
        {
            appended_to.push_back(fmt::format("--{}", parsed_switch));
        }

        if (more_than_one_mapped(ArtifactOperatingSystemsSwitchNamesStorage, parsed.switches))
        {
            Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgArtifactsSwitchOnlyOneOperatingSystem);
        }

        if (more_than_one_mapped(ArtifactHostPlatformSwitchNamesStorage, parsed.switches))
        {
            Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgArtifactsSwitchOnlyOneHostPlatform);
        }

        if (more_than_one_mapped(ArtifactTargetPlatformSwitchNamesStorage, parsed.switches))
        {
            Checks::msg_exit_with_error(VCPKG_LINE_INFO, msgArtifactsSwitchOnlyOneTargetPlatform);
        }

        for (auto&& parsed_option : parsed.settings)
        {
            appended_to.push_back(fmt::format("--{}", parsed_option.first));
            appended_to.push_back(parsed_option.second);
        }
    }
} // namespace vcpkg
