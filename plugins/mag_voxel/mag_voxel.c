static struct tm_error_api *tm_error_api;

#include "mag_voxel.h"

#include <foundation/allocator.h>
#include <foundation/api_registry.h>
#include <foundation/api_type_hashes.h>
#include <foundation/error.h>
#include <foundation/unit_test.h>

#include <foundation/carray.inl>
#include <foundation/hash.inl>
#include <foundation/math.inl>
#include <foundation/murmurhash64a.inl>

#include <plugins/renderer/render_backend.h>
#include <plugins/renderer/render_command_buffer.h>
#include <plugins/renderer/renderer.h>
#include <plugins/renderer/resources.h>
#include <plugins/shader_system/shader_system.h>

#define tm_hash_get_ptr(h, key) ((h)->values + tm_hash_index(h, key))

static struct tm_shader_api *tm_shader_api;
static struct tm_renderer_api *tm_renderer_api;
static struct tm_temp_allocator_api *tm_temp_allocator_api;

#define ADAPT(v0, v1) (-v0 / (v1 - v0))

static tm_vec3_t find_vertex(float x, float y, float z, tm_vec3_t p[12], tm_vec3_t n[12], int count, float corner_signs[8])
{
    // Based on https://www.inf.ufrgs.br/~comba/papers/thesis/diss-leonardo.pdf

    tm_vec3_t edges[8] = {
        { x, y, z },
        { x, y, z + 1 },
        { x, y + 1, z },
        { x, y + 1, z + 1 },
        { x + 1, y, z },
        { x + 1, y, z + 1 },
        { x + 1, y + 1, z },
        { x + 1, y + 1, z + 1 },
    };
    // force at each edge
    tm_vec3_t f[8] = { 0 };

    tm_vec3_t c = { 0 };
    float one_to_n = 1.f / (float)count;
    for (int i = 0; i < count; i++) {
        // distance to the plane at point p[i] with normal n[i]
        float d = -tm_vec3_dot(p[i], n[i]);

        c = tm_vec3_add(c, tm_vec3_mul(p[i], one_to_n));
        for (int e = 0; e < 8; ++e) {
            // add force pointed from the edge towards the plane
            f[e] = tm_vec3_add(f[e], tm_vec3_mul(n[i], -corner_signs[e] * fabsf(tm_vec3_dot(edges[e], n[i]) + d)));
        }
    }

    for (int i = 0; i < 50; ++i) {
        // trilinear interpolation
        tm_vec3_t cd = { c.x - x, c.y - y, c.z - z };
        tm_vec3_t fx1 = tm_vec3_lerp(f[0], f[4], cd.x);
        tm_vec3_t fx2 = tm_vec3_lerp(f[1], f[5], cd.x);
        tm_vec3_t fx3 = tm_vec3_lerp(f[2], f[6], cd.x);
        tm_vec3_t fx4 = tm_vec3_lerp(f[3], f[7], cd.x);

        tm_vec3_t fy1 = tm_vec3_lerp(fx1, fx3, cd.y);
        tm_vec3_t fy2 = tm_vec3_lerp(fx2, fx4, cd.y);

        tm_vec3_t f_total = tm_vec3_lerp(fy1, fy2, cd.z);

        float f_len = tm_vec3_dot(f_total, f_total);
        if (f_len * 0.0025f < 0.00001f) {
            break;
        }

        c = tm_vec3_add(c, tm_vec3_mul(f_total, 0.05f));
    }

    c = tm_vec3_clamp(c, (tm_vec3_t) { x, y, z }, (tm_vec3_t) { x + 1, y + 1, z + 1 });

    c.x -= (float)MAG_VOXEL_MARGIN;
    c.y -= (float)MAG_VOXEL_MARGIN;
    c.z -= (float)MAG_VOXEL_MARGIN;

    return c;
}

static bool dc_cell_vertex(const mag_voxel_region_t *region, int x, int y, int z, tm_vec3_t *vertex)
{
    const float(*v)[MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE] = region->densities;
    const tm_vec3_t(*n)[MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE] = region->normals;

    // positions of sign changes on the edges of the cell
    tm_vec3_t changes[12];
    // normals at the positions
    tm_vec3_t normals[12];
    int change_idx = 0;

    for (int dx = 0; dx != 2; ++dx) {
        for (int dy = 0; dy != 2; ++dy) {
            if ((v[x + dx][y + dy][z] > 0) != (v[x + dx][y + dy][z + 1] > 0)) {
                float distance = ADAPT(v[x + dx][y + dy][z], v[x + dx][y + dy][z + 1]);
                changes[change_idx] = (tm_vec3_t) { .x = (float)(x + dx), .y = (float)(y + dy), .z = (float)z + distance };
                normals[change_idx] = tm_vec3_normalize(tm_vec3_lerp(n[x + dx][y + dy][z], n[x + dx][y + dy][z + 1], distance));
                ++change_idx;
            }
        }
    }

    for (int dx = 0; dx != 2; ++dx) {
        for (int dz = 0; dz != 2; ++dz) {
            if ((v[x + dx][y][z + dz] > 0) != (v[x + dx][y + 1][z + dz] > 0)) {
                float distance = ADAPT(v[x + dx][y][z + dz], v[x + dx][y + 1][z + dz]);
                changes[change_idx] = (tm_vec3_t) { .x = (float)(x + dx), .y = (float)y + distance, .z = (float)(z + dz) };
                normals[change_idx] = tm_vec3_normalize(tm_vec3_lerp(n[x + dx][y][z + dz], n[x + dx][y + 1][z + dz], distance));
                ++change_idx;
            }
        }
    }

    for (int dy = 0; dy != 2; ++dy) {
        for (int dz = 0; dz != 2; ++dz) {
            if ((v[x][y + dy][z + dz] > 0) != (v[x + 1][y + dy][z + dz] > 0)) {
                float distance = ADAPT(v[x][y + dy][z + dz], v[x + 1][y + dy][z + dz]);
                changes[change_idx] = (tm_vec3_t) { .x = (float)x + distance, .y = (float)(y + dy), .z = (float)(z + dz) };
                normals[change_idx] = tm_vec3_normalize(tm_vec3_lerp(n[x][y + dy][z + dz], n[x + 1][y + dy][z + dz], distance));
                ++change_idx;
            }
        }
    }

    if (change_idx <= 1)
        return false;

    float corner_signs[8] = {
        v[x + 0][y + 0][z + 0] >= 0.f ? 1.f : -1.f,
        v[x + 0][y + 0][z + 1] >= 0.f ? 1.f : -1.f,
        v[x + 0][y + 1][z + 0] >= 0.f ? 1.f : -1.f,
        v[x + 0][y + 1][z + 1] >= 0.f ? 1.f : -1.f,
        v[x + 1][y + 0][z + 0] >= 0.f ? 1.f : -1.f,
        v[x + 1][y + 0][z + 1] >= 0.f ? 1.f : -1.f,
        v[x + 1][y + 1][z + 0] >= 0.f ? 1.f : -1.f,
        v[x + 1][y + 1][z + 1] >= 0.f ? 1.f : -1.f,
    };
    *vertex = find_vertex((float)x, (float)y, (float)z, changes, normals, change_idx, corner_signs);
    return true;
}

static bool set_resource(tm_shader_io_o *io, tm_renderer_resource_command_buffer_o *res_buf, tm_shader_resource_binder_instance_t *instance,
    tm_strhash_t name, const tm_renderer_handle_t *resource_handle, const uint32_t *aspect_flags, uint32_t first_resource, uint32_t n_resources)
{
    tm_shader_resource_t resource;
    uint32_t resource_slot;
    if (tm_shader_api->lookup_resource(io, name, &resource, &resource_slot)) {
        tm_shader_api->update_resources(io, res_buf, &(tm_shader_resource_update_t) { .instance_id = instance->instance_id, .resource_slot = resource_slot, .first_resource = first_resource, .num_resources = n_resources, .resources = resource_handle, .resources_view_aspect_flags = aspect_flags }, 1);
        return true;
    }
    return false;
}

enum {
    TM_VERTEX_SEMANTIC_POSITION = 0,
    TM_VERTEX_SEMANTIC_NORMAL,
    TM_VERTEX_SEMANTIC_TANGENT0,
    TM_VERTEX_SEMANTIC_TANGENT1,
    TM_VERTEX_SEMANTIC_SKIN_DATA,
    TM_VERTEX_SEMANTIC_TEXCOORD0,
    TM_VERTEX_SEMANTIC_TEXCOORD1,
    TM_VERTEX_SEMANTIC_TEXCOORD2,
    TM_VERTEX_SEMANTIC_TEXCOORD3,
    TM_VERTEX_SEMANTIC_COLOR0,
    TM_VERTEX_SEMANTIC_COLOR1,
    TM_VERTEX_SEMANTIC_MAX_SEMANTICS,
    TM_INDEX_SEMANTIC = 16
};

static void dual_contour_region(
    const mag_voxel_region_t *region,
    tm_renderer_backend_i *backend,
    tm_shader_io_o *io,
    mag_voxel_mesh_t *out_mesh,
    tm_shader_resource_binder_instance_t *inout_rbinder,
    tm_shader_constant_buffer_instance_t *inout_cbuffer)
{
    TM_INIT_TEMP_ALLOCATOR(ta);
    /* carray */ tm_vec3_t *vertices = 0;
    const int CAPACITY_INCREMENT = 256;
    int vertex_capacity = CAPACITY_INCREMENT;
    uint16_t vertex_count = 0;
    tm_carray_temp_resize(vertices, vertex_capacity, ta);

    uint16_t cell_vertices[MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE];

    for (int x = 0; x < MAG_VOXEL_REGION_SIZE - 1; ++x) {
        for (int y = 0; y < MAG_VOXEL_REGION_SIZE - 1; ++y) {
            for (int z = 0; z < MAG_VOXEL_REGION_SIZE - 1; ++z) {
                tm_vec3_t vertex;
                if (dc_cell_vertex(region, x, y, z, &vertex)) {
                    if (vertex_count >= vertex_capacity) {
                        vertex_capacity += CAPACITY_INCREMENT;
                        tm_carray_temp_resize(vertices, vertex_capacity, ta);
                    }
                    vertices[vertex_count] = vertex;
                    cell_vertices[x][y][z] = vertex_count;
                    ++vertex_count;
                }
            }
        }
    }
    if (vertex_count < 4) {
        *out_mesh = (mag_voxel_mesh_t) { 0 };
        goto end;
    }

    // TODO: we actually know the number of active edges at this point,
    // but this is a naive implementation

    /* carray */ uint16_t *triangles = 0;
    int ti = 0;
    int t_capacity = CAPACITY_INCREMENT;
    tm_carray_temp_resize(triangles, t_capacity, ta);

    const float(*v)[MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE] = region->densities;
    for (int x = 0; x < MAG_VOXEL_REGION_SIZE - 1; ++x) {
        for (int y = 0; y < MAG_VOXEL_REGION_SIZE - 1; ++y) {
            for (int z = 0; z < MAG_VOXEL_REGION_SIZE - 1; ++z) {
                if (x > 0 && y > 0) {
                    bool solid0 = (v[x][y][z] > 0);
                    bool solid1 = (v[x][y][z + 1] > 0);
                    if (solid0 != solid1) {
                        if (ti + 6 > t_capacity) {
                            t_capacity += CAPACITY_INCREMENT;
                            tm_carray_temp_resize(triangles, t_capacity, ta);
                        }
                        int swap = solid1 ? 2 : 0;
                        triangles[ti + 2 - swap] = cell_vertices[x - 1][y - 1][z];
                        triangles[ti + 1] = cell_vertices[x - 0][y - 1][z];
                        triangles[ti + 0 + swap] = cell_vertices[x - 1][y - 0][z];

                        triangles[ti + 3 + swap] = cell_vertices[x - 1][y - 0][z];
                        triangles[ti + 4] = cell_vertices[x - 0][y - 0][z];
                        triangles[ti + 5 - swap] = cell_vertices[x - 0][y - 1][z];
                        ti += 6;
                    }
                }

                if (x > 0 && z > 0) {
                    bool solid0 = (v[x][y][z] > 0);
                    bool solid1 = (v[x][y + 1][z] > 0);
                    if (solid0 != solid1) {
                        if (ti + 6 > t_capacity) {
                            t_capacity += CAPACITY_INCREMENT;
                            tm_carray_temp_resize(triangles, t_capacity, ta);
                        }
                        int swap = solid0 ? 2 : 0;
                        triangles[ti + 2 - swap] = cell_vertices[x - 1][y][z - 1];
                        triangles[ti + 1] = cell_vertices[x - 0][y][z - 1];
                        triangles[ti + 0 + swap] = cell_vertices[x - 1][y][z - 0];

                        triangles[ti + 3 + swap] = cell_vertices[x - 1][y][z - 0];
                        triangles[ti + 4] = cell_vertices[x - 0][y][z - 0];
                        triangles[ti + 5 - swap] = cell_vertices[x - 0][y][z - 1];
                        ti += 6;
                    }
                }

                if (y > 0 && z > 0) {
                    bool solid0 = (v[x][y][z] > 0);
                    bool solid1 = (v[x + 1][y][z] > 0);
                    if (solid0 != solid1) {
                        if (ti + 6 > t_capacity) {
                            t_capacity += CAPACITY_INCREMENT;
                            tm_carray_temp_resize(triangles, t_capacity, ta);
                        }
                        int swap = solid1 ? 2 : 0;
                        triangles[ti + 0 + swap] = cell_vertices[x][y - 1][z - 0];
                        triangles[ti + 1] = cell_vertices[x][y - 0][z - 1];
                        triangles[ti + 2 - swap] = cell_vertices[x][y - 1][z - 1];

                        triangles[ti + 3 + swap] = cell_vertices[x][y - 1][z - 0];
                        triangles[ti + 4] = cell_vertices[x][y - 0][z - 0];
                        triangles[ti + 5 - swap] = cell_vertices[x][y - 0][z - 1];
                        ti += 6;
                    }
                }
            }
        }
    }

    tm_renderer_resource_command_buffer_o *res_buf;
    backend->create_resource_command_buffers(backend->inst, &res_buf, 1);

    void *ibuf_data;
    out_mesh->ibuf = tm_renderer_api->tm_renderer_resource_command_buffer_api->map_create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = ti * sizeof(uint16_t), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_INDEX | TM_RENDERER_BUFFER_USAGE_UPDATABLE, .debug_tag = "voxel_ibuf" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, 0, &ibuf_data);
    memcpy(ibuf_data, triangles, ti * sizeof(uint16_t));
    out_mesh->num_indices = (uint32_t)ti;

    void *vbuf_data;
    out_mesh->vbuf = tm_renderer_api->tm_renderer_resource_command_buffer_api->map_create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = vertex_count * sizeof(tm_vec3_t), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_ACCELERATION_STRUCTURE | TM_RENDERER_BUFFER_USAGE_UPDATABLE, .debug_tag = "voxel_vbuf" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, 0, &vbuf_data);
    memcpy(vbuf_data, vertices, vertex_count * sizeof(tm_vec3_t));

    if (!inout_rbinder->instance_id)
        tm_shader_api->create_resource_binder_instances(io, 1, inout_rbinder);
    if (!inout_cbuffer->instance_id)
        tm_shader_api->create_constant_buffer_instances(io, 1, inout_cbuffer);

    set_resource(io, res_buf, inout_rbinder, TM_STATIC_HASH("vertex_buffer_position_buffer", 0x1ef08bede3820d69ULL), &out_mesh->vbuf, 0, 0, 1);
    set_resource(io, res_buf, inout_rbinder, TM_STATIC_HASH("index_buffer", 0xb773460d24bcec1fULL), &out_mesh->ibuf, 0, 0, 1);

#include <the_machinery/shaders/vertex_buffer_system.inl>
    tm_shader_vertex_buffer_system_t constants = { 0 };
    uint32_t *strides = (uint32_t *)&constants.vertex_buffer_strides;
    uint32_t *offsets = (uint32_t *)&constants.vertex_buffer_offsets;

    constants.vertex_buffer_header[0] |= (1 << TM_VERTEX_SEMANTIC_POSITION) | (1 << TM_INDEX_SEMANTIC);
    const uint32_t num_vertices = vertex_count;
    constants.vertex_buffer_header[1] = num_vertices;
    offsets[TM_VERTEX_SEMANTIC_POSITION] = 0;
    strides[TM_VERTEX_SEMANTIC_POSITION] = sizeof(tm_vec3_t);

    constants.index_buffer_offset_and_stride[0] = 0;
    constants.index_buffer_offset_and_stride[1] = 2;

    void *cbuf = (void *)&constants;
    tm_shader_api->update_constants_raw(io, res_buf,
        &inout_cbuffer->instance_id, (const void **)&cbuf, 0, sizeof(tm_shader_vertex_buffer_system_t), 1);

    backend->submit_resource_command_buffers(backend->inst, &res_buf, 1);
    backend->destroy_resource_command_buffers(backend->inst, &res_buf, 1);

end:
    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static struct mag_voxel_api mag_voxel_api = {
    .dual_contour_region = dual_contour_region,
};

typedef struct aabb_t
{
    tm_vec3_t center;
    tm_vec3_t half_size;
} aabb_t;

static aabb_t aabb_from_min_max(tm_vec3_t min, tm_vec3_t max)
{
    tm_vec3_t half_size = tm_vec3_mul(tm_vec3_sub(max, min), 0.5f);
    tm_vec3_t center = tm_vec3_add(min, half_size);
    return (aabb_t) { center, half_size };
}

static bool aabb_intersect(const aabb_t *a, const aabb_t *b)
{
    tm_vec3_t amin = tm_vec3_sub(a->center, a->half_size);
    tm_vec3_t amax = tm_vec3_add(a->center, a->half_size);

    tm_vec3_t bmin = tm_vec3_sub(b->center, b->half_size);
    tm_vec3_t bmax = tm_vec3_add(b->center, b->half_size);

    return (
        amin.x <= bmax.x && amax.x >= bmin.x && amin.y <= bmax.y && amax.y >= bmin.y && amin.z <= bmax.z && amax.z >= bmin.z);
}

typedef struct mag_tree_region_t
{
    tm_vec3_t pos;
    float cell_size;
    uint64_t key;
} mag_tree_region_t;

typedef struct mag_region_tree_node_t
{
    /* carray */ mag_tree_region_t *regions;
    uint8_t child_mask;
} mag_region_tree_node_t;

typedef struct mag_region_tree_t
{
    struct TM_HASH_T(uint64_t, mag_region_tree_node_t) nodes;
    mag_region_tree_node_t root;
    aabb_t aabb;
    tm_allocator_i *allocator;
} mag_region_tree_t;

static aabb_t tree_region_aabb(tm_vec3_t region_pos, float cell_size)
{
    float region_size = (float)MAG_VOXEL_CHUNK_SIZE * cell_size;
    float half_size = region_size * 0.5f;
    tm_vec3_t half_size_vec = { half_size, half_size, half_size };
    return (aabb_t) { tm_vec3_add(region_pos, half_size_vec), half_size_vec };
}

static aabb_t child_aabb(uint32_t child_idx, const aabb_t *node_aabb)
{
    tm_vec3_t child_half_size = tm_vec3_mul(node_aabb->half_size, 0.5f);
    tm_vec3_t center_diff = {
        child_idx & 1 ? child_half_size.x : -child_half_size.x,
        child_idx & 2 ? child_half_size.y : -child_half_size.y,
        child_idx & 4 ? child_half_size.z : -child_half_size.z,
    };
    tm_vec3_t child_center = tm_vec3_add(node_aabb->center, center_diff);

    return (aabb_t) { child_center, child_half_size };
}

static uint32_t child_idx_for_point(tm_vec3_t node_center, tm_vec3_t point)
{
    tm_vec3_t obj_center_diff = tm_vec3_sub(point, node_center);
    uint32_t child_i = 0;
    child_i |= obj_center_diff.x >= 0 ? 1 : 0;
    child_i |= obj_center_diff.y >= 0 ? 2 : 0;
    child_i |= obj_center_diff.z >= 0 ? 4 : 0;
    return child_i;
}

static mag_region_tree_t *octree_create(tm_allocator_i *allocator, tm_vec3_t min, tm_vec3_t max)
{
    mag_region_tree_t *result = tm_alloc(allocator, sizeof(mag_region_tree_t));
    *result = (mag_region_tree_t) {
        .allocator = allocator,
        .aabb = aabb_from_min_max(min, max),
        .nodes = { .allocator = allocator },
    };
    return result;
}

static void octree_free_regions_recur(mag_region_tree_t *octree, uint64_t node_id, mag_region_tree_node_t *node)
{
    uint8_t child_mask = node->child_mask;
    if (child_mask) {
        for (uint32_t i = 0; i < 8; ++i) {
            if (child_mask & ((uint8_t)1 << i)) {
                uint64_t child_id = (node_id << 3) | i;
                octree_free_regions_recur(octree, child_id, tm_hash_get_ptr(&octree->nodes, child_id));
            }
        }
    } else {
        tm_carray_free(node->regions, octree->allocator);
    }
}

static void octree_destroy(mag_region_tree_t *octree)
{
    octree_free_regions_recur(octree, 1, &octree->root);
    tm_hash_free(&octree->nodes);
    tm_free(octree->allocator, octree, sizeof(*octree));
}

static void octree_query_recur(const mag_region_tree_t *octree, const aabb_t *aabb, tm_temp_allocator_i *ta, uint64_t **result, const mag_region_tree_node_t *node, const aabb_t *node_aabb, uint64_t node_id)
{
    if (!node->child_mask) {
        for (uint64_t i = 0; i < tm_carray_size(node->regions); ++i) {
            mag_tree_region_t *region = node->regions + i;
            aabb_t region_aabb = tree_region_aabb(region->pos, region->cell_size);
            if (aabb_intersect(aabb, &region_aabb)) {
                tm_carray_temp_push(*result, region->key, ta);
            }
        }
    } else {
        for (uint8_t i = 0; i < 8; ++i) {
            if (node->child_mask & ((uint8_t)1 << i)) {
                uint64_t child_id = (node_id << 3) | (uint64_t)i;
                // TODO: actual hash
                uint64_t child_hash = child_id;
                aabb_t caabb = child_aabb(i, node_aabb);
                if (aabb_intersect(aabb, &caabb)) {
                    mag_region_tree_node_t *child = tm_hash_get_ptr(&octree->nodes, child_hash);
                    octree_query_recur(octree, aabb, ta, result, child, &caabb, child_id);
                }
            }
        }
    }
}

static uint64_t *octree_query(const mag_region_tree_t *octree, tm_vec3_t min, tm_vec3_t max, tm_temp_allocator_i *ta)
{
    tm_vec3_t half_size = tm_vec3_mul(tm_vec3_sub(max, min), 0.5f);
    tm_vec3_t center = tm_vec3_add(min, half_size);
    aabb_t aabb = { center, half_size };

    aabb_t cur_aabb = octree->aabb;
    const mag_region_tree_node_t *cur_node = &octree->root;
    uint64_t cur_id = 1;
    uint64_t *result = NULL;
    if (aabb_intersect(&aabb, &cur_aabb)) {
        octree_query_recur(octree, &aabb, ta, &result, cur_node, &cur_aabb, cur_id);
    }
    return result;
}

static mag_region_tree_node_t *octree_split_node(mag_region_tree_t *octree, mag_region_tree_node_t *node, uint64_t node_id, const aabb_t *node_aabb)
{
    uint8_t child_mask = 0;
    mag_tree_region_t *child_regions[8] = { 0 };
    for (uint64_t i = 0; i < tm_carray_size(node->regions); ++i) {
        tm_vec3_t center = tree_region_aabb(node->regions[i].pos, node->regions[i].cell_size).center;
        uint32_t child_i = child_idx_for_point(node_aabb->center, center);
        tm_carray_push(child_regions[child_i], node->regions[i], octree->allocator);
        child_mask |= ((uint8_t)1 << child_i);
    }

    tm_carray_free(node->regions, octree->allocator);
    node->regions = NULL;
    node->child_mask = child_mask;

    for (uint32_t i = 0; i < 8; ++i) {
        if (child_regions[i]) {
            uint64_t child_id = (node_id << 3) | i;
            *tm_hash_add_reference(&octree->nodes, child_id) = (mag_region_tree_node_t) {
                .regions = child_regions[i]
            };
        }
    }

    return node_id == 1 ? node : tm_hash_get_ptr(&octree->nodes, node_id);
}

#define SPLIT_THRESHOLD 16
#define COLLAPSE_THRESHOLD 8

static void octree_insert(mag_region_tree_t *octree, tm_vec3_t region_pos, float cell_size, uint64_t key)
{
    // TODO: warn if region is outside octree boundaries

    aabb_t aabb = tree_region_aabb(region_pos, cell_size);

    mag_region_tree_node_t *node = &octree->root;
    aabb_t node_aabb = octree->aabb;
    uint64_t node_id = 1;
    if (key == 16) {
        node_id = 1;
    }
    if (key == 54) {
        node_id = 1;
    }
    while (true) {
        if (!node->child_mask) {
            uint64_t cur_size = tm_carray_size(node->regions);
            if (cur_size < SPLIT_THRESHOLD) {
                mag_tree_region_t region = { .pos = region_pos, .cell_size = cell_size, .key = key };
                tm_carray_push(node->regions, region, octree->allocator);
                break;
            } else {
                node = octree_split_node(octree, node, node_id, &node_aabb);
            }
        }

        uint32_t child_i = child_idx_for_point(node_aabb.center, aabb.center);
        node_aabb = child_aabb(child_i, &node_aabb);
        node_id = (node_id << 3) | child_i;

        if (node->child_mask & ((uint8_t)1 << child_i)) {
            node = tm_hash_get_ptr(&octree->nodes, node_id);
        } else {
            node->child_mask |= (uint8_t)1 << child_i;
            node = tm_hash_add_reference(&octree->nodes, node_id);
            *node = (mag_region_tree_node_t) { 0 };
        }
    }
}

static void octree_collapse_regions(mag_region_tree_t *octree, uint64_t node_id, mag_tree_region_t **regions)
{
    mag_region_tree_node_t *node = tm_hash_get_ptr(&octree->nodes, node_id);

    if (!node->child_mask) {
        tm_carray_push_array(*regions, node->regions, tm_carray_size(node->regions), octree->allocator);
        tm_carray_free(node->regions, octree->allocator);
    } else {
        uint8_t child_mask = node->child_mask;
        for (uint32_t i = 0; i < 8; ++i) {
            if (child_mask & ((uint8_t)1 << i)) {
                uint64_t child_id = (node_id << 3) | i;
                octree_collapse_regions(octree, child_id, regions);
            }
        }
    }

    tm_hash_remove(&octree->nodes, node_id);
}

static void count_regions_for_collapse(mag_region_tree_t *octree, uint64_t node_id, uint64_t *region_count)
{
    mag_region_tree_node_t *node = tm_hash_get_ptr(&octree->nodes, node_id);
    if (node->child_mask) {
        for (uint32_t i = 0; i < 8; ++i) {
            if (node->child_mask & ((uint8_t)1 << i)) {
                uint64_t child_id = (node_id << 3) | i;
                count_regions_for_collapse(octree, child_id, region_count);
                if (*region_count >= COLLAPSE_THRESHOLD)
                    break;
            }
        }
    } else {
        *region_count += tm_carray_size(node->regions);
    }
}

static bool octree_remove(mag_region_tree_t *octree, tm_vec3_t region_pos, float cell_size, uint64_t key)
{
    aabb_t aabb = tree_region_aabb(region_pos, cell_size);

    mag_region_tree_node_t *node = &octree->root;
    aabb_t node_aabb = octree->aabb;
    uint64_t node_id = 1;
    while (node->child_mask) {
        uint32_t child_i = child_idx_for_point(node_aabb.center, aabb.center);
        if (!(node->child_mask & ((uint8_t)1 << child_i))) {
            return false;
        }

        node_aabb = child_aabb(child_i, &node_aabb);
        node_id = (node_id << 3) | child_i;
        node = tm_hash_get_ptr(&octree->nodes, node_id);
    }

    uint64_t region_count = tm_carray_size(node->regions);
    bool found = false;
    for (uint64_t i = 0; i < region_count; ++i) {
        if (node->regions[i].key == key) {
            if (i < region_count - 1)
                node->regions[i] = node->regions[region_count - 1];
            tm_carray_shrink(node->regions, region_count - 1);
            found = true;
            region_count -= 1;
            break;
        }
    }

    if (!found) {
        return false;
    }

    if (!region_count && node_id != 1) {
        tm_carray_free(node->regions, octree->allocator);
        tm_hash_remove(&octree->nodes, node_id);
    }

    if (region_count < COLLAPSE_THRESHOLD && node_id != 1) {
        uint64_t parent_id = node_id >> 3;
        mag_region_tree_node_t *parent = parent_id == 1 ? &octree->root : tm_hash_get_ptr(&octree->nodes, parent_id);
        if (!region_count)
            parent->child_mask &= ~((uint8_t)1 << (node_id & 7));

        for (uint32_t i = 0; i < 8; ++i) {
            uint64_t child_id = (parent_id << 3) | i;
            if (child_id == node_id) {
                continue;
            }
            count_regions_for_collapse(octree, child_id, &region_count);
            if (region_count >= COLLAPSE_THRESHOLD) {
                return true;
            }
        }

        // collapse threshold reached, turn this node into leaf
        uint8_t child_mask = parent->child_mask;
        parent->child_mask = 0;
        tm_carray_ensure(parent->regions, region_count, octree->allocator);
        // parent pointer will be invalid below, so need to copy the region pointer
        mag_tree_region_t *regions = parent->regions;
        for (uint32_t i = 0; i < 8; ++i) {
            if (child_mask & ((uint8_t)1 << i)) {
                uint64_t child_id = (parent_id << 3) | i;
                octree_collapse_regions(octree, child_id, &regions);
            }
        }
    }

    return true;
}

static struct mag_region_tree_api tree_api = {
    .create = octree_create,
    .destroy = octree_destroy,
    .insert = octree_insert,
    .remove = octree_remove,
    .query = octree_query,
};

#define MAG_TEST_KEY_COUNT(tr, keys, expected) \
    TM_UNIT_TESTF((tr), tm_carray_size(keys) == (expected), "expected %llu keys, got %llu", expected, tm_carray_size(keys));

static void unit_test_tree_api(tm_unit_test_runner_i *tr, tm_allocator_i *a)
{
    TM_INIT_TEMP_ALLOCATOR(ta);

    mag_region_tree_t *tree = tree_api.create(a, (tm_vec3_t) { 0, 0, 0 }, (tm_vec3_t) { 10000, 10000, 10000 });

    const uint32_t insert_size = 10;
    for (uint32_t x = 0; x < insert_size; ++x) {
        for (uint32_t y = 0; y < insert_size; ++y) {
            for (uint32_t z = 0; z < insert_size; ++z) {
                tree_api.insert(tree, (tm_vec3_t) { (float)(32 * x), (float)(32 * y), (float)(32 * z) }, 1.f, x * insert_size * insert_size + y * insert_size + z);
            }
        }
    }

    {
        uint64_t *keys = tree_api.query(tree, (tm_vec3_t) { 0, 0, 0 }, (tm_vec3_t) { 5, 5, 5 }, ta);
        MAG_TEST_KEY_COUNT(tr, keys, 1)
        TM_UNIT_TEST(tr, keys[0] == 0);
    }

    {
        uint64_t *keys = tree_api.query(tree, (tm_vec3_t) { 0, 0, 0 }, (tm_vec3_t) { 33, 33, 33 }, ta);
        MAG_TEST_KEY_COUNT(tr, keys, 8);
    }

    {
        tree_api.remove(tree, (tm_vec3_t) { 0, 0, 0 }, 1.f, 0);
        uint64_t *keys = tree_api.query(tree, (tm_vec3_t) { 0, 0, 0 }, (tm_vec3_t) { 5, 5, 5 }, ta);
        MAG_TEST_KEY_COUNT(tr, keys, 0);
    }

    for (uint32_t x = 0; x < insert_size; ++x) {
        for (uint32_t y = 0; y < insert_size; ++y) {
            for (uint32_t z = 0; z < insert_size; ++z) {
                tree_api.remove(tree, (tm_vec3_t) { (float)(32 * x), (float)(32 * y), (float)(32 * z) }, 1.f, x * insert_size * insert_size + y * insert_size + z);
            }
        }
    }

    {
        uint64_t *keys = tree_api.query(tree, (tm_vec3_t) { 0, 0, 0 }, (tm_vec3_t) { 10000, 10000, 10000 }, ta);
        MAG_TEST_KEY_COUNT(tr, keys, 0)
    }
    TM_UNIT_TEST(tr, !tree->root.child_mask);

    tree_api.destroy(tree);
    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static tm_unit_test_i *mag_region_tree_api_tests = &(tm_unit_test_i) {
    .name = "mag_region_tree_api",
    .test = unit_test_tree_api
};

TM_DLL_EXPORT void tm_load_plugin(struct tm_api_registry_api *reg, bool load)
{
    reg->begin_context("mag_voxel");

    tm_shader_api = tm_get_api(reg, tm_shader_api);
    tm_renderer_api = tm_get_api(reg, tm_renderer_api);
    tm_temp_allocator_api = tm_get_api(reg, tm_temp_allocator_api);
    tm_error_api = tm_get_api(reg, tm_error_api);

    tm_set_or_remove_api(reg, load, mag_voxel_api, &mag_voxel_api);
    tm_set_or_remove_api(reg, load, mag_region_tree_api, &tree_api);

    tm_add_or_remove_implementation(reg, load, tm_unit_test_i, mag_region_tree_api_tests);

    reg->end_context("mag_voxel");
}