// Benchmark of the import and export of Abaqus input files (tasks 3.2 and 3.3): a file of C3D10
// elements is written, read by the reader of fem.format.abaqus-inp, imported into a document by
// the module through the C ABI, as "File → Import" does, and exported again by the module. Prints
// the time and the speed of each phase; fails if the model does not have every node and element
// or the export does not read back to as many elements.
//
// Usage: fem_inp_benchmark [elements]   (default 100 000; the PR of task 3.2 quotes 1 000 000 in a
// Release build)

#include "fem/core/command/command_bus.h"
#include "fem/core/command/value.h"
#include "fem/core/context/context_keys.h"
#include "fem/core/extension/manager.h"
#include "fem/core/extension/open_file.h"
#include "fem/core/model/document.h"
#include "fem/core/model/mesh.h"

#include "inp/deck.h"
#include "inp/import.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

/// Nodes on a line of points and elements of ten consecutive nodes: the reader does the same
/// work for any geometry. 1.5 nodes per element, like a real tet10 mesh.
void write_model(const fs::path& file, long long elements) {
    std::ofstream out(file, std::ios::binary);
    const long long nodes = elements * 3 / 2 + 10;
    out << "*HEADING\nBenchmark of the import\n*NODE, NSET=NALL\n";
    char line[160];
    for (long long n = 1; n <= nodes; ++n) {
        const int length = std::snprintf(line, sizeof line, "%lld, %.6f, %.6f, %.6f\n", n,
                                         0.001 * static_cast<double>(n), 0.5, -0.25);
        out.write(line, length);
    }
    out << "*ELEMENT, TYPE=C3D10, ELSET=EALL\n";
    for (long long e = 1; e <= elements; ++e) {
        const long long first = (e - 1) * 3 / 2 + 1;
        const int length = std::snprintf(
            line, sizeof line, "%lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld, %lld\n",
            e, first, first + 1, first + 2, first + 3, first + 4, first + 5, first + 6, first + 7,
            first + 8, first + 9);
        out.write(line, length);
    }
    out << "*NSET, NSET=FIX, GENERATE\n1, 1000, 1\n"
           "*MATERIAL, NAME=Steel\n*ELASTIC\n210000., 0.3\n"
           "*SOLID SECTION, ELSET=EALL, MATERIAL=Steel\n"
           "*STEP\n*STATIC\n*BOUNDARY\nFIX, 1, 3\n*CLOAD\n10, 3, -1.\n*END STEP\n";
}

}  // namespace

int main(int argc, char** argv) {
    const long long elements = argc > 1 ? std::atoll(argv[1]) : 100'000;
    const fs::path folder = fs::temp_directory_path() / "stresslab-inp-benchmark";
    fs::create_directories(folder);
    const fs::path file = folder / "model.inp";

    auto start = Clock::now();
    write_model(file, elements);
    const double megabytes = static_cast<double>(fs::file_size(file)) / 1e6;
    std::printf("write %lld C3D10 elements: %.2f s, %.1f MB\n", elements, seconds_since(start),
                megabytes);

    start = Clock::now();
    {
        const fem::inp::Deck deck = fem::inp::read_deck(file, fem::inp::Encoding::automatic);
        std::printf("split into keywords and lines: %.2f s, %zu keywords\n", seconds_since(start),
                    deck.keywords.size());
    }
    start = Clock::now();
    const fem::inp::Import result = fem::inp::import_file(file, fem::inp::Encoding::automatic);
    const double reading = seconds_since(start);
    std::printf("read the file (fem_inp_reader): %.2f s, %.0f MB/s, %.0f elements/s\n", reading,
                megabytes / reading, static_cast<double>(elements) / reading);

    fem::model::Document document;
    fem::command::CommandBus bus(document);
    fem::context::ContextKeys keys;
    fem::extension::ExtensionManager manager(document, bus, keys);
    const fs::path staged(FEM_STAGED_EXTENSIONS_DIR);
    (void)manager.install(staged / "fem.model.structural",
                          fem::extension::ExtensionSource::builtin);
    (void)manager.install(staged / "fem.format.abaqus-inp",
                          fem::extension::ExtensionSource::builtin);
    (void)manager.enable("fem.format.abaqus-inp");
    start = Clock::now();
    (void)fem::extension::open_model_file(manager, bus, file);
    const double importing = seconds_since(start);
    std::printf("import into the model (module through the C ABI): %.2f s, %.0f elements/s\n",
                importing, static_cast<double>(elements) / importing);

    const auto mesh = document.data<fem::model::Mesh>(document.find_child({}, "mesh", "model"));
    fs::remove(file);
    if (mesh == nullptr || mesh->element_count() != static_cast<std::size_t>(elements) ||
        mesh->node_count() != static_cast<std::size_t>(elements * 3 / 2 + 10)) {
        std::fprintf(stderr, "the model does not have every node and element\n");
        return 1;
    }
    std::printf("objects in the model: %zu\n", document.size());

    const fs::path saved = folder / "saved.inp";
    start = Clock::now();
    (void)bus.execute("inp.export", {{"path", fem::command::Value(saved)}});
    const double exporting = seconds_since(start);
    std::printf("export the model (module through the C ABI): %.2f s, %.0f elements/s, %.1f MB\n",
                exporting, static_cast<double>(elements) / exporting,
                static_cast<double>(fs::file_size(saved)) / 1e6);
    const fem::inp::Import again = fem::inp::import_file(saved, fem::inp::Encoding::utf8);
    fs::remove(saved);
    std::size_t read_back = 0;
    for (const fem::inp::Item& item : again.items) {
        if (const auto* data = std::get_if<fem::inp::MeshData>(&item.data)) {
            for (const fem::inp::MeshBlock& block : data->blocks) {
                read_back += block.ids.size();
            }
        }
    }
    if (read_back != static_cast<std::size_t>(elements)) {
        std::fprintf(stderr, "the export reads back to %zu elements\n", read_back);
        return 1;
    }
    return 0;
}
