/* Included first in every C/C++/Objective-C file of the ios-cross build (cmake/toolchains/ios-cross.cmake).
 * In module mode the iPhoneOS SDK's math.h takes INFINITY and NAN from Apple's float.h via __need_infinity_nan;
 * upstream clang's float.h does not know that, so Metal's headers saw neither (PC, SDK 27.1, clang 21).
 * A -D with parentheses does not survive the shell of libvpx's and SDL's sub-builds, hence this file. */
#ifndef INFINITY
#define INFINITY __builtin_inff()
#endif
#ifndef NAN
#define NAN __builtin_nanf("")
#endif
