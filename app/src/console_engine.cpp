#include "console_engine.h"

#include "fem/python/interpreter.h"

#include <string>

namespace fem::app {

namespace {

QString qstring(const std::string& text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

}  // namespace

QString PythonConsoleEngine::banner() const {
    return qstring(console_.banner());
}

shell::ConsoleEngine::Reply PythonConsoleEngine::push(const QString& line) {
    Reply reply;
    try {
        const QByteArray text = line.toUtf8();
        const python::ConsoleReply result =
            console_.push({text.constData(), static_cast<std::size_t>(text.size())});
        reply.more = result.more;
        for (const python::ConsoleOutput& piece : result.output) {
            reply.output.push_back({piece.error, qstring(piece.text)});
        }
    } catch (const python::PythonError& error) {
        reply.output.push_back({true, qstring(error.what())});
    }
    return reply;
}

void PythonConsoleEngine::reset() {
    try {
        console_.reset();
    } catch (const python::PythonError&) {
        // Nothing to drop then.
    }
}

QStringList PythonConsoleEngine::complete(const QString& line) {
    QStringList lines;
    try {
        const QByteArray text = line.toUtf8();
        for (const std::string& match :
             console_.complete({text.constData(), static_cast<std::size_t>(text.size())})) {
            lines.push_back(qstring(match));
        }
    } catch (const python::PythonError&) {
        // No completion.
    }
    return lines;
}

}  // namespace fem::app
