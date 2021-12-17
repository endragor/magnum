static struct tm_api_registry_api *tm_global_api_registry;
static struct tm_entity_api *tm_entity_api;
static struct tm_error_api *tm_error_api;
static struct tm_profiler_api *tm_profiler_api;
static struct tm_localizer_api *tm_localizer_api;
static struct tm_properties_view_api *tm_properties_view_api;
static struct tm_renderer_api *tm_renderer_api;
static struct tm_render_graph_api *tm_render_graph_api;
static struct tm_shader_api *tm_shader_api;
static struct tm_shader_repository_api *tm_shader_repository_api;
static struct tm_shader_system_api *tm_shader_system_api;
static struct tm_temp_allocator_api *tm_temp_allocator_api;
static struct tm_the_truth_api *tm_the_truth_api;
static struct tm_the_truth_common_types_api *tm_the_truth_common_types_api;
static struct tm_logger_api *tm_logger_api;
static struct tm_visibility_flags_api *tm_visibility_flags_api;
static struct tm_entity_commands_api *tm_entity_commands_api;

static struct mag_voxel_api *mag_voxel_api;

#include "mag_terrain_component.h"

#include <foundation/api_registry.h>
#include <foundation/bounding_volume.h>
#include <foundation/buffer_format.h>
#include <foundation/camera.h>
#include <foundation/carray.inl>
#include <foundation/error.h>
#include <foundation/hash.inl>
#include <foundation/input.h>
#include <foundation/localizer.h>
#include <foundation/log.h>
#include <foundation/math.inl>
#include <foundation/murmurhash64a.inl>
#include <foundation/profiler.h>
#include <foundation/the_truth.h>
#include <foundation/the_truth_types.h>
#include <foundation/unit_test.h>
#include <foundation/visibility_flags.h>

#include <plugins/editor_views/properties.h>
#include <plugins/entity/entity.h>
#include <plugins/entity/transform_component.h>
#include <plugins/render_graph/render_graph.h>
#include <plugins/render_graph_toolbox/render_pipeline.h>
#include <plugins/renderer/commands.h>
#include <plugins/renderer/render_backend.h>
#include <plugins/renderer/render_command_buffer.h>
#include <plugins/renderer/renderer.h>
#include <plugins/renderer/resources.h>
#include <plugins/shader_system/shader_system.h>
#include <plugins/the_machinery_shared/component_interfaces/editor_ui_interface.h>
#include <plugins/the_machinery_shared/component_interfaces/render_interface.h>
#include <plugins/the_machinery_shared/frustum_culling.h>
#include <plugins/the_machinery_shared/render_context.h>

#include "plugins/mag_voxel/mag_voxel.h"

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

typedef struct op_t
{
    // TODO: use a more compact representation (half floats for size?)
    uint8_t type;
    uint8_t primitive;
    tm_vec3_t pos;
    tm_vec3_t size;
} op_t;

typedef struct region_data_t
{
    tm_vec3_t pos;
    float cell_size;
    uint64_t key;
} region_data_t;

typedef struct mag_terrain_component_t
{
    // Visibility mask built from VISIBILITY_FLAGS in The Truth.
    uint64_t visibility_mask;
    tm_vec3_t color_rgb;

    region_data_t region_data;

    mag_voxel_mesh_t mesh;
    tm_renderer_handle_t densities_handle;
    tm_renderer_handle_t normals_handle;
    tm_renderer_handle_t region_indirect;
    tm_renderer_handle_t nbuf;

    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuffer;

} mag_terrain_component_t;

static int64_t floor_to_f(float val, int64_t size)
{
    int64_t floored = (int64_t)floorf(val);
    int64_t modulo = floored % size;
    return modulo < 0 ? floored - size - modulo : floored - modulo;
}

static int64_t ceil_to_f(float val, int64_t size)
{
    int64_t ceiled = (int64_t)ceilf(val);
    int64_t modulo = ceiled % size;
    return modulo <= 0 ? ceiled - modulo : ceiled + size - modulo;
}

static tm_vec3_t floor_to(tm_vec3_t v, int64_t size)
{
    return (tm_vec3_t) { (float)floor_to_f(v.x, size), (float)floor_to_f(v.y, size), (float)floor_to_f(v.z, size) };
}

static tm_vec3_t ceil_to(tm_vec3_t v, int64_t size)
{
    return (tm_vec3_t) { (float)ceil_to_f(v.x, size), (float)ceil_to_f(v.y, size), (float)ceil_to_f(v.z, size) };
}

typedef struct aabb_t
{
    tm_vec3_t min;
    tm_vec3_t max;
} aabb_t;

static bool set_constant(tm_shader_io_o *io, tm_renderer_resource_command_buffer_o *res_buf, const tm_shader_constant_buffer_instance_t *instance, const char *name, const void *data, uint32_t data_size)
{
    tm_shader_constant_t constant;
    uint32_t constant_offset;

    if (tm_shader_api->lookup_constant(io, tm_murmur_hash_string(name), &constant, &constant_offset)) {
        tm_shader_api->update_constants(io, res_buf, &(tm_shader_constant_update_t) { .instance_id = instance->instance_id, .constant_offset = constant_offset, .num_bytes = data_size, .data = data }, 1);
        return true;
    }

    return false;
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

#define NATURAL_MAP(x) ((uint64_t)((x) < 0 ? (-(x)-1) * 2 + 1 : (x)*2))
#define NATURAL_PAIR_MAP(x, y) (((x) + (y)) * ((x) + (y) + 1) / 2 + (y))

static const float LOD_DISTANCES[] = {
    64.f,
    128.f,
    256.f,
    512.f,
};

static uint64_t region_key(tm_vec3_t start, float lod_size, uint64_t lod_i)
{
    uint64_t ux = NATURAL_MAP((int64_t)(start.x / lod_size));
    uint64_t uy = NATURAL_MAP((int64_t)(start.y / lod_size));
    uint64_t uz = NATURAL_MAP((int64_t)(start.z / lod_size));

    return ((NATURAL_PAIR_MAP(NATURAL_PAIR_MAP(ux, uy), uz) << 3) | lod_i) + 1;
}

static void add_regions_from_aabb(region_data_t **result, const aabb_t *aabb, uint64_t lod_i, tm_temp_allocator_i *ta)
{
    float cell_size = 1.f;
    for (uint64_t i = 0; i < lod_i; ++i) {
        cell_size *= 2.f;
    }

    float lod_size = (float)MAG_VOXEL_CHUNK_SIZE * cell_size;
    for (float x = aabb->min.x; x < aabb->max.x; x += lod_size) {
        for (float y = aabb->min.y; y < aabb->max.y; y += lod_size) {
            for (float z = aabb->min.z; z < aabb->max.z; z += lod_size) {
                region_data_t region = {
                    .pos = { x, y, z },
                    .cell_size = cell_size,
                    .key = region_key((tm_vec3_t) { x, y, z }, lod_size, lod_i),
                };
                tm_carray_temp_push(*result, region, ta);
            }
        }
    }
}

static region_data_t *wanted_regions(tm_vec3_t camera_pos, tm_temp_allocator_i *ta)
{

    tm_vec3_t prev_min = { 0 };
    tm_vec3_t prev_max = { 0 };
    float cell_size = 1.f;
    int64_t icell_size = 1;

    region_data_t *result = NULL;
    for (uint64_t i = 0; i < TM_ARRAY_COUNT(LOD_DISTANCES); ++i) {
        tm_vec3_t lod_distance = { LOD_DISTANCES[i], LOD_DISTANCES[i], LOD_DISTANCES[i] };
        int64_t ilod_size = MAG_VOXEL_CHUNK_SIZE * icell_size;
        tm_vec3_t lod_min = floor_to(tm_vec3_sub(camera_pos, lod_distance), ilod_size * 2);
        tm_vec3_t lod_max = ceil_to(tm_vec3_add(camera_pos, lod_distance), ilod_size * 2);

        aabb_t partial_aabbs[6];
        uint32_t aabb_count = 6;
        if (i == 0) {
            partial_aabbs[0] = (aabb_t) { lod_min, lod_max };
            aabb_count = 1;
        } else {
            // z-slices
            partial_aabbs[0] = (aabb_t) { lod_min, { lod_max.x, prev_min.y, lod_max.z } };
            partial_aabbs[1] = (aabb_t) { { lod_min.x, prev_max.y, lod_min.z }, lod_max };

            // y-slices
            partial_aabbs[2] = (aabb_t) { { lod_min.x, prev_min.y, lod_min.z }, { lod_max.x, prev_max.y, prev_min.z } };
            partial_aabbs[3] = (aabb_t) { { lod_min.x, prev_min.y, prev_max.z }, { lod_max.x, prev_max.y, lod_max.z } };

            // x-slices
            partial_aabbs[4] = (aabb_t) { { lod_min.x, prev_min.y, prev_min.z }, { prev_min.x, prev_max.y, prev_max.z } };
            partial_aabbs[5] = (aabb_t) { { prev_max.x, prev_min.y, prev_min.z }, { lod_max.x, prev_max.y, prev_max.z } };
        }

        for (uint32_t ai = 0; ai < aabb_count; ++ai) {
            add_regions_from_aabb(&result, partial_aabbs + ai, i, ta);
        }

        prev_min = lod_min;
        prev_max = lod_max;
        cell_size *= 2.f;
        icell_size *= 2;
    }

    return result;
}

static mag_terrain_component_t default_values = {
    .color_rgb = { 0.8f, 0.8f, 0.8f },
};

typedef struct mag_terrain_component_manager_o
{
    tm_entity_context_o *ctx;
    tm_allocator_i allocator;

    tm_renderer_backend_i *backend;
    tm_shader_repository_o *shader_repo;

    tm_component_mask_t component_mask;

    struct TM_HASH_T(uint64_t, mag_terrain_component_t *) component_map;

    // TODO: optimize to avoid copying on growth
    /* carray */ op_t *ops;
    uint64_t applied_op_count;

} mag_terrain_component_manager_o;

static float properties_ui(struct tm_properties_ui_args_t *args, tm_rect_t item_rect, tm_tt_id_t object);

static tm_ci_editor_ui_i *editor_aspect;
static tm_ci_render_i *render_aspect;
static tm_properties_aspect_i *properties_aspect;

static void create_truth_types(struct tm_the_truth_o *tt)
{
    static const tm_the_truth_property_definition_t voxel_terrain_component_properties[] = {
        { "visibility_flags", TM_THE_TRUTH_PROPERTY_TYPE_SUBOBJECT_SET, .type_hash = TM_TT_TYPE_HASH__VISIBILITY_FLAG },
        { "color", TM_THE_TRUTH_PROPERTY_TYPE_SUBOBJECT, .type_hash = TM_TT_TYPE_HASH__COLOR_RGB },
    };

    const tm_tt_type_t object_type = tm_the_truth_api->create_object_type(tt, MAG_TT_TYPE__TERRAIN_COMPONENT, voxel_terrain_component_properties, TM_ARRAY_COUNT(voxel_terrain_component_properties));

    const tm_tt_id_t component = tm_the_truth_api->create_object_of_type(tt, object_type, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_object_o *component_w = tm_the_truth_api->write(tt, component);
    tm_the_truth_common_types_api->set_color_rgb(tt, component_w, MAG_TT_PROP__TERRAIN_COMPONENT__COLOR, default_values.color_rgb, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_api->commit(tt, component_w, TM_TT_NO_UNDO_SCOPE);

    tm_the_truth_api->set_default_object(tt, object_type, component);

    tm_the_truth_api->set_aspect(tt, object_type, TM_CI_EDITOR_UI, editor_aspect);
    tm_the_truth_api->set_aspect(tt, object_type, TM_CI_RENDER, render_aspect);
    tm_the_truth_api->set_aspect(tt, object_type, TM_TT_ASPECT__PROPERTIES, properties_aspect);
}

static bool load_asset(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data, const tm_the_truth_o *tt, tm_tt_id_t asset)
{
    mag_terrain_component_t *c = data;
    const tm_the_truth_object_o *asset_obj = tm_tt_read(tt, asset);

    c->color_rgb = tm_the_truth_common_types_api->get_color_rgb(tt, asset_obj, MAG_TT_PROP__TERRAIN_COMPONENT__COLOR);

    TM_INIT_TEMP_ALLOCATOR(ta);

    const tm_tt_id_t *visibility_flags = tm_the_truth_api->get_subobject_set(tt, asset_obj, MAG_TT_PROP__TERRAIN_COMPONENT__VISIBILITY_FLAGS, ta);
    uint32_t n_visibility_flags = (uint32_t)tm_carray_size(visibility_flags);
    uint32_t *uuid = tm_temp_alloc(ta, n_visibility_flags * sizeof(uint32_t));
    for (uint32_t i = 0; i != n_visibility_flags; ++i)
        uuid[i] = tm_the_truth_api->get_uint32_t(tt, tm_tt_read(tt, visibility_flags[i]), TM_TT_PROP__VISIBILITY_FLAG__UUID);

    tm_visibility_context_o *context = tm_single_implementation(tm_global_api_registry, tm_visibility_context_o);
    c->visibility_mask = tm_visibility_flags_api->build_visibility_mask(context, uuid, n_visibility_flags);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
    return true;
}

static aabb_t adjust_aabb_to_lod(const aabb_t *aabb, int64_t lod_size)
{
    return (aabb_t) { floor_to(aabb->min, lod_size), ceil_to(aabb->max, lod_size) };
}

static aabb_t op_aabb(const op_t *op)
{
    tm_vec3_t half_size = tm_vec3_mul(op->size, 0.5f);
    return (aabb_t) { tm_vec3_sub(op->pos, half_size), tm_vec3_add(op->pos, half_size) };
}

static void apply_op_to_component(tm_renderer_backend_i *rb, tm_shader_repository_o *shader_repo, const mag_terrain_component_t *c, const op_t *op, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    if (!c->densities_handle.resource)
        return;

    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    // TODO: reuse these buffers

    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuf;

    tm_shader_o *shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_terrain_operations", 0x4e0fc94d36751938ULL));
    tm_shader_io_o *io = tm_shader_api->shader_io(shader);

    tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
    tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);

    uint32_t op_type = op->type;
    uint32_t op_primitive = op->primitive;

    set_constant(io, res_buf, &cbuf, "region_pos", &c->region_data.pos, sizeof(c->region_data.pos));
    set_constant(io, res_buf, &cbuf, "cell_size", &c->region_data.cell_size, sizeof(c->region_data.cell_size));
    set_constant(io, res_buf, &cbuf, "operation", &op_type, sizeof(op_type));
    set_constant(io, res_buf, &cbuf, "primitive_type", &op_primitive, sizeof(op_primitive));
    set_constant(io, res_buf, &cbuf, "primitive_position", &op->pos, sizeof(op->pos));
    set_constant(io, res_buf, &cbuf, "primitive_size", &op->size, sizeof(op->size));

    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("densities", 0x9d97839d5465b483ULL), &c->densities_handle, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("normals", 0x80b4d6fd3ed93beULL), &c->normals_handle, 0, 0, 1);

    tm_renderer_shader_info_t shader_info;
    tm_shader_system_context_o *shader_context = tm_shader_system_api->create_context(a, NULL);
    if (tm_shader_api->assemble_shader_infos(shader, 0, 0, shader_context, TM_STRHASH(0), res_buf, &cbuf, &rbinder, 1, &shader_info)) {
        tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { 1, MAG_VOXEL_REGION_SIZE, MAG_VOXEL_REGION_SIZE } }, &shader_info, 1);
        ++*sort_key;
    }

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->densities_handle, .source_state = state, .destination_state = state }, 1);
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->normals_handle, .source_state = state, .destination_state = state }, 1);
    ++*sort_key;

    tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);
    tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void generate_mesh(tm_renderer_backend_i *rb, tm_shader_repository_o *shader_repo, const mag_terrain_component_t *c, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    tm_renderer_draw_indexed_command_t *draw_command;
    tm_renderer_api->tm_renderer_resource_command_buffer_api->update_buffer(res_buf,
        c->region_indirect, 0, sizeof(*draw_command),
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, 0, &draw_command);
    *draw_command = (tm_renderer_draw_indexed_command_t) { .num_instances = 1 };

    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuf;

    tm_shader_o *shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_terrain_gen_region_with_mesh", 0xc427e87002185b98ULL));
    tm_shader_io_o *io = tm_shader_api->shader_io(shader);

    tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
    tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);

    set_constant(io, res_buf, &cbuf, "region_pos", &c->region_data.pos, sizeof(c->region_data.pos));
    set_constant(io, res_buf, &cbuf, "cell_size", &c->region_data.cell_size, sizeof(c->region_data.cell_size));
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("densities", 0x9d97839d5465b483ULL), &c->densities_handle, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("normals", 0x80b4d6fd3ed93beULL), &c->normals_handle, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("region_indirect", 0xe7e22c1a4906be7fULL), &c->region_indirect, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("vertices", 0x3288dd4327525f9aULL), &c->mesh.vbuf, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("vertex_normals", 0x45b9b61b4d82ac56ULL), &c->nbuf, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("triangles", 0x72976bf8d13d4449ULL), &c->mesh.ibuf, 0, 0, 1);

    tm_renderer_shader_info_t shader_info;
    tm_shader_system_context_o *shader_context = tm_shader_system_api->create_context(a, NULL);
    if (tm_shader_api->assemble_shader_infos(shader, 0, 0, shader_context, TM_STRHASH(0), res_buf, &cbuf, &rbinder, 1, &shader_info)) {
        tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { 1, MAG_VOXEL_REGION_SIZE - 1, MAG_VOXEL_REGION_SIZE - 1 } }, &shader_info, 1);
        *sort_key += 1;
    }

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->mesh.vbuf, .source_state = state, .destination_state = state }, 1);
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->region_indirect, .source_state = state, .destination_state = TM_RENDERER_RESOURCE_STATE_INDIRECT_ARGUMENT }, 1);
    *sort_key += 1;

    tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);
    tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void apply_op(mag_terrain_component_manager_o *man, const op_t *op, tm_engine_update_set_t *data)
{
    TM_INIT_TEMP_ALLOCATOR(ta);

    aabb_t aabb = op_aabb(op);
    region_data_t *affected_regions = NULL;

    int64_t lod_size = MAG_VOXEL_CHUNK_SIZE;
    for (uint64_t ilod = 0; ilod < TM_ARRAY_COUNT(LOD_DISTANCES); ++ilod) {
        aabb_t lod_aabb = adjust_aabb_to_lod(&aabb, lod_size);
        lod_size *= 2;
        add_regions_from_aabb(&affected_regions, &lod_aabb, ilod, ta);
    }

    tm_renderer_command_buffer_o *cmd_buf;
    man->backend->create_command_buffers(man->backend->inst, &cmd_buf, 1);
    tm_renderer_resource_command_buffer_o *res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);

    for (uint64_t ri = 0; ri < tm_carray_size(affected_regions); ++ri) {
        uint64_t key = affected_regions[ri].key;
        mag_terrain_component_t *c = tm_hash_get(&man->component_map, key);
        if (c) {
            uint64_t sort_key = 0;
            apply_op_to_component(man->backend, man->shader_repo, c, op, cmd_buf, res_buf, &sort_key);
            generate_mesh(man->backend, man->shader_repo, c, cmd_buf, res_buf, &sort_key);
        }
    }

    man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->submit_command_buffers(man->backend->inst, &cmd_buf, 1);
    man->backend->destroy_command_buffers(man->backend->inst, &cmd_buf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void add(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data)
{
    mag_terrain_component_t *c = data;

    tm_visibility_context_o *context = tm_single_implementation(tm_global_api_registry, tm_visibility_context_o);
    c->visibility_mask = tm_visibility_flags_api->build_visibility_mask(context, 0, 0);
}

static void destroy_mesh(mag_terrain_component_t *c, mag_terrain_component_manager_o *man)
{
    if (c->mesh.vbuf.resource) {
        tm_renderer_resource_command_buffer_o *res_buf;
        man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);

        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->mesh.ibuf);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->mesh.vbuf);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->densities_handle);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->normals_handle);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->region_indirect);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->nbuf);

        man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
        man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);

        c->mesh.vbuf = (tm_renderer_handle_t) { 0 };
        c->mesh.ibuf = (tm_renderer_handle_t) { 0 };
        c->densities_handle = (tm_renderer_handle_t) { 0 };
        c->normals_handle = (tm_renderer_handle_t) { 0 };
        c->region_indirect = (tm_renderer_handle_t) { 0 };
        c->nbuf = (tm_renderer_handle_t) { 0 };
    }
}

static void remove(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data)
{
    mag_terrain_component_t *c = data;
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;

    tm_shader_system_o *vertex_buffer_system = tm_shader_repository_api->lookup_system(man->shader_repo, tm_murmur_hash_string("vertex_buffer_system"));
    tm_shader_io_o *io = tm_shader_api->system_io(vertex_buffer_system);

    if (c->rbinder.instance_id)
        tm_shader_api->destroy_resource_binder_instances(io, &c->rbinder, 1);

    if (c->cbuffer.instance_id)
        tm_shader_api->destroy_constant_buffer_instances(io, &c->cbuffer, 1);

    destroy_mesh(c, man);
}

static void destroy(tm_component_manager_o *manager)
{
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;

    const tm_component_type_t terrain_component = tm_entity_api->lookup_component_type(man->ctx, MAG_TT_TYPE_HASH__TERRAIN_COMPONENT);
    tm_entity_api->call_remove_on_all_entities(man->ctx, terrain_component);

    tm_carray_free(man->ops, &man->allocator);
    tm_hash_free(&man->component_map);

    tm_entity_context_o *ctx = man->ctx;
    tm_allocator_i a = man->allocator;
    tm_free(&a, man, sizeof(*man));
    tm_entity_api->destroy_child_allocator(ctx, &a);
}

static void create_mag_terrain_component(tm_entity_context_o *ctx)
{
    tm_renderer_backend_i *backend = tm_first_implementation(tm_global_api_registry, tm_renderer_backend_i);
    tm_shader_repository_o *shader_repo = tm_first_implementation(tm_global_api_registry, tm_shader_repository_o);

    if (!backend || !shader_repo)
        return;

    tm_allocator_i a;
    tm_entity_api->create_child_allocator(ctx, MAG_TT_TYPE__TERRAIN_COMPONENT, &a);
    mag_terrain_component_manager_o *manager = tm_alloc(&a, sizeof(*manager));
    *manager = (mag_terrain_component_manager_o) {
        .ctx = ctx,
        .allocator = a,
        .backend = backend,
        .shader_repo = shader_repo,
    };
    manager->component_map.allocator = &manager->allocator;

    const tm_component_i component = {
        .name = MAG_TT_TYPE__TERRAIN_COMPONENT,
        .bytes = sizeof(mag_terrain_component_t),
        .default_data = &default_values,
        .manager = (tm_component_manager_o *)manager,
        .load_asset = load_asset,
        .add = add,
        .remove = remove,
        .destroy = destroy
    };

    tm_entity_api->register_component(ctx, &component);
}

static float properties_ui(struct tm_properties_ui_args_t *args, tm_rect_t item_rect, tm_tt_id_t object)
{
    const tm_the_truth_object_o *obj = tm_tt_read(args->tt, object);

    const tm_tt_id_t color = tm_the_truth_api->get_subobject(args->tt, obj, MAG_TT_PROP__TERRAIN_COMPONENT__COLOR);
    item_rect.y = tm_properties_view_api->ui_color_picker(args, item_rect, TM_LOCALIZE("Color"), NULL, color);

    item_rect.y = tm_properties_view_api->ui_visibility_flags(args, item_rect, TM_LOCALIZE("Visibility Flags"), NULL, object, MAG_TT_PROP__TERRAIN_COMPONENT__VISIBILITY_FLAGS);

    return item_rect.y;
}

static const char *get_type_display_name(void)
{
    return TM_LOCALIZE("Voxel Terrain Component");
}

static void generate_sdf(tm_renderer_backend_i *rb, tm_shader_repository_o *shader_repo, const mag_terrain_component_t *c, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    tm_shader_o *sdf_shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_terrain_gen_region", 0x3f8b44db04e9fd19ULL));
    tm_shader_io_o *io = tm_shader_api->shader_io(sdf_shader);
    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuf;
    tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
    tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);
    set_constant(io, res_buf, &cbuf, "region_pos", &c->region_data.pos, sizeof(c->region_data.pos));
    set_constant(io, res_buf, &cbuf, "cell_size", &c->region_data.cell_size, sizeof(c->region_data.cell_size));
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("densities", 0x9d97839d5465b483ULL), &c->densities_handle, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("normals", 0x80b4d6fd3ed93beULL), &c->normals_handle, 0, 0, 1);

    tm_renderer_shader_info_t shader_info;
    tm_shader_system_context_o *shader_context = tm_shader_system_api->create_context(a, NULL);
    if (tm_shader_api->assemble_shader_infos(sdf_shader, 0, 0, shader_context, TM_STRHASH(0), res_buf, &cbuf, &rbinder, 1, &shader_info)) {
        tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { 1, MAG_VOXEL_REGION_SIZE, MAG_VOXEL_REGION_SIZE } }, &shader_info, 1);
        *sort_key += 1;
    }

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->densities_handle, .source_state = state, .destination_state = state }, 1);
    *sort_key += 1;
    tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);
    tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void bind_vertex_system_resources(tm_shader_io_o *io, mag_terrain_component_t *c, tm_renderer_resource_command_buffer_o *res_buf)
{
    if (!c->rbinder.instance_id)
        tm_shader_api->create_resource_binder_instances(io, 1, &c->rbinder);
    if (!c->cbuffer.instance_id)
        tm_shader_api->create_constant_buffer_instances(io, 1, &c->cbuffer);

    set_resource(io, res_buf, &c->rbinder, TM_STATIC_HASH("vertex_buffer_position_buffer", 0x1ef08bede3820d69ULL), &c->mesh.vbuf, 0, 0, 1);
    set_resource(io, res_buf, &c->rbinder, TM_STATIC_HASH("vertex_buffer_normal_buffer", 0x781ed2624b12ebbcULL), &c->nbuf, 0, 0, 1);
    set_resource(io, res_buf, &c->rbinder, TM_STATIC_HASH("index_buffer", 0xb773460d24bcec1fULL), &c->mesh.ibuf, 0, 0, 1);

#include <the_machinery/shaders/vertex_buffer_system.inl>
    tm_shader_vertex_buffer_system_t constants = { 0 };
    uint32_t *strides = (uint32_t *)&constants.vertex_buffer_strides;
    uint32_t *offsets = (uint32_t *)&constants.vertex_buffer_offsets;

    constants.vertex_buffer_header[0] |= (1 << TM_VERTEX_SEMANTIC_POSITION) | (1 << TM_INDEX_SEMANTIC) | (1 << TM_VERTEX_SEMANTIC_NORMAL);
    const uint32_t num_vertices = MAG_VOXEL_REGION_SIZE * MAG_VOXEL_REGION_SIZE * MAG_VOXEL_REGION_SIZE;
    constants.vertex_buffer_header[1] = num_vertices;
    offsets[TM_VERTEX_SEMANTIC_POSITION] = 0;
    strides[TM_VERTEX_SEMANTIC_POSITION] = sizeof(tm_vec3_t);
    offsets[TM_VERTEX_SEMANTIC_NORMAL] = 0;
    strides[TM_VERTEX_SEMANTIC_NORMAL] = sizeof(tm_vec3_t);

    constants.index_buffer_offset_and_stride[0] = 0;
    constants.index_buffer_offset_and_stride[1] = 2;

    void *cbuf = (void *)&constants;
    tm_shader_api->update_constants_raw(io, res_buf,
        &c->cbuffer.instance_id, (const void **)&cbuf, 0, sizeof(tm_shader_vertex_buffer_system_t), 1);
}

static void generate_region_gpu(tm_renderer_backend_i *rb, tm_shader_repository_o *shader_repo, tm_shader_io_o *vertex_system_io, mag_terrain_component_t *c, const op_t *ops)
{
    tm_renderer_command_buffer_o *cmd_buf;
    rb->create_command_buffers(rb->inst, &cmd_buf, 1);
    tm_renderer_resource_command_buffer_o *res_buf;
    rb->create_resource_command_buffers(rb->inst, &res_buf, 1);

    const uint32_t REGION_SIZE = MAG_VOXEL_REGION_SIZE * MAG_VOXEL_REGION_SIZE * MAG_VOXEL_REGION_SIZE;

    if (!c->densities_handle.resource) {
        c->densities_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
            &(tm_renderer_buffer_desc_t) { .size = sizeof(float) * REGION_SIZE, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_densities" },
            TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
        c->normals_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
            &(tm_renderer_buffer_desc_t) { .size = sizeof(tm_vec3_t) * REGION_SIZE, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_normals" },
            TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
        c->region_indirect = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
            &(tm_renderer_buffer_desc_t) { .size = sizeof(tm_renderer_draw_indexed_command_t), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_INDIRECT | TM_RENDERER_BUFFER_USAGE_UPDATABLE, .debug_tag = "mag_region_indirect" },
            TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
        c->mesh.vbuf = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
            &(tm_renderer_buffer_desc_t) { .size = sizeof(tm_vec3_t) * REGION_SIZE, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_ACCELERATION_STRUCTURE, .debug_tag = "mag_region_vertices" },
            TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
        c->nbuf = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
            &(tm_renderer_buffer_desc_t) { .size = sizeof(tm_vec3_t) * REGION_SIZE, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_ACCELERATION_STRUCTURE, .debug_tag = "mag_region_vertex_normals" },
            TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
        c->mesh.ibuf = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
            &(tm_renderer_buffer_desc_t) { .size = sizeof(uint16_t) * 6 * 3 * REGION_SIZE, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_ACCELERATION_STRUCTURE | TM_RENDERER_BUFFER_USAGE_INDEX, .debug_tag = "mag_region_triangles" },
            TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);

        bind_vertex_system_resources(vertex_system_io, c, res_buf);
    }

    uint64_t sort_key = 0;

    generate_sdf(rb, shader_repo, c, cmd_buf, res_buf, &sort_key);
    for (uint64_t i = 0; i < tm_carray_size(ops); ++i) {
        apply_op_to_component(rb, shader_repo, c, ops + i, cmd_buf, res_buf, &sort_key);
    }
    generate_mesh(rb, shader_repo, c, cmd_buf, res_buf, &sort_key);

    rb->submit_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->destroy_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->submit_command_buffers(rb->inst, &cmd_buf, 1);
    rb->destroy_command_buffers(rb->inst, &cmd_buf, 1);

    // TODO: destroy/shrink unneeded buffers
}

static void dual_contour_cpu(mag_terrain_component_t *c, mag_terrain_component_manager_o *man, tm_shader_io_o *io, tm_renderer_backend_i *rb)
{
    mag_voxel_region_t region;

    tm_renderer_command_buffer_o *cmd_buf;
    rb->create_command_buffers(rb->inst, &cmd_buf, 1);
    uint64_t sort_key = UINT64_MAX;
    tm_renderer_api->tm_renderer_command_buffer_api->bind_queue(cmd_buf, sort_key, &(tm_renderer_queue_bind_t) { .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL });
    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    uint32_t densities_rr = tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, sort_key, &(tm_renderer_read_buffer_t) { .resource_handle = c->densities_handle, .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, .resource_state = state, .resource_queue = TM_RENDERER_QUEUE_GRAPHICS, .bits = region.densities, .size = sizeof(region.densities) });
    uint32_t normals_rr = tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, sort_key, &(tm_renderer_read_buffer_t) { .resource_handle = c->normals_handle, .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, .resource_state = state, .resource_queue = TM_RENDERER_QUEUE_GRAPHICS, .bits = region.normals, .size = sizeof(region.normals) });

    rb->submit_command_buffers(rb->inst, &cmd_buf, 1);
    rb->destroy_command_buffers(rb->inst, &cmd_buf, 1);

    while (!rb->read_complete(rb->inst, densities_rr, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL))
        ;
    while (!rb->read_complete(rb->inst, normals_rr, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL))
        ;

    destroy_mesh(c, man);

    mag_voxel_api->dual_contour_region(
        &region,
        man->backend,
        io,
        &c->mesh,
        &c->rbinder,
        &c->cbuffer);
    tm_renderer_resource_command_buffer_o *res_buf;
    rb->create_resource_command_buffers(rb->inst, &res_buf, 1);

    tm_renderer_draw_indexed_command_t *draw_command;
    c->region_indirect = tm_renderer_api->tm_renderer_resource_command_buffer_api->map_create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = sizeof(tm_renderer_draw_indexed_command_t), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_INDIRECT | TM_RENDERER_BUFFER_USAGE_UPDATABLE, .debug_tag = "mag_region_indirect" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, 0, &draw_command);
    *draw_command = (tm_renderer_draw_indexed_command_t) { .num_instances = 1, .num_indices = c->mesh.num_indices };

    rb->submit_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->destroy_resource_command_buffers(rb->inst, &res_buf, 1);
}

static void recreate_mesh(mag_terrain_component_t *c, mag_terrain_component_manager_o *man)
{
    TM_PROFILER_BEGIN_FUNC_SCOPE();

    tm_shader_system_o *vertex_buffer_system = tm_shader_repository_api->lookup_system(man->shader_repo, tm_murmur_hash_string("vertex_buffer_system"));
    tm_shader_io_o *vertex_buffer_io = tm_shader_api->system_io(vertex_buffer_system);

    generate_region_gpu(man->backend, man->shader_repo, vertex_buffer_io, c, man->ops);
    // dual_contour_cpu(c, man, vertex_buffer_io, man->backend);

    TM_PROFILER_END_FUNC_SCOPE();
}

static uint32_t bounding_volume_type(struct tm_component_manager_o *manager)
{
    return TM_BOUNDING_VOLUME_TYPE_BOX;
}

static void fill_bounding_volume_buffer(struct tm_component_manager_o *manager, struct tm_render_args_t *args, const union tm_entity_t *entities, const tm_transform_component_t *entity_transforms,
    const uint32_t *entity_indices, void **render_component_data, uint32_t num_renderables, uint8_t *bv_buffer)
{
    TM_PROFILER_BEGIN_FUNC_SCOPE();

    tm_bounding_volume_box_t *dest = (tm_bounding_volume_box_t *)bv_buffer;

    mag_terrain_component_t **cdata = (mag_terrain_component_t **)render_component_data;
    for (uint32_t i = 0; i != num_renderables; ++i) {
        mag_terrain_component_t *c = cdata[i];

        const float size = (float)MAG_VOXEL_REGION_SIZE * c->region_data.cell_size;
        const float margin = (float)MAG_VOXEL_MARGIN * c->region_data.cell_size;
        const tm_vec3_t margin_vec = { margin, margin, margin };

        tm_mat44_from_translation_quaternion_scale(&dest->tm, (tm_vec3_t) { 0, 0, 0 }, (tm_vec4_t) { 0, 0, 0, 0 }, (tm_vec3_t) { 1, 1, 1 });
        dest->visibility_mask = c->visibility_mask;
        dest->min = tm_vec3_sub(c->region_data.pos, margin_vec);
        dest->max = tm_vec3_add(dest->min, (tm_vec3_t) { size, size, size });
        ++dest;
    }

    TM_PROFILER_END_FUNC_SCOPE();
}

static void render(struct tm_component_manager_o *manager, struct tm_render_args_t *args, const tm_ci_render_viewer_t *viewers,
    uint32_t num_viewers, const tm_entity_t *entities, const tm_transform_component_t *entity_transforms, const bool *entity_selection_state, const uint32_t *entity_indices,
    void **render_component_data, uint32_t num_renderables, const uint8_t *frustum_visibility)
{
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    tm_shader_o *shader = tm_shader_repository_api->lookup_shader(args->shader_repository, tm_murmur_hash_string("magnum_terrain"));
    if (!shader)
        return;
    tm_shader_io_o *io = tm_shader_api->shader_io(shader);
    tm_shader_system_context_o *shader_context = tm_shader_system_api->clone_context(args->shader_context, a);

    tm_render_pipeline_shader_system_t selection_system;
    args->render_pipeline->api->global_shader_system(args->render_pipeline->inst, TM_RENDER_PIPELINE_EDITOR_SELECTION, &selection_system);

    /* carray */ tm_shader_constant_buffer_instance_t *cbufs = 0;
    tm_carray_temp_resize(cbufs, num_renderables, ta);
    tm_shader_api->create_constant_buffer_instances(io, num_renderables, cbufs);

    mag_terrain_component_t **cdata = (mag_terrain_component_t **)render_component_data;

    tm_renderer_shader_info_t *shader_infos = 0;
    tm_carray_temp_resize(shader_infos, num_viewers * num_renderables, ta);

    /* carray */ tm_renderer_draw_call_info_t *draw_calls = 0;
    tm_carray_temp_resize(draw_calls, num_viewers * num_renderables, ta);

    tm_shader_system_o *vertex_buffer_system = tm_shader_repository_api->lookup_system(man->shader_repo, tm_murmur_hash_string("vertex_buffer_system"));
    tm_shader_system_o *gbuffer_system = tm_shader_repository_api->lookup_system(args->shader_repository, tm_murmur_hash_string("gbuffer_system"));
    tm_shader_system_o *shadow_system = tm_shader_repository_api->lookup_system(args->shader_repository, tm_murmur_hash_string("shadow_system"));

    uint64_t gbuffer_sort_key = args->render_graph ? tm_render_graph_api->sort_key(args->render_graph, tm_murmur_hash_string("gbuffer")) : 0;
    uint64_t shadows_sort_key = 0;

    /* carray */ uint64_t *sort_keys = 0;
    tm_carray_temp_resize(sort_keys, num_viewers * num_renderables, ta);
    uint32_t num_draws = 0;
    for (uint32_t i = 0; i != num_renderables; ++i) {
        mag_terrain_component_t *component = cdata[i];
        if (!component->region_data.key) {
            // component->region_data = (region_data_t) {
            //     .cell_size = 1.f,
            //     .key = 1,
            // };
            // recreate_mesh(component, man);
            continue;
        }
        if (!component->mesh.vbuf.resource) {
            continue;
        }

        tm_shader_system_api->activate_system(shader_context, vertex_buffer_system,
            &component->cbuffer, 1,
            &component->rbinder, 1);

        bool updated = false;
        for (uint32_t v = 0; v != num_viewers; ++v) {
            if (!tm_culling_frustum_visible(frustum_visibility, i, v, num_viewers))
                continue;
            tm_shader_system_api->activate_system(shader_context, viewers[v].viewer_system, viewers[v].viewer_cbuffer, 1, viewers[v].viewer_rbinder, viewers[v].viewer_rbinder ? 1 : 0);
            tm_shader_system_o *context_system = v == 0 ? gbuffer_system : shadow_system;
            tm_shader_system_api->activate_system(shader_context, context_system, 0, 0, 0, 0);

            const uint32_t entity_idx = entity_indices[i];
            if (!updated) {
                // tm_mat44_t last_tm;
                // const tm_transform_t *t = &entity_transforms[entity_idx].world;
                // tm_mat44_from_translation_quaternion_scale(&last_tm, t->pos, t->rot, t->scl);

                // set_constant(io, args->default_resource_buffer, &cbufs[i], "last_tm", &last_tm, sizeof(last_tm));
                // set_constant(io, args->default_resource_buffer, &cbufs[i], "tm", &last_tm, sizeof(last_tm));

                tm_mat44_t tm;
                tm_mat44_from_translation_quaternion_scale(&tm, component->region_data.pos, (tm_vec4_t) { 0, 0, 0, 0 }, (tm_vec3_t) { 1, 1, 1 });
                // TODO: rm
                set_constant(io, args->default_resource_buffer, &cbufs[i], "last_tm", &tm, sizeof(tm));
                set_constant(io, args->default_resource_buffer, &cbufs[i], "tm", &tm, sizeof(tm));
                set_constant(io, args->default_resource_buffer, &cbufs[i], "color", &cdata[i]->color_rgb, sizeof(tm_vec3_t));
                updated = true;
            }

            const bool selected = entity_selection_state ? entity_selection_state[entity_idx] : false;
            if (v == 0 && selected) {
                tm_shader_system_api->activate_system(shader_context, selection_system.system,
                    selection_system.constants, selection_system.constants ? 1 : 0,
                    selection_system.resources, selection_system.resources ? 1 : 0);
            }

            tm_shader_api->assemble_shader_infos(shader, 0, 0, shader_context, TM_STRHASH(0), args->default_resource_buffer, &cbufs[i], 0, 1, &shader_infos[num_draws]);

            draw_calls[num_draws] = (tm_renderer_draw_call_info_t) {
                .primitive_type = TM_RENDERER_PRIMITIVE_TYPE_TRIANGLE_LIST,
                .draw_type = TM_RENDERER_DRAW_TYPE_INDEXED_INDIRECT,
                .indirect = {
                    .indirect_buffer = component->region_indirect,
                    .num_draws = 1 },
                .index_type = TM_RENDERER_INDEX_TYPE_UINT16,
                .index_buffer = component->mesh.ibuf,
            };
            sort_keys[num_draws] = viewers[v].sort_key | (v == 0 ? gbuffer_sort_key : shadows_sort_key);
            ++num_draws;

            if (v == 0 && selected)
                tm_shader_system_api->deactivate_system(shader_context, selection_system.system);

            tm_shader_system_api->deactivate_system(shader_context, context_system);
            tm_shader_system_api->deactivate_system(shader_context, viewers[v].viewer_system);
        }

        tm_shader_system_api->deactivate_system(shader_context, vertex_buffer_system);
    }
    if (num_draws)
        tm_renderer_api->tm_renderer_command_buffer_api->draw_calls(args->default_command_buffer, sort_keys, draw_calls, shader_infos, num_draws);

    tm_shader_api->destroy_constant_buffer_instances(io, cbufs, num_renderables);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static const char *component__category(void)
{
    return TM_LOCALIZE("Magnum Voxel");
}

static tm_ci_render_i *render_aspect = &(tm_ci_render_i) {
    .bounding_volume_type = bounding_volume_type,
    .fill_bounding_volume_buffer = fill_bounding_volume_buffer,
    .render = render
};

static tm_ci_editor_ui_i *editor_aspect = &(tm_ci_editor_ui_i) {
    .category = component__category
};

static tm_properties_aspect_i *properties_aspect = &(tm_properties_aspect_i) {
    .custom_ui = properties_ui,
    .get_type_display_name = get_type_display_name,
};

static void test_mag_terrain_component(tm_unit_test_runner_i *tr, tm_allocator_i *a)
{
    TM_INIT_TEMP_ALLOCATOR(ta);

    region_data_t *regions = wanted_regions((tm_vec3_t) { 0, 0, 0 }, ta);
    TM_UNIT_TEST(tr, tm_carray_size(regions) < 1200);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static tm_unit_test_i *mag_terrain_component_tests = &(tm_unit_test_i) {
    .name = "mag_terrain_component",
    .test = test_mag_terrain_component
};

void engine__update_terrain(tm_engine_o *inst, tm_engine_update_set_t *data, struct tm_entity_commands_o *commands)
{
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)inst;

    TM_INIT_TEMP_ALLOCATOR(ta);

    const tm_transform_t *camera_transform = tm_entity_api->get_blackboard_ptr(man->ctx, TM_ENTITY_BB__CAMERA_TRANSFORM);
    if (!camera_transform)
        return;
    region_data_t *regions_to_render = wanted_regions(camera_transform->pos, ta);

    for (uint64_t i = man->applied_op_count; i < tm_carray_size(man->ops); ++i) {
        apply_op(man, man->ops + i, data);
    }
    man->applied_op_count = tm_carray_size(man->ops);

    for (tm_engine_update_array_t *a = data->arrays; a < data->arrays + data->num_arrays; ++a) {
        mag_terrain_component_t *components = a->components[0];

        for (uint32_t i = 0; i < a->n; ++i) {
            mag_terrain_component_t *c = components + i;
            // if (c->region_data.key)
            //     continue;
            // c->region_data = (region_data_t){
            //     .pos = {0, 0, 0},
            //     .cell_size = 1.f,
            //     .key = 1,
            // };
            // recreate_mesh(c, man);
            // continue;

            bool existing = false;
            for (uint64_t r = 0; r < tm_carray_size(regions_to_render); ++r) {
                if (regions_to_render[r].key == c->region_data.key) {
                    regions_to_render[r] = regions_to_render[tm_carray_size(regions_to_render) - 1];
                    tm_carray_pop(regions_to_render);
                    existing = true;
                    break;
                }
            }

            if (!existing) {
                if (tm_carray_size(regions_to_render)) {
                    c->region_data = tm_carray_pop(regions_to_render);
                    tm_hash_add(&man->component_map, c->region_data.key, c);
                    recreate_mesh(c, man);
                } else {
                    tm_hash_remove(&man->component_map, c->region_data.key);
                    c->region_data.key = 0;
                }
            }
        }
    }

    if (tm_carray_size(regions_to_render)) {
        for (uint64_t r = 0; r < tm_carray_size(regions_to_render); ++r) {
            tm_entity_commands_api->create_entity_from_mask(commands, &man->component_mask);
        }
    }

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

#define CODE_ABOVE (1 << 0)
#define CODE_BELOW (1 << 1)
#define CODE_RIGHT (1 << 2)
#define CODE_LEFT (1 << 3)
#define CODE_BACK (1 << 4)
#define CODE_FRONT (1 << 5)

static uint8_t aabb_point_code(const aabb_t *aabb, const tm_vec3_t *p)
{
    uint8_t code = 0;

    code |= (p->y > aabb->max.y) ? CODE_ABOVE : 0;
    code |= (p->y < aabb->min.y) ? CODE_BELOW : 0;
    code |= (p->x > aabb->max.x) ? CODE_RIGHT : 0;
    code |= (p->x < aabb->min.x) ? CODE_LEFT : 0;
    code |= (p->z > aabb->max.z) ? CODE_BACK : 0;
    code |= (p->z < aabb->min.z) ? CODE_FRONT : 0;

    return code;
}

static bool aabb_segment_intersect(const aabb_t *aabb, tm_vec3_t start, tm_vec3_t end)
{
    // Cohen-Sutherland algorithm
    uint8_t code_start = aabb_point_code(aabb, &start);
    uint8_t code_end = aabb_point_code(aabb, &end);

    while (true) {
        if (!code_start || !code_end) {
            // one of the points is inside
            return true;
        }

        if (code_start & code_end) {
            return false;
        }

        uint8_t out_code = code_start > code_end ? code_start : code_end;

        tm_vec3_t new;
        if (out_code & CODE_ABOVE) {
            new.y = aabb->max.y;
            float t = (aabb->max.y - start.y) / (end.y - start.y);
            new.x = start.x + (end.x - start.x) * t;
            new.z = start.z + (end.z - start.z) * t;
        } else if (out_code & CODE_BELOW) {
            new.y = aabb->min.y;
            float t = (aabb->min.y - start.y) / (end.y - start.y);
            new.x = start.x + (end.x - start.x) * t;
            new.z = start.z + (end.z - start.z) * t;
        } else if (out_code & CODE_LEFT) {
            new.x = aabb->min.x;
            float t = (aabb->min.x - start.x) / (end.x - start.x);
            new.y = start.y + (end.y - start.y) * t;
            new.z = start.z + (end.z - start.z) * t;
        } else if (out_code & CODE_RIGHT) {
            new.x = aabb->max.x;
            float t = (aabb->max.x - start.x) / (end.x - start.x);
            new.y = start.y + (end.y - start.y) * t;
            new.z = start.z + (end.z - start.z) * t;
        } else if (out_code & CODE_FRONT) {
            new.z = aabb->min.z;
            float t = (aabb->min.z - start.z) / (end.z - start.z);
            new.x = start.x + (end.x - start.x) * t;
            new.y = start.y + (end.y - start.y) * t;
        } else if (out_code & CODE_BACK) {
            new.z = aabb->max.z;
            float t = (aabb->max.z - start.z) / (end.z - start.z);
            new.x = start.x + (end.x - start.x) * t;
            new.y = start.y + (end.y - start.y) * t;
        } else {
            // won't really get here, but otherwise the compiler complains
            // about the uninitialized variable
            return true;
        }

        if (out_code == code_start) {
            start = new;
            code_start = aabb_point_code(aabb, &start);
        } else {
            end = new;
            code_end = aabb_point_code(aabb, &end);
        }
    }
}

static bool cast_ray(mag_terrain_component_manager_o *man, tm_vec3_t ray_start, tm_vec3_t ray_dir, float max_distance, float *hit_distance)
{
    TM_ASSERT(max_distance < (float)MAG_VOXEL_CHUNK_SIZE, tm_error_api->def, "max ray distance (%f) beyond region size (%f) is not supported", max_distance, (float)MAG_VOXEL_CHUNK_SIZE);
    tm_vec3_t chunk_size = { (float)MAG_VOXEL_CHUNK_SIZE, (float)MAG_VOXEL_CHUNK_SIZE, (float)MAG_VOXEL_CHUNK_SIZE };

    tm_renderer_handle_t region_densities[5];
    tm_vec3_t region_positions[5];
    uint32_t region_count = 0;

    tm_vec3_t min = tm_vec3_sub(floor_to(ray_start, MAG_VOXEL_CHUNK_SIZE), chunk_size);
    tm_vec3_t max = tm_vec3_add(ceil_to(ray_start, MAG_VOXEL_CHUNK_SIZE), chunk_size);

    tm_vec3_t segment_end = tm_vec3_add(ray_start, tm_vec3_mul(ray_dir, max_distance));

    for (float x = min.x; x < max.x; x += MAG_VOXEL_CHUNK_SIZE) {
        for (float y = min.y; y < max.y; y += MAG_VOXEL_CHUNK_SIZE) {
            for (float z = min.z; z < max.z; z += MAG_VOXEL_CHUNK_SIZE) {
                tm_vec3_t region_min = { x, y, z };
                aabb_t aabb = { region_min, tm_vec3_add(region_min, chunk_size) };
                if (aabb_segment_intersect(&aabb, ray_start, segment_end)) {
                    uint64_t key = region_key(region_min, (float)MAG_VOXEL_CHUNK_SIZE, 0);
                    mag_terrain_component_t *c = tm_hash_get(&man->component_map, key);
                    if (!c)
                        return false;
                    region_densities[region_count] = c->densities_handle;
                    region_positions[region_count] = region_min;
                    ++region_count;
                    if (region_count > 4)
                        goto check_count;
                }
            }
        }
    }

check_count:
    bool count_ok = TM_ASSERT(region_count > 0 && region_count <= 4, tm_error_api->def,
        "Got ray (%f, %f, %f) -> (%f, %f, %f) intersecting %lu regions. How the hell?",
        ray_start.x, ray_start.y, ray_start.z, segment_end.x, segment_end.y, segment_end.z, region_count);

    if (!count_ok)
        return false;

    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    tm_renderer_backend_i *rb = man->backend;

    tm_renderer_command_buffer_o *cmd_buf;
    rb->create_command_buffers(rb->inst, &cmd_buf, 1);
    tm_renderer_resource_command_buffer_o *res_buf;
    rb->create_resource_command_buffers(rb->inst, &res_buf, 1);

    // TODO: cache this buffer and other shader data
    tm_renderer_handle_t result_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = sizeof(float), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_ray_cast" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);

    tm_shader_o *shader = tm_shader_repository_api->lookup_shader(man->shader_repo, TM_STATIC_HASH("magnum_raycast", 0xc7699175374f215fULL));
    tm_shader_io_o *io = tm_shader_api->shader_io(shader);

    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuf;
    tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
    tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);

    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("region_densities", 0x6b41b51de143171cULL), region_densities, 0, 0, region_count);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("ray_hit", 0x8afc43bd9fd77461ULL), &result_handle, 0, 0, 1);

    set_constant(io, res_buf, &cbuf, "region_positions", region_positions, region_count * sizeof(region_positions[0]));
    set_constant(io, res_buf, &cbuf, "region_count", &region_count, sizeof(region_count));
    set_constant(io, res_buf, &cbuf, "ray_start", &ray_start, sizeof(ray_start));
    set_constant(io, res_buf, &cbuf, "ray_dir", &ray_dir, sizeof(ray_dir));
    set_constant(io, res_buf, &cbuf, "ray_max_length", &max_distance, sizeof(max_distance));

    tm_renderer_shader_info_t shader_info;
    tm_shader_system_context_o *shader_context = tm_shader_system_api->create_context(a, NULL);
    if (tm_shader_api->assemble_shader_infos(shader, 0, 0, shader_context, TM_STRHASH(0), res_buf, &cbuf, &rbinder, 1, &shader_info)) {
        uint64_t zero_sort_key = 0;
        tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, &zero_sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { 1, 1, 1 } }, &shader_info, 1);
    }
    tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);
    tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);

    uint64_t sort_key = UINT64_MAX;
    tm_renderer_api->tm_renderer_command_buffer_api->bind_queue(cmd_buf, sort_key, &(tm_renderer_queue_bind_t) { .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL });
    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;

    uint32_t rr = tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, sort_key, &(tm_renderer_read_buffer_t) { .resource_handle = result_handle, .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, .resource_state = state, .resource_queue = TM_RENDERER_QUEUE_GRAPHICS, .bits = hit_distance, .size = sizeof(float) });

    rb->submit_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->destroy_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->submit_command_buffers(rb->inst, &cmd_buf, 1);
    rb->destroy_command_buffers(rb->inst, &cmd_buf, 1);

    // TODO: do something with this blocking
    while (!rb->read_complete(rb->inst, rr, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL))
        ;

    rb->create_resource_command_buffers(rb->inst, &res_buf, 1);
    tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, result_handle);
    rb->submit_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->destroy_resource_command_buffers(rb->inst, &res_buf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);

    return *hit_distance != -9999.f;
}

void apply_operation(mag_terrain_component_manager_o *man, mag_terrain_op_type_t type, mag_terrain_op_primitive_t primitive, tm_vec3_t pos, tm_vec3_t size)
{
    op_t op = {
        .type = type,
        .primitive = primitive,
        .pos = pos,
        .size = size,
    };
    tm_carray_push(man->ops, op, &man->allocator);
}

static struct mag_terrain_api terrain_api = {
    .cast_ray = cast_ray,
    .apply_operation = apply_operation,
};

static void entity_simulation__register(struct tm_entity_context_o *ctx)
{
    if (tm_entity_api->get_blackboard_double(ctx, TM_ENTITY_BB__EDITOR, 0))
        return;

    const tm_component_type_t mag_terrain_component = tm_entity_api->lookup_component_type(ctx, MAG_TT_TYPE_HASH__TERRAIN_COMPONENT);
    const tm_component_type_t transform_component = tm_entity_api->lookup_component_type(ctx, TM_TT_TYPE_HASH__TRANSFORM_COMPONENT);

    if (!mag_terrain_component.index)
        return;

    // TOOD: move to components_created
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)tm_entity_api->component_manager(ctx, mag_terrain_component);
    man->component_mask = tm_entity_api->create_component_mask((tm_component_type_t[2]) { transform_component, mag_terrain_component }, 2);

    const tm_engine_i mirror_sound_sources = {
        .ui_name = TM_LOCALIZE_LATER("Magnum Terrain"),
        .hash = MAG_ENGINE__TERRAIN,
        .num_components = 1,
        .components = { mag_terrain_component },
        .writes = { true },
        .update = engine__update_terrain,
        .inst = (tm_engine_o *)man,
        .before_me = { TM_PHASE__CAMERA },
        .after_me = { TM_PHASE__RENDER },
    };

    tm_entity_api->register_engine(ctx, &mirror_sound_sources);
}

TM_DLL_EXPORT void tm_load_plugin(struct tm_api_registry_api *reg, bool load)
{
    tm_global_api_registry = reg;

    tm_entity_api = tm_get_api(reg, tm_entity_api);
    tm_entity_commands_api = tm_get_api(reg, tm_entity_commands_api);
    tm_error_api = tm_get_api(reg, tm_error_api);
    tm_profiler_api = tm_get_api(reg, tm_profiler_api);
    tm_localizer_api = tm_get_api(reg, tm_localizer_api);
    tm_properties_view_api = tm_get_api(reg, tm_properties_view_api);
    tm_render_graph_api = tm_get_api(reg, tm_render_graph_api);
    tm_renderer_api = tm_get_api(reg, tm_renderer_api);
    tm_shader_api = tm_get_api(reg, tm_shader_api);
    tm_shader_system_api = tm_get_api(reg, tm_shader_system_api);
    tm_shader_repository_api = tm_get_api(reg, tm_shader_repository_api);
    tm_temp_allocator_api = tm_get_api(reg, tm_temp_allocator_api);
    tm_the_truth_api = tm_get_api(reg, tm_the_truth_api);
    tm_the_truth_common_types_api = tm_get_api(reg, tm_the_truth_common_types_api);
    tm_logger_api = tm_get_api(reg, tm_logger_api);
    tm_visibility_flags_api = tm_get_api(reg, tm_visibility_flags_api);

    mag_voxel_api = tm_get_api(reg, mag_voxel_api);

    tm_set_or_remove_api(reg, load, mag_terrain_api, &terrain_api);

    tm_add_or_remove_implementation(reg, load, tm_unit_test_i, mag_terrain_component_tests);
    tm_add_or_remove_implementation(reg, load, tm_entity_register_engines_simulation_i, entity_simulation__register);

    tm_add_or_remove_implementation(reg, load, tm_the_truth_create_types_i, create_truth_types);
    tm_add_or_remove_implementation(reg, load, tm_entity_create_component_i, create_mag_terrain_component);
}
