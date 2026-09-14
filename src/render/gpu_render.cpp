// One frame of the GPU pipeline: submit the Vulkan draw, hand the RGBA render
// target to CUDA through the external semaphores, convert to NV12 on the slot's
// own stream and signal the slot back to Vulkan.
#include "render/gpu.hpp"
#include <glm/gtc/matrix_transform.hpp>
extern "C" cudaError_t convert_nv12(cudaSurfaceObject_t, unsigned char *, int, int, int,
                                    cudaStream_t);
void Gpu::render(int i, const Camera &cam, FrameTiming t) {
  auto &s = slots.at(i);
  CU(cudaSetDevice(cudaDevice));
  // Local copies of the handles that Vulkan needs as writable pointers; the
  // owners keep ownership throughout.
  VkFence fence = s.fence;
  VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
  VK(vkResetFences(device, 1, &fence));
  VkCommandBuffer command = s.command;
  VK(vkResetCommandBuffer(command, 0));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK(vkBeginCommandBuffer(command, &begin));
  vkCmdResetQueryPool(command, queries, i * 2, 2);
  vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, i * 2);
  VkImage image = s.image;
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = s.used ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  // Vulkan hands ownership back to the external (CUDA) queue family.
  b.srcQueueFamilyIndex = s.used ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = s.used ? family : VK_QUEUE_FAMILY_IGNORED;
  b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &b);
  VkClearValue clear[2]{};
  clear[0].color = {{.028f, .043f, .068f, 1}};
  clear[1].depthStencil = {1, 0};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = renderpass;
  rp.framebuffer = s.framebuffer;
  rp.renderArea.extent = {uint32_t(width), uint32_t(height)};
  rp.clearValueCount = 2;
  rp.pClearValues = clear;
  vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  glm::vec3 target(cam.panX, cam.panY, 0),
      eye = target + cam.distance * glm::vec3(cos(cam.pitch) * sin(cam.yaw), sin(cam.pitch),
                                              cos(cam.pitch) * cos(cam.yaw));
  auto projection = glm::perspectiveRH_ZO(glm::radians(45.f), float(width) / height, .01f, 100.f);
  projection[1][1] *= -1;
  auto mvp = projection * glm::lookAtRH(eye, target, glm::vec3(0, 1, 0));
  vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &mvp);
  VkDeviceSize offset = 0;
  VkBuffer vbo = vertices;
  vkCmdBindVertexBuffers(command, 0, 1, &vbo, &offset);
  vkCmdDraw(command, uint32_t(vertexCount), 1, 0, 0);
  // Small binary frame marker enables exact presented-frame/control association without clock
  // guessing.
  for (int bit = 0; bit < 24; bit++) {
    VkClearAttachment a{};
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    float v = (t.frame & (1ull << bit)) ? 1.f : 0.f;
    a.clearValue.color = {{v, v, v, 1}};
    VkClearRect r{{{bit * 8, 0}, {8, 8}}, 0, 1};
    vkCmdClearAttachments(command, 1, &a, 1, &r);
  }
  vkCmdEndRenderPass(command);
  // Release the RGBA image to CUDA; the ready semaphore below carries the
  // completion signal that CUDA waits on.
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = family;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstAccessMask = 0;
  vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, i * 2 + 1);
  VK(vkEndCommandBuffer(command));
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphore ready = s.ready, released = s.released;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  if (s.used) {
    // Only reuse the slot once CUDA has signalled that it is finished with it.
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &released;
    submit.pWaitDstStageMask = &stage;
  }
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &ready;
  t.submit = now_ms();
  VK(vkQueueSubmit(queue, 1, &submit, s.fence));
  cudaExternalSemaphore_t cudaReady = s.cudaReady, cudaReleased = s.cudaReleased;
  cudaExternalSemaphoreWaitParams wp{};
  CU(cudaWaitExternalSemaphoresAsync(&cudaReady, &wp, 1, s.stream));
  CU(cudaEventRecord(s.start, s.stream));
  CU(convert_nv12(s.surface, s.nv12, pitch, width, height, s.stream));
  CU(cudaEventRecord(s.end, s.stream));
  if (s.legacyInput)
    CU(cudaMemcpyAsync(s.encodeInput, s.nv12, size_t(pitch) * height * 3 / 2,
                       cudaMemcpyDeviceToDevice, s.stream));
  cudaExternalSemaphoreSignalParams sp{};
  CU(cudaSignalExternalSemaphoresAsync(&cudaReleased, &sp, 1, s.stream));
  s.used = true;
  s.timing = t;
}
