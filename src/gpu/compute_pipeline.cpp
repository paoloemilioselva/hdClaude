#include "hdclaude/gpu/compute_pipeline.h"

#include <map>
#include <utility>

namespace hdclaude {

ComputePipeline::ComputePipeline(const VulkanContext& context,
                                 const std::vector<std::uint32_t>& spirv,
                                 const std::vector<BindingDescription>& bindings,
                                 std::uint32_t pushConstantBytes,
                                 std::string debugName)
    : _context(&context),
      _bindings(bindings),
      _pushConstantBytes(pushConstantBytes),
      _debugName(std::move(debugName))
{
    context.RequireLive("ComputePipeline");

    if (spirv.empty()) {
        throw VulkanError(VK_ERROR_INITIALIZATION_FAILED,
                          "Empty SPIR-V for pipeline " + _debugName);
    }

    // Everything is built into locals and published at the end, so a failure
    // part-way leaves this object empty rather than half-constructed.
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    const VkDevice device = context.Device();

    auto releaseAll = [&] {
        if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, layout, nullptr);
        if (pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (module != VK_NULL_HANDLE) vkDestroyShaderModule(device, module, nullptr);
    };

    try {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
        moduleInfo.pCode = spirv.data();
        context.Check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module),
                      "vkCreateShaderModule(" + _debugName + ")");

        // --- Descriptor set layout ------------------------------------------
        std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
        std::vector<VkDescriptorBindingFlags> bindingFlags;
        layoutBindings.reserve(bindings.size());
        bindingFlags.reserve(bindings.size());

        for (const BindingDescription& binding : bindings) {
            VkDescriptorSetLayoutBinding entry{};
            entry.binding = binding.binding;
            entry.descriptorType = binding.type;
            entry.descriptorCount = binding.count;
            entry.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            layoutBindings.push_back(entry);

            bindingFlags.push_back(
                binding.partiallyBound
                    ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                    : VkDescriptorBindingFlags{0});
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        flagsInfo.bindingCount = static_cast<std::uint32_t>(bindingFlags.size());
        flagsInfo.pBindingFlags = bindingFlags.data();

        VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setLayoutInfo.bindingCount = static_cast<std::uint32_t>(layoutBindings.size());
        setLayoutInfo.pBindings = layoutBindings.data();
        setLayoutInfo.pNext = &flagsInfo;
        context.Check(
            vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &setLayout),
            "vkCreateDescriptorSetLayout(" + _debugName + ")");

        // --- Descriptor pool, sized from the binding table -------------------
        // R10: derived, never restated. Adding a binding cannot exhaust the
        // pool, because the pool is computed from the same list the layout is.
        std::map<VkDescriptorType, std::uint32_t> counts;
        for (const BindingDescription& binding : bindings) {
            counts[binding.type] += binding.count * kDescriptorSetsPerPipeline;
        }
        std::vector<VkDescriptorPoolSize> poolSizes;
        poolSizes.reserve(counts.size());
        for (const auto& [type, count] : counts) {
            poolSizes.push_back({type, count});
        }

        if (!poolSizes.empty()) {
            VkDescriptorPoolCreateInfo poolInfo{
                VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets = kDescriptorSetsPerPipeline;
            poolInfo.poolSizeCount = static_cast<std::uint32_t>(poolSizes.size());
            poolInfo.pPoolSizes = poolSizes.data();
            context.Check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool),
                          "vkCreateDescriptorPool(" + _debugName + ")");
        }

        // --- Pipeline layout -------------------------------------------------
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pushRange.offset = 0;
        pushRange.size = pushConstantBytes;

        VkPipelineLayoutCreateInfo layoutInfo{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        layoutInfo.pushConstantRangeCount = pushConstantBytes > 0 ? 1 : 0;
        layoutInfo.pPushConstantRanges = pushConstantBytes > 0 ? &pushRange : nullptr;
        context.Check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout),
                      "vkCreatePipelineLayout(" + _debugName + ")");

        // --- Pipeline ---------------------------------------------------------
        VkPipelineShaderStageCreateInfo stage{
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipelineInfo{
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = stage;
        pipelineInfo.layout = layout;
        context.Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                               &pipelineInfo, nullptr, &pipeline),
                      "vkCreateComputePipelines(" + _debugName + ")");
    } catch (...) {
        releaseAll();
        _context = nullptr;
        throw;
    }

    // Publish.
    _module = module;
    _setLayout = setLayout;
    _layout = layout;
    _pipeline = pipeline;
    _pool = pool;
    _generation = NextResourceGeneration();
}

void ComputePipeline::Reset()
{
    if (_context != nullptr) {
        const VkDevice device = _context->Device();
        if (_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, _pipeline, nullptr);
        if (_layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, _layout, nullptr);
        if (_pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(device, _pool, nullptr);
        if (_setLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(device, _setLayout, nullptr);
        if (_module != VK_NULL_HANDLE) vkDestroyShaderModule(device, _module, nullptr);
    }
    _context = nullptr;
    _module = VK_NULL_HANDLE;
    _setLayout = VK_NULL_HANDLE;
    _layout = VK_NULL_HANDLE;
    _pipeline = VK_NULL_HANDLE;
    _pool = VK_NULL_HANDLE;
    _bindings.clear();
    _pushConstantBytes = 0;
    _generation = 0;
}

ComputePipeline::~ComputePipeline() { Reset(); }

ComputePipeline::ComputePipeline(ComputePipeline&& other) noexcept
    : _context(std::exchange(other._context, nullptr)),
      _module(std::exchange(other._module, VK_NULL_HANDLE)),
      _setLayout(std::exchange(other._setLayout, VK_NULL_HANDLE)),
      _layout(std::exchange(other._layout, VK_NULL_HANDLE)),
      _pipeline(std::exchange(other._pipeline, VK_NULL_HANDLE)),
      _pool(std::exchange(other._pool, VK_NULL_HANDLE)),
      _bindings(std::move(other._bindings)),
      _pushConstantBytes(std::exchange(other._pushConstantBytes, 0)),
      _generation(std::exchange(other._generation, 0)),
      _debugName(std::move(other._debugName))
{
}

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept
{
    if (this != &other) {
        Reset();
        _context = std::exchange(other._context, nullptr);
        _module = std::exchange(other._module, VK_NULL_HANDLE);
        _setLayout = std::exchange(other._setLayout, VK_NULL_HANDLE);
        _layout = std::exchange(other._layout, VK_NULL_HANDLE);
        _pipeline = std::exchange(other._pipeline, VK_NULL_HANDLE);
        _pool = std::exchange(other._pool, VK_NULL_HANDLE);
        _bindings = std::move(other._bindings);
        _pushConstantBytes = std::exchange(other._pushConstantBytes, 0);
        _generation = std::exchange(other._generation, 0);
        _debugName = std::move(other._debugName);
    }
    return *this;
}

VkDescriptorSet ComputePipeline::AllocateSet()
{
    _context->RequireLive("ComputePipeline::AllocateSet");

    VkDescriptorSetAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = _pool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &_setLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    _context->Check(vkAllocateDescriptorSets(_context->Device(), &allocateInfo, &set),
                    "vkAllocateDescriptorSets(" + _debugName + ")");
    return set;
}

void ComputePipeline::WriteSampledImageArray(
    VkDescriptorSet set, std::uint32_t binding,
    const std::vector<VkDescriptorImageInfo>& images) const
{
    if (images.empty() || _context == nullptr) {
        return;
    }
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorCount = static_cast<std::uint32_t>(images.size());
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = images.data();
    vkUpdateDescriptorSets(_context->Device(), 1, &write, 0, nullptr);
}

void ComputePipeline::ResetSets()
{
    if (_pool == VK_NULL_HANDLE || _context == nullptr ||
        _context->IsDeviceLost()) {
        return;
    }
    _context->Check(vkResetDescriptorPool(_context->Device(), _pool, 0),
                    "vkResetDescriptorPool(" + _debugName + ")");
}

void ComputePipeline::WriteBuffer(VkDescriptorSet set, std::uint32_t binding,
                                  const VulkanBuffer& buffer,
                                  VkDescriptorType type) const
{
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = buffer.Handle();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = type;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(_context->Device(), 1, &write, 0, nullptr);
}

void ComputePipeline::WriteStorageImage(VkDescriptorSet set, std::uint32_t binding,
                                        const VulkanImage& image,
                                        VkImageLayout layout) const
{
    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = image.View();
    imageInfo.imageLayout = layout;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(_context->Device(), 1, &write, 0, nullptr);
}

void ComputePipeline::Dispatch(VkCommandBuffer command, VkDescriptorSet set,
                               std::uint32_t groupsX, std::uint32_t groupsY,
                               std::uint32_t groupsZ, const void* pushConstants,
                               std::uint32_t pushConstantBytes) const
{
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1,
                            &set, 0, nullptr);
    if (pushConstants != nullptr && pushConstantBytes > 0) {
        vkCmdPushConstants(command, _layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           pushConstantBytes, pushConstants);
    }
    vkCmdDispatch(command, groupsX, groupsY, groupsZ);
}

void ComputePipeline::DispatchIndirect(VkCommandBuffer command,
                                       VkDescriptorSet set,
                                       const VulkanBuffer& args,
                                       VkDeviceSize offset,
                                       const void* pushConstants,
                                       std::uint32_t pushConstantBytes) const
{
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, 1,
                            &set, 0, nullptr);
    if (pushConstants != nullptr && pushConstantBytes > 0) {
        vkCmdPushConstants(command, _layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           pushConstantBytes, pushConstants);
    }
    vkCmdDispatchIndirect(command, args.Handle(), offset);
}

}  // namespace hdclaude
