// import_game() (game_import.hpp): calls org.lgp.recomp.GameImport.run on the
// calling thread (SDL's main thread, attached to the JVM by SDL).
#include "lgp/platform/android/game_import.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

namespace lgp::platform::android {

ImportResult import_game(const std::string& message, const std::string& dest, std::string* error) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) {
        *error = "no JNI environment";
        return ImportResult::Failed;
    }
    jclass cls = env->FindClass("org/lgp/recomp/GameImport");
    jclass string_cls = env->FindClass("java/lang/String");
    jmethodID run = cls != nullptr ? env->GetStaticMethodID(cls, "run", "(Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;)I")
                                   : nullptr;
    if (cls == nullptr || string_cls == nullptr || run == nullptr) {
        env->ExceptionClear();
        *error = "org.lgp.recomp.GameImport missing in the APK";
        return ImportResult::Failed;
    }
    jstring jmessage = env->NewStringUTF(message.c_str());
    jstring jdest = env->NewStringUTF(dest.c_str());
    jobjectArray jerror = env->NewObjectArray(1, string_cls, nullptr);
    const jint r = env->CallStaticIntMethod(cls, run, jmessage, jdest, jerror);
    ImportResult result = ImportResult::Failed;
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        *error = "GameImport.run threw an exception (logcat)";
    } else {
        result = r == 0 ? ImportResult::Imported : r == 1 ? ImportResult::Cancelled : ImportResult::Failed;
        if (auto* text = static_cast<jstring>(env->GetObjectArrayElement(jerror, 0))) {
            const char* chars = env->GetStringUTFChars(text, nullptr);
            *error = chars != nullptr ? chars : "";
            env->ReleaseStringUTFChars(text, chars);
            env->DeleteLocalRef(text);
        }
    }
    env->DeleteLocalRef(jerror);
    env->DeleteLocalRef(jdest);
    env->DeleteLocalRef(jmessage);
    env->DeleteLocalRef(string_cls);
    env->DeleteLocalRef(cls);
    return result;
}

}  // namespace lgp::platform::android
