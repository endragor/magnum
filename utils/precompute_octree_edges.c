#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

#define MAX_DEPTH 5

static inline uint32_t edges_for_depth(uint32_t depth)
{
    uint32_t cells_on_depth = 1;
    uint32_t edge_count = 0;
    for (uint32_t i = 0; i < depth; ++i) {
        uint32_t faces_on_depth = cells_on_depth * 12;
        for (uint32_t j = i + 1; j < depth; ++j) {
            uint32_t edges_on_depth = faces_on_depth * 4;
            for (uint32_t k = j + 1; k <= depth; ++k) {
                edge_count += edges_on_depth;
                edges_on_depth *= 2;
            }
            faces_on_depth *= 4;
        }

        {
            uint32_t edges_on_depth = cells_on_depth * 6;
            for (uint32_t k = i + 1; k <= depth; ++k) {
                edge_count += edges_on_depth;
                edges_on_depth *= 2;
            }
        }
        cells_on_depth *= 8;
    }

    return edge_count;
}

#define OCTREE_AXIS_Z 0
#define OCTREE_AXIS_Y 1
#define OCTREE_AXIS_X 2

#define CHILD_ID(parent, x, y, z) ((parent << 3) + (x << 2) + (y << 1) + (z << 0) + 1)
#define EDGE_PROC( \
        cell0, x0, y0, z0, \
        cell1, x1, y1, z1, \
        cell2, x2, y2, z2, \
        cell3, x3, y3, z3, \
        axis \
    ) \
    { \
        uint32_t new_cells[4] = {CHILD_ID(cell0, x0, y0, z0), CHILD_ID(cell1, x1, y1, z1), CHILD_ID(cell2, x2, y2, z2), CHILD_ID(cell3, x3, y3, z3)}; \
        edge_proc(new_cells, axis, depth + 1, edges, edge_index); \
    }

#define FACE_PROC( \
        cell0, x0, y0, z0, \
        cell1, x1, y1, z1, \
        axis \
    ) \
    { \
        uint32_t new_cells[2] = {CHILD_ID(cell0, x0, y0, z0), CHILD_ID(cell1, x1, y1, z1)}; \
        face_proc(new_cells, axis, depth + 1, edges, edge_index); \
    }

#define CELL_PROC(cell, x, y, z) cell_proc(CHILD_ID(cell, x, y, z), depth + 1, edges, edge_index)

void edge_proc(uint32_t cells[4], uint32_t axis, uint32_t depth, uint32_t *edges, uint32_t *edge_index)
{
    for (uint32_t i = 0; i < 4; ++i) {
        edges[*edge_index * 4 + i] = cells[i];
    }
    edges[*edge_index * 4] |= axis << 30;
    ++*edge_index;

    if (depth == MAX_DEPTH) return;

    if (axis == OCTREE_AXIS_X) {
        EDGE_PROC(
            cells[0], 0, 1, 1,
            cells[1], 0, 1, 0,
            cells[2], 0, 0, 1,
            cells[3], 0, 0, 0,
            OCTREE_AXIS_X);
        EDGE_PROC(
            cells[0], 1, 1, 1,
            cells[1], 1, 1, 0,
            cells[2], 1, 0, 1,
            cells[3], 1, 0, 0,
            OCTREE_AXIS_X);
    } else if (axis == OCTREE_AXIS_Y) {
        EDGE_PROC(
            cells[0], 1, 0, 1,
            cells[1], 0, 0, 1,
            cells[2], 1, 0, 0,
            cells[3], 0, 0, 0,
            OCTREE_AXIS_Y);
        EDGE_PROC(
            cells[0], 1, 1, 1,
            cells[1], 0, 1, 1,
            cells[2], 1, 1, 0,
            cells[3], 0, 1, 0,
            OCTREE_AXIS_Y);
    } else { // fixed-z
        EDGE_PROC(
            cells[0], 1, 1, 0,
            cells[1], 0, 1, 0,
            cells[2], 1, 0, 0,
            cells[3], 0, 0, 0,
            OCTREE_AXIS_Z);
        EDGE_PROC(
            cells[0], 1, 1, 1,
            cells[1], 0, 1, 1,
            cells[2], 1, 0, 1,
            cells[3], 0, 0, 1,
            OCTREE_AXIS_Z);
    }
}

void face_proc(uint32_t cells[2], uint32_t axis, uint32_t depth, uint32_t *edges, uint32_t *edge_index)
{
    if (depth == MAX_DEPTH) return;
    if (axis == OCTREE_AXIS_X) {
        FACE_PROC(
            cells[0], 1, 0, 0,
            cells[1], 0, 0, 0,
            OCTREE_AXIS_X);
        FACE_PROC(
            cells[0], 1, 0, 1,
            cells[1], 0, 0, 1,
            OCTREE_AXIS_X);
        FACE_PROC(
            cells[0], 1, 1, 0,
            cells[1], 0, 1, 0,
            OCTREE_AXIS_X);
        FACE_PROC(
            cells[0], 1, 1, 1,
            cells[1], 0, 1, 1,
            OCTREE_AXIS_X);

        EDGE_PROC(
            cells[0], 1, 0, 0,
            cells[1], 0, 0, 0,
            cells[0], 1, 0, 1,
            cells[1], 0, 0, 1,
            OCTREE_AXIS_Y);
        EDGE_PROC(
            cells[0], 1, 1, 0,
            cells[1], 0, 1, 0,
            cells[0], 1, 1, 1,
            cells[1], 0, 1, 1,
            OCTREE_AXIS_Y);
        EDGE_PROC(
            cells[0], 1, 0, 0,
            cells[1], 0, 0, 0,
            cells[0], 1, 1, 0,
            cells[1], 0, 1, 0,
            OCTREE_AXIS_Z);
        EDGE_PROC(
            cells[0], 1, 0, 1,
            cells[1], 0, 0, 1,
            cells[0], 1, 1, 1,
            cells[1], 0, 1, 1,
            OCTREE_AXIS_Z);
    } else if (axis == OCTREE_AXIS_Y) {
        FACE_PROC(
            cells[0], 0, 1, 0,
            cells[1], 0, 0, 0,
            OCTREE_AXIS_Y);
        FACE_PROC(
            cells[0], 0, 1, 1,
            cells[1], 0, 0, 1,
            OCTREE_AXIS_Y);
        FACE_PROC(
            cells[0], 1, 1, 0,
            cells[1], 1, 0, 0,
            OCTREE_AXIS_Y);
        FACE_PROC(
            cells[0], 1, 1, 1,
            cells[1], 1, 0, 1,
            OCTREE_AXIS_Y);

        EDGE_PROC(
            cells[0], 0, 1, 0,
            cells[0], 0, 1, 1,
            cells[1], 0, 0, 0,
            cells[1], 0, 0, 1,
            OCTREE_AXIS_X);
        EDGE_PROC(
            cells[0], 1, 1, 0,
            cells[0], 1, 1, 1,
            cells[1], 1, 0, 0,
            cells[1], 1, 0, 1,
            OCTREE_AXIS_X);
        EDGE_PROC(
            cells[0], 0, 1, 0,
            cells[0], 1, 1, 0,
            cells[1], 0, 0, 0,
            cells[1], 1, 0, 0,
            OCTREE_AXIS_Z);
        EDGE_PROC(
            cells[0], 0, 1, 1,
            cells[0], 1, 1, 1,
            cells[1], 0, 0, 1,
            cells[1], 1, 0, 1,
            OCTREE_AXIS_Z);
    } else { // Z
        FACE_PROC(
            cells[0], 0, 0, 1,
            cells[1], 0, 0, 0,
            OCTREE_AXIS_Z);
        FACE_PROC(
            cells[0], 0, 1, 1,
            cells[1], 0, 1, 0,
            OCTREE_AXIS_Z);
        FACE_PROC(
            cells[0], 1, 0, 1,
            cells[1], 1, 0, 0,
            OCTREE_AXIS_Z);
        FACE_PROC(
            cells[0], 1, 1, 1,
            cells[1], 1, 1, 0,
            OCTREE_AXIS_Z);

        EDGE_PROC(
            cells[0], 0, 0, 1,
            cells[1], 0, 0, 0,
            cells[0], 0, 1, 1,
            cells[1], 0, 1, 0,
            OCTREE_AXIS_X);
        EDGE_PROC(
            cells[0], 1, 0, 1,
            cells[1], 1, 0, 0,
            cells[0], 1, 1, 1,
            cells[1], 1, 1, 0,
            OCTREE_AXIS_X);
        EDGE_PROC(
            cells[0], 0, 0, 1,
            cells[0], 1, 0, 1,
            cells[1], 0, 0, 0,
            cells[1], 1, 0, 0,
            OCTREE_AXIS_Y);
        EDGE_PROC(
            cells[0], 0, 1, 1,
            cells[0], 1, 1, 1,
            cells[1], 0, 1, 0,
            cells[1], 1, 1, 0,
            OCTREE_AXIS_Y);
    }
}

void cell_proc(uint32_t cell, uint32_t depth, uint32_t *edges, uint32_t *edge_index)
{
    if (depth == MAX_DEPTH) return;
    CELL_PROC(cell, 0, 0, 0);
    CELL_PROC(cell, 0, 0, 1);
    CELL_PROC(cell, 0, 1, 0);
    CELL_PROC(cell, 0, 1, 1);
    CELL_PROC(cell, 1, 0, 0);
    CELL_PROC(cell, 1, 0, 1);
    CELL_PROC(cell, 1, 1, 0);
    CELL_PROC(cell, 1, 1, 1);

    FACE_PROC(
        cell, 0, 0, 0,
        cell, 0, 1, 0,
        OCTREE_AXIS_Y);
    FACE_PROC(
        cell, 0, 0, 1,
        cell, 0, 1, 1,
        OCTREE_AXIS_Y);
    FACE_PROC(
        cell, 1, 0, 0,
        cell, 1, 1, 0,
        OCTREE_AXIS_Y);
    FACE_PROC(
        cell, 1, 0, 1,
        cell, 1, 1, 1,
        OCTREE_AXIS_Y);

    FACE_PROC(
        cell, 0, 0, 0,
        cell, 1, 0, 0,
        OCTREE_AXIS_X);
    FACE_PROC(
        cell, 0, 0, 1,
        cell, 1, 0, 1,
        OCTREE_AXIS_X);
    FACE_PROC(
        cell, 0, 1, 0,
        cell, 1, 1, 0,
        OCTREE_AXIS_X);
    FACE_PROC(
        cell, 0, 1, 1,
        cell, 1, 1, 1,
        OCTREE_AXIS_X);

    FACE_PROC(
        cell, 0, 0, 0,
        cell, 0, 0, 1,
        OCTREE_AXIS_Z);
    FACE_PROC(
        cell, 0, 1, 0,
        cell, 0, 1, 1,
        OCTREE_AXIS_Z);
    FACE_PROC(
        cell, 1, 0, 0,
        cell, 1, 0, 1,
        OCTREE_AXIS_Z);
    FACE_PROC(
        cell, 1, 1, 0,
        cell, 1, 1, 1,
        OCTREE_AXIS_Z);

    EDGE_PROC(
        cell, 0, 0, 0,
        cell, 1, 0, 0,
        cell, 0, 0, 1,
        cell, 1, 0, 1,
        OCTREE_AXIS_Y);
    EDGE_PROC(
        cell, 0, 1, 0,
        cell, 1, 1, 0,
        cell, 0, 1, 1,
        cell, 1, 1, 1,
        OCTREE_AXIS_Y);
    EDGE_PROC(
        cell, 0, 0, 0,
        cell, 0, 0, 1,
        cell, 0, 1, 0,
        cell, 0, 1, 1,
        OCTREE_AXIS_X);
    EDGE_PROC(
        cell, 1, 0, 0,
        cell, 1, 0, 1,
        cell, 1, 1, 0,
        cell, 1, 1, 1,
        OCTREE_AXIS_X);
    EDGE_PROC(
        cell, 0, 0, 0,
        cell, 1, 0, 0,
        cell, 0, 1, 0,
        cell, 1, 1, 0,
        OCTREE_AXIS_Z);
    EDGE_PROC(
        cell, 0, 0, 1,
        cell, 1, 0, 1,
        cell, 0, 1, 1,
        cell, 1, 1, 1,
        OCTREE_AXIS_Z);
}

uint32_t main()
{
    uint32_t edge_count = edges_for_depth(MAX_DEPTH);
    uint32_t *edges = malloc(sizeof(int) * edge_count * 4);
    uint32_t edge_index = 0;
    cell_proc(0, 0, edges, &edge_index);
    assert(edge_count == edge_index);

    printf("// Generated with utils/precompute_octree_edges.c\n");
    printf("TM_STATIC_ASSERT(OCTREE_DEPTH == %u);\n", MAX_DEPTH);
    printf("#define OCTREE_EDGE_COUNT %u\n", edge_index);
    printf("static const uint32_t OCTREE_EDGES[OCTREE_EDGE_COUNT][4] = {\n");
    for (uint32_t i = 0; i < edge_index; ++i) {
        printf("\t{%u, %u, %u, %u},\n", edges[i * 4 + 0], edges[i * 4 + 1], edges[i * 4 + 2], edges[i * 4 + 3]);
    }
    printf("};\n");

    return 0;
}
