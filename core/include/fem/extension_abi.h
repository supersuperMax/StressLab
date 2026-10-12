/*
 * Stable C ABI between the platform and native modules (docs/architecture.md §4.4,
 * docs/adr/0006-extension-manager.md). Plain C, so a module can be built by any compiler and does
 * not depend on the C++ standard library or the platform's C++ types.
 *
 * A native module is a shared library named in its manifest by `entry`, without the file
 * extension (".dll" on Windows, ".dylib" on macOS, ".so" elsewhere). It exports one function:
 *
 *     FEM_EXTENSION_EXPORT const FemExtension* fem_extension_entry(void);
 *
 * The platform calls `activate` when the module is activated and `deactivate` when it is disabled.
 * The library is never unloaded while the application runs: if the module is enabled again, the
 * same library is activated again, so `activate` must work after `deactivate`.
 *
 * Rules:
 * - All text is UTF-8 and ends with '\0'. Values (arguments, results, context key values) are
 *   JSON text.
 * - Text the platform passes stays valid until the call returns, or, for the fields of
 *   FemHostApi, until `deactivate` returns. The platform copies all text it receives.
 * - Every call happens on the thread that owns the model; none may be made from other threads.
 * - Nothing may throw across this boundary: functions report errors with FEM_ERROR.
 * - Structures start with their size and only grow at the end. FEM_EXTENSION_ABI_VERSION changes
 *   only for an incompatible change, and the platform refuses modules of another version.
 */
#ifndef FEM_EXTENSION_ABI_H
#define FEM_EXTENSION_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FEM_EXTENSION_ABI_VERSION 1u

/* Name of the function every native module exports. */
#define FEM_EXTENSION_ENTRY_NAME "fem_extension_entry"

#if defined(_WIN32)
#define FEM_EXTENSION_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define FEM_EXTENSION_EXPORT __attribute__((visibility("default")))
#else
#define FEM_EXTENSION_EXPORT
#endif

typedef int32_t FemStatus;
#define FEM_OK ((FemStatus)0)
#define FEM_ERROR ((FemStatus)1)

/* The platform side of an active module; opaque. */
typedef struct FemHost FemHost;
/* A running command; opaque. */
typedef struct FemCommandCall FemCommandCall;

/* Runs a command of the module. Returns FEM_OK, or FEM_ERROR after `set_error`; the platform then
 * reverts every change the command made. */
typedef FemStatus (*FemCommandHandler)(FemCommandCall* call, void* user_data);

/* What a command handler can do with its call. */
typedef struct FemCommandApi {
    uint32_t struct_size;
    /* Id of the running command. */
    const char* (*command_id)(const FemCommandCall* call);
    /* Arguments as a JSON object, checked against the schema of the manifest. Paths are text. */
    const char* (*arguments)(const FemCommandCall* call);
    /* Runs another command as a part of this one (its changes join this command's undo step), e.g.
     * "model.create" to change the model. `arguments` is a JSON object or NULL. On success
     * `*result` (if `result` is not NULL) is the JSON result, valid until the call returns. On
     * failure returns FEM_ERROR and `last_error` tells why. */
    FemStatus (*execute)(FemCommandCall* call, const char* command_id, const char* arguments,
                         const char** result);
    /* Why the last `execute` of this call failed; "" if none failed. */
    const char* (*last_error)(const FemCommandCall* call);
    /* The result of the command as JSON; null if never set. */
    void (*set_result)(FemCommandCall* call, const char* result);
    /* Why the command fails; used when the handler returns FEM_ERROR. Without it the reason is
     * `last_error`, or a generic message. */
    void (*set_error)(FemCommandCall* call, const char* message);
} FemCommandApi;

/* What the platform gives an activated module. */
typedef struct FemHostApi {
    uint32_t struct_size;
    uint32_t abi_version;
    FemHost* host;
    const char* extension_id;   /* Id of the module, from its manifest. */
    const char* extension_path; /* Folder of the module. */
    const char* engine_version; /* Version of the platform, e.g. "1.4.0". */
    const FemCommandApi* commands;
    /* Gives the code of a command declared in the manifest of the module. FEM_ERROR for a command
     * the manifest does not declare. Call it from `activate`. */
    FemStatus (*register_command)(FemHost* host, const char* command_id, FemCommandHandler handler,
                                  void* user_data);
    /* Sets a context key of the module (declared in its manifest under `contextKeys`) to a JSON
     * value: true/false, a number, a string, an array of strings or null. */
    FemStatus (*set_context_key)(FemHost* host, const char* name, const char* value);
    /* Why `activate` fails; used when it returns FEM_ERROR. */
    void (*set_error)(FemHost* host, const char* message);
} FemHostApi;

/* What a native module gives the platform. */
typedef struct FemExtension {
    uint32_t struct_size;
    uint32_t abi_version; /* FEM_EXTENSION_ABI_VERSION the module was built with. */
    /* Activates the module. `host` stays valid until `deactivate` returns; `*state` is passed to
     * `deactivate`. */
    FemStatus (*activate)(const FemHostApi* host, void** state);
    /* Releases what `activate` acquired; may be NULL. */
    void (*deactivate)(void* state);
} FemExtension;

typedef const FemExtension* (*FemExtensionEntry)(void);

#ifdef __cplusplus
}
#endif

#endif /* FEM_EXTENSION_ABI_H */
