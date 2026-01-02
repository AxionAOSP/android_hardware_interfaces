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

#include <algorithm>
#include <cstddef>
#include <dlfcn.h>
#include <memory>
#include <unordered_set>

#include <aidl/android/hardware/audio/effect/DefaultExtension.h>
#define LOG_TAG "AHAL_ViperEffect"
#include <android-base/logging.h>
#include <fmq/AidlMessageQueue.h>
#include <system/audio_effects/effect_uuid.h>

#include "ViperEffect.h"

using aidl::android::hardware::audio::effect::DefaultExtension;
using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::getEffectImplUuidViper;
using aidl::android::hardware::audio::effect::getEffectTypeUuidViper;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::Range;
using aidl::android::hardware::audio::effect::VendorExtension;
using aidl::android::hardware::audio::effect::ViperEffect;
using aidl::android::media::audio::common::AudioUuid;

static const char* kViperLibPaths[] = {
    "/vendor/lib64/soundfx/libv4a_re.so",
    "/vendor/lib/soundfx/libv4a_re.so",
    "/system/lib64/soundfx/libv4a_re.so",
    "/system/lib/soundfx/libv4a_re.so"
};

static const effect_uuid_t kViperUuid = {
    0x41d3c987, 0xe6cf, 0x11e3, 0xa88a, {0x11, 0xab, 0xa5, 0xd5, 0xc5, 0x1b}
};

extern "C" binder_exception_t createEffect(const AudioUuid* in_impl_uuid,
                                           std::shared_ptr<IEffect>* instanceSpp) {
    if (!in_impl_uuid || *in_impl_uuid != getEffectImplUuidViper()) {
        LOG(ERROR) << __func__ << " uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    if (instanceSpp) {
        *instanceSpp = ndk::SharedRefBase::make<ViperEffect>();
        LOG(DEBUG) << __func__ << " instance " << instanceSpp->get() << " created";
        return EX_NONE;
    } else {
        LOG(ERROR) << __func__ << " invalid input parameter!";
        return EX_ILLEGAL_ARGUMENT;
    }
}

extern "C" binder_exception_t queryEffect(const AudioUuid* in_impl_uuid, Descriptor* _aidl_return) {
    if (!in_impl_uuid || *in_impl_uuid != getEffectImplUuidViper()) {
        LOG(ERROR) << __func__ << " uuid not supported";
        return EX_ILLEGAL_ARGUMENT;
    }
    *_aidl_return = ViperEffect::kDescriptor;
    return EX_NONE;
}

namespace aidl::android::hardware::audio::effect {

const std::string ViperEffect::kEffectName = "ViPER4Android";

const Descriptor ViperEffect::kDescriptor = {
        .common = {.id = {.type = getEffectTypeUuidViper(),
                          .uuid = getEffectImplUuidViper(),
                          .proxy = std::nullopt},
                   .flags = {.type = Flags::Type::INSERT,
                             .insert = Flags::Insert::LAST,
                             .volume = Flags::Volume::CTRL},
                   .name = ViperEffect::kEffectName,
                   .implementor = "ViPER Team"}};

ViperEffect::ViperEffect() {
    loadLegacyLibrary();
}

ViperEffect::~ViperEffect() {
    cleanUp();
    unloadLegacyLibrary();
}

bool ViperEffect::loadLegacyLibrary() {
    for (const char* path : kViperLibPaths) {
        mLibHandle = dlopen(path, RTLD_NOW);
        if (mLibHandle) {
            LOG(INFO) << __func__ << " loaded legacy library from " << path;
            break;
        }
    }

    if (!mLibHandle) {
        LOG(WARNING) << __func__ << " failed to load legacy library: " << dlerror();
        return false;
    }

    mLibrary = static_cast<audio_effect_library_t*>(
            dlsym(mLibHandle, AUDIO_EFFECT_LIBRARY_INFO_SYM_AS_STR));
    if (!mLibrary) {
        LOG(ERROR) << __func__ << " failed to find AELI symbol: " << dlerror();
        dlclose(mLibHandle);
        mLibHandle = nullptr;
        return false;
    }

    if (mLibrary->tag != AUDIO_EFFECT_LIBRARY_TAG) {
        LOG(ERROR) << __func__ << " invalid library tag";
        dlclose(mLibHandle);
        mLibHandle = nullptr;
        mLibrary = nullptr;
        return false;
    }

    LOG(INFO) << __func__ << " legacy library loaded successfully: " << mLibrary->name;
    return true;
}

void ViperEffect::unloadLegacyLibrary() {
    if (mLegacyHandle && mLibrary) {
        mLibrary->release_effect(mLegacyHandle);
        mLegacyHandle = nullptr;
    }
    if (mLibHandle) {
        dlclose(mLibHandle);
        mLibHandle = nullptr;
        mLibrary = nullptr;
    }
}

ndk::ScopedAStatus ViperEffect::getDescriptor(Descriptor* _aidl_return) {
    LOG(DEBUG) << __func__ << kDescriptor.toString();
    *_aidl_return = kDescriptor;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ViperEffect::setParameterSpecific(const Parameter::Specific& specific) {
    RETURN_IF(Parameter::Specific::vendorEffect != specific.getTag(), EX_ILLEGAL_ARGUMENT,
              "EffectNotSupported");
    RETURN_IF(!mContext, EX_NULL_POINTER, "nullContext");

    auto& vendorEffect = specific.get<Parameter::Specific::vendorEffect>();
    std::optional<DefaultExtension> defaultExt;
    RETURN_IF(STATUS_OK != vendorEffect.extension.getParcelable(&defaultExt), EX_ILLEGAL_ARGUMENT,
              "getParcelableFailed");
    RETURN_IF(!defaultExt.has_value(), EX_ILLEGAL_ARGUMENT, "parcelableNull");

    if (mLegacyHandle && defaultExt->bytes.size() >= sizeof(effect_param_t)) {
        const effect_param_t* param = reinterpret_cast<const effect_param_t*>(defaultExt->bytes.data());
        uint32_t replySize = sizeof(int32_t);
        int32_t reply = 0;
        (*mLegacyHandle)->command(mLegacyHandle, EFFECT_CMD_SET_PARAM,
                                   defaultExt->bytes.size(), 
                                   const_cast<void*>(static_cast<const void*>(defaultExt->bytes.data())),
                                   &replySize, &reply);
        LOG(DEBUG) << __func__ << " forwarded param to legacy, reply=" << reply;
    }

    RETURN_IF(mContext->setParams(defaultExt->bytes) != RetCode::SUCCESS, EX_ILLEGAL_ARGUMENT,
              "paramNotSupported");

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus ViperEffect::getParameterSpecific(const Parameter::Id& id,
                                                     Parameter::Specific* specific) {
    auto tag = id.getTag();
    RETURN_IF(Parameter::Id::vendorEffectTag != tag, EX_ILLEGAL_ARGUMENT, "wrongIdTag");
    auto extensionId = id.get<Parameter::Id::vendorEffectTag>();
    std::optional<DefaultExtension> defaultIdExt;
    RETURN_IF(STATUS_OK != extensionId.extension.getParcelable(&defaultIdExt), EX_ILLEGAL_ARGUMENT,
              "getIdParcelableFailed");
    RETURN_IF(!defaultIdExt.has_value(), EX_ILLEGAL_ARGUMENT, "parcelableIdNull");

    VendorExtension extension;
    DefaultExtension defaultExt;
    defaultExt.bytes = mContext->getParams(defaultIdExt->bytes);
    RETURN_IF(STATUS_OK != extension.extension.setParcelable(defaultExt), EX_ILLEGAL_ARGUMENT,
              "setParcelableFailed");
    specific->set<Parameter::Specific::vendorEffect>(extension);
    return ndk::ScopedAStatus::ok();
}

std::shared_ptr<EffectContext> ViperEffect::createContext(const Parameter::Common& common) {
    if (mContext) {
        LOG(DEBUG) << __func__ << " context already exist";
        return mContext;
    }

    mContext = std::make_shared<ViperEffectContext>(1, common);

    if (mLibrary && !mLegacyHandle) {
        int32_t sessionId = common.session;
        int32_t ioId = common.ioHandle;
        int ret = mLibrary->create_effect(&kViperUuid, sessionId, ioId, &mLegacyHandle);
        if (ret != 0 || !mLegacyHandle) {
            LOG(ERROR) << __func__ << " failed to create legacy effect: " << ret;
            mLegacyHandle = nullptr;
        } else {
            LOG(INFO) << __func__ << " created legacy effect handle";
            mContext->setLegacyHandle(mLegacyHandle);

            effect_config_t config = {};
            config.inputCfg.samplingRate = common.input.base.sampleRate;
            config.inputCfg.channels = AUDIO_CHANNEL_OUT_STEREO;
            config.inputCfg.format = AUDIO_FORMAT_PCM_FLOAT;
            config.inputCfg.accessMode = EFFECT_BUFFER_ACCESS_READ;
            config.outputCfg = config.inputCfg;
            config.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;

            uint32_t replySize = sizeof(int32_t);
            int32_t reply = 0;
            (*mLegacyHandle)->command(mLegacyHandle, EFFECT_CMD_SET_CONFIG,
                                       sizeof(config), &config, &replySize, &reply);
            LOG(DEBUG) << __func__ << " SET_CONFIG reply=" << reply;

            (*mLegacyHandle)->command(mLegacyHandle, EFFECT_CMD_ENABLE,
                                       0, nullptr, &replySize, &reply);
            LOG(DEBUG) << __func__ << " ENABLE reply=" << reply;
            mLegacyEnabled = true;
        }
    }

    return mContext;
}

RetCode ViperEffect::releaseContext() {
    if (mLegacyHandle && mLegacyEnabled) {
        uint32_t replySize = sizeof(int32_t);
        int32_t reply = 0;
        (*mLegacyHandle)->command(mLegacyHandle, EFFECT_CMD_DISABLE,
                                   0, nullptr, &replySize, &reply);
        mLegacyEnabled = false;
    }
    if (mContext) {
        mContext.reset();
    }
    return RetCode::SUCCESS;
}

IEffect::Status ViperEffect::effectProcessImpl(float* in, float* out, int samples) {
    if (mLegacyHandle && mLegacyEnabled) {
        audio_buffer_t inBuf = {
            .frameCount = static_cast<size_t>(samples / 2),
            .f32 = in
        };
        audio_buffer_t outBuf = {
            .frameCount = static_cast<size_t>(samples / 2),
            .f32 = out
        };
        int ret = (*mLegacyHandle)->process(mLegacyHandle, &inBuf, &outBuf);
        if (ret == 0) {
            return {STATUS_OK, samples, samples};
        }
        LOG(WARNING) << __func__ << " legacy process returned " << ret;
    }

    for (int i = 0; i < samples; i++) {
        out[i] = in[i];
    }
    return {STATUS_OK, samples, samples};
}

}  // namespace aidl::android::hardware::audio::effect
