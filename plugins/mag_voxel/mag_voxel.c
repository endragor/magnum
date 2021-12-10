#include "mag_voxel.h"

#include <foundation/api_registry.h>
#include <foundation/api_type_hashes.h>
#include <foundation/carray.inl>
#include <foundation/math.inl>
#include <foundation/murmurhash64a.inl>

#include <plugins/renderer/commands.h>
#include <plugins/renderer/render_backend.h>
#include <plugins/renderer/render_command_buffer.h>
#include <plugins/renderer/renderer.h>
#include <plugins/renderer/resources.h>
#include <plugins/shader_system/shader_system.h>

static struct tm_shader_api *tm_shader_api;
static struct tm_renderer_api *tm_renderer_api;
static struct tm_temp_allocator_api *tm_temp_allocator_api;

#define ADAPT(v0, v1) (-v0 / (v1 - v0))

static tm_vec3_t find_vertex(float x, float y, float z, tm_vec3_t *p, tm_vec3_t *n, int count)
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
            f[e] = tm_vec3_add(f[e], tm_vec3_mul(n[i], -tm_vec3_dot(edges[e], n[i]) - d));
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

        if (tm_vec3_dot(f_total, f_total) * 0.0025f < 0.00001f) {
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
                normals[change_idx] = tm_vec3_lerp(n[x + dx][y + dy][z], n[x + dx][y + dy][z + 1], distance);
                ++change_idx;
            }
        }
    }

    for (int dx = 0; dx != 2; ++dx) {
        for (int dz = 0; dz != 2; ++dz) {
            if ((v[x + dx][y][z + dz] > 0) != (v[x + dx][y + 1][z + dz] > 0)) {
                float distance = ADAPT(v[x + dx][y][z + dz], v[x + dx][y + 1][z + dz]);
                changes[change_idx] = (tm_vec3_t) { .x = (float)(x + dx), .y = (float)y + distance, .z = (float)(z + dz) };
                normals[change_idx] = tm_vec3_lerp(n[x + dx][y][z + dz], n[x + dx][y + 1][z + dz], distance);
                ++change_idx;
            }
        }
    }

    for (int dy = 0; dy != 2; ++dy) {
        for (int dz = 0; dz != 2; ++dz) {
            if ((v[x][y + dy][z + dz] > 0) != (v[x + 1][y + dy][z + dz] > 0)) {
                float distance = ADAPT(v[x][y + dy][z + dz], v[x + 1][y + dy][z + dz]);
                changes[change_idx] = (tm_vec3_t) { .x = (float)x + distance, .y = (float)(y + dy), .z = (float)(z + dz) };
                normals[change_idx] = tm_vec3_lerp(n[x][y + dy][z + dz], n[x + 1][y + dy][z + dz], distance);
                ++change_idx;
            }
        }
    }

    if (change_idx <= 1)
        return false;

    *vertex = find_vertex((float)x, (float)y, (float)z, changes, normals, change_idx);
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

static struct mag_voxel_api api = {
    .dual_contour_region = dual_contour_region,
};

TM_DLL_EXPORT void tm_load_plugin(struct tm_api_registry_api *reg, bool load)
{
    reg->begin_context("mag_voxel");

    tm_shader_api = tm_get_api(reg, tm_shader_api);
    tm_renderer_api = tm_get_api(reg, tm_renderer_api);
    tm_temp_allocator_api = tm_get_api(reg, tm_temp_allocator_api);

    tm_set_or_remove_api(reg, load, mag_voxel_api, &api);

    reg->end_context("mag_voxel");
}