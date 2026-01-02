/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <aidl/android/hardware/audio/effect/BnEffect.h>
#include <fmq/AidlMessageQueue.h>
#include <hardware/audio_effect.h>
#include <memory>
#include <vector>

#include "effect-impl/EffectImpl.h"

namespace aidl::android::hardware::audio::effect {

class ViperEffectContext final : public EffectContext {
  public:
    ViperEffectContext(int statusDepth, const Parameter::Common& common)
        : EffectContext(statusDepth, common) {}

    void setLegacyHandle(effect_handle_t handle) { mLegacyHandle = handle; }
    effect_handle_t getLegacyHandle() const { return mLegacyHandle; }

    RetCode setParams(const std::vector<uint8_t>& params) {
        mParams = params;
        return RetCode::SUCCESS;
    }
    std::vector<uint8_t> getParams(std::vector<uint8_t> id __unused) const { return mParams; }

  private:
    std::vector<uint8_t> mParams;
    effect_handle_t mLegacyHandle = nullptr;
};

class ViperEffect final : public EffectImpl {
  public:
    static const std::string kEffectName;
    static const Descriptor kDescriptor;

    ViperEffect();
    ~ViperEffect();

    ndk::ScopedAStatus getDescriptor(Descriptor* _aidl_return) override;
    ndk::ScopedAStatus setParameterSpecific(const Parameter::Specific& specific)
            REQUIRES(mImplMutex) override;
    ndk::ScopedAStatus getParameterSpecific(const Parameter::Id& id, Parameter::Specific* specific)
            REQUIRES(mImplMutex) override;

    std::shared_ptr<EffectContext> createContext(const Parameter::Common& common)
            REQUIRES(mImplMutex) override;
    RetCode releaseContext() REQUIRES(mImplMutex) override;

    std::string getEffectName() override { return kEffectName; }
    IEffect::Status effectProcessImpl(float* in, float* out, int samples)
            REQUIRES(mImplMutex) override;

  private:
    bool loadLegacyLibrary();
    void unloadLegacyLibrary();

    std::shared_ptr<ViperEffectContext> mContext GUARDED_BY(mImplMutex);
    void* mLibHandle = nullptr;
    audio_effect_library_t* mLibrary = nullptr;
    effect_handle_t mLegacyHandle = nullptr;
    bool mLegacyEnabled = false;
};

}  // namespace aidl::android::hardware::audio::effect
