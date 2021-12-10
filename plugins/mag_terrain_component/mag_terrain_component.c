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

static struct mag_voxel_api *mag_voxel_api;

#include "mag_terrain_component.h"

#include <foundation/api_registry.h>
#include <foundation/bounding_volume.h>
#include <foundation/buffer_format.h>
#include <foundation/carray.inl>
#include <foundation/error.h>
#include <foundation/localizer.h>
#include <foundation/log.h>
#include <foundation/murmurhash64a.inl>
#include <foundation/profiler.h>
#include <foundation/the_truth.h>
#include <foundation/the_truth_types.h>
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

#include <foundation/math.inl>

#include "plugins/mag_voxel/mag_voxel.h"

typedef struct mag_terrain_component_t
{
    // Visibility mask built from VISIBILITY_FLAGS in The Truth.
    uint64_t visibility_mask;

    tm_vec3_t color_rgb;

    mag_voxel_mesh_t mesh;
    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_constant_buffer_instance_t cbuffer;

    tm_mat44_t local_to_world;
} mag_terrain_component_t;

typedef enum region_state_t {
    // running compute shader to generate voxel data
    REGION_BUILDING_VOXELS,
    // running dual contouring
    REGION_BUILDING_MESH,
    REGION_READY,
} region_state_t;

// 1. Obtain voxel data (large cube) from GPU
// 2. Dual contour it into mesh
// -- physics/ai missing --
// 3. Render mesh

static mag_terrain_component_t default_values = {
    .color_rgb = { 0.8f, 0.8f, 0.8f },
};

typedef struct mag_terrain_component_manager_t
{
    tm_entity_context_o *ctx;
    tm_allocator_i allocator;

    tm_renderer_backend_i *backend;
    tm_shader_repository_o *shader_repo;
} mag_terrain_component_manager_t;

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

static void add(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data)
{
    mag_terrain_component_t *c = data;

    tm_visibility_context_o *context = tm_single_implementation(tm_global_api_registry, tm_visibility_context_o);
    c->visibility_mask = tm_visibility_flags_api->build_visibility_mask(context, 0, 0);
}

static void destroy_mesh(mag_terrain_component_t *c, mag_terrain_component_manager_t *man)
{
    if (c->mesh.vbuf.resource) {
        tm_renderer_resource_command_buffer_o *res_buf;
        man->backend->create_resource_command_buffers(man->backend->inst, &res_buf, 1);

        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->mesh.ibuf);
        tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, c->mesh.vbuf);

        man->backend->submit_resource_command_buffers(man->backend->inst, &res_buf, 1);
        man->backend->destroy_resource_command_buffers(man->backend->inst, &res_buf, 1);

        c->mesh.vbuf = (tm_renderer_handle_t) { 0 };
        c->mesh.ibuf = (tm_renderer_handle_t) { 0 };
    }
}

static void remove(tm_component_manager_o *manager, struct tm_entity_commands_o *commands, tm_entity_t e, void *data)
{
    mag_terrain_component_t *c = data;
    mag_terrain_component_manager_t *man = (mag_terrain_component_manager_t *)manager;

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
    mag_terrain_component_manager_t *man = (mag_terrain_component_manager_t *)manager;

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
    mag_terrain_component_manager_t *manager = tm_alloc(&a, sizeof(*manager));
    *manager = (mag_terrain_component_manager_t) {
        .ctx = ctx,
        .allocator = a,
        .backend = backend,
        .shader_repo = shader_repo
    };

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

static void generate_region_gpu(mag_voxel_region_t *region, tm_renderer_backend_i *rb, tm_shader_repository_o *shader_repo) {
    TM_INIT_TEMP_ALLOCATOR_WITH_ADAPTER(ta, a);

    tm_renderer_command_buffer_o *cmd_buf;
    rb->create_command_buffers(rb->inst, &cmd_buf, 1);
    tm_renderer_resource_command_buffer_o *res_buf;
    rb->create_resource_command_buffers(rb->inst, &res_buf, 1);

    tm_renderer_handle_t densities_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = sizeof(region->densities), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_densities" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);
    tm_renderer_handle_t normals_handle = tm_renderer_api->tm_renderer_resource_command_buffer_api->create_buffer(res_buf,
        &(tm_renderer_buffer_desc_t) { .size = sizeof(region->normals), .usage_flags = TM_RENDERER_BUFFER_USAGE_STORAGE | TM_RENDERER_BUFFER_USAGE_UAV, .debug_tag = "mag_region_normals" },
        TM_RENDERER_DEVICE_AFFINITY_MASK_ALL);

    tm_shader_o *shader = tm_shader_repository_api->lookup_shader(shader_repo, TM_STATIC_HASH("magnum_terrain_gen_region", 0x3f8b44db04e9fd19ULL));
    tm_shader_io_o *io = tm_shader_api->shader_io(shader);
    tm_shader_resource_binder_instance_t rbinder;
    tm_shader_api->create_resource_binder_instances(io, 1, &rbinder);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("densities", 0x9d97839d5465b483ULL), &densities_handle, 0, 0, 1);
    set_resource(io, res_buf, &rbinder, TM_STATIC_HASH("normals", 0x80b4d6fd3ed93beULL), &normals_handle, 0, 0, 1);
    tm_renderer_shader_info_t shader_info;
    tm_shader_system_context_o *shader_context = tm_shader_system_api->create_context(a, NULL);
    uint64_t cur_sort_key = 0;
    if (tm_shader_api->assemble_shader_infos(shader, 0, 0, shader_context, TM_STRHASH(0), res_buf, 0, &rbinder, 1, &shader_info)) {
        tm_renderer_api->tm_renderer_command_buffer_api->compute_dispatches(cmd_buf, &cur_sort_key, &(tm_renderer_compute_info_t){ .dispatch.group_count = { 1, MAG_VOXEL_REGION_SIZE, MAG_VOXEL_REGION_SIZE } }, &shader_info, 1);
        cur_sort_key += 1;
    }

    uint16_t state = TM_RENDERER_RESOURCE_STATE_COMPUTE_SHADER | TM_RENDERER_RESOURCE_STATE_UAV;
    tm_renderer_api->tm_renderer_command_buffer_api->transition_resources(cmd_buf, cur_sort_key, &(tm_renderer_resource_barrier_t){ .resource_handle = densities_handle, .source_state = state, .destination_state = state }, 1);

    const uint64_t sort_key = UINT64_MAX;
    tm_renderer_api->tm_renderer_command_buffer_api->bind_queue(cmd_buf, sort_key, &(tm_renderer_queue_bind_t){ .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL });
    uint32_t densities_rr = tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, sort_key, &(tm_renderer_read_buffer_t){ .resource_handle = densities_handle, .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, .resource_state = state, .resource_queue = TM_RENDERER_QUEUE_GRAPHICS, .bits = region->densities, .size = sizeof(region->densities) });
    uint32_t normals_rr = tm_renderer_api->tm_renderer_command_buffer_api->read_buffer(cmd_buf, sort_key, &(tm_renderer_read_buffer_t){ .resource_handle = normals_handle, .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, .resource_state = state, .resource_queue = TM_RENDERER_QUEUE_GRAPHICS, .bits = region->normals, .size = sizeof(region->normals) });

    rb->submit_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->destroy_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->submit_command_buffers(rb->inst, &cmd_buf, 1);
    rb->destroy_command_buffers(rb->inst, &cmd_buf, 1);
    tm_shader_api->destroy_resource_binder_instances(io, &rbinder, 1);

    while (!rb->read_complete(rb->inst, densities_rr, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL))
        ;
    while(!rb->read_complete(rb->inst, normals_rr, TM_RENDERER_DEVICE_AFFINITY_MASK_ALL))
        ;

    rb->create_resource_command_buffers(rb->inst, &res_buf, 1);
    tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, densities_handle);
    tm_renderer_api->tm_renderer_resource_command_buffer_api->destroy_resource(res_buf, normals_handle);

    rb->submit_resource_command_buffers(rb->inst, &res_buf, 1);
    rb->destroy_resource_command_buffers(rb->inst, &res_buf, 1);

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void recreate_mesh(mag_terrain_component_t *c, mag_terrain_component_manager_t *man, tm_shader_io_o *io, tm_renderer_backend_i *rb, tm_shader_repository_o *shader_repo)
{
    TM_PROFILER_BEGIN_FUNC_SCOPE();
    destroy_mesh(c, man);

    mag_voxel_region_t region;
    generate_region_gpu(&region, rb, shader_repo);

    // for (int x = 0; x < MAG_VOXEL_REGION_SIZE; ++x) {
    //     for (int y = 0; y < MAG_VOXEL_REGION_SIZE; ++y) {
    //         for (int z = 0; z < MAG_VOXEL_REGION_SIZE; ++z) {
    //             tm_vec3_t world_pos = {
    //                 (float)x - (float)MAG_VOXEL_MARGIN,
    //                 (float)y - (float)MAG_VOXEL_MARGIN,
    //                 (float)z - (float)MAG_VOXEL_MARGIN,
    //             };
    //             const float height = 2.f;
    //             region.densities[x][y][z] = -world_pos.y + height;
    //             region.normals[x][y][z] = (tm_vec3_t) {
    //                 0.f,
    //                 world_pos.y >= height ? 1.f : -1.f,
    //                 0.f
    //             };
    //         }
    //     }
    // }

    mag_voxel_api->dual_contour_region(
        &region,
        man->backend,
        io,
        &c->mesh,
        &c->rbinder,
        &c->cbuffer);

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
        const tm_transform_t *t = &entity_transforms[entity_indices[i]].world;
        tm_mat44_from_translation_quaternion_scale(&dest->tm, t->pos, t->rot, t->scl);
        mag_terrain_component_t *c = cdata[i];

        const float size = (float)MAG_VOXEL_REGION_SIZE;
        dest->visibility_mask = c->visibility_mask;
        dest->min = (tm_vec3_t) { 0 };
        dest->max = (tm_vec3_t) { size, size, size };
        ++dest;
    }

    TM_PROFILER_END_FUNC_SCOPE();
}

static void render(struct tm_component_manager_o *manager, struct tm_render_args_t *args, const tm_ci_render_viewer_t *viewers,
    uint32_t num_viewers, const tm_entity_t *entities, const tm_transform_component_t *entity_transforms, const bool *entity_selection_state, const uint32_t *entity_indices,
    void **render_component_data, uint32_t num_renderables, const uint8_t *frustum_visibility)
{
    mag_terrain_component_manager_t *man = (mag_terrain_component_manager_t *)manager;
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

    tm_shader_system_o *gbuffer_system = tm_shader_repository_api->lookup_system(args->shader_repository, tm_murmur_hash_string("gbuffer_system"));
    tm_shader_system_o *shadow_system = tm_shader_repository_api->lookup_system(args->shader_repository, tm_murmur_hash_string("shadow_system"));

    tm_shader_system_o *vertex_buffer_system = tm_shader_repository_api->lookup_system(args->shader_repository, tm_murmur_hash_string("vertex_buffer_system"));
    tm_shader_io_o *vertex_buffer_io = tm_shader_api->system_io(vertex_buffer_system);

    uint64_t gbuffer_sort_key = args->render_graph ? tm_render_graph_api->sort_key(args->render_graph, tm_murmur_hash_string("gbuffer")) : 0;
    uint64_t shadows_sort_key = 0;

    /* carray */ uint64_t *sort_keys = 0;
    tm_carray_temp_resize(sort_keys, num_viewers * num_renderables, ta);
    uint32_t num_draws = 0;
    for (uint32_t i = 0; i != num_renderables; ++i) {
        mag_terrain_component_t *component = cdata[i];
        recreate_mesh(component, man, vertex_buffer_io, man->backend, args->shader_repository);
        if (!component->mesh.vbuf.resource) {
            continue;
        }

        tm_shader_system_api->activate_system(shader_context, vertex_buffer_system,
            &component->cbuffer, 1,
            &component->rbinder, 1);

        bool updated = false;
        for (uint32_t v = 0; v != num_viewers; ++v) {
            tm_shader_system_api->activate_system(shader_context, viewers[v].viewer_system, viewers[v].viewer_cbuffer, 1, viewers[v].viewer_rbinder, viewers[v].viewer_rbinder ? 1 : 0);
            tm_shader_system_o *context_system = v == 0 ? gbuffer_system : shadow_system;
            tm_shader_system_api->activate_system(shader_context, context_system, 0, 0, 0, 0);

            const uint32_t entity_idx = entity_indices[i];
            if (!updated) {
                tm_mat44_t last_tm = component->local_to_world;
                const tm_transform_t *t = &entity_transforms[entity_idx].world;
                tm_mat44_from_translation_quaternion_scale(&component->local_to_world, t->pos, t->rot, t->scl);

                set_constant(io, args->default_resource_buffer, &cbufs[i], "last_tm", &last_tm, sizeof(last_tm));
                set_constant(io, args->default_resource_buffer, &cbufs[i], "tm", &component->local_to_world, sizeof(component->local_to_world));
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
                .draw_type = TM_RENDERER_DRAW_TYPE_INDEXED,
                .indexed = {
                    .first_index = 0,
                    .num_indices = component->mesh.num_indices,
                    .num_instances = 1 },
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

TM_DLL_EXPORT void tm_load_plugin(struct tm_api_registry_api *reg, bool load)
{
    tm_global_api_registry = reg;

    tm_entity_api = tm_get_api(reg, tm_entity_api);
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

    tm_add_or_remove_implementation(reg, load, tm_the_truth_create_types_i, create_truth_types);
    tm_add_or_remove_implementation(reg, load, tm_entity_create_component_i, create_mag_terrain_component);
}