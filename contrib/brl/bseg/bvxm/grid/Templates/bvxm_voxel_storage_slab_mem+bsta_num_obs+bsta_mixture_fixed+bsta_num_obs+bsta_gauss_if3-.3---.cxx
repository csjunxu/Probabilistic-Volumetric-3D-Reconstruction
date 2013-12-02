#include <bvxm/grid/bvxm_voxel_storage_slab_mem.txx>
#include <bsta/bsta_gauss_if3.h>
#include <bsta/bsta_attributes.h>
#include <bsta/bsta_mixture_fixed.h>

typedef bsta_num_obs<bsta_gauss_if3> gauss_type;
typedef bsta_mixture_fixed<gauss_type, 3> mix_gauss;
typedef bsta_num_obs<mix_gauss> mix_gauss_type;

BVXM_VOXEL_STORAGE_SLAB_MEM_INSTANTIATE(mix_gauss_type);