#pragma once

// The model as an Abaqus input file (task 3.3, docs/adr/0025-abaqus-inp-export.md): meshes and
// sets, objects of fem.model.structural, and the keywords the import kept without interpretation
// (`abaqus_keyword`) back in their places. What the file cannot hold is reported as losses.

#include "inp/model.h"
#include "inp/problem.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace fem::inp {

struct Export {
    std::string text;             ///< The file, UTF-8 with '\n'; empty if it was not asked for.
    std::size_t keywords = 0;     ///< Keyword lines written.
    std::vector<Problem> losses;  ///< What the file does not hold, as problems "loss.*".
};

/// Writes the model. The same model always gives the same bytes. `file_stem` is the name of the
/// file without its extension: the mesh of a file without parts is named after the file when it
/// is read, so a mesh of another name is a loss; empty if the file is not known yet. With `text`
/// false only the losses are found (`describe_losses`).
[[nodiscard]] Export export_model(const Model& model, std::string_view file_stem = {},
                                  bool text = true);

/// What an export of the model would lose, without writing it.
[[nodiscard]] std::vector<Problem> describe_losses(const Model& model,
                                                   std::string_view file_stem = {});

}  // namespace fem::inp
