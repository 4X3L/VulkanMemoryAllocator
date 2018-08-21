//
// Copyright (c) 2018 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "VmaUsage.h"
#include "Common.h"
#include <unordered_map>

static const int RESULT_EXCEPTION          = -1000;
static const int RESULT_ERROR_COMMAND_LINE = -1;
static const int RESULT_ERROR_SOURCE_FILE  = -2;
static const int RESULT_ERROR_FORMAT       = -3;
static const int RESULT_ERROR_VULKAN       = -4;

enum CMD_LINE_OPT
{
    CMD_LINE_OPT_VERBOSITY,
    CMD_LINE_OPT_ITERATIONS,
    CMD_LINE_OPT_LINES,
    CMD_LINE_OPT_PHYSICAL_DEVICE,
    CMD_LINE_OPT_USER_DATA,
};

static enum class VERBOSITY
{
    MINIMUM = 0,
    DEFAULT,
    MAXIMUM,
    COUNT,
} g_Verbosity = VERBOSITY::DEFAULT;

enum class OBJECT_TYPE { BUFFER, IMAGE };

enum class VMA_FUNCTION
{
    CreatePool,
    DestroyPool,
    SetAllocationUserData,
    CreateBuffer,
    DestroyBuffer,
    CreateImage,
    DestroyImage,
    FreeMemory,
    CreateLostAllocation,
    AllocateMemory,
    AllocateMemoryForBuffer,
    AllocateMemoryForImage,
    MapMemory,
    UnmapMemory,
    FlushAllocation,
    InvalidateAllocation,
    TouchAllocation,
    GetAllocationInfo,
    MakePoolAllocationsLost,
    Count
};
static const char* VMA_FUNCTION_NAMES[] = {
    "vmaCreatePool",
    "vmaDestroyPool",
    "vmaSetAllocationUserData",
    "vmaCreateBuffer",
    "vmaDestroyBuffer",
    "vmaCreateImage",
    "vmaDestroyImage",
    "vmaFreeMemory",
    "vmaCreateLostAllocation",
    "vmaAllocateMemory",
    "vmaAllocateMemoryForBuffer",
    "vmaAllocateMemoryForImage",
    "vmaMapMemory",
    "vmaUnmapMemory",
    "vmaFlushAllocation",
    "vmaInvalidateAllocation",
    "vmaTouchAllocation",
    "vmaGetAllocationInfo",
    "vmaMakePoolAllocationsLost",
};
static_assert(
    _countof(VMA_FUNCTION_NAMES) == (size_t)VMA_FUNCTION::Count,
    "VMA_FUNCTION_NAMES array doesn't match VMA_FUNCTION enum.");

// Set this to false to disable deleting leaked VmaAllocation, VmaPool objects
// and let VMA report asserts about them.
static const bool CLEANUP_LEAKED_OBJECTS = true;

static std::string g_FilePath;
// Most significant 16 bits are major version, least significant 16 bits are minor version.
static uint32_t g_FileVersion;

static size_t g_IterationCount = 1;
static uint32_t g_PhysicalDeviceIndex = 0;
static RangeSequence<size_t> g_LineRanges;
static bool g_UserDataEnabled = true;

static bool ValidateFileVersion()
{
    const uint32_t major = g_FileVersion >> 16;
    const uint32_t minor = g_FileVersion & 0xFFFF;
    return major == 1 && minor <= 2;
}

static bool ParseFileVersion(const StrRange& s)
{
    CsvSplit csvSplit;
    csvSplit.Set(s, 2);
    uint32_t major, minor;
    if(csvSplit.GetCount() == 2 &&
        StrRangeToUint(csvSplit.GetRange(0), major) &&
        StrRangeToUint(csvSplit.GetRange(1), minor))
    {
        g_FileVersion = (major << 16) | minor;
        return true;
    }
    else
    {
        return false;
    }
}

////////////////////////////////////////////////////////////////////////////////
// class Statistics

class Statistics
{
public:
    static uint32_t BufferUsageToClass(uint32_t usage);
    static uint32_t ImageUsageToClass(uint32_t usage);

    Statistics() { }

    const size_t* GetFunctionCallCount() const { return m_FunctionCallCount; }
    size_t GetImageCreationCount(uint32_t imgClass) const { return m_ImageCreationCount[imgClass]; }
    size_t GetLinearImageCreationCount() const { return m_LinearImageCreationCount; }
    size_t GetBufferCreationCount(uint32_t bufClass) const { return m_BufferCreationCount[bufClass]; }
    size_t GetAllocationCreationCount() const { return m_AllocationCreationCount; }
    size_t GetPoolCreationCount() const { return m_PoolCreationCount; }

    void RegisterFunctionCall(VMA_FUNCTION func);
    void RegisterCreateImage(uint32_t usage, uint32_t tiling);
    void RegisterCreateBuffer(uint32_t usage);
    void RegisterCreatePool();
    void RegisterCreateAllocation();

private:
    size_t m_FunctionCallCount[(size_t)VMA_FUNCTION::Count] = {};
    size_t m_ImageCreationCount[4] = { };
    size_t m_LinearImageCreationCount = 0;
    size_t m_BufferCreationCount[4] = { };
    size_t m_AllocationCreationCount = 0; // Also includes buffers and images, and lost allocations.
    size_t m_PoolCreationCount = 0;
};

uint32_t Statistics::BufferUsageToClass(uint32_t usage)
{
    // Buffer is used as source of data for fixed-function stage of graphics pipeline.
    // It's indirect, vertex, or index buffer.
    if ((usage & (VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) != 0)
    {
        return 0;
    }
    // Buffer is accessed by shaders for load/store/atomic.
    // Aka "UAV"
    else if ((usage & (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) != 0)
    {
        return 1;
    }
    // Buffer is accessed by shaders for reading uniform data.
    // Aka "constant buffer"
    else if ((usage & (VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)) != 0)
    {
        return 2;
    }
    // Any other type of buffer.
    // Notice that VK_BUFFER_USAGE_TRANSFER_SRC_BIT and VK_BUFFER_USAGE_TRANSFER_DST_BIT
    // flags are intentionally ignored.
    else
    {
        return 3;
    }
}

uint32_t Statistics::ImageUsageToClass(uint32_t usage)
{
    // Image is used as depth/stencil "texture/surface".
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
    {
        return 0;
    }
    // Image is used as other type of attachment.
    // Aka "render target"
    else if ((usage & (VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) != 0)
    {
        return 1;
    }
    // Image is accessed by shaders for sampling.
    // Aka "texture"
    else if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0)
    {
        return 2;
    }
    // Any other type of image.
    // Notice that VK_IMAGE_USAGE_TRANSFER_SRC_BIT and VK_IMAGE_USAGE_TRANSFER_DST_BIT
    // flags are intentionally ignored.
    else
    {
        return 3;
    }
}

void Statistics::RegisterFunctionCall(VMA_FUNCTION func)
{
    ++m_FunctionCallCount[(size_t)func];
}

void Statistics::RegisterCreateImage(uint32_t usage, uint32_t tiling)
{
    if(tiling == VK_IMAGE_TILING_LINEAR)
        ++m_LinearImageCreationCount;
    else
    {
        const uint32_t imgClass = ImageUsageToClass(usage);
        ++m_ImageCreationCount[imgClass];
    }

    ++m_AllocationCreationCount;
}

void Statistics::RegisterCreateBuffer(uint32_t usage)
{
    const uint32_t bufClass = BufferUsageToClass(usage);
    ++m_BufferCreationCount[bufClass];

    ++m_AllocationCreationCount;
}

void Statistics::RegisterCreatePool()
{
    ++m_PoolCreationCount;
}

void Statistics::RegisterCreateAllocation()
{
    ++m_AllocationCreationCount;
}

////////////////////////////////////////////////////////////////////////////////
// class Player

static const char* const VALIDATION_LAYER_NAME = "VK_LAYER_LUNARG_standard_validation";

static bool g_MemoryAliasingWarningEnabled = true;
static bool g_EnableValidationLayer = true;
static bool VK_KHR_get_memory_requirements2_enabled = false;
static bool VK_KHR_dedicated_allocation_enabled = false;

static VKAPI_ATTR VkBool32 VKAPI_CALL MyDebugReportCallback(
    VkDebugReportFlagsEXT flags,
    VkDebugReportObjectTypeEXT objectType,
    uint64_t object,
    size_t location,
    int32_t messageCode,
    const char* pLayerPrefix,
    const char* pMessage,
    void* pUserData)
{
    // "Non-linear image 0xebc91 is aliased with linear buffer 0xeb8e4 which may indicate a bug."
    if(!g_MemoryAliasingWarningEnabled && flags == VK_DEBUG_REPORT_WARNING_BIT_EXT &&
        (strstr(pMessage, " is aliased with non-linear ") || strstr(pMessage, " is aliased with linear ")))
    {
        return VK_FALSE;
    }

    // Ignoring because when VK_KHR_dedicated_allocation extension is enabled,
    // vkGetBufferMemoryRequirements2KHR function is used instead, while Validation
    // Layer seems to be unaware of it.
    if (strstr(pMessage, "but vkGetBufferMemoryRequirements() has not been called on that buffer") != nullptr)
    {
        return VK_FALSE;
    }
    if (strstr(pMessage, "but vkGetImageMemoryRequirements() has not been called on that image") != nullptr)
    {
        return VK_FALSE;
    }
    
    /*
    "Mapping an image with layout VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL can result in undefined behavior if this memory is used by the device. Only GENERAL or PREINITIALIZED should be used."
    Ignoring because we map entire VkDeviceMemory blocks, where different types of
    images and buffers may end up together, especially on GPUs with unified memory
    like Intel.
    */
    if(strstr(pMessage, "Mapping an image with layout") != nullptr &&
        strstr(pMessage, "can result in undefined behavior if this memory is used by the device") != nullptr)
    {
        return VK_FALSE;
    }

    printf("%s \xBA %s\n", pLayerPrefix, pMessage);

    return VK_FALSE;
}

static bool IsLayerSupported(const VkLayerProperties* pProps, size_t propCount, const char* pLayerName)
{
    const VkLayerProperties* propsEnd = pProps + propCount;
    return std::find_if(
        pProps,
        propsEnd,
        [pLayerName](const VkLayerProperties& prop) -> bool {
            return strcmp(pLayerName, prop.layerName) == 0;
        }) != propsEnd;
}

static const size_t FIRST_PARAM_INDEX = 4;

static void InitVulkanFeatures(
    VkPhysicalDeviceFeatures& outFeatures,
    const VkPhysicalDeviceFeatures& supportedFeatures)
{
    ZeroMemory(&outFeatures, sizeof(outFeatures));

    // Enable something what may interact with memory/buffer/image support.

    outFeatures.fullDrawIndexUint32 = supportedFeatures.fullDrawIndexUint32;
    outFeatures.imageCubeArray = supportedFeatures.imageCubeArray;
    outFeatures.geometryShader = supportedFeatures.geometryShader;
    outFeatures.tessellationShader = supportedFeatures.tessellationShader;
    outFeatures.multiDrawIndirect = supportedFeatures.multiDrawIndirect;
    outFeatures.textureCompressionETC2 = supportedFeatures.textureCompressionETC2;
    outFeatures.textureCompressionASTC_LDR = supportedFeatures.textureCompressionASTC_LDR;
    outFeatures.textureCompressionBC = supportedFeatures.textureCompressionBC;
}

class Player
{
public:
    Player();
    int Init();
    ~Player();

    void ExecuteLine(size_t lineNumber, const StrRange& line);
    void PrintStats();

private:
    static const size_t MAX_WARNINGS_TO_SHOW = 64;

    size_t m_WarningCount = 0;
    bool m_AllocateForBufferImageWarningIssued = false;

    VkInstance m_VulkanInstance = VK_NULL_HANDLE;
    VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
    uint32_t m_GraphicsQueueFamilyIndex = UINT_MAX;
    VkDevice m_Device = VK_NULL_HANDLE;
    VmaAllocator m_Allocator = VK_NULL_HANDLE;

    PFN_vkCreateDebugReportCallbackEXT m_pvkCreateDebugReportCallbackEXT;
    PFN_vkDebugReportMessageEXT m_pvkDebugReportMessageEXT;
    PFN_vkDestroyDebugReportCallbackEXT m_pvkDestroyDebugReportCallbackEXT;
    VkDebugReportCallbackEXT m_hCallback;

    uint32_t m_VmaFrameIndex = 0;

    // Any of these handles null can mean it was created in original but couldn't be created now.
    struct Pool
    {
        VmaPool pool;
    };
    struct Allocation
    {
        uint32_t allocationFlags;
        VmaAllocation allocation;
        VkBuffer buffer;
        VkImage image;
    };
    std::unordered_map<uint64_t, Pool> m_Pools;
    std::unordered_map<uint64_t, Allocation> m_Allocations;

    struct Thread
    {
        uint32_t callCount;
    };
    std::unordered_map<uint32_t, Thread> m_Threads;

    // Copy of column [1] from previously parsed line.
    std::string m_LastLineTimeStr;
    Statistics m_Stats;

    std::vector<char> m_UserDataTmpStr;

    void Destroy(const Allocation& alloc);

    // Finds VmaPool bu original pointer.
    // If origPool = null, returns true and outPool = null.
    // If failed, prints warning, returns false and outPool = null.
    bool FindPool(size_t lineNumber, uint64_t origPool, VmaPool& outPool);
    // If allocation with that origPtr already exists, prints warning and replaces it.
    void AddAllocation(size_t lineNumber, uint64_t origPtr, VkResult res, const char* functionName, Allocation&& allocDesc);

    // Increments warning counter. Returns true if warning message should be printed.
    bool IssueWarning();

    int InitVulkan();
    void FinalizeVulkan();
    void RegisterDebugCallbacks();

    // If parmeter count doesn't match, issues warning and returns false.
    bool ValidateFunctionParameterCount(size_t lineNumber, const CsvSplit& csvSplit, size_t expectedParamCount, bool lastUnbound);

    // If failed, prints warning, returns false, and sets allocCreateInfo.pUserData to null.
    bool PrepareUserData(size_t lineNumber, uint32_t allocCreateFlags, const StrRange& userDataColumn, const StrRange& wholeLine, void*& outUserData);

    void ExecuteCreatePool(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteDestroyPool(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteSetAllocationUserData(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteCreateBuffer(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteDestroyBuffer(size_t lineNumber, const CsvSplit& csvSplit) { m_Stats.RegisterFunctionCall(VMA_FUNCTION::DestroyBuffer); DestroyAllocation(lineNumber, csvSplit); }
    void ExecuteCreateImage(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteDestroyImage(size_t lineNumber, const CsvSplit& csvSplit) { m_Stats.RegisterFunctionCall(VMA_FUNCTION::DestroyImage); DestroyAllocation(lineNumber, csvSplit); }
    void ExecuteFreeMemory(size_t lineNumber, const CsvSplit& csvSplit) { m_Stats.RegisterFunctionCall(VMA_FUNCTION::FreeMemory); DestroyAllocation(lineNumber, csvSplit); }
    void ExecuteCreateLostAllocation(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteAllocateMemory(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteAllocateMemoryForBufferOrImage(size_t lineNumber, const CsvSplit& csvSplit, OBJECT_TYPE objType);
    void ExecuteMapMemory(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteUnmapMemory(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteFlushAllocation(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteInvalidateAllocation(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteTouchAllocation(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteGetAllocationInfo(size_t lineNumber, const CsvSplit& csvSplit);
    void ExecuteMakePoolAllocationsLost(size_t lineNumber, const CsvSplit& csvSplit);

    void DestroyAllocation(size_t lineNumber, const CsvSplit& csvSplit);
};

Player::Player()
{
}

int Player::Init()
{
    return InitVulkan();
}

Player::~Player()
{
    FinalizeVulkan();

    if(g_Verbosity < VERBOSITY::MAXIMUM && m_WarningCount > MAX_WARNINGS_TO_SHOW)
        printf("WARNING: %zu more warnings not shown.\n", m_WarningCount - MAX_WARNINGS_TO_SHOW);
}

void Player::ExecuteLine(size_t lineNumber, const StrRange& line)
{
    CsvSplit csvSplit;
    csvSplit.Set(line);

    if(csvSplit.GetCount() >= FIRST_PARAM_INDEX)
    {
        // Check thread ID.
        uint32_t threadId;
        if(StrRangeToUint(csvSplit.GetRange(0), threadId))
        {
            const auto it = m_Threads.find(threadId);
            if(it != m_Threads.end())
            {
                ++it->second.callCount;
            }
            else
            {
                Thread threadInfo{};
                threadInfo.callCount = 1;
                m_Threads[threadId] = threadInfo;
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Incorrect thread ID.\n", lineNumber);
            }
        }

        // Save time.
        csvSplit.GetRange(1).to_str(m_LastLineTimeStr);

        // Update VMA current frame index.
        StrRange frameIndexStr = csvSplit.GetRange(2);
        uint32_t frameIndex;
        if(StrRangeToUint(frameIndexStr, frameIndex))
        {
            if(frameIndex != m_VmaFrameIndex)
            {
                vmaSetCurrentFrameIndex(m_Allocator, frameIndex);
                m_VmaFrameIndex = frameIndex;
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Incorrect frame index.\n", lineNumber);
            }
        }

        StrRange functionName = csvSplit.GetRange(3);

        if(StrRangeEq(functionName, "vmaCreateAllocator"))
        {
            if(ValidateFunctionParameterCount(lineNumber, csvSplit, 0, false))
            {
                // Nothing.
            }
        }
        else if(StrRangeEq(functionName, "vmaDestroyAllocator"))
        {
            if(ValidateFunctionParameterCount(lineNumber, csvSplit, 0, false))
            {
                // Nothing.
            }
        }
        else if(StrRangeEq(functionName, "vmaCreatePool"))
            ExecuteCreatePool(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaDestroyPool"))
            ExecuteDestroyPool(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaSetAllocationUserData"))
            ExecuteSetAllocationUserData(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaCreateBuffer"))
            ExecuteCreateBuffer(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaDestroyBuffer"))
            ExecuteDestroyBuffer(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaCreateImage"))
            ExecuteCreateImage(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaDestroyImage"))
            ExecuteDestroyImage(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaFreeMemory"))
            ExecuteFreeMemory(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaCreateLostAllocation"))
            ExecuteCreateLostAllocation(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaAllocateMemory"))
            ExecuteAllocateMemory(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaAllocateMemoryForBuffer"))
            ExecuteAllocateMemoryForBufferOrImage(lineNumber, csvSplit, OBJECT_TYPE::BUFFER);
        else if(StrRangeEq(functionName, "vmaAllocateMemoryForImage"))
            ExecuteAllocateMemoryForBufferOrImage(lineNumber, csvSplit, OBJECT_TYPE::IMAGE);
        else if(StrRangeEq(functionName, "vmaMapMemory"))
            ExecuteMapMemory(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaUnmapMemory"))
            ExecuteUnmapMemory(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaFlushAllocation"))
            ExecuteFlushAllocation(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaInvalidateAllocation"))
            ExecuteInvalidateAllocation(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaTouchAllocation"))
            ExecuteTouchAllocation(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaGetAllocationInfo"))
            ExecuteGetAllocationInfo(lineNumber, csvSplit);
        else if(StrRangeEq(functionName, "vmaMakePoolAllocationsLost"))
            ExecuteMakePoolAllocationsLost(lineNumber, csvSplit);
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Unknown function.\n", lineNumber);
            }
        }
    }
    else
    {
        if(IssueWarning())
        {
            printf("Line %zu: Too few columns.\n", lineNumber);
        }
    }
}

void Player::Destroy(const Allocation& alloc)
{
    if(alloc.buffer)
    {
        assert(alloc.image == VK_NULL_HANDLE);
        vmaDestroyBuffer(m_Allocator, alloc.buffer, alloc.allocation);
    }
    else if(alloc.image)
    {
        vmaDestroyImage(m_Allocator, alloc.image, alloc.allocation);
    }
    else
        vmaFreeMemory(m_Allocator, alloc.allocation);
}

bool Player::FindPool(size_t lineNumber, uint64_t origPool, VmaPool& outPool)
{
    outPool = VK_NULL_HANDLE;

    if(origPool != 0)
    {
        const auto poolIt = m_Pools.find(origPool);
        if(poolIt != m_Pools.end())
        {
            outPool = poolIt->second.pool;
            return true;
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Pool %llX not found.\n", lineNumber, origPool);
            }
        }
    }

    return true;
}

void Player::AddAllocation(size_t lineNumber, uint64_t origPtr, VkResult res, const char* functionName, Allocation&& allocDesc)
{
    if(origPtr)
    {
        if(res == VK_SUCCESS)
        {
            // Originally succeeded, currently succeeded.
            // Just save pointer (done below).
        }
        else
        {
            // Originally succeeded, currently failed.
            // Print warning. Save null pointer.
            if(IssueWarning())
            {
                printf("Line %zu: %s failed (%d), while originally succeeded.\n", lineNumber, functionName, res);
            }
        }

        const auto existingIt = m_Allocations.find(origPtr);
        if(existingIt != m_Allocations.end())
        {
            if(IssueWarning())
            {
                printf("Line %zu: Allocation %llX already exists.\n", lineNumber, origPtr);
            }
        }
        m_Allocations[origPtr] = std::move(allocDesc);
    }
    else
    {
        if(res == VK_SUCCESS)
        {
            // Originally failed, currently succeeded.
            // Print warning, destroy the object.
            if(IssueWarning())
            {
                printf("Line %zu: %s succeeded, originally failed.\n", lineNumber, functionName);
            }

            Destroy(allocDesc);
        }
        else
        {
            // Originally failed, currently failed.
            // Print warning.
            if(IssueWarning())
            {
                printf("Line %zu: %s failed (%d), originally also failed.\n", lineNumber, functionName, res);
            }
        }
    }
}

bool Player::IssueWarning()
{
    if(g_Verbosity < VERBOSITY::MAXIMUM)
    {
        return m_WarningCount++ < MAX_WARNINGS_TO_SHOW;
    }
    else
    {
        ++m_WarningCount;
        return true;
    }
}

int Player::InitVulkan()
{
    if(g_Verbosity == VERBOSITY::MAXIMUM)
    {
        printf("Initializing Vulkan...\n");
    }

    uint32_t instanceLayerPropCount = 0;
    VkResult res = vkEnumerateInstanceLayerProperties(&instanceLayerPropCount, nullptr);
    assert(res == VK_SUCCESS);

    std::vector<VkLayerProperties> instanceLayerProps(instanceLayerPropCount);
    if(instanceLayerPropCount > 0)
    {
        res = vkEnumerateInstanceLayerProperties(&instanceLayerPropCount, instanceLayerProps.data());
        assert(res == VK_SUCCESS);
    }

    if(g_EnableValidationLayer == true)
    {
        if(IsLayerSupported(instanceLayerProps.data(), instanceLayerProps.size(), VALIDATION_LAYER_NAME) == false)
        {
            printf("WARNING: Layer \"%s\" not supported.\n", VALIDATION_LAYER_NAME);
            g_EnableValidationLayer = false;
        }
    }

    std::vector<const char*> instanceExtensions;
    //instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    //instanceExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);

    std::vector<const char*> instanceLayers;
    if(g_EnableValidationLayer)
    {
        instanceLayers.push_back(VALIDATION_LAYER_NAME);
        instanceExtensions.push_back("VK_EXT_debug_report");
    }

    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "VmaReplay";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Vulkan Memory Allocator";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instInfo = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = (uint32_t)instanceExtensions.size();
    instInfo.ppEnabledExtensionNames = instanceExtensions.data();
    instInfo.enabledLayerCount = (uint32_t)instanceLayers.size();
    instInfo.ppEnabledLayerNames = instanceLayers.data();

    res = vkCreateInstance(&instInfo, NULL, &m_VulkanInstance);
    if(res != VK_SUCCESS)
    {
        printf("ERROR: vkCreateInstance failed (%d)\n", res);
        return RESULT_ERROR_VULKAN;
    }

    if(g_EnableValidationLayer)
        RegisterDebugCallbacks();

    // Find physical device

    uint32_t physicalDeviceCount = 0;
    res = vkEnumeratePhysicalDevices(m_VulkanInstance, &physicalDeviceCount, nullptr);
    assert(res == VK_SUCCESS);
    if(physicalDeviceCount == 0)
    {
        printf("ERROR: No Vulkan physical devices found.\n");
        return RESULT_ERROR_VULKAN;
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    res = vkEnumeratePhysicalDevices(m_VulkanInstance, &physicalDeviceCount, physicalDevices.data());
    assert(res == VK_SUCCESS);

    if(g_PhysicalDeviceIndex >= physicalDeviceCount)
    {
        printf("ERROR: Incorrect Vulkan physical device index %u. System has %u physical devices.\n",
            g_PhysicalDeviceIndex,
            physicalDeviceCount);
        return RESULT_ERROR_VULKAN;
    }

    m_PhysicalDevice = physicalDevices[0];

    // Find queue family index

    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, nullptr);
    if(queueFamilyCount)
    {
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_PhysicalDevice, &queueFamilyCount, queueFamilies.data());
        for(uint32_t i = 0; i < queueFamilyCount; ++i)
        {
            if(queueFamilies[i].queueCount > 0 &&
                (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                m_GraphicsQueueFamilyIndex = i;
                break;
            }
        }
    }
    if(m_GraphicsQueueFamilyIndex == UINT_MAX)
    {
        printf("ERROR: Couldn't find graphics queue.\n");
        return RESULT_ERROR_VULKAN;
    }

    VkPhysicalDeviceFeatures supportedFeatures;
    vkGetPhysicalDeviceFeatures(m_PhysicalDevice, &supportedFeatures);

    // Create logical device

    const float queuePriority = 1.f;

    VkDeviceQueueCreateInfo deviceQueueCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    deviceQueueCreateInfo.queueFamilyIndex = m_GraphicsQueueFamilyIndex;
    deviceQueueCreateInfo.queueCount = 1;
    deviceQueueCreateInfo.pQueuePriorities = &queuePriority;

    // Enable something what may interact with memory/buffer/image support.
    VkPhysicalDeviceFeatures enabledFeatures;
    InitVulkanFeatures(enabledFeatures, supportedFeatures);

    // Determine list of device extensions to enable.
    std::vector<const char*> enabledDeviceExtensions;
    //enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    {
        uint32_t propertyCount = 0;
        res = vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &propertyCount, nullptr);
        assert(res == VK_SUCCESS);

        if(propertyCount)
        {
            std::vector<VkExtensionProperties> properties{propertyCount};
            res = vkEnumerateDeviceExtensionProperties(m_PhysicalDevice, nullptr, &propertyCount, properties.data());
            assert(res == VK_SUCCESS);

            for(uint32_t i = 0; i < propertyCount; ++i)
            {
                if(strcmp(properties[i].extensionName, VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME) == 0)
                {
                    enabledDeviceExtensions.push_back(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
                    VK_KHR_get_memory_requirements2_enabled = true;
                }
                else if(strcmp(properties[i].extensionName, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME) == 0)
                {
                    enabledDeviceExtensions.push_back(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
                    VK_KHR_dedicated_allocation_enabled = true;
                }
            }
        }
    }

    VkDeviceCreateInfo deviceCreateInfo = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceCreateInfo.enabledExtensionCount = (uint32_t)enabledDeviceExtensions.size();
    deviceCreateInfo.ppEnabledExtensionNames = !enabledDeviceExtensions.empty() ? enabledDeviceExtensions.data() : nullptr;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &deviceQueueCreateInfo;
    deviceCreateInfo.pEnabledFeatures = &enabledFeatures;

    res = vkCreateDevice(m_PhysicalDevice, &deviceCreateInfo, nullptr, &m_Device);
    if(res != VK_SUCCESS)
    {
        printf("ERROR: vkCreateDevice failed (%d)\n", res);
        return RESULT_ERROR_VULKAN;
    }

    // Create memory allocator

    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = m_PhysicalDevice;
    allocatorInfo.device = m_Device;

    if(VK_KHR_dedicated_allocation_enabled)
    {
        allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_DEDICATED_ALLOCATION_BIT;
    }

    res = vmaCreateAllocator(&allocatorInfo, &m_Allocator);
    if(res != VK_SUCCESS)
    {
        printf("ERROR: vmaCreateAllocator failed (%d)\n", res);
        return RESULT_ERROR_VULKAN;
    }

    return 0;
}

void Player::FinalizeVulkan()
{
    if(!m_Allocations.empty())
    {
        printf("WARNING: Allocations not destroyed: %zu.\n", m_Allocations.size());

        if(CLEANUP_LEAKED_OBJECTS)
        {
            for(const auto it : m_Allocations)
            {
                Destroy(it.second);
            }
        }

        m_Allocations.clear();
    }

    if(!m_Pools.empty())
    {
        printf("WARNING: Custom pools not destroyed: %zu.\n", m_Pools.size());

        if(CLEANUP_LEAKED_OBJECTS)
        {
            for(const auto it : m_Pools)
            {
                vmaDestroyPool(m_Allocator, it.second.pool);
            }
        }

        m_Pools.clear();
    }

    vkDeviceWaitIdle(m_Device);

    if(m_Allocator != VK_NULL_HANDLE)
    {
        vmaDestroyAllocator(m_Allocator);
        m_Allocator = nullptr;
    }

    if(m_Device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(m_Device, nullptr);
        m_Device = nullptr;
    }

    if(m_pvkDestroyDebugReportCallbackEXT && m_hCallback != VK_NULL_HANDLE)
    {
        m_pvkDestroyDebugReportCallbackEXT(m_VulkanInstance, m_hCallback, nullptr);
        m_hCallback = VK_NULL_HANDLE;
    }

    if(m_VulkanInstance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(m_VulkanInstance, NULL);
        m_VulkanInstance = VK_NULL_HANDLE;
    }
}

void Player::RegisterDebugCallbacks()
{
    m_pvkCreateDebugReportCallbackEXT =
        reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>
            (vkGetInstanceProcAddr(m_VulkanInstance, "vkCreateDebugReportCallbackEXT"));
    m_pvkDebugReportMessageEXT =
        reinterpret_cast<PFN_vkDebugReportMessageEXT>
            (vkGetInstanceProcAddr(m_VulkanInstance, "vkDebugReportMessageEXT"));
    m_pvkDestroyDebugReportCallbackEXT =
        reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>
            (vkGetInstanceProcAddr(m_VulkanInstance, "vkDestroyDebugReportCallbackEXT"));
    assert(m_pvkCreateDebugReportCallbackEXT);
    assert(m_pvkDebugReportMessageEXT);
    assert(m_pvkDestroyDebugReportCallbackEXT);

    VkDebugReportCallbackCreateInfoEXT callbackCreateInfo = { VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT };
    callbackCreateInfo.flags = //VK_DEBUG_REPORT_INFORMATION_BIT_EXT |
        VK_DEBUG_REPORT_ERROR_BIT_EXT |
        VK_DEBUG_REPORT_WARNING_BIT_EXT |
        VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT /*|
        VK_DEBUG_REPORT_DEBUG_BIT_EXT*/;
    callbackCreateInfo.pfnCallback = &MyDebugReportCallback;

    VkResult res = m_pvkCreateDebugReportCallbackEXT(m_VulkanInstance, &callbackCreateInfo, nullptr, &m_hCallback);
    assert(res == VK_SUCCESS);
}

void Player::PrintStats()
{
    if(g_Verbosity == VERBOSITY::MINIMUM)
    {
        return;
    }

    printf("Statistics:\n");
    if(m_Stats.GetAllocationCreationCount() > 0)
    {
        printf("    Total allocations created: %zu\n", m_Stats.GetAllocationCreationCount());
    }

    // Buffers
    const size_t bufferCreationCount =
        m_Stats.GetBufferCreationCount(0) +
        m_Stats.GetBufferCreationCount(1) +
        m_Stats.GetBufferCreationCount(2) +
        m_Stats.GetBufferCreationCount(3);
    if(bufferCreationCount > 0)
    {
        printf("    Total buffers created: %zu\n", bufferCreationCount);
        if(g_Verbosity == VERBOSITY::MAXIMUM)
        {
            printf("        Class 0 (indirect/vertex/index): %zu\n", m_Stats.GetBufferCreationCount(0));
            printf("        Class 1 (storage): %zu\n", m_Stats.GetBufferCreationCount(1));
            printf("        Class 2 (uniform): %zu\n", m_Stats.GetBufferCreationCount(2));
            printf("        Class 3 (other): %zu\n", m_Stats.GetBufferCreationCount(3));
        }
    }
    
    // Images
    const size_t imageCreationCount =
        m_Stats.GetImageCreationCount(0) +
        m_Stats.GetImageCreationCount(1) +
        m_Stats.GetImageCreationCount(2) +
        m_Stats.GetImageCreationCount(3) +
        m_Stats.GetLinearImageCreationCount();
    if(imageCreationCount > 0)
    {
        printf("    Total images created: %zu\n", imageCreationCount);
        if(g_Verbosity == VERBOSITY::MAXIMUM)
        {
            printf("        Class 0 (depth/stencil): %zu\n", m_Stats.GetImageCreationCount(0));
            printf("        Class 1 (attachment): %zu\n", m_Stats.GetImageCreationCount(1));
            printf("        Class 2 (sampled): %zu\n", m_Stats.GetImageCreationCount(2));
            printf("        Class 3 (other): %zu\n", m_Stats.GetImageCreationCount(3));
            if(m_Stats.GetLinearImageCreationCount() > 0)
            {
                printf("        LINEAR tiling: %zu\n", m_Stats.GetLinearImageCreationCount());
            }
        }
    }
    
    if(m_Stats.GetPoolCreationCount() > 0)
    {
        printf("    Total custom pools created: %zu\n", m_Stats.GetPoolCreationCount());
    }

    float lastTime;
    if(!m_LastLineTimeStr.empty() && StrRangeToFloat(StrRange(m_LastLineTimeStr), lastTime))
    {
        std::string origTimeStr;
        SecondsToFriendlyStr(lastTime, origTimeStr);
        printf("    Original recording time: %s\n", origTimeStr.c_str());
    }

    // Thread statistics.
    const size_t threadCount = m_Threads.size();
    if(threadCount > 1)
    {
        uint32_t threadCallCountMax = 0;
        uint32_t threadCallCountSum = 0;
        for(const auto& it : m_Threads)
        {
            threadCallCountMax = std::max(threadCallCountMax, it.second.callCount);
            threadCallCountSum += it.second.callCount;
        }
        printf("    Threads making calls to VMA: %zu\n", threadCount);
        printf("        %.2f%% calls from most active thread.\n",
            (float)threadCallCountMax * 100.f / (float)threadCallCountSum);
    }
    else
    {
        printf("    VMA used from only one thread.\n");
    }

    // Function call count
    if(g_Verbosity == VERBOSITY::MAXIMUM)
    {
        printf("    Function call count:\n");
        const size_t* const functionCallCount = m_Stats.GetFunctionCallCount();
        for(size_t i = 0; i < (size_t)VMA_FUNCTION::Count; ++i)
        {
            if(functionCallCount[i] > 0)
            {
                printf("        %s %zu\n", VMA_FUNCTION_NAMES[i], functionCallCount[i]);
            }
        }
    }
}

bool Player::ValidateFunctionParameterCount(size_t lineNumber, const CsvSplit& csvSplit, size_t expectedParamCount, bool lastUnbound)
{
    bool ok;
    if(lastUnbound)
        ok = csvSplit.GetCount() >= FIRST_PARAM_INDEX + expectedParamCount - 1;
    else
        ok = csvSplit.GetCount() == FIRST_PARAM_INDEX + expectedParamCount;

    if(!ok)
    {
        if(IssueWarning())
        {
            printf("Line %zu: Incorrect number of function parameters.\n", lineNumber);
        }
    }

    return ok;
}

bool Player::PrepareUserData(size_t lineNumber, uint32_t allocCreateFlags, const StrRange& userDataColumn, const StrRange& wholeLine, void*& outUserData)
{
    if(!g_UserDataEnabled)
    {
        outUserData = nullptr;
        return true;
    }

    // String
    if((allocCreateFlags & VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT) != 0)
    {
        const size_t len = wholeLine.end - userDataColumn.beg;
        m_UserDataTmpStr.resize(len + 1);
        memcpy(m_UserDataTmpStr.data(), userDataColumn.beg, len);
        m_UserDataTmpStr[len] = '\0';
        outUserData = m_UserDataTmpStr.data();
        return true;
    }
    // Pointer
    else
    {
        uint64_t pUserData = 0;
        if(StrRangeToPtr(userDataColumn, pUserData))
        {
            outUserData = (void*)(uintptr_t)pUserData;
            return true;
        }
    }

    if(IssueWarning())
    {
        printf("Line %zu: Invalid pUserData.\n", lineNumber);
    }
    outUserData = 0;
    return false;
}

void Player::ExecuteCreatePool(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::CreatePool);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 7, false))
    {
        VmaPoolCreateInfo poolCreateInfo = {};
        uint64_t origPtr = 0;

        if(StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX), poolCreateInfo.memoryTypeIndex) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 1), poolCreateInfo.flags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 2), poolCreateInfo.blockSize) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 3), poolCreateInfo.minBlockCount) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 4), poolCreateInfo.maxBlockCount) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 5), poolCreateInfo.frameInUseCount) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 6), origPtr))
        {
            m_Stats.RegisterCreatePool();

            Pool poolDesc = {};
            VkResult res = vmaCreatePool(m_Allocator, &poolCreateInfo, &poolDesc.pool);

            if(origPtr)
            {
                if(res == VK_SUCCESS)
                {
                    // Originally succeeded, currently succeeded.
                    // Just save pointer (done below).
                }
                else
                {
                    // Originally succeeded, currently failed.
                    // Print warning. Save null pointer.
                    if(IssueWarning())
                    {
                        printf("Line %zu: vmaCreatePool failed (%d), while originally succeeded.\n", lineNumber, res);
                    }
               }

                const auto existingIt = m_Pools.find(origPtr);
                if(existingIt != m_Pools.end())
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Pool %llX already exists.\n", lineNumber, origPtr);
                    }
                }
                m_Pools[origPtr] = poolDesc;
            }
            else
            {
                if(res == VK_SUCCESS)
                {
                    // Originally failed, currently succeeded.
                    // Print warning, destroy the pool.
                    if(IssueWarning())
                    {
                        printf("Line %zu: vmaCreatePool succeeded, originally failed.\n", lineNumber);
                    }

                    vmaDestroyPool(m_Allocator, poolDesc.pool);
                }
                else
                {
                    // Originally failed, currently failed.
                    // Print warning.
                    if(IssueWarning())
                    {
                        printf("Line %zu: vmaCreatePool failed (%d), originally also failed.\n", lineNumber, res);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaCreatePool.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteDestroyPool(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::DestroyPool);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origPtr = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            if(origPtr != 0)
            {
                const auto it = m_Pools.find(origPtr);
                if(it != m_Pools.end())
                {
                    vmaDestroyPool(m_Allocator, it->second.pool);
                    m_Pools.erase(it);
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Pool %llX not found.\n", lineNumber, origPtr);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaDestroyPool.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteSetAllocationUserData(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::SetAllocationUserData);

    if(!g_UserDataEnabled)
    {
        return;
    }

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 2, true))
    {
        uint64_t origPtr = 0;
        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            const auto it = m_Allocations.find(origPtr);
            if(it != m_Allocations.end())
            {
                void* pUserData = nullptr;
                if(csvSplit.GetCount() > FIRST_PARAM_INDEX + 1)
                {
                    PrepareUserData(
                        lineNumber,
                        it->second.allocationFlags,
                        csvSplit.GetRange(FIRST_PARAM_INDEX + 1),
                        csvSplit.GetLine(),
                        pUserData);
                }

                vmaSetAllocationUserData(m_Allocator, it->second.allocation, pUserData);
            }
            else
            {
                if(IssueWarning())
                {
                    printf("Line %zu: Allocation %llX not found.\n", lineNumber, origPtr);
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaSetAllocationUserData.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteCreateBuffer(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::CreateBuffer);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 12, true))
    {
        VkBufferCreateInfo bufCreateInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VmaAllocationCreateInfo allocCreateInfo = {};
        uint64_t origPool = 0;
        uint64_t origPtr = 0;

        if(StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX), bufCreateInfo.flags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 1), bufCreateInfo.size) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 2), bufCreateInfo.usage) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 3), (uint32_t&)bufCreateInfo.sharingMode) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 4), allocCreateInfo.flags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 5), (uint32_t&)allocCreateInfo.usage) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 6), allocCreateInfo.requiredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 7), allocCreateInfo.preferredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 8), allocCreateInfo.memoryTypeBits) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 9), origPool) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 10), origPtr))
        {
            FindPool(lineNumber, origPool, allocCreateInfo.pool);

            if(csvSplit.GetCount() > FIRST_PARAM_INDEX + 11)
            {
                PrepareUserData(
                    lineNumber,
                    allocCreateInfo.flags,
                    csvSplit.GetRange(FIRST_PARAM_INDEX + 11),
                    csvSplit.GetLine(),
                    allocCreateInfo.pUserData);
            }

            m_Stats.RegisterCreateBuffer(bufCreateInfo.usage);

            Allocation allocDesc = { };
            allocDesc.allocationFlags = allocCreateInfo.flags;
            VkResult res = vmaCreateBuffer(m_Allocator, &bufCreateInfo, &allocCreateInfo, &allocDesc.buffer, &allocDesc.allocation, nullptr);
            AddAllocation(lineNumber, origPtr, res, "vmaCreateBuffer", std::move(allocDesc));
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaCreateBuffer.\n", lineNumber);
            }
        }
    }
}

void Player::DestroyAllocation(size_t lineNumber, const CsvSplit& csvSplit)
{
    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origAllocPtr = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origAllocPtr))
        {
            if(origAllocPtr != 0)
            {
                const auto it = m_Allocations.find(origAllocPtr);
                if(it != m_Allocations.end())
                {
                    Destroy(it->second);
                    m_Allocations.erase(it);
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Allocation %llX not found.\n", lineNumber, origAllocPtr);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaDestroyBuffer.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteCreateImage(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::CreateImage);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 21, true))
    {
        VkImageCreateInfo imageCreateInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        VmaAllocationCreateInfo allocCreateInfo = {};
        uint64_t origPool = 0;
        uint64_t origPtr = 0;

        if(StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX), imageCreateInfo.flags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 1), (uint32_t&)imageCreateInfo.imageType) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 2), (uint32_t&)imageCreateInfo.format) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 3), imageCreateInfo.extent.width) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 4), imageCreateInfo.extent.height) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 5), imageCreateInfo.extent.depth) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 6), imageCreateInfo.mipLevels) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 7), imageCreateInfo.arrayLayers) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 8), (uint32_t&)imageCreateInfo.samples) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 9), (uint32_t&)imageCreateInfo.tiling) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 10), imageCreateInfo.usage) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 11), (uint32_t&)imageCreateInfo.sharingMode) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 12), (uint32_t&)imageCreateInfo.initialLayout) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 13), allocCreateInfo.flags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 14), (uint32_t&)allocCreateInfo.usage) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 15), allocCreateInfo.requiredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 16), allocCreateInfo.preferredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 17), allocCreateInfo.memoryTypeBits) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 18), origPool) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 19), origPtr))
        {
            FindPool(lineNumber, origPool, allocCreateInfo.pool);

            if(csvSplit.GetCount() > FIRST_PARAM_INDEX + 20)
            {
                PrepareUserData(
                    lineNumber,
                    allocCreateInfo.flags,
                    csvSplit.GetRange(FIRST_PARAM_INDEX + 20),
                    csvSplit.GetLine(),
                    allocCreateInfo.pUserData);
            }

            m_Stats.RegisterCreateImage(imageCreateInfo.usage, imageCreateInfo.tiling);

            Allocation allocDesc = {};
            allocDesc.allocationFlags = allocCreateInfo.flags;
            VkResult res = vmaCreateImage(m_Allocator, &imageCreateInfo, &allocCreateInfo, &allocDesc.image, &allocDesc.allocation, nullptr);
            AddAllocation(lineNumber, origPtr, res, "vmaCreateImage", std::move(allocDesc));
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaCreateImage.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteCreateLostAllocation(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::CreateLostAllocation);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origPtr = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            Allocation allocDesc = {};
            vmaCreateLostAllocation(m_Allocator, &allocDesc.allocation);
            m_Stats.RegisterCreateAllocation();

            AddAllocation(lineNumber, origPtr, VK_SUCCESS, "vmaCreateLostAllocation", std::move(allocDesc));
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaCreateLostAllocation.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteAllocateMemory(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::AllocateMemory);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 11, true))
    {
        VkMemoryRequirements memReq = {};
        VmaAllocationCreateInfo allocCreateInfo = {};
        uint64_t origPool = 0;
        uint64_t origPtr = 0;

        if(StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX), memReq.size) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 1), memReq.alignment) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 2), memReq.memoryTypeBits) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 3), allocCreateInfo.flags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 4), (uint32_t&)allocCreateInfo.usage) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 5), allocCreateInfo.requiredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 6), allocCreateInfo.preferredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 7), allocCreateInfo.memoryTypeBits) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 8), origPool) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 9), origPtr))
        {
            FindPool(lineNumber, origPool, allocCreateInfo.pool);

            if(csvSplit.GetCount() > FIRST_PARAM_INDEX + 10)
            {
                PrepareUserData(
                    lineNumber,
                    allocCreateInfo.flags,
                    csvSplit.GetRange(FIRST_PARAM_INDEX + 10),
                    csvSplit.GetLine(),
                    allocCreateInfo.pUserData);
            }

            m_Stats.RegisterCreateAllocation();

            Allocation allocDesc = {};
            allocDesc.allocationFlags = allocCreateInfo.flags;
            VkResult res = vmaAllocateMemory(m_Allocator, &memReq, &allocCreateInfo, &allocDesc.allocation, nullptr);
            AddAllocation(lineNumber, origPtr, res, "vmaAllocateMemory", std::move(allocDesc));
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaAllocateMemory.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteAllocateMemoryForBufferOrImage(size_t lineNumber, const CsvSplit& csvSplit, OBJECT_TYPE objType)
{
    switch(objType)
    {
    case OBJECT_TYPE::BUFFER:
        m_Stats.RegisterFunctionCall(VMA_FUNCTION::AllocateMemoryForBuffer);
        break;
    case OBJECT_TYPE::IMAGE:
        m_Stats.RegisterFunctionCall(VMA_FUNCTION::AllocateMemoryForImage);
        break;
    default: assert(0);
    }

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 13, true))
    {
        VkMemoryRequirements memReq = {};
        VmaAllocationCreateInfo allocCreateInfo = {};
        bool requiresDedicatedAllocation = false;
        bool prefersDedicatedAllocation = false;
        uint64_t origPool = 0;
        uint64_t origPtr = 0;

        if(StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX), memReq.size) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 1), memReq.alignment) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 2), memReq.memoryTypeBits) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 3), allocCreateInfo.flags) &&
            StrRangeToBool(csvSplit.GetRange(FIRST_PARAM_INDEX + 4), requiresDedicatedAllocation) &&
            StrRangeToBool(csvSplit.GetRange(FIRST_PARAM_INDEX + 5), prefersDedicatedAllocation) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 6), (uint32_t&)allocCreateInfo.usage) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 7), allocCreateInfo.requiredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 8), allocCreateInfo.preferredFlags) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 9), allocCreateInfo.memoryTypeBits) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 10), origPool) &&
            StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX + 11), origPtr))
        {
            FindPool(lineNumber, origPool, allocCreateInfo.pool);

            if(csvSplit.GetCount() > FIRST_PARAM_INDEX + 12)
            {
                PrepareUserData(
                    lineNumber,
                    allocCreateInfo.flags,
                    csvSplit.GetRange(FIRST_PARAM_INDEX + 12),
                    csvSplit.GetLine(),
                    allocCreateInfo.pUserData);
            }

            if(requiresDedicatedAllocation || prefersDedicatedAllocation)
            {
                allocCreateInfo.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            }

            if(!m_AllocateForBufferImageWarningIssued)
            {
                if(IssueWarning())
                {
                    printf("Line %zu: vmaAllocateMemoryForBuffer or vmaAllocateMemoryForImage cannot be replayed accurately. Using vmaCreateAllocation instead.\n", lineNumber);
                }
                m_AllocateForBufferImageWarningIssued = true;
            }

            m_Stats.RegisterCreateAllocation();

            Allocation allocDesc = {};
            allocDesc.allocationFlags = allocCreateInfo.flags;
            VkResult res = vmaAllocateMemory(m_Allocator, &memReq, &allocCreateInfo, &allocDesc.allocation, nullptr);
            AddAllocation(lineNumber, origPtr, res, "vmaAllocateMemory (called as vmaAllocateMemoryForBuffer or vmaAllocateMemoryForImage)", std::move(allocDesc));
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaAllocateMemoryForBuffer or vmaAllocateMemoryForImage.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteMapMemory(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::MapMemory);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origPtr = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            if(origPtr != 0)
            {
                const auto it = m_Allocations.find(origPtr);
                if(it != m_Allocations.end())
                {
                    if(it->second.allocation)
                    {
                        void* pData;
                        VkResult res = vmaMapMemory(m_Allocator, it->second.allocation, &pData);
                        if(res != VK_SUCCESS)
                        {
                            printf("Line %zu: vmaMapMemory failed (%d)\n", lineNumber, res);
                        }
                    }
                    else
                    {
                        if(IssueWarning())
                        {
                            printf("Line %zu: Cannot call vmaMapMemory - allocation is null.\n", lineNumber);
                        }
                    }
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Allocation %llX not found.\n", lineNumber, origPtr);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaMapMemory.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteUnmapMemory(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::UnmapMemory);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origPtr = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            if(origPtr != 0)
            {
                const auto it = m_Allocations.find(origPtr);
                if(it != m_Allocations.end())
                {
                    if(it->second.allocation)
                    {
                        vmaUnmapMemory(m_Allocator, it->second.allocation);
                    }
                    else
                    {
                        if(IssueWarning())
                        {
                            printf("Line %zu: Cannot call vmaUnmapMemory - allocation is null.\n", lineNumber);
                        }
                    }
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Allocation %llX not found.\n", lineNumber, origPtr);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaMapMemory.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteFlushAllocation(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::FlushAllocation);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 3, false))
    {
        uint64_t origPtr = 0;
        uint64_t offset = 0;
        uint64_t size = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 1), offset) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 2), size))
        {
            if(origPtr != 0)
            {
                const auto it = m_Allocations.find(origPtr);
                if(it != m_Allocations.end())
                {
                    if(it->second.allocation)
                    {
                        vmaFlushAllocation(m_Allocator, it->second.allocation, offset, size);
                    }
                    else
                    {
                        if(IssueWarning())
                        {
                            printf("Line %zu: Cannot call vmaFlushAllocation - allocation is null.\n", lineNumber);
                        }
                    }
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Allocation %llX not found.\n", lineNumber, origPtr);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaFlushAllocation.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteInvalidateAllocation(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::InvalidateAllocation);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 3, false))
    {
        uint64_t origPtr = 0;
        uint64_t offset = 0;
        uint64_t size = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 1), offset) &&
            StrRangeToUint(csvSplit.GetRange(FIRST_PARAM_INDEX + 2), size))
        {
            if(origPtr != 0)
            {
                const auto it = m_Allocations.find(origPtr);
                if(it != m_Allocations.end())
                {
                    if(it->second.allocation)
                    {
                        vmaInvalidateAllocation(m_Allocator, it->second.allocation, offset, size);
                    }
                    else
                    {
                        if(IssueWarning())
                        {
                            printf("Line %zu: Cannot call vmaInvalidateAllocation - allocation is null.\n", lineNumber);
                        }
                    }
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Allocation %llX not found.\n", lineNumber, origPtr);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaInvalidateAllocation.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteTouchAllocation(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::TouchAllocation);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origPtr = 0;
        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            const auto it = m_Allocations.find(origPtr);
            if(it != m_Allocations.end())
            {
                if(it->second.allocation)
                {
                    vmaTouchAllocation(m_Allocator, it->second.allocation);
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Cannot call vmaTouchAllocation - allocation is null.\n", lineNumber);
                    }
                }
            }
            else
            {
                if(IssueWarning())
                {
                    printf("Line %zu: Allocation %llX not found.\n", lineNumber, origPtr);
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaTouchAllocation.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteGetAllocationInfo(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::GetAllocationInfo);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origPtr = 0;
        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            const auto it = m_Allocations.find(origPtr);
            if(it != m_Allocations.end())
            {
                if(it->second.allocation)
                {
                    VmaAllocationInfo allocInfo;
                    vmaGetAllocationInfo(m_Allocator, it->second.allocation, &allocInfo);
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Cannot call vmaGetAllocationInfo - allocation is null.\n", lineNumber);
                    }
                }
            }
            else
            {
                if(IssueWarning())
                {
                    printf("Line %zu: Allocation %llX not found.\n", lineNumber, origPtr);
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaGetAllocationInfo.\n", lineNumber);
            }
        }
    }
}

void Player::ExecuteMakePoolAllocationsLost(size_t lineNumber, const CsvSplit& csvSplit)
{
    m_Stats.RegisterFunctionCall(VMA_FUNCTION::MakePoolAllocationsLost);

    if(ValidateFunctionParameterCount(lineNumber, csvSplit, 1, false))
    {
        uint64_t origPtr = 0;

        if(StrRangeToPtr(csvSplit.GetRange(FIRST_PARAM_INDEX), origPtr))
        {
            if(origPtr != 0)
            {
                const auto it = m_Pools.find(origPtr);
                if(it != m_Pools.end())
                {
                    vmaMakePoolAllocationsLost(m_Allocator, it->second.pool, nullptr);
                }
                else
                {
                    if(IssueWarning())
                    {
                        printf("Line %zu: Pool %llX not found.\n", lineNumber, origPtr);
                    }
                }
            }
        }
        else
        {
            if(IssueWarning())
            {
                printf("Line %zu: Invalid parameters for vmaMakePoolAllocationsLost.\n", lineNumber);
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Main functions

static void PrintCommandLineSyntax()
{
    printf(
        "Command line syntax:\n"
        "    VmaReplay [Options] <SrcFile.csv>\n"
        "Available options:\n"
        "    -v <Number> - Verbosity level:\n"
        "        0 - Minimum verbosity. Prints only warnings and errors.\n"
        "        1 - Default verbosity. Prints important messages and statistics.\n"
        "        2 - Maximum verbosity. Prints a lot of information.\n"
        "    -i <Number> - Repeat playback given number of times (iterations)\n"
        "        Default is 1. Vulkan is reinitialized with every iteration.\n"
        "    --Lines <Ranges> - Replay only limited set of lines from file\n"
        "        Ranges is comma-separated list of ranges, e.g. \"-10,15,18-25,31-\".\n"
        "    --PhysicalDevice <Index> - Choice of Vulkan physical device. Default: 0.\n"
        "    --UserData <Value> - 0 to disable or 1 to enable setting pUserData during playback.\n"
        "        Default is 1. Affects both creation of buffers and images, as well as calls to vmaSetAllocationUserData.\n"
    );
}

static int ProcessFile(size_t iterationIndex, const char* data, size_t numBytes, duration& outDuration)
{
    outDuration = duration::max();

    const bool useLineRanges = !g_LineRanges.IsEmpty();

    LineSplit lineSplit(data, numBytes);
    StrRange line;

    if(!lineSplit.GetNextLine(line) ||
        !StrRangeEq(line, "Vulkan Memory Allocator,Calls recording"))
    {
        printf("ERROR: Incorrect file format.\n");
        return RESULT_ERROR_FORMAT;
    }

    if(!lineSplit.GetNextLine(line) || !ParseFileVersion(line) || !ValidateFileVersion())
    {
        printf("ERROR: Incorrect file format version.\n");
        return RESULT_ERROR_FORMAT;
    }

    if(g_Verbosity == VERBOSITY::MAXIMUM)
    {
        printf("Format version: %u,%u\n", g_FileVersion >> 16, g_FileVersion & 0xFFFF);
    }

    Player player;
    int result = player.Init();
    size_t executedLineCount = 0;
    if(result == 0)
    {
        if(g_Verbosity > VERBOSITY::MINIMUM)
        {
            if(useLineRanges)
            {
                printf("Playing #%zu (limited range of lines)...\n", iterationIndex + 1);
            }
            else
            {
                printf("Playing #%zu...\n", iterationIndex + 1);
            }
        }

        const time_point timeBeg = std::chrono::high_resolution_clock::now();

        while(lineSplit.GetNextLine(line))
        {
            bool execute = true;
            if(useLineRanges)
            {
                execute = g_LineRanges.Includes(lineSplit.GetNextLineIndex());
            }

            if(execute)
            {
                player.ExecuteLine(lineSplit.GetNextLineIndex(), line);
                ++executedLineCount;
            }
        }

        const duration playDuration = std::chrono::high_resolution_clock::now() - timeBeg;
        outDuration = playDuration;

        // End stats.
        if(g_Verbosity > VERBOSITY::MINIMUM)
        {
            std::string playDurationStr;
            SecondsToFriendlyStr(ToFloatSeconds(playDuration), playDurationStr);

            printf("Done.\n");
            printf("Playback took: %s\n", playDurationStr.c_str());
        }
        if(g_Verbosity == VERBOSITY::MAXIMUM)
        {
            printf("File lines: %zu\n", lineSplit.GetNextLineIndex());
            printf("Executed %zu file lines\n", executedLineCount);
        }

        player.PrintStats();
    }

    return result;
}

static int ProcessFile()
{
    if(g_Verbosity > VERBOSITY::MINIMUM)
    {
        printf("Loading file \"%s\"...\n", g_FilePath.c_str());
    }
    int result = 0;

    FILE* file = nullptr;
    const errno_t err = fopen_s(&file, g_FilePath.c_str(), "rb");
    if(err == 0)
    {
        _fseeki64(file, 0, SEEK_END);
        const size_t fileSize = (size_t)_ftelli64(file);
        _fseeki64(file, 0, SEEK_SET);

        if(fileSize > 0)
        {
            std::vector<char> fileContents(fileSize);
            fread(fileContents.data(), 1, fileSize, file);

            // Begin stats.
            if(g_Verbosity == VERBOSITY::MAXIMUM)
            {
                printf("File size: %zu B\n", fileSize);
            }

            duration durationSum = duration::zero();
            for(size_t i = 0; i < g_IterationCount; ++i)
            {
                duration currDuration;
                ProcessFile(i, fileContents.data(), fileContents.size(), currDuration);
                durationSum += currDuration;
            }

            if(g_IterationCount > 1)
            {
                std::string playDurationStr;
                SecondsToFriendlyStr(ToFloatSeconds(durationSum / g_IterationCount), playDurationStr);
                printf("Average playback time from %zu iterations: %s\n", g_IterationCount, playDurationStr.c_str());
            }
        }
        else
        {
            printf("ERROR: Source file is empty.\n");
            result = RESULT_ERROR_SOURCE_FILE;
        }

        fclose(file);
    }
    else
    {
        printf("ERROR: Couldn't open file (%i).\n", err);
        result = RESULT_ERROR_SOURCE_FILE;
    }

    return result;
}

static int main2(int argc, char** argv)
{
    CmdLineParser cmdLineParser(argc, argv);

    cmdLineParser.RegisterOpt(CMD_LINE_OPT_VERBOSITY, 'v', true);
    cmdLineParser.RegisterOpt(CMD_LINE_OPT_ITERATIONS, 'i', true);
    cmdLineParser.RegisterOpt(CMD_LINE_OPT_LINES, "Lines", true);
    cmdLineParser.RegisterOpt(CMD_LINE_OPT_PHYSICAL_DEVICE, "PhysicalDevice", true);
    cmdLineParser.RegisterOpt(CMD_LINE_OPT_USER_DATA, "UserData", true);

    CmdLineParser::RESULT res;
    while((res = cmdLineParser.ReadNext()) != CmdLineParser::RESULT_END)
    {
        switch(res)
        {
        case CmdLineParser::RESULT_OPT:
            switch(cmdLineParser.GetOptId())
            {
            case CMD_LINE_OPT_VERBOSITY:
                {
                    uint32_t verbosityVal = UINT32_MAX;
                    if(StrRangeToUint(StrRange(cmdLineParser.GetParameter()), verbosityVal) &&
                        verbosityVal < (uint32_t)VERBOSITY::COUNT)
                    {
                        g_Verbosity = (VERBOSITY)verbosityVal;
                    }
                    else
                    {
                        PrintCommandLineSyntax();
                        return RESULT_ERROR_COMMAND_LINE;
                    }
                }
                break;
            case CMD_LINE_OPT_ITERATIONS:
                if(!StrRangeToUint(StrRange(cmdLineParser.GetParameter()), g_IterationCount))
                {
                    PrintCommandLineSyntax();
                    return RESULT_ERROR_COMMAND_LINE;
                }
                break;
            case CMD_LINE_OPT_LINES:
                if(!g_LineRanges.Parse(StrRange(cmdLineParser.GetParameter())))
                {
                    PrintCommandLineSyntax();
                    return RESULT_ERROR_COMMAND_LINE;
                }
                break;
            case CMD_LINE_OPT_PHYSICAL_DEVICE:
                if(!StrRangeToUint(StrRange(cmdLineParser.GetParameter()), g_PhysicalDeviceIndex))
                {
                    PrintCommandLineSyntax();
                    return RESULT_ERROR_COMMAND_LINE;
                }
                break;
            case CMD_LINE_OPT_USER_DATA:
                if(!StrRangeToBool(StrRange(cmdLineParser.GetParameter()), g_UserDataEnabled))
                {
                    PrintCommandLineSyntax();
                    return RESULT_ERROR_COMMAND_LINE;
                }
                break;
            default:
                assert(0);
            }
            break;
        case CmdLineParser::RESULT_PARAMETER:
            if(g_FilePath.empty())
            {
                g_FilePath = cmdLineParser.GetParameter();
            }
            else
            {
                PrintCommandLineSyntax();
                return RESULT_ERROR_COMMAND_LINE;
            }
            break;
        case CmdLineParser::RESULT_ERROR:
            PrintCommandLineSyntax();
            return RESULT_ERROR_COMMAND_LINE;
            break;
        default:
            assert(0);
        }
    }

    if(g_FilePath.empty())
    {
        PrintCommandLineSyntax();
        return RESULT_ERROR_COMMAND_LINE;
    }

    return ProcessFile();
}

int main(int argc, char** argv)
{
    try
    {
        return main2(argc, argv);
    }
    catch(const std::exception& e)
    {
        printf("ERROR: %s\n", e.what());
        return RESULT_EXCEPTION;
    }
    catch(...)
    {
        printf("UNKNOWN ERROR\n");
        return RESULT_EXCEPTION;
    }
}
