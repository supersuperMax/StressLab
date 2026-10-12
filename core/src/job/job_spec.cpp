#include "fem/core/job/job_spec.h"

#include "fem/core/job/executor.h"

namespace fem::job {

std::filesystem::path resolve_in_workdir(const JobSpec& spec, const std::filesystem::path& path) {
    if (path.is_absolute() || spec.workdir.empty()) {
        return path;
    }
    return spec.workdir / path;
}

std::string_view to_string(OutputStream stream) noexcept {
    switch (stream) {
        case OutputStream::standard_output:
            return "stdout";
        case OutputStream::standard_error:
            return "stderr";
    }
    return "unknown";
}

}  // namespace fem::job
