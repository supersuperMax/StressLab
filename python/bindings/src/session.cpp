#include "fem/python/session.h"

#include "fem/core/extension/error.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace fem::python {

// --- PythonResources -----------------------------------------------------------------------------

void PythonResources::keep(const std::shared_ptr<PythonResource>& resource) {
    std::erase_if(kept_, [](const std::weak_ptr<PythonResource>& weak) {
        const std::shared_ptr<PythonResource> kept = weak.lock();
        return kept == nullptr || kept->is_released();
    });
    kept_.push_back(resource);
}

void PythonResources::hold(std::shared_ptr<PythonResource> resource) {
    std::erase_if(held_,
                  [](const std::shared_ptr<PythonResource>& held) { return held->is_released(); });
    held_.push_back(std::move(resource));
}

void PythonResources::release_all() noexcept {
    // A resource may add others while it is released, so the lists are taken first.
    while (!kept_.empty() || !held_.empty()) {
        const std::vector<std::weak_ptr<PythonResource>> kept = std::move(kept_);
        const std::vector<std::shared_ptr<PythonResource>> held = std::move(held_);
        kept_.clear();
        held_.clear();
        for (const std::weak_ptr<PythonResource>& weak : kept) {
            if (const std::shared_ptr<PythonResource> resource = weak.lock()) {
                resource->release();
            }
        }
        for (const std::shared_ptr<PythonResource>& resource : held) {
            resource->release();
        }
    }
}

// --- DocumentHandle ------------------------------------------------------------------------------

std::shared_ptr<DocumentHandle> DocumentHandle::create() {
    std::shared_ptr<DocumentHandle> handle(new DocumentHandle());
    handle->owned_document_ = std::make_unique<model::Document>();
    handle->owned_bus_ = std::make_unique<command::CommandBus>(*handle->owned_document_);
    handle->bus_ = handle->owned_bus_.get();
    handle->own_project_ = std::make_unique<storage::ProjectSession>(*handle->bus_);
    Session::instance().track(handle);
    return handle;
}

std::shared_ptr<DocumentHandle> DocumentHandle::borrow(command::CommandBus& bus,
                                                       storage::ProjectSession* project) {
    std::shared_ptr<DocumentHandle> handle(new DocumentHandle());
    handle->bus_ = &bus;
    handle->lent_project_ = project;
    if (project == nullptr) {
        handle->own_project_ = std::make_unique<storage::ProjectSession>(bus);
    }
    return handle;
}

DocumentHandle::~DocumentHandle() {
    close();
}

command::CommandBus& DocumentHandle::bus() const {
    if (bus_ == nullptr) {
        throw ClosedError("the document is closed");
    }
    return *bus_;
}

const model::Document& DocumentHandle::document() const {
    return bus().document();
}

model::Document& DocumentHandle::owned_document() const {
    (void)bus();
    if (owned_document_ == nullptr) {
        throw std::runtime_error(
            "modules of the application's document are enabled by its Extension Manager");
    }
    return *owned_document_;
}

storage::ProjectSession& DocumentHandle::project() const {
    (void)bus();
    return lent_project_ != nullptr ? *lent_project_ : *own_project_;
}

void DocumentHandle::close() noexcept {
    if (bus_ == nullptr) {
        return;
    }
    resources_.release_all();
    own_project_.reset();
    lent_project_ = nullptr;
    bus_ = nullptr;
    owned_bus_.reset();
    owned_document_.reset();
}

// --- Session -------------------------------------------------------------------------------------

Session& Session::instance() {
    static Session* const session = new Session();
    return *session;
}

void Session::attach(command::CommandBus& bus, context::ContextKeys& keys,
                     extension::ExtensionManager* extensions, storage::ProjectSession* project,
                     const l10n::Localization* localization) {
    detach();
    core_keys_.reset();
    resources_.release_all();
    attached_ = DocumentHandle::borrow(bus, project);
    attached_keys_ = &keys;
    attached_extensions_ = extensions;
    attached_localization_ = localization;
    active_ = attached_;
}

void Session::detach() noexcept {
    if (attached_ == nullptr) {
        return;
    }
    resources_.release_all();
    attached_->close();
    if (active_ == attached_) {
        active_.reset();
    }
    attached_.reset();
    attached_keys_ = nullptr;
    attached_extensions_ = nullptr;
    attached_localization_ = nullptr;
}

void Session::shutdown() noexcept {
    core_keys_.reset();
    resources_.release_all();
    if (attached_ != nullptr) {
        attached_->close();
    }
    active_.reset();
    const std::vector<std::weak_ptr<DocumentHandle>> documents = std::move(documents_);
    documents_.clear();
    for (const std::weak_ptr<DocumentHandle>& weak : documents) {
        if (const std::shared_ptr<DocumentHandle> document = weak.lock()) {
            document->close();
        }
    }
}

void Session::track(const std::shared_ptr<DocumentHandle>& document) {
    std::erase_if(documents_, [](const std::weak_ptr<DocumentHandle>& weak) {
        const std::shared_ptr<DocumentHandle> known = weak.lock();
        return known == nullptr || !known->is_open();
    });
    documents_.push_back(document);
}

std::shared_ptr<DocumentHandle> Session::active_document() {
    if (active_ == nullptr || !active_->is_open()) {
        set_active_document(DocumentHandle::create());
    }
    return active_;
}

void Session::set_active_document(std::shared_ptr<DocumentHandle> document) {
    if (document == nullptr || !document->is_open()) {
        throw ClosedError("the document is closed");
    }
    if (document == active_) {
        return;
    }
    // The keys of the core refer to the bus of the active document: unbind before it may go.
    core_keys_.reset();
    active_ = std::move(document);
    bind_core_keys();
}

context::ContextKeys& Session::context() {
    if (attached_keys_ != nullptr) {
        return *attached_keys_;
    }
    if (own_keys_ == nullptr) {
        own_keys_ = std::make_unique<context::ContextKeys>();
        bind_core_keys();
    }
    return *own_keys_;
}

extension::ExtensionManager& Session::extensions() {
    if (attached_extensions_ == nullptr) {
        throw extension::ExtensionError(
            extension::ExtensionErrc::invalid_state,
            "modules are managed by the application: run the script with "
            "stresslab --headless run-script, or in the Python console of the window");
    }
    return *attached_extensions_;
}

const extension::ContributionRegistry* Session::registry() const noexcept {
    return attached_extensions_ != nullptr ? &attached_extensions_->registry() : nullptr;
}

const l10n::Localization& Session::localization() {
    if (attached_localization_ != nullptr) {
        return *attached_localization_;
    }
    if (own_localization_ == nullptr) {
        own_localization_ = std::make_unique<l10n::Localization>();
    }
    return *own_localization_;
}

void Session::bind_core_keys() {
    // The application keeps the keys of the core of the context keys it lends.
    if (attached_keys_ != nullptr || own_keys_ == nullptr || active_ == nullptr ||
        !active_->is_open()) {
        return;
    }
    core_keys_ = std::make_unique<context::CoreKeys>(active_->bus(), *own_keys_);
}

}  // namespace fem::python
