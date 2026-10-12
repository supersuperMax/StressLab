/*
 * A native module for tests/unit/core/extension/native_test.cpp, written in plain C to check that
 * fem/extension_abi.h is a C header. Built twice: as `hello`, and with HELLO_ABI_VERSION=999 as
 * `hello_bad_abi`, which the platform must refuse.
 *
 * - hello.greet creates an object of the module with "model.create", its properties being the
 *   arguments of the call, and returns the id of the object.
 * - hello.fail creates an object, then fails: the platform must revert the object.
 * - Activation reports how many times the library was activated and deactivated in the context
 *   keys hello.activations and hello.deactivations: the counters live in the library, so they show
 *   whether it stayed loaded between activations.
 */
#include "fem/extension_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HELLO_ABI_VERSION
#define HELLO_ABI_VERSION FEM_EXTENSION_ABI_VERSION
#endif

static const FemHostApi* g_host = NULL;
static int g_activations = 0;
static int g_deactivations = 0;

static FemStatus set_counter(const char* key, int value) {
    char text[32];
    snprintf(text, sizeof text, "%d", value);
    return g_host->set_context_key(g_host->host, key, text);
}

static FemStatus greet(FemCommandCall* call, void* user_data) {
    static const char prefix[] = "{\"type\":\"greeting\",\"owner\":\"test.native\",\"properties\":";
    const size_t prefix_length = sizeof prefix - 1;
    const FemCommandApi* api = g_host->commands;
    const char* arguments = api->arguments(call);
    const size_t arguments_length = strlen(arguments);
    const char* result = NULL;
    FemStatus status;
    char* request;

    (void)user_data;
    request = (char*)malloc(prefix_length + arguments_length + 2);
    if (request == NULL) {
        api->set_error(call, "out of memory");
        return FEM_ERROR;
    }
    memcpy(request, prefix, prefix_length);
    memcpy(request + prefix_length, arguments, arguments_length);
    request[prefix_length + arguments_length] = '}';
    request[prefix_length + arguments_length + 1] = '\0';
    status = api->execute(call, "model.create", request, &result);
    free(request);
    if (status != FEM_OK) {
        return FEM_ERROR; /* The reason is last_error. */
    }
    api->set_result(call, result);
    return FEM_OK;
}

static FemStatus fail(FemCommandCall* call, void* user_data) {
    const FemCommandApi* api = g_host->commands;

    (void)user_data;
    if (api->execute(call, "model.create",
                     "{\"type\":\"greeting\",\"owner\":\"test.native\",\"name\":\"partial\"}",
                     NULL) != FEM_OK) {
        return FEM_ERROR;
    }
    if (api->execute(call, "no.such-command", NULL, NULL) != FEM_ERROR ||
        strstr(api->last_error(call), "no.such-command") == NULL) {
        api->set_error(call, "last_error does not name the unknown command");
        return FEM_ERROR;
    }
    api->set_error(call, "hello.fail fails on purpose");
    return FEM_ERROR;
}

static FemStatus activate(const FemHostApi* host, void** state) {
    if (host->abi_version != FEM_EXTENSION_ABI_VERSION || host->struct_size < sizeof(FemHostApi) ||
        host->commands->struct_size < sizeof(FemCommandApi)) {
        return FEM_ERROR;
    }
    if (strcmp(host->extension_id, "test.native") != 0) {
        host->set_error(host->host, "wrong extension_id");
        return FEM_ERROR;
    }
    g_host = host;
    ++g_activations;
    *state = &g_activations;
    if (host->register_command(host->host, "hello.greet", greet, NULL) != FEM_OK ||
        host->register_command(host->host, "hello.fail", fail, NULL) != FEM_OK) {
        host->set_error(host->host, "cannot register the commands of the manifest");
        return FEM_ERROR;
    }
    if (host->register_command(host->host, "hello.undeclared", greet, NULL) != FEM_ERROR) {
        host->set_error(host->host, "registered a command the manifest does not declare");
        return FEM_ERROR;
    }
    if (set_counter("hello.activations", g_activations) != FEM_OK ||
        set_counter("hello.deactivations", g_deactivations) != FEM_OK) {
        host->set_error(host->host, "cannot set the context keys of the manifest");
        return FEM_ERROR;
    }
    return FEM_OK;
}

static void deactivate(void* state) {
    if (state == &g_activations) {
        ++g_deactivations;
    }
    g_host = NULL;
}

static const FemExtension extension = {
    (uint32_t)sizeof(FemExtension),
    HELLO_ABI_VERSION,
    activate,
    deactivate,
};

FEM_EXTENSION_EXPORT const FemExtension* fem_extension_entry(void) {
    return &extension;
}
