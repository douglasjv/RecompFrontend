#include "file.h"

#include <SDL.h>
#include "nfd.h"
#include "RmlUi/Core.h"

#include "recompui/program_config.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

#ifdef __ANDROID__
#include <jni.h>
#endif

#if defined(_WIN32)
#include <Shlobj.h>
#elif defined(__linux__)
#include <unistd.h>
#include <pwd.h>
#elif defined(__APPLE__)
#include "apple/rt64_apple.h"
#endif

namespace {
#ifdef __ANDROID__
    constexpr const char* AndroidRuntimeAssetRoot = "runtime";
    constexpr const char* AndroidRuntimeManifestAsset = "runtime/runtime-assets.txt";

    std::once_flag android_runtime_assets_once;
    std::filesystem::path android_runtime_root;
    std::mutex android_document_picker_mutex;

    enum class AndroidDocumentPickerMode {
        None,
        Single,
        Multiple,
    };

    struct AndroidDocumentPickerResult {
        AndroidDocumentPickerMode mode = AndroidDocumentPickerMode::None;
        std::vector<std::filesystem::path> paths{};
        std::string error{};
    };

    AndroidDocumentPickerMode android_document_picker_mode = AndroidDocumentPickerMode::None;
    std::function<void(bool, const std::filesystem::path&)> android_single_file_callback{};
    std::function<void(bool, const std::list<std::filesystem::path>&)> android_multiple_file_callback{};
    std::optional<AndroidDocumentPickerResult> android_pending_document_picker_result{};

    std::string get_program_id_utf8() {
        const auto& program_id = recompui::programconfig::get_program_id();
        return std::string(reinterpret_cast<const char*>(program_id.c_str()), program_id.size());
    }

    std::filesystem::path get_android_app_folder_path_impl() {
        const int external_storage_state = SDL_AndroidGetExternalStorageState();
        if ((external_storage_state & SDL_ANDROID_EXTERNAL_STORAGE_WRITE) != 0) {
            if (const char* external_storage_path = SDL_AndroidGetExternalStoragePath(); external_storage_path != nullptr && external_storage_path[0] != '\0') {
                return std::filesystem::path{external_storage_path};
            }
        }

        std::string program_id = get_program_id_utf8();
        char* pref_path = SDL_GetPrefPath("BanjoRecomp", program_id.c_str());
        if (pref_path == nullptr) {
            return {};
        }

        std::filesystem::path ret{pref_path};
        SDL_free(pref_path);
        return ret;
    }

    std::string load_android_asset_text(const char* asset_path) {
        size_t byte_count = 0;
        void* bytes = SDL_LoadFile(asset_path, &byte_count);
        if (bytes == nullptr) {
            return {};
        }

        std::string ret{static_cast<const char*>(bytes), byte_count};
        SDL_free(bytes);
        return ret;
    }

    std::vector<std::string> parse_android_manifest(const std::string& manifest_contents) {
        std::vector<std::string> entries{};
        std::istringstream manifest_stream{manifest_contents};
        std::string line{};

        while (std::getline(manifest_stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                entries.push_back(line);
            }
        }

        return entries;
    }

    bool android_runtime_manifest_matches(const std::filesystem::path& runtime_root, const std::string& manifest_contents, const std::vector<std::string>& entries) {
        std::ifstream existing_manifest_stream{runtime_root / "runtime-assets.txt", std::ios::binary};
        std::ostringstream existing_manifest_buffer{};
        existing_manifest_buffer << existing_manifest_stream.rdbuf();

        if (existing_manifest_buffer.str() != manifest_contents) {
            return false;
        }

        for (const std::string& entry : entries) {
            if (!std::filesystem::exists(runtime_root / entry)) {
                return false;
            }
        }

        return true;
    }

    bool copy_android_asset_file(const std::string& relative_path, const std::filesystem::path& runtime_root) {
        const std::string source_asset_path = std::string{AndroidRuntimeAssetRoot} + "/" + relative_path;

        size_t byte_count = 0;
        void* bytes = SDL_LoadFile(source_asset_path.c_str(), &byte_count);
        if (bytes == nullptr) {
            fprintf(stderr, "Failed to read Android asset %s: %s\n", source_asset_path.c_str(), SDL_GetError());
            return false;
        }

        const std::filesystem::path output_path = runtime_root / relative_path;
        std::error_code ec;
        std::filesystem::create_directories(output_path.parent_path(), ec);
        if (ec) {
            fprintf(stderr, "Failed to create Android asset directory %s: %s\n", output_path.parent_path().string().c_str(), ec.message().c_str());
            SDL_free(bytes);
            return false;
        }

        std::ofstream output_stream{output_path, std::ios::binary};
        if (!output_stream.good()) {
            fprintf(stderr, "Failed to open Android asset destination %s\n", output_path.string().c_str());
            SDL_free(bytes);
            return false;
        }

        output_stream.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(byte_count));
        SDL_free(bytes);
        return output_stream.good();
    }

    bool extract_android_runtime_assets(const std::filesystem::path& runtime_root) {
        const std::string manifest_contents = load_android_asset_text(AndroidRuntimeManifestAsset);
        if (manifest_contents.empty()) {
            fprintf(stderr, "Failed to load Android runtime asset manifest %s: %s\n", AndroidRuntimeManifestAsset, SDL_GetError());
            return false;
        }

        const std::vector<std::string> manifest_entries = parse_android_manifest(manifest_contents);
        if (manifest_entries.empty()) {
            fprintf(stderr, "Android runtime asset manifest was empty.\n");
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(runtime_root, ec);
        if (ec) {
            fprintf(stderr, "Failed to create Android runtime asset root %s: %s\n", runtime_root.string().c_str(), ec.message().c_str());
            return false;
        }

        if (android_runtime_manifest_matches(runtime_root, manifest_contents, manifest_entries)) {
            return true;
        }

        for (const std::string& entry : manifest_entries) {
            if (!copy_android_asset_file(entry, runtime_root)) {
                return false;
            }
        }

        std::ofstream manifest_output{runtime_root / "runtime-assets.txt", std::ios::binary};
        if (!manifest_output.good()) {
            fprintf(stderr, "Failed to write Android runtime asset manifest cache.\n");
            return false;
        }

        manifest_output << manifest_contents;
        return manifest_output.good();
    }

    std::filesystem::path get_android_runtime_root() {
        std::call_once(android_runtime_assets_once, [] {
            const std::filesystem::path app_folder = get_android_app_folder_path_impl();
            if (app_folder.empty()) {
                fprintf(stderr, "Failed to determine an Android app storage path.\n");
                return;
            }

            android_runtime_root = app_folder / "runtime";
            if (!extract_android_runtime_assets(android_runtime_root)) {
                fprintf(stderr, "Failed to extract Android runtime assets into %s.\n", android_runtime_root.string().c_str());
            }
        });

        return android_runtime_root;
    }

    void clear_android_document_picker_state_locked() {
        android_document_picker_mode = AndroidDocumentPickerMode::None;
        android_single_file_callback = {};
        android_multiple_file_callback = {};
        android_pending_document_picker_result.reset();
    }

    bool launch_android_document_picker(bool allow_multiple, std::string& error) {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (env == nullptr) {
            error = "Failed to get the Android JNI environment.";
            return false;
        }

        jobject activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
        if (activity == nullptr) {
            error = "Failed to get the Android activity.";
            return false;
        }

        jclass activity_class = env->GetObjectClass(activity);
        if (activity_class == nullptr) {
            error = "Failed to get the Android activity class.";
            return false;
        }

        jmethodID open_document_picker_method = env->GetMethodID(activity_class, "openDocumentPicker", "(Z)V");
        if (open_document_picker_method == nullptr) {
            env->DeleteLocalRef(activity_class);
            error = "Android document picker entry point is missing.";
            return false;
        }

        env->CallVoidMethod(activity, open_document_picker_method, allow_multiple ? JNI_TRUE : JNI_FALSE);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            env->DeleteLocalRef(activity_class);
            error = "Failed to launch the Android document picker.";
            return false;
        }

        env->DeleteLocalRef(activity_class);
        return true;
    }

    AndroidDocumentPickerResult build_android_document_picker_result(JNIEnv* env, jobjectArray selected_paths, jstring error_message) {
        AndroidDocumentPickerResult result{};
        {
            std::lock_guard lock{ android_document_picker_mutex };
            result.mode = android_document_picker_mode;
        }

        if (selected_paths != nullptr) {
            const jsize path_count = env->GetArrayLength(selected_paths);
            result.paths.reserve(static_cast<size_t>(path_count));
            for (jsize i = 0; i < path_count; i++) {
                auto path_string = static_cast<jstring>(env->GetObjectArrayElement(selected_paths, i));
                if (path_string != nullptr) {
                    const char* path_chars = env->GetStringUTFChars(path_string, nullptr);
                    if (path_chars != nullptr) {
                        result.paths.emplace_back(path_chars);
                        env->ReleaseStringUTFChars(path_string, path_chars);
                    }
                    env->DeleteLocalRef(path_string);
                }
            }
        }

        if (error_message != nullptr) {
            const char* error_chars = env->GetStringUTFChars(error_message, nullptr);
            if (error_chars != nullptr) {
                result.error = error_chars;
                env->ReleaseStringUTFChars(error_message, error_chars);
            }
        }

        return result;
    }

    bool store_android_document_picker_request(AndroidDocumentPickerMode mode, std::function<void(bool, const std::filesystem::path&)> single_callback, std::function<void(bool, const std::list<std::filesystem::path>&)> multiple_callback, std::string& error) {
        std::lock_guard lock{ android_document_picker_mutex };
        if (android_document_picker_mode != AndroidDocumentPickerMode::None) {
            error = "Another file selection request is already active.";
            return false;
        }

        android_document_picker_mode = mode;
        android_single_file_callback = std::move(single_callback);
        android_multiple_file_callback = std::move(multiple_callback);
        android_pending_document_picker_result.reset();
        return true;
    }

    void reset_android_document_picker_request() {
        std::lock_guard lock{ android_document_picker_mutex };
        clear_android_document_picker_state_locked();
    }
#endif
}

#ifdef __ANDROID__
extern "C" JNIEXPORT void JNICALL Java_io_github_banjorecomp_android_BanjoRecompiledActivity_nativeOnDocumentPickerResult(JNIEnv* env, jclass, jobjectArray selected_paths, jstring error_message) {
    AndroidDocumentPickerResult result = build_android_document_picker_result(env, selected_paths, error_message);

    std::lock_guard lock{ android_document_picker_mutex };
    if (android_document_picker_mode != AndroidDocumentPickerMode::None) {
        android_pending_document_picker_result = std::move(result);
    }
}
#endif

namespace recompui {
    static void perform_file_dialog_operation(const std::function<void(bool, const std::filesystem::path&)>& callback) {
        nfdnchar_t* native_path = nullptr;
        nfdresult_t result = NFD_OpenDialogN(&native_path, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::filesystem::path path;

        if (success) {
            path = std::filesystem::path{native_path};
            NFD_FreePathN(native_path);
        }

        callback(success, path);
    }

    static void perform_file_dialog_operation_multiple(const std::function<void(bool, const std::list<std::filesystem::path>&)>& callback) {
        const nfdpathset_t* native_paths = nullptr;
        nfdresult_t result = NFD_OpenDialogMultipleN(&native_paths, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::list<std::filesystem::path> paths;
        nfdpathsetsize_t count = 0;

        if (success) {
            NFD_PathSet_GetCount(native_paths, &count);
            for (nfdpathsetsize_t i = 0; i < count; i++) {
                nfdnchar_t* cur_path = nullptr;
                nfdresult_t cur_result = NFD_PathSet_GetPathN(native_paths, i, &cur_path);
                if (cur_result == NFD_OKAY) {
                    paths.emplace_back(std::filesystem::path{cur_path});
                }
            }
            NFD_PathSet_Free(native_paths);
        }

        callback(success, paths);
    }

    std::filesystem::path file::get_app_folder_path() {
#if !defined(__ANDROID__)
        // directly check for portable.txt (windows and native linux binary)
        if (std::filesystem::exists("portable.txt")) {
            return std::filesystem::current_path();
        }

#if defined(__APPLE__)
        // Check for portable file in the directory containing the app bundle.
        const auto app_bundle_path = file::apple::get_bundle_directory().parent_path();
        if (std::filesystem::exists(app_bundle_path / "portable.txt")) {
            return app_bundle_path;
        }
#endif
#endif

        std::filesystem::path recomp_dir{};

#if defined(_WIN32)
        // Deduce local app data path.
        PWSTR known_path = NULL;
        HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &known_path);
        if (result == S_OK) {
            recomp_dir = std::filesystem::path{known_path} / programconfig::get_program_id();
        }

        CoTaskMemFree(known_path);
#elif defined(__ANDROID__)
        recomp_dir = get_android_app_folder_path_impl();
#elif defined(__linux__) || defined(__APPLE__)
        // check for APP_FOLDER_PATH env var
        if (getenv("APP_FOLDER_PATH") != nullptr) {
            return std::filesystem::path{getenv("APP_FOLDER_PATH")};
        }

#if defined(__APPLE__)
        const auto supportdir = file::apple::get_application_support_directory();
        if (supportdir) {
            return *supportdir / programconfig::get_program_id();
        }
#endif

        const char *homedir;

        if ((homedir = getenv("HOME")) == nullptr) {
            #if defined(__linux__)
            homedir = getpwuid(getuid())->pw_dir;
            #elif defined(__APPLE__)
                homedir = GetHomeDirectory();
            #endif
        }

        if (homedir != nullptr) {
            recomp_dir = std::filesystem::path{homedir} / (std::u8string{u8".config/"} + programconfig::get_program_id());
        }
#endif

        return recomp_dir;
    }

    std::filesystem::path file::get_program_path() {
#if defined(__APPLE__)
        return file::apple::get_bundle_resource_directory();
#elif defined(__ANDROID__)
        return get_android_runtime_root();
#elif defined(__linux__) && defined(RECOMP_FLATPAK)
        return "/app/bin";
#else
        return "";
#endif
    }

    std::filesystem::path file::get_asset_path(const char* asset) {
        return file::get_program_path() / "assets" / asset;
    }

    void file::open_file_dialog(std::function<void(bool success, const std::filesystem::path& path)> callback) {
#ifdef __ANDROID__
        std::string error{};
        if (!store_android_document_picker_request(AndroidDocumentPickerMode::Single, std::move(callback), {}, error)) {
            file::show_error_message_box(programconfig::get_program_name().c_str(), error.c_str());
            return;
        }

        if (!launch_android_document_picker(false, error)) {
            reset_android_document_picker_request();
            file::show_error_message_box(programconfig::get_program_name().c_str(), error.c_str());
        }
#elif defined(__APPLE__)
        file::apple::dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation(callback);
        });
#else
        perform_file_dialog_operation(callback);
#endif
    }

    void file::open_file_dialog_multiple(std::function<void(bool success, const std::list<std::filesystem::path>& paths)> callback) {
#ifdef __ANDROID__
        std::string error{};
        if (!store_android_document_picker_request(AndroidDocumentPickerMode::Multiple, {}, std::move(callback), error)) {
            file::show_error_message_box(programconfig::get_program_name().c_str(), error.c_str());
            return;
        }

        if (!launch_android_document_picker(true, error)) {
            reset_android_document_picker_request();
            file::show_error_message_box(programconfig::get_program_name().c_str(), error.c_str());
        }
#elif defined(__APPLE__)
        file::apple::dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation_multiple(callback);
        });
#else
        perform_file_dialog_operation_multiple(callback);
#endif
    }

    void file::poll_async_results() {
#ifdef __ANDROID__
        AndroidDocumentPickerResult result{};
        std::function<void(bool, const std::filesystem::path&)> single_callback{};
        std::function<void(bool, const std::list<std::filesystem::path>&)> multiple_callback{};

        {
            std::lock_guard lock{ android_document_picker_mutex };
            if (!android_pending_document_picker_result.has_value()) {
                return;
            }

            result = std::move(*android_pending_document_picker_result);
            single_callback = std::move(android_single_file_callback);
            multiple_callback = std::move(android_multiple_file_callback);
            clear_android_document_picker_state_locked();
        }

        const bool success = result.error.empty() && !result.paths.empty();
        if (!result.error.empty()) {
            file::show_error_message_box(programconfig::get_program_name().c_str(), result.error.c_str());
        }

        if (result.mode == AndroidDocumentPickerMode::Single && single_callback) {
            single_callback(success, success ? result.paths.front() : std::filesystem::path{});
        } else if (result.mode == AndroidDocumentPickerMode::Multiple && multiple_callback) {
            std::list<std::filesystem::path> path_list{};
            for (const auto& path : result.paths) {
                path_list.emplace_back(path);
            }
            multiple_callback(success, path_list);
        }
#endif
    }

    void file::show_error_message_box(const char *title, const char *message) {
#ifdef __APPLE__
    std::string title_copy(title);
    std::string message_copy(message);

    file::apple::dispatch_on_ui_thread([title_copy, message_copy] {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title_copy.c_str(), message_copy.c_str(), nullptr);
    });
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
#endif
    }
}
