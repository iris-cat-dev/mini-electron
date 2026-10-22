// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "mini_ui/ui.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::wstring framework;
    std::filesystem::path resource_root;
    std::filesystem::path output = L"benchmark-run.json";
    std::filesystem::path capture = L"benchmark-frame.bmp";
    int rows = 1000;
    int warmups = 2;
    int iterations = 7;
    int width = 1280;
    int height = 720;
    float device_scale = 1.0f;
    int timeout_ms = 120000;
};

struct ProcessCpu {
    std::uint64_t kernel_100ns = 0;
    std::uint64_t user_100ns = 0;
};

struct ProcessMemory {
    std::uint64_t current_working_set_bytes = 0;
    std::uint64_t peak_working_set_bytes = 0;
    std::uint64_t current_private_bytes = 0;
};

struct FrameTick {
    double wall_ms = 0;
    std::uint64_t generation_before = 0;
    std::uint64_t generation_after = 0;
};

struct TickMetrics {
    std::uint64_t calls = 0;
    std::uint64_t generation_advance = 0;
    std::vector<double> all_wall_ms;
    std::vector<FrameTick> generated_frame_ticks;
};

struct Failure {
    std::string stage;
    std::string message;
    int mini_ui_error_code = 0;
    std::string application_result;
};

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return "<invalid UTF-16>";
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::string PathUtf8(const std::filesystem::path& path)
{
    return WideToUtf8(path.wstring());
}

std::string JsonString(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20) {
                result += "\\u00";
                result.push_back(hex[character >> 4]);
                result.push_back(hex[character & 15]);
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    result.push_back('"');
    return result;
}

void Log(std::string_view message)
{
    HANDLE stream = GetStdHandle(STD_ERROR_HANDLE);
    if (!stream || stream == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(stream, message.data(), static_cast<DWORD>(message.size()), &written, nullptr);
    WriteFile(stream, "\n", 1, &written, nullptr);
}

double Milliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

double SecondsNow()
{
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

std::uint64_t FileTimeValue(const FILETIME& time)
{
    ULARGE_INTEGER value {};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

bool ReadProcessCpu(ProcessCpu* result, std::string* error)
{
    FILETIME created {}, exited {}, kernel {}, user {};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        *error = "GetProcessTimes failed with Win32 error " + std::to_string(GetLastError());
        return false;
    }
    result->kernel_100ns = FileTimeValue(kernel);
    result->user_100ns = FileTimeValue(user);
    return true;
}

bool ReadProcessMemory(ProcessMemory* result, std::string* error)
{
    PROCESS_MEMORY_COUNTERS_EX counters {};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        *error = "GetProcessMemoryInfo failed with Win32 error " + std::to_string(GetLastError());
        return false;
    }
    result->current_working_set_bytes = counters.WorkingSetSize;
    result->peak_working_set_bytes = counters.PeakWorkingSetSize;
    result->current_private_bytes = counters.PrivateUsage;
    return true;
}

bool SaveBitmap(const std::filesystem::path& path, mini_ui::FrameView frame, std::string* error)
{
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0
        || frame.stride < static_cast<std::size_t>(frame.width) * 4) {
        *error = "frame is not a valid premultiplied BGRA surface";
        return false;
    }
    const std::uint64_t bytes = std::uint64_t(frame.width) * frame.height * 4;
    if (bytes > std::numeric_limits<DWORD>::max() - sizeof(BITMAPFILEHEADER) - sizeof(BITMAPINFOHEADER)) {
        *error = "frame is too large for a Windows BMP";
        return false;
    }
    BITMAPFILEHEADER file_header {};
    file_header.bfType = 0x4d42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + static_cast<DWORD>(bytes);
    BITMAPINFOHEADER bitmap {};
    bitmap.biSize = sizeof(bitmap);
    bitmap.biWidth = frame.width;
    bitmap.biHeight = -frame.height;
    bitmap.biPlanes = 1;
    bitmap.biBitCount = 32;
    bitmap.biCompression = BI_RGB;
    bitmap.biSizeImage = static_cast<DWORD>(bytes);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
    output.write(reinterpret_cast<const char*>(&bitmap), sizeof(bitmap));
    for (int y = 0; y < frame.height; ++y) {
        output.write(reinterpret_cast<const char*>(frame.pixels + y * frame.stride),
            static_cast<std::streamsize>(frame.width) * 4);
    }
    if (!output.good()) {
        *error = "could not write BGRA capture to " + PathUtf8(path);
        return false;
    }
    return true;
}

bool WriteText(const std::filesystem::path& path, std::string_view text, std::string* error)
{
    std::error_code directory_error;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        *error = "could not create output directory: " + directory_error.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output.good()) {
        *error = "could not write " + PathUtf8(path);
        return false;
    }
    return true;
}

bool ParseInt(const wchar_t* text, int minimum, int maximum, int* value)
{
    wchar_t* end = nullptr;
    errno = 0;
    long parsed = std::wcstol(text, &end, 10);
    if (errno || end == text || *end || parsed < minimum || parsed > maximum)
        return false;
    *value = static_cast<int>(parsed);
    return true;
}

bool ParseFloat(const wchar_t* text, float minimum, float maximum, float* value)
{
    wchar_t* end = nullptr;
    errno = 0;
    double parsed = std::wcstod(text, &end);
    if (errno || end == text || *end || !std::isfinite(parsed) || parsed < minimum || parsed > maximum)
        return false;
    *value = static_cast<float>(parsed);
    return true;
}

bool ParseOptions(Options* options, std::string* error)
{
    int argc = 0;
    PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        *error = "CommandLineToArgvW failed";
        return false;
    }
    const auto require_value = [&](int index, const wchar_t* name) {
        if (index + 1 < argc)
            return true;
        *error = "missing value after " + WideToUtf8(name);
        return false;
    };
    bool valid = true;
    for (int i = 1; valid && i < argc; ++i) {
        std::wstring_view argument(argv[i]);
        if (argument == L"--framework" && require_value(i, argv[i])) options->framework = argv[++i];
        else if (argument == L"--resource-root" && require_value(i, argv[i])) options->resource_root = argv[++i];
        else if (argument == L"--output" && require_value(i, argv[i])) options->output = argv[++i];
        else if (argument == L"--capture" && require_value(i, argv[i])) options->capture = argv[++i];
        else if (argument == L"--rows" && require_value(i, argv[i])) valid = ParseInt(argv[++i], 1, 100000, &options->rows);
        else if (argument == L"--warmups" && require_value(i, argv[i])) valid = ParseInt(argv[++i], 0, 1000, &options->warmups);
        else if (argument == L"--iterations" && require_value(i, argv[i])) valid = ParseInt(argv[++i], 1, 1000, &options->iterations);
        else if (argument == L"--width" && require_value(i, argv[i])) valid = ParseInt(argv[++i], 1, 32768, &options->width);
        else if (argument == L"--height" && require_value(i, argv[i])) valid = ParseInt(argv[++i], 1, 32768, &options->height);
        else if (argument == L"--device-scale" && require_value(i, argv[i])) valid = ParseFloat(argv[++i], 0.1f, 8.0f, &options->device_scale);
        else if (argument == L"--timeout-ms" && require_value(i, argv[i])) valid = ParseInt(argv[++i], 100, 3600000, &options->timeout_ms);
        else {
            *error = "unknown argument: " + WideToUtf8(argument);
            valid = false;
        }
        if (!valid && error->empty())
            *error = "invalid numeric value for " + WideToUtf8(argument);
    }
    LocalFree(argv);
    if (!valid)
        return false;
    if (options->framework != L"vue" && options->framework != L"react") {
        *error = "--framework must be vue or react";
        return false;
    }
    if (options->resource_root.empty()) {
        *error = "--resource-root is required";
        return false;
    }
    try {
        options->resource_root = std::filesystem::absolute(options->resource_root);
        options->output = std::filesystem::absolute(options->output);
        options->capture = std::filesystem::absolute(options->capture);
    } catch (const std::filesystem::filesystem_error& path_error) {
        *error = "could not resolve benchmark paths: " + std::string(path_error.what());
        return false;
    }
    return true;
}

bool Tick(mini_ui::View& view, TickMetrics* metrics, mini_ui::Error* error)
{
    const std::uint64_t before_generation = view.frame().generation;
    const auto before = Clock::now();
    const bool success = view.Tick(SecondsNow(), error);
    const auto after = Clock::now();
    const double wall_ms = Milliseconds(after - before);
    ++metrics->calls;
    metrics->all_wall_ms.push_back(wall_ms);
    const std::uint64_t after_generation = view.frame().generation;
    if (after_generation > before_generation) {
        metrics->generation_advance += after_generation - before_generation;
        metrics->generated_frame_ticks.push_back({ wall_ms, before_generation, after_generation });
    }
    return success;
}

bool Evaluate(mini_ui::View& view, std::string_view source, std::string* result, Failure* failure)
{
    mini_ui::Error error;
    if (view.Evaluate(source, "mini-ui-framework-benchmark-host.js", result, &error))
        return true;
    failure->stage = "evaluate";
    failure->message = error.message;
    failure->mini_ui_error_code = static_cast<int>(error.code);
    return false;
}

bool PumpUntil(mini_ui::View& view, std::string_view expression, std::string_view expected,
    int timeout_ms, TickMetrics* metrics, Failure* failure)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        mini_ui::Error error;
        if (!Tick(view, metrics, &error)) {
            failure->stage = "tick";
            failure->message = error.message;
            failure->mini_ui_error_code = static_cast<int>(error.code);
            return false;
        }
        std::string result;
        if (!Evaluate(view, expression, &result, failure))
            return false;
        if (result == expected)
            return true;
        if (expression.find("status") != std::string_view::npos && result == "\"error\"") {
            failure->stage = "application";
            failure->message = "framework application reported an error";
            Evaluate(view, "uiBench.result()", &failure->application_result, failure);
            return false;
        }
        Sleep(1);
    }
    failure->stage = "timeout";
    failure->message = "timed out after " + std::to_string(timeout_ms) + " ms while polling " + std::string(expression);
    return false;
}

void AppendDoubleArray(std::ostringstream& output, const std::vector<double>& values)
{
    output << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) output << ',';
        output << values[i];
    }
    output << ']';
}

std::string FailureJson(const Options& options, const Failure& failure)
{
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17);
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"status\": \"error\",\n"
           << "  \"framework_requested\": " << JsonString(WideToUtf8(options.framework)) << ",\n"
           << "  \"rows\": " << options.rows << ",\n"
           << "  \"warmups\": " << options.warmups << ",\n"
           << "  \"iterations\": " << options.iterations << ",\n"
           << "  \"diagnostic\": {\"stage\": " << JsonString(failure.stage)
           << ", \"message\": " << JsonString(failure.message)
           << ", \"mini_ui_error_code\": " << failure.mini_ui_error_code << "},\n"
           << "  \"application_result\": "
           << (failure.application_result.empty() ? "null" : failure.application_result) << "\n}\n";
    return output.str();
}

bool WriteFailure(const Options& options, const Failure& failure)
{
    const std::string document = FailureJson(options, failure);
    std::string write_error;
    if (!WriteText(options.output, document, &write_error)) {
        Log(write_error);
        Log(document);
        return false;
    }
    Log(failure.stage + ": " + failure.message);
    return true;
}

std::string SuccessJson(const Options& options, std::string_view page,
    double create_ms, double load_ms, double ready_after_load_ms, double cold_ready_ms,
    double workload_wall_ms, double view_teardown_wall_ms,
    const TickMetrics& startup_ticks, const TickMetrics& workload_ticks,
    const TickMetrics& capture_ticks,
    const ProcessCpu& cpu_before, const ProcessCpu& cpu_after, const ProcessMemory& memory,
    mini_ui::FrameView frame, std::string_view application_result)
{
    const std::uint64_t kernel_delta = cpu_after.kernel_100ns - cpu_before.kernel_100ns;
    const std::uint64_t user_delta = cpu_after.user_100ns - cpu_before.user_100ns;
    double generated_tick_ms = 0;
    for (const FrameTick& tick : workload_ticks.generated_frame_ticks)
        generated_tick_ms += tick.wall_ms;

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(17);
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"status\": \"complete\",\n"
           << "  \"reproducibility\": {\n"
           << "    \"host_version\": 2,\n"
           << "    \"public_api\": \"include/mini_ui/ui.h\",\n"
           << "    \"framework_requested\": " << JsonString(WideToUtf8(options.framework)) << ",\n"
           << "    \"page\": " << JsonString(page) << ",\n"
           << "    \"resource_root\": " << JsonString(PathUtf8(options.resource_root)) << ",\n"
           << "    \"clock\": {\"name\": \"std::chrono::steady_clock\", \"is_steady\": "
           << (Clock::is_steady ? "true" : "false") << ", \"period_numerator\": "
           << Clock::period::num << ", \"period_denominator\": " << Clock::period::den << "},\n"
           << "    \"viewport\": {\"css_width\": " << options.width << ", \"css_height\": "
           << options.height << ", \"device_scale\": " << options.device_scale << "},\n"
           << "    \"run_definition\": {\"rows\": " << options.rows << ", \"warmups\": "
           << options.warmups << ", \"measured_iterations\": " << options.iterations
           << ", \"operation_order\": [\"mount\",\"update_selected\",\"keyed_reorder\",\"prepend\",\"filter\",\"restore\",\"unmount\"]"
           << ", \"final_capture_state\": \"untimed mounted full row count with every tenth row selected\""
           << ", \"workload_wall_interval\": \"uiBench.start dispatch through warmup and measured operations; excludes final capture\"},\n"
           << "    \"timeout_ms\": " << options.timeout_ms << "\n"
           << "  },\n"
           << "  \"cold_startup_wall_ms\": {\"view_create\": " << create_ms
           << ", \"load_file_call\": " << load_ms << ", \"load_to_protocol_ready\": "
           << ready_after_load_ms << ", \"process_host_entry_to_protocol_ready\": " << cold_ready_ms << "},\n"
           << "  \"workload_wall_ms\": " << workload_wall_ms << ",\n"
           << "  \"view_teardown_wall_ms\": " << view_teardown_wall_ms << ",\n"
           << "  \"process_cpu\": {\"source\": \"GetProcessTimes\", \"interval\": \"before View::Create through populated capture; excludes View and process teardown\", \"kernel_100ns_delta\": "
           << kernel_delta << ", \"user_100ns_delta\": " << user_delta
           << ", \"total_seconds_delta\": " << ((kernel_delta + user_delta) / 10000000.0) << "},\n"
           << "  \"process_memory\": {\"source\": \"GetProcessMemoryInfo\", \"current_working_set_bytes\": "
           << memory.current_working_set_bytes << ", \"peak_working_set_bytes\": "
           << memory.peak_working_set_bytes << ", \"current_private_bytes\": "
           << memory.current_private_bytes << "},\n"
           << "  \"cpu_offscreen_frames\": {\n"
           << "    \"description\": \"Software offscreen frames produced by workload View::Tick calls, excluding the separate populated-capture phase; this is not onscreen or vsync FPS.\",\n"
           << "    \"startup_tick_calls\": " << startup_ticks.calls << ",\n"
           << "    \"startup_generation_advance\": " << startup_ticks.generation_advance << ",\n"
           << "    \"workload_tick_calls\": " << workload_ticks.calls << ",\n"
           << "    \"workload_generation_advance\": " << workload_ticks.generation_advance << ",\n"
           << "    \"capture_verification_tick_calls\": " << capture_ticks.calls << ",\n"
           << "    \"capture_verification_generation_advance\": " << capture_ticks.generation_advance << ",\n"
           << "    \"generation_change_tick_count\": " << workload_ticks.generated_frame_ticks.size() << ",\n"
           << "    \"generated_frame_tick_wall_ms_sum\": " << generated_tick_ms << ",\n"
           << "    \"throughput_frames_per_cpu_tick_second\": "
           << workload_ticks.generation_advance * 1000.0 / generated_tick_ms << ",\n"
           << "    \"all_tick_wall_ms\": ";
    AppendDoubleArray(output, workload_ticks.all_wall_ms);
    output << ",\n    \"generation_change_ticks\": [";
    for (std::size_t i = 0; i < workload_ticks.generated_frame_ticks.size(); ++i) {
        if (i) output << ',';
        const FrameTick& tick = workload_ticks.generated_frame_ticks[i];
        output << "{\"wall_ms\":" << tick.wall_ms << ",\"generation_before\":"
               << tick.generation_before << ",\"generation_after\":" << tick.generation_after << '}';
    }
    output << "]\n  },\n"
           << "  \"frame\": {\"pixel_format\": \"premultiplied BGRA8\", \"width\": " << frame.width
           << ", \"height\": " << frame.height << ", \"stride_bytes\": " << frame.stride
           << ", \"generation\": " << frame.generation << ", \"capture_bmp\": "
           << JsonString(PathUtf8(options.capture)) << "},\n"
           << "  \"application_result\": " << application_result << "\n}\n";
    return output.str();
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto host_start = Clock::now();
    Options options;
    std::string parse_error;
    if (!ParseOptions(&options, &parse_error)) {
        Failure failure { "arguments", parse_error };
        WriteFailure(options, failure);
        return 2;
    }

    Failure failure;
    ProcessCpu cpu_before;
    std::string metric_error;
    if (!ReadProcessCpu(&cpu_before, &metric_error)) {
        failure = { "process_cpu_before", metric_error };
        WriteFailure(options, failure);
        return 1;
    }

    mini_ui::ViewOptions view_options;
    view_options.resource_root = options.resource_root;
    view_options.width = options.width;
    view_options.height = options.height;
    view_options.device_scale = options.device_scale;
    view_options.native_window = nullptr;

    mini_ui::Error error;
    const auto create_start = Clock::now();
    std::unique_ptr<mini_ui::View> view = mini_ui::View::Create(view_options, &error);
    const double create_ms = Milliseconds(Clock::now() - create_start);
    if (!view) {
        failure = { "view_create", error.message, static_cast<int>(error.code) };
        WriteFailure(options, failure);
        return 1;
    }

    const std::string page = options.framework == L"vue" ? "vue.html" : "react.html";
    const auto load_start = Clock::now();
    const bool loaded = view->LoadFile(page, &error);
    const double load_ms = Milliseconds(Clock::now() - load_start);
    if (!loaded) {
        failure = { "load_file", error.message, static_cast<int>(error.code) };
        WriteFailure(options, failure);
        return 1;
    }

    TickMetrics startup_ticks;
    if (!PumpUntil(*view, "typeof uiBench==='object' ? uiBench.status : 'loading'", "\"ready\"",
            options.timeout_ms, &startup_ticks, &failure)) {
        WriteFailure(options, failure);
        return 1;
    }
    const double ready_after_load_ms = Milliseconds(Clock::now() - load_start);
    const double cold_ready_ms = Milliseconds(Clock::now() - host_start);

    const auto workload_start = Clock::now();
    std::string start_result;
    std::string start_expression = "uiBench.start({rows:" + std::to_string(options.rows)
        + ",warmups:" + std::to_string(options.warmups)
        + ",iterations:" + std::to_string(options.iterations) + "})";
    if (!Evaluate(*view, start_expression, &start_result, &failure) || start_result != "true") {
        if (failure.stage.empty()) {
            failure.stage = "application_start";
            failure.message = "uiBench.start returned " + start_result;
        }
        WriteFailure(options, failure);
        return 1;
    }

    TickMetrics workload_ticks;
    if (!PumpUntil(*view, "uiBench.done", "true", options.timeout_ms,
            &workload_ticks, &failure)) {
        WriteFailure(options, failure);
        return 1;
    }
    const double workload_wall_ms = Milliseconds(Clock::now() - workload_start);

    std::string status;
    if (!Evaluate(*view, "uiBench.status", &status, &failure)) {
        WriteFailure(options, failure);
        return 1;
    }
    std::string application_result;
    if (status != "\"complete\"") {
        Evaluate(*view, "uiBench.result()", &application_result, &failure);
        failure.stage = "application";
        failure.message = "framework workload ended with status " + status;
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }

    std::string capture_start_result;
    if (!Evaluate(*view, "uiBench.prepareCapture()", &capture_start_result, &failure)
        || capture_start_result != "true") {
        if (failure.stage.empty()) {
            failure.stage = "capture_verification_start";
            failure.message = "uiBench.prepareCapture returned " + capture_start_result;
        }
        WriteFailure(options, failure);
        return 1;
    }
    TickMetrics capture_ticks;
    if (!PumpUntil(*view, "uiBench.captureReady", "true", options.timeout_ms,
            &capture_ticks, &failure)) {
        WriteFailure(options, failure);
        return 1;
    }
    if (!Evaluate(*view, "uiBench.status", &status, &failure)
        || !Evaluate(*view, "uiBench.result()", &application_result, &failure)) {
        WriteFailure(options, failure);
        return 1;
    }
    if (status != "\"complete\"") {
        failure.stage = "capture_verification";
        failure.message = "populated capture verification ended with status " + status;
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }
    if (capture_ticks.generation_advance == 0) {
        failure.stage = "capture_verification";
        failure.message = "populated capture verification produced no offscreen frame";
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }
    if (workload_ticks.generation_advance == 0 || workload_ticks.generated_frame_ticks.empty()) {
        failure.stage = "frame_metrics";
        failure.message = "workload completed without an observed offscreen frame generation change";
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }
    double generated_tick_wall_ms = 0;
    for (const FrameTick& tick : workload_ticks.generated_frame_ticks)
        generated_tick_wall_ms += tick.wall_ms;
    if (generated_tick_wall_ms <= 0) {
        failure.stage = "frame_metrics";
        failure.message = "generation-producing View::Tick calls had no measurable steady-clock duration";
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }

    mini_ui::FrameView frame = view->frame();
    std::error_code capture_directory_error;
    if (options.capture.has_parent_path())
        std::filesystem::create_directories(options.capture.parent_path(), capture_directory_error);
    if (capture_directory_error) {
        failure = { "capture_directory", capture_directory_error.message() };
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }
    std::string capture_error;
    if (!SaveBitmap(options.capture, frame, &capture_error)) {
        failure = { "capture", capture_error };
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }

    ProcessCpu cpu_after;
    ProcessMemory memory;
    if (!ReadProcessCpu(&cpu_after, &metric_error)) {
        failure = { "process_cpu_after", metric_error };
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }
    if (!ReadProcessMemory(&memory, &metric_error)) {
        failure = { "process_memory", metric_error };
        failure.application_result = application_result;
        WriteFailure(options, failure);
        return 1;
    }

    // Preserve only copied frame metadata after invalidating its borrowed pixels.
    frame.pixels = nullptr;
    Log("Workload assertions and populated capture complete; releasing the view.");
    const auto view_teardown_start = Clock::now();
    view.reset();
    const double view_teardown_wall_ms = Milliseconds(Clock::now() - view_teardown_start);
    Log("View released; writing the benchmark report.");

    const std::string document = SuccessJson(options, page, create_ms, load_ms,
        ready_after_load_ms, cold_ready_ms, workload_wall_ms, view_teardown_wall_ms,
        startup_ticks, workload_ticks,
        capture_ticks, cpu_before, cpu_after, memory, frame, application_result);
    std::string write_error;
    if (!WriteText(options.output, document, &write_error)) {
        Log(write_error);
        return 1;
    }
    return 0;
}
