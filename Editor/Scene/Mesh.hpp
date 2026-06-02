#pragma once
#include <Renderer/Mesh.hpp>

/**
 * Loads a Wavefront OBJ file into a mesh, with no optimization applied.
 * @note Vendor format import lives in the editor/asset layer, not the renderer.
 */
void LoadObj(FImportedMesh& mesh, StringView path);
