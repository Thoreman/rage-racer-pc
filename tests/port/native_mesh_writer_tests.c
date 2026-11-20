#include "native_mesh_writer.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { ++failures; fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)

int main(void) {
    CHECK(!ImportWriteFinish(NULL, 0));
    {
        uint8_t offsets[20] = {0};
        RageImportedWrite write = {.offsets = offsets, .meshLimit = 4,
            .vertexLimit = 4, .vertexCount = 4, .indexLimit = 6, .indexCount = 6};
        CHECK(ImportWriteFinish(&write, 4));
        CHECK(write.currentMesh == 4);
        for (unsigned i = 0; i < sizeof(offsets); ++i)
            CHECK(offsets[i] == (i > 0 && i % 4 == 0 ? 6 : 0));
    }
    for (unsigned test = 0; test < 5; ++test) {
        uint8_t bytes[20], saved[20];
        memset(bytes, 0xA5, sizeof(bytes));
        memcpy(saved, bytes, sizeof(bytes));
        RageImportedWrite write = {.offsets = bytes, .meshLimit = 4,
            .vertexLimit = 4, .vertexCount = 4, .indexLimit = 6, .indexCount = 6};
        uint32_t meshes = 4;
        if (test == 0) meshes = 5;
        if (test == 1) meshes = 3;
        if (test == 2) write.vertexCount = 0;
        if (test == 3) write.indexCount = 0;
        if (test == 4) write.currentMesh = 5;
        uint32_t before = write.currentMesh;
        CHECK(!ImportWriteFinish(&write, meshes));
        CHECK(write.currentMesh == before);
        CHECK(memcmp(bytes, saved, sizeof(bytes)) == 0);
    }
    SVec positions[4] = {0};
    RageImportedMeshEntry entry = {0};
    RageImportedTextureKey material = {0};
    entry.materials = &material;
    entry.materialCount = 1;
    for (unsigned test = 0; test < 8; ++test) {
        unsigned char bytes[256], saved[256];
        memset(bytes, 0xA5, sizeof(bytes));
        RageImportedFace face = {0};
        face.vertices = positions;
        for (unsigned i = 0; i < 4; ++i) face.vertex[i] = (uint16_t)i;
        RageImportedWrite write = {0};
        write.entry = &entry;
        write.offsets = bytes + 8;
        write.vertexCursor = bytes + 32;
        write.indexCursor = bytes + 192;
        write.meshLimit = 1;
        write.vertexLimit = 4;
        write.indexLimit = 6;
        uint32_t mesh = 0;
        if (test == 0) mesh = 1; /* More meshes than sizing pass. */
        if (test == 1) write.currentMesh = 1; /* Reversed traversal. */
        if (test == 2) write.vertexLimit = 3;
        if (test == 3) write.indexLimit = 5;
        if (test == 4) write.vertexCount = UINT32_MAX;
        if (test == 5) write.indexCount = UINT32_MAX;
        if (test == 6) { face.textured = 1; face.texture.tpage = 1; }
        if (test == 7) {
            /* First face fits exactly; second face must not write anything. */
            CHECK(ImportWriteFace(0, &face, &write));
            CHECK(write.vertexCount == 4 && write.indexCount == 6);
            for (unsigned i = 0; i < 8; ++i) CHECK(bytes[i] == 0xA5);
            for (unsigned i = 216; i < sizeof(bytes); ++i) CHECK(bytes[i] == 0xA5);
        }
        memcpy(saved, bytes, sizeof(bytes));
        RageImportedWrite before = write;
        CHECK(!ImportWriteFace(mesh, &face, &write));
        CHECK(memcmp(bytes, saved, sizeof(bytes)) == 0);
        CHECK(write.currentMesh == before.currentMesh &&
              write.vertexCount == before.vertexCount && write.indexCount == before.indexCount);
        CHECK(write.vertexCursor == before.vertexCursor && write.indexCursor == before.indexCursor);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        uint8_t bytes[216] = {0};
        RageImportedFace face = {0};
        SVec normal = {.vx = 10, .vy = 20, .vz = -30};
        positions[0] = (SVec){.vx = 11, .vy = -22, .vz = 33};
        face.vertices = positions;
        face.normals = &normal;
        face.hasNormals = mode != 0;
        face.textured = mode != 0;
        face.prim = mode == 1 ? 3 : 0;
        face.flags = mode == 2 ? 2 : 0;
        face.depthBias = mode == 2 ? -4 : 0;
        face.color[0] = 17; face.color[1] = 34; face.color[2] = 51;
        face.uv[0][0] = 127; face.uv[0][1] = 255;
        entry.cached.assetSet = mode == 2 ? RAGE_RENDER_ASSET_TERRAIN : RAGE_RENDER_ASSET_COURSE;
        RageImportedWrite write = {.entry = &entry, .offsets = bytes + 24,
            .vertexCursor = bytes + 32, .indexCursor = bytes + 192,
            .meshLimit = 1, .vertexLimit = 4, .indexLimit = 6};
        CHECK(RuntimeMeshEncodeHeader(bytes, sizeof(bytes), 1, 4, 6));
        CHECK(ImportWriteFace(0, &face, &write));
        CHECK(ImportWriteFinish(&write, 1));
        CHECK(ImportWriteFinish(&write, 1)); /* Repeated finalization is harmless. */
        RageRuntimeMesh mesh;
        RageRuntimeVertex vertex;
        CHECK(RuntimeMeshOpen(&mesh, bytes, sizeof(bytes)));
        CHECK(RuntimeMeshVertex(&mesh, 0, &vertex));
        CHECK(vertex.position[0] == 11 && vertex.position[1] == 22 && vertex.position[2] == -33);
        CHECK(vertex.normal[0] == (mode ? 10 : 0) &&
              vertex.normal[1] == (mode ? -20 : 1) && vertex.normal[2] == (mode ? 30 : 0));
        CHECK(vertex.color[0] == 17 && vertex.color[1] == 34 && vertex.color[2] == 51 && vertex.color[3] == 255);
        CHECK(vertex.uv[0] == (mode ? 127.5f / 256 : 0) && vertex.uv[1] == (mode ? 255.5f / 256 : 0));
        const uint32_t expectedMaterial[] = {UINT32_MAX, UINT32_C(0x80000000), UINT32_C(0x74FC0000)};
        CHECK(vertex.material == expectedMaterial[mode]);
        const uint32_t expectedIndices[] = {0, 2, 1, 1, 2, 3};
        for (unsigned i = 0; i < 6; ++i) {
            uint32_t index;
            CHECK(RuntimeMeshIndex(&mesh, i, &index));
            CHECK(index == expectedIndices[i]);
        }
    }
    {
        /* Course 1 pit facade and the banner hung in front of it (renderer
         * space, captured from a draw dump): the banner's lower edge sinks
         * ~7 units behind the wall. */
        RageImportedQuad quads[4] = {
            {{40404.5f, -5665.5f, -12335.5f}, {40732.5f, -5665.5f, -12369.5f},
             {40403.5f, -5815.5f, -12336.25f}, {40732.0f, -5830.0f, -12370.5f}},
            {{40497.75f, -5613.75f, -12335.75f}, {40752.75f, -5613.75f, -12362.25f},
             {40496.25f, -5722.75f, -12352.25f}, {40751.0f, -5723.25f, -12378.75f}},
            /* Same banner moved well clear: untouched. */
            {{40497.75f, -5613.75f, -12305.75f}, {40752.75f, -5613.75f, -12332.25f},
             {40496.25f, -5722.75f, -12322.25f}, {40751.0f, -5723.25f, -12348.75f}},
            /* Penetrating but attached to the wall by a corner: untouched. */
            {{40404.5f, -5665.5f, -12335.5f}, {40600.0f, -5600.0f, -12345.0f},
             {40450.0f, -5760.0f, -12352.0f}, {40600.0f, -5760.0f, -12370.0f}},
        };
        float offsets[4][3];
        const uint32_t meshes[4] = {7, 7, 7, 7};
        CHECK(ImportResolveSignPenetration((const RageImportedQuad *)quads, meshes, 4,
                                           1.0f, offsets) == 1);
        {
            /* The same scene in GTE units (x4) moves by four times as much;
             * a wall in a different mesh is not considered. */
            RageImportedQuad scaled[4];
            float scaledOffsets[4][3];
            const uint32_t split[4] = {6, 7, 7, 7};
            for (unsigned f = 0; f < 4; ++f)
                for (unsigned c = 0; c < 4; ++c)
                    for (unsigned a = 0; a < 3; ++a) scaled[f][c][a] = quads[f][c][a] * 4.0f;
            CHECK(ImportResolveSignPenetration((const RageImportedQuad *)scaled, meshes, 4,
                                               4.0f, scaledOffsets) == 1);
            CHECK(fabsf(scaledOffsets[1][2] - 4.0f * offsets[1][2]) < 0.05f);
            CHECK(ImportResolveSignPenetration((const RageImportedQuad *)quads, split, 4,
                                               1.0f, scaledOffsets) == 0);
        }
        CHECK(offsets[0][0] == 0 && offsets[0][1] == 0 && offsets[0][2] == 0);
        CHECK(offsets[2][0] == 0 && offsets[2][1] == 0 && offsets[2][2] == 0);
        CHECK(offsets[3][0] == 0 && offsets[3][1] == 0 && offsets[3][2] == 0);
        CHECK(offsets[1][1] == 0 && offsets[1][2] > 5.0f && offsets[1][2] < 16.0f);
        {
            /* Every moved corner now clears the wall plane by >= 5.9. */
            float ax = quads[0][1][0] - quads[0][0][0];
            float az = quads[0][1][2] - quads[0][0][2];
            float length = sqrtf(ax * ax + az * az);
            float nx = -az / length, nz = ax / length;
            if (nz < 0) { nx = -nx; nz = -nz; }
            for (unsigned corner = 0; corner < 4; ++corner) {
                float d = (quads[1][corner][0] + offsets[1][0] - quads[0][0][0]) * nx +
                          (quads[1][corner][2] + offsets[1][2] - quads[0][0][2]) * nz;
                CHECK(d >= 5.9f);
            }
        }
    }
    {
        /* A road dipping 10 units at x=400 and a flat dash over it from
         * x=100 to 700. Draped, every piece corner lies on the road and the
         * texture runs on continuously. */
        RageImportedQuad quads[3] = {
            {{0, 0, -50}, {0, 0, 50}, {400, -10, -50}, {400, -10, 50}},
            {{400, -10, -50}, {400, -10, 50}, {800, 0, -50}, {800, 0, 50}},
            {{100, -4, -5}, {100, -4, 5}, {700, -4, -5}, {700, -4, 5}},
        };
        float uvs[3][4][2] = {{{0}}, {{0}}, {{0, 0}, {0, 8}, {60, 0}, {60, 8}}};
        uint32_t meshes[3] = {1, 1, 1};
        RageImportedDrape drapes[3];
        RageImportedPiece *pieces = NULL;
        uint32_t count = ImportDrapeRoadMarkings((const RageImportedQuad *)quads,
            (const float (*)[4][2])uvs, meshes, NULL, 3, 1.0f, 0.0f, drapes, &pieces);
        CHECK(count >= 2 && pieces != NULL);
        CHECK(drapes[0].count == 0 && drapes[1].count == 0);
        CHECK(drapes[2].count == count && drapes[2].first == 0);
        float covered = 0.0f;
        for (uint32_t p = 0; pieces != NULL && p < count; ++p) {
            for (unsigned c = 0; c < 4; ++c) {
                float x = pieces[p].position[c][0];
                float road = x <= 400 ? -0.025f * x : -10.0f + 0.025f * (x - 400);
                CHECK(fabsf(pieces[p].position[c][1] - road) < 0.01f);
                CHECK(fabsf(pieces[p].uv[c][0] - (x - 100) / 10) < 0.01f);
            }
            covered += pieces[p].position[2][0] - pieces[p].position[0][0];
        }
        CHECK(fabsf(covered - 600.0f) < 0.01f);
        free(pieces);

        /* The second road quad in a neighbouring mesh, in its own local
         * space 400 units along: found only when placements are given. */
        RageImportedQuad split[3] = {
            {{0, 0, -50}, {0, 0, 50}, {400, -10, -50}, {400, -10, 50}},
            {{100, -4, -5}, {100, -4, 5}, {700, -4, -5}, {700, -4, 5}},
            {{0, -10, -50}, {0, -10, 50}, {400, 0, -50}, {400, 0, 50}},
        };
        float splitUvs[3][4][2] = {{{0}}, {{0, 0}, {0, 8}, {60, 0}, {60, 8}}, {{0}}};
        uint32_t splitMeshes[3] = {1, 1, 2};
        float origins[3][2] = {{0, 0}, {0, 0}, {400, 0}};
        CHECK(ImportDrapeRoadMarkings((const RageImportedQuad *)split,
            (const float (*)[4][2])splitUvs, splitMeshes, NULL, 3, 1.0f, 0.0f,
            drapes, &pieces) == 0);
        CHECK(pieces == NULL && drapes[1].count == 0);
        count = ImportDrapeRoadMarkings((const RageImportedQuad *)split,
            (const float (*)[4][2])splitUvs, splitMeshes,
            (const float (*)[2])origins, 3, 1.0f, 1000.0f, drapes, &pieces);
        CHECK(count >= 2 && drapes[1].count == count);
        for (uint32_t p = 0; pieces != NULL && p < count; ++p)
            for (unsigned c = 0; c < 4; ++c) {
                float x = pieces[p].position[c][0];
                float road = x <= 400 ? -0.025f * x : -10.0f + 0.025f * (x - 400);
                CHECK(fabsf(pieces[p].position[c][1] - road) < 0.01f);
            }
        free(pieces);
    }
    {
        /* Drape pieces replace their face in the written mesh; the face
         * count, not the vertex count, indexes per-face data. */
        uint8_t bytes[2048] = {0};
        RageRuntimeMeshLayout layout;
        CHECK(RuntimeMeshLayout(1, 12, 18, &layout) && layout.totalSize <= sizeof(bytes));
        CHECK(RuntimeMeshEncodeHeader(bytes, layout.totalSize, 1, 12, 18));
        SVec corners[4] = {{0, 0, 0, 0}, {0, 0, 10, 0}, {300, 0, 0, 0}, {300, 0, 10, 0}};
        RageImportedFace face = {0};
        RageImportedPiece twoPieces[2] = {0};
        RageImportedDrape drape[2] = {{0, 2}, {0, 0}};
        float faceOffsets[2][3] = {{0, 0, 0}, {0, 5, 0}};
        RageImportedWrite write = {0};
        face.vertices = corners;
        for (unsigned i = 0; i < 4; ++i) face.vertex[i] = (uint16_t)i;
        twoPieces[1].position[3][0] = 77.0f;
        write.entry = &entry;
        write.offsets = bytes + layout.offsetsOffset;
        write.vertexCursor = bytes + layout.verticesOffset;
        write.indexCursor = bytes + layout.indicesOffset;
        write.meshLimit = 1;
        write.vertexLimit = 12;
        write.indexLimit = 18;
        write.drapes = drape;
        write.pieces = twoPieces;
        write.drapeCount = 2;
        write.faceOffsets = (const float (*)[3])faceOffsets;
        write.faceOffsetCount = 2;
        CHECK(ImportWriteFace(0, &face, &write));
        CHECK(write.vertexCount == 8 && write.indexCount == 12 && write.sourceFace == 1);
        CHECK(ImportWriteFace(0, &face, &write));
        CHECK(write.vertexCount == 12 && write.sourceFace == 2);
        CHECK(!ImportWriteFace(0, &face, &write)); /* no room left */
        CHECK(ImportWriteFinish(&write, 1));
        RageRuntimeMesh mesh;
        RageRuntimeVertex vertex;
        CHECK(RuntimeMeshOpen(&mesh, bytes, layout.totalSize));
        CHECK(RuntimeMeshVertex(&mesh, 7, &vertex));
        CHECK(vertex.position[0] == 77.0f);
        CHECK((vertex.material & RAGE_RUNTIME_MATERIAL_TERRAIN_ROAD_DECAL) != 0);
        CHECK(RuntimeMeshVertex(&mesh, 8, &vertex));
        CHECK(vertex.position[1] == 5.0f); /* second face's own offset */
        CHECK((vertex.material & RAGE_RUNTIME_MATERIAL_TERRAIN_ROAD_DECAL) == 0);
    }
    return failures ? 1 : 0;
}
