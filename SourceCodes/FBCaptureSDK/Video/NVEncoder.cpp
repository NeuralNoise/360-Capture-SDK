/*
*  Copyright (c) 2014, Facebook, Inc.
*  All rights reserved.
*
*  This source code is licensed under the BSD-style license found in the
*  LICENSE file in the root directory of this source tree. An additional grant
*  of patent rights can be found in the PATENTS file in the same directory.
*
*/

#include "NVEncoder.h"
#include "3rdParty/NVidia/common/inc/nvFileIO.h"
#include "3rdParty/NVidia/common/inc/nvUtils.h"

// To print out error string got from NV HW encoder API
const char* nvidiaStatus[] = { "NV_ENC_SUCCESS", "NV_ENC_ERR_NO_ENCODE_DEVICE", "NV_ENC_ERR_UNSUPPORTED_DEVICE", "NV_ENC_ERR_INVALID_ENCODERDEVICE",
"NV_ENC_ERR_INVALID_DEVICE", "NV_ENC_ERR_DEVICE_NOT_EXIST", "NV_ENC_ERR_INVALID_PTR", "NV_ENC_ERR_INVALID_EVENT",
"NV_ENC_ERR_INVALID_PARAM", "NV_ENC_ERR_INVALID_CALL", "NV_ENC_ERR_OUT_OF_MEMORY", "NV_ENC_ERR_ENCODER_NOT_INITIALIZED",
"NV_ENC_ERR_UNSUPPORTED_PARAM", "NV_ENC_ERR_LOCK_BUSY", "NV_ENC_ERR_NOT_ENOUGH_BUFFER", "NV_ENC_ERR_INVALID_VERSION",
"NV_ENC_ERR_MAP_FAILED", "NV_ENC_ERR_NEED_MORE_INPUT", "NV_ENC_ERR_ENCODER_BUSY", "NV_ENC_ERR_EVENT_NOT_REGISTERD",
"NV_ENC_ERR_GENERIC", "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY", "NV_ENC_ERR_UNIMPLEMENTED", "NV_ENC_ERR_RESOURCE_REGISTER_FAILED",
"NV_ENC_ERR_RESOURCE_NOT_REGISTERED", "NV_ENC_ERR_RESOURCE_NOT_MAPPED" };

namespace FBCapture {
  namespace Video {

    NVEncoder::NVEncoder(ID3D11Device* device) {
      device_ = device;

			this->encodingInitiated_ = false;
      encodeBufferCount_ = 1;
      eosOutputBfr_ = {};

      memset(encodeBuffer_, 0, MAX_ENCODE_QUEUE * sizeof(EncodeBuffer));
    }

    NVEncoder::~NVEncoder() {
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

      // Clean up session
      if (nvHWEncoder_) {
        nvHWEncoder_->NvEncDestroyEncoder();
        delete nvHWEncoder_;
        nvHWEncoder_ = nullptr;
      }

      releaseD3D11Resources();

    }

    FBCAPTURE_STATUS NVEncoder::setEncodeConfigs(const wstring& fullSavePath,
																										uint32_t width, 
																									  uint32_t height, 
																										int bitrate, 
																										int fps) {

      wstring_convert<std::codecvt_utf8<wchar_t>> stringTypeConversion;
      videoFileName_ = stringTypeConversion.to_bytes(fullSavePath);

      // Set encoding configuration
      memset(&encodeConfig_, 0, sizeof(EncodeConfig));
      encodeConfig_.endFrameIdx = INT_MAX;
      encodeConfig_.bitrate = bitrate;
      encodeConfig_.rcMode = NV_ENC_PARAMS_RC_CONSTQP;
      encodeConfig_.gopLength = NVENC_INFINITE_GOPLENGTH;
      encodeConfig_.deviceType = NV_ENC_DX11;
      encodeConfig_.codec = NV_ENC_H264;
      encodeConfig_.fps = fps;
      encodeConfig_.qp = 28;
      encodeConfig_.i_quant_factor = DEFAULT_I_QFACTOR;
      encodeConfig_.b_quant_factor = DEFAULT_B_QFACTOR;
      encodeConfig_.i_quant_offset = DEFAULT_I_QOFFSET;
      encodeConfig_.b_quant_offset = DEFAULT_B_QOFFSET;
      encodeConfig_.presetGUID = NV_ENC_PRESET_DEFAULT_GUID;
      encodeConfig_.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
      encodeConfig_.inputFormat = NV_ENC_BUFFER_FORMAT_ABGR;
      encodeConfig_.width = width;
      encodeConfig_.height = height;
      encodeConfig_.outputFileName = videoFileName_.c_str();
      encodeConfig_.fOutput = fopen(encodeConfig_.outputFileName, "wb");
      if (encodeConfig_.fOutput == nullptr) {
        DEBUG_ERROR_VAR("Failed to create ", videoFileName_.c_str());
        fclose(encodeConfig_.fOutput);
        remove(videoFileName_.c_str());
        return FBCAPTURE_STATUS_OUTPUT_FILE_OPEN_FAILED;
      }

      if (!encodeConfig_.outputFileName || encodeConfig_.width == 0 || encodeConfig_.height == 0) {
        DEBUG_ERROR("Invalid texture file");
        return FBCAPTURE_STATUS_ENCODE_SET_CONFIG_FAILED;
      }

      return FBCAPTURE_STATUS_OK;
    }

    void NVEncoder::releaseD3D11Resources() {
      encodingTexure_ = nullptr;
    }

    FBCAPTURE_STATUS NVEncoder::releaseEncodingResources() {
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

			if (nvHWEncoder_ == nullptr) {
				return FBCAPTURE_STATUS_OK;
			}

      releaseIOBuffers();

      if (encodeConfig_.fOutput) {
        fclose(encodeConfig_.fOutput);
      }

      // Clean up current encode session
      if (nvHWEncoder_->m_pEncodeAPI) {
        nvStatus = nvHWEncoder_->NvEncDestroyEncoder();
        delete nvHWEncoder_->m_pEncodeAPI;
        nvHWEncoder_->m_pEncodeAPI = nullptr;
        if (nvStatus != NV_ENC_SUCCESS) {
          DEBUG_ERROR_VAR("Failed to release resources. [Error code] ", nvidiaStatus[nvStatus]);
          return FBCAPTURE_STATUS_ENCODE_DESTROY_FAILED;
        }
      }

      return FBCAPTURE_STATUS_OK;
    }

    void NVEncoder::releaseIOBuffers() {
      for (uint32_t i = 0; i < encodeBufferCount_; i++) {
        if (encodeBuffer_[i].stInputBfr.pNVSurface) {
          encodeBuffer_[i].stInputBfr.pNVSurface->Release();
          encodeBuffer_[i].stInputBfr.pNVSurface = nullptr;
        }

        if (encodeBuffer_[i].stInputBfr.hInputSurface) {
          nvHWEncoder_->NvEncDestroyInputBuffer(encodeBuffer_[i].stInputBfr.hInputSurface);
          encodeBuffer_->stInputBfr.hInputSurface = nullptr;
        }

        if (encodeBuffer_[i].stOutputBfr.hBitstreamBuffer) {
          nvHWEncoder_->NvEncDestroyBitstreamBuffer(encodeBuffer_[i].stOutputBfr.hBitstreamBuffer);
          encodeBuffer_->stOutputBfr.hBitstreamBuffer = nullptr;
        }

        if (encodeBuffer_[i].stOutputBfr.hOutputEvent) {
          nvHWEncoder_->NvEncUnregisterAsyncEvent(encodeBuffer_[i].stOutputBfr.hOutputEvent);
          nvCloseFile(encodeBuffer_[i].stOutputBfr.hOutputEvent);
          encodeBuffer_[i].stOutputBfr.hOutputEvent = nullptr;
        }
      }

      if (eosOutputBfr_.hOutputEvent) {
        nvHWEncoder_->NvEncUnregisterAsyncEvent(eosOutputBfr_.hOutputEvent);
        nvCloseFile(eosOutputBfr_.hOutputEvent);
        eosOutputBfr_.hOutputEvent = nullptr;
      }
    }

    NVENCSTATUS NVEncoder::flushEncoder() {
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

      if (eosOutputBfr_.hOutputEvent == NULL)
        return NV_ENC_ERR_INVALID_CALL;

      nvStatus = nvHWEncoder_->NvEncFlushEncoderQueue(eosOutputBfr_.hOutputEvent);
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR_VAR("Failed on flush. Error code: ", nvidiaStatus[nvStatus]);
        assert(0);
        return nvStatus;
      }

      EncodeBuffer *encodeBuffer = encodeBufferQueue_.getPending();
      while (encodeBuffer) {
        nvHWEncoder_->ProcessOutput(encodeBuffer);
        encodeBuffer = encodeBufferQueue_.getPending();
        // UnMap the input buffer after frame is done
        if (encodeBuffer && encodeBuffer->stInputBfr.hInputSurface) {
          nvStatus = nvHWEncoder_->NvEncUnmapInputResource(encodeBuffer->stInputBfr.hInputSurface);
          encodeBuffer->stInputBfr.hInputSurface = NULL;
        }
      }

      if (WaitForSingleObject(eosOutputBfr_.hOutputEvent, 500) != WAIT_OBJECT_0) {
        assert(0);
        nvStatus = NV_ENC_ERR_GENERIC;
      }

      return nvStatus;
    }

    NVENCSTATUS NVEncoder::allocateIOBuffers(uint32_t width, uint32_t height, NV_ENC_BUFFER_FORMAT inputFormat) {
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

      // Initialize encode buffer
      encodeBufferQueue_.initialize(encodeBuffer_, encodeBufferCount_);
      for (uint32_t i = 0; i < encodeBufferCount_; i++) {

        // Create input buffer
        nvStatus = nvHWEncoder_->NvEncCreateInputBuffer(width, height, &encodeBuffer_[i].stInputBfr.hInputSurface, inputFormat);
        if (nvStatus != NV_ENC_SUCCESS) {
          DEBUG_ERROR_VAR("Creating input buffer has failed. Error code: ", nvidiaStatus[nvStatus]);
          return nvStatus;
        }

        encodeBuffer_[i].stInputBfr.bufferFmt = inputFormat;
        encodeBuffer_[i].stInputBfr.dwWidth = width;
        encodeBuffer_[i].stInputBfr.dwHeight = height;
        // Bit stream buffer
        nvStatus = nvHWEncoder_->NvEncCreateBitstreamBuffer(BITSTREAM_BUFFER_SIZE, &encodeBuffer_[i].stOutputBfr.hBitstreamBuffer);
        if (nvStatus != NV_ENC_SUCCESS) {
          DEBUG_ERROR_VAR("Creating bit stream buffer has failed. Error code: ", nvidiaStatus[nvStatus]);
          return nvStatus;
        }

        encodeBuffer_[i].stOutputBfr.dwBitstreamBufferSize = BITSTREAM_BUFFER_SIZE;
        // hOutputEvent
        nvStatus = nvHWEncoder_->NvEncRegisterAsyncEvent(&encodeBuffer_[i].stOutputBfr.hOutputEvent);
        if (nvStatus != NV_ENC_SUCCESS) {
          DEBUG_ERROR_VAR("Registering async event has failed. Error code: ", nvidiaStatus[nvStatus]);
          return nvStatus;
        }
      }
      eosOutputBfr_.bEOSFlag = TRUE;

      nvStatus = nvHWEncoder_->NvEncRegisterAsyncEvent(&eosOutputBfr_.hOutputEvent);
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR_VAR("Registering asyn event has failed. Error code: ", nvidiaStatus[nvStatus]);
        return nvStatus;
      }

      return nvStatus;
    }

    NVENCSTATUS NVEncoder::copyReources(uint32_t width, uint32_t height) {
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
      D3D11_MAPPED_SUBRESOURCE resource = {};
			
			if (mapTexture(resource) != FBCAPTURE_STATUS_OK) {
				DEBUG_ERROR("Failed on context mapping");
				return NV_ENC_ERR_GENERIC;
			}

      // lock input buffer
      NV_ENC_LOCK_INPUT_BUFFER lockInputBufferParams = {};

      uint32_t pitch = 0;
      lockInputBufferParams.bufferDataPtr = nullptr;

      nvStatus = nvHWEncoder_->NvEncLockInputBuffer(encodeBuffer_->stInputBfr.hInputSurface, &lockInputBufferParams.bufferDataPtr, &pitch);
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR_VAR("Creating nVidia input buffer failed. [Error code] ", nvidiaStatus[nvStatus]);
        return nvStatus;
      }

      // Write into Encode buffer
      memcpy(lockInputBufferParams.bufferDataPtr, resource.pData, height * resource.RowPitch);

      // Unlock input buffer
      nvStatus = nvHWEncoder_->NvEncUnlockInputBuffer(encodeBuffer_->stInputBfr.hInputSurface);
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR_VAR("Failed on nVidia unlock input buffer. [Error code] ", nvidiaStatus[nvStatus]);
        return nvStatus;
      }

      // Unmap buffer
      context_->Unmap(encodingTexure_.Get(), 0);

      return nvStatus;
    }

    FBCAPTURE_STATUS NVEncoder::dummyTextureEncoding() {
			FBCAPTURE_STATUS status = FBCAPTURE_STATUS_OK;

			// Block another dummy encoding session when session is already running
			if (this->encodingInitiated_) {
				return status;
			}

      D3D11_TEXTURE2D_DESC desc = {};
      HRESULT hr = S_OK;
      ScopedCOMPtr<ID3D11Texture2D> dummyTexture = nullptr;
      ZeroMemory(&desc, sizeof(desc));
      desc.Width = 100;
      desc.Height = 100;
      desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.ArraySize = 1;
      desc.MipLevels = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      hr = device_->CreateTexture2D(&desc, nullptr,
                                    &dummyTexture);
      if (FAILED(hr)) {
        DEBUG_HRESULT_ERROR(
          "Failed to create encoding Texture2D FBCaptureSystem. [Error code] ",
          hr);
        return FBCAPTURE_STATUS_SYSTEM_ENCODING_TEXTURE_CREATION_FAILED;
      }

      // Save dummy h264 to %LOCALAPPDATA%\FBCapture\dummy.h264
      PWSTR localAppPath = nullptr;
      hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppPath);
      if (FAILED(hr)) {
        throw runtime_error("Unable to locate LocalAppData");
      }
      std::wstring dummyFile = localAppPath;
      dummyFile += L"\\FBCapture\\dummy.h264";

      
      status = encodeProcess(dummyTexture, dummyFile, 1000000, 30, false);
      if (status != FBCAPTURE_STATUS_OK) {
        DEBUG_ERROR_VAR("Dummy encode session failed", to_string(status));
				releaseEncodingResources();
        _wremove(dummyFile.c_str());
        return status;
      }

      status = flushInputTextures();
      _wremove(dummyFile.c_str());

      return status;
    }

    FBCAPTURE_STATUS NVEncoder::initEncodingSession() {
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
      FBCAPTURE_STATUS status = FBCAPTURE_STATUS_OK;

      if (nvHWEncoder_ == nullptr) {
        nvHWEncoder_ = new CNvHWEncoder;
      }

      nvStatus = nvHWEncoder_->Initialize(device_, NV_ENC_DEVICE_TYPE_DIRECTX);
      if (nvStatus == NV_ENC_ERR_INVALID_VERSION) {
        DEBUG_ERROR("Not supported NVidia graphics driver version. Driver version should be 379.95 or newer.");
        status = FBCAPTURE_STATUS_UNSUPPORTED_GRAPHICS_CARD_DRIVER_VERSION;
        return status;
      } else if (nvStatus == NV_ENC_ERR_OUT_OF_MEMORY) {
        DEBUG_ERROR("Hardware encoder doesn't allow to open multiple encoding seesion. Please close another app using different encoding session.");
        status = FBCAPTURE_STATUS_MULTIPLE_ENCODING_SESSION;
        return status;
      } else if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR_VAR("Failed on initializing encoder. [Error code]", nvidiaStatus[nvStatus]);
        status = FBCAPTURE_STATUS_UNSUPPORTED_ENCODING_ENVIRONMENT;
        return status;
      }

      return status;
    }

    FBCAPTURE_STATUS NVEncoder::encodeProcess(const void* texturePtr, const wstring& fullSavePath, int bitrate, int fps, bool needFlipping) {
      int numFramesEncoded = 0;
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;
      FBCAPTURE_STATUS status = FBCAPTURE_STATUS_OK;

      status = createD3D11Resources((ID3D11Texture2D*)texturePtr, encodingTexure_);
      if (status != FBCAPTURE_STATUS_OK) {
        DEBUG_ERROR("Failed to create texture");
        return status;
      }

			if (globalTexDesc_.Width > 4096 || globalTexDesc_.Height > 4096) {
				DEBUG_ERROR("Invalid texture resolution. Max resolution is 4096 x 4096 on NVIDIA graphics card");
				return FBCAPTURE_STATUS_INVALID_TEXTURE_RESOLUTION;
			}

      // Initialize Encoder
      if (!this->encodingInitiated_) {

        status = setEncodeConfigs(fullSavePath, globalTexDesc_.Width, globalTexDesc_.Height, bitrate, fps);  // Set Encode Configs
        if (status != FBCAPTURE_STATUS_OK) {
          return status;
        }

        encodeConfig_.presetGUID = nvHWEncoder_->GetPresetGUID(encodeConfig_.encoderPreset, encodeConfig_.codec);

        nvStatus = nvHWEncoder_->CreateEncoder(&encodeConfig_);
        if (nvStatus != NV_ENC_SUCCESS) {
          DEBUG_ERROR_VAR("Failed on creating encoder. [Error code]", nvidiaStatus[nvStatus]);
          return FBCAPTURE_STATUS_ENCODER_CREATION_FAILED;
        }

        nvStatus = allocateIOBuffers(encodeConfig_.width, encodeConfig_.height, encodeConfig_.inputFormat);
        if (nvStatus != NV_ENC_SUCCESS) {
          DEBUG_ERROR_VAR("Failed on allocating IO buffers. [Error code]", nvidiaStatus[nvStatus]);
          return FBCAPTURE_STATUS_IO_BUFFER_ALLOCATION_FAILED;
        }

        setTextureDirtyRegion();
        this->encodingInitiated_ = true;
      }

      //Copy framebuffer to encoding buffers
      nvStatus = copyReources(encodeConfig_.width, encodeConfig_.height);
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR("Failed on copying framebuffers to encode  input buffers");
        return FBCAPTURE_STATUS_TEXTURE_RESOURCES_COPY_FAILED;
      }

      // Encode
      nvStatus = encodeFrame(encodeConfig_.width, encodeConfig_.height, encodeConfig_.inputFormat);
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR("Failed on copying framebuffers to encode  input buffers");
        return FBCAPTURE_STATUS_ENCODE_PICTURE_FAILED;
      }

      return status;
    }

    FBCAPTURE_STATUS NVEncoder::flushInputTextures() {
      NVENCSTATUS nvStatus;
      FBCAPTURE_STATUS status;

			this->encodingInitiated_ = false;

      nvStatus = flushEncoder();
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR_VAR("Failed to flush inputs from buffer. [Error code] ", nvidiaStatus[nvStatus]);
      }
      status = releaseEncodingResources();

      return nvStatus != NV_ENC_SUCCESS ? FBCAPTURE_STATUS_ENCODE_FLUSH_FAILED : status;
    }

    NVENCSTATUS NVEncoder::encodeFrame(uint32_t width, uint32_t height, NV_ENC_BUFFER_FORMAT inputformat) {
      NVENCSTATUS nvStatus = NV_ENC_SUCCESS;

      uint32_t lockedPitch = 0;
      EncodeBuffer *pEncodeBuffer = nullptr;

      int8_t* qpDeltaMapArray = nullptr;
      unsigned int qpDeltaMapArraySize = 0;

      pEncodeBuffer = encodeBufferQueue_.getAvailable();
      if (!pEncodeBuffer) {
        nvHWEncoder_->ProcessOutput(encodeBufferQueue_.getPending());
        pEncodeBuffer = encodeBufferQueue_.getAvailable();
      }

      nvStatus = nvHWEncoder_->NvEncEncodeFrame(pEncodeBuffer, nullptr, width, height, NV_ENC_PIC_STRUCT_FRAME, qpDeltaMapArray, qpDeltaMapArraySize);
      if (nvStatus != NV_ENC_SUCCESS) {
        DEBUG_ERROR_VAR("Failed on encoding frames. Error code:", nvidiaStatus[nvStatus]);
        return nvStatus;
      }

      return nvStatus;
    }
  }
}
