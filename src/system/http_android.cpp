// Copyright (C) 2025-2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __ANDROID__

#include "system/http_android.h"

#include <spdlog/spdlog.h>

#include <SDL.h>
#include <jni.h>

namespace helix::android {

std::pair<int, std::string> https_get(const std::string& url, const std::string& user_agent,
                                      const std::string& accept, int timeout_sec) {
    // SDL_AndroidGetJNIEnv attaches the calling thread to the JavaVM if needed,
    // so this is safe from the UpdateChecker worker thread.
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (!env) {
        spdlog::error("[http_android] Failed to get JNI env");
        return {0, "JNI env unavailable"};
    }

    jclass cls = env->FindClass("org/helixscreen/app/HelixActivity");
    if (!cls) {
        spdlog::error("[http_android] Failed to find HelixActivity class");
        env->ExceptionClear();
        return {0, "HelixActivity class not found"};
    }

    jmethodID method = env->GetStaticMethodID(
        cls, "httpsGet",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    if (!method) {
        spdlog::error("[http_android] Failed to find httpsGet method");
        env->DeleteLocalRef(cls);
        env->ExceptionClear();
        return {0, "httpsGet method not found"};
    }

    jstring j_url = env->NewStringUTF(url.c_str());
    jstring j_ua = env->NewStringUTF(user_agent.c_str());
    jstring j_accept = env->NewStringUTF(accept.c_str());

    if (!j_url || !j_ua || !j_accept) {
        if (j_url)
            env->DeleteLocalRef(j_url);
        if (j_ua)
            env->DeleteLocalRef(j_ua);
        if (j_accept)
            env->DeleteLocalRef(j_accept);
        env->DeleteLocalRef(cls);
        env->ExceptionClear();
        return {0, "JNI string allocation failed"};
    }

    auto j_result = static_cast<jstring>(env->CallStaticObjectMethod(
        cls, method, j_url, j_ua, j_accept, static_cast<jint>(timeout_sec)));

    env->DeleteLocalRef(j_url);
    env->DeleteLocalRef(j_ua);
    env->DeleteLocalRef(j_accept);
    env->DeleteLocalRef(cls);

    if (!j_result || env->ExceptionCheck()) {
        env->ExceptionClear();
        return {0, "JNI call failed"};
    }

    const char* result_cstr = env->GetStringUTFChars(j_result, nullptr);
    std::string result(result_cstr);
    env->ReleaseStringUTFChars(j_result, result_cstr);
    env->DeleteLocalRef(j_result);

    // Java side returns "STATUS\nBODY" or "0\nERROR".
    auto newline = result.find('\n');
    if (newline == std::string::npos) {
        return {0, result};
    }
    int status = 0;
    try {
        status = std::stoi(result.substr(0, newline));
    } catch (...) {
        status = 0;
    }
    return {status, result.substr(newline + 1)};
}

} // namespace helix::android

#endif // __ANDROID__
