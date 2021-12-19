static struct tm_entity_api *tm_entity_api;
static struct tm_simulation_api *tm_simulation_api;
static struct tm_camera_api *tm_camera_api;
static struct tm_ui_api *tm_ui_api;
static struct tm_tag_component_api *tm_tag_component_api;

static struct mag_terrain_api *mag_terrain_api;

#include <foundation/allocator.h>
#include <foundation/api_registry.h>
#include <foundation/camera.h>
#include <foundation/localizer.h>
#include <foundation/math.inl>
#include <foundation/rect.inl>

#include <plugins/entity/entity.h>
#include <plugins/entity/tag_component.h>
#include <plugins/simulation/simulation.h>
#include <plugins/simulation/simulation_entry.h>
#include <plugins/ui/ui.h>
#include <plugins/ui/ui_custom.h>

#include "plugins/mag_terrain_component/mag_terrain_component.h"

#define MAX_OPS_PER_SECOND 5
#define MAX_SCULPT_DISTANCE 31.0

typedef struct tm_simulation_state_o
{
    tm_allocator_i *allocator;
    tm_simulation_o *simulation_ctx;
    tm_entity_context_o *entity_ctx;

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

static void tick(tm_simulation_state_o *state, tm_simulation_frame_args_t *args)
{
    if (!args->ui || args->time - state->last_op_time < 1.0 / MAX_OPS_PER_SECOND)
        return;

    tm_ui_buffers_t uib = tm_ui_api->buffers(args->ui);
    if (!uib.input->left_mouse_is_down)
        return;

    if (!tm_vec2_in_rect(uib.input->mouse_pos, args->rect))
        return;

    const tm_camera_t *camera = tm_entity_api->get_blackboard_ptr(state->entity_ctx, TM_ENTITY_BB__CAMERA);
    if (!camera)
        return;

    tm_vec3_t cursor_pos;
    tm_vec3_t cursor_dir;
    private__cursor_line(camera, uib.input->mouse_pos, args->rect, &cursor_pos, &cursor_dir);

    float hit_length;
    if (mag_terrain_api->cast_ray(state->terrain_mgr, cursor_pos, cursor_dir, MAX_SCULPT_DISTANCE, &hit_length)) {
        state->last_op_time = args->time;
        tm_vec3_t pos = tm_vec3_add(cursor_pos, tm_vec3_mul(cursor_dir, hit_length));
        mag_terrain_api->apply_operation(state->terrain_mgr, TERRAIN_OP_UNION, TERRAIN_OP_SPHERE, pos, (tm_vec3_t) { 2, 0, 0 });
    }
}

static tm_simulation_state_o *start(tm_simulation_start_args_t *args)
{
    tm_simulation_state_o *state = tm_alloc(args->allocator, sizeof(*state));
    *state = (tm_simulation_state_o) {
        .allocator = args->allocator,
        .entity_ctx = args->entity_ctx,
        .simulation_ctx = args->simulation_ctx,
        .last_op_time = -1.0 / MAX_OPS_PER_SECOND,
    };
    tm_component_type_t tag_component = tm_entity_api->lookup_component_type(state->entity_ctx, TM_TT_TYPE_HASH__TAG_COMPONENT);
    tm_tag_component_manager_o *tag_mgr = (tm_tag_component_manager_o *)tm_entity_api->component_manager(state->entity_ctx, tag_component);
    const tm_entity_t camera = tm_tag_component_api->find_first(tag_mgr, TM_STATIC_HASH("camera", 0x60ed8c3931822dc7ULL));
    if (camera.u64)
        tm_simulation_api->set_camera(state->simulation_ctx, camera);

    tm_component_type_t terrain_component = tm_entity_api->lookup_component_type(state->entity_ctx, MAG_TT_TYPE_HASH__TERRAIN_COMPONENT);
    state->terrain_mgr = (mag_terrain_component_manager_o *)tm_entity_api->component_manager(state->entity_ctx, terrain_component);

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

    tm_entity_api = tm_get_api(reg, tm_entity_api);
    tm_simulation_api = tm_get_api(reg, tm_simulation_api);
    tm_ui_api = tm_get_api(reg, tm_ui_api);
    tm_camera_api = tm_get_api(reg, tm_camera_api);
    tm_tag_component_api = tm_get_api(reg, tm_tag_component_api);

    mag_terrain_api = tm_get_api(reg, mag_terrain_api);

    tm_add_or_remove_implementation(reg, load, tm_simulation_entry_i, &simulation_entry_i);
}