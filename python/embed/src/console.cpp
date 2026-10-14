// Python.h comes first, through nanobind.
#include "fem/python/console.h"

#include "fem/python/interpreter.h"

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <string>
#include <utility>

namespace fem::python {

namespace nb = nanobind;

namespace {

nb::str to_str(std::string_view text) {
    return nb::str(text.data(), text.size());
}

std::string to_string(const nb::handle& text) {
    return nb::cast<std::string>(text);
}

/// Runs `call` with the GIL; a Python exception becomes PythonError.
template <class Call>
auto with_gil(Call&& call) {
    const nb::gil_scoped_acquire gil;
    try {
        return call();
    } catch (nb::python_error& error) {
        throw PythonError(error.what());
    } catch (const nb::cast_error& error) {
        throw PythonError(std::string("the console returned an unexpected value: ") + error.what());
    }
}

}  // namespace

struct Console::State {
    nb::object console;
};

Console::Console() : state_(std::make_unique<State>()) {
    with_gil(
        [&] { state_->console = nb::module_::import_("stresslab._console").attr("Console")(); });
}

Console::~Console() {
    const nb::gil_scoped_acquire gil;
    state_->console.reset();
}

std::string Console::banner() const {
    return with_gil([&] { return to_string(state_->console.attr("banner")); });
}

ConsoleReply Console::push(std::string_view line) {
    return with_gil([&] {
        const nb::tuple result = nb::cast<nb::tuple>(state_->console.attr("push")(to_str(line)));
        ConsoleReply reply;
        reply.more = nb::cast<bool>(result[0]);
        for (const nb::handle piece : nb::cast<nb::list>(result[1])) {
            const nb::tuple pair = nb::cast<nb::tuple>(piece);
            reply.output.push_back({nb::cast<bool>(pair[0]), to_string(pair[1])});
        }
        return reply;
    });
}

void Console::reset() {
    with_gil([&] { state_->console.attr("reset")(); });
}

std::vector<std::string> Console::complete(std::string_view line) {
    return with_gil([&] {
        std::vector<std::string> matches;
        for (const nb::handle match : state_->console.attr("complete")(to_str(line))) {
            matches.push_back(to_string(match));
        }
        return matches;
    });
}

}  // namespace fem::python
