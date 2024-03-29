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
static struct tm_creation_graph_api *tm_creation_graph_api;
static struct tm_ui_api *tm_ui_api;
static struct tm_statistics_source_api *tm_statistics_source_api;
static struct tm_os_api *tm_os_api;
static struct tm_task_system_api *tm_task_system_api;
static struct tm_physx_scene_api *tm_physx_scene_api;

static struct mag_voxel_api *mag_voxel_api;
static struct mag_async_gpu_queue_api *mag_async_gpu_queue_api;

#include "mag_terrain_component.h"

#include <foundation/api_registry.h>
#include <foundation/atomics.inl>
#include <foundation/bounding_volume.h>
#include <foundation/buffer.h>
#include <foundation/buffer_format.h>
#include <foundation/camera.h>
#include <foundation/carray.inl>
#include <foundation/error.h>
#include <foundation/hash.inl>
#include <foundation/input.h>
#include <foundation/job_system.h>
#include <foundation/localizer.h>
#include <foundation/log.h>
#include <foundation/math.inl>
#include <foundation/os.h>
#include <foundation/profiler.h>
#include <foundation/random.h>
#include <foundation/slab.inl>
#include <foundation/sort.inl>
#include <foundation/task_system.h>
#include <foundation/the_truth.h>
#include <foundation/the_truth_types.h>
#include <foundation/undo.h>
#include <foundation/unit_test.h>
#include <foundation/visibility_flags.h>

#include <plugins/creation_graph/creation_graph.h>
#include <plugins/creation_graph/creation_graph_interpreter.h>
#include <plugins/creation_graph/creation_graph_output.inl>
#include <plugins/creation_graph/image_nodes.h>
#include <plugins/editor_views/properties.h>
#include <plugins/entity/entity.h>
#include <plugins/entity/transform_component.h>
#include <plugins/physics/physics_collision.h>
#include <plugins/physics/physics_material.h>
#include <plugins/physics/physics_shape_component.h>
#include <plugins/physx/physx_scene.h>
#include <plugins/render_graph/render_graph.h>
#include <plugins/render_graph_toolbox/render_pipeline.h>
#include <plugins/renderer/commands.h>
#include <plugins/renderer/render_backend.h>
#include <plugins/renderer/render_command_buffer.h>
#include <plugins/renderer/renderer.h>
#include <plugins/renderer/resources.h>
#include <plugins/shader_system/shader_system.h>
#include <plugins/statistics/statistics_source.h>
#include <plugins/the_machinery_shared/component_interfaces/editor_ui_interface.h>
#include <plugins/the_machinery_shared/component_interfaces/render_interface.h>
#include <plugins/the_machinery_shared/frustum_culling.h>
#include <plugins/the_machinery_shared/render_context.h>
#include <plugins/ui/ui.h>
#include <plugins/ui/ui_icon.h>

#include "plugins/mag_async_gpu_queue/mag_async_gpu_queue.h"
#include "plugins/mag_voxel/mag_voxel.h"

#define MAX_ASYNC_GPU_TASKS 10
// TODO: compute this
#define MAX_REGIONS_PER_OP 30
#define MAX_TASK_BUFFERS (MAX_ASYNC_GPU_TASKS + MAX_REGIONS_PER_OP)
#define ALPHA_SPEED 3.0f
#define MAX_EXTRA_REGIONS 100

#define MAX_SCULPT_DISTANCE 65.f

// NOTE: must be kept in sync with magnum_dc_octree.tmsl
#define OCTREE_DEPTH 5
#define OCTREE_NODE_SIZE 64

#include "mag_octree_edges.inl"

// up to 6 indexes (1 quad) per edge
#define MAX_INDICES_PER_REGION (6 * OCTREE_EDGE_COUNT)
// 1 vertex per cell
#define MAX_VERTICES_PER_REGION (MAG_VOXEL_REGION_SIZE * MAG_VOXEL_REGION_SIZE * MAG_VOXEL_REGION_SIZE)

static const uint32_t is_critical_cube[256] = {
    0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0
};

static const struct
{
    float distance;
    float size;
    float qef_tolerance;
    // must be 0 for LODs with physics, otherwise physics won't match visuals
    float visual_depth_bias;
    // bits enough to fit ceil((distance + CHUNK_SIZE * size))
    uint16_t bits;
    bool needs_physics;
    bool needs_shadows;
} LODS[] = {
    // { 64.f, 1.f, 15 },
    { 128.f, 1.f, 0.15f, 0.f, 7, true, true },
    { 200.f, 2.f, 0.15f, 0.f, 8, true, false },
    // { 128.f, 2.f, 0.15f, 0.f, 10, true },
    // { 64.f, 2.f, 0.15f, 10, true, true },
    { 256.f, 4.f, 0.15f, 0.f, 10, true, false },
    { 512.f, 8.f, 0.15f, 0.f, 10, true, false },
    { 1024.f, 16.f, 0.15f, 0.f, 10, true, false },
    { 2048.f, 32.f, 0.15f, 0.f, 10, true, false },
    { 4096.f, 64.f, 0.15f, 0.f, 10, false, false },
    { 8192.f, 128.f, 0.15f, 0.f, 10, false, false },
    // { 512.f, 16.f, 0.15f, 0.f, 12, true },
    // { 2048.f, 32.f, 0.15f, 3.f, 14, false },
    // { 10000.f, 128.f, 0.15f, 6.f, 16, false },
    //{ 10000.f, 128.f, 1.f, 20, false },
    //{ 128.f, 10.f / 32.f, 15 },
    // { 256.f, 2.f, 10 },
    // { 1024.f, 8.f, 16 },
    //{ 10000.f, 64.f, 20 },
};

typedef struct aabb_t
{
    tm_vec3_t min;
    tm_vec3_t max;
} aabb_t;

static inline float aabb_point_distance_sqr(const aabb_t *aabb, const tm_vec3_t *p)
{
    float dx = tm_max(0.f, tm_max(aabb->min.x - p->x, p->x - aabb->max.x));
    float dy = tm_max(0.f, tm_max(aabb->min.y - p->y, p->y - aabb->max.y));
    float dz = tm_max(0.f, tm_max(aabb->min.z - p->z, p->z - aabb->max.z));

    return (dx * dx + dy * dy + dz * dz);
}

static inline float aabb_point_distance(const aabb_t *aabb, const tm_vec3_t *p)
{
    return sqrtf(aabb_point_distance_sqr(aabb, p));
}

static inline bool aabb_intersect(const aabb_t *a, const aabb_t *b)
{
    return (
        a->min.x <= b->max.x && a->max.x >= b->min.x && a->min.y <= b->max.y && a->max.y >= b->min.y && a->min.z <= b->max.z && a->max.z >= b->min.z);
}

typedef struct op_t
{
    // TODO: use a more compact representation (half floats for size?)
    uint8_t type;
    uint8_t primitive;
    tm_vec3_t pos;
    tm_vec3_t radius;
    struct op_t *next;
} op_t;

static inline aabb_t op_aabb(const op_t *op)
{
    tm_vec3_t radius_plus_epsilon = tm_vec3_add(op->radius, (tm_vec3_t) { 0.1f, 0.1f, 0.1f });
    return (aabb_t) { tm_vec3_sub(op->pos, radius_plus_epsilon), tm_vec3_add(op->pos, radius_plus_epsilon) };
}

typedef struct region_data_t
{
    tm_vec3_t pos;
    uint8_t lod;
    TM_PAD(3);
    uint64_t key;
    tm_vec3_t cull_min;
    tm_vec3_t cull_max;
} region_data_t;

static inline aabb_t region_aabb_with_margin(const region_data_t *region_data)
{
    const float margin = MAG_VOXEL_MARGIN * LODS[region_data->lod].size;
    const float full_size = MAG_VOXEL_REGION_SIZE * LODS[region_data->lod].size;

    tm_vec3_t margin3 = { margin, margin, margin };
    tm_vec3_t size3 = { full_size, full_size, full_size };
    tm_vec3_t min = tm_vec3_sub(region_data->pos, margin3);
    tm_vec3_t max = tm_vec3_add(min, size3);

    return (aabb_t) { min, max };
}

static inline tm_vec3_t region_center(const region_data_t *region_data)
{
    const float half_size = MAG_VOXEL_CHUNK_SIZE * LODS[region_data->lod].size * 0.5f;
    return (tm_vec3_t) {
        region_data->pos.x + half_size,
        region_data->pos.y + half_size,
        region_data->pos.z + half_size,
    };
}

static inline uint64_t region_priority(const region_data_t *region_data, const tm_vec3_t *camera_pos)
{
    uint64_t prev_lod_bits = 0;
    for (uint8_t i = 0; i < region_data->lod; ++i) {
        prev_lod_bits += LODS[i].bits;
    }
    float center_distance = tm_vec3_dist(*camera_pos, region_center(region_data));

    return (uint64_t)center_distance << prev_lod_bits;
}

typedef struct mag_terrain_material_t
{
    tm_creation_graph_instance_t creation_graph_instance;
    float static_friction;
    float dynamic_friction;
    float restitution;
    bool allow_from_top;
    bool allow_from_sides;
    tm_tt_id_t collision_id;

} mag_terrain_material_t;

typedef struct mag_terrain_settings_t
{
    /* carray */ mag_terrain_material_t *materials;
    /* carray */ tm_renderer_handle_t *diffuse_maps;
    /* carray */ tm_renderer_handle_t *normal_maps;
    /* carray */ tm_renderer_handle_t *orm_maps;
} mag_terrain_settings_t;

// see update_region_bounds() in magnum_common.tmsl
#define REGION_INFO_BOUNDS_SCALE 1000
// workaround for HLSL -> SPIR-V compiler not supporting signed interlocked min/max
// https://github.com/microsoft/DirectXShaderCompiler/issues/3196
#define REGION_INFO_BOUNDS_OFFSET (MAG_VOXEL_MARGIN + 1)

// Region info returned from the GPU
typedef struct gpu_region_info_t
{
    uint32_t num_indices;
    uint32_t num_vertices;

    // Bounds are returned relative to region_pos and are scaled by REGION_INFO_BOUNDS_SCALE. LOD scale is not applied.
    uint32_t min_x;
    uint32_t min_y;
    uint32_t min_z;

    uint32_t max_x;
    uint32_t max_y;
    uint32_t max_z;
} gpu_region_info_t;

typedef struct mag_terrain_component_buffers_t
{
    // (f16, f16, f16) vertex, (f16, f16, f16) normal
    tm_renderer_handle_t vertices_handle;
    tm_renderer_handle_t ibuf;

    tm_renderer_handle_t densities_handle;
    // contains gpu_region_info_t
    tm_renderer_handle_t region_info_handle;

    atomic_uint_least32_t generate_fence;
    gpu_region_info_t region_info;

    uint32_t gen_region_task_buffers_id;
    uint32_t read_mesh_task_buffers_id;

    tm_shader_resource_binder_instance_t region_contouring_rbinder;
    tm_shader_constant_buffer_instance_t region_contouring_cbuf;
} mag_terrain_component_buffers_t;

typedef struct generate_physics_task_data_t
{
    mag_terrain_component_manager_o *man;
    atomic_uint_least64_t current_task_id;

    tm_the_truth_o *tt;

    uint32_t read_mesh_task_buffers_id;

    tm_critical_section_o cs;
    uint32_t buffer_id;

    tm_physics_shape_component_t physics_component;
} generate_physics_task_data_t;

typedef struct mag_terrain_component_t
{
    // Visibility mask built from VISIBILITY_FLAGS in The Truth.
    uint64_t visibility_mask;
    tm_vec4_t color_rgba;

    region_data_t region_data;

    mag_terrain_component_buffers_t *buffers;
    generate_physics_task_data_t *physics_data;

    const op_t *last_applied_op;

    uint64_t generate_task_id;
    uint64_t read_mesh_task_id;
    const op_t *last_task_op;

    tm_shader_resource_binder_instance_t region_render_rbinder;
    tm_shader_constant_buffer_instance_t region_render_cbuf;
} mag_terrain_component_t;

static mag_terrain_component_t default_values = {
    .color_rgba = { 0.8f, 0.8f, 0.8f, 1.f },
};

typedef struct mag_terrain_component_state_t
{
    mag_terrain_component_buffers_t *buffers;
    uint64_t generate_task_id;
} mag_terrain_component_state_t;

typedef struct region_task_buffers_t
{
    tm_shader_resource_binder_instance_t gen_region_rbinder;
    tm_shader_constant_buffer_instance_t gen_region_cbuf;
    tm_shader_system_context_o *gen_region_context;

    tm_shader_system_context_o *octree_context;
    tm_shader_constant_buffer_instance_t octree_collapse_cbufs[OCTREE_DEPTH - 1];

    tm_renderer_handle_t octree_handle;
    tm_renderer_handle_t collapsed_octree_handle;
} region_task_buffers_t;

typedef struct read_mesh_task_buffers_t
{
    /* carray */ uint16_t *indices;
    /* carray */ tm_vec3_t *vertices;
    tm_renderer_handle_t unpacked_vertices;

    tm_shader_o *unpack_vertices_shader;
    tm_shader_system_context_o *unpack_vertices_context;
    tm_shader_resource_binder_instance_t unpack_vertices_rbinder;
    uint32_t unpack_vertices_packed_vertices_slot;
} read_mesh_task_buffers_t;

typedef struct mag_terrain_component_manager_o
{
    tm_entity_context_o *ctx;
    tm_allocator_i allocator;

    tm_renderer_backend_i *backend;
    tm_shader_repository_o *shader_repo;

    mag_terrain_settings_t terrain_settings;
    tm_tt_id_t terrain_settings_id;

    tm_tt_id_t raycast_collision_id;

    tm_component_mask_t component_mask;
    tm_component_type_t physics_shape_component_type;
    tm_component_type_t rigid_static_component_type;

    struct TM_HASH_T(uint64_t, mag_terrain_component_state_t) component_map;

    // keys that are either full of air or are fully solid
    tm_set_t empty_regions;

    /* slab */ op_t *ops;
    op_t *last_empty_check_op;

    mag_async_gpu_queue_o *gpu_queue;

    atomic_uint_least32_t region_task_buffers_locks[MAX_TASK_BUFFERS];
    region_task_buffers_t region_task_buffers[MAX_TASK_BUFFERS];

    atomic_uint_least32_t read_mesh_task_buffers_locks[MAX_TASK_BUFFERS];
    read_mesh_task_buffers_t read_mesh_task_buffers[MAX_TASK_BUFFERS];

    tm_renderer_handle_t precomputed_octree_edges_handle;

    tm_shader_o *gen_region_shader;
    tm_shader_o *apply_op_shader;
    tm_shader_o *octree_create_shader;
    tm_shader_o *octree_collapse_shader;
    tm_shader_o *octree_contour_shader;
    tm_shader_system_o *region_contouring_system;
    tm_shader_system_o *region_render_system;

    tm_shader_system_o *material_system;
    tm_shader_resource_binder_instance_t material_rbinder;
} mag_terrain_component_manager_o;

static bool set_constant(tm_shader_io_o *io, tm_renderer_resource_command_buffer_o *res_buf, const tm_shader_constant_buffer_instance_t *instance, tm_strhash_t name, const void *data, uint32_t data_size)
{
    uint32_t constant_offset;

    if (tm_shader_api->lookup_constant(io, name, NULL, &constant_offset)) {
        tm_shader_api->update_constants(io, res_buf, &(tm_shader_constant_update_t) { .instance_id = instance->instance_id, .constant_offset = constant_offset, .num_bytes = data_size, .data = data }, 1);
        return true;
    }

    return false;
}

static bool set_resource(tm_shader_io_o *io, tm_renderer_resource_command_buffer_o *res_buf, tm_shader_resource_binder_instance_t *instance,
    tm_strhash_t name, const tm_renderer_handle_t *resource_handle, const uint32_t *aspect_flags, uint32_t first_resource, uint32_t n_resources)
{
    uint32_t resource_slot;
    if (tm_shader_api->lookup_resource(io, name, NULL, &resource_slot)) {
        tm_shader_api->update_resources(io, res_buf, &(tm_shader_resource_update_t) { .instance_id = instance->instance_id, .resource_slot = resource_slot, .first_resource = first_resource, .num_resources = n_resources, .resources = resource_handle, .resources_view_aspect_flags = aspect_flags }, 1);
        return true;
    }
    return false;
}

static inline void random_rotation_matrix(tm_mat44_t *mat, uint64_t rand[2])
{
    float u = tm_random_to_float(tm_random_next(rand));
    float v = tm_random_to_float(tm_random_next(rand));
    float w = tm_random_to_float(tm_random_next(rand));
    // http://planning.cs.uiuc.edu/node198.html
    tm_vec4_t quat = {
        sqrtf(1.f - u) * sinf(2.f * TM_PI * v),
        sqrtf(1.f - u) * cosf(2.f * TM_PI * v),
        sqrtf(u) * sinf(2.f * TM_PI * w),
        sqrtf(u) * cosf(2.f * TM_PI * w)
    };
    tm_mat44_from_quaternion(mat, quat);
}

static inline uint32_t octree_node_count(uint32_t depth)
{
    uint32_t node_count = 1;
    uint32_t pow8 = 1;
    for (uint32_t i = 0; i < depth; ++i) {
        pow8 *= 8;
        node_count += pow8;
    }
    return node_count;
}

static void init_region_task_buffers(mag_terrain_component_manager_o *man, region_task_buffers_t *buffers, tm_shader_repository_o *shader_repo, tm_renderer_resource_command_buffer_o *res_buf, tm_allocator_i *allocator)
{
    {
        tm_shader_io_o *io = tm_shader_api->shader_io(man->gen_region_shader);
        tm_shader_api->create_resource_binder_instances(io, 1, &buffers->gen_region_rbinder);
        tm_shader_api->create_constant_buffer_instances(io, 1, &buffers->gen_region_cbuf);
    }

    {
        uint64_t rand[2] = { 12876523487, 95734826 };
        tm_shader_system_o *density_system = tm_shader_repository_api->lookup_system(shader_repo, TM_STATIC_HASH("magnum_density", 0x4e7ad65be7b8da4dULL));
        tm_shader_constant_buffer_instance_t density_cbuf;
        tm_shader_io_o *density_io = tm_shader_api->system_io(density_system);
        tm_shader_api->create_constant_buffer_instances(density_io, 1, &density_cbuf);
        tm_mat44_t rot_mats[9];
        for (uint32_t i = 0; i < TM_ARRAY_COUNT(rot_mats); ++i) {
            random_rotation_matrix(rot_mats + i, rand);
        }
        set_constant(density_io, res_buf, &density_cbuf, TM_STATIC_HASH("rot_mat", 0x1f42c345b2db8b68ULL), rot_mats, sizeof(rot_mats));

        buffers->gen_region_context = tm_shader_system_api->create_context(allocator, NULL);
        tm_shader_system_api->activate_system(buffers->gen_region_context, density_system, &density_cbuf, 1, NULL, 0);
        tm_shader_api->destroy_constant_buffer_instances(density_io, &density_cbuf, 1);
    }

    buffers->octree_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = OCTREE_NODE_SIZE * octree_node_count(OCTREE_DEPTH), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_UPDATABLE, .debug_tag = "mag_region_octree" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);

    buffers->collapsed_octree_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = 4 * octree_node_count(OCTREE_DEPTH), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_octree_collapsed" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);

    {
        buffers->octree_context = tm_shader_system_api->create_context(allocator, NULL);

        tm_shader_system_o *octree_system = tm_shader_repository_api->lookup_system(shader_repo, TM_STATIC_HASH("magnum_terrain_octree_system", 0x68f27467d86cd35eULL));
        tm_shader_io_o *octree_io = tm_shader_api->system_io(octree_system);

        tm_shader_resource_binder_instance_t octree_rbinder;
        tm_shader_api->create_resource_binder_instances(octree_io, 1, &octree_rbinder);
        set_resource(octree_io, res_buf, &octree_rbinder, TM_STATIC_HASH("octree", 0x2a78a6f66423a87fULL), &buffers->octree_handle, 0, 0, 1);
        set_resource(octree_io, res_buf, &octree_rbinder, TM_STATIC_HASH("collapsed_octree", 0x76cbd4dd06628da3ULL), &buffers->collapsed_octree_handle, 0, 0, 1);
        tm_shader_system_api->activate_system(buffers->octree_context, octree_system, NULL, 0, &octree_rbinder, 1);
        tm_shader_api->destroy_resource_binder_instances(octree_io, &octree_rbinder, 1);
    }

    {
        tm_shader_io_o *octree_collapse_io = tm_shader_api->shader_io(man->octree_collapse_shader);
        tm_shader_api->create_constant_buffer_instances(octree_collapse_io, TM_ARRAY_COUNT(buffers->octree_collapse_cbufs), buffers->octree_collapse_cbufs);

        uint32_t nodes_on_level = 1;
        uint32_t last_root_id = 0;
        for (uint32_t i = 0; i < TM_ARRAY_COUNT(buffers->octree_collapse_cbufs) - 1; ++i) {
            last_root_id = last_root_id * 8 + 1;
            nodes_on_level *= 8;
        }

        for (uint32_t depth = TM_ARRAY_COUNT(buffers->octree_collapse_cbufs); depth > 0; --depth) {
            uint32_t parent_depth = depth - 1;
            set_constant(octree_collapse_io, res_buf, buffers->octree_collapse_cbufs + parent_depth, TM_STATIC_HASH("first_node_id", 0x21e66c5ada72b20bULL), &last_root_id, sizeof(last_root_id));
            set_constant(octree_collapse_io, res_buf, buffers->octree_collapse_cbufs + parent_depth, TM_STATIC_HASH("first_node_depth", 0x7204da62374e7fc2ULL), &parent_depth, sizeof(parent_depth));
            nodes_on_level /= 8;
            last_root_id = (last_root_id - 1) / 8;
        }
    }

    {
        tm_shader_system_o *octree_common_system = tm_shader_repository_api->lookup_system(shader_repo, TM_STATIC_HASH("magnum_dc_octree", 0x7cf5adfe4cbe132fULL));
        tm_shader_constant_buffer_instance_t cbuf;
        tm_shader_resource_binder_instance_t rbinder;
        tm_shader_io_o *io = tm_shader_api->system_io(octree_common_system);
        tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);
        tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
        set_constant(io, res_buf, &cbuf, TM_STATIC_HASH("is_critical_cube", 0x55276f2e1d8770b0ULL), is_critical_cube, sizeof(is_critical_cube));
        set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("precomputed_edges", 0xe8084bd05a04703dULL), &man->precomputed_octree_edges_handle, 0, 0, 1);
        tm_shader_system_api->activate_system(buffers->octree_context, octree_common_system, &cbuf, 1, &rbinder, 1);
        tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);
        tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);
    }
}

static void destroy_region_task_buffers(mag_terrain_component_manager_o *man, region_task_buffers_t *buffers, tm_renderer_resource_command_buffer_o *res_buf, tm_allocator_i *allocator)
{
    tm_shader_io_o *io = tm_shader_api->shader_io(man->gen_region_shader);
    tm_shader_api->destroy_resource_binder_instances(io, &buffers->gen_region_rbinder, 1);
    tm_shader_api->destroy_constant_buffer_instances(io, &buffers->gen_region_cbuf, 1);
    tm_shader_system_api->destroy_context(buffers->gen_region_context);
    tm_shader_system_api->destroy_context(buffers->octree_context);

    tm_shader_io_o *octree_collapse_io = tm_shader_api->shader_io(man->octree_collapse_shader);
    tm_shader_api->destroy_constant_buffer_instances(octree_collapse_io, buffers->octree_collapse_cbufs, TM_ARRAY_COUNT(buffers->octree_collapse_cbufs));

    tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, buffers->octree_handle);
    tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, buffers->collapsed_octree_handle);
}

static void init_read_mesh_task_buffers(read_mesh_task_buffers_t *buffers, tm_shader_repository_o *shader_repo, tm_renderer_resource_command_buffer_o *res_buf, tm_allocator_i *allocator)
{
    buffers->vertices = 0;
    buffers->indices = 0;
    tm_carray_ensure(buffers->vertices, MAX_VERTICES_PER_REGION, allocator);
    tm_carray_ensure(buffers->indices, MAX_INDICES_PER_REGION, allocator);

    buffers->unpacked_vertices = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = sizeof(tm_vec3_t) * MAX_VERTICES_PER_REGION, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_unpacked_vertices" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
}

static void destroy_read_mesh_task_buffers(read_mesh_task_buffers_t *buffers, tm_renderer_resource_command_buffer_o *res_buf, tm_allocator_i *allocator)
{
    tm_carray_free(buffers->indices, allocator);
    tm_carray_free(buffers->vertices, allocator);

    tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, buffers->unpacked_vertices);
}

#define LOCK_BUFFERS(buffer_locks, out_id)                                                                               \
    {                                                                                                                    \
        out_id = 0;                                                                                                      \
        while (true) {                                                                                                   \
            for (uint32_t macro_buffer_idx = 0; macro_buffer_idx < TM_ARRAY_COUNT((buffer_locks)); ++macro_buffer_idx) { \
                if (atomic_exchange_uint32_t((buffer_locks) + macro_buffer_idx, 1) == 0) {                               \
                    out_id = macro_buffer_idx + 1;                                                                       \
                    break;                                                                                               \
                }                                                                                                        \
            }                                                                                                            \
            if (out_id)                                                                                                  \
                break;                                                                                                   \
        }                                                                                                                \
    }
#define GET_BUFFERS(buffers, id) ((buffers) + ((id)-1))
#define RELEASE_BUFFERS(buffer_locks, id)                       \
    {                                                           \
        atomic_exchange_uint32_t((buffer_locks) + ((id)-1), 0); \
        (id) = 0;                                               \
    }

typedef struct read_mesh_task_data_t
{
    mag_terrain_component_buffers_t *c;
    mag_terrain_component_manager_o *man;
} read_mesh_task_data_t;

void read_mesh_task(mag_async_gpu_queue_task_args_t *args);

static float floor_to_f(float val, float size)
{
    float floored = floorf(val);
    float modulo = fmodf(floored, size);
    return modulo < 0.f ? floored - size - modulo : floored - modulo;
}

static float ceil_to_f(float val, float size)
{
    float ceiled = ceilf(val);
    float modulo = fmodf(ceiled, size);
    return modulo <= 0.f ? ceiled - modulo : ceiled + size - modulo;
}

static tm_vec3_t floor_to(tm_vec3_t v, float size)
{
    return (tm_vec3_t) { (float)floor_to_f(v.x, size), (float)floor_to_f(v.y, size), (float)floor_to_f(v.z, size) };
}

static tm_vec3_t ceil_to(tm_vec3_t v, float size)
{
    return (tm_vec3_t) { (float)ceil_to_f(v.x, size), (float)ceil_to_f(v.y, size), (float)ceil_to_f(v.z, size) };
}

#define NATURAL_MAP(x) ((uint64_t)((x) < 0 ? (-(x)-1) * 2 + 1 : (x)*2))
#define NATURAL_PAIR_MAP(x, y) (((x) + (y)) * ((x) + (y) + 1) / 2 + (y))

static uint64_t region_key(tm_vec3_t start, float lod_size, uint64_t lod_i)
{
    uint64_t ux = NATURAL_MAP((int64_t)(start.x / lod_size));
    uint64_t uy = NATURAL_MAP((int64_t)(start.y / lod_size));
    uint64_t uz = NATURAL_MAP((int64_t)(start.z / lod_size));

    return ((NATURAL_PAIR_MAP(NATURAL_PAIR_MAP(ux, uy), uz) << 3) | lod_i) + 1;
}

typedef struct TM_HASH_T(uint64_t, region_data_t) region_data_map_t;

static void add_regions_from_aabb(const aabb_t *aabb, uint8_t lod_i, const tm_vec3_t *exclude_min, const tm_vec3_t *exclude_max, float prev_lod_size, region_data_map_t *out_regions)
{
    float cell_size = LODS[lod_i].size;
    float lod_size = (float)MAG_VOXEL_CHUNK_SIZE * cell_size;
    for (float x = aabb->min.x; x < aabb->max.x; x += lod_size) {
        for (float y = aabb->min.y; y < aabb->max.y; y += lod_size) {
            for (float z = aabb->min.z; z < aabb->max.z; z += lod_size) {
                tm_vec3_t prev_lod_size_vec = { prev_lod_size * (MAG_VOXEL_CHUNK_SIZE), prev_lod_size * (MAG_VOXEL_CHUNK_SIZE), prev_lod_size * (MAG_VOXEL_CHUNK_SIZE) };
                tm_vec3_t cull_min = tm_vec3_add(*exclude_min, prev_lod_size_vec);
                tm_vec3_t cull_max = tm_vec3_sub(*exclude_max, prev_lod_size_vec);
                bool exclude = x >= cull_min.x && y >= cull_min.y && z >= cull_min.z && (x + lod_size) <= cull_max.x && (y + lod_size) <= cull_max.y && (z + lod_size) <= cull_max.z;
                if (exclude)
                    continue;

                region_data_t region = {
                    .pos = { x, y, z },
                    .lod = lod_i,
                    .cull_min = cull_min,
                    .cull_max = cull_max,
                    .key = region_key((tm_vec3_t) { x, y, z }, lod_size, lod_i),
                };
                tm_hash_add(out_regions, region.key, region);
            }
        }
    }
}

static void wanted_regions(tm_vec3_t camera_pos, region_data_map_t *out_regions)
{
    tm_vec3_t prev_min = { 0 };
    tm_vec3_t prev_max = { 0 };
    float prev_lod_size = 0.f;

    for (uint64_t i = 0; i < TM_ARRAY_COUNT(LODS); ++i) {
        tm_vec3_t lod_distance = { LODS[i].distance, LODS[i].distance, LODS[i].distance };
        tm_vec3_t lod_min = floor_to(tm_vec3_sub(camera_pos, lod_distance), (LODS[i].size * (float)MAG_VOXEL_CHUNK_SIZE));
        tm_vec3_t lod_max = ceil_to(tm_vec3_add(camera_pos, lod_distance), (LODS[i].size * (float)MAG_VOXEL_CHUNK_SIZE));

        aabb_t partial_aabbs[6];
        uint32_t aabb_count = 6;
        if (i == 0 || true) {
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
            add_regions_from_aabb(partial_aabbs + ai, (uint8_t)i, &prev_min, &prev_max, prev_lod_size, out_regions);
        }

        prev_min = lod_min;
        prev_max = lod_max;
        prev_lod_size = LODS[i].size;
    }
}

static float properties_ui(struct tm_properties_ui_args_t *args, tm_rect_t item_rect, tm_tt_id_t object);

static tm_ci_editor_ui_i *editor_aspect;
static tm_ci_render_i *render_aspect;
static tm_properties_aspect_i *properties_aspect;
static tm_properties_aspect_i *material_properties_aspect;

static void create_settings_type(struct tm_the_truth_o *tt)
{
    static const tm_the_truth_property_definition_t properties[] = {
        { "materials", TM_THE_TRUTH_PROPERTY_TYPE_SUBOBJECT_SET, .type_hash = MAG_TT_TYPE_HASH__TERRAIN_MATERIAL },
    };

    const tm_tt_type_t settings_type = tm_the_truth_api->create_object_type(tt, MAG_TT_TYPE__TERRAIN_SETTINGS, properties, TM_ARRAY_COUNT(properties));
    tm_tt_set_property_aspect(tt, settings_type, MAG_TT_PROP__TERRAIN_SETTINGS__MATERIALS, tm_tt_prop_aspect__add_remove_subobjects_by_default, (void *)1);
}

static void create_material_type(struct tm_the_truth_o *tt)
{
    static const tm_the_truth_property_definition_t properties[] = {
        [MAG_TT_PROP__TERRAIN_MATERIAL__ORDER] = { "order", TM_THE_TRUTH_PROPERTY_TYPE_DOUBLE },
        [MAG_TT_PROP__TERRAIN_MATERIAL__TEXTURES] = { "textures", TM_THE_TRUTH_PROPERTY_TYPE_SUBOBJECT, .type_hash = TM_TT_TYPE_HASH__CREATION_GRAPH },
        [MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_TOP] = { "allow_from_top", TM_THE_TRUTH_PROPERTY_TYPE_BOOL },
        [MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_SIDES] = { "allow_from_sides", TM_THE_TRUTH_PROPERTY_TYPE_BOOL },
        [MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_MATERIAL] = { "physics_material", TM_THE_TRUTH_PROPERTY_TYPE_REFERENCE, .type_hash = TM_TT_TYPE_HASH__PHYSICS_MATERIAL },
        [MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_COLLISION] = { "physics_collision", TM_THE_TRUTH_PROPERTY_TYPE_REFERENCE, .type_hash = TM_TT_TYPE_HASH__PHYSICS_COLLISION },
    };

    const tm_tt_type_t object_type = tm_the_truth_api->create_object_type(tt, MAG_TT_TYPE__TERRAIN_MATERIAL, properties, TM_ARRAY_COUNT(properties));

    const tm_tt_id_t object = tm_the_truth_api->create_object_of_type(tt, object_type, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_object_o *object_w = tm_the_truth_api->write(tt, object);
    const tm_tt_id_t graph_object = tm_the_truth_api->create_object_of_type(tt, tm_the_truth_api->object_type_from_name_hash(tt, TM_TT_TYPE_HASH__CREATION_GRAPH), TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_api->set_subobject_id(tt, object_w, MAG_TT_PROP__TERRAIN_MATERIAL__TEXTURES, graph_object, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_api->commit(tt, object_w, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_api->set_default_object(tt, object_type, object);

    tm_tt_set_aspect(tt, object_type, tm_properties_aspect_i, material_properties_aspect);
    tm_tt_set_property_aspect(tt, object_type, MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_MATERIAL, tm_tt_prop_aspect__properties__asset_picker, TM_TT_TYPE__PHYSICS_MATERIAL);
    tm_tt_set_property_aspect(tt, object_type, MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_COLLISION, tm_tt_prop_aspect__properties__asset_picker, TM_TT_TYPE__PHYSICS_COLLISION);
}

static void create_truth_types(struct tm_the_truth_o *tt)
{
    create_material_type(tt);
    create_settings_type(tt);

    static const tm_the_truth_property_definition_t voxel_terrain_component_properties[] = {
        { "visibility_flags", TM_THE_TRUTH_PROPERTY_TYPE_SUBOBJECT_SET, .type_hash = TM_TT_TYPE_HASH__VISIBILITY_FLAG },
        { "color", TM_THE_TRUTH_PROPERTY_TYPE_SUBOBJECT, .type_hash = TM_TT_TYPE_HASH__COLOR_RGBA },
        { "settings", TM_THE_TRUTH_PROPERTY_TYPE_SUBOBJECT, .type_hash = MAG_TT_TYPE_HASH__TERRAIN_SETTINGS },
        { "raycast_collision", TM_THE_TRUTH_PROPERTY_TYPE_REFERENCE, .type_hash = TM_TT_TYPE_HASH__PHYSICS_COLLISION },
    };

    const tm_tt_type_t object_type = tm_the_truth_api->create_object_type(tt, MAG_TT_TYPE__TERRAIN_COMPONENT, voxel_terrain_component_properties, TM_ARRAY_COUNT(voxel_terrain_component_properties));

    const tm_tt_id_t component = tm_the_truth_api->create_object_of_type(tt, object_type, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_object_o *component_w = tm_the_truth_api->write(tt, component);
    tm_the_truth_common_types_api->set_color_rgba(tt, component_w, MAG_TT_PROP__TERRAIN_COMPONENT__COLOR, default_values.color_rgba, TM_TT_NO_UNDO_SCOPE);
    const tm_tt_id_t settings = tm_the_truth_api->create_object_of_hash(tt, MAG_TT_TYPE_HASH__TERRAIN_SETTINGS, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_api->set_subobject_id(tt, component_w, MAG_TT_PROP__TERRAIN_COMPONENT__SETTINGS, settings, TM_TT_NO_UNDO_SCOPE);
    tm_the_truth_api->commit(tt, component_w, TM_TT_NO_UNDO_SCOPE);

    tm_the_truth_api->set_default_object(tt, object_type, component);

    tm_tt_set_aspect(tt, object_type, tm_ci_editor_ui_i, editor_aspect);
    tm_tt_set_aspect(tt, object_type, tm_ci_render_i, render_aspect);
    tm_tt_set_aspect(tt, object_type, tm_properties_aspect_i, properties_aspect);
    tm_tt_set_property_aspect(tt, object_type, MAG_TT_PROP__TERRAIN_COMPONENT__RAYCAST_COLLISION, tm_tt_prop_aspect__properties__asset_picker, TM_TT_TYPE__PHYSICS_COLLISION);
}

static void free_terrain_settings(mag_terrain_component_manager_o *man, tm_the_truth_o *tt)
{
    for (mag_terrain_material_t *mat = man->terrain_settings.materials; mat != tm_carray_end(man->terrain_settings.materials); ++mat) {
        if (mat->creation_graph_instance.graph.u64) {
            tm_creation_graph_context_t ctx = {
                .tt = tt,
                .rb = man->backend,
                .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
            };
            tm_creation_graph_api->destroy_instance(&mat->creation_graph_instance, &ctx);
        }
    }
    tm_carray_free(man->terrain_settings.materials, &man->allocator);
    tm_carray_free(man->terrain_settings.normal_maps, &man->allocator);
    tm_carray_free(man->terrain_settings.diffuse_maps, &man->allocator);
    tm_carray_free(man->terrain_settings.orm_maps, &man->allocator);
    man->terrain_settings.materials = NULL;
    man->terrain_settings.normal_maps = NULL;
    man->terrain_settings.diffuse_maps = NULL;
    man->terrain_settings.orm_maps = NULL;
}

static inline int compare_double_inv(const void *a, const void *b)
{
    const double sub = *(const double *)b - *(const double *)a;
    return sub < 0 ? -1 : sub > 0 ? 1
                                  : 0;
}

static bool load_asset(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data, const tm_the_truth_o *tt, tm_tt_id_t asset)
{
    mag_terrain_component_t *c = data;
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;
    const tm_the_truth_object_o *asset_obj = tm_tt_read(tt, asset);

    c->color_rgba = tm_the_truth_common_types_api->get_color_rgba(tt, asset_obj, MAG_TT_PROP__TERRAIN_COMPONENT__COLOR);

    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    const tm_tt_id_t *visibility_flags = tm_the_truth_api->get_subobject_set(tt, asset_obj, MAG_TT_PROP__TERRAIN_COMPONENT__VISIBILITY_FLAGS, ta);
    uint32_t n_visibility_flags = (uint32_t)tm_carray_size(visibility_flags);
    uint32_t *uuid = tm_temp_alloc(ta, n_visibility_flags * sizeof(uint32_t));
    for (uint32_t i = 0; i != n_visibility_flags; ++i)
        uuid[i] = tm_the_truth_api->get_uint32_t(tt, tm_tt_read(tt, visibility_flags[i]), TM_TT_PROP__VISIBILITY_FLAG__UUID);

    tm_visibility_context_o *context = tm_single_implementation(tm_global_api_registry, tm_visibility_context_o);
    c->visibility_mask = tm_visibility_flags_api->build_visibility_mask(context, uuid, n_visibility_flags);

    man->raycast_collision_id = tm_the_truth_api->get_reference(tt, asset_obj, MAG_TT_PROP__TERRAIN_COMPONENT__RAYCAST_COLLISION);

    const tm_tt_id_t settings_id = tm_the_truth_api->get_subobject(tt, asset_obj, MAG_TT_PROP__TERRAIN_COMPONENT__SETTINGS);
    tm_the_truth_o *truth = tm_entity_api->the_truth(man->ctx);
    if (settings_id.u64 && man->terrain_settings_id.u64 != settings_id.u64) {
        free_terrain_settings(man, truth);
        man->terrain_settings_id = settings_id;

        const tm_the_truth_object_o *settings_obj = tm_tt_read(tt, settings_id);
        const tm_tt_id_t *materials = tm_the_truth_api->get_subobject_set(tt, settings_obj, MAG_TT_PROP__TERRAIN_SETTINGS__MATERIALS, ta);

        double *orders = NULL;
        uint64_t material_count = tm_carray_size(materials);
        tm_carray_temp_resize(orders, material_count, ta);

        // collect array of structs first to sort it, then split into struct of arrays
        typedef struct merged_material_t
        {
            tm_renderer_handle_t diffuse_map;
            tm_renderer_handle_t normal_map;
            tm_renderer_handle_t orm_map;
            mag_terrain_material_t material;
        } merged_material_t;
        merged_material_t *merged_materials = 0;
        tm_carray_temp_resize(merged_materials, material_count, ta);
        for (uint64_t im = 0; im < material_count; ++im) {
            merged_materials[im] = (merged_material_t) { 0 };

            const tm_the_truth_object_o *mat_obj = tm_tt_read(tt, materials[im]);
            orders[im] = tm_the_truth_api->get_double(tt, mat_obj, MAG_TT_PROP__TERRAIN_MATERIAL__ORDER);
            const tm_tt_id_t graph_asset = tm_the_truth_api->get_subobject(tt, mat_obj, MAG_TT_PROP__TERRAIN_MATERIAL__TEXTURES);
            merged_materials[im].material.allow_from_top = tm_the_truth_api->get_bool(tt, mat_obj, MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_TOP);
            merged_materials[im].material.allow_from_sides = tm_the_truth_api->get_bool(tt, mat_obj, MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_SIDES);

            const tm_tt_id_t physics_material_id = tm_the_truth_api->get_reference(tt, mat_obj, MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_MATERIAL);

            if (physics_material_id.u64) {
                const tm_the_truth_object_o *physics_material = tm_tt_read(tt, physics_material_id);

                merged_materials[im].material.static_friction = tm_the_truth_api->get_float(tt, physics_material, TM_TT_PROP__PHYSICS_MATERIAL__STATIC_FRICTION);
                merged_materials[im].material.dynamic_friction = tm_the_truth_api->get_float(tt, physics_material, TM_TT_PROP__PHYSICS_MATERIAL__DYNAMIC_FRICTION);
                merged_materials[im].material.restitution = tm_the_truth_api->get_float(tt, physics_material, TM_TT_PROP__PHYSICS_MATERIAL__RESTITUTION);
            }
            merged_materials[im].material.collision_id = tm_the_truth_api->get_reference(tt, mat_obj, MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_COLLISION);

            tm_creation_graph_context_t cg_ctx = (tm_creation_graph_context_t) { .rb = man->backend, .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, .tt = truth };
            merged_materials[im].material.creation_graph_instance = tm_creation_graph_api->create_instance(truth, graph_asset, &cg_ctx);

            tm_creation_graph_output_t images = tm_creation_graph_api->output(&merged_materials[im].material.creation_graph_instance, TM_CREATION_GRAPH__IMAGE__OUTPUT_NODE_HASH, &cg_ctx, 0);
            const tm_creation_graph_image_data_t *image_data = (tm_creation_graph_image_data_t *)images.output;
            const uint32_t num_images = images.num_output_objects;

            for (uint32_t i = 0; i != num_images; ++i) {
                if (TM_STRHASH_EQUAL(image_data[i].resource_name, TM_STATIC_HASH("diffuse", 0x4ab99823c979a20aULL)))
                    merged_materials[im].diffuse_map = image_data[i].handle;
                else if (TM_STRHASH_EQUAL(image_data[i].resource_name, TM_STATIC_HASH("normal", 0xcaed6cd644ec6ba7ULL)))
                    merged_materials[im].normal_map = image_data[i].handle;
                else if (TM_STRHASH_EQUAL(image_data[i].resource_name, TM_STATIC_HASH("orm", 0x8655079c80c7ddf9ULL)))
                    merged_materials[im].orm_map = image_data[i].handle;
            }
        }
        if (merged_materials) {
            qsort_by(merged_materials, material_count, sizeof(*merged_materials),
                orders, sizeof(*orders), compare_double_inv);
        }

        tm_carray_ensure(man->terrain_settings.materials, material_count, &man->allocator);
        tm_carray_ensure(man->terrain_settings.diffuse_maps, material_count, &man->allocator);
        tm_carray_ensure(man->terrain_settings.orm_maps, material_count, &man->allocator);
        tm_carray_ensure(man->terrain_settings.normal_maps, material_count, &man->allocator);
        for (merged_material_t *mat = merged_materials; mat != tm_carray_end(merged_materials); ++mat) {
            tm_carray_push(man->terrain_settings.materials, mat->material, &man->allocator);
            tm_carray_push(man->terrain_settings.diffuse_maps, mat->diffuse_map, &man->allocator);
            tm_carray_push(man->terrain_settings.orm_maps, mat->orm_map, &man->allocator);
            tm_carray_push(man->terrain_settings.normal_maps, mat->normal_map, &man->allocator);
        }

        if (material_count && !tm_entity_api->get_blackboard_double(man->ctx, TM_ENTITY_BB__EDITOR, 0)) {
            uint32_t *aspect_flags = tm_carray_create(uint32_t, material_count, a);
            for (uint32_t *flag = aspect_flags; flag != tm_carray_end(aspect_flags); ++flag) {
                *flag = TM_RENDERER_IMAGE_ASPECT_SRGB;
            }
            tm_renderer_resource_command_buffer_o *res_buf;
            man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);

            tm_shader_io_o *io = tm_shader_api->system_io(man->material_system);
            set_resource(io, res_buf, &man->material_rbinder, TM_STATIC_HASH("diffuse_map", 0x3aa8b87edcc9a470ULL), man->terrain_settings.diffuse_maps, aspect_flags, 0, (uint32_t)material_count);
            set_resource(io, res_buf, &man->material_rbinder, TM_STATIC_HASH("normal_map", 0xf5c97d31c5c8a1e1ULL), man->terrain_settings.normal_maps, 0, 0, (uint32_t)material_count);
            set_resource(io, res_buf, &man->material_rbinder, TM_STATIC_HASH("orm_map", 0x6d998fb1680c0e50ULL), man->terrain_settings.orm_maps, 0, 0, (uint32_t)material_count);

            man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
            man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);
        }
    }

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
    return true;
}

static aabb_t adjust_aabb_to_lod(const aabb_t *aabb, float lod_size)
{
    return (aabb_t) { floor_to(aabb->min, lod_size), ceil_to(aabb->max, lod_size) };
}

static void apply_op_to_component(mag_terrain_component_manager_o *man, region_task_buffers_t *task_buffers, const mag_terrain_component_buffers_t *c, const region_data_t *region_data, const op_t *op, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    uint32_t op_type = op->type;
    uint32_t op_primitive = op->primitive;

    tm_shader_io_o *io = tm_shader_api->shader_io(man->apply_op_shader);

    // TODO: cache these in the op?
    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuf;
    tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
    tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);

    set_constant(io, res_buf, &cbuf, TM_STATIC_HASH("region_pos", 0x5af0fcabdb39700fULL), &region_data->pos, sizeof(region_data->pos));
    set_constant(io, res_buf, &cbuf, TM_STATIC_HASH("cell_size", 0x50b5f09b4c1a94fdULL), &LODS[region_data->lod].size, sizeof(LODS[region_data->lod].size));
    set_constant(io, res_buf, &cbuf, TM_STATIC_HASH("operation", 0xd2d6e5b89b8e7927ULL), &op_type, sizeof(op_type));
    set_constant(io, res_buf, &cbuf, TM_STATIC_HASH("primitive_type", 0xe29996cba9a2d434ULL), &op_primitive, sizeof(op_primitive));
    set_constant(io, res_buf, &cbuf, TM_STATIC_HASH("primitive_position", 0x2b8f053bb3f9250cULL), &op->pos, sizeof(op->pos));
    set_constant(io, res_buf, &cbuf, TM_STATIC_HASH("primitive_size", 0xccd2986a9d8d1bb1ULL), &op->radius, sizeof(op->radius));
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("densities", 0x9d97839d5465b483ULL), &c->densities_handle, 0, 0, 1);

    tm_renderer_shader_info_t shader_info;
    tm_shader_api->assemble_shader_infos(man->apply_op_shader, 0, 0, NULL, TM_STRHASH(0), res_buf, &cbuf, &rbinder, 1, &shader_info);
    tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { 1, MAG_VOXEL_REGION_SIZE + 1, MAG_VOXEL_REGION_SIZE + 1 } }, &shader_info, 1);
    ++*sort_key;

    tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);
    tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->densities_handle, .source_state = state, .destination_state = state }, 1);
    ++*sort_key;
}

static void init_region_info_buffer(const mag_terrain_component_buffers_t *c, tm_renderer_resource_command_buffer_o *res_buf)
{
    gpu_region_info_t *info;
    tm_renderer_api->tm_renderer_resource_command_buffer_api->update_buffer(res_buf,
        c->region_info_handle, 0, sizeof(*info),
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, 0, &info);
    *info = (gpu_region_info_t) {
        .max_x = 0,
        .max_y = 0,
        .max_z = 0,
        .min_x = (REGION_INFO_BOUNDS_OFFSET + MAG_VOXEL_REGION_SIZE) * REGION_INFO_BOUNDS_SCALE,
        .min_y = (REGION_INFO_BOUNDS_OFFSET + MAG_VOXEL_REGION_SIZE) * REGION_INFO_BOUNDS_SCALE,
        .min_z = (REGION_INFO_BOUNDS_OFFSET + MAG_VOXEL_REGION_SIZE) * REGION_INFO_BOUNDS_SCALE,
    };
}

static void dc_octree_create(mag_terrain_component_manager_o *man, region_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, const region_data_t *region_data, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    tm_renderer_api->tm_renderer_resource_command_buffer_api->fill_buffer(res_buf, buffers->octree_handle, 0, OCTREE_NODE_SIZE * octree_node_count(OCTREE_DEPTH - 1), 0, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);

    tm_renderer_shader_info_t shader_info;
    tm_shader_api->assemble_shader_infos(man->octree_create_shader, 0, 0, buffers->octree_context, TM_STRHASH(0), res_buf, NULL, NULL, 1, &shader_info);
    tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { 1, MAG_VOXEL_REGION_SIZE, MAG_VOXEL_REGION_SIZE } }, &shader_info, 1);
    *sort_key += 1;

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = buffers->octree_handle, .source_state = state, .destination_state = state }, 1);
    *sort_key += 1;
}

static void dc_octree_collapse(mag_terrain_component_manager_o *man, region_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, const region_data_t *region_data, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    tm_renderer_shader_info_t shader_infos[TM_ARRAY_COUNT(buffers->octree_collapse_cbufs)];

    uint32_t nodes_on_level = 1;
    for (uint32_t i = 0; i < TM_ARRAY_COUNT(buffers->octree_collapse_cbufs) - 1; ++i) {
        nodes_on_level *= 8;
    }

    // TODO: reduce by one dispatch
    tm_shader_api->assemble_shader_infos(man->octree_collapse_shader, 0, 0, buffers->octree_context, TM_STRHASH(0), res_buf, buffers->octree_collapse_cbufs, NULL, TM_ARRAY_COUNT(shader_infos), shader_infos);

    for (uint32_t depth = TM_ARRAY_COUNT(shader_infos); depth > 0; --depth) {
        uint32_t parent_depth = depth - 1;
        tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { nodes_on_level, 1, 1 } }, shader_infos + parent_depth, 1);
        *sort_key += 1;
        nodes_on_level /= 8;

        uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
        tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = buffers->collapsed_octree_handle, .source_state = state, .destination_state = state }, 1);
        *sort_key += 1;
    }
}

static void dc_octree_contour(mag_terrain_component_manager_o *man, region_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, const region_data_t *region_data, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    tm_renderer_shader_info_t shader_info;
    tm_shader_api->assemble_shader_infos(man->octree_contour_shader, NULL, 0, buffers->octree_context, TM_STRHASH(0), res_buf, NULL, NULL, 1, &shader_info);
    tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { OCTREE_EDGE_COUNT / 2, 1, 1 } }, &shader_info, 1);
    *sort_key += 1;

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->ibuf, .source_state = state, .destination_state = state }, 1);
    *sort_key += 1;
}

static void generate_mesh(mag_terrain_component_manager_o *man, region_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, const region_data_t *region_data, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    init_region_info_buffer(c, res_buf);

    tm_shader_system_api->activate_system(buffers->octree_context, man->region_contouring_system, &c->region_contouring_cbuf, 1, &c->region_contouring_rbinder, 1);
    dc_octree_create(man, buffers, c, region_data, cmd_buf, res_buf, sort_key);
    dc_octree_collapse(man, buffers, c, region_data, cmd_buf, res_buf, sort_key);
    dc_octree_contour(man, buffers, c, region_data, cmd_buf, res_buf, sort_key);
    tm_shader_system_api->deactivate_system(buffers->octree_context, man->region_contouring_system);
}

static void add(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data)
{
    mag_terrain_component_t *c = data;
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;

    tm_visibility_context_o *context = tm_single_implementation(tm_global_api_registry, tm_visibility_context_o);
    c->visibility_mask = tm_visibility_flags_api->build_visibility_mask(context, 0, 0);

    tm_renderer_resource_command_buffer_o *res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);

    c->buffers = tm_alloc(&man->allocator, sizeof(*c->buffers));
    *c->buffers = (mag_terrain_component_buffers_t) { 0 };

    const uint32_t CORNERS_IN_REGION = (MAG_VOXEL_REGION_SIZE + 1) * (MAG_VOXEL_REGION_SIZE + 1) * (MAG_VOXEL_REGION_SIZE + 1);
    c->buffers->densities_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = 4 * sizeof(uint16_t) * CORNERS_IN_REGION, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_densities" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
    c->buffers->region_info_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = sizeof(gpu_region_info_t), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_UPDATABLE, .debug_tag = "mag_region_info" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
    c->buffers->vertices_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = 6 * sizeof(uint16_t) * MAX_VERTICES_PER_REGION, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_ACCELERATION_STRUCTURE, .debug_tag = "mag_region_vertices" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
    c->buffers->ibuf = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = sizeof(uint16_t) * MAX_INDICES_PER_REGION, .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV | TM_RENDERER_BUFFER_USAGE_ACCELERATION_STRUCTURE | TM_RENDERER_BUFFER_USAGE_INDEX, .debug_tag = "mag_region_triangles" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);

    {
        tm_shader_io_o *io = tm_shader_api->system_io(man->region_contouring_system);
        tm_shader_api->create_resource_binder_instances(io, 1, &c->buffers->region_contouring_rbinder);
        tm_shader_api->create_constant_buffer_instances(io, 1, &c->buffers->region_contouring_cbuf);

        set_resource(io, res_buf, &c->buffers->region_contouring_rbinder, TM_STATIC_HASH("densities", 0x9d97839d5465b483ULL), &c->buffers->densities_handle, 0, 0, 1);
        set_resource(io, res_buf, &c->buffers->region_contouring_rbinder, TM_STATIC_HASH("region_info", 0x5385edbb61c5ae2bULL), &c->buffers->region_info_handle, 0, 0, 1);
        set_resource(io, res_buf, &c->buffers->region_contouring_rbinder, TM_STATIC_HASH("triangles", 0x72976bf8d13d4449ULL), &c->buffers->ibuf, 0, 0, 1);
        set_resource(io, res_buf, &c->buffers->region_contouring_rbinder, TM_STATIC_HASH("vertices", 0x3288dd4327525f9aULL), &c->buffers->vertices_handle, 0, 0, 1);
    }

    {
        tm_shader_io_o *io = tm_shader_api->system_io(man->region_render_system);
        tm_shader_api->create_resource_binder_instances(io, 1, &c->region_render_rbinder);
        tm_shader_api->create_constant_buffer_instances(io, 1, &c->region_render_cbuf);

        set_resource(io, res_buf, &c->region_render_rbinder, TM_STATIC_HASH("vertices", 0x3288dd4327525f9aULL), &c->buffers->vertices_handle, 0, 0, 1);
    }

    man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);

    c->physics_data = tm_alloc(&man->allocator, sizeof(*c->physics_data));
    *c->physics_data = (generate_physics_task_data_t) {
        .man = man,
        .tt = tm_entity_api->the_truth(man->ctx),
        .physics_component = {
            .shape = TM_PHYSICS_SHAPE__MESH,
            .offset = {
                .scl = (tm_vec3_t) { 1, 1, 1 },
                .rot = (tm_vec4_t) { 0, 0, 0, 1 },
            },
        },
    };
    if (tm_carray_size(man->terrain_settings.materials)) {
        c->physics_data->physics_component.collision_id = man->terrain_settings.materials[0].collision_id;
        c->physics_data->physics_component.material = (tm_physics_shape_material_t) {
            .static_friction = man->terrain_settings.materials[0].static_friction,
            .dynamic_friction = man->terrain_settings.materials[0].dynamic_friction,
            .restitution = man->terrain_settings.materials[0].restitution,
        };
    }
    tm_os_api->thread->create_critical_section(&c->physics_data->cs);
}

static void destroy_mesh(mag_terrain_component_buffers_t *c, tm_renderer_resource_command_buffer_o *res_buf, mag_terrain_component_manager_o *man)
{
    if (c->vertices_handle.resource) {
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->ibuf);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->vertices_handle);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->densities_handle);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->region_info_handle);

        c->vertices_handle = (tm_renderer_handle_t) { 0 };
        c->ibuf = (tm_renderer_handle_t) { 0 };
        c->densities_handle = (tm_renderer_handle_t) { 0 };
        c->region_info_handle = (tm_renderer_handle_t) { 0 };
    }
}

static void remove(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data)
{
    mag_terrain_component_t *c = data;
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;

    if (c->generate_task_id) {
        while (!mag_async_gpu_queue_api->is_task_done(man->gpu_queue, c->generate_task_id))
            ;
    }

    if (c->read_mesh_task_id) {
        while (!mag_async_gpu_queue_api->is_task_done(man->gpu_queue, c->read_mesh_task_id))
            ;
        RELEASE_BUFFERS(man->read_mesh_task_buffers_locks, c->buffers->read_mesh_task_buffers_id);
    }

    tm_renderer_resource_command_buffer_o *res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);
    if (c->buffers->generate_fence) {
        tm_renderer_handle_t handle = { .resource = c->buffers->generate_fence };
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, handle);
    }
    destroy_mesh(c->buffers, res_buf, man);

    {
        tm_shader_io_o *io = tm_shader_api->system_io(man->region_contouring_system);
        tm_shader_api->destroy_resource_binder_instances(io, &c->buffers->region_contouring_rbinder, 1);
        tm_shader_api->destroy_constant_buffer_instances(io, &c->buffers->region_contouring_cbuf, 1);
    }
    {
        tm_shader_io_o *io = tm_shader_api->system_io(man->region_render_system);
        tm_shader_api->destroy_resource_binder_instances(io, &c->region_render_rbinder, 1);
        tm_shader_api->destroy_constant_buffer_instances(io, &c->region_render_cbuf, 1);
    }

    man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);

    tm_free(&man->allocator, c->buffers, sizeof(*c->buffers));

    uint64_t cur_task_id = atomic_exchange_uint64_t(&c->physics_data->current_task_id, 0);
    if (cur_task_id) {
        tm_task_system_api->cancel_task(cur_task_id);
        // entering and leaving the section guarantees that the task will not touch the fields,
        // because the task checks the ID not to be 0
        TM_OS_ENTER_CRITICAL_SECTION(&c->physics_data->cs);
        TM_OS_LEAVE_CRITICAL_SECTION(&c->physics_data->cs);
        if (c->physics_data->read_mesh_task_buffers_id)
            RELEASE_BUFFERS(man->read_mesh_task_buffers_locks, c->physics_data->read_mesh_task_buffers_id);
    }

    if (c->physics_data->buffer_id) {
        tm_the_truth_o *tt = tm_entity_api->the_truth(man->ctx);
        tm_buffers_i *buffers = tm_the_truth_api->buffers(tt);
        buffers->release(buffers->inst, c->physics_data->buffer_id);
    }

    tm_os_api->thread->destroy_critical_section(&c->physics_data->cs);

    tm_free(&man->allocator, c->physics_data, sizeof(*c->physics_data));
}

static void destroy(tm_component_manager_o *manager)
{
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;

    const tm_component_type_t terrain_component = tm_entity_api->lookup_component_type(man->ctx, MAG_TT_TYPE_HASH__TERRAIN_COMPONENT);
    tm_entity_api->call_remove_on_all_entities(man->ctx, terrain_component);

    free_terrain_settings(man, tm_entity_api->the_truth(man->ctx));

    if (!tm_entity_api->get_blackboard_double(man->ctx, TM_ENTITY_BB__EDITOR, 0)) {
        mag_async_gpu_queue_api->destroy(man->gpu_queue);

        tm_slab_destroy(man->ops);
        tm_hash_free(&man->component_map);
        tm_set_free(&man->empty_regions);

        tm_renderer_resource_command_buffer_o *res_buf;
        man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);

        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, man->precomputed_octree_edges_handle);

        for (uint32_t i = 0; i < MAX_TASK_BUFFERS; ++i) {
            destroy_read_mesh_task_buffers(man->read_mesh_task_buffers + i, res_buf, &man->allocator);
            destroy_region_task_buffers(man, man->region_task_buffers + i, res_buf, &man->allocator);
        }

        man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
        man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);

        tm_shader_api->destroy_resource_binder_instances(tm_shader_api->system_io(man->material_system), &man->material_rbinder, 1);
    }

    tm_entity_context_o *ctx = man->ctx;
    tm_allocator_i a = man->allocator;
    tm_free(&a, man, sizeof(*man));
    tm_entity_api->destroy_child_allocator(ctx, &a);
}

static void components_created(tm_component_manager_o *manager)
{
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;
    man->rigid_static_component_type = tm_entity_api->lookup_component_type(man->ctx, TM_TT_TYPE_HASH__PHYSX_RIGID_STATIC_COMPONENT);
    man->physics_shape_component_type = tm_entity_api->lookup_component_type(man->ctx, TM_TT_TYPE_HASH__PHYSICS_SHAPE_COMPONENT);
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
        .terrain_settings = { 0 },

        .region_contouring_system = tm_shader_repository_api->lookup_system(shader_repo, TM_STATIC_HASH("magnum_terrain_region_contouring_system", 0x86edb0e840e34f8dULL)),
        .region_render_system = tm_shader_repository_api->lookup_system(shader_repo, TM_STATIC_HASH("magnum_terrain_region_render_system", 0xa07fdbf37fa448a2ULL)),
        .material_system = tm_shader_repository_api->lookup_system(shader_repo, TM_STATIC_HASH("magnum_terrain_material_system", 0x9770ee95243e8af6ULL)),
        .gen_region_shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_terrain_gen_region", 0x3f8b44db04e9fd19ULL)),
        .octree_create_shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_octree_create", 0x6f541ac0ad78aec8ULL)),
        .octree_collapse_shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_octree_collapse", 0xd635c539960e45aeULL)),
        .octree_contour_shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_octree_contour", 0x20b73a462ae9e28dULL)),
        .apply_op_shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_terrain_operations", 0x4e0fc94d36751938ULL)),
    };

    if (!tm_entity_api->get_blackboard_double(ctx, TM_ENTITY_BB__EDITOR, 0)) {
        manager->component_map.allocator = &manager->allocator;
        manager->empty_regions.allocator = &manager->allocator;

        mag_async_gpu_queue_params_t params = {
            .max_simultaneous_tasks = MAX_ASYNC_GPU_TASKS,
            .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
        };
        manager->gpu_queue = mag_async_gpu_queue_api->create(&manager->allocator, backend, &params);
        tm_slab_create(&manager->ops, &manager->allocator, 64 * 1024);
        manager->last_empty_check_op = manager->ops;

        tm_renderer_resource_command_buffer_o *res_buf;
        manager->backend->create_resource_command_buffers(manager->backend->inst, &res_buf, 1);

        uint32_t *edges;
        manager->precomputed_octree_edges_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->map_create_buffer(res_buf,
            &(tm_renderer_buffer_desc_t) { .size = sizeof(OCTREE_EDGES), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE, .debug_tag = "mag_precomputed_octree_edges" },
            TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, 0, &edges);
        memcpy(edges, OCTREE_EDGES, sizeof(OCTREE_EDGES));

        for (uint32_t i = 0; i < MAX_TASK_BUFFERS; ++i) {
            init_region_task_buffers(manager, manager->region_task_buffers + i, manager->shader_repo, res_buf, &manager->allocator);
            init_read_mesh_task_buffers(manager->read_mesh_task_buffers + i, manager->shader_repo, res_buf, &manager->allocator);
        }

        {
            tm_shader_io_o *io = tm_shader_api->system_io(manager->material_system);
            tm_shader_api->create_resource_binder_instances(io, 1, &manager->material_rbinder);
        }

        manager->backend->submit_resource_command_buffers(manager->backend->inst, &res_buf, 1);
        manager->backend->destroy_resource_command_buffers(manager->backend->inst, &res_buf, 1);
    }

    const tm_component_i component = {
        .name = MAG_TT_TYPE__TERRAIN_COMPONENT,
        .bytes = sizeof(mag_terrain_component_t),
        .default_data = &default_values,
        .manager = (tm_component_manager_o *)manager,
        .load_asset = load_asset,
        .add = add,
        .remove = remove,
        .destroy = destroy,
        .components_created = components_created,
    };

    tm_entity_api->register_component(ctx, &component);
}

static float material_properties_ui(struct tm_properties_ui_args_t *args, tm_rect_t item_rect, tm_tt_id_t object)
{
    item_rect.y = tm_properties_view_api->ui_subobject(args, item_rect, TM_LOCALIZE("Creation Graph"), NULL, object, MAG_TT_PROP__TERRAIN_MATERIAL__TEXTURES, false);
    item_rect.y = tm_properties_view_api->ui_bool(args, item_rect, TM_LOCALIZE("Allow From Top"), NULL, object, MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_TOP);
    item_rect.y = tm_properties_view_api->ui_bool(args, item_rect, TM_LOCALIZE("Allow From Sides"), NULL, object, MAG_TT_PROP__TERRAIN_MATERIAL__ALLOW_FROM_SIDES);
    item_rect.y = tm_properties_view_api->ui_property(args, item_rect, object, MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_MATERIAL);
    item_rect.y = tm_properties_view_api->ui_property(args, item_rect, object, MAG_TT_PROP__TERRAIN_MATERIAL__PHYSICS_COLLISION);

    return item_rect.y;
}

static float properties_ui(struct tm_properties_ui_args_t *args, tm_rect_t item_rect, tm_tt_id_t object)
{
    const tm_the_truth_object_o *obj = tm_tt_read(args->tt, object);
    tm_tt_id_t settings = tm_the_truth_api->get_subobject(args->tt, obj, MAG_TT_PROP__TERRAIN_COMPONENT__SETTINGS);
    if (!settings.u64) {
        tm_the_truth_object_o *obj_w = tm_the_truth_api->write(args->tt, object);
        settings = tm_the_truth_api->create_object_of_hash(args->tt, MAG_TT_TYPE_HASH__TERRAIN_SETTINGS, TM_TT_NO_UNDO_SCOPE);
        tm_the_truth_api->set_subobject_id(args->tt, obj_w, MAG_TT_PROP__TERRAIN_COMPONENT__SETTINGS, settings, TM_TT_NO_UNDO_SCOPE);
        tm_the_truth_api->commit(args->tt, obj_w, TM_TT_NO_UNDO_SCOPE);
    }

    const tm_tt_id_t color = tm_the_truth_api->get_subobject(args->tt, obj, MAG_TT_PROP__TERRAIN_COMPONENT__COLOR);
    item_rect.y = tm_properties_view_api->ui_color_picker(args, item_rect, TM_LOCALIZE("Color"), NULL, color);
    item_rect.y = tm_properties_view_api->ui_property(args, item_rect, object, MAG_TT_PROP__TERRAIN_COMPONENT__RAYCAST_COLLISION);

    item_rect.y = tm_properties_view_api->ui_visibility_flags(args, item_rect, TM_LOCALIZE("Visibility Flags"), NULL, object, MAG_TT_PROP__TERRAIN_COMPONENT__VISIBILITY_FLAGS);
    item_rect.y = tm_properties_view_api->ui_subobject_set_reorderable(args, item_rect, TM_LOCALIZE("Materials"), NULL, settings, MAG_TT_PROP__TERRAIN_SETTINGS__MATERIALS, MAG_TT_PROP__TERRAIN_MATERIAL__ORDER);

    // const tm_rect_t add_button_r = { item_rect.x, item_rect.y, item_rect.h, item_rect.h };
    // if (tm_ui_api->button(args->ui, args->uistyle, &(tm_ui_button_t) { .icon = TM_UI_ICON__ADD, .rect = add_button_r })) {
    //     const tm_tt_undo_scope_t undo_scope = tm_the_truth_api->create_undo_scope(args->tt, TM_LOCALIZE("Add Material"));
    //     tm_the_truth_object_o *object_w = tm_the_truth_api->write(args->tt, object);

    //     const tm_tt_id_t settings_id = tm_the_truth_api->get_subobject(args->tt, object_w, MAG_TT_PROP__TERRAIN_COMPONENT__SETTINGS);
    //     tm_the_truth_object_o *settings_w = tm_the_truth_api->write(args->tt, settings_id);

    //     // TODO: order
    //     const tm_tt_id_t new_material = tm_the_truth_api->create_object_of_type(args->tt, tm_the_truth_api->object_type_from_name_hash(args->tt, MAG_TT_TYPE_HASH__TERRAIN_MATERIAL), undo_scope);
    //     tm_the_truth_object_o *new_material_w = tm_the_truth_api->write(args->tt, new_material);
    //     tm_the_truth_api->add_to_subobject_set(args->tt, object_w, MAG_TT_PROP__TERRAIN_SETTINGS__MATERIALS, &new_material_w, 1);
    //     tm_the_truth_api->commit(args->tt, new_material_w, undo_scope);
    //     tm_the_truth_api->commit(args->tt, settings_w, undo_scope);
    //     tm_the_truth_api->commit(args->tt, object_w, undo_scope);
    //     args->undo_stack->add(args->undo_stack->inst, args->tt, undo_scope);
    // }
    // item_rect.y += add_button_r.h;

    return item_rect.y;
}

static const char *get_type_display_name(void)
{
    return TM_LOCALIZE("Voxel Terrain Component");
}

static const char *get_material_type_display_name(void)
{
    return TM_LOCALIZE("Voxel Terrain Material");
}

static void generate_sdf(mag_terrain_component_manager_o *man, region_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, const region_data_t *region_data, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key)
{
    tm_shader_io_o *io = tm_shader_api->shader_io(man->gen_region_shader);
    set_constant(io, res_buf, &buffers->gen_region_cbuf, TM_STATIC_HASH("region_pos", 0x5af0fcabdb39700fULL), &region_data->pos, sizeof(region_data->pos));
    set_constant(io, res_buf, &buffers->gen_region_cbuf, TM_STATIC_HASH("cell_size", 0x50b5f09b4c1a94fdULL), &LODS[region_data->lod].size, sizeof(LODS[region_data->lod].size));
    set_resource(io, res_buf, &buffers->gen_region_rbinder, TM_STATIC_HASH("densities", 0x9d97839d5465b483ULL), &c->densities_handle, 0, 0, 1);

    tm_renderer_shader_info_t shader_info;
    tm_shader_api->assemble_shader_infos(man->gen_region_shader, 0, 0, buffers->gen_region_context, TM_STRHASH(0), res_buf, &buffers->gen_region_cbuf, &buffers->gen_region_rbinder, 1, &shader_info);
    tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { 1, MAG_VOXEL_REGION_SIZE + 1, MAG_VOXEL_REGION_SIZE + 1 } }, &shader_info, 1);
    *sort_key += 1;

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = c->densities_handle, .source_state = state, .destination_state = state }, 1);
    *sort_key += 1;
}

static void generate_region_gpu(tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key, mag_terrain_component_manager_o *man, mag_terrain_component_buffers_t *c, const region_data_t *region_data, const op_t *ops, const op_t *last_op)
{
    if (!c->gen_region_task_buffers_id) {
        LOCK_BUFFERS(man->region_task_buffers_locks, c->gen_region_task_buffers_id);
    }
    region_task_buffers_t *task_buffers = GET_BUFFERS(man->region_task_buffers, c->gen_region_task_buffers_id);

    generate_sdf(man, task_buffers, c, region_data, cmd_buf, res_buf, sort_key);
    const aabb_t region_aabb = region_aabb_with_margin(region_data);
    for (const op_t *op = ops; op != last_op; op = tm_slab_next(op)) {
        if (!tm_slab_is_valid(op))
            continue;
        const aabb_t aabb = op_aabb(op);
        if (aabb_intersect(&region_aabb, &aabb))
            apply_op_to_component(man, task_buffers, c, region_data, op, cmd_buf, res_buf, sort_key);
    }
    generate_mesh(man, task_buffers, c, region_data, cmd_buf, res_buf, sort_key);
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
        if (c->generate_task_id || !c->buffers->region_info.num_indices) {
            dest->visibility_mask = 0;
            dest->min = (tm_vec3_t) { 0 };
            dest->max = (tm_vec3_t) { 0 };
        } else {
            const gpu_region_info_t *info = &c->buffers->region_info;
            tm_vec3_t min = {
                (float)info->min_x / (float)REGION_INFO_BOUNDS_SCALE - (float)REGION_INFO_BOUNDS_OFFSET,
                (float)info->min_y / (float)REGION_INFO_BOUNDS_SCALE - (float)REGION_INFO_BOUNDS_OFFSET,
                (float)info->min_z / (float)REGION_INFO_BOUNDS_SCALE - (float)REGION_INFO_BOUNDS_OFFSET,
            };
            tm_vec3_t max = {
                (float)info->max_x / (float)REGION_INFO_BOUNDS_SCALE - (float)REGION_INFO_BOUNDS_OFFSET,
                (float)info->max_y / (float)REGION_INFO_BOUNDS_SCALE - (float)REGION_INFO_BOUNDS_OFFSET,
                (float)info->max_z / (float)REGION_INFO_BOUNDS_SCALE - (float)REGION_INFO_BOUNDS_OFFSET,
            };

            dest->tm = *tm_mat44_identity();
            dest->visibility_mask = c->visibility_mask;
            dest->min = tm_vec3_add(c->region_data.pos, tm_vec3_mul(min, LODS[c->region_data.lod].size));
            dest->max = tm_vec3_add(c->region_data.pos, tm_vec3_mul(max, LODS[c->region_data.lod].size));
        }

        ++dest;
    }

    TM_PROFILER_END_FUNC_SCOPE();
}

static void free_read_mesh_task_data(void *data)
{
    read_mesh_task_data_t *task_data = (read_mesh_task_data_t *)data;
    tm_free(&task_data->man->allocator, task_data, sizeof(*task_data));
}

static void handle_generate_task_completion(mag_terrain_component_manager_o *man, mag_terrain_component_t *component, tm_renderer_resource_command_buffer_o *res_buf)
{
    component->generate_task_id = 0;
    component->last_applied_op = component->last_task_op;

    if (component->buffers->region_info.num_indices && LODS[component->region_data.lod].needs_physics) {
        read_mesh_task_data_t *task_data;
        task_data = tm_alloc(&man->allocator, sizeof(*task_data));

        *task_data = (read_mesh_task_data_t) {
            .c = component->buffers,
            .man = man,
        };

        mag_async_gpu_queue_task_params_t params = {
            .f = read_mesh_task,
            .data = task_data,
            .cancel_callback = free_read_mesh_task_data,
            .completion_callback = free_read_mesh_task_data,
            .priority = 0,
        };
        component->read_mesh_task_id = mag_async_gpu_queue_api->submit_task(man->gpu_queue, &params);
    }
}

#define REGION_LOD_BITS 3
#define REGION_LOD_BITS_START (TM_RENDER_GRAPH_SORT_DEPTH_BITS_START + (TM_RENDER_GRAPH_SORT_DEPTH_BITS - REGION_LOD_BITS))

static inline uint64_t region_depth_sort_key(uint16_t depth, uint8_t lod)
{
    return (((uint64_t)depth) << TM_RENDER_GRAPH_SORT_DEPTH_BITS_START) | (((uint64_t)lod) << REGION_LOD_BITS_START);
}

static void magnum_terrain_render(struct tm_component_manager_o *manager, struct tm_render_args_t *args, const tm_ci_render_viewer_t *viewers,
    uint32_t num_viewers, const tm_entity_t *entities, const tm_transform_component_t *entity_transforms, const bool *entity_selection_state, const uint32_t *entity_indices,
    void **render_component_data, uint32_t num_renderables, const uint8_t *frustum_visibility)
{
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)manager;

#include <plugins/the_machinery_shared/viewer.h>
    tm_shader_o *shader = tm_shader_repository_api->lookup_shader(args->shader_repository, TM_STATIC_HASH("magnum_terrain", 0xf20b997cd1a8b092ULL));
    if (!shader)
        return;

    TM_PROFILER_BEGIN_FUNC_SCOPE();
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    tm_shader_io_o *io = tm_shader_api->shader_io(shader);
    tm_shader_system_context_o *shader_context = tm_shader_system_api->clone_context(args->shader_context, a);

    mag_terrain_component_t **cdata = (mag_terrain_component_t **)render_component_data;

    tm_renderer_shader_info_t *shader_infos = 0;
    tm_carray_temp_resize(shader_infos, num_viewers * num_renderables, ta);

    /* carray */ tm_renderer_draw_call_info_t *draw_calls = 0;
    tm_carray_temp_resize(draw_calls, num_viewers * num_renderables, ta);

    tm_shader_system_o *gbuffer_system = tm_shader_repository_api->lookup_system(args->shader_repository, TM_STATIC_HASH("gbuffer_system", 0xa80f4bbd19f07012ULL));
    tm_shader_system_o *shadow_system = tm_shader_repository_api->lookup_system(args->shader_repository, TM_STATIC_HASH("shadow_system", 0x44caf3774afb381eULL));

    uint64_t gbuffer_sort_key = args->render_graph ? tm_render_graph_api->sort_key(args->render_graph, TM_STATIC_HASH("gbuffer", 0xc0d9fff4f568ebfdULL)) : 0;
    uint64_t shadows_sort_key = 0;

    /* carray */ uint64_t *sort_keys = 0;
    tm_carray_temp_resize(sort_keys, num_viewers * num_renderables, ta);
    uint32_t num_draws = 0;

    // TODO: if we end up needing only camera pos, simplify this
    /* carray */ tm_vec3_t *viewer_positions = 0;
    tm_carray_temp_resize(viewer_positions, num_viewers, ta);
    for (uint32_t v = 0; v != 1; ++v) {
        tm_mat44_t view_inverse;
        tm_mat44_inverse(&view_inverse, viewers[v].camera->view);
        viewer_positions[v] = (tm_vec3_t) { view_inverse.wx, view_inverse.wy, view_inverse.wz };
    }

    tm_shader_constant_buffer_instance_t cbuf;
    tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);
    set_constant(io, args->default_resource_buffer, &cbuf, TM_STATIC_HASH("camera_pos", 0x75053e0e1e09e303ULL), &viewer_positions[0], sizeof(viewer_positions[0]));

    tm_shader_system_api->activate_system(shader_context, man->material_system, NULL, 0, &man->material_rbinder, 1);
    for (uint32_t i = 0; i != num_renderables; ++i) {
        mag_terrain_component_t *component = cdata[i];
        if (!component->region_data.key || component->generate_task_id || !component->buffers->region_info.num_indices) {
            continue;
        }

        // tm_vec3_t center = region_center(&component->region_data);
        uint32_t max_viewers = LODS[component->region_data.lod].needs_shadows ? num_viewers : 1;
        tm_shader_system_api->activate_system(shader_context, man->region_render_system, &component->region_render_cbuf, 1, &component->region_render_rbinder, 1);
        for (uint32_t v = 0; v != max_viewers; ++v) {
            if (!tm_culling_frustum_visible(frustum_visibility, i, v, num_viewers))
                continue;
            tm_shader_system_api->activate_system(shader_context, viewers[v].viewer_system, viewers[v].viewer_cbuffer, 1, viewers[v].viewer_rbinder, viewers[v].viewer_rbinder ? 1 : 0);
            tm_shader_system_o *context_system = v == 0 ? gbuffer_system : shadow_system;
            tm_shader_system_api->activate_system(shader_context, context_system, 0, 0, 0, 0);

            TM_ASSERT(tm_shader_api->assemble_shader_infos(shader, 0, 0, shader_context, viewers[v].visibility_context, args->default_resource_buffer, &cbuf, NULL, 1, &shader_infos[num_draws]), "Failed to assemble shader to render terrain region");

            tm_shader_system_api->deactivate_system(shader_context, context_system);
            tm_shader_system_api->deactivate_system(shader_context, viewers[v].viewer_system);

            draw_calls[num_draws] = (tm_renderer_draw_call_info_t) {
                .primitive_type = TM_RENDERER_PRIMITIVE_TYPE_TRIANGLE_LIST,
                .draw_type = TM_RENDERER_DRAW_TYPE_INDEXED,
                .indexed = {
                    .num_indices = component->buffers->region_info.num_indices,
                    .num_instances = 1 },
                .index_type = TM_RENDERER_INDEX_TYPE_UINT16,
                .index_buffer = component->buffers->ibuf,
            };

            float lod_size = LODS[component->region_data.lod].size * (float)MAG_VOXEL_CHUNK_SIZE;
            tm_vec3_t half_size = { lod_size / 2.f, lod_size / 2.f, lod_size / 2.f };
            tm_vec3_t center = tm_vec3_add(component->region_data.pos, half_size);
            float camera_distance = tm_vec3_dist(center, viewer_positions[0]);
            uint16_t depth = (uint16_t)floorf((camera_distance + lod_size / 2.f) / lod_size * 100.f);
            //camera_distance /= viewers[0].camera->settings.far_plane;
            //camera_distance = tm_clamp(camera_distance, 0.f, 1.f);
            sort_keys[num_draws] = viewers[v].sort_key | (v == 0 ? gbuffer_sort_key : shadows_sort_key);
            sort_keys[num_draws] |= region_depth_sort_key(depth, component->region_data.lod);
            //sort_keys[num_draws] |= ((uint64_t)component->region_data.lod) << TM_RENDER_GRAPH_SORT_INTERNAL_PASS_BITS_START;
            ++num_draws;
        }
        tm_shader_system_api->deactivate_system(shader_context, man->region_render_system);
    }
    if (num_draws)
        tm_renderer_api->tm_renderer_command_buffer_api->draw_calls(args->default_command_buffer, sort_keys, draw_calls, shader_infos, num_draws);
    tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
    TM_PROFILER_END_FUNC_SCOPE();
}

static const char *component__category(void)
{
    return TM_LOCALIZE("Magnum Voxel");
}

static tm_ci_render_i *render_aspect = &(tm_ci_render_i) {
    .bounding_volume_type = bounding_volume_type,
    .fill_bounding_volume_buffer = fill_bounding_volume_buffer,
    .render = magnum_terrain_render,
};

static tm_ci_editor_ui_i *editor_aspect = &(tm_ci_editor_ui_i) {
    .category = component__category
};

static tm_properties_aspect_i *properties_aspect = &(tm_properties_aspect_i) {
    .custom_ui = properties_ui,
    .get_type_display_name = get_type_display_name,
};

static tm_properties_aspect_i *material_properties_aspect = &(tm_properties_aspect_i) {
    .custom_ui = material_properties_ui,
    .get_type_display_name = get_material_type_display_name,
};

static void test_mag_terrain_component(tm_unit_test_runner_i *tr, tm_allocator_i *a)
{
    region_data_map_t regions = { .allocator = a };
    wanted_regions((tm_vec3_t) { 0, 0, 0 }, &regions);
    TM_UNIT_TEST(tr, regions.num_used < 1200);

    tm_hash_free(&regions);
}

static tm_unit_test_i *mag_terrain_component_tests = &(tm_unit_test_i) {
    .name = "mag_terrain_component",
    .test = test_mag_terrain_component
};

typedef struct generate_region_task_data_t
{
    mag_terrain_component_buffers_t *c;
    mag_terrain_component_manager_o *man;

    region_data_t region_data;

    op_t *ops_end;
} generate_region_task_data_t;

uint32_t readback_region_info(mag_terrain_component_buffers_t *c, tm_renderer_command_buffer_o *cmd_buf, uint16_t resource_state)
{
    return tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, UINT64_MAX,
        &(tm_renderer_read_buffer_t) {
            .resource_handle = c->region_info_handle,
            .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
            .resource_state = resource_state,
            .resource_queue = TM_RENDERER_QUEUE_GRAPHICS,
            .bits = &c->region_info,
            .size = sizeof(c->region_info) });
}

uint32_t readback_unpacked_vertices(read_mesh_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_handle_t unpacked_vertices, uint16_t resource_state, uint32_t vertex_count)
{
    tm_carray_shrink(buffers->vertices, vertex_count);
    return tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, UINT64_MAX,
        &(tm_renderer_read_buffer_t) {
            .resource_handle = unpacked_vertices,
            .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
            .resource_state = resource_state,
            .resource_queue = TM_RENDERER_QUEUE_GRAPHICS,
            .bits = buffers->vertices,
            .size = tm_carray_size(buffers->vertices) * sizeof(*buffers->vertices) });
}

uint32_t readback_indices(read_mesh_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, tm_renderer_command_buffer_o *cmd_buf, uint16_t resource_state, uint32_t index_count)
{
    tm_carray_shrink(buffers->indices, index_count);
    return tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, UINT64_MAX,
        &(tm_renderer_read_buffer_t) {
            .resource_handle = c->ibuf,
            .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
            .resource_state = resource_state,
            .resource_queue = TM_RENDERER_QUEUE_GRAPHICS,
            .bits = buffers->indices,
            .size = tm_carray_size(buffers->indices) * sizeof(*buffers->indices) });
}

void generate_region_task(mag_async_gpu_queue_task_args_t *args)
{
    generate_region_task_data_t *data = (generate_region_task_data_t *)args->data;
    mag_terrain_component_buffers_t *c = data->c;
    mag_terrain_component_manager_o *man = data->man;

    tm_renderer_command_buffer_o *cmd_buf;
    man->backend->create_command_buffers(man->backend->inst, &cmd_buf, 1);
    tm_renderer_resource_command_buffer_o *res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);
    tm_renderer_resource_command_buffer_o *post_res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &post_res_buf, 1);

    tm_renderer_handle_t new_fence = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_queue_fence(res_buf, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
    uint32_t old_fence_id = atomic_exchange_uint32_t(&c->generate_fence, new_fence.resource);

    tm_renderer_handle_t old_fence = { .resource = old_fence_id };
    if (old_fence.resource) {
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(post_res_buf, old_fence);
    }

    uint64_t sort_key = 0;
    const tm_renderer_queue_bind_t bind_info = {
        .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
        // TODO: probably compute queue is better suited for this
        .queue_family = TM_RENDERER_QUEUE_GRAPHICS,
        .scheduling.signal_device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
        .scheduling.signal_queue_fence = new_fence,

        .scheduling.num_wait_fences = old_fence.resource ? 1 : 0,
        .scheduling.wait_queue_fences[0] = old_fence,
        .scheduling.wait_device_affinity_masks[0] = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
    };

    tm_renderer_api->tm_renderer_command_buffer_api->bind_queue(cmd_buf, sort_key, &bind_info);
    ++sort_key;

    generate_region_gpu(cmd_buf, res_buf, &sort_key, man, c, &data->region_data, man->ops, data->ops_end);

    tm_renderer_api->tm_renderer_command_buffer_api->bind_queue(cmd_buf, UINT64_MAX - 1, &(tm_renderer_queue_bind_t) { .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL });

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    uint32_t fence_id = readback_region_info(c, cmd_buf, state);
    tm_carray_push(args->out_fences, fence_id, args->fences_allocator);

    man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);

    man->backend->submit_command_buffers(man->backend->inst, &cmd_buf, 1);
    man->backend->destroy_command_buffers(man->backend->inst, &cmd_buf, 1);

    man->backend->submit_resource_command_buffers(man->backend->inst, &post_res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &post_res_buf, 1);
}

void unpack_vertices(tm_shader_repository_o *shader_repo, read_mesh_task_buffers_t *buffers, const mag_terrain_component_buffers_t *c, tm_renderer_command_buffer_o *cmd_buf, tm_renderer_resource_command_buffer_o *res_buf, uint64_t *sort_key, uint32_t vertex_count)
{
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    tm_shader_o *shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_terrain_unpack_vertices", 0xe1fb26519ebe19d6ULL));
    tm_shader_io_o *io = tm_shader_api->shader_io(shader);
    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuf;
    tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
    tm_shader_api->create_constant_buffer_instances(io, 1, &cbuf);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("packed_vertices", 0xe6c723c81dbf5ba0ULL), &c->vertices_handle, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("unpacked_vertices", 0xae69f79617645fcaULL), &buffers->unpacked_vertices, 0, 0, 1);

    tm_shader_system_context_o *shader_context = tm_shader_system_api->create_context(a, NULL);
    tm_renderer_shader_info_t shader_info;
    if (tm_shader_api->assemble_shader_infos(shader, 0, 0, shader_context, TM_STRHASH(0), res_buf, &cbuf, &rbinder, 1, &shader_info)) {
        tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, sort_key, &(tm_renderer_compute_info_t) { .dispatch.group_count = { vertex_count, 1, 1 } }, &shader_info, 1);
        *sort_key += 1;
    }

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, *sort_key, &(tm_renderer_resource_barrier_t) { .resource_handle = buffers->unpacked_vertices, .source_state = state, .destination_state = state }, 1);
    *sort_key += 1;
    tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);
    tm_shader_api->destroy_constant_buffer_instances(io, &cbuf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

void read_mesh_task(mag_async_gpu_queue_task_args_t *args)
{
    read_mesh_task_data_t *data = (read_mesh_task_data_t *)args->data;
    mag_terrain_component_buffers_t *c = data->c;
    mag_terrain_component_manager_o *man = data->man;

    tm_renderer_command_buffer_o *cmd_buf;
    man->backend->create_command_buffers(man->backend->inst, &cmd_buf, 1);
    tm_renderer_resource_command_buffer_o *res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);
    tm_renderer_resource_command_buffer_o *post_res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &post_res_buf, 1);

    tm_renderer_handle_t new_fence = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_queue_fence(res_buf, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
    uint32_t old_fence_id = atomic_exchange_uint32_t(&c->generate_fence, new_fence.resource);

    tm_renderer_handle_t old_fence = { .resource = old_fence_id };
    if (old_fence.resource) {
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(post_res_buf, old_fence);
    }

    uint64_t sort_key = 0;
    const tm_renderer_queue_bind_t bind_info = {
        .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
        .queue_family = TM_RENDERER_QUEUE_GRAPHICS,
        .scheduling.signal_device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
        .scheduling.signal_queue_fence = new_fence,

        .scheduling.num_wait_fences = old_fence.resource ? 1 : 0,
        .scheduling.wait_queue_fences[0] = old_fence,
        .scheduling.wait_device_affinity_masks[0] = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL,
    };

    tm_renderer_api->tm_renderer_command_buffer_api->bind_queue(cmd_buf, sort_key, &bind_info);
    ++sort_key;

    if (!c->read_mesh_task_buffers_id) {
        LOCK_BUFFERS(man->read_mesh_task_buffers_locks, c->read_mesh_task_buffers_id);
    }
    read_mesh_task_buffers_t *task_buffers = GET_BUFFERS(man->read_mesh_task_buffers, c->read_mesh_task_buffers_id);

    // TODO: cache unpacking too
    unpack_vertices(man->shader_repo, task_buffers, c, cmd_buf, res_buf, &sort_key, c->region_info.num_vertices);

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    uint32_t vertices_fence_id = readback_unpacked_vertices(task_buffers, c, cmd_buf, task_buffers->unpacked_vertices, state, c->region_info.num_vertices);
    tm_carray_push(args->out_fences, vertices_fence_id, args->fences_allocator);

    uint32_t indices_fence_id = readback_indices(task_buffers, c, cmd_buf, TM_RENDERER_RESOURCE_STATE_COPY_SOURCE, c->region_info.num_indices);
    tm_carray_push(args->out_fences, indices_fence_id, args->fences_allocator);

    man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);

    man->backend->submit_command_buffers(man->backend->inst, &cmd_buf, 1);
    man->backend->destroy_command_buffers(man->backend->inst, &cmd_buf, 1);

    man->backend->submit_resource_command_buffers(man->backend->inst, &post_res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &post_res_buf, 1);
}

static double *terrain_regions_stat;
static double *terrain_vertices_stat;

static void do_generate_physics(generate_physics_task_data_t *data, tm_buffers_i *buffers, read_mesh_task_buffers_t *read_mesh_buffers)
{
    tm_physics_cook_mesh_raw_args_t args = {
        .vertex_data = read_mesh_buffers->vertices,
        .vertex_count = (uint32_t)tm_carray_size(read_mesh_buffers->vertices),
        .vertex_stride = sizeof(tm_vec3_t),

        .index_data = read_mesh_buffers->indices,
        .triangle_count = (uint32_t)tm_carray_size(read_mesh_buffers->indices) / 3,
        .triangle_stride = sizeof(uint16_t) * 3,

        .flags = TM_PHYSICS_COOK_MESH_16_BIT_INDICES,
    };

    if (data->buffer_id) {
        buffers->release(buffers->inst, data->buffer_id);
        data->buffer_id = 0;
    }

    char *format;
    uint32_t error;

    struct tm_physics_shape_cooking_i *physics_cooking = tm_first_implementation(tm_global_api_registry, tm_physics_shape_cooking_i);
    data->buffer_id = physics_cooking->cook_mesh_raw(physics_cooking->inst, &args, buffers, &format, &error);

    if (data->buffer_id) {
        uint64_t buf_size;
        const void *buf_data = buffers->get(buffers->inst, data->buffer_id, &buf_size);
        uint64_t buf_hash = buffers->hash(buffers->inst, data->buffer_id);

        // the remaining component fields should already be initialized
        data->physics_component.mesh = (tm_physics_shape_cooked_t) {
            .format = format,
            .size = buf_size,
            .data = buf_data,
            .hash = buf_hash,
        };
    } else {
        TM_ERROR("Failed to cook physics mesh. Error: %u", error);
    }
}

static void generate_physics_job(void *task_data)
{
    generate_physics_task_data_t *data = (generate_physics_task_data_t *)task_data;
    tm_buffers_i *buffers = tm_the_truth_api->buffers(data->tt);

    read_mesh_task_buffers_t *read_mesh_buffers = GET_BUFFERS(data->man->read_mesh_task_buffers, data->read_mesh_task_buffers_id);
    do_generate_physics(data, buffers, read_mesh_buffers);
    RELEASE_BUFFERS(data->man->read_mesh_task_buffers_locks, data->read_mesh_task_buffers_id);
}

static void generate_physics_task(void *task_data, uint64_t id)
{
    generate_physics_task_data_t *data = (generate_physics_task_data_t *)task_data;
    tm_buffers_i *buffers = tm_the_truth_api->buffers(data->tt);

    read_mesh_task_buffers_t *read_mesh_buffers = GET_BUFFERS(data->man->read_mesh_task_buffers, data->read_mesh_task_buffers_id);

    TM_OS_ENTER_CRITICAL_SECTION(&data->cs);
    if (atomic_fetch_add_uint64_t(&data->current_task_id, 0) == id) {
        do_generate_physics(data, buffers, read_mesh_buffers);
    }
    TM_OS_LEAVE_CRITICAL_SECTION(&data->cs);

    RELEASE_BUFFERS(data->man->read_mesh_task_buffers_locks, data->read_mesh_task_buffers_id);
}

static void start_physics_task(mag_terrain_component_manager_o *man, mag_terrain_component_t *c, tm_jobdecl_t *job)
{
    uint64_t cur_task_id;
    TM_OS_ENTER_CRITICAL_SECTION(&c->physics_data->cs);
    if (c->physics_data->read_mesh_task_buffers_id)
        RELEASE_BUFFERS(man->read_mesh_task_buffers_locks, c->physics_data->read_mesh_task_buffers_id);
    c->physics_data->read_mesh_task_buffers_id = c->buffers->read_mesh_task_buffers_id;
    c->buffers->read_mesh_task_buffers_id = 0;

    uint64_t new_task_id = 0;
    if (!job)
        new_task_id = tm_task_system_api->run_task(generate_physics_task, c->physics_data, "mag_terrain_generate_physics", 0, true);
    cur_task_id = atomic_exchange_uint64_t(&c->physics_data->current_task_id, new_task_id);
    TM_OS_LEAVE_CRITICAL_SECTION(&c->physics_data->cs);

    if (cur_task_id)
        tm_task_system_api->cancel_task(cur_task_id);

    if (job)
        *job = (tm_jobdecl_t) { .task = generate_physics_job, .data = c->physics_data };
}

static void remove_physics_components(mag_terrain_component_manager_o *man, mag_terrain_component_t *c, tm_entity_t entity, struct tm_entity_commands_o *commands)
{
    tm_entity_commands_api->remove_component(commands, entity, man->physics_shape_component_type);
    tm_entity_commands_api->remove_component(commands, entity, man->rigid_static_component_type);
}

static void update_physics_component(mag_terrain_component_manager_o *man, mag_terrain_component_t *c, tm_entity_t entity, struct tm_entity_commands_o *commands)
{
    if (c->physics_data->buffer_id) {
        tm_physics_shape_component_t *physics_component = tm_entity_commands_api->add_component(commands, entity, man->physics_shape_component_type);
        *physics_component = c->physics_data->physics_component;
        physics_component->offset.pos = c->region_data.pos;
        float scl = LODS[c->region_data.lod].size;
        physics_component->offset.scl = (tm_vec3_t) { scl, scl, scl };
    }
}

static bool needs_sculpting(const region_data_t *region_data, tm_vec3_t camera_pos)
{
    const aabb_t region_aabb = region_aabb_with_margin(region_data);
    return aabb_point_distance_sqr(&region_aabb, &camera_pos) <= MAX_SCULPT_DISTANCE * MAX_SCULPT_DISTANCE;
}

static void generate_region_cancel(void *data)
{
    generate_region_task_data_t *task_data = (generate_region_task_data_t *)data;
    tm_free(&task_data->man->allocator, task_data, sizeof(*task_data));
}

static void generate_region_complete(void *data)
{
    generate_region_task_data_t *task_data = (generate_region_task_data_t *)data;
    RELEASE_BUFFERS(task_data->man->region_task_buffers_locks, task_data->c->gen_region_task_buffers_id);
    tm_free(&task_data->man->allocator, task_data, sizeof(*task_data));
}

static void engine__update_terrain(tm_engine_o *inst, tm_engine_update_set_t *data, struct tm_entity_commands_o *commands)
{
    mag_terrain_component_manager_o *man = (mag_terrain_component_manager_o *)inst;
    // TODO: hacky, need to fix TM
    if (!man->rigid_static_component_type.index)
        man->rigid_static_component_type = tm_entity_api->lookup_component_type(man->ctx, TM_TT_TYPE_HASH__PHYSX_RIGID_STATIC_COMPONENT);

    const tm_transform_t *camera_transform = tm_entity_api->get_blackboard_ptr(man->ctx, TM_ENTITY_BB__CAMERA_TRANSFORM);
    if (!camera_transform)
        return;

    TM_PROFILER_BEGIN_FUNC_SCOPE();
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, temp_allocator);

    region_data_map_t alive_regions = { .allocator = temp_allocator };
    wanted_regions(camera_transform->pos, &alive_regions);

    aabb_t *new_op_aabbs = 0;

    if (man->last_empty_check_op != tm_slab_end(man->ops)) {
        for (const op_t *op = man->last_empty_check_op; op != tm_slab_end(man->ops); op = tm_slab_next(op)) {
            if (!tm_slab_is_valid(op))
                continue;
            tm_carray_temp_push(new_op_aabbs, op_aabb(op), ta);
        }
        man->last_empty_check_op = tm_slab_end(man->ops);
    }

    for (uint64_t *region_key = man->empty_regions.keys; region_key < man->empty_regions.keys + man->empty_regions.num_buckets; ++region_key) {
        if (tm_set_use_key(&man->empty_regions, region_key)) {
            int32_t idx = tm_hash_index(&alive_regions, *region_key);
            bool exists = idx != -1;
            if (!exists) {
                // region no longer needed
                *region_key = TM_HASH_TOMBSTONE;
                man->empty_regions.num_used -= 1;
            } else {
                const aabb_t region_aabb = region_aabb_with_margin(alive_regions.values + idx);

                if (aabb_point_distance_sqr(&region_aabb, &camera_transform->pos) <= MAX_SCULPT_DISTANCE * MAX_SCULPT_DISTANCE) {
                    // Region can be sculpted. Need to cache densities for faster sculpting.
                    *region_key = TM_HASH_TOMBSTONE;
                    man->empty_regions.num_used -= 1;
                } else {
                    bool has_new_operations = false;
                    for (const aabb_t *new_op_aabb = new_op_aabbs; new_op_aabb != tm_carray_end(new_op_aabbs); ++new_op_aabb) {
                        if (aabb_intersect(&region_aabb, new_op_aabb)) {
                            // An operation was applied to the region. It's possible it's no longer empty.
                            has_new_operations = true;
                            *region_key = TM_HASH_TOMBSTONE;
                            man->empty_regions.num_used -= 1;
                            break;
                        }
                    }
                    if (!has_new_operations) {
                        alive_regions.keys[idx] = TM_HASH_TOMBSTONE;
                        alive_regions.num_used -= 1;
                    }
                }
            }
        }
    }

    // TODO: split to multiple cache-efficient components depending on the region state

    tm_renderer_command_buffer_o *cmd_buf;
    man->backend->create_command_buffers(man->backend->inst, &cmd_buf, 1);
    tm_renderer_resource_command_buffer_o *res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);
    tm_renderer_resource_command_buffer_o *post_res_buf;
    man->backend->create_resource_command_buffers(man->backend->inst, &post_res_buf, 1);

    // TODO: batch operations on the async gpu queue

    uint32_t total_components = 0;
    for (tm_engine_update_array_t *a = data->arrays; a < data->arrays + data->num_arrays; ++a) {
        total_components += a->n;
    }

    const float dt = (float)tm_entity_api->get_blackboard_double(man->ctx, TM_ENTITY_BB__DELTA_TIME, 0);

    typedef struct physics_update_fences_t
    {
        mag_terrain_component_t *c;
        tm_entity_t entity;
        uint32_t vertices_fence;
        uint32_t indices_fence;
        bool vertices_read;
        bool indices_read;
        bool task_started;
    } physics_update_fences_t;

    typedef struct region_update_fence_t
    {
        mag_terrain_component_t *c;
        tm_entity_t entity;
        uint32_t fence_id;
        uint32_t region_task_buffers_id;
    } region_update_fence_t;

    region_update_fence_t *region_info_fences = 0;
    tm_carray_temp_ensure(region_info_fences, total_components, ta);
    uint32_t region_updates_with_physics = 0;

    mag_terrain_component_t **free_components = NULL;
    tm_carray_temp_ensure(free_components, total_components, ta);
    uint64_t *free_component_entities = NULL;
    tm_carray_temp_ensure(free_component_entities, total_components, ta);

    uint64_t sort_key = 0;
    uint32_t active_task_count = 0;
    tm_shader_io_o *render_io = tm_shader_api->system_io(man->region_render_system);
    for (tm_engine_update_array_t *a = data->arrays; a < data->arrays + data->num_arrays; ++a) {
        mag_terrain_component_t *components = a->components[0];

        for (uint32_t i = 0; i < a->n; ++i) {
            mag_terrain_component_t *c = components + i;

            bool existing = false;
            if (c->region_data.key) {
                int32_t idx = tm_hash_index(&alive_regions, c->region_data.key);
                existing = idx != -1;
                if (existing) {
                    // update prev_lod_min, prev_lod_max and possibly other transient data that
                    // isn't encoded in the key
                    region_data_t old_data = c->region_data;
                    c->region_data = alive_regions.values[idx];
                    if (!tm_vec3_equal(old_data.cull_min, c->region_data.cull_min))
                        set_constant(render_io, res_buf, &c->region_render_cbuf, TM_STATIC_HASH("cull_min", 0x7847b0225627923eULL), &c->region_data.cull_min, sizeof(c->region_data.cull_min));
                    if (!tm_vec3_equal(old_data.cull_max, c->region_data.cull_max))
                        set_constant(render_io, res_buf, &c->region_render_cbuf, TM_STATIC_HASH("cull_max", 0x9cd906a428fb9b7eULL), &c->region_data.cull_max, sizeof(c->region_data.cull_max));
                    alive_regions.keys[idx] = TM_HASH_TOMBSTONE;
                    alive_regions.num_used -= 1;
                }
            }
            if (existing) {
                if (c->generate_task_id && mag_async_gpu_queue_api->is_task_done(man->gpu_queue, c->generate_task_id)) {
                    handle_generate_task_completion(man, c, res_buf);
                    if (c->buffers->region_info.num_indices || needs_sculpting(&c->region_data, camera_transform->pos)) {
                        mag_terrain_component_state_t state = {
                            .buffers = c->buffers,
                        };
                        tm_hash_update(&man->component_map, c->region_data.key, state);
                    }
                    c->color_rgba.w = 0.f;
                }

                if (!c->generate_task_id && !c->buffers->region_info.num_indices && !needs_sculpting(&c->region_data, camera_transform->pos)) {
                    tm_set_add(&man->empty_regions, c->region_data.key);
                    tm_hash_remove(&man->component_map, c->region_data.key);
                    tm_carray_temp_push(free_components, c, ta);
                    tm_carray_temp_push(free_component_entities, a->entities[i].u64, ta);
                    c->region_data.key = 0;
                }

                if (c->read_mesh_task_id && mag_async_gpu_queue_api->is_task_done(man->gpu_queue, c->read_mesh_task_id)) {
                    c->read_mesh_task_id = 0;
                    // TODO: sort by lod, so that near lods are generated first
                    start_physics_task(man, c, NULL);
                }

                if (c->physics_data->current_task_id && tm_task_system_api->is_task_done(c->physics_data->current_task_id)) {
                    c->physics_data->current_task_id = 0;
                    update_physics_component(man, c, a->entities[i], commands);
                }

                if (c->region_data.key && !c->generate_task_id) {
                    if (c->color_rgba.w < 1.0f) {
                        c->color_rgba.w = min(1.0f, c->color_rgba.w + ALPHA_SPEED * dt);
                        set_constant(tm_shader_api->system_io(man->region_render_system), res_buf, &c->region_render_cbuf, TM_STATIC_HASH("alpha", 0x3f6973542dd6a4fbULL), &c->color_rgba.w, sizeof(c->color_rgba.w));
                    }
                    // TODO: do we need to wait for physics too?
                    if (c->last_applied_op != tm_slab_end(man->ops) && !c->read_mesh_task_id) {
                        ++sort_key;
                        const aabb_t region_aabb = region_aabb_with_margin(&c->region_data);
                        uint32_t region_task_buffers_id = 0;
                        region_task_buffers_t *task_buffers = 0;
                        for (const op_t *op = c->last_applied_op; op != tm_slab_end(man->ops); op = tm_slab_next(op)) {
                            if (!tm_slab_is_valid(op))
                                continue;
                            const aabb_t aabb = op_aabb(op);
                            if (aabb_intersect(&region_aabb, &aabb)) {
                                if (!region_task_buffers_id) {
                                    LOCK_BUFFERS(man->region_task_buffers_locks, region_task_buffers_id);
                                    task_buffers = GET_BUFFERS(man->region_task_buffers, region_task_buffers_id);
                                }
                                apply_op_to_component(man, task_buffers, c->buffers, &c->region_data, op, cmd_buf, res_buf, &sort_key);
                            }
                        }

                        if (region_task_buffers_id) {
                            generate_mesh(man, task_buffers, c->buffers, &c->region_data, cmd_buf, res_buf, &sort_key);
                            uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
                            uint32_t region_info_fence = readback_region_info(c->buffers, cmd_buf, state);
                            region_update_fence_t fence = { .fence_id = region_info_fence, .region_task_buffers_id = region_task_buffers_id, .c = c, .entity = a->entities[i] };
                            tm_carray_temp_push(region_info_fences, fence, ta);
                            if (LODS[c->region_data.lod].needs_physics) {
                                ++region_updates_with_physics;
                            }
                        }
                        c->last_applied_op = tm_slab_end(man->ops);
                    }
                }

                if (c->generate_task_id || c->read_mesh_task_id)
                    ++active_task_count;
            } else {
                // TM_LOG("discarding region: (%f, %f, %f) cell size %f, key %llu", c->region_data.pos.x, c->region_data.pos.y, c->region_data.pos.z, c->region_data.cell_size, c->region_data.key);
                // TODO: think of how to cancel this safely
                if (c->generate_task_id && mag_async_gpu_queue_api->is_task_done(man->gpu_queue, c->generate_task_id)) {
                    c->generate_task_id = 0;
                    c->color_rgba.w = 0.f;
                }
                if (c->read_mesh_task_id && mag_async_gpu_queue_api->is_task_done(man->gpu_queue, c->read_mesh_task_id)) {
                    RELEASE_BUFFERS(man->read_mesh_task_buffers_locks, c->buffers->read_mesh_task_buffers_id);
                    c->read_mesh_task_id = 0;
                }
                if (c->region_data.key && LODS[c->region_data.lod].needs_physics) {
                    remove_physics_components(man, c, a->entities[i], commands);
                }

                if (c->color_rgba.w > 0.f) {
                    c->color_rgba.w = max(0.f, c->color_rgba.w - ALPHA_SPEED * dt);
                    set_constant(tm_shader_api->system_io(man->region_render_system), res_buf, &c->region_render_cbuf, TM_STATIC_HASH("alpha", 0x3f6973542dd6a4fbULL), &c->color_rgba.w, sizeof(c->color_rgba.w));
                }

                if ((c->color_rgba.w <= 0.f || !c->region_data.key) && !c->generate_task_id && !c->read_mesh_task_id) {
                    tm_carray_temp_push(free_components, c, ta);
                    tm_carray_temp_push(free_component_entities, a->entities[i].u64, ta);

                    if (c->region_data.key) {
                        tm_hash_remove(&man->component_map, c->region_data.key);
                        c->region_data.key = 0;
                    }
                }
            }
        }
    }

    for (uint32_t i = 0; i < alive_regions.num_buckets; ++i) {
        if (tm_hash_skip_index(&alive_regions, i))
            continue;

        region_data_t region_data = alive_regions.values[i];
        if (!tm_carray_size(free_components)) {
            if (active_task_count >= MAX_EXTRA_REGIONS)
                break;
            // TODO: start the task immediately and use add_component_by_handle
            // to avoid the 1-frame lag
            tm_entity_commands_api->create_entity_from_mask(commands, &man->component_mask);
        } else {
            mag_terrain_component_t *c = tm_carray_pop(free_components);
            uint64_t entity_id = tm_carray_pop(free_component_entities);
            c->region_data = region_data;

            set_constant(tm_shader_api->system_io(man->region_contouring_system), res_buf, &c->buffers->region_contouring_cbuf, TM_STATIC_HASH("tolerance", 0xc500d6c49d9c007aULL), &LODS[region_data.lod].qef_tolerance, sizeof(LODS[region_data.lod].qef_tolerance));

            set_constant(render_io, res_buf, &c->region_render_cbuf, TM_STATIC_HASH("cell_size", 0x50b5f09b4c1a94fdULL), &LODS[region_data.lod].size, sizeof(LODS[region_data.lod].size));
            set_constant(render_io, res_buf, &c->region_render_cbuf, TM_STATIC_HASH("entity_id", 0x99b0b65b80bcf53eULL), &entity_id, sizeof(entity_id));
            set_constant(render_io, res_buf, &c->region_render_cbuf, TM_STATIC_HASH("region_pos", 0x5af0fcabdb39700fULL), &region_data.pos, sizeof(region_data.pos));
            set_constant(render_io, res_buf, &c->region_render_cbuf, TM_STATIC_HASH("cull_min", 0x7847b0225627923eULL), &region_data.cull_min, sizeof(region_data.cull_min));
            set_constant(render_io, res_buf, &c->region_render_cbuf, TM_STATIC_HASH("cull_max", 0x9cd906a428fb9b7eULL), &region_data.cull_max, sizeof(region_data.cull_max));

            generate_region_task_data_t *task_data;
            task_data = tm_alloc(&man->allocator, sizeof(*task_data));

            *task_data = (generate_region_task_data_t) {
                .c = c->buffers,
                .man = man,
                .region_data = c->region_data,
                .ops_end = tm_slab_end(man->ops)
            };

            c->last_task_op = task_data->ops_end;

            mag_async_gpu_queue_task_params_t params = {
                .f = generate_region_task,
                .data = task_data,
                .cancel_callback = generate_region_cancel,
                .completion_callback = generate_region_complete,
                .priority = region_priority(&c->region_data, &camera_transform->pos),
            };
            c->generate_task_id = mag_async_gpu_queue_api->submit_task(man->gpu_queue, &params);

            mag_terrain_component_state_t state = {
                .generate_task_id = c->generate_task_id,
                .buffers = c->buffers,
            };
            tm_hash_add(&man->component_map, c->region_data.key, state);
        }
        ++active_task_count;
    }

    man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);
    man->backend->submit_command_buffers(man->backend->inst, &cmd_buf, 1);
    man->backend->destroy_command_buffers(man->backend->inst, &cmd_buf, 1);
    man->backend->submit_resource_command_buffers(man->backend->inst, &post_res_buf, 1);
    man->backend->destroy_resource_command_buffers(man->backend->inst, &post_res_buf, 1);

    // if (tm_carray_size(region_info_fences) || tm_carray_size(physics_update_fences))
    //     TM_LOG("Region updates: %llu; physics updates: %llu", tm_carray_size(region_info_fences), tm_carray_size(physics_update_fences));

    physics_update_fences_t *physics_update_fences = 0;
    tm_carray_temp_ensure(physics_update_fences, region_updates_with_physics, ta);
    if (region_updates_with_physics) {
        man->backend->create_command_buffers(man->backend->inst, &cmd_buf, 1);
        man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);
    }

    uint64_t region_updates_profiler_id = tm_profiler_api->begin("wait_for_region_updates", NULL, NULL);
    for (region_update_fence_t *region_info_fence = region_info_fences; region_info_fence != tm_carray_end(region_info_fences); ++region_info_fence) {
        while (!man->backend->read_complete(man->backend->inst, region_info_fence->fence_id, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL))
            ;
        RELEASE_BUFFERS(man->region_task_buffers_locks, region_info_fence->region_task_buffers_id);
        mag_terrain_component_t *c = region_info_fence->c;
        if (LODS[c->region_data.lod].needs_physics && c->buffers->region_info.num_indices) {
            if (!c->buffers->read_mesh_task_buffers_id)
                LOCK_BUFFERS(man->read_mesh_task_buffers_locks, c->buffers->read_mesh_task_buffers_id);
            read_mesh_task_buffers_t *buffers = GET_BUFFERS(man->read_mesh_task_buffers, c->buffers->read_mesh_task_buffers_id);
            unpack_vertices(man->shader_repo, buffers, c->buffers, cmd_buf, res_buf, &sort_key, c->buffers->region_info.num_vertices);
            uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
            physics_update_fences_t update_fences = {
                .c = c,
                .entity = region_info_fence->entity,
                .vertices_fence = readback_unpacked_vertices(buffers, c->buffers, cmd_buf, buffers->unpacked_vertices, state, c->buffers->region_info.num_vertices),
                .indices_fence = readback_indices(buffers, c->buffers, cmd_buf, TM_RENDERER_RESOURCE_STATE_COPY_SOURCE, c->buffers->region_info.num_indices),
            };
            tm_carray_temp_push(physics_update_fences, update_fences, ta);
        }
    }
    tm_profiler_api->end(region_updates_profiler_id);

    if (region_updates_with_physics) {
        uint64_t submit_readback_physics_profiler_id = tm_profiler_api->begin("submit_readback_physics", NULL, NULL);
        man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
        man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);
        man->backend->submit_command_buffers(man->backend->inst, &cmd_buf, 1);
        man->backend->destroy_command_buffers(man->backend->inst, &cmd_buf, 1);
        tm_profiler_api->end(submit_readback_physics_profiler_id);
    }

    uint64_t wait_physics_readbacks_profiler_id = tm_profiler_api->begin("wait_physics_readbacks", NULL, NULL);
    uint64_t remaining_updates = tm_carray_size(physics_update_fences);
    while (remaining_updates) {
        for (physics_update_fences_t *update_info = physics_update_fences; update_info != tm_carray_end(physics_update_fences); ++update_info) {
            if (!update_info->vertices_read && man->backend->read_complete(man->backend->inst, update_info->vertices_fence, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL)) {
                update_info->vertices_read = true;
            }
            if (!update_info->indices_read && man->backend->read_complete(man->backend->inst, update_info->indices_fence, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL)) {
                update_info->indices_read = true;
            }
            if (!update_info->task_started && update_info->vertices_read && update_info->indices_read) {
                --remaining_updates;
                update_info->task_started = true;
                mag_terrain_component_t *c = update_info->c;
                read_mesh_task_buffers_t *buffers = GET_BUFFERS(man->read_mesh_task_buffers, c->buffers->read_mesh_task_buffers_id);
                tm_carray_resize(buffers->indices, c->buffers->region_info.num_indices, &man->allocator);
                // TODO: job would be better in this case
                start_physics_task(man, c, NULL);
            }
        }
    }
    tm_profiler_api->end(wait_physics_readbacks_profiler_id);

    uint64_t create_physics_meshes_profiler_id = tm_profiler_api->begin("create_physics_meshes", NULL, NULL);
    for (physics_update_fences_t *update_info = physics_update_fences; update_info != tm_carray_end(physics_update_fences); ++update_info) {
        mag_terrain_component_t *c = update_info->c;
        remove_physics_components(man, c, update_info->entity, commands);
        if (c->physics_data->current_task_id) {
            while (!tm_task_system_api->is_task_done_else_assist(c->physics_data->current_task_id))
                ;
            c->physics_data->current_task_id = 0;
            update_physics_component(man, update_info->c, update_info->entity, commands);
        }
    }
    tm_profiler_api->end(create_physics_meshes_profiler_id);

    if (!terrain_regions_stat) {
        terrain_regions_stat = tm_statistics_source_api->source("magnum/terrain_regions", "Terrain Regions");
        terrain_vertices_stat = tm_statistics_source_api->source("magnum/terrain_vertices", "Terrain Vertices");
    }

    *terrain_regions_stat = 0;
    *terrain_vertices_stat = 0;
    for (tm_engine_update_array_t *a = data->arrays; a < data->arrays + data->num_arrays; ++a) {
        mag_terrain_component_t *components = a->components[0];

        for (uint32_t i = 0; i < a->n; ++i) {
            mag_terrain_component_t *c = components + i;
            if (c->region_data.key)
                *terrain_regions_stat += 1.0;
            if (!c->generate_task_id)
                *terrain_vertices_stat += (double)c->buffers->region_info.num_indices;
        }
    }

    // if (!ft_stat)
    //     ft_stat = tm_statistics_source_api->source("tm_application/frame_time_ms", "Frame Time (ms)");
    //
    // **t_stat = frame_time;

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
    TM_PROFILER_END_FUNC_SCOPE();
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
    struct tm_physx_scene_o *physx_scene = tm_entity_api->get_blackboard_ptr(man->ctx, TM_ENTITY_BB__PHYSX_SCENE);
    tm_physx_raycast_t raycast = tm_physx_scene_api->raycast(
        physx_scene, ray_start, ray_dir, max_distance, man->raycast_collision_id,
        (tm_physx_raycast_flags_t) { .ignore_dynamic = true }, NULL, 0);

    *hit_distance = raycast.block.distance;
    return raycast.has_block;
}

static void apply_operation(mag_terrain_component_manager_o *man, mag_terrain_op_type_t type, mag_terrain_op_primitive_t primitive, tm_vec3_t pos, tm_vec3_t radius)
{
    op_t *op = tm_slab_add(man->ops);
    *op = (op_t) {
        .type = type,
        .primitive = primitive,
        .pos = pos,
        .radius = radius,
        .next = op->next,
    };
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

    const tm_engine_i terrain_engine = {
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

    tm_entity_api->register_engine(ctx, &terrain_engine);
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
    tm_creation_graph_api = tm_get_api(reg, tm_creation_graph_api);
    tm_ui_api = tm_get_api(reg, tm_ui_api);
    tm_statistics_source_api = tm_get_api(reg, tm_statistics_source_api);
    tm_os_api = tm_get_api(reg, tm_os_api);
    tm_task_system_api = tm_get_api(reg, tm_task_system_api);
    tm_physx_scene_api = tm_get_api(reg, tm_physx_scene_api);

    mag_voxel_api = tm_get_api(reg, mag_voxel_api);
    mag_async_gpu_queue_api = tm_get_api(reg, mag_async_gpu_queue_api);

    tm_set_or_remove_api(reg, load, mag_terrain_api, &terrain_api);

    tm_add_or_remove_implementation(reg, load, tm_unit_test_i, mag_terrain_component_tests);
    tm_add_or_remove_implementation(reg, load, tm_entity_register_engines_simulation_i, entity_simulation__register);

    tm_add_or_remove_implementation(reg, load, tm_the_truth_create_types_i, create_truth_types);
    tm_add_or_remove_implementation(reg, load, tm_entity_create_component_i, create_mag_terrain_component);
}
