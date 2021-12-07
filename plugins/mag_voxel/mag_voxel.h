#pragma once
#include <plugins/renderer/renderer_api_types.h>

struct tm_renderer_backend_i;
struct tm_shader_io_o;
struct tm_shader_resource_binder_instance_t;
struct tm_shader_constant_buffer_instance_t;

enum {
    MAG_VOXEL_CHUNK_SIZE = 32,
    MAG_VOXEL_MARGIN = 2,
    MAG_VOXEL_REGION_SIZE = (MAG_VOXEL_CHUNK_SIZE + MAG_VOXEL_MARGIN * 2),
};

typedef struct mag_voxel_region_t
{
    float densities[MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE];
    tm_vec3_t normals[MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE][MAG_VOXEL_REGION_SIZE];
} mag_voxel_region_t;

typedef struct mag_voxel_mesh_t
{
    tm_renderer_handle_t vbuf;
    // indexes are 16 bit
    tm_renderer_handle_t ibuf;
    uint32_t num_indices;
} mag_voxel_mesh_t;

struct mag_voxel_api
{
    void (*dual_contour_region)(
        const mag_voxel_region_t *region,
        struct tm_renderer_backend_i *backend,
        struct tm_shader_io_o *io,
        mag_voxel_mesh_t *out_mesh,
        struct tm_shader_resource_binder_instance_t *inout_rbinder,
        struct tm_shader_constant_buffer_instance_t *inout_cbuffer);
};

#define mag_voxel_api_version TM_VERSION(1, 0, 0)