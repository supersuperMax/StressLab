#pragma once

#include "fem/core/command/command_bus.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/context/core_keys.h"
#include "fem/core/extension/manager.h"
#include "fem/core/model/document.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// What the `stresslab` Python module works with (task 1.8, docs/architecture.md §4.7). Python code
// keeps handles to documents; the application that embeds Python lends its own document and
// context keys and takes them back before it destroys them. Everything here is used with the GIL
// held, from the thread Python runs on.
namespace fem::python {

/// A document, its Command Bus or a command that is no longer available to Python: the
/// application closed it, or Python is shutting down. In Python, `stresslab.ClosedError`.
class ClosedError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Something Python keeps inside the core (a listener, the code of a command, an open
/// transaction) that must be dropped while Python still runs.
class PythonResource {
public:
    PythonResource() = default;
    PythonResource(const PythonResource&) = delete;
    PythonResource& operator=(const PythonResource&) = delete;
    virtual ~PythonResource() = default;

    /// Drops it; later calls do nothing.
    void release() noexcept {
        if (!released_) {
            released_ = true;
            on_release();
        }
    }
    [[nodiscard]] bool is_released() const noexcept { return released_; }

protected:
    virtual void on_release() noexcept = 0;

private:
    bool released_ = false;
};

/// Resources kept until released: weakly (the resource lives as long as its owner does) or
/// strongly (it lives until released, like a listener that Python did not keep a handle to).
class PythonResources {
public:
    void keep(const std::shared_ptr<PythonResource>& resource);
    void hold(std::shared_ptr<PythonResource> resource);
    /// Releases all of them, also those added meanwhile.
    void release_all() noexcept;

private:
    std::vector<std::weak_ptr<PythonResource>> kept_;
    std::vector<std::shared_ptr<PythonResource>> held_;
};

/// A document as Python sees it: the model with its Command Bus. Either Python made it
/// (`stresslab.Document()`) and it lives while Python refers to it, or the application lends its
/// own and takes it back with `detach`. After that every use raises ClosedError instead of
/// touching freed memory.
class DocumentHandle : public std::enable_shared_from_this<DocumentHandle> {
public:
    /// A new document with a bus of its own.
    [[nodiscard]] static std::shared_ptr<DocumentHandle> create();
    /// The document of `bus`, which the application owns.
    [[nodiscard]] static std::shared_ptr<DocumentHandle> borrow(command::CommandBus& bus);

    DocumentHandle(const DocumentHandle&) = delete;
    DocumentHandle& operator=(const DocumentHandle&) = delete;
    DocumentHandle(DocumentHandle&&) = delete;
    DocumentHandle& operator=(DocumentHandle&&) = delete;
    ~DocumentHandle();

    [[nodiscard]] bool is_open() const noexcept { return bus_ != nullptr; }
    /// True if Python made the document, false if the application lent it.
    [[nodiscard]] bool is_owned() const noexcept { return owned_bus_ != nullptr; }
    /// Throw ClosedError if the document is closed.
    [[nodiscard]] command::CommandBus& bus() const;
    [[nodiscard]] const model::Document& document() const;
    /// The document Python made, for what is not a change of the model: enabling the objects of a
    /// module. Throws ClosedError if closed and std::runtime_error for a document the application
    /// lent (its Extension Manager enables modules).
    [[nodiscard]] model::Document& owned_document() const;

    /// What Python keeps in the bus; released when the document closes.
    [[nodiscard]] PythonResources& resources() noexcept { return resources_; }
    /// Releases what Python keeps in the bus and closes the handle. For an owned document the
    /// model itself is destroyed when the last handle goes.
    void close() noexcept;

private:
    DocumentHandle() = default;

    std::unique_ptr<model::Document> owned_document_;
    std::unique_ptr<command::CommandBus> owned_bus_;
    command::CommandBus* bus_ = nullptr;
    PythonResources resources_;
};

/// The state of the `stresslab` module in this process: the active document (what `stresslab.cmd`
/// changes) and the context keys (`stresslab.context`).
///
/// In a plain Python process the session makes its own: a new document on first use and context
/// keys whose keys of the core follow the active document. An application that embeds Python
/// attaches its document and context keys instead, and detaches them before destroying them.
class Session {
public:
    /// The session of this process.
    [[nodiscard]] static Session& instance();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // --- For the application ---------------------------------------------------------------------

    /// Python works with the document of `bus` and with `keys` until `detach`; the document becomes
    /// the active one. `extensions`, if given, is what `stresslab.extensions` enables and disables
    /// modules with. They must outlive the attachment. Replaces an earlier attachment.
    void attach(command::CommandBus& bus, context::ContextKeys& keys,
                extension::ExtensionManager* extensions = nullptr);
    /// Takes back what `attach` lent: releases what Python keeps in them and closes the handles
    /// Python has to the document. Does nothing if nothing is attached.
    void detach() noexcept;
    [[nodiscard]] bool is_attached() const noexcept { return attached_ != nullptr; }

    /// Releases everything Python keeps in the core and closes every document handle, the
    /// documents Python made included: listeners and commands of Python may refer back to the
    /// objects that keep the documents (a cycle through C++ that the garbage collector cannot
    /// see). Called when Python shuts down (an `atexit` hook of the module), while Python can still
    /// destroy its objects. An attached document stays attached, but its handles are closed.
    void shutdown() noexcept;

    // --- For the module --------------------------------------------------------------------------

    /// The active document; a new one if there is none.
    [[nodiscard]] std::shared_ptr<DocumentHandle> active_document();
    /// Throws ClosedError for a closed document.
    void set_active_document(std::shared_ptr<DocumentHandle> document);
    /// The attached document, or nullptr.
    [[nodiscard]] std::shared_ptr<DocumentHandle> attached_document() const { return attached_; }

    /// Remembers a document Python made, to close it at shutdown.
    void track(const std::shared_ptr<DocumentHandle>& document);

    [[nodiscard]] context::ContextKeys& context();
    /// The Extension Manager the application lent; throws ExtensionError(invalid_state) if none:
    /// outside the application modules are not managed from Python.
    [[nodiscard]] extension::ExtensionManager& extensions();
    /// What Python keeps outside documents (watches on context keys, listeners of tasks);
    /// released when the context keys change hands and when Python shuts down.
    [[nodiscard]] PythonResources& resources() noexcept { return resources_; }

private:
    Session() = default;
    // Never destroyed: Python may still refer to it while the process exits.
    ~Session() = default;

    /// Keys of the core follow the active document when the session owns the context keys.
    void bind_core_keys();

    std::shared_ptr<DocumentHandle> active_;
    std::shared_ptr<DocumentHandle> attached_;
    context::ContextKeys* attached_keys_ = nullptr;
    extension::ExtensionManager* attached_extensions_ = nullptr;
    std::unique_ptr<context::ContextKeys> own_keys_;
    std::unique_ptr<context::CoreKeys> core_keys_;
    PythonResources resources_;
    std::vector<std::weak_ptr<DocumentHandle>> documents_;
};

}  // namespace fem::python
