#include "inp/deck.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace fem::inp {

namespace {

bool is_blank(char c) noexcept {
    return c == ' ' || c == '\t';
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && is_blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view unquote(std::string_view text) noexcept {
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return text.substr(1, text.size() - 2);
    }
    return text;
}

/// Upper case with blanks around removed and inner runs of blanks made one space.
std::string normalize_name(std::string_view text) {
    std::string out;
    bool blank = false;
    for (const char c : trim(text)) {
        if (is_blank(c)) {
            blank = true;
            continue;
        }
        if (blank) {
            out.push_back(' ');
            blank = false;
        }
        out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return out;
}

/// Splits at commas outside double quotes.
void split_raw(std::string_view text, std::vector<std::string_view>& fields) {
    fields.clear();
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '"') {
            quoted = !quoted;
        } else if (text[i] == ',' && !quoted) {
            fields.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    fields.push_back(text.substr(start));
}

Problem located(Problem::Severity severity, std::string key, nlohmann::json args,
                const SourceFile& file, std::int64_t line) {
    return Problem{severity, std::move(key), std::move(args), file.display, line};
}

[[noreturn]] void fail(std::string key, nlohmann::json args, const SourceFile& file,
                       std::int64_t line) {
    throw InpError(located(Problem::Severity::error, std::move(key), std::move(args), file, line));
}

std::optional<std::string> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.seekg(0);
    if (!in.read(bytes.data(), size)) {
        return std::nullopt;
    }
    return bytes;
}

class Reader {
public:
    Reader(Deck& deck, Encoding encoding, std::filesystem::path main_folder)
        : deck_(deck), encoding_(encoding), main_folder_(std::move(main_folder)) {}

    void read(const std::filesystem::path& path, const SourceFile* includer,
              std::int64_t include_line) {
        std::error_code error;
        std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
        if (error) {
            canonical = std::filesystem::absolute(path, error).lexically_normal();
        }
        SourceFile file{path, display(canonical), {}};
        if (std::find(stack_.begin(), stack_.end(), canonical) != stack_.end()) {
            fail("error.includeCycle", {{"file", file.display}}, *includer, include_line);
        }
        const std::optional<std::string> bytes = read_bytes(path);
        if (!bytes) {
            if (includer == nullptr) {
                throw InpError(Problem{Problem::Severity::error,
                                       "error.fileUnreadable",
                                       {{"file", path_text(path)}},
                                       {},
                                       0});
            }
            fail("error.includeMissing", {{"file", file.display}}, *includer, include_line);
        }
        std::optional<DecodedText> decoded = decode(*bytes, encoding_);
        if (!decoded) {
            throw InpError(Problem{Problem::Severity::error,
                                   "error.badEncoding",
                                   {{"encoding", std::string(to_string(encoding_))}},
                                   file.display,
                                   0});
        }
        file.encoding = decoded->encoding;
        if (decoded->fallback) {
            deck_.problems.push_back(located(Problem::Severity::warning, "problem.encodingFallback",
                                             {{"encoding", decoded->encoding}}, file, 0));
        }
        const std::size_t index = deck_.files.size();
        deck_.files.push_back(file);
        deck_.texts.push_back(std::make_unique<const std::string>(std::move(decoded->text)));
        stack_.push_back(canonical);
        scan(index, *deck_.texts.back());
        stack_.pop_back();
    }

private:
    struct Line {
        std::int64_t number;
        std::string_view text;
    };

    static std::vector<Line> split_lines(std::string_view text) {
        std::vector<Line> lines;
        std::int64_t number = 1;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
                lines.push_back({number, text.substr(start, i - start)});
                if (i < text.size() && text[i] == '\r' && i + 1 < text.size() &&
                    text[i + 1] == '\n') {
                    ++i;
                }
                start = i + 1;
                ++number;
            }
        }
        return lines;
    }

    void scan(std::size_t file, std::string_view text) {
        const std::vector<Line> lines = split_lines(text);
        // Copied: reading an include adds files and may move the vector.
        const SourceFile source = deck_.files[file];
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string_view content = trim(lines[i].text);
            if (content.empty() || content.starts_with("**")) {
                continue;
            }
            if (!content.starts_with('*')) {
                if (deck_.keywords.empty()) {
                    fail("error.dataBeforeKeyword", nlohmann::json::object(), source,
                         lines[i].number);
                }
                deck_.keywords.back().data.push_back({lines[i].number, content, file});
                continue;
            }
            // A keyword line that ends with a comma goes on in the next line.
            const std::int64_t first_line = lines[i].number;
            std::string written(lines[i].text);
            std::string joined(content);
            while (!joined.empty() && trim(joined).ends_with(',') && i + 1 < lines.size()) {
                const std::string_view next = trim(lines[i + 1].text);
                if (next.empty() || next.starts_with('*')) {
                    break;
                }
                ++i;
                written += "\n" + std::string(lines[i].text);
                joined += std::string(next);
            }
            Keyword keyword = parse_keyword(joined);
            keyword.file = file;
            keyword.line = first_line;
            keyword.written = std::move(written);
            if (keyword.name == "INCLUDE") {
                const std::optional<std::string> input = keyword.value("INPUT");
                if (!input || input->empty()) {
                    fail("error.missingParameter",
                         {{"keyword", "*INCLUDE"}, {"parameter", "INPUT"}}, source, keyword.line);
                }
                std::filesystem::path path = text_path(*input);
                if (path.is_relative()) {
                    path = source.path.parent_path() / path;
                }
                // The included lines take the place of the *INCLUDE line: data lines at the start
                // of the file belong to the keyword before it, and so on.
                read(path, &source, keyword.line);
                continue;
            }
            keyword.index = deck_.keywords.size();
            deck_.keywords.push_back(std::move(keyword));
        }
    }

    static Keyword parse_keyword(std::string_view line) {
        std::vector<std::string_view> fields;
        split_raw(line.substr(1), fields);
        Keyword keyword;
        keyword.name = normalize_name(fields.front());
        for (std::size_t f = 1; f < fields.size(); ++f) {
            const std::string_view field = trim(fields[f]);
            if (field.empty()) {
                continue;
            }
            Parameter parameter;
            const std::size_t equals = field.find('=');
            if (equals == std::string_view::npos) {
                parameter.key = normalize_name(field);
            } else {
                parameter.key = normalize_name(field.substr(0, equals));
                parameter.value = std::string(unquote(trim(field.substr(equals + 1))));
                parameter.has_value = true;
            }
            keyword.params.push_back(std::move(parameter));
        }
        return keyword;
    }

    std::string display(const std::filesystem::path& canonical) const {
        std::u8string text;
        if (deck_.files.empty()) {
            text = canonical.filename().generic_u8string();
        } else {
            std::error_code error;
            std::filesystem::path folder = std::filesystem::weakly_canonical(main_folder_, error);
            if (error) {
                folder = main_folder_;
            }
            const std::filesystem::path relative = canonical.lexically_relative(folder);
            text = relative.empty() ? canonical.generic_u8string() : relative.generic_u8string();
        }
        return {text.begin(), text.end()};
    }

    Deck& deck_;
    Encoding encoding_;
    std::filesystem::path main_folder_;
    std::vector<std::filesystem::path> stack_;
};

}  // namespace

void split_fields(std::string_view text, std::vector<std::string_view>& fields) {
    split_raw(text, fields);
    for (std::string_view& field : fields) {
        field = unquote(trim(field));
    }
}

std::vector<std::string_view> split_fields(std::string_view text) {
    std::vector<std::string_view> fields;
    split_fields(text, fields);
    return fields;
}

const Parameter* Keyword::find(std::string_view key) const noexcept {
    for (const Parameter& parameter : params) {
        if (parameter.key == key) {
            return &parameter;
        }
    }
    return nullptr;
}

std::optional<std::string> Keyword::value(std::string_view key) const {
    const Parameter* parameter = find(key);
    if (parameter == nullptr) {
        return std::nullopt;
    }
    return parameter->value;
}

std::string Keyword::text() const {
    std::string out = written;
    for (const DataLine& data_line : data) {
        out += '\n';
        out += data_line.text;
    }
    return out;
}

Deck read_deck(const std::filesystem::path& file, Encoding encoding) {
    Deck deck;
    std::error_code error;
    const std::filesystem::path absolute = std::filesystem::absolute(file, error);
    Reader reader(deck, encoding, (error ? file : absolute).parent_path());
    reader.read(file, nullptr, 0);
    return deck;
}

std::string path_text(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path text_path(std::string_view text) {
    return {std::u8string(text.begin(), text.end())};
}

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

}  // namespace fem::inp
