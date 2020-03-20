// Copyright 2017 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dawn_native/BindGroupLayout.h"

#include "common/BitSetIterator.h"
#include "common/HashUtils.h"
#include "dawn_native/Device.h"
#include "dawn_native/ValidationUtils_autogen.h"

#include <functional>
#include <set>

namespace dawn_native {

    MaybeError ValidateBindingTypeWithShaderStageVisibility(
        wgpu::BindingType bindingType,
        wgpu::ShaderStage shaderStageVisibility) {
        // TODO(jiawei.shao@intel.com): support read-write storage textures.
        switch (bindingType) {
            case wgpu::BindingType::StorageBuffer: {
                if ((shaderStageVisibility & wgpu::ShaderStage::Vertex) != 0) {
                    return DAWN_VALIDATION_ERROR(
                        "storage buffer binding is not supported in vertex shader");
                }
            } break;

            case wgpu::BindingType::WriteonlyStorageTexture: {
                if ((shaderStageVisibility &
                     (wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment)) != 0) {
                    return DAWN_VALIDATION_ERROR(
                        "write-only storage texture binding is only supported in compute shader");
                }
            } break;

            case wgpu::BindingType::StorageTexture: {
                return DAWN_VALIDATION_ERROR("Read-write storage texture binding is not supported");
            } break;

            case wgpu::BindingType::UniformBuffer:
            case wgpu::BindingType::ReadonlyStorageBuffer:
            case wgpu::BindingType::Sampler:
            case wgpu::BindingType::SampledTexture:
            case wgpu::BindingType::ReadonlyStorageTexture:
                break;
        }

        return {};
    }

    MaybeError ValidateStorageTextureFormat(DeviceBase* device,
                                            wgpu::BindingType bindingType,
                                            wgpu::TextureFormat storageTextureFormat) {
        switch (bindingType) {
            case wgpu::BindingType::ReadonlyStorageTexture:
            case wgpu::BindingType::WriteonlyStorageTexture: {
                DAWN_TRY(ValidateTextureFormat(storageTextureFormat));

                const Format& format = device->GetValidInternalFormat(storageTextureFormat);
                if (!format.supportsStorageUsage) {
                    return DAWN_VALIDATION_ERROR("The storage texture format is not supported");
                }
            } break;

            case wgpu::BindingType::StorageBuffer:
            case wgpu::BindingType::UniformBuffer:
            case wgpu::BindingType::ReadonlyStorageBuffer:
            case wgpu::BindingType::Sampler:
            case wgpu::BindingType::SampledTexture:
                break;
            default:
                UNREACHABLE();
                break;
        }

        return {};
    }

    MaybeError ValidateBindGroupLayoutDescriptor(DeviceBase* device,
                                                 const BindGroupLayoutDescriptor* descriptor) {
        if (descriptor->nextInChain != nullptr) {
            return DAWN_VALIDATION_ERROR("nextInChain must be nullptr");
        }

        std::set<BindingNumber> bindingsSet;
        uint32_t dynamicUniformBufferCount = 0;
        uint32_t dynamicStorageBufferCount = 0;
        for (BindingIndex i = 0; i < descriptor->bindingCount; ++i) {
            const BindGroupLayoutBinding& binding = descriptor->bindings[i];
            BindingNumber bindingNumber = BindingNumber(binding.binding);

            DAWN_TRY(ValidateShaderStage(binding.visibility));
            DAWN_TRY(ValidateBindingType(binding.type));
            DAWN_TRY(ValidateTextureComponentType(binding.textureComponentType));

            if (binding.textureDimension != wgpu::TextureViewDimension::Undefined) {
                DAWN_TRY(ValidateTextureViewDimension(binding.textureDimension));
            }

            if (bindingsSet.count(bindingNumber) != 0) {
                return DAWN_VALIDATION_ERROR("some binding index was specified more than once");
            }

            DAWN_TRY(
                ValidateBindingTypeWithShaderStageVisibility(binding.type, binding.visibility));

            DAWN_TRY(
                ValidateStorageTextureFormat(device, binding.type, binding.storageTextureFormat));

            switch (binding.type) {
                case wgpu::BindingType::UniformBuffer:
                    if (binding.hasDynamicOffset) {
                        ++dynamicUniformBufferCount;
                    }
                    break;
                case wgpu::BindingType::StorageBuffer:
                case wgpu::BindingType::ReadonlyStorageBuffer:
                    if (binding.hasDynamicOffset) {
                        ++dynamicStorageBufferCount;
                    }
                    break;
                case wgpu::BindingType::SampledTexture:
                case wgpu::BindingType::Sampler:
                case wgpu::BindingType::ReadonlyStorageTexture:
                case wgpu::BindingType::WriteonlyStorageTexture:
                    if (binding.hasDynamicOffset) {
                        return DAWN_VALIDATION_ERROR("Samplers and textures cannot be dynamic");
                    }
                    break;
                case wgpu::BindingType::StorageTexture:
                    return DAWN_VALIDATION_ERROR("storage textures aren't supported (yet)");
            }

            if (binding.multisampled) {
                return DAWN_VALIDATION_ERROR(
                    "BindGroupLayoutBinding::multisampled must be false (for now)");
            }

            bindingsSet.insert(bindingNumber);
        }

        if (dynamicUniformBufferCount > kMaxDynamicUniformBufferCount) {
            return DAWN_VALIDATION_ERROR(
                "The number of dynamic uniform buffer exceeds the maximum value");
        }

        if (dynamicStorageBufferCount > kMaxDynamicStorageBufferCount) {
            return DAWN_VALIDATION_ERROR(
                "The number of dynamic storage buffer exceeds the maximum value");
        }

        return {};
    }  // namespace dawn_native

    namespace {
        size_t HashBindingInfo(const BindGroupLayoutBase::LayoutBindingInfo& info) {
            size_t hash = 0;
            HashCombine(&hash, info.hasDynamicOffset, info.multisampled);

            for (BindingIndex i = 0; i < info.bindingCount; ++i) {
                HashCombine(&hash, info.visibilities[i], info.types[i],
                            info.textureComponentTypes[i], info.textureDimensions[i]);
            }

            return hash;
        }

        bool operator==(const BindGroupLayoutBase::LayoutBindingInfo& a,
                        const BindGroupLayoutBase::LayoutBindingInfo& b) {
            if (a.bindingCount != b.bindingCount || a.hasDynamicOffset != b.hasDynamicOffset ||
                a.multisampled != b.multisampled) {
                return false;
            }

            for (BindingIndex i = 0; i < a.bindingCount; ++i) {
                if ((a.visibilities[i] != b.visibilities[i]) || (a.types[i] != b.types[i]) ||
                    (a.textureComponentTypes[i] != b.textureComponentTypes[i]) ||
                    (a.textureDimensions[i] != b.textureDimensions[i])) {
                    return false;
                }
            }

            return true;
        }

        bool SortBindingsCompare(const BindGroupLayoutBinding& a, const BindGroupLayoutBinding& b) {
            if (a.hasDynamicOffset != b.hasDynamicOffset) {
                // Buffers with dynamic offsets should come before those without.
                // This makes it easy to iterate over the dynamic buffer bindings
                // [0, dynamicBufferCount) during validation.
                return a.hasDynamicOffset > b.hasDynamicOffset;
            }
            if (a.type != b.type) {
                // Buffers have smaller type enums. They should be placed first.
                return a.type < b.type;
            }
            if (a.visibility != b.visibility) {
                return a.visibility < b.visibility;
            }
            if (a.multisampled != b.multisampled) {
                return a.multisampled < b.multisampled;
            }
            if (a.textureDimension != b.textureDimension) {
                return a.textureDimension < b.textureDimension;
            }
            if (a.textureComponentType != b.textureComponentType) {
                return a.textureComponentType < b.textureComponentType;
            }
            if (a.storageTextureFormat != b.storageTextureFormat) {
                return a.storageTextureFormat < b.storageTextureFormat;
            }
            return false;
        }

        // This is a utility function to help ASSERT that the BGL-binding comparator places buffers
        // first.
        bool CheckBufferBindingsFirst(const BindGroupLayoutBinding* bindings, size_t count) {
            ASSERT(count <= kMaxBindingsPerGroup);

            BindingIndex lastBufferIndex = 0;
            BindingIndex firstNonBufferIndex = std::numeric_limits<BindingIndex>::max();
            for (BindingIndex i = 0; i < count; ++i) {
                switch (bindings[i].type) {
                    case wgpu::BindingType::UniformBuffer:
                    case wgpu::BindingType::StorageBuffer:
                    case wgpu::BindingType::ReadonlyStorageBuffer:
                        lastBufferIndex = std::max(i, lastBufferIndex);
                        break;
                    case wgpu::BindingType::SampledTexture:
                    case wgpu::BindingType::Sampler:
                    case wgpu::BindingType::StorageTexture:
                    case wgpu::BindingType::ReadonlyStorageTexture:
                    case wgpu::BindingType::WriteonlyStorageTexture:
                        firstNonBufferIndex = std::min(i, firstNonBufferIndex);
                        break;
                    default:
                        UNREACHABLE();
                        break;
                }
            }

            // If there are no buffers, then |lastBufferIndex| is initialized to 0 and
            // |firstNonBufferIndex| gets set to 0.
            return firstNonBufferIndex >= lastBufferIndex;
        }

    }  // namespace

    // BindGroupLayoutBase

    BindGroupLayoutBase::BindGroupLayoutBase(DeviceBase* device,
                                             const BindGroupLayoutDescriptor* descriptor)
        : CachedObject(device) {
        mBindingInfo.bindingCount = descriptor->bindingCount;

        std::vector<BindGroupLayoutBinding> sortedBindings(
            descriptor->bindings, descriptor->bindings + descriptor->bindingCount);

        std::sort(sortedBindings.begin(), sortedBindings.end(), SortBindingsCompare);
        ASSERT(CheckBufferBindingsFirst(sortedBindings.data(), sortedBindings.size()));

        for (BindingIndex i = 0; i < mBindingInfo.bindingCount; ++i) {
            const BindGroupLayoutBinding& binding = sortedBindings[i];
            mBindingInfo.types[i] = binding.type;
            mBindingInfo.visibilities[i] = binding.visibility;
            mBindingInfo.textureComponentTypes[i] = binding.textureComponentType;

            switch (binding.type) {
                case wgpu::BindingType::UniformBuffer:
                case wgpu::BindingType::StorageBuffer:
                case wgpu::BindingType::ReadonlyStorageBuffer:
                    // Buffers must be contiguously packed at the start of the binding info.
                    ASSERT(mBufferCount == i);
                    ++mBufferCount;
                    break;
                default:
                    break;
            }

            if (binding.textureDimension == wgpu::TextureViewDimension::Undefined) {
                mBindingInfo.textureDimensions[i] = wgpu::TextureViewDimension::e2D;
            } else {
                mBindingInfo.textureDimensions[i] = binding.textureDimension;
            }

            if (binding.hasDynamicOffset) {
                mBindingInfo.hasDynamicOffset.set(i);
                switch (binding.type) {
                    case wgpu::BindingType::UniformBuffer:
                        ++mDynamicUniformBufferCount;
                        break;
                    case wgpu::BindingType::StorageBuffer:
                    case wgpu::BindingType::ReadonlyStorageBuffer:
                        ++mDynamicStorageBufferCount;
                        break;
                    case wgpu::BindingType::SampledTexture:
                    case wgpu::BindingType::Sampler:
                    case wgpu::BindingType::StorageTexture:
                    case wgpu::BindingType::ReadonlyStorageTexture:
                    case wgpu::BindingType::WriteonlyStorageTexture:
                        UNREACHABLE();
                        break;
                }
            }
            mBindingInfo.multisampled.set(i, binding.multisampled);

            const auto& it = mBindingMap.emplace(BindingNumber(binding.binding), i);
            ASSERT(it.second);
        }
    }

    BindGroupLayoutBase::BindGroupLayoutBase(DeviceBase* device, ObjectBase::ErrorTag tag)
        : CachedObject(device, tag) {
    }

    BindGroupLayoutBase::~BindGroupLayoutBase() {
        // Do not uncache the actual cached object if we are a blueprint
        if (IsCachedReference()) {
            GetDevice()->UncacheBindGroupLayout(this);
        }
    }

    // static
    BindGroupLayoutBase* BindGroupLayoutBase::MakeError(DeviceBase* device) {
        return new BindGroupLayoutBase(device, ObjectBase::kError);
    }

    const BindGroupLayoutBase::LayoutBindingInfo& BindGroupLayoutBase::GetBindingInfo() const {
        ASSERT(!IsError());
        return mBindingInfo;
    }

    const BindGroupLayoutBase::BindingMap& BindGroupLayoutBase::GetBindingMap() const {
        ASSERT(!IsError());
        return mBindingMap;
    }

    BindingIndex BindGroupLayoutBase::GetBindingIndex(BindingNumber bindingNumber) const {
        ASSERT(!IsError());
        const auto& it = mBindingMap.find(bindingNumber);
        ASSERT(it != mBindingMap.end());
        return it->second;
    }

    size_t BindGroupLayoutBase::HashFunc::operator()(const BindGroupLayoutBase* bgl) const {
        size_t hash = HashBindingInfo(bgl->mBindingInfo);
        // std::map is sorted by key, so two BGLs constructed in different orders
        // will still hash the same.
        for (const auto& it : bgl->mBindingMap) {
            HashCombine(&hash, it.first, it.second);
        }
        return hash;
    }

    bool BindGroupLayoutBase::EqualityFunc::operator()(const BindGroupLayoutBase* a,
                                                       const BindGroupLayoutBase* b) const {
        return a->mBindingInfo == b->mBindingInfo && a->mBindingMap == b->mBindingMap;
    }

    BindingIndex BindGroupLayoutBase::GetBindingCount() const {
        return mBindingInfo.bindingCount;
    }

    BindingIndex BindGroupLayoutBase::GetDynamicBufferCount() const {
        return mDynamicStorageBufferCount + mDynamicUniformBufferCount;
    }

    uint32_t BindGroupLayoutBase::GetDynamicUniformBufferCount() const {
        return mDynamicUniformBufferCount;
    }

    uint32_t BindGroupLayoutBase::GetDynamicStorageBufferCount() const {
        return mDynamicStorageBufferCount;
    }

    size_t BindGroupLayoutBase::GetBindingDataSize() const {
        // | ------ buffer-specific ----------| ------------ object pointers -------------|
        // | --- offsets + sizes -------------| --------------- Ref<ObjectBase> ----------|
        size_t objectPointerStart = mBufferCount * sizeof(BufferBindingData);
        ASSERT(IsAligned(objectPointerStart, alignof(Ref<ObjectBase>)));
        return objectPointerStart + mBindingInfo.bindingCount * sizeof(Ref<ObjectBase>);
    }

    BindGroupLayoutBase::BindingDataPointers BindGroupLayoutBase::ComputeBindingDataPointers(
        void* dataStart) const {
        BufferBindingData* bufferData = reinterpret_cast<BufferBindingData*>(dataStart);
        auto bindings = reinterpret_cast<Ref<ObjectBase>*>(bufferData + mBufferCount);

        ASSERT(IsPtrAligned(bufferData, alignof(BufferBindingData)));
        ASSERT(IsPtrAligned(bindings, alignof(Ref<ObjectBase>)));

        return {bufferData, bindings};
    }

}  // namespace dawn_native
