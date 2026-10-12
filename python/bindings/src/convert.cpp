#include "fem/core/command/error.h"

#include "common.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace fem::python {

// --- PyRef, PySubscription -----------------------------------------------------------------------

PyRef::Holder::~Holder() {
    if (!object.is_valid()) {
        return;
    }
    if (Py_IsInitialized() == 0) {
        (void)object.release();  // Python is gone: nothing to decrement.
        return;
    }
    const nb::gil_scoped_acquire gil;
    object.reset();
}

nb::object PyRef::get() const {
    if (holder_ == nullptr || !holder_->object.is_valid()) {
        throw ClosedError("the Python object has been released");
    }
    return holder_->object;
}

void PyRef::clear() noexcept {
    if (holder_ != nullptr) {
        holder_->object.reset();
    }
}

void PySubscription::on_release() noexcept {
    if (concurrent_ && PyGILState_Check() != 0) {
        // A call in progress on another thread may be waiting for the GIL.
        const nb::gil_scoped_release unlocked;
        subscription_.reset();
    } else {
        subscription_.reset();
    }
}

// --- Text and paths ------------------------------------------------------------------------------

std::string to_utf8(nb::handle object) {
    if (!PyUnicode_Check(object.ptr())) {
        throw nb::type_error("expected str");
    }
    Py_ssize_t size = 0;
    const char* data = PyUnicode_AsUTF8AndSize(object.ptr(), &size);
    if (data == nullptr) {
        throw nb::python_error();  // Lone surrogates cannot be UTF-8.
    }
    return {data, static_cast<std::size_t>(size)};
}

nb::str to_str(std::string_view text) {
    return nb::str(text.data(), text.size());
}

nb::object to_python_path(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return nb::module_::import_("pathlib").attr("Path")(
        to_str({reinterpret_cast<const char*>(text.data()), text.size()}));
}

std::filesystem::path to_path(nb::handle object) {
    const nb::object text = nb::module_::import_("os").attr("fspath")(object);
    if (!PyUnicode_Check(text.ptr())) {
        throw nb::type_error("paths must be text, not bytes");
    }
    const std::string utf8 = to_utf8(text);
    return {std::u8string(utf8.begin(), utf8.end())};
}

namespace {

bool is_path_like(nb::handle object) {
    return !PyUnicode_Check(object.ptr()) && !PyBytes_Check(object.ptr()) &&
           PyObject_HasAttrString(object.ptr(), "__fspath__") != 0;
}

std::int64_t to_int64(nb::handle object) {
    int overflow = 0;
    const long long value = PyLong_AsLongLongAndOverflow(object.ptr(), &overflow);
    if (overflow != 0) {
        throw command::CommandError(command::CommandErrc::invalid_value,
                                    "integer does not fit into 64 bits");
    }
    if (value == -1 && PyErr_Occurred() != nullptr) {
        throw nb::python_error();
    }
    return value;
}

std::string python_type_name(nb::handle object) {
    return nb::type_name(object.type()).c_str();
}

}  // namespace

// --- Command values ------------------------------------------------------------------------------

command::Value to_value(nb::handle object) {
    PyObject* const ptr = object.ptr();
    if (object.is_none()) {
        return {};
    }
    if (PyBool_Check(ptr)) {
        return ptr == Py_True;
    }
    if (PyLong_Check(ptr)) {
        return to_int64(object);
    }
    if (PyFloat_Check(ptr)) {
        return PyFloat_AsDouble(ptr);
    }
    if (PyUnicode_Check(ptr)) {
        return to_utf8(object);
    }
    if (nb::isinstance<PyModelObject>(object)) {
        return nb::cast<const PyModelObject&>(object).id;
    }
    if (PyList_Check(ptr) || PyTuple_Check(ptr)) {
        command::Value::List list;
        for (nb::handle item : object) {
            list.push_back(to_value(item));
        }
        return list;
    }
    if (PyDict_Check(ptr)) {
        return to_value_dict(object);
    }
    if (is_path_like(object)) {
        return to_path(object);
    }
    if (PyIndex_Check(ptr) != 0) {  // numpy integers and other integer-like types
        return to_int64(nb::steal(PyNumber_Index(ptr)));
    }
    throw nb::type_error(
        ("a command cannot take a value of type " + python_type_name(object)).c_str());
}

command::Value::Dict to_value_dict(nb::handle kwargs) {
    command::Value::Dict values;
    for (auto [key, value] : nb::borrow<nb::dict>(kwargs)) {
        if (!PyUnicode_Check(key.ptr())) {
            throw nb::type_error("dictionary keys must be str");
        }
        values.insert_or_assign(to_utf8(key), to_value(value));
    }
    return values;
}

nb::object from_value(const command::Value& value) {
    using Kind = command::Value::Kind;
    switch (value.kind()) {
        case Kind::none:
            return nb::none();
        case Kind::boolean:
            return nb::bool_(value.as_boolean());
        case Kind::integer:
            return nb::int_(value.as_integer());
        case Kind::number:
            return nb::float_(value.as_number());
        case Kind::string:
            return to_str(value.as_string());
        case Kind::path:
            return to_python_path(value.as_path());
        case Kind::list: {
            nb::list list;
            for (const command::Value& item : value.as_list()) {
                list.append(from_value(item));
            }
            return list;
        }
        case Kind::dict:
            return from_value_dict(value.as_dict());
    }
    return nb::none();
}

nb::dict from_value_dict(const command::Value::Dict& values) {
    nb::dict dict;
    for (const auto& [key, item] : values) {
        dict[to_str(key)] = from_value(item);
    }
    return dict;
}

// --- Properties ----------------------------------------------------------------------------------

nb::object from_json(const nlohmann::json& value) {
    using Type = nlohmann::json::value_t;
    switch (value.type()) {
        case Type::null:
        case Type::discarded:
            return nb::none();
        case Type::boolean:
            return nb::bool_(value.get<bool>());
        case Type::number_integer:
            return nb::int_(value.get<std::int64_t>());
        case Type::number_unsigned:
            return nb::steal(PyLong_FromUnsignedLongLong(value.get<std::uint64_t>()));
        case Type::number_float:
            return nb::float_(value.get<double>());
        case Type::string:
            return to_str(value.get_ref<const std::string&>());
        case Type::array: {
            nb::list list;
            for (const nlohmann::json& item : value) {
                list.append(from_json(item));
            }
            return list;
        }
        case Type::object: {
            nb::dict dict;
            for (const auto& [key, item] : value.items()) {
                dict[to_str(key)] = from_json(item);
            }
            return dict;
        }
        case Type::binary:
            break;
    }
    throw nb::type_error("binary JSON values are not supported");
}

nlohmann::json to_json(nb::handle object) {
    PyObject* const ptr = object.ptr();
    if (object.is_none()) {
        return nullptr;
    }
    if (PyBool_Check(ptr)) {
        return ptr == Py_True;
    }
    if (PyLong_Check(ptr)) {
        return to_int64(object);
    }
    if (PyFloat_Check(ptr)) {
        return PyFloat_AsDouble(ptr);
    }
    if (PyUnicode_Check(ptr)) {
        return to_utf8(object);
    }
    if (PyList_Check(ptr) || PyTuple_Check(ptr)) {
        nlohmann::json array = nlohmann::json::array();
        for (nb::handle item : object) {
            array.push_back(to_json(item));
        }
        return array;
    }
    if (PyDict_Check(ptr)) {
        nlohmann::json dict = nlohmann::json::object();
        for (auto [key, value] : nb::borrow<nb::dict>(object)) {
            if (!PyUnicode_Check(key.ptr())) {
                throw nb::type_error("property names must be str");
            }
            dict[to_utf8(key)] = to_json(value);
        }
        return dict;
    }
    if (PyIndex_Check(ptr) != 0) {
        return to_int64(nb::steal(PyNumber_Index(ptr)));
    }
    throw nb::type_error(
        ("a property cannot hold a value of type " + python_type_name(object)).c_str());
}

// --- Context values ------------------------------------------------------------------------------

context::ContextValue to_context_value(nb::handle object) {
    PyObject* const ptr = object.ptr();
    if (object.is_none()) {
        return {};
    }
    if (PyBool_Check(ptr)) {
        return ptr == Py_True;
    }
    if (PyLong_Check(ptr) || PyFloat_Check(ptr)) {
        const double number = PyFloat_AsDouble(ptr);
        if (number == -1.0 && PyErr_Occurred() != nullptr) {
            throw nb::python_error();
        }
        return number;
    }
    if (PyUnicode_Check(ptr)) {
        return to_utf8(object);
    }
    if (PyList_Check(ptr) || PyTuple_Check(ptr)) {
        context::ContextValue::List list;
        for (nb::handle item : object) {
            if (!PyUnicode_Check(item.ptr())) {
                throw nb::type_error("a list value of a context key holds only str");
            }
            list.push_back(to_utf8(item));
        }
        return list;
    }
    throw nb::type_error(
        ("a context key cannot hold a value of type " + python_type_name(object)).c_str());
}

nb::object from_context_value(const context::ContextValue& value) {
    using Kind = context::ContextValue::Kind;
    switch (value.kind()) {
        case Kind::none:
            return nb::none();
        case Kind::boolean:
            return nb::bool_(value.as_boolean());
        case Kind::number:
            return nb::float_(value.as_number());
        case Kind::string:
            return to_str(value.as_string());
        case Kind::list: {
            nb::list list;
            for (const std::string& item : value.as_list()) {
                list.append(to_str(item));
            }
            return list;
        }
    }
    return nb::none();
}

}  // namespace fem::python
