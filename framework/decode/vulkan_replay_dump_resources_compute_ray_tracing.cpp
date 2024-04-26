/*
** Copyright (c) 2024 LunarG, Inc.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/

#include "decode/vulkan_object_info.h"
#include "decode/vulkan_replay_dump_resources_compute_ray_tracing.h"
#include "format/format.h"
#include "graphics/vulkan_resources_util.h"
#include "util/image_writer.h"
#include "util/buffer_writer.h"
#include "util/logging.h"
#include "util/platform.h"
#include "util/to_string.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <vector>
#include <vulkan/vulkan_core.h>
#if !defined(WIN32)
#include <dirent.h>
#endif

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(decode)

DispatchTraceRaysDumpingContext::DispatchTraceRaysDumpingContext(const std::vector<uint64_t>&   dispatch_indices,
                                                                 const std::vector<uint64_t>&   trace_rays_indices,
                                                                 VulkanObjectInfoTable&         object_info_table,
                                                                 const VulkanReplayOptions&     options,
                                                                 VulkanReplayDumpResourcesJson& dump_json) :
    original_command_buffer_info(nullptr),
    DR_command_buffer(VK_NULL_HANDLE), dispatch_indices(dispatch_indices), trace_rays_indices(trace_rays_indices),
    bound_pipelines{ nullptr }, dump_resources_before(options.dump_resources_before),
    dump_resource_path(options.dump_resources_output_dir), image_file_format(options.dump_resources_image_format),
    dump_resources_scale(options.dump_resources_scale), device_table(nullptr), instance_table(nullptr),
    object_info_table(object_info_table), replay_device_phys_mem_props(nullptr), current_dispatch_index(0),
    current_trace_rays_index(0), dump_json(dump_json), output_json_per_command(options.dump_resources_json_per_command),
    dump_immutable_resources(options.dump_resources_dump_immutable_resources),
    dump_all_image_subresources(options.dump_resources_dump_all_image_subresources)
{}

DispatchTraceRaysDumpingContext::~DispatchTraceRaysDumpingContext()
{
    Release();
}

void DispatchTraceRaysDumpingContext::Release()
{
    if (original_command_buffer_info)
    {
        if (DR_command_buffer != VK_NULL_HANDLE)
        {
            const DeviceInfo* device_info = object_info_table.GetDeviceInfo(original_command_buffer_info->parent_id);
            if (device_info)
            {
                DestroyMutableResourcesClones();

                VkDevice device = device_info->handle;

                assert(device_table);

                const CommandPoolInfo* pool_info =
                    object_info_table.GetCommandPoolInfo(original_command_buffer_info->pool_id);
                assert(pool_info);

                device_table->FreeCommandBuffers(device, pool_info->handle, 1, &DR_command_buffer);
                DR_command_buffer = VK_NULL_HANDLE;
            }
        }

        original_command_buffer_info = nullptr;
    }
}

VkResult DispatchTraceRaysDumpingContext::CloneCommandBuffer(CommandBufferInfo*                 orig_cmd_buf_info,
                                                             const encode::VulkanDeviceTable*   dev_table,
                                                             const encode::VulkanInstanceTable* inst_table)
{
    assert(orig_cmd_buf_info);
    assert(dev_table);
    assert(inst_table);

    const CommandPoolInfo* cb_pool_info = object_info_table.GetCommandPoolInfo(orig_cmd_buf_info->pool_id);

    const VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          nullptr,
                                          cb_pool_info->handle,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          1 };

    const DeviceInfo* dev_info = object_info_table.GetDeviceInfo(orig_cmd_buf_info->parent_id);

    VkResult res = dev_table->AllocateCommandBuffers(dev_info->handle, &ai, &DR_command_buffer);
    if (res != VK_SUCCESS)
    {
        GFXRECON_LOG_ERROR("AllocateCommandBuffers failed with %s", util::ToString<VkResult>(res).c_str());
        return res;
    }

    const VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr };
    dev_table->BeginCommandBuffer(DR_command_buffer, &bi);

    assert(original_command_buffer_info == nullptr);
    original_command_buffer_info = orig_cmd_buf_info;

    assert(device_table == nullptr);
    device_table = dev_table;
    assert(instance_table == nullptr);
    instance_table = inst_table;

    const DeviceInfo* device_info = object_info_table.GetDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info->parent_id != format::kNullHandleId);
    const PhysicalDeviceInfo* phys_dev_info = object_info_table.GetPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    assert(phys_dev_info->replay_device_info);
    assert(phys_dev_info->replay_device_info->memory_properties.get());
    replay_device_phys_mem_props = phys_dev_info->replay_device_info->memory_properties.get();

    return VK_SUCCESS;
}

void DispatchTraceRaysDumpingContext::FinalizeCommandBuffer(bool is_dispatch)
{
    if (is_dispatch)
    {
        ++current_dispatch_index;
    }
    else
    {
        ++current_trace_rays_index;
    }

    if (!IsRecording())
    {
        assert((dump_resources_before ? (current_dispatch_index / 2) : current_dispatch_index) ==
                   dispatch_indices.size() &&
               (dump_resources_before ? (current_trace_rays_index / 2) : current_trace_rays_index) ==
                   trace_rays_indices.size());
        assert(DR_command_buffer != VK_NULL_HANDLE);

        device_table->EndCommandBuffer(DR_command_buffer);
    }
}

void DispatchTraceRaysDumpingContext::BindPipeline(VkPipelineBindPoint bind_point, const PipelineInfo* pipeline)
{
    PipelineBindPoints point = VkPipelineBindPointToPipelineBindPoint(bind_point);
    bound_pipelines[point]   = pipeline;
}

void DispatchTraceRaysDumpingContext::BindDescriptorSets(
    VkPipelineBindPoint                          pipeline_bind_point,
    uint32_t                                     first_set,
    const std::vector<const DescriptorSetInfo*>& descriptor_sets_infos,
    uint32_t                                     dynamicOffsetCount,
    const uint32_t*                              pDynamicOffsets)
{
    PipelineBindPoints bind_point = VkPipelineBindPointToPipelineBindPoint(pipeline_bind_point);

    uint32_t dynamic_offset_index = 0;
    for (size_t i = 0; i < descriptor_sets_infos.size(); ++i)
    {
        uint32_t set_index = first_set + i;

        DescriptorSetInfo* bound_descriptor_sets;
        if (bind_point == kBindPoint_compute)
        {
            bound_descriptor_sets = &bound_descriptor_sets_compute[set_index];
        }
        else
        {
            assert(bind_point == kBindPoint_ray_tracing);
            bound_descriptor_sets = &bound_descriptor_sets_ray_tracing[set_index];
        }

        *bound_descriptor_sets = *descriptor_sets_infos[i];

        if (dynamicOffsetCount && pDynamicOffsets != nullptr)
        {
            for (const auto& binding : descriptor_sets_infos[i]->descriptors)
            {
                const uint32_t bindind_index = binding.first;

                if (binding.second.desc_type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
                    binding.second.desc_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                {
                    for (size_t ai = 0; ai < bound_descriptor_sets->descriptors[bindind_index].buffer_info.size(); ++ai)
                    {
                        bound_descriptor_sets->descriptors[bindind_index].buffer_info[ai].offset +=
                            pDynamicOffsets[dynamic_offset_index];
                        ++dynamic_offset_index;
                    }
                }
            }
        }
    }

    assert((dynamic_offset_index == dynamicOffsetCount && pDynamicOffsets != nullptr) || (!dynamic_offset_index));
}

bool DispatchTraceRaysDumpingContext::MustDumpDispatch(uint64_t index) const
{
    // Indices should be sorted
    if (!IsInsideRange(dispatch_indices, index))
    {
        return false;
    }

    for (size_t i = dump_resources_before ? current_dispatch_index / 2 : current_dispatch_index;
         i < dispatch_indices.size();
         ++i)
    {
        if (index == dispatch_indices[i])
        {
            return true;
        }
        else if (index > dispatch_indices[i])
        {
            // Indices should be sorted
            return false;
        }
    }

    return false;
}

bool DispatchTraceRaysDumpingContext::MustDumpTraceRays(uint64_t index) const
{
    // Indices should be sorted
    if (!IsInsideRange(trace_rays_indices, index))
    {
        return false;
    }

    for (size_t i = dump_resources_before ? current_trace_rays_index / 2 : current_trace_rays_index;
         i < trace_rays_indices.size();
         ++i)
    {
        if (index == trace_rays_indices[i])
        {
            return true;
        }
        else if (index > trace_rays_indices[i])
        {
            // Indices should be sorted
            return false;
        }
    }

    return false;
}

void DispatchTraceRaysDumpingContext::CopyBufferResource(const BufferInfo* src_buffer_info, VkBuffer dst_buffer)
{
    assert(src_buffer_info);
    assert(dst_buffer != VK_NULL_HANDLE);
}

void DispatchTraceRaysDumpingContext::CopyImageResource(const ImageInfo* src_image_info, VkImage dst_image)
{
    assert(src_image_info != nullptr);
    assert(dst_image != VK_NULL_HANDLE);

    VkImageLayout old_layout;
    assert(original_command_buffer_info != nullptr);
    const auto img_layout_entry = original_command_buffer_info->image_layout_barriers.find(src_image_info->capture_id);
    if (img_layout_entry != original_command_buffer_info->image_layout_barriers.end())
    {
        old_layout = img_layout_entry->second;
    }
    else
    {
        old_layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // Make sure any potential writes are complete and transition image to TRANSFER_SRC_OPTIMAL layout
    VkImageMemoryBarrier img_barrier;
    img_barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    img_barrier.pNext               = nullptr;
    img_barrier.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
    img_barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    img_barrier.oldLayout           = old_layout;
    img_barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    img_barrier.srcQueueFamilyIndex = src_image_info->queue_family_index;
    img_barrier.dstQueueFamilyIndex = src_image_info->queue_family_index;
    img_barrier.image               = src_image_info->handle;
    img_barrier.subresourceRange    = {
        graphics::GetFormatAspectMask(src_image_info->format), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS
    };

    assert(device_table != nullptr);
    device_table->CmdPipelineBarrier(DR_command_buffer,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VkDependencyFlags(0),
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &img_barrier);

    // Transition destination image
    img_barrier.srcAccessMask = VK_ACCESS_NONE;
    img_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    img_barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    img_barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    img_barrier.image         = dst_image;

    device_table->CmdPipelineBarrier(DR_command_buffer,
                                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VkDependencyFlags(0),
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &img_barrier);

    assert(src_image_info->level_count);
    assert(src_image_info->layer_count);

    std::vector<VkImageCopy> copies(src_image_info->level_count, VkImageCopy());
    VkImageCopy              copy;
    copy.srcSubresource.aspectMask     = graphics::GetFormatAspectMask(src_image_info->format);
    copy.srcSubresource.baseArrayLayer = 0;
    copy.srcSubresource.layerCount     = src_image_info->layer_count;
    copy.srcOffset.x                   = 0;
    copy.srcOffset.y                   = 0;
    copy.srcOffset.z                   = 0;

    copy.dstSubresource.aspectMask     = graphics::GetFormatAspectMask(src_image_info->format);
    copy.dstSubresource.baseArrayLayer = 0;
    copy.dstSubresource.layerCount     = src_image_info->layer_count;
    copy.dstOffset.x                   = 0;
    copy.dstOffset.y                   = 0;
    copy.dstOffset.z                   = 0;

    for (uint32_t i = 0; i < src_image_info->level_count; ++i)
    {
        copy.srcSubresource.mipLevel = i;
        copy.dstSubresource.mipLevel = i;
        copy.extent.width            = (src_image_info->extent.width << i);
        copy.extent.height           = (src_image_info->extent.height << i);
        copy.extent.depth            = (src_image_info->extent.depth << i);

        copies[i] = copy;
    }

    device_table->CmdCopyImage(DR_command_buffer,
                               src_image_info->handle,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dst_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               copies.size(),
                               copies.data());

    // Wait for transfer and transition source image back to previous layout
    img_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    img_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    img_barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    img_barrier.newLayout     = old_layout;
    img_barrier.image         = src_image_info->handle;

    device_table->CmdPipelineBarrier(DR_command_buffer,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     VkDependencyFlags(0),
                                     0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     1,
                                     &img_barrier);
}

VkResult DispatchTraceRaysDumpingContext::CloneDispatchMutableResources(uint64_t index, bool cloning_before_cmd)
{
    auto entry = dispatch_params.find(index);
    assert(entry != dispatch_params.end());

    DispatchParameters& params = entry->second;
    return CloneMutableResources(
        cloning_before_cmd ? params.mutable_resources_clones_before : params.mutable_resources_clones, true);
}

VkResult DispatchTraceRaysDumpingContext::CloneTraceRaysMutableResources(uint64_t index, bool cloning_before_cmd)
{
    auto entry = trace_rays_params.find(index);
    assert(entry != trace_rays_params.end());

    TraceRaysParameters& params = entry->second;
    return CloneMutableResources(
        cloning_before_cmd ? params.mutable_resources_clones_before : params.mutable_resources_clones, false);
}

void DispatchTraceRaysDumpingContext::SnapshotBoundDescriptorsDispatch(uint64_t index)
{
    auto params_entry = dispatch_params.find(index);
    assert(params_entry != dispatch_params.end());
    SnapshotBoundDescriptors(params_entry->second);
}

void DispatchTraceRaysDumpingContext::SnapshotBoundDescriptorsTraceRays(uint64_t index)
{
    auto params_entry = trace_rays_params.find(index);
    assert(params_entry != trace_rays_params.end());
    SnapshotBoundDescriptors(params_entry->second);
}

VkResult DispatchTraceRaysDumpingContext::CloneMutableResources(MutableResourcesBackupContext& resource_backup_context,
                                                                bool                           is_dispatch)
{
    assert(IsRecording());

    // Scan for mutable resources in the bound pipeline
    const uint32_t      bind_point = static_cast<uint32_t>(is_dispatch ? kBindPoint_compute : kBindPoint_ray_tracing);
    const PipelineInfo* pipeline   = bound_pipelines[bind_point];
    assert(pipeline != nullptr);

    for (const auto& shader : pipeline->shaders)
    {
        for (const auto& shader_desc_set : shader.second.used_descriptors_info)
        {
            const uint32_t desc_set_index = shader_desc_set.first;

            for (const auto& shader_desc_binding : shader_desc_set.second)
            {
                // Search for resources that are not marked as read only
                if (shader_desc_binding.second.accessed && !shader_desc_binding.second.readonly)
                {
                    const uint32_t binding_index = shader_desc_binding.first;

                    const DescriptorSetInfo* bound_descriptor_sets =
                        is_dispatch ? &bound_descriptor_sets_compute[desc_set_index]
                                    : &bound_descriptor_sets_ray_tracing[desc_set_index];
                    assert(bound_descriptor_sets != nullptr);

                    const auto& bound_desc_binding = bound_descriptor_sets->descriptors.find(binding_index);
                    assert(bound_desc_binding != bound_descriptor_sets->descriptors.end());
                    assert(CheckDescriptorCompatibility(bound_desc_binding->second.desc_type,
                                                        shader_desc_binding.second.type));

                    switch (shader_desc_binding.second.type)
                    {
                        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        {
                            for (const auto& img_desc : bound_desc_binding->second.image_info)
                            {
                                if (img_desc.image_view_info == nullptr)
                                {
                                    continue;
                                }

                                const ImageInfo* img_info =
                                    object_info_table.GetImageInfo(img_desc.image_view_info->image_id);
                                assert(img_info);

                                VkImage*        new_img_ptr        = nullptr;
                                VkDeviceMemory* new_img_memory_ptr = nullptr;

                                resource_backup_context.original_images.push_back(img_info);
                                resource_backup_context.image_shader_stage.push_back(shader.first);
                                resource_backup_context.image_desc_set_binding_pair.push_back(
                                    std::make_pair(desc_set_index, binding_index));
                                resource_backup_context.image_desc_types.push_back(shader_desc_binding.second.type);

                                new_img_ptr = &*(resource_backup_context.images.insert(
                                    resource_backup_context.images.end(), VK_NULL_HANDLE));

                                new_img_memory_ptr = &*(resource_backup_context.image_memories.insert(
                                    resource_backup_context.image_memories.end(), VK_NULL_HANDLE));

                                assert(new_img_ptr != nullptr);
                                assert(new_img_memory_ptr != nullptr);

                                VkResult res = CloneImage(object_info_table,
                                                          device_table,
                                                          replay_device_phys_mem_props,
                                                          img_info,
                                                          new_img_ptr,
                                                          new_img_memory_ptr);
                                if (res != VK_SUCCESS)
                                {
                                    GFXRECON_LOG_ERROR("Cloning image resource %" PRIu64 " failed (%s)",
                                                       img_info->capture_id,
                                                       util::ToString<VkResult>(res).c_str())
                                    return res;
                                }

                                CopyImageResource(img_info, *new_img_ptr);
                            }
                        }
                        break;

                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                        {
                            for (const auto& buf_desc : bound_desc_binding->second.buffer_info)
                            {
                                const BufferInfo* buf_info = buf_desc.buffer_info;
                                if (buf_info == nullptr)
                                {
                                    continue;
                                }

                                VkBuffer*       new_buf_ptr        = nullptr;
                                VkDeviceMemory* new_buf_memory_ptr = nullptr;

                                resource_backup_context.original_buffers.push_back(buf_info);
                                resource_backup_context.buffer_shader_stage.push_back(shader.first);
                                resource_backup_context.buffer_desc_set_binding_pair.push_back(
                                    std::make_pair(desc_set_index, binding_index));
                                resource_backup_context.buffer_desc_types.push_back(shader_desc_binding.second.type);

                                new_buf_ptr = &*(resource_backup_context.buffers.insert(
                                    resource_backup_context.buffers.end(), VK_NULL_HANDLE));

                                new_buf_memory_ptr = &*(resource_backup_context.buffer_memories.insert(
                                    resource_backup_context.buffer_memories.end(), VK_NULL_HANDLE));

                                assert(new_buf_ptr != nullptr);
                                assert(new_buf_memory_ptr != nullptr);

                                VkResult res = CloneBuffer(object_info_table,
                                                           device_table,
                                                           replay_device_phys_mem_props,
                                                           buf_info,
                                                           new_buf_ptr,
                                                           new_buf_memory_ptr);

                                if (res != VK_SUCCESS)
                                {
                                    GFXRECON_LOG_ERROR("Cloning buffer resource %" PRIu64 " failed (%s)",
                                                       buf_info->capture_id,
                                                       util::ToString<VkResult>(res).c_str())
                                    return res;
                                }

                                CopyBufferResource(buf_info, *new_buf_ptr);
                            }
                        }
                        break;

                        case VK_DESCRIPTOR_TYPE_SAMPLER:
                        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                            // These are read only resources
                            break;

                        default:
                            GFXRECON_LOG_WARNING_ONCE(
                                "%s(): Descriptor type (%s) not handled",
                                __func__,
                                util::ToString<VkDescriptorType>(shader_desc_binding.second.type).c_str());
                            break;
                    }
                }
            }
        }
    }

    return VK_SUCCESS;
}

void DispatchTraceRaysDumpingContext::DestroyMutableResourcesClones()
{
    for (auto& dis_params : dispatch_params)
    {
        assert(dis_params.second.mutable_resources_clones.original_images.size() ==
               dis_params.second.mutable_resources_clones.images.size());
        assert(dis_params.second.mutable_resources_clones.original_images.size() ==
               dis_params.second.mutable_resources_clones.image_memories.size());

        if (dump_resources_before)
        {
            assert(dis_params.second.mutable_resources_clones_before.original_images.size() ==
                   dis_params.second.mutable_resources_clones_before.images.size());
            assert(dis_params.second.mutable_resources_clones_before.original_images.size() ==
                   dis_params.second.mutable_resources_clones_before.image_memories.size());
        }

        for (size_t i = 0; i < dis_params.second.mutable_resources_clones.original_images.size(); ++i)
        {
            const DeviceInfo* device_info = object_info_table.GetDeviceInfo(
                dis_params.second.mutable_resources_clones.original_images[i]->parent_id);
            assert(device_info != nullptr);
            VkDevice device = device_info->handle;

            device_table->FreeMemory(device, dis_params.second.mutable_resources_clones.image_memories[i], nullptr);
            device_table->DestroyImage(device, dis_params.second.mutable_resources_clones.images[i], nullptr);

            if (dump_resources_before)
            {
                device_table->FreeMemory(
                    device, dis_params.second.mutable_resources_clones_before.image_memories[i], nullptr);
                device_table->DestroyImage(
                    device, dis_params.second.mutable_resources_clones_before.images[i], nullptr);
            }
        }

        assert(dis_params.second.mutable_resources_clones.original_buffers.size() ==
               dis_params.second.mutable_resources_clones.buffers.size());
        assert(dis_params.second.mutable_resources_clones.original_buffers.size() ==
               dis_params.second.mutable_resources_clones.buffer_memories.size());

        if (dump_resources_before)
        {
            assert(dis_params.second.mutable_resources_clones_before.original_buffers.size() ==
                   dis_params.second.mutable_resources_clones_before.buffers.size());
            assert(dis_params.second.mutable_resources_clones_before.original_buffers.size() ==
                   dis_params.second.mutable_resources_clones_before.buffer_memories.size());
        }

        for (size_t i = 0; i < dis_params.second.mutable_resources_clones.original_buffers.size(); ++i)
        {
            const DeviceInfo* device_info = object_info_table.GetDeviceInfo(
                dis_params.second.mutable_resources_clones.original_buffers[i]->parent_id);
            assert(device_info != nullptr);
            VkDevice device = device_info->handle;

            device_table->FreeMemory(device, dis_params.second.mutable_resources_clones.buffer_memories[i], nullptr);
            device_table->DestroyBuffer(device, dis_params.second.mutable_resources_clones.buffers[i], nullptr);
            if (dump_resources_before)
            {
                device_table->FreeMemory(
                    device, dis_params.second.mutable_resources_clones_before.buffer_memories[i], nullptr);
                device_table->DestroyBuffer(
                    device, dis_params.second.mutable_resources_clones_before.buffers[i], nullptr);
            }
        }
    }

    for (auto& tr_params : trace_rays_params)
    {
        assert(tr_params.second.mutable_resources_clones.original_images.size() ==
               tr_params.second.mutable_resources_clones.images.size());
        assert(tr_params.second.mutable_resources_clones.original_images.size() ==
               tr_params.second.mutable_resources_clones.image_memories.size());

        if (dump_resources_before)
        {
            assert(tr_params.second.mutable_resources_clones_before.original_images.size() ==
                   tr_params.second.mutable_resources_clones_before.images.size());
            assert(tr_params.second.mutable_resources_clones_before.original_images.size() ==
                   tr_params.second.mutable_resources_clones_before.image_memories.size());
        }

        for (size_t i = 0; i < tr_params.second.mutable_resources_clones.original_images.size(); ++i)
        {
            const DeviceInfo* device_info = object_info_table.GetDeviceInfo(
                tr_params.second.mutable_resources_clones.original_images[i]->parent_id);
            assert(device_info != nullptr);
            VkDevice device = device_info->handle;

            device_table->FreeMemory(device, tr_params.second.mutable_resources_clones.image_memories[i], nullptr);
            device_table->DestroyImage(device, tr_params.second.mutable_resources_clones.images[i], nullptr);

            if (dump_resources_before)
            {
                device_table->FreeMemory(
                    device, tr_params.second.mutable_resources_clones_before.image_memories[i], nullptr);
                device_table->DestroyImage(device, tr_params.second.mutable_resources_clones_before.images[i], nullptr);
            }
        }

        assert(tr_params.second.mutable_resources_clones.original_buffers.size() ==
               tr_params.second.mutable_resources_clones.buffers.size());
        assert(tr_params.second.mutable_resources_clones.original_buffers.size() ==
               tr_params.second.mutable_resources_clones.buffer_memories.size());

        if (dump_resources_before)
        {
            assert(tr_params.second.mutable_resources_clones_before.original_buffers.size() ==
                   tr_params.second.mutable_resources_clones_before.buffers.size());
            assert(tr_params.second.mutable_resources_clones_before.original_buffers.size() ==
                   tr_params.second.mutable_resources_clones_before.buffer_memories.size());
        }

        for (size_t i = 0; i < tr_params.second.mutable_resources_clones.original_buffers.size(); ++i)
        {
            const DeviceInfo* device_info = object_info_table.GetDeviceInfo(
                tr_params.second.mutable_resources_clones.original_buffers[i]->parent_id);
            assert(device_info != nullptr);
            VkDevice device = device_info->handle;

            device_table->FreeMemory(device, tr_params.second.mutable_resources_clones.buffer_memories[i], nullptr);
            device_table->DestroyBuffer(device, tr_params.second.mutable_resources_clones.buffers[i], nullptr);
            if (dump_resources_before)
            {
                device_table->FreeMemory(
                    device, tr_params.second.mutable_resources_clones_before.buffer_memories[i], nullptr);
                device_table->DestroyBuffer(
                    device, tr_params.second.mutable_resources_clones_before.buffers[i], nullptr);
            }
        }
    }
}

VkResult DispatchTraceRaysDumpingContext::DumpDispatchTraceRays(
    VkQueue queue, uint64_t qs_index, uint64_t bcb_index, const VkSubmitInfo& submit_info, VkFence fence)
{
    VkSubmitInfo si;
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.pNext                = nullptr;
    si.waitSemaphoreCount   = submit_info.waitSemaphoreCount;
    si.pWaitSemaphores      = submit_info.pWaitSemaphores;
    si.pWaitDstStageMask    = submit_info.pWaitDstStageMask;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &DR_command_buffer;
    si.signalSemaphoreCount = submit_info.signalSemaphoreCount;
    si.pSignalSemaphores    = submit_info.pSignalSemaphores;

    const DeviceInfo* device_info = object_info_table.GetDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    VkResult res = VK_SUCCESS;

    const VkFenceCreateInfo ci               = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };
    VkFence                 submission_fence = VK_NULL_HANDLE;
    if (fence == VK_NULL_HANDLE)
    {
        res = device_table->CreateFence(device_info->handle, &ci, nullptr, &submission_fence);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("CreateFence failed with %s", util::ToString<VkResult>(res).c_str());
            return res;
        }
    }
    else
    {
        submission_fence = fence;
    }

    res = device_table->QueueSubmit(queue, 1, &si, submission_fence);
    if (res != VK_SUCCESS)
    {
        device_table->DestroyFence(device_info->handle, submission_fence, nullptr);
        GFXRECON_LOG_ERROR(
            "(%s:%u) QueueSubmit failed with %s", __FILE__, __LINE__, util::ToString<VkResult>(res).c_str());
        return res;
    }

    // Wait
    res = device_table->WaitForFences(device_info->handle, 1, &submission_fence, VK_TRUE, ~0UL);
    if (res != VK_SUCCESS)
    {
        device_table->DestroyFence(device_info->handle, submission_fence, nullptr);
        GFXRECON_LOG_ERROR("WaitForFences failed with %s", util::ToString<VkResult>(res).c_str());
        return res;
    }

    if (fence == VK_NULL_HANDLE)
    {
        device_table->DestroyFence(device_info->handle, submission_fence, nullptr);
    }

    res = FetchIndirectParams();
    if (res != VK_SUCCESS)
    {
        GFXRECON_LOG_ERROR("Fetching params for indirect calls failed (%s).", util::ToString<VkResult>(res).c_str())
        return res;
    }

    for (auto disp_index : dispatch_indices)
    {
        GFXRECON_LOG_INFO("Dumping mutable resources for dispatch index %" PRIu64, disp_index);

        res = DumpMutableResources(bcb_index, disp_index, qs_index, true);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Dumping compute mutable resources failed (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }
    }

    for (auto tr_index : trace_rays_indices)
    {
        GFXRECON_LOG_INFO("Dumping mutable resources for trace rays index %" PRIu64, tr_index);

        res = DumpMutableResources(bcb_index, tr_index, qs_index, false);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Dumping ray tracing mutable resources failed. (%s)",
                               util::ToString<VkResult>(res).c_str())
            return res;
        }
    }

    if (dump_immutable_resources)
    {
        res = DumpImmutableResources(qs_index, bcb_index);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Dumping immutable resources failed (%s).", util::ToString<VkResult>(res).c_str())
            return res;
        }
    }

    GenerateOutputJson(qs_index, bcb_index);

    assert(res == VK_SUCCESS);
    return VK_SUCCESS;
}

std::vector<std::string>
DispatchTraceRaysDumpingContext::GenerateDispatchTraceRaysImageFilename(VkFormat              format,
                                                                        uint32_t              levels,
                                                                        uint32_t              layers,
                                                                        bool                  is_dispatch,
                                                                        uint64_t              index,
                                                                        uint32_t              desc_set,
                                                                        uint32_t              desc_binding,
                                                                        VkShaderStageFlagBits stage,
                                                                        bool                  before_cmd,
                                                                        bool dump_all_subresources) const
{
    const util::imagewriter::DataFormats output_image_format = VkFormatToImageWriterDataFormat(format);

    const std::string shader_stage_name_whole = util::ToString<VkShaderStageFlagBits>(stage);
    std::string       shader_stage_name;
    if (!shader_stage_name_whole.compare(shader_stage_name_whole.size() - 4, 4, "_BIT"))
    {
        shader_stage_name = shader_stage_name_whole.substr(16, shader_stage_name_whole.size() - 20);
    }
    else if (!shader_stage_name_whole.compare(shader_stage_name_whole.size() - 8, 8, "_BIT_KHR"))
    {
        shader_stage_name = shader_stage_name_whole.substr(16, shader_stage_name_whole.size() - 24);
    }

    std::vector<VkImageAspectFlagBits> aspects;
    bool                               combined_depth_stencil;
    graphics::GetFormatAspects(format, &aspects, &combined_depth_stencil);

    const uint32_t total_files = dump_all_subresources ? (levels * layers * aspects.size()) : (aspects.size());

    std::vector<std::string> filenames(total_files);

    uint32_t f = 0;
    for (size_t a = 0; a < aspects.size(); ++a)
    {
        std::string aspect_str_whole(util::ToString<VkImageAspectFlagBits>(aspects[a]));
        std::string aspect_str(aspect_str_whole.begin() + 16, aspect_str_whole.end() - 4);

        std::stringstream filename;
        if (before_cmd)
        {
            filename << (is_dispatch ? "Dispatch_" : "TraceRays_") << index << "_before_stage_" << shader_stage_name
                     << "_set_" << desc_set << "_binding_" << desc_binding;
            if (output_image_format != util::imagewriter::DataFormats::kFormat_UNSPECIFIED)
            {
                filename << "_" << util::ToString<VkFormat>(format).c_str();
            }
            filename << "_aspect_" << aspect_str;
        }
        else
        {
            filename << (is_dispatch ? "Dispatch_" : "TraceRays_") << index << "_"
                     << (dump_resources_before ? "after_" : "") << "stage_" << shader_stage_name << "_set_" << desc_set
                     << "_binding_" << desc_binding;
            if (output_image_format != util::imagewriter::DataFormats::kFormat_UNSPECIFIED)
            {
                filename << "_" << util::ToString<VkFormat>(format).c_str();
            }
            filename << "_aspect_" << aspect_str;
        }

        if (dump_all_subresources)
        {
            for (uint32_t level = 0; level < levels; ++level)
            {
                for (uint32_t layer = 0; layer < layers; ++layer)
                {
                    filename << "_mip_" << level << "_layer_" << layer;
                }
            }
        }

        std::filesystem::path filedirname(dump_resource_path);
        std::filesystem::path filebasename(filename.str());
        filenames[f++] = (filedirname / filebasename).string();
    }

    return filenames;
}

std::string DispatchTraceRaysDumpingContext::GenerateDispatchTraceRaysBufferFilename(bool                  is_dispatch,
                                                                                     uint64_t              index,
                                                                                     uint32_t              desc_set,
                                                                                     uint32_t              desc_binding,
                                                                                     VkShaderStageFlagBits stage,
                                                                                     bool before_cmd) const
{
    std::stringstream filename;

    const std::string shader_stage_name_whole = util::ToString<VkShaderStageFlagBits>(stage);
    std::string       shader_stage_name;
    if (!shader_stage_name_whole.compare(shader_stage_name_whole.size() - 4, 4, "_BIT"))
    {
        shader_stage_name = shader_stage_name_whole.substr(16, shader_stage_name_whole.size() - 20);
    }
    else if (!shader_stage_name_whole.compare(shader_stage_name_whole.size() - 8, 8, "_BIT_KHR"))
    {
        shader_stage_name = shader_stage_name_whole.substr(16, shader_stage_name_whole.size() - 24);
    }

    if (before_cmd)
    {
        filename << (is_dispatch ? "Dispatch_" : "TraceRays_") << index << "_before_stage_" << shader_stage_name
                 << "_set_" << desc_set << "_binding_" << desc_binding << "_buffer.bin";
    }
    else
    {
        filename << (is_dispatch ? "Dispatch_" : "TraceRays_") << index << "_"
                 << (dump_resources_before ? "after_" : "") << "stage_" << shader_stage_name << "_set_" << desc_set
                 << "_binding_" << desc_binding << "_buffer.bin";
    }

    std::filesystem::path filedirname(dump_resource_path);
    std::filesystem::path filebasename(filename.str());
    return (filedirname / filebasename).string();
}

VkResult DispatchTraceRaysDumpingContext::DumpMutableResources(uint64_t bcb_index,
                                                               uint64_t index,
                                                               uint64_t qs_index,
                                                               bool     is_dispatch)
{
    const auto dis_params = dispatch_params.find(index);
    const auto tr_params  = trace_rays_params.find(index);

    if (is_dispatch && (dis_params == dispatch_params.end()))
    {
        assert(0);
        return VK_SUCCESS;
    }
    else if (!is_dispatch && (tr_params == trace_rays_params.end()))
    {
        assert(0);
        return VK_SUCCESS;
    }

    const MutableResourcesBackupContext& mutable_resources_clones =
        is_dispatch ? dis_params->second.mutable_resources_clones : tr_params->second.mutable_resources_clones;

    const MutableResourcesBackupContext& mutable_resources_clones_before =
        is_dispatch ? dis_params->second.mutable_resources_clones_before
                    : tr_params->second.mutable_resources_clones_before;

    if (mutable_resources_clones.images.empty() && mutable_resources_clones.buffers.empty())
    {
        assert(mutable_resources_clones_before.images.empty() && mutable_resources_clones_before.buffers.empty());
        return VK_SUCCESS;
    }

    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const DeviceInfo* device_info = object_info_table.GetDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    const PhysicalDeviceInfo* phys_dev_info = object_info_table.GetPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    graphics::VulkanResourcesUtil resource_util(device_info->handle,
                                                device_info->parent,
                                                *device_table,
                                                *instance_table,
                                                *phys_dev_info->replay_device_info->memory_properties);

    if (dump_resources_before)
    {
        // Dump images
        for (size_t i = 0; i < mutable_resources_clones_before.original_images.size(); ++i)
        {
            assert(mutable_resources_clones_before.original_images[i] != nullptr);

            ImageInfo modified_image_info = *mutable_resources_clones_before.original_images[i];
            modified_image_info.handle    = mutable_resources_clones_before.images[i];

            const uint32_t              desc_set = mutable_resources_clones_before.image_desc_set_binding_pair[i].first;
            const uint32_t              binding = mutable_resources_clones_before.image_desc_set_binding_pair[i].second;
            const VkShaderStageFlagBits stage   = mutable_resources_clones_before.image_shader_stage[i];

            std::vector<std::string> filenames = GenerateDispatchTraceRaysImageFilename(modified_image_info.format,
                                                                                        modified_image_info.level_count,
                                                                                        modified_image_info.layer_count,
                                                                                        is_dispatch,
                                                                                        index,
                                                                                        desc_set,
                                                                                        binding,
                                                                                        stage,
                                                                                        true,
                                                                                        dump_all_image_subresources);

            VkResult res = DumpImageToFile(&modified_image_info,
                                           device_info,
                                           device_table,
                                           instance_table,
                                           object_info_table,
                                           filenames,
                                           dump_resources_scale,
                                           image_file_format,
                                           false,
                                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            if (res != VK_SUCCESS)
            {
                GFXRECON_LOG_ERROR("Dumping image failed (%s)", util::ToString<VkResult>(res).c_str())
                return res;
            }
        }

        // Dump buffers
        for (size_t i = 0; i < mutable_resources_clones_before.original_buffers.size(); ++i)
        {
            const BufferInfo*    buffer_info = mutable_resources_clones_before.original_buffers[i];
            std::vector<uint8_t> data;

            VkResult res = resource_util.ReadFromBufferResource(mutable_resources_clones_before.buffers[i],
                                                                buffer_info->size,
                                                                0,
                                                                buffer_info->queue_family_index,
                                                                data);
            if (res != VK_SUCCESS)
            {
                GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s)", util::ToString<VkResult>(res).c_str())
                return res;
            }

            const uint32_t desc_set           = mutable_resources_clones_before.buffer_desc_set_binding_pair[i].first;
            const uint32_t binding            = mutable_resources_clones_before.buffer_desc_set_binding_pair[i].second;
            const VkShaderStageFlagBits stage = mutable_resources_clones_before.buffer_shader_stage[i];
            std::string                 filename =
                GenerateDispatchTraceRaysBufferFilename(is_dispatch, index, desc_set, binding, stage, true);

            util::bufferwriter::WriteBuffer(filename, data.data(), data.size());
        }
    }

    for (size_t i = 0; i < mutable_resources_clones.original_images.size(); ++i)
    {
        assert(mutable_resources_clones.original_images[i] != nullptr);

        ImageInfo modified_image_info = *mutable_resources_clones.original_images[i];
        modified_image_info.handle    = mutable_resources_clones.images[i];

        const uint32_t              desc_set = mutable_resources_clones.image_desc_set_binding_pair[i].first;
        const uint32_t              binding  = mutable_resources_clones.image_desc_set_binding_pair[i].second;
        const VkShaderStageFlagBits stage    = mutable_resources_clones.image_shader_stage[i];

        std::vector<std::string> filenames = GenerateDispatchTraceRaysImageFilename(modified_image_info.format,
                                                                                    modified_image_info.level_count,
                                                                                    modified_image_info.layer_count,
                                                                                    is_dispatch,
                                                                                    index,
                                                                                    desc_set,
                                                                                    binding,
                                                                                    stage,
                                                                                    false,
                                                                                    dump_all_image_subresources);

        VkResult res = DumpImageToFile(&modified_image_info,
                                       device_info,
                                       device_table,
                                       instance_table,
                                       object_info_table,
                                       filenames,
                                       dump_resources_scale,
                                       image_file_format,
                                       false,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Dumping image failed (%s)", util::ToString<VkResult>(res).c_str())
            return res;
        }
    }

    // Dump buffers
    for (size_t i = 0; i < mutable_resources_clones.original_buffers.size(); ++i)
    {
        const BufferInfo*    buffer_info = mutable_resources_clones.original_buffers[i];
        std::vector<uint8_t> data;

        VkResult res = resource_util.ReadFromBufferResource(
            mutable_resources_clones.buffers[i], buffer_info->size, 0, buffer_info->queue_family_index, data);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s)", util::ToString<VkResult>(res).c_str())
            return res;
        }

        const uint32_t              desc_set = mutable_resources_clones.buffer_desc_set_binding_pair[i].first;
        const uint32_t              binding  = mutable_resources_clones.buffer_desc_set_binding_pair[i].second;
        const VkShaderStageFlagBits stage    = mutable_resources_clones.buffer_shader_stage[i];
        std::string                 filename =
            GenerateDispatchTraceRaysBufferFilename(is_dispatch, index, desc_set, binding, stage, false);

        util::bufferwriter::WriteBuffer(filename, data.data(), data.size());
    }

    return VK_SUCCESS;
}

bool DispatchTraceRaysDumpingContext::IsRecording() const
{
    if (!dump_resources_before)
    {
        return current_dispatch_index < dispatch_indices.size() || current_trace_rays_index < trace_rays_indices.size();
    }
    else
    {
        return ((current_dispatch_index / 2) < dispatch_indices.size()) ||
               ((current_trace_rays_index / 2) < trace_rays_indices.size());
    }
}

void DispatchTraceRaysDumpingContext::SnapshotBoundDescriptors(DispatchParameters& disp_params)
{
    const PipelineInfo* compute_ppl = bound_pipelines[kBindPoint_compute];
    if (compute_ppl == nullptr)
    {
        return;
    }

    assert(compute_ppl->shaders.size() == 1);

    const auto shader_stage_entry = compute_ppl->shaders.find(VK_SHADER_STAGE_COMPUTE_BIT);
    if (shader_stage_entry == compute_ppl->shaders.end())
    {
        return;
    }

    const ShaderModuleInfo& compute_shader = shader_stage_entry->second;
    for (const auto& shader_desc_set : compute_shader.used_descriptors_info)
    {
        const uint32_t desc_set_index             = shader_desc_set.first;
        const auto&    bound_descriptor_set_entry = bound_descriptor_sets_compute.find(desc_set_index);
        if (bound_descriptor_set_entry == bound_descriptor_sets_compute.end())
        {
            continue;
        }

        const DescriptorSetInfo* bound_descriptor_set = &bound_descriptor_set_entry->second;

        for (const auto& shader_desc_binding : shader_desc_set.second)
        {
            // if (!shader_desc_binding.second.accessed)
            // {
            //     continue;
            // }

            const uint32_t desc_binding_index = shader_desc_binding.first;

            const auto& bound_descriptor_entry = bound_descriptor_set->descriptors.find(desc_binding_index);
            if (bound_descriptor_entry == bound_descriptor_set->descriptors.end())
            {
                continue;
            }

            disp_params.referenced_descriptors[desc_set_index][desc_binding_index] = bound_descriptor_entry->second;
        }
    }
}

void DispatchTraceRaysDumpingContext::SnapshotBoundDescriptors(TraceRaysParameters& tr_params)
{
    const PipelineInfo* ray_tracing_ppl = bound_pipelines[kBindPoint_ray_tracing];
    if (ray_tracing_ppl == nullptr)
    {
        return;
    }

    for (const auto& shader_stage_entry : ray_tracing_ppl->shaders)
    {
        const ShaderModuleInfo& rt_stage_shader_info = shader_stage_entry.second;
        for (const auto& shader_desc_set : rt_stage_shader_info.used_descriptors_info)
        {
            const uint32_t desc_set_index             = shader_desc_set.first;
            const auto&    bound_descriptor_set_entry = bound_descriptor_sets_ray_tracing.find(desc_set_index);
            if (bound_descriptor_set_entry == bound_descriptor_sets_ray_tracing.end())
            {
                continue;
            }

            const DescriptorSetInfo* bound_descriptor_set = &bound_descriptor_set_entry->second;

            for (const auto& shader_desc_binding : shader_desc_set.second)
            {
                // if (!shader_desc_binding.second.accessed)
                // {
                //     continue;
                // }

                const uint32_t desc_binding_index = shader_desc_binding.first;

                const auto& bound_descriptor_entry = bound_descriptor_set->descriptors.find(desc_binding_index);
                if (bound_descriptor_entry == bound_descriptor_set->descriptors.end())
                {
                    continue;
                }

                tr_params.referenced_descriptors[shader_stage_entry.first][desc_set_index][desc_binding_index] =
                    bound_descriptor_entry->second;
            }
        }
    }
}

std::vector<std::string>
DispatchTraceRaysDumpingContext::GenerateImageDescriptorFilename(const ImageInfo* img_info) const
{
    assert(img_info != nullptr);

    std::vector<VkImageAspectFlagBits> aspects;
    bool                               combined_depth_stencil;
    graphics::GetFormatAspects(img_info->format, &aspects, &combined_depth_stencil);

    const uint32_t total_files =
        dump_all_image_subresources ? (aspects.size() * img_info->level_count * img_info->layer_count) : aspects.size();
    std::vector<std::string> filenames(total_files);

    uint32_t f = 0;
    for (size_t i = 0; i < aspects.size(); ++i)
    {
        std::string       aspect_str_whole(util::ToString<VkImageAspectFlagBits>(aspects[i]));
        std::string       aspect_str(aspect_str_whole.begin() + 16, aspect_str_whole.end() - 4);
        std::stringstream base_filename;

        if (VkFormatToImageWriterDataFormat(img_info->format) != util::imagewriter::DataFormats::kFormat_UNSPECIFIED)
        {
            base_filename << "Image_" << img_info->capture_id << "_aspect_" << aspect_str;
        }
        else
        {
            std::string whole_format_name = util::ToString<VkFormat>(img_info->format);
            std::string format_name(whole_format_name.begin() + 10, whole_format_name.end());

            base_filename << "Image_" << img_info->capture_id << "_" << format_name << "_aspect_" << aspect_str;
        }

        if (dump_all_image_subresources && (img_info->level_count > 1 || img_info->layer_count > 1))
        {
            for (uint32_t level = 0; level < img_info->level_count; ++level)
            {
                for (uint32_t layer = 0; layer < img_info->layer_count; ++layer)
                {
                    std::stringstream sub_resources_str;
                    sub_resources_str << base_filename.str() << "_mip_" << level << "_layer_" << layer;
                    std::filesystem::path filedirname(dump_resource_path);
                    std::filesystem::path filebasename(sub_resources_str.str());
                    filenames[f++] = (filedirname / filebasename).string();
                }
            }
        }
        else
        {
            std::filesystem::path filedirname(dump_resource_path);
            std::filesystem::path filebasename(base_filename.str());
            filenames[f++] = (filedirname / filebasename).string();
        }
    }

    assert(f == total_files);

    return std::move(filenames);
}

std::string DispatchTraceRaysDumpingContext::GenerateBufferDescriptorFilename(format::HandleId buffer_id) const
{
    std::stringstream filename;

    filename << "Buffer_" << buffer_id << ".bin";

    std::filesystem::path filedirname(dump_resource_path);
    std::filesystem::path filebasename(filename.str());
    return (filedirname / filebasename).string();
}

std::string DispatchTraceRaysDumpingContext::GenerateInlineUniformBufferDescriptorFilename(uint32_t set,
                                                                                           uint32_t binding) const
{
    std::stringstream filename;
    filename << "InlineUniformBlock_set_" << set << "_binding_" << binding << ".bin";

    std::filesystem::path filedirname(dump_resource_path);
    std::filesystem::path filebasename(filename.str());
    return (filedirname / filebasename).string();
}

VkResult DispatchTraceRaysDumpingContext::DumpImmutableResources(uint64_t qs_index, uint64_t bcb_index) const
{
    // Create a list of all descriptors referenced by all commands
    std::unordered_set<const ImageInfo*> image_descriptors;

    struct buffer_descriptor_info
    {
        VkDeviceSize offset;
        VkDeviceSize range;
    };
    std::unordered_map<const BufferInfo*, buffer_descriptor_info> buffer_descriptors;

    struct inline_uniform_block_info
    {
        uint32_t                    set;
        uint32_t                    binding;
        const std::vector<uint8_t>* data;
    };
    std::unordered_map<const std::vector<uint8_t>*, inline_uniform_block_info> inline_uniform_blocks;

    // Scan through descriptors used in compute commands
    for (const auto& disp_params : dispatch_params)
    {
        for (const auto& desc_set : disp_params.second.referenced_descriptors)
        {
            const uint32_t desc_set_index = desc_set.first;

            for (const auto& desc_binding : desc_set.second)
            {
                const uint32_t desc_binding_index = desc_binding.first;

                switch (desc_binding.second.desc_type)
                {
                    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    {
                        for (size_t i = 0; i < desc_binding.second.image_info.size(); ++i)
                        {
                            if (desc_binding.second.image_info[i].image_view_info != nullptr)
                            {
                                const ImageInfo* img_info = object_info_table.GetImageInfo(
                                    desc_binding.second.image_info[i].image_view_info->image_id);
                                assert(img_info);

                                image_descriptors.insert(img_info);
                            }
                        }
                    }
                    break;

                    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                    {
                        for (size_t i = 0; i < desc_binding.second.buffer_info.size(); ++i)
                        {
                            if (desc_binding.second.buffer_info[i].buffer_info != nullptr)
                            {
                                buffer_descriptors.emplace(
                                    std::piecewise_construct,
                                    std::forward_as_tuple(desc_binding.second.buffer_info[i].buffer_info),
                                    std::forward_as_tuple(
                                        buffer_descriptor_info{ desc_binding.second.buffer_info[i].offset,
                                                                desc_binding.second.buffer_info[i].range }));
                            }
                        }
                    }
                    break;

                    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                        break;

                    case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                    {
                        inline_uniform_blocks[&(desc_binding.second.inline_uniform_block)] = {
                            desc_set_index, desc_binding_index, &(desc_binding.second.inline_uniform_block)
                        };
                    }
                    break;

                    default:
                        GFXRECON_LOG_WARNING_ONCE(
                            "%s(): Descriptor type (%s) not handled",
                            __func__,
                            util::ToString<VkDescriptorType>(desc_binding.second.desc_type).c_str());
                        break;
                }
            }
        }
    }

    // Scan through descriptors used in ray tracing commands
    for (const auto& tr_params : trace_rays_params)
    {
        for (const auto& shader_stage : tr_params.second.referenced_descriptors)
        {
            for (const auto& desc_set : shader_stage.second)
            {
                const uint32_t desc_set_index = desc_set.first;

                for (const auto& desc_binding : desc_set.second)
                {
                    const uint32_t desc_binding_index = desc_binding.first;

                    switch (desc_binding.second.desc_type)
                    {
                        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                        {
                            for (size_t i = 0; i < desc_binding.second.image_info.size(); ++i)
                            {
                                if (desc_binding.second.image_info[i].image_view_info != nullptr)
                                {
                                    const ImageInfo* img_info = object_info_table.GetImageInfo(
                                        desc_binding.second.image_info[i].image_view_info->image_id);
                                    assert(img_info);

                                    image_descriptors.insert(img_info);
                                }
                            }
                        }
                        break;

                        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                        {
                            for (size_t i = 0; i < desc_binding.second.buffer_info.size(); ++i)
                            {
                                if (desc_binding.second.buffer_info[i].buffer_info != nullptr)
                                {
                                    buffer_descriptors.emplace(
                                        std::piecewise_construct,
                                        std::forward_as_tuple(desc_binding.second.buffer_info[i].buffer_info),
                                        std::forward_as_tuple(
                                            buffer_descriptor_info{ desc_binding.second.buffer_info[i].offset,
                                                                    desc_binding.second.buffer_info[i].range }));
                                }
                            }
                        }
                        break;

                        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                        case VK_DESCRIPTOR_TYPE_SAMPLER:
                            break;

                        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                        {
                            inline_uniform_blocks[&(desc_binding.second.inline_uniform_block)] = {
                                desc_set_index, desc_binding_index, &(desc_binding.second.inline_uniform_block)
                            };
                        }
                        break;

                        default:
                            GFXRECON_LOG_WARNING_ONCE(
                                "%s(): Descriptor type (%s) not handled",
                                __func__,
                                util::ToString<VkDescriptorType>(desc_binding.second.desc_type).c_str());
                            break;
                    }
                }
            }
        }
    }

    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const DeviceInfo* device_info = object_info_table.GetDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    for (const auto& img_info : image_descriptors)
    {
        const std::vector<std::string> filenames = GenerateImageDescriptorFilename(img_info);

        VkResult res = DumpImageToFile(img_info,
                                       device_info,
                                       device_table,
                                       instance_table,
                                       object_info_table,
                                       filenames,
                                       dump_resources_scale,
                                       image_file_format,
                                       dump_all_image_subresources);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Dumping image failed (%s)", util::ToString<VkResult>(res).c_str())
            return res;
        }
    }

    const PhysicalDeviceInfo* phys_dev_info = object_info_table.GetPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    graphics::VulkanResourcesUtil resource_util(device_info->handle,
                                                device_info->parent,
                                                *device_table,
                                                *instance_table,
                                                *phys_dev_info->replay_device_info->memory_properties);

    for (const auto& buf : buffer_descriptors)
    {
        const BufferInfo*  buffer_info = buf.first;
        const VkDeviceSize offset      = buf.second.offset;
        const VkDeviceSize range       = buf.second.range;
        const VkDeviceSize size        = range == VK_WHOLE_SIZE ? buffer_info->size - offset : range;

        std::vector<uint8_t> data;
        VkResult             res = resource_util.ReadFromBufferResource(
            buffer_info->handle, size, offset, buffer_info->queue_family_index, data);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reading from buffer resource failed (%s)", util::ToString<VkResult>(res).c_str())
            return res;
        }

        const std::string filename = GenerateBufferDescriptorFilename(buffer_info->capture_id);
        util::bufferwriter::WriteBuffer(filename, data.data(), data.size());
    }

    for (const auto& iub : inline_uniform_blocks)
    {
        std::string filename = GenerateInlineUniformBufferDescriptorFilename(iub.second.set, iub.second.binding);
        util::bufferwriter::WriteBuffer(filename, iub.second.data->data(), iub.second.data->size());
    }

    return VK_SUCCESS;
}

VkResult DispatchTraceRaysDumpingContext::CopyDispatchIndirectParameters(uint64_t index)
{
    auto entry = dispatch_params.find(index);
    assert(entry != dispatch_params.end());
    DispatchParameters& disp_params = entry->second;

    assert(disp_params.type == kDispatchIndirect);
    assert(disp_params.dispatch_params_union.dispatch_indirect.params_buffer_info != nullptr);
    assert(disp_params.dispatch_params_union.dispatch_indirect.params_buffer_info->handle != VK_NULL_HANDLE);

    const VkDeviceSize size = sizeof(VkDispatchIndirectCommand);
    VkResult           res  = CloneBuffer(object_info_table,
                               device_table,
                               replay_device_phys_mem_props,
                               disp_params.dispatch_params_union.dispatch_indirect.params_buffer_info,
                               &disp_params.dispatch_params_union.dispatch_indirect.new_params_buffer,
                               &disp_params.dispatch_params_union.dispatch_indirect.new_params_memory,
                               size);
    if (res != VK_SUCCESS)
    {
        GFXRECON_LOG_ERROR("Cloning buffer resources failed (%s)", util::ToString<VkResult>(res).c_str())
        return res;
    }

    // Inject a cmdCopyBuffer to copy the dispatch params into the new buffer
    {
        const VkDeviceSize offset  = disp_params.dispatch_params_union.dispatch_indirect.params_buffer_offset;
        const VkBufferCopy region  = { offset, 0, size };
        VkCommandBuffer    cmd_buf = DR_command_buffer;
        device_table->CmdCopyBuffer(cmd_buf,
                                    disp_params.dispatch_params_union.dispatch_indirect.params_buffer_info->handle,
                                    disp_params.dispatch_params_union.dispatch_indirect.new_params_buffer,
                                    1,
                                    &region);

        VkBufferMemoryBarrier buf_barrier;
        buf_barrier.sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        buf_barrier.pNext               = nullptr;
        buf_barrier.buffer              = disp_params.dispatch_params_union.dispatch_indirect.new_params_buffer;
        buf_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        buf_barrier.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
        buf_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buf_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        buf_barrier.size                = size;
        buf_barrier.offset              = 0;

        device_table->CmdPipelineBarrier(cmd_buf,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VkDependencyFlags{ 0 },
                                         0,
                                         nullptr,
                                         1,
                                         &buf_barrier,
                                         0,
                                         nullptr);
    }

    return VK_SUCCESS;
}

VkResult DispatchTraceRaysDumpingContext::FetchIndirectParams()
{
    assert(original_command_buffer_info);
    assert(original_command_buffer_info->parent_id != format::kNullHandleId);
    const DeviceInfo* device_info = object_info_table.GetDeviceInfo(original_command_buffer_info->parent_id);
    assert(device_info);

    const PhysicalDeviceInfo* phys_dev_info = object_info_table.GetPhysicalDeviceInfo(device_info->parent_id);
    assert(phys_dev_info);

    graphics::VulkanResourcesUtil resource_util(device_info->handle,
                                                device_info->parent,
                                                *device_table,
                                                *instance_table,
                                                *phys_dev_info->replay_device_info->memory_properties);

    for (auto& dis_params : dispatch_params)
    {
        if (dis_params.second.type != kDispatchIndirect)
        {
            continue;
        }

        assert(dis_params.second.dispatch_params_union.dispatch_indirect.new_params_buffer != VK_NULL_HANDLE);
        const VkDeviceSize   size = sizeof(VkDispatchIndirectCommand);
        std::vector<uint8_t> data;
        VkResult             res = resource_util.ReadFromBufferResource(
            dis_params.second.dispatch_params_union.dispatch_indirect.new_params_buffer,
            size,
            0,
            VK_QUEUE_FAMILY_IGNORED,
            data);
        if (res != VK_SUCCESS)
        {
            GFXRECON_LOG_ERROR("Reading from buffer resources failed (%s)", util::ToString<VkResult>(res).c_str())
            return res;
        }

        assert(data.size() == sizeof(VkDispatchIndirectCommand));
        util::platform::MemoryCopy(
            &dis_params.second.dispatch_params_union.dispatch_indirect.dispatch_params, size, data.data(), size);
    }

    return VK_SUCCESS;
}

void DispatchTraceRaysDumpingContext::GenerateOutputJson(uint64_t qs_index, uint64_t bcb_index) const
{
    // Handle Dispatch commands
    auto& dispatch_json_entries =
        (!dispatch_params.empty()) ? dump_json.InsertSubEntry("dispatchCommands") : dump_json.GetData();
    uint32_t command_count = 0;
    for (const auto& disp_params : dispatch_params)
    {
        const uint64_t index          = disp_params.first;
        auto&          dispatch_entry = dispatch_json_entries[command_count++];

        dispatch_entry["dispatchIndex"]           = index;
        dispatch_entry["beginCommandBufferIndex"] = bcb_index;
        dispatch_entry["queueSubmitIndex"]        = qs_index;

        auto& outputs_json_entries = dispatch_entry["outputs"];

        if (dump_resources_before)
        {
            auto&       before_command_output_entries = outputs_json_entries["before"];
            const auto& mutable_resource_entry_before = disp_params.second.mutable_resources_clones_before;

            if (!mutable_resource_entry_before.original_images.empty())
            {
                auto&    before_command_output_image_entries = before_command_output_entries["images"];
                uint32_t output_image_index                  = 0;
                for (size_t i = 0; i < mutable_resource_entry_before.original_images.size(); ++i)
                {
                    const uint32_t desc_set = mutable_resource_entry_before.image_desc_set_binding_pair[i].first;
                    const uint32_t binding  = mutable_resource_entry_before.image_desc_set_binding_pair[i].second;
                    const VkShaderStageFlagBits stage = mutable_resource_entry_before.image_shader_stage[i];

                    const ImageInfo* img_info = mutable_resource_entry_before.original_images[i];
                    assert(img_info != nullptr);

                    std::vector<std::string> filenames =
                        GenerateDispatchTraceRaysImageFilename(img_info->format,
                                                               img_info->level_count,
                                                               img_info->layer_count,
                                                               true,
                                                               index,
                                                               desc_set,
                                                               binding,
                                                               stage,
                                                               true,
                                                               dump_all_image_subresources);

                    std::vector<VkImageAspectFlagBits> aspects;
                    bool                               combined_depth_stencil;
                    graphics::GetFormatAspects(img_info->format, &aspects, &combined_depth_stencil);

                    auto& image_json_entry = before_command_output_image_entries[output_image_index++];
                    image_json_entry["type"] =
                        util::ToString<VkDescriptorType>(mutable_resource_entry_before.image_desc_types[i]);
                    image_json_entry["set"]     = desc_set;
                    image_json_entry["binding"] = binding;
                    auto& image_json_entry_desc = image_json_entry["images"];

                    for (size_t f = 0; f < filenames.size(); ++f)
                    {
                        filenames[f] += ImageFileExtension(img_info->format, image_file_format);
                        dump_json.InsertImageInfo(image_json_entry_desc[f], img_info, filenames[i], aspects[f]);
                    }
                }
            }

            if (!mutable_resource_entry_before.original_buffers.empty())
            {
                auto&    before_command_output_buffer_entries = before_command_output_entries["buffers"];
                uint32_t output_buffer_index                  = 0;

                for (size_t i = 0; i < mutable_resource_entry_before.original_buffers.size(); ++i)
                {
                    const BufferInfo* buffer_info = mutable_resource_entry_before.original_buffers[i];
                    assert(buffer_info != nullptr);

                    const uint32_t desc_set = mutable_resource_entry_before.buffer_desc_set_binding_pair[i].first;
                    const uint32_t binding  = mutable_resource_entry_before.buffer_desc_set_binding_pair[i].second;
                    const VkShaderStageFlagBits stage = mutable_resource_entry_before.buffer_shader_stage[i];
                    std::string                 filename =
                        GenerateDispatchTraceRaysBufferFilename(true, index, desc_set, binding, stage, true);

                    auto& buffer_json_entry = before_command_output_buffer_entries[output_buffer_index++];
                    buffer_json_entry["type"] =
                        util::ToString<VkDescriptorType>(mutable_resource_entry_before.buffer_desc_types[i]);
                    buffer_json_entry["set"]     = desc_set;
                    buffer_json_entry["binding"] = binding;
                    dump_json.InsertBufferInfo(buffer_json_entry, buffer_info, filename);
                }
            }
        }

        const auto& mutable_resource_entry = disp_params.second.mutable_resources_clones;
        auto& outputs_json_entries_after = dump_resources_before ? outputs_json_entries["after"] : outputs_json_entries;

        if (!mutable_resource_entry.original_images.empty())
        {
            uint32_t mutable_images_count       = 0;
            auto&    image_outputs_json_entries = outputs_json_entries_after["images"];

            for (size_t i = 0; i < mutable_resource_entry.original_images.size(); ++i)
            {
                const ImageInfo* img_info = mutable_resource_entry.original_images[i];
                assert(img_info != nullptr);

                const uint32_t              desc_set = mutable_resource_entry.image_desc_set_binding_pair[i].first;
                const uint32_t              binding  = mutable_resource_entry.image_desc_set_binding_pair[i].second;
                const VkShaderStageFlagBits stage    = mutable_resource_entry.image_shader_stage[i];

                std::vector<std::string> filenames =
                    GenerateDispatchTraceRaysImageFilename(img_info->format,
                                                           img_info->level_count,
                                                           img_info->layer_count,
                                                           true,
                                                           index,
                                                           desc_set,
                                                           binding,
                                                           stage,
                                                           false,
                                                           dump_all_image_subresources);

                std::vector<VkImageAspectFlagBits> aspects;
                bool                               combined_depth_stencil;
                graphics::GetFormatAspects(img_info->format, &aspects, &combined_depth_stencil);

                auto& image_json_entry  = image_outputs_json_entries[mutable_images_count++];
                image_json_entry["set"] = util::ToString<VkDescriptorType>(mutable_resource_entry.image_desc_types[i]);
                image_json_entry["set"] = desc_set;
                image_json_entry["binding"] = binding;
                auto& image_json_entry_desc = image_json_entry["images"];

                for (size_t f = 0; f < filenames.size(); ++f)
                {
                    filenames[f] += ImageFileExtension(img_info->format, image_file_format);
                    dump_json.InsertImageInfo(image_json_entry_desc[f], img_info, filenames[f], aspects[f]);
                }
            }
        }

        if (!mutable_resource_entry.original_buffers.empty())
        {
            uint32_t mutable_buffers_count       = 0;
            auto&    buffer_outputs_json_entries = outputs_json_entries_after["buffers"];
            for (size_t i = 0; i < mutable_resource_entry.original_buffers.size(); ++i)
            {
                const BufferInfo* buffer_info = mutable_resource_entry.original_buffers[i];
                assert(buffer_info != nullptr);

                const uint32_t              desc_set = mutable_resource_entry.buffer_desc_set_binding_pair[i].first;
                const uint32_t              binding  = mutable_resource_entry.buffer_desc_set_binding_pair[i].second;
                const VkShaderStageFlagBits stage    = mutable_resource_entry.buffer_shader_stage[i];
                std::string                 filename =
                    GenerateDispatchTraceRaysBufferFilename(true, index, desc_set, binding, stage, false);

                auto& buffer_json_entry      = buffer_outputs_json_entries[mutable_buffers_count++];
                buffer_json_entry["set"]     = mutable_resource_entry.buffer_desc_types[i];
                buffer_json_entry["set"]     = desc_set;
                buffer_json_entry["binding"] = binding;
                dump_json.InsertBufferInfo(buffer_json_entry, buffer_info, filename);
            }
        }

        if (dump_immutable_resources)
        {
            auto&    descriptor_entries       = dispatch_entry["descriptors"];
            uint32_t descriptor_entries_count = 0;
            for (const auto& desc_set : disp_params.second.referenced_descriptors)
            {
                const uint32_t desc_set_index = desc_set.first;

                for (const auto& desc_binding : desc_set.second)
                {
                    const uint32_t desc_binding_index = desc_binding.first;

                    switch (desc_binding.second.desc_type)
                    {
                        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                        {
                            for (size_t i = 0; i < desc_binding.second.image_info.size(); ++i)
                            {
                                if (desc_binding.second.image_info[i].image_view_info != nullptr)
                                {
                                    descriptor_entries[descriptor_entries_count]["type"] =
                                        util::ToString<VkDescriptorType>(desc_binding.second.desc_type);
                                    descriptor_entries[descriptor_entries_count]["set"]        = desc_set_index;
                                    descriptor_entries[descriptor_entries_count]["binding"]    = desc_binding_index;
                                    descriptor_entries[descriptor_entries_count]["arrayIndex"] = i;

                                    const ImageInfo* img_info = object_info_table.GetImageInfo(
                                        desc_binding.second.image_info[i].image_view_info->image_id);
                                    assert(img_info);

                                    std::vector filenames = GenerateImageDescriptorFilename(img_info);

                                    std::vector<VkImageAspectFlagBits> aspects;
                                    bool                               combined_depth_stencil;
                                    graphics::GetFormatAspects(img_info->format, &aspects, &combined_depth_stencil);

                                    for (size_t f = 0; f < filenames.size(); ++f)
                                    {
                                        auto& image_descriptor_json_entry =
                                            descriptor_entries[descriptor_entries_count]["descriptor"];
                                        filenames[f] += ImageFileExtension(img_info->format, image_file_format);
                                        dump_json.InsertImageInfo(
                                            image_descriptor_json_entry, img_info, filenames[f], aspects[f]);

                                        ++descriptor_entries_count;
                                    }
                                }
                            }
                        }
                        break;

                        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                        {
                            for (size_t i = 0; i < desc_binding.second.buffer_info.size(); ++i)
                            {
                                if (desc_binding.second.buffer_info[i].buffer_info != nullptr)
                                {
                                    descriptor_entries[descriptor_entries_count]["type"] =
                                        util::ToString<VkDescriptorType>(desc_binding.second.desc_type);
                                    descriptor_entries[descriptor_entries_count]["set"]        = desc_set_index;
                                    descriptor_entries[descriptor_entries_count]["binding"]    = desc_binding_index;
                                    descriptor_entries[descriptor_entries_count]["arrayIndex"] = i;

                                    auto& buffer_descriptor_json_entry =
                                        descriptor_entries[descriptor_entries_count]["descriptor"];

                                    const std::string filename = GenerateBufferDescriptorFilename(
                                        desc_binding.second.buffer_info[i].buffer_info->capture_id);
                                    dump_json.InsertBufferInfo(buffer_descriptor_json_entry,
                                                               desc_binding.second.buffer_info[i].buffer_info,
                                                               filename);
                                    ++descriptor_entries_count;
                                }
                            }
                        }
                        break;

                        case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                        case VK_DESCRIPTOR_TYPE_SAMPLER:
                            break;

                        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                        {
                            descriptor_entries[descriptor_entries_count]["type"] =
                                util::ToString<VkDescriptorType>(desc_binding.second.desc_type);
                            descriptor_entries[descriptor_entries_count]["set"]     = desc_set_index;
                            descriptor_entries[descriptor_entries_count]["binding"] = desc_binding_index;
                            descriptor_entries[descriptor_entries_count]["size"] =
                                desc_binding.second.inline_uniform_block.size();

                            const std::string filename =
                                GenerateInlineUniformBufferDescriptorFilename(desc_set_index, desc_binding_index);
                            descriptor_entries[descriptor_entries_count]["file"] = filename;
                            ++descriptor_entries_count;
                        }
                        break;

                        default:
                            GFXRECON_LOG_WARNING_ONCE(
                                "%s(): Descriptor type (%s) not handled",
                                __func__,
                                util::ToString<VkDescriptorType>(desc_binding.second.desc_type).c_str());
                            break;
                    }
                }
            }
        }
    }

    // Handle TraceRays commands
    auto& tr_json_entries =
        (!trace_rays_params.empty()) ? dump_json.InsertSubEntry("traceRaysCommands") : dump_json.GetData();
    command_count = 0;
    for (const auto& tr_params : trace_rays_params)
    {
        const uint64_t index    = tr_params.first;
        auto&          tr_entry = tr_json_entries[command_count++];

        tr_entry["traceRaysIndex"]          = index;
        tr_entry["beginCommandBufferIndex"] = bcb_index;
        tr_entry["queueSubmitIndex"]        = qs_index;

        auto& outputs_json_entries = tr_entry["outputs"];

        if (dump_resources_before)
        {
            auto&       before_command_output_entries = outputs_json_entries["before"];
            const auto& mutable_resource_entry_before = tr_params.second.mutable_resources_clones_before;

            if (!mutable_resource_entry_before.original_images.empty())
            {
                auto&    before_command_output_image_entries = before_command_output_entries["images"];
                uint32_t output_image_index                  = 0;
                for (size_t i = 0; i < mutable_resource_entry_before.original_images.size(); ++i)
                {
                    const uint32_t desc_set = mutable_resource_entry_before.image_desc_set_binding_pair[i].first;
                    const uint32_t binding  = mutable_resource_entry_before.image_desc_set_binding_pair[i].second;
                    const VkShaderStageFlagBits stage = mutable_resource_entry_before.image_shader_stage[i];

                    const ImageInfo* img_info = mutable_resource_entry_before.original_images[i];
                    assert(img_info != nullptr);

                    std::vector<std::string> filenames =
                        GenerateDispatchTraceRaysImageFilename(img_info->format,
                                                               img_info->level_count,
                                                               img_info->layer_count,
                                                               false,
                                                               index,
                                                               desc_set,
                                                               binding,
                                                               stage,
                                                               true,
                                                               dump_all_image_subresources);

                    std::vector<VkImageAspectFlagBits> aspects;
                    bool                               combined_depth_stencil;
                    graphics::GetFormatAspects(img_info->format, &aspects, &combined_depth_stencil);

                    auto& image_json_entry = before_command_output_image_entries[output_image_index++];
                    image_json_entry["type"] =
                        util::ToString<VkDescriptorType>(mutable_resource_entry_before.image_desc_types[i]);
                    image_json_entry["set"]     = desc_set;
                    image_json_entry["binding"] = binding;
                    auto& image_json_entry_desc = image_json_entry["images"];

                    for (size_t f = 0; f < filenames.size(); ++f)
                    {
                        filenames[f] += ImageFileExtension(img_info->format, image_file_format);
                        dump_json.InsertImageInfo(image_json_entry_desc[f], img_info, filenames[f], aspects[f]);
                    }
                }
            }

            if (!mutable_resource_entry_before.original_buffers.empty())
            {
                auto&    before_command_output_buffer_entries = before_command_output_entries["buffers"];
                uint32_t output_buffer_index                  = 0;
                for (size_t i = 0; i < mutable_resource_entry_before.original_buffers.size(); ++i)
                {
                    const BufferInfo* buffer_info = mutable_resource_entry_before.original_buffers[i];
                    assert(buffer_info != nullptr);

                    const uint32_t desc_set = mutable_resource_entry_before.buffer_desc_set_binding_pair[i].first;
                    const uint32_t binding  = mutable_resource_entry_before.buffer_desc_set_binding_pair[i].second;
                    const VkShaderStageFlagBits stage = mutable_resource_entry_before.buffer_shader_stage[i];
                    std::string                 filename =
                        GenerateDispatchTraceRaysBufferFilename(false, index, desc_set, binding, stage, true);

                    auto& buffer_json_entry = before_command_output_buffer_entries[output_buffer_index++];
                    buffer_json_entry["type"] =
                        util::ToString<VkDescriptorType>(mutable_resource_entry_before.buffer_desc_types[i]);
                    buffer_json_entry["set"]     = desc_set;
                    buffer_json_entry["binding"] = binding;
                    dump_json.InsertBufferInfo(buffer_json_entry, buffer_info, filename);
                }
            }
        }

        const auto& mutable_resource_entry = tr_params.second.mutable_resources_clones;
        auto& outputs_json_entries_after = dump_resources_before ? outputs_json_entries["after"] : outputs_json_entries;

        if (!mutable_resource_entry.original_images.empty())
        {
            uint32_t mutable_images_count       = 0;
            auto&    image_outputs_json_entries = outputs_json_entries_after["images"];

            for (size_t i = 0; i < mutable_resource_entry.original_images.size(); ++i)
            {
                const ImageInfo* img_info = mutable_resource_entry.original_images[i];
                assert(img_info != nullptr);

                const uint32_t              desc_set = mutable_resource_entry.image_desc_set_binding_pair[i].first;
                const uint32_t              binding  = mutable_resource_entry.image_desc_set_binding_pair[i].second;
                const VkShaderStageFlagBits stage    = mutable_resource_entry.image_shader_stage[i];

                std::vector<std::string> filenames =
                    GenerateDispatchTraceRaysImageFilename(img_info->format,
                                                           img_info->level_count,
                                                           img_info->layer_count,
                                                           false,
                                                           index,
                                                           desc_set,
                                                           binding,
                                                           stage,
                                                           false,
                                                           dump_all_image_subresources);

                std::vector<VkImageAspectFlagBits> aspects;
                bool                               combined_depth_stencil;
                graphics::GetFormatAspects(img_info->format, &aspects, &combined_depth_stencil);

                auto& image_json_entry   = image_outputs_json_entries[mutable_images_count++];
                image_json_entry["type"] = util::ToString<VkDescriptorType>(mutable_resource_entry.image_desc_types[i]);
                image_json_entry["set"]  = desc_set;
                image_json_entry["binding"] = binding;
                auto& image_json_entry_desc = image_json_entry["image"];

                for (size_t f = 0; f < filenames.size(); ++f)
                {
                    filenames[f] += ImageFileExtension(img_info->format, image_file_format);
                    dump_json.InsertImageInfo(image_json_entry_desc[f], img_info, filenames[f], aspects[f]);
                }
            }
        }

        if (!mutable_resource_entry.original_buffers.empty())
        {
            uint32_t mutable_buffers_count       = 0;
            auto&    buffer_outputs_json_entries = outputs_json_entries_after["buffers"];
            for (size_t i = 0; i < mutable_resource_entry.original_buffers.size(); ++i)
            {
                const BufferInfo* buffer_info = mutable_resource_entry.original_buffers[i];
                assert(buffer_info != nullptr);

                const uint32_t              desc_set = mutable_resource_entry.buffer_desc_set_binding_pair[i].first;
                const uint32_t              binding  = mutable_resource_entry.buffer_desc_set_binding_pair[i].second;
                const VkShaderStageFlagBits stage    = mutable_resource_entry.buffer_shader_stage[i];
                std::string                 filename =
                    GenerateDispatchTraceRaysBufferFilename(false, index, desc_set, binding, stage, false);

                auto& buffer_json_entry = buffer_outputs_json_entries[mutable_buffers_count++];
                buffer_json_entry["set"] =
                    util::ToString<VkDescriptorType>(mutable_resource_entry.buffer_desc_types[i]);
                buffer_json_entry["set"]     = desc_set;
                buffer_json_entry["binding"] = binding;
                dump_json.InsertBufferInfo(buffer_json_entry, buffer_info, filename);
            }
        }

        if (dump_immutable_resources)
        {
            auto& descriptor_entries = tr_entry["descriptors"];
            for (const auto& shader_stage : tr_params.second.referenced_descriptors)
            {
                uint32_t          stage_entry_index       = 0;
                const std::string shader_stage_name_whole = util::ToString<VkShaderStageFlagBits>(shader_stage.first);
                std::string       shader_stage_name;
                if (!shader_stage_name_whole.compare(shader_stage_name_whole.size() - 4, 4, "_BIT"))
                {
                    shader_stage_name = shader_stage_name_whole.substr(10, shader_stage_name_whole.size() - 14);
                }
                else if (!shader_stage_name_whole.compare(shader_stage_name_whole.size() - 8, 8, "_BIT_KHR"))
                {
                    shader_stage_name = shader_stage_name_whole.substr(10, shader_stage_name_whole.size() - 18);
                }

                auto& desc_shader_stage_json_entry = descriptor_entries[shader_stage_name];

                for (const auto& desc_set : shader_stage.second)
                {
                    const uint32_t desc_set_index = desc_set.first;

                    for (const auto& desc_binding : desc_set.second)
                    {
                        const uint32_t desc_binding_index = desc_binding.first;

                        switch (desc_binding.second.desc_type)
                        {
                            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                            {
                                for (size_t img = 0; img < desc_binding.second.image_info.size(); ++img)
                                {
                                    if (desc_binding.second.image_info[img].image_view_info != nullptr)
                                    {
                                        auto& desc_json_entry = desc_shader_stage_json_entry[stage_entry_index];

                                        const ImageInfo* img_info = object_info_table.GetImageInfo(
                                            desc_binding.second.image_info[img].image_view_info->image_id);
                                        assert(img_info);

                                        desc_json_entry["type"] =
                                            util::ToString<VkDescriptorType>(desc_binding.second.desc_type);
                                        desc_json_entry["set"]        = desc_set_index;
                                        desc_json_entry["binding"]    = desc_binding_index;
                                        desc_json_entry["arrayIndex"] = img;

                                        std::vector filenames = GenerateImageDescriptorFilename(img_info);
                                        std::vector<VkImageAspectFlagBits> aspects;
                                        bool                               combined_depth_stencil;
                                        graphics::GetFormatAspects(img_info->format, &aspects, &combined_depth_stencil);

                                        for (size_t f = 0; f < filenames.size(); ++f)
                                        {
                                            filenames[f] += ImageFileExtension(img_info->format, image_file_format);
                                            auto& image_descriptor_json_entry = desc_json_entry["descriptor"];
                                            dump_json.InsertImageInfo(
                                                image_descriptor_json_entry[f], img_info, filenames[f], aspects[f]);

                                            ++stage_entry_index;
                                        }
                                    }
                                }
                            }
                            break;

                            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                            {
                                for (size_t i = 0; i < desc_binding.second.buffer_info.size(); ++i)
                                {
                                    if (desc_binding.second.buffer_info[i].buffer_info != nullptr)
                                    {
                                        auto& desc_json_entry = desc_shader_stage_json_entry[stage_entry_index];

                                        desc_json_entry["type"] =
                                            util::ToString<VkDescriptorType>(desc_binding.second.desc_type);
                                        desc_json_entry["set"]        = desc_set_index;
                                        desc_json_entry["binding"]    = desc_binding_index;
                                        desc_json_entry["arrayIndex"] = i;

                                        const std::string filename = GenerateBufferDescriptorFilename(
                                            desc_binding.second.buffer_info[i].buffer_info->capture_id);
                                        auto& buffer_descriptor_json_entry = desc_json_entry["descriptor"];
                                        dump_json.InsertBufferInfo(buffer_descriptor_json_entry,
                                                                   desc_binding.second.buffer_info[i].buffer_info,
                                                                   filename);

                                        ++stage_entry_index;
                                    }
                                }
                            }
                            break;

                            case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
                            {
                                auto& desc_json_entry = desc_shader_stage_json_entry[stage_entry_index];

                                const std::string filename =
                                    GenerateInlineUniformBufferDescriptorFilename(desc_set_index, desc_binding_index);

                                desc_json_entry["type"] =
                                    util::ToString<VkDescriptorType>(desc_binding.second.desc_type);
                                desc_json_entry["set"]     = desc_set_index;
                                desc_json_entry["binding"] = desc_binding_index;
                                desc_json_entry["size"]    = desc_binding.second.inline_uniform_block.size();
                                desc_json_entry["file"]    = filename;

                                ++stage_entry_index;
                            }
                            break;

                            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                            case VK_DESCRIPTOR_TYPE_SAMPLER:
                                break;

                            default:
                                GFXRECON_LOG_WARNING_ONCE(
                                    "%s(): Descriptor type (%s) not handled",
                                    __func__,
                                    util::ToString<VkDescriptorType>(desc_binding.second.desc_type).c_str());
                                break;
                        }
                    }
                }
            }
        }
    }
}

void DispatchTraceRaysDumpingContext::InsertNewDispatchParameters(uint64_t index,
                                                                  uint32_t groupCountX,
                                                                  uint32_t groupCountY,
                                                                  uint32_t groupCountZ)
{
    auto new_entry = dispatch_params.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(index),
        std::forward_as_tuple(
            DispatchTraceRaysDumpingContext::DispatchTypes::kDispatch, groupCountX, groupCountY, groupCountZ));
    assert(new_entry.second);
}

void DispatchTraceRaysDumpingContext::InsertNewDispatchParameters(uint64_t          index,
                                                                  const BufferInfo* buffer_info,
                                                                  VkDeviceSize      offset)
{
    auto new_entry = dispatch_params.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(index),
        std::forward_as_tuple(DispatchTraceRaysDumpingContext::DispatchTypes::kDispatchIndirect, buffer_info, offset));
    assert(new_entry.second);
}

void DispatchTraceRaysDumpingContext::InsertNewTraceRaysParameters(
    uint64_t                               index,
    const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
    const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable,
    uint32_t                               width,
    uint32_t                               height,
    uint32_t                               depth)
{
    auto new_entry =
        trace_rays_params.emplace(std::piecewise_construct,
                                  std::forward_as_tuple(index),
                                  std::forward_as_tuple(DispatchTraceRaysDumpingContext::TraceRaysTypes::kTraceRays,
                                                        pRaygenShaderBindingTable,
                                                        pMissShaderBindingTable,
                                                        pHitShaderBindingTable,
                                                        pCallableShaderBindingTable,
                                                        width,
                                                        height,
                                                        depth));
    assert(new_entry.second);
}

GFXRECON_END_NAMESPACE(gfxrecon)
GFXRECON_END_NAMESPACE(decode)