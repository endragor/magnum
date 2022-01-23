static struct tm_api_registry_api *tm_global_api_registry;
static struct tm_entity_api *tm_entity_api;
static struct tm_simulation_api *tm_simulation_api;
static struct tm_camera_api *tm_camera_api;
static struct tm_ui_api *tm_ui_api;
static struct tm_tag_component_api *tm_tag_component_api;
static struct tm_the_truth_assets_api *tm_the_truth_assets_api;
static struct tm_render_component_api *tm_render_component_api;
static struct tm_transform_component_api *tm_transform_component_api;
static struct tm_creation_graph_api *tm_creation_graph_api;
static struct tm_temp_allocator_api *tm_temp_allocator_api;
static struct tm_input_api *tm_input_api;

static struct mag_terrain_api *mag_terrain_api;

#include <foundation/allocator.h>
#include <foundation/api_registry.h>
#include <foundation/camera.h>
#include <foundation/carray.inl>
#include <foundation/input.h>
#include <foundation/localizer.h>
#include <foundation/math.inl>
#include <foundation/rect.inl>
#include <foundation/temp_allocator.h>
#include <foundation/the_truth.h>
#include <foundation/the_truth_assets.h>

#include <plugins/creation_graph/creation_graph.h>
#include <plugins/entity/entity.h>
#include <plugins/entity/tag_component.h>
#include <plugins/entity/transform_component.h>
#include <plugins/render_utilities/render_component.h>
#include <plugins/renderer/render_backend.h>
#include <plugins/simulation/simulation.h>
#include <plugins/simulation/simulation_entry.h>
#include <plugins/ui/ui.h>
#include <plugins/ui/ui_custom.h>

#include "plugins/mag_terrain_component/mag_terrain_component.h"

#define MAX_OPS_PER_SECOND 20
#define MAX_SCULPT_DISTANCE 60.f

typedef struct input_state_t
{
    tm_vec2_t mouse_delta;
    float mouse_wheel;
    bool held_keys[TM_INPUT_KEYBOARD_ITEM_COUNT];
    bool left_mouse_held;
    bool left_mouse_pressed;
    bool right_mouse_held;
    bool right_mouse_pressed;
} input_state_t;

typedef struct tm_simulation_state_o
{
    tm_allocator_i *allocator;
    tm_simulation_o *simulation_ctx;
    tm_entity_context_o *entity_ctx;
    tm_the_truth_o *tt;
    tm_renderer_backend_i *rb;

    // Contains keyboard and mouse input state.
    input_state_t input;
    uint64_t processed_events;

    tm_component_type_t render_comp_type;
    tm_transform_component_manager_o *transform_man;
    tm_entity_t sphere_aim;
    // either sphere aim or box aim
    tm_entity_t sculpt_aim;
    float sculpt_radius;

    tm_entity_t player;

    mag_terrain_component_manager_o *terrain_mgr;
    double last_op_time;
} tm_simulation_state_o;

static void private__cursor_line(const tm_camera_t *camera, tm_vec2_t mouse_pos, tm_rect_t viewport_r, tm_vec3_t *cursor_pos, tm_vec3_t *cursor_dir)
{
    const tm_vec3_t cursor[2] = { { mouse_pos.x, mouse_pos.y, 0 }, { mouse_pos.x, mouse_pos.y, 1 } };
    tm_vec3_t cursor_world[2];
    tm_camera_api->screen_to_world(camera, TM_CAMERA_TRANSFORM_DEFAULT, viewport_r, cursor, cursor_world, 2);
    *cursor_pos = cursor_world[0];
    *cursor_dir = tm_vec3_normalize(tm_vec3_sub(cursor_world[1], cursor_world[0]));
}

static void update_aim_radius(tm_simulation_state_o *state, tm_entity_t aim, float radius)
{
    TM_INIT_TEMP_ALLOCATOR(ta);
    tm_transform_component_api->set_local_scale(state->transform_man, aim, (tm_vec3_t) { radius, radius, radius });

    // tm_creation_graph_instance_t **instances = tm_creation_graph_api->get_instances_from_component(state->tt, state->entity_ctx, aim, TM_TT_TYPE_HASH__RENDER_COMPONENT, ta);
    // tm_creation_graph_context_t ctx = (tm_creation_graph_context_t) { .rb = state->rb, .device_affinity_mask = TM_RENDERER_DEVICE_AFFINITY_MASK_ALL, .tt = state->tt };

    // for (uint32_t instance_idx = 0; instance_idx < tm_carray_size(instances); ++instance_idx) {
    //     tm_creation_graph_instance_t *instance = instances[instance_idx];
    //     tm_creation_graph_api->set_input_value(instance, &ctx, TM_STATIC_HASH("Radius", 0xabb1bd83748b60e4ULL), &radius, sizeof(radius));
    //     tm_creation_graph_api->refresh_outputs(instance, &ctx);
    // }

    TM_SHUTDOWN_TEMP_ALLOCATOR(ta);
}

static void update_render_visibility(tm_simulation_state_o *state, tm_entity_t e, bool visible)
{
    tm_render_component_public_t *render_comp = tm_entity_api->get_component(state->entity_ctx, e, state->render_comp_type);
    tm_render_component_api->set_draws_enable(render_comp, visible);
}

#define DEFAULT_SCULPT_RADIUS 2.f
#define MIN_SCULPT_RADIUS 2.f
#define MAX_SCULPT_RADIUS 16.f
#define SELF_SCULPT_THRESHOLD 0.5f

static void tick(tm_simulation_state_o *state, tm_simulation_frame_args_t *args)
{
    // Reset per-frame input
    state->input.mouse_delta.x = state->input.mouse_delta.y = state->input.mouse_wheel = 0;
    state->input.left_mouse_pressed = false;

    // Read input
    tm_input_event_t events[32];
    while (true) {
        uint64_t n = tm_input_api->events(state->processed_events, events, 32);
        for (uint64_t i = 0; i < n; ++i) {
            const tm_input_event_t *e = events + i;
            if (e->source && e->source->controller_type == TM_INPUT_CONTROLLER_TYPE_MOUSE) {
                if (e->item_id == TM_INPUT_MOUSE_ITEM_BUTTON_LEFT) {
                    const bool down = e->data.f.x > 0.5f;
                    state->input.left_mouse_pressed = down && !state->input.left_mouse_held;
                    state->input.left_mouse_held = down;
                } else if (e->item_id == TM_INPUT_MOUSE_ITEM_BUTTON_RIGHT) {
                    const bool down = e->data.f.x > 0.5f;
                    state->input.right_mouse_pressed = down && !state->input.right_mouse_held;
                    state->input.right_mouse_held = down;
                } else if (e->item_id == TM_INPUT_MOUSE_ITEM_MOVE) {
                    state->input.mouse_delta.x += e->data.f.x;
                    state->input.mouse_delta.y += e->data.f.y;
                } else if (e->item_id == TM_INPUT_MOUSE_ITEM_WHEEL) {
                    state->input.mouse_wheel += e->data.f.x;
                }
            }
            if (e->source && e->source->controller_type == TM_INPUT_CONTROLLER_TYPE_KEYBOARD) {
                if (e->type == TM_INPUT_EVENT_TYPE_DATA_CHANGE) {
                    state->input.held_keys[e->item_id] = e->data.f.x == 1.0f;
                }
            }
        }
        state->processed_events += n;
        if (n < 32)
            break;
    }

    if (state->input.mouse_wheel != 0.f) {
        state->sculpt_radius = tm_clamp(state->sculpt_radius + state->input.mouse_wheel, MIN_SCULPT_RADIUS, MAX_SCULPT_RADIUS);
        update_aim_radius(state, state->sculpt_aim, state->sculpt_radius);
    }

    if (!args->ui)
        return;

    tm_ui_buffers_t uib = tm_ui_api->buffers(args->ui);

    if (!tm_vec2_in_rect(uib.input->mouse_pos, args->rect))
        return;

    const tm_camera_t *camera = tm_entity_api->get_blackboard_ptr(state->entity_ctx, TM_ENTITY_BB__CAMERA);
    if (!camera || tm_simulation_api->default_camera(state->simulation_ctx).u64 == tm_simulation_api->camera(state->simulation_ctx).u64) {
        update_render_visibility(state, state->sculpt_aim, false);
        return;
    }

    tm_vec3_t cursor_pos;
    tm_vec3_t cursor_dir;
    private__cursor_line(camera, tm_rect_center(args->rect), args->rect, &cursor_pos, &cursor_dir);

    float hit_length;
    bool ray_intersects = mag_terrain_api->cast_ray(state->terrain_mgr, cursor_pos, cursor_dir, MAX_SCULPT_DISTANCE - state->sculpt_radius, &hit_length);
    bool aim_visible = false;
    if (ray_intersects) {
        tm_vec3_t pos = tm_vec3_add(cursor_pos, tm_vec3_mul(cursor_dir, hit_length));
        const tm_vec3_t player_pos = tm_get_position(state->transform_man, state->player);
        const float min_distance = (state->sculpt_radius + SELF_SCULPT_THRESHOLD) * (state->sculpt_radius + SELF_SCULPT_THRESHOLD);
        if (tm_vec3_dist_sqr(pos, player_pos) >= min_distance) {
            if ((state->input.left_mouse_held || state->input.right_mouse_held) && (args->time - state->last_op_time >= 1.0 / MAX_OPS_PER_SECOND)) {
                state->last_op_time = args->time;
                mag_terrain_op_type_t op = state->input.right_mouse_held ? TERRAIN_OP_SUBTRACT : TERRAIN_OP_UNION;
                mag_terrain_api->apply_operation(state->terrain_mgr, op, TERRAIN_OP_SPHERE, pos, (tm_vec3_t) { state->sculpt_radius, state->sculpt_radius, state->sculpt_radius });
            }
            aim_visible = true;
            tm_transform_component_api->set_local_position(state->transform_man, state->sculpt_aim, pos);
        }
    }
    update_render_visibility(state, state->sculpt_aim, aim_visible);
}

static tm_simulation_state_o *start(tm_simulation_start_args_t *args)
{
    tm_simulation_state_o *state = tm_alloc(args->allocator, sizeof(*state));
    *state = (tm_simulation_state_o) {
        .allocator = args->allocator,
        .entity_ctx = args->entity_ctx,
        .simulation_ctx = args->simulation_ctx,
        .last_op_time = -1.0 / MAX_OPS_PER_SECOND,
        .tt = args->tt,
        .rb = tm_first_implementation(tm_global_api_registry, tm_renderer_backend_i),
    };
    tm_component_type_t tag_component = tm_entity_api->lookup_component_type(state->entity_ctx, TM_TT_TYPE_HASH__TAG_COMPONENT);
    tm_tag_component_manager_o *tag_mgr = (tm_tag_component_manager_o *)tm_entity_api->component_manager(state->entity_ctx, tag_component);
    const tm_entity_t camera = tm_tag_component_api->find_first(tag_mgr, TM_STATIC_HASH("camera", 0x60ed8c3931822dc7ULL));
    if (camera.u64)
        tm_simulation_api->set_camera(state->simulation_ctx, camera);

    tm_component_type_t terrain_component = tm_entity_api->lookup_component_type(state->entity_ctx, MAG_TT_TYPE_HASH__TERRAIN_COMPONENT);
    state->terrain_mgr = (mag_terrain_component_manager_o *)tm_entity_api->component_manager(state->entity_ctx, terrain_component);

    tm_tt_id_t sphere_aim_asset = tm_the_truth_assets_api->asset_object_from_path(args->tt, args->asset_root, "sculpt/sphere.entity");
    state->sphere_aim = tm_entity_api->create_entity_from_asset(state->entity_ctx, sphere_aim_asset);
    state->sculpt_aim = state->sphere_aim;

    state->render_comp_type = tm_entity_api->lookup_component_type(state->entity_ctx, TM_TT_TYPE_HASH__RENDER_COMPONENT);
    state->transform_man = (tm_transform_component_manager_o *)tm_entity_api->component_manager_by_hash(state->entity_ctx, TM_TT_TYPE_HASH__TRANSFORM_COMPONENT);

    state->player = tm_tag_component_api->find_first(tag_mgr, TM_STATIC_HASH("player", 0xafff68de8a0598dfULL));

    state->sculpt_radius = DEFAULT_SCULPT_RADIUS;

    update_render_visibility(state, state->sculpt_aim, false);
    update_render_visibility(state, state->sculpt_aim, state->sculpt_radius);

    return state;
}

static void stop(tm_simulation_state_o *state, struct tm_entity_commands_o *commands)
{
    tm_allocator_i a = *state->allocator;
    tm_free(&a, state, sizeof(*state));
}

static tm_simulation_entry_i simulation_entry_i = {
    .id = TM_STATIC_HASH("magnum_free_flight_simulation_entry", 0x376015f09b45c0f2ULL),
    .display_name = TM_LOCALIZE_LATER("Magnum Free Flight"),
    .start = start,
    .stop = stop,
    .tick = tick,
};

TM_DLL_EXPORT void tm_load_plugin(struct tm_api_registry_api *reg, bool load)
{
    tm_global_api_registry = reg;
    tm_entity_api = tm_get_api(reg, tm_entity_api);
    tm_simulation_api = tm_get_api(reg, tm_simulation_api);
    tm_ui_api = tm_get_api(reg, tm_ui_api);
    tm_camera_api = tm_get_api(reg, tm_camera_api);
    tm_tag_component_api = tm_get_api(reg, tm_tag_component_api);
    tm_the_truth_assets_api = tm_get_api(reg, tm_the_truth_assets_api);
    tm_render_component_api = tm_get_api(reg, tm_render_component_api);
    tm_transform_component_api = tm_get_api(reg, tm_transform_component_api);
    tm_creation_graph_api = tm_get_api(reg, tm_creation_graph_api);
    tm_temp_allocator_api = tm_get_api(reg, tm_temp_allocator_api);
    tm_input_api = tm_get_api(reg, tm_input_api);

    mag_terrain_api = tm_get_api(reg, mag_terrain_api);

    tm_add_or_remove_implementation(reg, load, tm_simulation_entry_i, &simulation_entry_i);
}