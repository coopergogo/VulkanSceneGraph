/* <editor-fold desc="MIT License">

Copyright(c) 2018 Robert Osfield

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

</editor-fold> */

#include <vsg/io/Logger.h>
#include <vsg/traversals/RecordTraversal.h>
#include <vsg/ui/ApplicationEvent.h>
#include <vsg/viewer/RecordAndSubmitTask.h>
#include <vsg/viewer/View.h>
#include <vsg/vk/State.h>

using namespace vsg;

RecordAndSubmitTask::RecordAndSubmitTask(Device* in_device, uint32_t numBuffers) :
    device(in_device)
{
    _currentFrameIndex = numBuffers; // numBuffers is used to signify unset value
    for (uint32_t i = 0; i < numBuffers; ++i)
    {
        _indices.emplace_back(numBuffers); // numBuffers is used to signify unset value
    }

    _frames.resize(numBuffers);
    for (uint32_t i = 0; i < numBuffers; ++i)
    {
        _frames[i].fence = vsg::Fence::create(device);
    }
}

void RecordAndSubmitTask::advance()
{
    if (_currentFrameIndex >= _indices.size())
    {
        // first frame so set to 0
        _currentFrameIndex = 0;
    }
    else
    {
        ++_currentFrameIndex;
        if (_currentFrameIndex > _indices.size() - 1) _currentFrameIndex = 0;

        // shift the index for previous frames
        for (size_t i = _indices.size() - 1; i >= 1; --i)
        {
            _indices[i] = _indices[i - 1];
        }
    }

    // pass the index for the current frame
    _indices[0] = _currentFrameIndex;
}

size_t RecordAndSubmitTask::index(size_t relativeFrameIndex) const
{
    return relativeFrameIndex < _indices.size() ? _indices[relativeFrameIndex] : _indices.size();
}

/// fence() and fence(0) return the Fence for the frame currently being rendered, fence(1) return the previous frame's Fence etc.
Fence* RecordAndSubmitTask::fence(size_t relativeFrameIndex)
{
    size_t i = index(relativeFrameIndex);
    return i < _frames.size() ? _frames[i].fence.get() : nullptr;
}

VkResult RecordAndSubmitTask::submit(ref_ptr<FrameStamp> frameStamp)
{
    CommandBuffers recordedCommandBuffers;
    if (VkResult result = start(); result != VK_SUCCESS) return result;
    if (VkResult result = record(recordedCommandBuffers, frameStamp); result != VK_SUCCESS) return result;
    return finish(recordedCommandBuffers);
}

VkResult RecordAndSubmitTask::start()
{
    currentTransferCompletedSemaphore = {};

    auto current_fence = fence();
    if (current_fence->hasDependencies())
    {
        uint64_t timeout = std::numeric_limits<uint64_t>::max();
        if (VkResult result = current_fence->wait(timeout); result != VK_SUCCESS) return result;

        current_fence->resetFenceAndDependencies();
    }
    return VK_SUCCESS;
}

VkResult RecordAndSubmitTask::record(CommandBuffers& recordedCommandBuffers, ref_ptr<FrameStamp> frameStamp)
{
    for (auto& commandGraph : commandGraphs)
    {
        commandGraph->record(recordedCommandBuffers, frameStamp, databasePager);
    }

    if (VkResult result = transferDynamicData(); result != VK_SUCCESS) return result;

    return VK_SUCCESS;
}

void RecordAndSubmitTask::assignDynamicBufferInfos(const BufferInfoList& bufferInfoList)
{
    for (auto& bufferInfo : bufferInfoList)
    {
        _dynamicDataMap[bufferInfo->buffer][bufferInfo->offset] = bufferInfo;
    }

    // compute total size
    VkDeviceSize offset = 0;
    VkDeviceSize alignment = 4;

    _dynamicDataTotalRegions = 0;
    for(auto& [buffer, bufferInfos] : _dynamicDataMap)
    {
        for(auto& offset_bufferInfo : bufferInfos)
        {
            auto& bufferInfo = offset_bufferInfo.second;
            VkDeviceSize endOfEntry = offset + bufferInfo->range;
            offset = (alignment == 1 || (endOfEntry % alignment) == 0) ? endOfEntry : ((endOfEntry / alignment) + 1) * alignment;
            ++_dynamicDataTotalRegions;
        }
    }
    _dynamicDataTotalSize = offset;
}

VkResult RecordAndSubmitTask::transferDynamicData()
{
    Logger::Level level = Logger::LOGGER_DEBUG;
    //level = Logger::LOGGER_INFO;

    size_t frameIndex = index(0);
    if (frameIndex < _frames.size() && !_dynamicDataMap.empty())
    {
        uint32_t deviceID = device->deviceID;
        auto& frame = _frames[frameIndex];
        auto& staging = frame.staging;
        auto& commandBuffer = frame.transferCommandBuffer;
        auto& semaphore = frame.transferCompledSemaphore;
        auto& copyRegions = frame.copyRegions;
        auto& buffer_data = frame.buffer_data;

        log(level, "RecordAndSubmitTask::record() ", _currentFrameIndex, ", _dynamicDataMap.size() ", _dynamicDataMap.size());
        log(level, "   transferQueue = ", transferQueue);
        log(level, "   queue = ", queue);
        log(level, "   staging = ", staging);
        log(level, "   copyRegions.size() = ", copyRegions.size());

        if (!commandBuffer)
        {
            auto cp = CommandPool::create(device, transferQueue->queueFamilyIndex());
            commandBuffer = cp->allocate(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        }
        else
        {
            commandBuffer->reset();
        }

        if (!semaphore)
        {
            semaphore = Semaphore::create(device, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }

        VkResult result = VK_SUCCESS;

        // allocate staging buffer if required
        if (!staging || staging->size < _dynamicDataTotalSize)
        {
            VkMemoryPropertyFlags stagingMemoryPropertiesFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            staging = vsg::createBufferAndMemory(device, _dynamicDataTotalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, stagingMemoryPropertiesFlags);

            auto stagingMemory = staging->getDeviceMemory(deviceID);
            buffer_data = nullptr;
            result = stagingMemory->map(staging->getMemoryOffset(deviceID), staging->size, 0, &buffer_data);
            if (result != VK_SUCCESS) return result;
        }

        log(level, "   totalSize = ", _dynamicDataTotalSize);

        VkDeviceSize offset = 0;
        VkDeviceSize alignment = 4;

        copyRegions.clear();
        copyRegions.resize(_dynamicDataTotalRegions);
        VkBufferCopy* pRegions = copyRegions.data();

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        VkCommandBuffer vk_commandBuffer = *commandBuffer;
        vkBeginCommandBuffer(vk_commandBuffer, &beginInfo);

        for(auto buffer_itr = _dynamicDataMap.begin(); buffer_itr != _dynamicDataMap.end();)
        {
            auto& buffer = buffer_itr->first;
            auto& bufferInfos = buffer_itr->second;

            uint32_t regionCount = 0;
            for(auto bufferInfo_itr = bufferInfos.begin(); bufferInfo_itr != bufferInfos.end();)
            {
                auto& bufferInfo = bufferInfo_itr->second;
                if (bufferInfo->referenceCount()==1)
                {
                    log(level, "BufferInfo only ref left ", bufferInfo, ", ", bufferInfo->referenceCount());
                    bufferInfo_itr = bufferInfos.erase(bufferInfo_itr);
                }
                else
                {
                    if (bufferInfo->data->getModifiedCount(bufferInfo->copiedModifiedCounts[deviceID]))
                    {
                        // copy data to staging buffer memory
                        char* ptr = reinterpret_cast<char*>(buffer_data) + offset;
                        std::memcpy(ptr, bufferInfo->data->dataPointer(), bufferInfo->range);

                        // record region
                        pRegions[regionCount++] = VkBufferCopy{offset, bufferInfo->offset, bufferInfo->range};

                        log(level, "       copying ", bufferInfo, ", ", bufferInfo->data, " to ", (void*)ptr);

                        VkDeviceSize endOfEntry = offset + bufferInfo->range;
                        offset = (alignment == 1 || (endOfEntry % alignment) == 0) ? endOfEntry : ((endOfEntry / alignment) + 1) * alignment;
                    }
                    ++bufferInfo_itr;
                }
            }

            if (regionCount > 0)
            {
                vkCmdCopyBuffer(vk_commandBuffer, staging->vk(deviceID), buffer->vk(deviceID), regionCount, pRegions);
                log(level, "   vkCmdCopyBuffer(", ", ", staging->vk(deviceID), ", ", buffer->vk(deviceID), ", ", regionCount, ", ", pRegions);

                // advance to next buffer
                pRegions += regionCount;
            }

            if (bufferInfos.empty())
            {
                log(level, "bufferInfos.empty()");
                buffer_itr = _dynamicDataMap.erase(buffer_itr);
            }
            else
            {
                ++buffer_itr;
            }
        }

        vkEndCommandBuffer(vk_commandBuffer);

        // if no regions to copy have been found then commandBuffer will be empty so no need to submit it to queue and use the assocaited single semaphore
        if (pRegions != copyRegions.data())
        {
            // submit the transfer commands
            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

            submitInfo.waitSemaphoreCount = 0;
            submitInfo.pWaitSemaphores = nullptr;
            submitInfo.pWaitDstStageMask = nullptr;

            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &vk_commandBuffer;

            submitInfo.signalSemaphoreCount = 1;
            VkSemaphore vk_transferCompletedSemaphore = *semaphore;
            submitInfo.pSignalSemaphores = &vk_transferCompletedSemaphore;

            result = transferQueue->submit(submitInfo);
            if (result != VK_SUCCESS) return result;

            currentTransferCompletedSemaphore = semaphore;
        }
        else
        {
            log(level, "Nothing to submit");
        }
    }
    return VK_SUCCESS;
}

VkResult RecordAndSubmitTask::finish(CommandBuffers& recordedCommandBuffers)
{
    auto current_fence = fence();

    if (recordedCommandBuffers.empty())
    {
        // nothing to do so return early
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // sleep for 1/60th of a second
        return VK_SUCCESS;
    }

    // convert VSG CommandBuffer to Vulkan handles and add to the Fence's list of dependent CommandBuffers
    std::vector<VkCommandBuffer> vk_commandBuffers;
    std::vector<VkSemaphore> vk_waitSemaphores;
    std::vector<VkPipelineStageFlags> vk_waitStages;
    std::vector<VkSemaphore> vk_signalSemaphores;

    // convert VSG CommandBuffer to Vulkan handles and add to the Fence's list of dependent CommandBuffers
    for (auto& commandBuffer : recordedCommandBuffers)
    {
        if (commandBuffer->level() == VK_COMMAND_BUFFER_LEVEL_PRIMARY) vk_commandBuffers.push_back(*commandBuffer);

        current_fence->dependentCommandBuffers().emplace_back(commandBuffer);
    }

    current_fence->dependentSemaphores() = signalSemaphores;

    if (currentTransferCompletedSemaphore)
    {
        vk_waitSemaphores.emplace_back(*currentTransferCompletedSemaphore);
        vk_waitStages.emplace_back(currentTransferCompletedSemaphore->pipelineStageFlags());
    }

    for (auto& window : windows)
    {
        auto imageIndex = window->imageIndex();
        if (imageIndex >= window->numFrames()) continue;

        auto& semaphore = window->frame(imageIndex).imageAvailableSemaphore;

        vk_waitSemaphores.emplace_back(*semaphore);
        vk_waitStages.emplace_back(semaphore->pipelineStageFlags());
    }

    for (auto& semaphore : waitSemaphores)
    {
        vk_waitSemaphores.emplace_back(*(semaphore));
        vk_waitStages.emplace_back(semaphore->pipelineStageFlags());
    }

    for (auto& semaphore : signalSemaphores)
    {
        vk_signalSemaphores.emplace_back(*(semaphore));
    }

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(vk_waitSemaphores.size());
    submitInfo.pWaitSemaphores = vk_waitSemaphores.data();
    submitInfo.pWaitDstStageMask = vk_waitStages.data();

    submitInfo.commandBufferCount = static_cast<uint32_t>(vk_commandBuffers.size());
    submitInfo.pCommandBuffers = vk_commandBuffers.data();

    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(vk_signalSemaphores.size());
    submitInfo.pSignalSemaphores = vk_signalSemaphores.data();

    return queue->submit(submitInfo, current_fence);
}

void vsg::updateTasks(RecordAndSubmitTasks& tasks, ref_ptr<CompileManager> compileManager, const CompileResult& compileResult)
{
    //info("vsg::updateTasks(RecordAndSubmitTasks& tasks..) ", compileResult.dynamicBufferInfos.size());
    if (!compileResult.dynamicBufferInfos.empty())
    {
        //for(auto& bufferInfo : compileResult.dynamicBufferInfos)
        //{
        //    info("    ", bufferInfo, ", ", bufferInfo->data);
        //}
        for (auto& task : tasks)
        {
            task->assignDynamicBufferInfos(compileResult.dynamicBufferInfos);
        }
    }

    // assign database pager if required
    for (auto& task : tasks)
    {
        for (auto& commandGraph : task->commandGraphs)
        {
            if (compileResult.maxSlot > commandGraph->maxSlot)
            {
                commandGraph->maxSlot = compileResult.maxSlot;
            }
        }
    }

    // assign database pager if required
    if (compileResult.containsPagedLOD)
    {
        vsg::ref_ptr<vsg::DatabasePager> databasePager;
        for (auto& task : tasks)
        {
            if (task->databasePager && !databasePager) databasePager = task->databasePager;
        }

        if (!databasePager)
        {
            databasePager = vsg::DatabasePager::create();
            for (auto& task : tasks)
            {
                if (!task->databasePager)
                {
                    task->databasePager = databasePager;
                    task->databasePager->compileManager = compileManager;
                }
            }

            databasePager->start();
        }
    }

    /// handle any need Bin needs
    for (auto& [const_view, binDetails] : compileResult.views)
    {
        auto view = const_cast<vsg::View*>(const_view);
        for (auto& binNumber : binDetails.indices)
        {
            bool binNumberMatched = false;
            for (auto& bin : view->bins)
            {
                if (bin->binNumber == binNumber)
                {
                    binNumberMatched = true;
                }
            }
            if (!binNumberMatched)
            {
                vsg::Bin::SortOrder sortOrder = (binNumber < 0) ? vsg::Bin::ASCENDING : ((binNumber == 0) ? vsg::Bin::NO_SORT : vsg::Bin::DESCENDING);
                view->bins.push_back(vsg::Bin::create(binNumber, sortOrder));
            }
        }
    }
}
