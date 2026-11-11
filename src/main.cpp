/* Copyright (C) 2019 G'k
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gui/MainWindow.hpp"
#include "gui/utils/utils.hpp"
#include "logger.hpp"
#include "manager.hpp"
#include "settings/settings.hpp"
#include "version.hpp"

#include <fmt/format.h>
#include <plog/Log.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QLibraryInfo>
#include <QMessageBox>
#include <QTranslator>

#include <csignal>
#include <stop_token>

// Process exit codes, so scripts can tell the outcomes of a headless run apart
constexpr int k_exit_ok          = 0;
constexpr int k_exit_fatal       = 1;
constexpr int k_exit_had_errors  = 2;
constexpr int k_exit_interrupted = 130; // 128 + SIGINT, shell convention

#ifdef CAO_CLI_ONLY
constexpr bool k_cli_only = true; // console-subsystem build: there is no GUI to fall back to
#else
constexpr bool k_cli_only = false;
#endif

void init()
{
    QCoreApplication::setApplicationName("Cathedral Assets Optimizer");
    QCoreApplication::setApplicationVersion(k_cao_version);
}

void init_translations()
{
    // Qt translations
    static QTranslator qt_translator;
    if (qt_translator.load(QLocale::system(),
                           "qtbase",
                           "_",
                           QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
    {
        QCoreApplication::installTranslator(&qt_translator);
    }

    // CAO translations
    static QTranslator assets_opt_translator;
    if (qt_translator.load(QLocale::system(), "AssetsOpt", "_", "translations"))
    {
        QCoreApplication::installTranslator(&assets_opt_translator);
    }
}

void display_error(bool cli, const std::string &err)
{
    std::cerr << err << '\n' << std::flush;
    PLOG_FATAL << err;
    if (!cli)
        QMessageBox::critical(nullptr, "Unknown error", QString::fromStdString(err));
}

// Ctrl+C / SIGTERM request a cooperative stop through the same stop_token the GUI's cancel button uses.
// A second signal falls back to the default handler and kills the process.
std::stop_source g_stop_source;

// UTF-8 for logs and messages; path::string() throws on Windows for non-ANSI characters
[[nodiscard]] auto to_utf8(const std::filesystem::path &path) -> std::string
{
    return cao::to_qstring(path.u8string()).toStdString();
}

extern "C" void handle_stop_signal(int sig)
{
    std::signal(sig, SIG_DFL);
    // shortcut: request_stop is not formally async-signal-safe on POSIX (it is fine on Windows, where
    // handlers run on their own thread); move to a sigwait thread if a hang on Ctrl+C is ever seen.
    g_stop_source.request_stop();
}

// Applies the --input / --mode / --dry-run overrides to the selected profile. Not saved to disk.
void apply_cli_overrides(const QCommandLineParser &parser, cao::Profile &profile)
{
    if (parser.isSet("input"))
        profile.input_path = std::filesystem::path(cao::to_u8string(parser.value("input")));

    if (parser.isSet("mode"))
    {
        const auto mode = parser.value("mode").toLower();
        if (mode == "single")
            profile.optimization_mode = cao::OptimizationMode::SingleMod;
        else if (mode == "several")
            profile.optimization_mode = cao::OptimizationMode::SeveralMods;
        else
            throw std::runtime_error(
                fmt::format("Invalid --mode '{}'. Expected 'single' or 'several'", mode.toStdString()));
    }

    if (parser.isSet("dry-run"))
        profile.dry_run = true;

    if (profile.input_path.empty())
        throw std::runtime_error("No input directory. Set it in the profile or pass --input <path>");

    if (!std::filesystem::is_directory(profile.input_path))
        throw std::runtime_error(fmt::format("Input directory does not exist: {}", to_utf8(profile.input_path)));
}

// Runs one optimization without any window and maps the outcome to an exit code
auto run_cli(const QCommandLineParser &parser, cao::Settings settings) -> int
{
    const auto profile_name = parser.positionalArguments().value(0);

    if (!settings.set_current_profile(cao::to_u8string(profile_name)))
    {
        std::string available;
        for (const auto name : settings.list_profiles())
            available += fmt::format(" '{}'", cao::to_qstring(name).toStdString());

        throw std::runtime_error(fmt::format("Profile '{}' not found. Available profiles:{}",
                                             profile_name.toStdString(),
                                             available));
    }

    apply_cli_overrides(parser, settings.current_profile());

    PLOGI << fmt::format("Cathedral Assets Optimizer {} (headless), profile '{}', log directory {}",
                         k_cao_version,
                         profile_name.toStdString(),
                         to_utf8(cao::Settings::state_directory()));

    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);

    cao::Manager manager;
    manager.run_optimization(std::move(settings), g_stop_source.get_token());

    if (g_stop_source.stop_requested())
    {
        PLOGW << "Interrupted. Files processed so far were kept";
        return k_exit_interrupted;
    }

    if (const auto errors = cao::logged_error_count(); errors > 0)
    {
        PLOGW << fmt::format("Finished with {} error(s). See the log for details", errors);
        return k_exit_had_errors;
    }

    return k_exit_ok;
}

auto main(int argc, char *argv[]) -> int
{
    auto app = std::make_unique<QCoreApplication>(argc, argv);

    init();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Exit codes in headless mode: 0 success, 1 fatal error, 2 finished with errors, 130 interrupted");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("profile", "The profile to use (required with --cli)");
    parser.addOption({"cli", "Do not run the GUI"});
    parser.addOption({"input", "Override the profile's input directory", "path"});
    parser.addOption({"mode", "Override the optimization mode: single or several", "mode"});
    parser.addOption({"dry-run", "Do not modify any file"});
    parser.process(*app);

    // Operand order matters: a constant left side would trip MSVC C4127 under /W4 /WX
    const bool cli = parser.isSet("cli") || k_cli_only;

    if (!cli)
    {
        app.reset(); // Destroying the QCoreApplication before creating a QApplication is required
        app = std::make_unique<QApplication>(argc, argv);
    }

    init_translations();

    try
    {
        if (!cao::init_logging(cao::Settings::state_directory()))
            throw std::runtime_error("Failed to initialize logging.");

        auto settings = cao::load_settings();

        if (cli)
            return run_cli(parser, std::move(settings));

        auto window = cao::MainWindow{settings};
        window.show();
        return app->exec();
    }
    catch (const std::exception &e)
    {
        display_error(cli, e.what());
        return k_exit_fatal;
    }
}
