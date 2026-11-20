#ifndef RAGE_NATIVE_MESH_WRITER_H
#define RAGE_NATIVE_MESH_WRITER_H

#include "common.h"
#include "game/vector.h"
#include "render/rmesh_cache.h"

typedef struct RageImportedTextureKey {
    uint16_t tpage;
    uint16_t clut;
    uint16_t windowWidthU;
    uint16_t windowWidthV;
    uint16_t windowOffsetU;
    uint16_t windowOffsetV;
    uint8_t hasWindow;
    uint8_t emissive;
    uint8_t terrainEnvironmentClut;
} RageImportedTextureKey;

typedef struct RageImportedMeshEntry {
    RageRuntimeCachedMesh cached;
    RageImportedTextureKey *materials;
    uint32_t materialCount;
} RageImportedMeshEntry;

typedef struct RageImportedFace {
    const SVec *vertices;
    const SVec *normals;
    uint16_t vertex[4];
    uint16_t normal[4];
    uint8_t uv[4][2];
    uint8_t color[3];
    RageImportedTextureKey texture;
    int8_t depthBias;
    uint8_t prim;
    uint8_t flags;
    uint8_t textured;
    uint8_t hasNormals;
} RageImportedFace;

/* A replacement quad for part of a source face (written space; uv in texels,
 * as RageImportedFace::uv but fractional). Corner order as the source face. */
typedef struct RageImportedPiece {
    float position[4][3];
    float uv[4][2];
} RageImportedPiece;

/* Per source face: count == 0 writes the face itself, otherwise `count`
 * pieces starting at `first`. */
typedef struct RageImportedDrape {
    uint32_t first;
    uint32_t count;
} RageImportedDrape;

typedef struct RageImportedWrite {
    RageImportedMeshEntry *entry;
    uint8_t *offsets;
    uint8_t *vertexCursor;
    uint8_t *indexCursor;
    uint32_t currentMesh;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t meshLimit;
    uint32_t vertexLimit;
    uint32_t indexLimit;
    /* Source faces written so far (pieces count once). */
    uint32_t sourceFace;
    /* Optional per-source-face translation (3 floats per face). */
    const float (*faceOffsets)[3];
    uint32_t faceOffsetCount;
    /* Optional per-source-face replacement pieces. */
    const RageImportedDrape *drapes;
    const RageImportedPiece *pieces;
    uint32_t drapeCount;
} RageImportedWrite;

/* One imported face in written (renderer) space: four corners, a triangle
 * repeating its last corner. */
typedef float RageImportedQuad[4][3];

/* Hanging signs that the PS1 drew over a wall by ordering-table sort but that
 * geometrically sink into it. A sign is a free-standing tilted quad (no corner
 * shared with any other face of its mesh); when its lower part goes behind a
 * parallel vertical wall of the same mesh, it is moved out along the wall
 * normal, towards the side its top edge hangs on, until it clears the wall.
 * `meshes` gives each face's mesh (faces of a mesh are contiguous, in one local
 * space); `unit` is the size of one world unit in quad coordinates (terrain:
 * 4, GTE units). Writes one offset per face (zero for everything else) and
 * returns the number of faces moved. */
uint32_t ImportResolveSignPenetration(const RageImportedQuad *quads,
                                      const uint32_t *meshes, uint32_t count,
                                      float unit, float (*offsets)[3]);

/* Road paint (the draw builder's long-thin-horizontal shape) is authored as
 * flat strips spanning several road triangles, so in stereo it floats over
 * dips and sinks into crests. Each marking quad of a mesh is cut where its
 * sides cross the edges of the road triangles beneath it, and every piece
 * corner is put on the road surface. The road is searched in the marking's
 * own mesh and, when `origins` (per face: its mesh's x/z placement, NAN when
 * unknown) is given, in meshes placed within `cellPitch` of it. Markings
 * whose road cannot be found are left as they are. Fills `drapes` (one per
 * face) and allocates `*pieces` (free() it). Returns the number of pieces,
 * 0 when nothing was draped or on allocation failure. */
uint32_t ImportDrapeRoadMarkings(const RageImportedQuad *quads,
                                 const float (*uvs)[4][2],
                                 const uint32_t *meshes,
                                 const float (*origins)[2], uint32_t count,
                                 float unit, float cellPitch,
                                 RageImportedDrape *drapes,
                                 RageImportedPiece **pieces);

/* Private synchronous importer writer. Source arrays are borrowed during the
 * call; output storage/capacities belong to the sizing pass. */
int ImportTextureEqual(const RageImportedTextureKey *left,
                       const RageImportedTextureKey *right);
int ImportWriteFace(uint32_t mesh, const RageImportedFace *face, void *context);
/* Verify the second pass matches its sizing limits before filling trailing
 * empty submesh endpoints. Rejection leaves writer and output unchanged. */
int ImportWriteFinish(RageImportedWrite *write, uint32_t meshes);

#endif
