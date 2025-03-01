/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_command_builder.hh"
#include "vk_render_graph.hh"

namespace blender::gpu::render_graph {

VKCommandBuilder::VKCommandBuilder()
{
  vk_buffer_memory_barrier_ = {};
  vk_buffer_memory_barrier_.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  vk_buffer_memory_barrier_.pNext = nullptr;
  vk_buffer_memory_barrier_.srcAccessMask = VK_ACCESS_NONE;
  vk_buffer_memory_barrier_.dstAccessMask = VK_ACCESS_NONE;
  vk_buffer_memory_barrier_.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  vk_buffer_memory_barrier_.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  vk_buffer_memory_barrier_.buffer = VK_NULL_HANDLE;
  vk_buffer_memory_barrier_.offset = 0;
  vk_buffer_memory_barrier_.size = VK_WHOLE_SIZE;

  vk_image_memory_barrier_ = {};
  vk_image_memory_barrier_.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  vk_image_memory_barrier_.pNext = nullptr;
  vk_image_memory_barrier_.srcAccessMask = VK_ACCESS_NONE;
  vk_image_memory_barrier_.dstAccessMask = VK_ACCESS_NONE;
  vk_image_memory_barrier_.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  vk_image_memory_barrier_.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  vk_image_memory_barrier_.image = VK_NULL_HANDLE;
  vk_image_memory_barrier_.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vk_image_memory_barrier_.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vk_image_memory_barrier_.subresourceRange.aspectMask = VK_IMAGE_ASPECT_NONE;
  vk_image_memory_barrier_.subresourceRange.baseArrayLayer = 0;
  vk_image_memory_barrier_.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
  vk_image_memory_barrier_.subresourceRange.baseMipLevel = 0;
  vk_image_memory_barrier_.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
}

/* -------------------------------------------------------------------- */
/** \name Build nodes
 * \{ */

void VKCommandBuilder::build_nodes(VKRenderGraph &render_graph,
                                   VKCommandBufferInterface &command_buffer,
                                   Span<NodeHandle> nodes)
{
  /* Swap chain images layouts needs to be reset as the image layouts are changed externally.  */
  render_graph.resources_.reset_image_layouts();

  state_.active_pipelines = {};

  command_buffer.begin_recording();
  state_.debug_level = 0;
  state_.active_debug_group_id = -1;
  std::optional<NodeHandle> rendering_scope;
  IndexRange nodes_range = nodes.index_range();
  while (!nodes_range.is_empty()) {
    IndexRange node_group = nodes_range.slice(0, 1);
    NodeHandle node_handle = nodes[nodes_range.first()];
    VKRenderGraphNode &node = render_graph.nodes_[node_handle];
    while (node_type_is_rendering(node.type) && node_group.size() < nodes_range.size()) {
      NodeHandle node_handle = nodes[nodes_range[node_group.size()]];
      VKRenderGraphNode &node = render_graph.nodes_[node_handle];
      if (!node_type_is_rendering(node.type) || node.type == VKNodeType::BEGIN_RENDERING) {
        break;
      }
      node_group = nodes_range.slice(0, node_group.size() + 1);
    }

    build_node_group(render_graph, command_buffer, nodes.slice(node_group), rendering_scope);
    nodes_range = nodes_range.drop_front(node_group.size());
  }

  finish_debug_groups(command_buffer);
  state_.debug_level = 0;

  command_buffer.end_recording();
}

void VKCommandBuilder::build_node_group(VKRenderGraph &render_graph,
                                        VKCommandBufferInterface &command_buffer,
                                        Span<NodeHandle> node_group,
                                        std::optional<NodeHandle> &r_rendering_scope)
{
  bool is_rendering = false;
  for (NodeHandle node_handle : node_group) {
    VKRenderGraphNode &node = render_graph.nodes_[node_handle];
    build_pipeline_barriers(render_graph, command_buffer, node_handle, node.pipeline_stage_get());
  }

  for (NodeHandle node_handle : node_group) {
    VKRenderGraphNode &node = render_graph.nodes_[node_handle];
    if (node.type == VKNodeType::BEGIN_RENDERING) {
      BLI_assert(!r_rendering_scope.has_value());
      BLI_assert(!is_rendering);
      r_rendering_scope = node_handle;
      is_rendering = true;

      /* Check of the node_group spans a full rendering scope. In that case we don't need to set
       * the VK_RENDERING_SUSPENDING_BIT. */
      const VKRenderGraphNode &last_node = render_graph.nodes_[node_group[node_group.size() - 1]];
      bool will_be_suspended = last_node.type != VKNodeType::END_RENDERING;
      if (will_be_suspended) {
        node.begin_rendering.vk_rendering_info.flags = VK_RENDERING_SUSPENDING_BIT;
      }
    }

    else if (node.type == VKNodeType::END_RENDERING) {
      BLI_assert(r_rendering_scope.has_value());
      r_rendering_scope.reset();
      is_rendering = false;
    }
    else if (node_type_is_within_rendering(node.type)) {
      BLI_assert(r_rendering_scope.has_value());
      if (!is_rendering) {
        // Resuming paused rendering scope.
        VKRenderGraphNode &rendering_node = render_graph.nodes_[*r_rendering_scope];
        rendering_node.begin_rendering.vk_rendering_info.flags = VK_RENDERING_RESUMING_BIT;
        rendering_node.build_commands(command_buffer, state_.active_pipelines);
        is_rendering = true;
      }
    }
#if 0
    std::cout << "node_group: " << node_group.first() << "-" << node_group.last()
              << ", node_handle: " << node_handle << ", node_type: " << node.type << "\n";
#endif
    if (G.debug & G_DEBUG_GPU) {
      activate_debug_group(render_graph, command_buffer, node_handle);
    }
    node.build_commands(command_buffer, state_.active_pipelines);
  }
  if (is_rendering) {
    /* Suspend rendering as the next node group will contain data transfer/dispatch commands. */
    is_rendering = false;
    command_buffer.end_rendering();
  }
}

void VKCommandBuilder::activate_debug_group(VKRenderGraph &render_graph,
                                            VKCommandBufferInterface &command_buffer,
                                            NodeHandle node_handle)
{
  VKRenderGraph::DebugGroupID debug_group = render_graph.debug_.node_group_map[node_handle];
  if (debug_group == state_.active_debug_group_id) {
    return;
  }

  /* Determine the number of pops and pushes that will happen on the debug stack. */
  int num_ends = 0;
  int num_begins = 0;

  if (debug_group == -1) {
    num_ends = state_.debug_level;
  }
  else {
    Vector<VKRenderGraph::DebugGroupNameID> &to_group =
        render_graph.debug_.used_groups[debug_group];
    if (state_.active_debug_group_id != -1) {
      Vector<VKRenderGraph::DebugGroupNameID> &from_group =
          render_graph.debug_.used_groups[state_.active_debug_group_id];

      num_ends = max_ii(from_group.size() - to_group.size(), 0);
      int num_checks = min_ii(from_group.size(), to_group.size());
      for (int index : IndexRange(num_checks)) {
        if (from_group[index] != to_group[index]) {
          num_ends += num_checks - index;
          break;
        }
      }
    }

    num_begins = to_group.size() - (state_.debug_level - num_ends);
  }

  /* Perform the pops from the debug stack. */
  for (int index = 0; index < num_ends; index++) {
    command_buffer.end_debug_utils_label();
  }
  state_.debug_level -= num_ends;

  /* Perform the pushes to the debug stack. */
  if (num_begins > 0) {
    Vector<VKRenderGraph::DebugGroupNameID> &to_group =
        render_graph.debug_.used_groups[debug_group];
    VkDebugUtilsLabelEXT debug_utils_label = {};
    debug_utils_label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    for (int index : IndexRange(state_.debug_level, num_begins)) {
      std::string group_name = render_graph.debug_.group_names[to_group[index]];
      debug_utils_label.pLabelName = group_name.c_str();
      command_buffer.begin_debug_utils_label(&debug_utils_label);
    }
  }

  state_.debug_level += num_begins;
  state_.active_debug_group_id = debug_group;
}

void VKCommandBuilder::finish_debug_groups(VKCommandBufferInterface &command_buffer)
{
  for (int i = 0; i < state_.debug_level; i++) {
    command_buffer.end_debug_utils_label();
  }
  state_.debug_level = 0;
}

void VKCommandBuilder::build_pipeline_barriers(VKRenderGraph &render_graph,
                                               VKCommandBufferInterface &command_buffer,
                                               NodeHandle node_handle,
                                               VkPipelineStageFlags pipeline_stage)
{
  reset_barriers();
  add_image_barriers(render_graph, node_handle, pipeline_stage);
  add_buffer_barriers(render_graph, node_handle, pipeline_stage);
  send_pipeline_barriers(command_buffer);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Pipeline barriers
 * \{ */

void VKCommandBuilder::reset_barriers()
{
  vk_buffer_memory_barriers_.clear();
  vk_image_memory_barriers_.clear();
  state_.src_stage_mask = VK_PIPELINE_STAGE_NONE;
  state_.dst_stage_mask = VK_PIPELINE_STAGE_NONE;
}

void VKCommandBuilder::send_pipeline_barriers(VKCommandBufferInterface &command_buffer)
{
  if (vk_image_memory_barriers_.is_empty() && vk_buffer_memory_barriers_.is_empty()) {
    reset_barriers();
    return;
  }

  /* When no resources have been used, we can start the barrier at the top of the pipeline.
   * It is not allowed to set it to None. */
  /* TODO: VK_KHR_synchronization2 allows setting src_stage_mask to NONE. */
  if (state_.src_stage_mask == VK_PIPELINE_STAGE_NONE) {
    state_.src_stage_mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  }

  command_buffer.pipeline_barrier(state_.src_stage_mask,
                                  state_.dst_stage_mask,
                                  VK_DEPENDENCY_BY_REGION_BIT,
                                  0,
                                  nullptr,
                                  vk_buffer_memory_barriers_.size(),
                                  vk_buffer_memory_barriers_.data(),
                                  vk_image_memory_barriers_.size(),
                                  vk_image_memory_barriers_.data());
  reset_barriers();
}

void VKCommandBuilder::add_buffer_barriers(VKRenderGraph &render_graph,
                                           NodeHandle node_handle,
                                           VkPipelineStageFlags node_stages)
{
  add_buffer_read_barriers(render_graph, node_handle, node_stages);
  add_buffer_write_barriers(render_graph, node_handle, node_stages);
}

void VKCommandBuilder::add_buffer_read_barriers(VKRenderGraph &render_graph,
                                                NodeHandle node_handle,
                                                VkPipelineStageFlags node_stages)
{
  for (const VKRenderGraphLink &link : render_graph.links_[node_handle].inputs) {
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.resources_.lookup(
        versioned_resource.handle);
    if (resource.type == VKResourceType::IMAGE) {
      /* Ignore image resources. */
      continue;
    }
    VKResourceBarrierState &resource_state = resource.barrier_state;
    if ((resource_state.vk_access & link.vk_access_flags) == link.vk_access_flags &&
        (resource_state.vk_pipeline_stages & node_stages) == node_stages)
    {
      /* Has already been covered in a previous call no need to add this one. */
      continue;
    }

    const VkAccessFlags wait_access = resource_state.vk_access;

    state_.src_stage_mask |= resource_state.vk_pipeline_stages;
    state_.dst_stage_mask |= node_stages;

    resource_state.vk_access |= link.vk_access_flags;
    resource_state.vk_pipeline_stages |= node_stages;

    add_buffer_barrier(resource.buffer.vk_buffer, wait_access, link.vk_access_flags);
  }
}

void VKCommandBuilder::add_buffer_write_barriers(VKRenderGraph &render_graph,
                                                 NodeHandle node_handle,
                                                 VkPipelineStageFlags node_stages)
{
  for (const VKRenderGraphLink link : render_graph.links_[node_handle].outputs) {
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.resources_.lookup(
        versioned_resource.handle);
    if (resource.type == VKResourceType::IMAGE) {
      /* Ignore image resources. */
      continue;
    }
    VKResourceBarrierState &resource_state = resource.barrier_state;
    const VkAccessFlags wait_access = resource_state.vk_access;

    state_.src_stage_mask |= resource_state.vk_pipeline_stages;
    state_.dst_stage_mask |= node_stages;

    resource_state.vk_access = link.vk_access_flags;
    resource_state.vk_pipeline_stages = node_stages;

    if (wait_access != VK_ACCESS_NONE) {
      add_buffer_barrier(resource.buffer.vk_buffer, wait_access, link.vk_access_flags);
    }
  }
}

void VKCommandBuilder::add_buffer_barrier(VkBuffer vk_buffer,
                                          VkAccessFlags src_access_mask,
                                          VkAccessFlags dst_access_mask)
{
  for (VkBufferMemoryBarrier &vk_buffer_memory_barrier : vk_buffer_memory_barriers_) {
    if (vk_buffer_memory_barrier.buffer == vk_buffer) {
      /* When registering read/write buffers, it can be that the node internally requires
       * read/write. In this case we adjust the dstAccessMask of the read barrier. */
      if ((vk_buffer_memory_barrier.dstAccessMask & src_access_mask) == src_access_mask) {
        vk_buffer_memory_barrier.dstAccessMask |= dst_access_mask;
        return;
      }
      /* When re-registering resources we can skip if access mask already contain all the flags. */
      if ((vk_buffer_memory_barrier.dstAccessMask & dst_access_mask) == dst_access_mask &&
          (vk_buffer_memory_barrier.srcAccessMask & src_access_mask) == src_access_mask)
      {
        return;
      }
    }
  }

  vk_buffer_memory_barrier_.srcAccessMask = src_access_mask;
  vk_buffer_memory_barrier_.dstAccessMask = dst_access_mask;
  vk_buffer_memory_barrier_.buffer = vk_buffer;
  vk_buffer_memory_barriers_.append(vk_buffer_memory_barrier_);
  vk_buffer_memory_barrier_.srcAccessMask = VK_ACCESS_NONE;
  vk_buffer_memory_barrier_.dstAccessMask = VK_ACCESS_NONE;
  vk_buffer_memory_barrier_.buffer = VK_NULL_HANDLE;
}

void VKCommandBuilder::add_image_barriers(VKRenderGraph &render_graph,
                                          NodeHandle node_handle,
                                          VkPipelineStageFlags node_stages)
{
  add_image_read_barriers(render_graph, node_handle, node_stages);
  add_image_write_barriers(render_graph, node_handle, node_stages);
}

void VKCommandBuilder::add_image_read_barriers(VKRenderGraph &render_graph,
                                               NodeHandle node_handle,
                                               VkPipelineStageFlags node_stages)
{
  for (const VKRenderGraphLink &link : render_graph.links_[node_handle].inputs) {
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.resources_.lookup(
        versioned_resource.handle);
    if (resource.type == VKResourceType::BUFFER) {
      /* Ignore buffer resources. */
      continue;
    }
    VKResourceBarrierState &resource_state = resource.barrier_state;
    const VkAccessFlags wait_access = resource_state.vk_access;

    if ((resource_state.vk_access & link.vk_access_flags) == link.vk_access_flags &&
        (resource_state.vk_pipeline_stages & node_stages) == node_stages &&
        resource_state.image_layout == link.vk_image_layout)
    {
      /* Has already been covered in previous barrier no need to add this one. */
      continue;
    }

    state_.src_stage_mask |= resource_state.vk_pipeline_stages;
    state_.dst_stage_mask |= node_stages;

    resource_state.vk_access |= link.vk_access_flags;
    resource_state.vk_pipeline_stages |= node_stages;

    add_image_barrier(resource.image.vk_image,
                      wait_access,
                      link.vk_access_flags,
                      resource_state.image_layout,
                      link.vk_image_layout,
                      link.vk_image_aspect);
    resource_state.image_layout = link.vk_image_layout;
  }
}

void VKCommandBuilder::add_image_write_barriers(VKRenderGraph &render_graph,
                                                NodeHandle node_handle,
                                                VkPipelineStageFlags node_stages)
{
  for (const VKRenderGraphLink link : render_graph.links_[node_handle].outputs) {
    const ResourceWithStamp &versioned_resource = link.resource;
    VKResourceStateTracker::Resource &resource = render_graph.resources_.resources_.lookup(
        versioned_resource.handle);
    if (resource.type == VKResourceType::BUFFER) {
      /* Ignore buffer resources. */
      continue;
    }
    VKResourceBarrierState &resource_state = resource.barrier_state;
    const VkAccessFlags wait_access = resource_state.vk_access;

    state_.src_stage_mask |= resource_state.vk_pipeline_stages;
    state_.dst_stage_mask |= node_stages;

    resource_state.vk_access = link.vk_access_flags;
    resource_state.vk_pipeline_stages = node_stages;

    if (wait_access != VK_ACCESS_NONE || link.vk_image_layout != resource_state.image_layout) {
      add_image_barrier(resource.image.vk_image,
                        wait_access,
                        link.vk_access_flags,
                        resource_state.image_layout,
                        link.vk_image_layout,
                        link.vk_image_aspect);
      resource_state.image_layout = link.vk_image_layout;
    }
  }
}

void VKCommandBuilder::add_image_barrier(VkImage vk_image,
                                         VkAccessFlags src_access_mask,
                                         VkAccessFlags dst_access_mask,
                                         VkImageLayout old_layout,
                                         VkImageLayout new_layout,
                                         VkImageAspectFlags aspect_mask)
{
  BLI_assert(aspect_mask != VK_IMAGE_ASPECT_NONE);
  for (VkImageMemoryBarrier &vk_image_memory_barrier : vk_image_memory_barriers_) {
    if (vk_image_memory_barrier.image == vk_image) {
      /* When registering read/write buffers, it can be that the node internally requires
       * read/write. In this case we adjust the dstAccessMask of the read barrier. An example is
       * EEVEE update HIZ compute shader and shadow tagging. */
      if ((vk_image_memory_barrier.dstAccessMask & src_access_mask) == src_access_mask) {
        vk_image_memory_barrier.dstAccessMask |= dst_access_mask;
        return;
      }
      /* When re-registering resources we can skip if access mask already contain all the flags. */
      if ((vk_image_memory_barrier.dstAccessMask & dst_access_mask) == dst_access_mask &&
          (vk_image_memory_barrier.srcAccessMask & src_access_mask) == src_access_mask &&
          old_layout == new_layout)
      {
        return;
      }
    }
  }

  vk_image_memory_barrier_.srcAccessMask = src_access_mask;
  vk_image_memory_barrier_.dstAccessMask = dst_access_mask;
  vk_image_memory_barrier_.image = vk_image;
  vk_image_memory_barrier_.oldLayout = old_layout;
  vk_image_memory_barrier_.newLayout = new_layout;
  vk_image_memory_barrier_.subresourceRange.aspectMask = aspect_mask;
  vk_image_memory_barriers_.append(vk_image_memory_barrier_);
  /* Reset state for reuse. */
  vk_image_memory_barrier_.srcAccessMask = VK_ACCESS_NONE;
  vk_image_memory_barrier_.dstAccessMask = VK_ACCESS_NONE;
  vk_image_memory_barrier_.image = VK_NULL_HANDLE;
  vk_image_memory_barrier_.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vk_image_memory_barrier_.newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vk_image_memory_barrier_.subresourceRange.aspectMask = VK_IMAGE_ASPECT_NONE;
}

/** \} */

}  // namespace blender::gpu::render_graph
