/*
 * A native module for tests/unit/core/extension/native_test.cpp, written in plain C to check that
 * fem/extension_abi.h is a C header. Built twice: as `hello`, and with HELLO_ABI_VERSION=999 as
 * `hello_bad_abi`, which the platform must refuse.
 *
 * - hello.greet creates an object of the module with "model.create", its properties being the
 *   arguments of the call, and returns the id of the object.
 * - hello.fail creates an object, then fails: the platform must revert the object.
 * - hello.data creates a mesh, a node set and a surface with bulk data and reports a problem.
 * - hello.read reads the model back (task 3.3): it returns {"model": <read_model>, "nodes": the
 *   node labels of the object `object`, "sides": its surface pairs, "kind": the data kind}.
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

/* hello.data: objects with bulk data through create_object (task 3.2). A mesh of one tet4,
 * twice with "unique_name" ("tet", "tet-2"), a node set and a surface on the first one, and a
 * problem. A mesh whose element refers to a node it does not have must be refused. Returns the id
 * of the first mesh. */
static FemStatus data(FemCommandCall* call, void* user_data) {
    static const int64_t node_ids[] = {10, 20, 30, 40};
    static const double x[] = {0.0, 1.0, 0.0, 0.0};
    static const double y[] = {0.0, 0.0, 1.0, 0.0};
    static const double z[] = {0.0, 0.0, 0.0, 1.0};
    static const int64_t element_ids[] = {7};
    static const int32_t connectivity[] = {0, 1, 2, 3};
    static const int32_t broken[] = {0, 1, 2, 9};
    static const int32_t nodes[] = {3, 0};
    static const int32_t sides[] = {0, 2};
    const FemCommandApi* api = g_host->commands;
    FemElementBlock block = {(uint32_t)sizeof(FemElementBlock), "tet4", 1, element_ids,
                             connectivity};
    FemMeshData mesh = {(uint32_t)sizeof(FemMeshData), 4, node_ids, x, y, z, 1, &block};
    FemObjectData object = {(uint32_t)sizeof(FemObjectData), FEM_DATA_MESH, &mesh, NULL, 0};
    FemObjectData set = {(uint32_t)sizeof(FemObjectData), FEM_DATA_NODE_SET, NULL, nodes, 2};
    FemObjectData surface = {(uint32_t)sizeof(FemObjectData), FEM_DATA_SURFACE, NULL, sides, 1};
    int64_t first = 0;
    int64_t second = 0;
    int64_t child = 0;
    char request[160];

    (void)user_data;
    if (api->struct_size < sizeof(FemCommandApi)) {
        api->set_error(call, "the platform has no create_object");
        return FEM_ERROR;
    }
    if (api->create_object(call,
                           "{\"type\":\"mesh\",\"owner\":\"fem.core\",\"name\":\"tet\","
                           "\"unique_name\":true}",
                           &object, &first) != FEM_OK ||
        api->create_object(call,
                           "{\"type\":\"mesh\",\"owner\":\"fem.core\",\"name\":\"tet\","
                           "\"unique_name\":true}",
                           &object, &second) != FEM_OK) {
        return FEM_ERROR;
    }
    snprintf(request, sizeof request,
             "{\"type\":\"node_set\",\"owner\":\"fem.core\",\"name\":\"corners\",\"parent\":%lld}",
             (long long)first);
    if (api->create_object(call, request, &set, &child) != FEM_OK) {
        return FEM_ERROR;
    }
    snprintf(request, sizeof request,
             "{\"type\":\"surface\",\"owner\":\"fem.core\",\"name\":\"side\",\"parent\":%lld}",
             (long long)first);
    if (api->create_object(call, request, &surface, &child) != FEM_OK) {
        return FEM_ERROR;
    }
    block.connectivity = broken;
    if (api->create_object(call, "{\"type\":\"mesh\",\"owner\":\"fem.core\",\"name\":\"broken\"}",
                           &object, &child) != FEM_ERROR ||
        api->last_error(call)[0] == '\0') {
        api->set_error(call, "a mesh with a missing node was not refused");
        return FEM_ERROR;
    }
    if (api->report_problem(call,
                            "{\"severity\":\"info\",\"message\":\"%problem.made%\","
                            "\"args\":{\"count\":2},\"file\":\"tet.inp\",\"line\":3}") != FEM_OK ||
        api->report_problem(call, "{\"severity\":\"loud\",\"message\":\"x\"}") != FEM_ERROR) {
        api->set_error(call, "report_problem does not check the problem");
        return FEM_ERROR;
    }
    snprintf(request, sizeof request, "%lld", (long long)first);
    api->set_result(call, request);
    return FEM_OK;
}

/* hello.read: read_model and object_data of the object given as `object`. */
static FemStatus read(FemCommandCall* call, void* user_data) {
    const FemCommandApi* api = g_host->commands;
    const char* arguments = api->arguments(call);
    const char* model;
    const char* at;
    FemObjectData data = {(uint32_t)sizeof(FemObjectData), FEM_DATA_NONE, NULL, NULL, 0};
    long long id;
    char* result;
    size_t length;
    size_t used;
    int64_t i;

    (void)user_data;
    model = api->read_model(call);
    at = strstr(arguments, "\"object\":");
    if (model == NULL || at == NULL) {
        api->set_error(call, "read_model failed or no object given");
        return FEM_ERROR;
    }
    id = strtoll(at + 9, NULL, 10);
    if (api->object_data(call, id, &data) != FEM_OK) {
        return FEM_ERROR;
    }
    if (api->object_data(call, -1, &data) != FEM_ERROR || api->last_error(call)[0] == '\0') {
        api->set_error(call, "object_data accepted an invalid id");
        return FEM_ERROR;
    }
    if (api->object_data(call, id, &data) != FEM_OK) {
        return FEM_ERROR;
    }
    length = strlen(model) + 256 + (size_t)(data.mesh != NULL ? data.mesh->node_count : 0) * 24 +
             (size_t)data.count * 24;
    result = (char*)malloc(length);
    if (result == NULL) {
        api->set_error(call, "out of memory");
        return FEM_ERROR;
    }
    used =
        (size_t)snprintf(result, length, "{\"kind\":%u,\"model\":%s,\"nodes\":[", data.kind, model);
    if (data.kind == FEM_DATA_MESH) {
        for (i = 0; i < data.mesh->node_count; ++i) {
            used += (size_t)snprintf(result + used, length - used, "%s%lld", i > 0 ? "," : "",
                                     (long long)data.mesh->node_ids[i]);
        }
        used += (size_t)snprintf(result + used, length - used, "],\"type\":\"%s\",\"sides\":[",
                                 data.mesh->blocks[0].type);
    } else {
        used += (size_t)snprintf(result + used, length - used, "],\"type\":\"\",\"sides\":[");
    }
    if (data.kind == FEM_DATA_SURFACE) {
        for (i = 0; i < data.count * 2; ++i) {
            used += (size_t)snprintf(result + used, length - used, "%s%d", i > 0 ? "," : "",
                                     data.indices[i]);
        }
    }
    snprintf(result + used, length - used, "]}");
    api->set_result(call, result);
    free(result);
    return FEM_OK;
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
        host->register_command(host->host, "hello.fail", fail, NULL) != FEM_OK ||
        host->register_command(host->host, "hello.data", data, NULL) != FEM_OK ||
        host->register_command(host->host, "hello.read", read, NULL) != FEM_OK) {
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
