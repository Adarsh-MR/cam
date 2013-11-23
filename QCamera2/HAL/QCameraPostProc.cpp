/* Copyright (c) 2012-2013, The Linux Foundataion. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are
* met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above
*       copyright notice, this list of conditions and the following
*       disclaimer in the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of The Linux Foundation nor the names of its
*       contributors may be used to endorse or promote products derived
*       from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#define LOG_TAG "QCameraPostProc"

#include <fcntl.h>
#include <stdlib.h>
#include <utils/Errors.h>

#include "QCamera2HWI.h"
#include "QCameraPostProc.h"

namespace qcamera {

const char *QCameraPostProcessor::STORE_LOCATION = "/sdcard/img_%d.jpg";

/*===========================================================================
 * FUNCTION   : QCameraPostProcessor
 *
 * DESCRIPTION: constructor of QCameraPostProcessor.
 *
 * PARAMETERS :
 *   @cam_ctrl : ptr to HWI object
 *
 * RETURN     : None
 *==========================================================================*/
QCameraPostProcessor::QCameraPostProcessor(QCamera2HardwareInterface *cam_ctrl)
    : m_parent(cam_ctrl),
      mJpegCB(NULL),
      mJpegUserData(NULL),
      mJpegClientHandle(0),
      mJpegSessionId(0),
      m_pJpegOutputMem(NULL),
      m_pJpegExifObj(NULL),
      m_bThumbnailNeeded(TRUE),
      m_pReprocChannel(NULL),
      m_bInited(FALSE),
      m_inputPPQ(releasePPInputData, this),
      m_ongoingPPQ(releaseOngoingPPData, this),
      m_inputJpegQ(releaseJpegData, this),
      m_ongoingJpegQ(releaseJpegData, this),
      m_inputRawQ(releaseRawData, this),
      mRawBurstCount(0),
      mSaveFrmCnt(0),
      mUseSaveProc(false),
      mUseJpegBurst(false)
{
    memset(&mJpegHandle, 0, sizeof(mJpegHandle));
}

/*===========================================================================
 * FUNCTION   : ~QCameraPostProcessor
 *
 * DESCRIPTION: deconstructor of QCameraPostProcessor.
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraPostProcessor::~QCameraPostProcessor()
{
    if (m_pJpegOutputMem != NULL) {
        m_pJpegOutputMem->deallocate();
        delete m_pJpegOutputMem;
        m_pJpegOutputMem = NULL;
    }
    if (m_pJpegExifObj != NULL) {
        delete m_pJpegExifObj;
        m_pJpegExifObj = NULL;
    }
    if (m_pReprocChannel != NULL) {
        m_pReprocChannel->stop();
        delete m_pReprocChannel;
        m_pReprocChannel = NULL;
    }
}

/*===========================================================================
 * FUNCTION   : init
 *
 * DESCRIPTION: initialization of postprocessor
 *
 * PARAMETERS :
 *   @jpeg_cb      : callback to handle jpeg event from mm-camera-interface
 *   @user_data    : user data ptr for jpeg callback
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::init(jpeg_encode_callback_t jpeg_cb, void *user_data)
{
    mJpegCB = jpeg_cb;
    mJpegUserData = user_data;
    mm_dimension max_size;

    //set max pic size
    memset(&max_size, 0, sizeof(mm_dimension));
    max_size.w = m_parent->m_max_pic_width;
    max_size.h = m_parent->m_max_pic_height;

    mJpegClientHandle = jpeg_open(&mJpegHandle, max_size);
    if(!mJpegClientHandle) {
        ALOGE("%s : jpeg_open did not work", __func__);
        return UNKNOWN_ERROR;
    }

    m_dataProcTh.launch(dataProcessRoutine, this);
    m_saveProcTh.launch(dataSaveRoutine, this);

    m_bInited = TRUE;
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : deinit
 *
 * DESCRIPTION: de-initialization of postprocessor
 *
 * PARAMETERS : None
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::deinit()
{
    if (m_bInited == TRUE) {
        m_dataProcTh.exit();
        m_saveProcTh.exit();

        if(mJpegClientHandle > 0) {
            int rc = mJpegHandle.close(mJpegClientHandle);
            ALOGE("%s: Jpeg closed, rc = %d, mJpegClientHandle = %x",
                  __func__, rc, mJpegClientHandle);
            mJpegClientHandle = 0;
            memset(&mJpegHandle, 0, sizeof(mJpegHandle));
        }
        m_bInited = FALSE;
    }
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : start
 *
 * DESCRIPTION: start postprocessor. Data process thread and data notify thread
 *              will be launched.
 *
 * PARAMETERS :
 *   @pSrcChannel : source channel obj ptr that possibly needs reprocess
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : if any reprocess is needed, a reprocess channel/stream
 *              will be started.
 *==========================================================================*/
int32_t QCameraPostProcessor::start(QCameraChannel *pSrcChannel)
{
    char prop[PROPERTY_VALUE_MAX];
    int32_t rc = NO_ERROR;
    if (m_bInited == FALSE) {
        ALOGE("%s: postproc not initialized yet", __func__);
        return UNKNOWN_ERROR;
    }

    if (m_parent->needReprocess()) {
        if (m_pReprocChannel != NULL) {
            delete m_pReprocChannel;
            m_pReprocChannel = NULL;
        }
        // if reprocess is needed, start reprocess channel
        m_pReprocChannel = m_parent->addOnlineReprocChannel(pSrcChannel);
        if (m_pReprocChannel == NULL) {
            ALOGE("%s: cannot add reprocess channel", __func__);
            return UNKNOWN_ERROR;
        }

        rc = m_pReprocChannel->start();
        if (rc != 0) {
            ALOGE("%s: cannot start reprocess channel", __func__);
            delete m_pReprocChannel;
            m_pReprocChannel = NULL;
            return rc;
        }
    }

    property_get("persist.camera.longshot.save", prop, "0");
    mUseSaveProc = atoi(prop) > 0 ? true : false;

    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_START_DATA_PROC, FALSE, FALSE);
    m_parent->m_cbNotifier.startSnapshots();
    mRawBurstCount = m_parent->numOfSnapshotsExpected();
    return rc;
}

/*===========================================================================
 * FUNCTION   : stop
 *
 * DESCRIPTION: stop postprocessor. Data process and notify thread will be stopped.
 *
 * PARAMETERS : None
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : reprocess channel will be stopped and deleted if there is any
 *==========================================================================*/
int32_t QCameraPostProcessor::stop()
{
    if (m_bInited == TRUE) {
        m_parent->m_cbNotifier.stopSnapshots();
        // dataProc Thread need to process "stop" as sync call because abort jpeg job should be a sync call
        m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_STOP_DATA_PROC, TRUE, TRUE);
    }

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : getJpegEncodingConfig
 *
 * DESCRIPTION: function to prepare encoding job information
 *
 * PARAMETERS :
 *   @encode_parm   : param to be filled with encoding configuration
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::getJpegEncodingConfig(mm_jpeg_encode_params_t& encode_parm,
                                                    QCameraStream *main_stream,
                                                    QCameraStream *thumb_stream)
{
    ALOGV("%s : E", __func__);
    int32_t ret = NO_ERROR;
    camera_memory_t *jpeg_mem = NULL;

    char prop[PROPERTY_VALUE_MAX];
    property_get("persist.camera.jpeg_burst", prop, "0");
    mUseJpegBurst = (atoi(prop) > 0) && !mUseSaveProc;
    encode_parm.burst_mode = mUseJpegBurst;

    cam_rect_t crop;
    memset(&crop, 0, sizeof(cam_rect_t));
    main_stream->getCropInfo(crop);

    cam_dimension_t src_dim, dst_dim;
    memset(&src_dim, 0, sizeof(cam_dimension_t));
    memset(&dst_dim, 0, sizeof(cam_dimension_t));
    main_stream->getFrameDimension(src_dim);

    bool hdr_output_crop = m_parent->mParameters.isHDROutputCropEnabled();
    if (hdr_output_crop && crop.height) {
        dst_dim.height = crop.height;
    } else {
        dst_dim.height = src_dim.height;
    }
    if (hdr_output_crop && crop.width) {
        dst_dim.width = crop.width;
    } else {
        dst_dim.width = src_dim.width;
    }

    // set rotation only when no online rotation or offline pp rotation is done before
    if (!m_parent->needRotationReprocess()) {
        encode_parm.rotation = m_parent->getJpegRotation();
    }

    encode_parm.main_dim.src_dim = src_dim;
    encode_parm.main_dim.dst_dim = dst_dim;

    encode_parm.jpeg_cb = mJpegCB;
    encode_parm.userdata = mJpegUserData;

    m_bThumbnailNeeded = TRUE; // need encode thumbnail by default
    cam_dimension_t thumbnailSize;
    memset(&thumbnailSize, 0, sizeof(cam_dimension_t));
    m_parent->getThumbnailSize(thumbnailSize);
    if (thumbnailSize.width == 0 || thumbnailSize.height == 0) {
        // (0,0) means no thumbnail
        m_bThumbnailNeeded = FALSE;
    }
    encode_parm.encode_thumbnail = m_bThumbnailNeeded;

    // get color format
    cam_format_t img_fmt = CAM_FORMAT_YUV_420_NV12;
    main_stream->getFormat(img_fmt);
    encode_parm.color_format = getColorfmtFromImgFmt(img_fmt);

    // get jpeg quality
    encode_parm.quality = m_parent->getJpegQuality();
    if (encode_parm.quality <= 0) {
        encode_parm.quality = 85;
    }
    cam_frame_len_offset_t main_offset;
    memset(&main_offset, 0, sizeof(cam_frame_len_offset_t));
    main_stream->getFrameOffset(main_offset);

    // src buf config
    QCameraMemory *pStreamMem = main_stream->getStreamBufs();
    if (pStreamMem == NULL) {
        ALOGE("%s: cannot get stream bufs from main stream", __func__);
        ret = BAD_VALUE;
        goto on_error;
    }
    encode_parm.num_src_bufs = pStreamMem->getCnt();
    for (uint32_t i = 0; i < encode_parm.num_src_bufs; i++) {
        camera_memory_t *stream_mem = pStreamMem->getMemory(i, false);
        if (stream_mem != NULL) {
            encode_parm.src_main_buf[i].index = i;
            encode_parm.src_main_buf[i].buf_size = stream_mem->size;
            encode_parm.src_main_buf[i].buf_vaddr = (uint8_t *)stream_mem->data;
            encode_parm.src_main_buf[i].fd = pStreamMem->getFd(i);
            encode_parm.src_main_buf[i].format = MM_JPEG_FMT_YUV;
            encode_parm.src_main_buf[i].offset = main_offset;
        }
    }

    if (m_bThumbnailNeeded == TRUE) {
        if (thumb_stream == NULL) {
            thumb_stream = main_stream;
        }
        pStreamMem = thumb_stream->getStreamBufs();
        if (pStreamMem == NULL) {
            ALOGE("%s: cannot get stream bufs from thumb stream", __func__);
            ret = BAD_VALUE;
            goto on_error;
        }
        cam_frame_len_offset_t thumb_offset;
        memset(&thumb_offset, 0, sizeof(cam_frame_len_offset_t));
        thumb_stream->getFrameOffset(thumb_offset);
        encode_parm.num_tmb_bufs =  pStreamMem->getCnt();
        for (int i = 0; i < pStreamMem->getCnt(); i++) {
            camera_memory_t *stream_mem = pStreamMem->getMemory(i, false);
            if (stream_mem != NULL) {
                encode_parm.src_thumb_buf[i].index = i;
                encode_parm.src_thumb_buf[i].buf_size = stream_mem->size;
                encode_parm.src_thumb_buf[i].buf_vaddr = (uint8_t *)stream_mem->data;
                encode_parm.src_thumb_buf[i].fd = pStreamMem->getFd(i);
                encode_parm.src_thumb_buf[i].format = MM_JPEG_FMT_YUV;
                encode_parm.src_thumb_buf[i].offset = thumb_offset;
            }
        }
        cam_format_t img_fmt_thumb = CAM_FORMAT_YUV_420_NV12;
        thumb_stream->getFormat(img_fmt_thumb);
        encode_parm.thumb_color_format = getColorfmtFromImgFmt(img_fmt_thumb);

        memset(&crop, 0, sizeof(cam_rect_t));
        thumb_stream->getCropInfo(crop);
        memset(&src_dim, 0, sizeof(cam_dimension_t));
        thumb_stream->getFrameDimension(src_dim);
        encode_parm.thumb_dim.src_dim = src_dim;
        m_parent->getThumbnailSize(encode_parm.thumb_dim.dst_dim);
        int rotation = m_parent->getJpegRotation();
        if (rotation == 90 || rotation ==270) {
            // swap dimension if rotation is 90 or 270
            int32_t temp = encode_parm.thumb_dim.dst_dim.height;
            encode_parm.thumb_dim.dst_dim.height =
                encode_parm.thumb_dim.dst_dim.width;
            encode_parm.thumb_dim.dst_dim.width = temp;
          }
        encode_parm.thumb_dim.crop = crop;
    }


    // allocate output buf for jpeg encoding
    if (m_pJpegOutputMem != NULL) {
        m_pJpegOutputMem->deallocate();
        delete m_pJpegOutputMem;
        m_pJpegOutputMem = NULL;
    }
    m_pJpegOutputMem = new QCameraStreamMemory(m_parent->mGetMemory,
                                               QCAMERA_ION_USE_CACHE);
    if (NULL == m_pJpegOutputMem) {
        ret = NO_MEMORY;
        ALOGE("%s : No memory for m_pJpegOutputMem", __func__);
        goto on_error;
    }

    encode_parm.num_dst_bufs = 1;
    if (mUseJpegBurst) {
        encode_parm.num_dst_bufs = 2;
    }

    ret = m_pJpegOutputMem->allocate(encode_parm.num_dst_bufs, main_offset.frame_len);
    if(ret != OK) {
        ret = NO_MEMORY;
        ALOGE("%s : No memory for m_pJpegOutputMem", __func__);
        goto on_error;
    }

    for (int i = 0; i < (int)encode_parm.num_dst_bufs; i++) {
        jpeg_mem = m_pJpegOutputMem->getMemory(i, false);
        if (NULL == jpeg_mem) {
          ret = NO_MEMORY;
          ALOGE("%s : initHeapMem for jpeg, ret = NO_MEMORY", __func__);
          goto on_error;
        }

        encode_parm.dest_buf[i].index = i;
        encode_parm.dest_buf[i].buf_size = jpeg_mem->size;
        encode_parm.dest_buf[i].buf_vaddr = (uint8_t *)jpeg_mem->data;
        encode_parm.dest_buf[i].fd = m_pJpegOutputMem->getFd(i);
        encode_parm.dest_buf[i].format = MM_JPEG_FMT_YUV;
        encode_parm.dest_buf[i].offset = main_offset;
    }


    ALOGV("%s : X", __func__);
    return NO_ERROR;

on_error:
    if (m_pJpegOutputMem != NULL) {
        m_pJpegOutputMem->deallocate();
        delete m_pJpegOutputMem;
        m_pJpegOutputMem = NULL;
    }
    ALOGV("%s : X with error %d", __func__, ret);
    return ret;
}

/*===========================================================================
 * FUNCTION   : sendEvtNotify
 *
 * DESCRIPTION: send event notify through notify callback registered by upper layer
 *
 * PARAMETERS :
 *   @msg_type: msg type of notify
 *   @ext1    : extension
 *   @ext2    : extension
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::sendEvtNotify(int32_t msg_type,
                                            int32_t ext1,
                                            int32_t ext2)
{
    return m_parent->sendEvtNotify(msg_type, ext1, ext2);
}

/*===========================================================================
 * FUNCTION   : sendDataNotify
 *
 * DESCRIPTION: enqueue data into dataNotify thread
 *
 * PARAMETERS :
 *   @msg_type: data callback msg type
 *   @data    : ptr to data memory struct
 *   @index   : index to data buffer
 *   @metadata: ptr to meta data buffer if there is any
 *   @release_data : ptr to struct indicating if data need to be released
 *                   after notify
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::sendDataNotify(int32_t msg_type,
                                             camera_memory_t *data,
                                             uint8_t index,
                                             camera_frame_metadata_t *metadata,
                                             qcamera_release_data_t *release_data)
{
    qcamera_data_argm_t *data_cb = (qcamera_data_argm_t *)malloc(sizeof(qcamera_data_argm_t));
    if (NULL == data_cb) {
        ALOGE("%s: no mem for acamera_data_argm_t", __func__);
        return NO_MEMORY;
    }
    memset(data_cb, 0, sizeof(qcamera_data_argm_t));
    data_cb->msg_type = msg_type;
    data_cb->data = data;
    data_cb->index = index;
    data_cb->metadata = metadata;
    if (release_data != NULL) {
        data_cb->release_data = *release_data;
    }

    qcamera_callback_argm_t cbArg;
    memset(&cbArg, 0, sizeof(qcamera_callback_argm_t));
    cbArg.cb_type = QCAMERA_DATA_SNAPSHOT_CALLBACK;
    cbArg.msg_type = msg_type;
    cbArg.data = data;
    cbArg.metadata = metadata;
    cbArg.user_data = data_cb;
    cbArg.cookie = this;
    cbArg.release_cb = releaseNotifyData;
    int rc = m_parent->m_cbNotifier.notifyCallback(cbArg);
    if ( NO_ERROR != rc ) {
        ALOGE("%s: Error enqueuing jpeg data into notify queue", __func__);
        releaseNotifyData(data_cb, this, UNKNOWN_ERROR);
        return UNKNOWN_ERROR;
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : processData
 *
 * DESCRIPTION: enqueue data into dataProc thread
 *
 * PARAMETERS :
 *   @frame   : process frame received from mm-camera-interface
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : depends on if offline reprocess is needed, received frame will
 *              be sent to either input queue of postprocess or jpeg encoding
 *==========================================================================*/
int32_t QCameraPostProcessor::processData(mm_camera_super_buf_t *frame)
{
    if (m_bInited == FALSE) {
        ALOGE("%s: postproc not initialized yet", __func__);
        return UNKNOWN_ERROR;
    }

    if (m_parent->needReprocess()) {
        if ( !m_parent->isLongshotEnabled() ) {
            //play shutter sound
            m_parent->playShutter();
        }

        ALOGD("%s: need reprocess", __func__);
        // enqueu to post proc input queue
        m_inputPPQ.enqueue((void *)frame);
    } else if (m_parent->mParameters.isNV16PictureFormat() ||
        m_parent->mParameters.isNV21PictureFormat()) {
        //check if raw frame information is needed.
        if(m_parent->mParameters.isYUVFrameInfoNeeded())
            setYUVFrameInfo(frame);

        processRawData(frame);
    } else {
        //play shutter sound
        if(!m_parent->m_stateMachine.isNonZSLCaptureRunning() &&
           !m_parent->mLongshotEnabled)
           m_parent->playShutter();

        ALOGD("%s: no need offline reprocess, sending to jpeg encoding", __func__);
        qcamera_jpeg_data_t *jpeg_job =
            (qcamera_jpeg_data_t *)malloc(sizeof(qcamera_jpeg_data_t));
        if (jpeg_job == NULL) {
            ALOGE("%s: No memory for jpeg job", __func__);
            return NO_MEMORY;
        }

        memset(jpeg_job, 0, sizeof(qcamera_jpeg_data_t));
        jpeg_job->src_frame = frame;

        // enqueu to jpeg input queue
        m_inputJpegQ.enqueue((void *)jpeg_job);
    }
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : processRawData
 *
 * DESCRIPTION: enqueue raw data into dataProc thread
 *
 * PARAMETERS :
 *   @frame   : process frame received from mm-camera-interface
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::processRawData(mm_camera_super_buf_t *frame)
{
    if (m_bInited == FALSE) {
        ALOGE("%s: postproc not initialized yet", __func__);
        return UNKNOWN_ERROR;
    }

    // enqueu to raw input queue
    m_inputRawQ.enqueue((void *)frame);
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : processJpegEvt
 *
 * DESCRIPTION: process jpeg event from mm-jpeg-interface.
 *
 * PARAMETERS :
 *   @evt     : payload of jpeg event, including information about jpeg encoding
 *              status, jpeg size and so on.
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : This event will also trigger DataProc thread to move to next job
 *              processing (i.e., send a new jpeg encoding job to mm-jpeg-interface
 *              if there is any pending job in jpeg input queue)
 *==========================================================================*/
int32_t QCameraPostProcessor::processJpegEvt(qcamera_jpeg_evt_payload_t *evt)
{
    if (m_bInited == FALSE) {
        ALOGE("%s: postproc not initialized yet", __func__);
        return UNKNOWN_ERROR;
    }

    int32_t rc = NO_ERROR;
    camera_memory_t *jpeg_mem = NULL;

    if (mUseSaveProc && m_parent->isLongshotEnabled()) {
        qcamera_jpeg_evt_payload_t *saveData = ( qcamera_jpeg_evt_payload_t * ) malloc(sizeof(qcamera_jpeg_evt_payload_t));
        if ( NULL == saveData ) {
            ALOGE("%s: Can not allocate save data message!", __func__);
            return NO_MEMORY;
        }
        *saveData = *evt;
        m_inputSaveQ.enqueue((void *) saveData);
        m_saveProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);
    } else {
        // Release jpeg job data
        m_ongoingJpegQ.flushNodes(matchJobId, (void*)&evt->jobId);

        ALOGD("[KPI Perf] %s : jpeg job %d", __func__, evt->jobId);

        if (m_parent->mDataCb == NULL ||
            m_parent->msgTypeEnabledWithLock(CAMERA_MSG_COMPRESSED_IMAGE) == 0 ) {
            ALOGD("%s: No dataCB or CAMERA_MSG_COMPRESSED_IMAGE not enabled",
                  __func__);
            rc = NO_ERROR;
            goto end;
        }

        if(evt->status == JPEG_JOB_STATUS_ERROR) {
            ALOGE("%s: Error event handled from jpeg, status = %d",
                  __func__, evt->status);
            rc = FAILED_TRANSACTION;
            goto end;
        }

        m_parent->dumpJpegToFile(evt->out_data.buf_vaddr,
                                  evt->out_data.buf_filled_len,
                                  evt->jobId);
        ALOGD("%s: Dump jpeg_size=%d", __func__, evt->out_data.buf_filled_len);

        // alloc jpeg memory to pass to upper layer
        jpeg_mem = m_parent->mGetMemory(-1, evt->out_data.buf_filled_len, 1, m_parent->mCallbackCookie);
        if (NULL == jpeg_mem) {
            rc = NO_MEMORY;
            ALOGE("%s : getMemory for jpeg, ret = NO_MEMORY", __func__);
            goto end;
        }
        memcpy(jpeg_mem->data, evt->out_data.buf_vaddr, evt->out_data.buf_filled_len);

        ALOGE("%s : Calling upperlayer callback to store JPEG image", __func__);
        qcamera_release_data_t release_data;
        memset(&release_data, 0, sizeof(qcamera_release_data_t));
        release_data.data = jpeg_mem;
        ALOGE("[KPI Perf] %s: PROFILE_JPEG_CB ",__func__);
        rc = sendDataNotify(CAMERA_MSG_COMPRESSED_IMAGE,
                            jpeg_mem,
                            0,
                            NULL,
                            &release_data);

end:
        if (rc != NO_ERROR) {
            // send error msg to upper layer
            sendEvtNotify(CAMERA_MSG_ERROR,
                          UNKNOWN_ERROR,
                          0);

            if (NULL != jpeg_mem) {
                jpeg_mem->release(jpeg_mem);
                jpeg_mem = NULL;
            }
        }
    }

    // wait up data proc thread to do next job,
    // if previous request is blocked due to ongoing jpeg job
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);

    return rc;
}

/*===========================================================================
 * FUNCTION   : processPPData
 *
 * DESCRIPTION: process received frame after reprocess.
 *
 * PARAMETERS :
 *   @frame   : received frame from reprocess channel.
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : The frame after reprocess need to send to jpeg encoding.
 *==========================================================================*/
int32_t QCameraPostProcessor::processPPData(mm_camera_super_buf_t *frame)
{
    if (m_bInited == FALSE) {
        ALOGE("%s: postproc not initialized yet", __func__);
        return UNKNOWN_ERROR;
    }

    qcamera_pp_data_t *job = (qcamera_pp_data_t *)m_ongoingPPQ.dequeue();

    if (job == NULL || job->src_frame == NULL) {
        ALOGE("%s: Cannot find reprocess job", __func__);
        return BAD_VALUE;
    }

    if (m_parent->mParameters.isNV16PictureFormat() ||
        m_parent->mParameters.isNV21PictureFormat()) {
        releaseSuperBuf(job->src_frame);
        free(job->src_frame);
        free(job);

        if(m_parent->mParameters.isYUVFrameInfoNeeded())
            setYUVFrameInfo(frame);
        return processRawData(frame);
    }

    if ( m_parent->isLongshotEnabled() ) {
        // play shutter sound for longshot
        // after reprocess is done
        // TODO: Move this after CAC done event

        m_parent->playShutter();
    }

    qcamera_jpeg_data_t *jpeg_job =
        (qcamera_jpeg_data_t *)malloc(sizeof(qcamera_jpeg_data_t));
    if (jpeg_job == NULL) {
        ALOGE("%s: No memory for jpeg job", __func__);
        return NO_MEMORY;
    }

    memset(jpeg_job, 0, sizeof(qcamera_jpeg_data_t));
    jpeg_job->src_frame = frame;
    jpeg_job->src_reproc_frame = job->src_frame;

    // free pp job buf
    free(job);

    // enqueu reprocessed frame to jpeg input queue
    m_inputJpegQ.enqueue((void *)jpeg_job);

    // wait up data proc thread
    m_dataProcTh.sendCmd(CAMERA_CMD_TYPE_DO_NEXT_JOB, FALSE, FALSE);

    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : findJpegJobByJobId
 *
 * DESCRIPTION: find a jpeg job from ongoing Jpeg queue by its job ID
 *
 * PARAMETERS :
 *   @jobId   : job Id of the job
 *
 * RETURN     : ptr to a jpeg job struct. NULL if not found.
 *
 * NOTE       : Currently only one job is sending to mm-jpeg-interface for jpeg
 *              encoding. Therefore simply dequeue from the ongoing Jpeg Queue
 *              will serve the purpose to find the jpeg job.
 *==========================================================================*/
qcamera_jpeg_data_t *QCameraPostProcessor::findJpegJobByJobId(uint32_t jobId)
{
    qcamera_jpeg_data_t * job = NULL;
    if (jobId == 0) {
        ALOGE("%s: not a valid jpeg jobId", __func__);
        return NULL;
    }

    // currely only one jpeg job ongoing, so simply dequeue the head
    job = (qcamera_jpeg_data_t *)m_ongoingJpegQ.dequeue();
    return job;
}

/*===========================================================================
 * FUNCTION   : releasePPInputData
 *
 * DESCRIPTION: callback function to release post process input data node
 *
 * PARAMETERS :
 *   @data      : ptr to post process input data
 *   @user_data : user data ptr (QCameraReprocessor)
 *
 * RETURN     : None
 *==========================================================================*/
void QCameraPostProcessor::releasePPInputData(void *data, void *user_data)
{
    QCameraPostProcessor *pme = (QCameraPostProcessor *)user_data;
    if (NULL != pme) {
        pme->releaseSuperBuf((mm_camera_super_buf_t *)data);
    }
}

/*===========================================================================
 * FUNCTION   : releaseJpegData
 *
 * DESCRIPTION: callback function to release jpeg job node
 *
 * PARAMETERS :
 *   @data      : ptr to ongoing jpeg job data
 *   @user_data : user data ptr (QCameraReprocessor)
 *
 * RETURN     : None
 *==========================================================================*/
void QCameraPostProcessor::releaseJpegData(void *data, void *user_data)
{
    QCameraPostProcessor *pme = (QCameraPostProcessor *)user_data;
    if (NULL != pme) {
        pme->releaseJpegJobData((qcamera_jpeg_data_t *)data);
        ALOGD("%s : Rleased job ID %u", __func__,
            ((qcamera_jpeg_data_t *)data)->jobId);
    }
}

/*===========================================================================
 * FUNCTION   : releaseOngoingPPData
 *
 * DESCRIPTION: callback function to release ongoing postprocess job node
 *
 * PARAMETERS :
 *   @data      : ptr to onging postprocess job
 *   @user_data : user data ptr (QCameraReprocessor)
 *
 * RETURN     : None
 *==========================================================================*/
void QCameraPostProcessor::releaseOngoingPPData(void *data, void *user_data)
{
    QCameraPostProcessor *pme = (QCameraPostProcessor *)user_data;
    if (NULL != pme) {
        qcamera_pp_data_t *pp_job = (qcamera_pp_data_t *)data;
        if (NULL != pp_job->src_frame) {
            pme->releaseSuperBuf(pp_job->src_frame);
            free(pp_job->src_frame);
            pp_job->src_frame = NULL;
        }
    }
}

/*===========================================================================
 * FUNCTION   : releaseNotifyData
 *
 * DESCRIPTION: function to release internal resources in notify data struct
 *
 * PARAMETERS :
 *   @user_data  : ptr user data
 *   @cookie     : callback cookie
 *   @cb_status  : callback status
 *
 * RETURN     : None
 *
 * NOTE       : deallocate jpeg heap memory if it's not NULL
 *==========================================================================*/
void QCameraPostProcessor::releaseNotifyData(void *user_data,
                                             void *cookie,
                                             int32_t cb_status)
{
    qcamera_data_argm_t *app_cb = ( qcamera_data_argm_t * ) user_data;
    QCameraPostProcessor *postProc = ( QCameraPostProcessor * ) cookie;
    if ( ( NULL != app_cb ) && ( NULL != postProc ) ) {

        if ( postProc->mUseSaveProc &&
             app_cb->release_data.unlinkFile &&
             ( NO_ERROR != cb_status ) ) {

            String8 unlinkPath((const char *) app_cb->release_data.data->data,
                                app_cb->release_data.data->size);
            int rc = unlink(unlinkPath.string());
            ALOGD("%s : Unlinking stored file rc = %d",
                  __func__,
                  rc);
        }

        if (app_cb && NULL != app_cb->release_data.data) {
            app_cb->release_data.data->release(app_cb->release_data.data);
            app_cb->release_data.data = NULL;
        }
        if (app_cb && NULL != app_cb->release_data.frame) {
            postProc->releaseSuperBuf(app_cb->release_data.frame);
            free(app_cb->release_data.frame);
            app_cb->release_data.frame = NULL;
        }
        if (app_cb && NULL != app_cb->release_data.streamBufs) {
            app_cb->release_data.streamBufs->deallocate();
            delete app_cb->release_data.streamBufs;
            app_cb->release_data.streamBufs = NULL;
        }
        free(app_cb);
    }
}

/*===========================================================================
 * FUNCTION   : releaseSuperBuf
 *
 * DESCRIPTION: function to release a superbuf frame by returning back to kernel
 *
 * PARAMETERS :
 *   @super_buf : ptr to the superbuf frame
 *
 * RETURN     : None
 *==========================================================================*/
void QCameraPostProcessor::releaseSuperBuf(mm_camera_super_buf_t *super_buf)
{
    QCameraChannel *pChannel = NULL;

    if (NULL != super_buf) {
        pChannel = m_parent->getChannelByHandle(super_buf->ch_id);

        if ( NULL == pChannel ) {
            if (m_pReprocChannel != NULL &&
                m_pReprocChannel->getMyHandle() == super_buf->ch_id) {
                pChannel = m_pReprocChannel;
            }
        }

        if (pChannel != NULL) {
            pChannel->bufDone(super_buf);
        } else {
            ALOGE(" %s : Channel id %d not found!!",
                  __func__,
                  super_buf->ch_id);
        }
    }
}

/*===========================================================================
 * FUNCTION   : releaseJpegJobData
 *
 * DESCRIPTION: function to release internal resources in jpeg job struct
 *
 * PARAMETERS :
 *   @job     : ptr to jpeg job struct
 *
 * RETURN     : None
 *
 * NOTE       : original source frame need to be queued back to kernel for
 *              future use. Output buf of jpeg job need to be released since
 *              it's allocated for each job. Exif object need to be deleted.
 *==========================================================================*/
void QCameraPostProcessor::releaseJpegJobData(qcamera_jpeg_data_t *job)
{
    ALOGV("%s: E", __func__);
    if (NULL != job) {
        if (NULL != job->src_reproc_frame) {
            releaseSuperBuf(job->src_reproc_frame);
            free(job->src_reproc_frame);
            job->src_reproc_frame = NULL;
        }

        if (NULL != job->src_frame) {
            releaseSuperBuf(job->src_frame);
            free(job->src_frame);
            job->src_frame = NULL;
        }

        if (NULL != job->pJpegExifObj) {
            delete job->pJpegExifObj;
            job->pJpegExifObj = NULL;
        }
    }
    ALOGV("%s: X", __func__);
}

/*===========================================================================
 * FUNCTION   : releaseSaveJobData
 *
 * DESCRIPTION: function to release internal resources in store jobs
 *
 * PARAMETERS :
 *   @job     : ptr to save job struct
 *
 * RETURN     : None
 *
 *==========================================================================*/
void QCameraPostProcessor::releaseSaveJobData(void *data, void *user_data)
{
    ALOGV("%s: E", __func__);

    QCameraPostProcessor *pme = (QCameraPostProcessor *) user_data;
    if (NULL == pme) {
        ALOGE("%s: Invalid postproc handle", __func__);
        return;
    }

    qcamera_jpeg_evt_payload_t *job_data = (qcamera_jpeg_evt_payload_t *) data;
    if (job_data == NULL) {
        ALOGE("%s: Invalid jpeg event data", __func__);
        return;
    }

    // find job by jobId
    qcamera_jpeg_data_t *job = pme->findJpegJobByJobId(job_data->jobId);

    if (NULL != job) {
        pme->releaseJpegJobData(job);
        free(job);
    } else {
        ALOGE("%s : Invalid jpeg job", __func__);
    }

    ALOGV("%s: X", __func__);
}

/*===========================================================================
 * FUNCTION   : releaseRawData
 *
 * DESCRIPTION: function to release internal resources in store jobs
 *
 * PARAMETERS :
 *   @job     : ptr to save job struct
 *
 * RETURN     : None
 *
 *==========================================================================*/
void QCameraPostProcessor::releaseRawData(void *data, void *user_data)
{
    ALOGV("%s: E", __func__);

    QCameraPostProcessor *pme = (QCameraPostProcessor *) user_data;
    if (NULL == pme) {
        ALOGE("%s: Invalid postproc handle", __func__);
        return;
    }
    mm_camera_super_buf_t *super_buf = (mm_camera_super_buf_t *) data;
    pme->releaseSuperBuf(super_buf);

    ALOGV("%s: X", __func__);
}


/*===========================================================================
 * FUNCTION   : getColorfmtFromImgFmt
 *
 * DESCRIPTION: function to return jpeg color format based on its image format
 *
 * PARAMETERS :
 *   @img_fmt : image format
 *
 * RETURN     : jpeg color format that can be understandable by omx lib
 *==========================================================================*/
mm_jpeg_color_format QCameraPostProcessor::getColorfmtFromImgFmt(cam_format_t img_fmt)
{
    switch (img_fmt) {
    case CAM_FORMAT_YUV_420_NV21:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V2;
    case CAM_FORMAT_YUV_420_NV21_ADRENO:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V2;
    case CAM_FORMAT_YUV_420_NV12:
        return MM_JPEG_COLOR_FORMAT_YCBCRLP_H2V2;
    case CAM_FORMAT_YUV_420_YV12:
        return MM_JPEG_COLOR_FORMAT_YCBCRLP_H2V2;
    case CAM_FORMAT_YUV_422_NV61:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V1;
    case CAM_FORMAT_YUV_422_NV16:
        return MM_JPEG_COLOR_FORMAT_YCBCRLP_H2V1;
    default:
        return MM_JPEG_COLOR_FORMAT_YCRCBLP_H2V2;
    }
}

/*===========================================================================
 * FUNCTION   : getJpegImgTypeFromImgFmt
 *
 * DESCRIPTION: function to return jpeg encode image type based on its image format
 *
 * PARAMETERS :
 *   @img_fmt : image format
 *
 * RETURN     : return jpeg source image format (YUV or Bitstream)
 *==========================================================================*/
mm_jpeg_format_t QCameraPostProcessor::getJpegImgTypeFromImgFmt(cam_format_t img_fmt)
{
    switch (img_fmt) {
    case CAM_FORMAT_YUV_420_NV21:
    case CAM_FORMAT_YUV_420_NV21_ADRENO:
    case CAM_FORMAT_YUV_420_NV12:
    case CAM_FORMAT_YUV_420_YV12:
    case CAM_FORMAT_YUV_422_NV61:
    case CAM_FORMAT_YUV_422_NV16:
        return MM_JPEG_FMT_YUV;
    default:
        return MM_JPEG_FMT_YUV;
    }
}

/*===========================================================================
 * FUNCTION   : encodeData
 *
 * DESCRIPTION: function to prepare encoding job information and send to
 *              mm-jpeg-interface to do the encoding job
 *
 * PARAMETERS :
 *   @jpeg_job_data : ptr to a struct saving job related information
 *   @needNewSess   : flag to indicate if a new jpeg encoding session need
 *                    to be created. After creation, this flag will be toggled
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::encodeData(qcamera_jpeg_data_t *jpeg_job_data,
                                         uint8_t &needNewSess)
{
    ALOGV("%s : E", __func__);
    int32_t ret = NO_ERROR;
    mm_jpeg_job_t jpg_job;
    uint32_t jobId = 0;
    QCameraStream *main_stream = NULL;
    mm_camera_buf_def_t *main_frame = NULL;
    QCameraStream *thumb_stream = NULL;
    mm_camera_buf_def_t *thumb_frame = NULL;
    mm_camera_super_buf_t *recvd_frame = jpeg_job_data->src_frame;

    // find channel
    QCameraChannel *pChannel = m_parent->getChannelByHandle(recvd_frame->ch_id);
    // check reprocess channel if not found
    if (pChannel == NULL) {
        if (m_pReprocChannel != NULL &&
            m_pReprocChannel->getMyHandle() == recvd_frame->ch_id) {
            pChannel = m_pReprocChannel;
        }
    }
    if (pChannel == NULL) {
        ALOGE("%s: No corresponding channel (ch_id = %d) exist, return here",
              __func__, recvd_frame->ch_id);
        return BAD_VALUE;
    }

    // find snapshot frame and thumnail frame
    for (int i = 0; i < recvd_frame->num_bufs; i++) {
        QCameraStream *pStream =
            pChannel->getStreamByHandle(recvd_frame->bufs[i]->stream_id);
        if (pStream != NULL) {
            if (pStream->isTypeOf(CAM_STREAM_TYPE_SNAPSHOT) ||
                pStream->isOrignalTypeOf(CAM_STREAM_TYPE_SNAPSHOT)) {
                main_stream = pStream;
                main_frame = recvd_frame->bufs[i];
            } else if (pStream->isTypeOf(CAM_STREAM_TYPE_PREVIEW) ||
                       pStream->isTypeOf(CAM_STREAM_TYPE_POSTVIEW) ||
                       pStream->isOrignalTypeOf(CAM_STREAM_TYPE_PREVIEW) ||
                       pStream->isOrignalTypeOf(CAM_STREAM_TYPE_POSTVIEW)) {
                thumb_stream = pStream;
                thumb_frame = recvd_frame->bufs[i];
            }
        }
    }

    if(NULL == main_frame){
       ALOGE("%s : Main frame is NULL", __func__);
       return BAD_VALUE;
    }

    //Sometimes thumbnail might be skipped in reprocess. Reprocessed channel will not have
    //thumbnail in such case.  Hence, look through reprocess source superbuf.
    if (thumb_frame == NULL && jpeg_job_data->src_reproc_frame != NULL) {
        mm_camera_super_buf_t *src_reproc_frame = jpeg_job_data->src_reproc_frame;
        QCameraChannel *pSrcReprocChannel = m_parent->getChannelByHandle(src_reproc_frame->ch_id);
        if (pSrcReprocChannel == NULL) {
            ALOGE("%s: No corresponding channel (ch_id = %d) exist, return here",
                __func__, src_reproc_frame->ch_id);
            return BAD_VALUE;
        }
        // find thumnail frame
        for (int i = 0; i < src_reproc_frame->num_bufs; i++) {
            QCameraStream *pStream =
                pSrcReprocChannel->getStreamByHandle(src_reproc_frame->bufs[i]->stream_id);
            if (pStream != NULL) {
                if (pStream->isTypeOf(CAM_STREAM_TYPE_PREVIEW) ||
                    pStream->isTypeOf(CAM_STREAM_TYPE_POSTVIEW)) {
                    thumb_stream = pStream;
                    thumb_frame = src_reproc_frame->bufs[i];
                }
            }
        }
    }

    if(NULL == thumb_frame){
       ALOGV("%s : Thumbnail frame does not exist", __func__);
    }

    QCameraMemory *memObj = (QCameraMemory *)main_frame->mem_info;
    if (NULL == memObj) {
        ALOGE("%s : Memeory Obj of main frame is NULL", __func__);
        return NO_MEMORY;
    }

    // dump snapshot frame if enabled
    m_parent->dumpFrameToFile(main_stream, main_frame, QCAMERA_DUMP_FRM_SNAPSHOT);

    // send upperlayer callback for raw image
    camera_memory_t *mem = memObj->getMemory(main_frame->buf_idx, false);
    if (NULL != m_parent->mDataCb &&
        m_parent->msgTypeEnabledWithLock(CAMERA_MSG_RAW_IMAGE) > 0) {
        qcamera_callback_argm_t cbArg;
        memset(&cbArg, 0, sizeof(qcamera_callback_argm_t));
        cbArg.cb_type = QCAMERA_DATA_CALLBACK;
        cbArg.msg_type = CAMERA_MSG_RAW_IMAGE;
        cbArg.data = mem;
        cbArg.index = 1;
        m_parent->m_cbNotifier.notifyCallback(cbArg);
    }
    if (NULL != m_parent->mNotifyCb &&
        m_parent->msgTypeEnabledWithLock(CAMERA_MSG_RAW_IMAGE_NOTIFY) > 0) {
        qcamera_callback_argm_t cbArg;
        memset(&cbArg, 0, sizeof(qcamera_callback_argm_t));
        cbArg.cb_type = QCAMERA_NOTIFY_CALLBACK;
        cbArg.msg_type = CAMERA_MSG_RAW_IMAGE_NOTIFY;
        cbArg.ext1 = 0;
        cbArg.ext2 = 0;
        m_parent->m_cbNotifier.notifyCallback(cbArg);
    }

    if (thumb_frame != NULL) {
        // dump thumbnail frame if enabled
        m_parent->dumpFrameToFile(thumb_stream, thumb_frame, QCAMERA_DUMP_FRM_THUMBNAIL);
    }

    if (mJpegClientHandle <= 0) {
        ALOGE("%s: Error: bug here, mJpegClientHandle is 0", __func__);
        return UNKNOWN_ERROR;
    }

    if (needNewSess) {
        // create jpeg encoding session
        mm_jpeg_encode_params_t encodeParam;
        memset(&encodeParam, 0, sizeof(mm_jpeg_encode_params_t));
        getJpegEncodingConfig(encodeParam, main_stream, thumb_stream);
        ALOGD("[KPI Perf] %s : call jpeg create_session", __func__);
        ret = mJpegHandle.create_session(mJpegClientHandle, &encodeParam, &mJpegSessionId);
        if (ret != NO_ERROR) {
            ALOGE("%s: error creating a new jpeg encoding session", __func__);
            return ret;
        }
        needNewSess = FALSE;
    }
    // Fill in new job
    memset(&jpg_job, 0, sizeof(mm_jpeg_job_t));
    jpg_job.job_type = JPEG_JOB_TYPE_ENCODE;
    jpg_job.encode_job.session_id = mJpegSessionId;
    jpg_job.encode_job.src_index = main_frame->buf_idx;
    jpg_job.encode_job.dst_index = 0;
    if (mUseJpegBurst) {
      jpg_job.encode_job.dst_index = -1;
    }

    cam_rect_t crop;
    memset(&crop, 0, sizeof(cam_rect_t));
    main_stream->getCropInfo(crop);

    cam_dimension_t src_dim;
    memset(&src_dim, 0, sizeof(cam_dimension_t));
    main_stream->getFrameDimension(src_dim);

    cam_dimension_t dst_dim;
    bool hdr_output_crop = m_parent->mParameters.isHDROutputCropEnabled();

    if (hdr_output_crop && crop.height) {
        dst_dim.height = crop.height;
    } else {
        dst_dim.height = src_dim.height;
    }
    if (hdr_output_crop && crop.width) {
        dst_dim.width = crop.width;
    } else {
        dst_dim.width = src_dim.width;
    }

    // main dim
    jpg_job.encode_job.main_dim.src_dim = src_dim;
    jpg_job.encode_job.main_dim.dst_dim = dst_dim;
    jpg_job.encode_job.main_dim.crop = crop;

    // get exif data
    QCameraExif *pJpegExifObj = m_parent->getExifData();
    jpeg_job_data->pJpegExifObj = pJpegExifObj;
    if (pJpegExifObj != NULL) {
        jpg_job.encode_job.exif_info.exif_data = pJpegExifObj->getEntries();
        jpg_job.encode_job.exif_info.numOfEntries =
            pJpegExifObj->getNumOfEntries();
    }

    // set rotation only when no online rotation or offline pp rotation is done before
    if (!m_parent->needRotationReprocess()) {
        jpg_job.encode_job.rotation = m_parent->getJpegRotation();
    }
    ALOGD("%s: jpeg rotation is set to %d", __func__, jpg_job.encode_job.rotation);

    // thumbnail dim
    if (m_bThumbnailNeeded == TRUE) {
        if (thumb_stream == NULL) {
            // need jpeg thumbnail, but no postview/preview stream exists
            // we use the main stream/frame to encode thumbnail
            thumb_stream = main_stream;
            thumb_frame = main_frame;
        }
        memset(&crop, 0, sizeof(cam_rect_t));
        thumb_stream->getCropInfo(crop);
        memset(&src_dim, 0, sizeof(cam_dimension_t));
        thumb_stream->getFrameDimension(src_dim);
        jpg_job.encode_job.thumb_dim.src_dim = src_dim;
        m_parent->getThumbnailSize(jpg_job.encode_job.thumb_dim.dst_dim);
        int rotation = m_parent->getJpegRotation();
        if ((rotation == 90 || rotation == 270)
            && jpg_job.encode_job.rotation == 0) {
            // swap dimension if rotation is 90 or 270,
            // and the rotation is already done in cpp.
            int32_t temp = jpg_job.encode_job.thumb_dim.dst_dim.height;
            jpg_job.encode_job.thumb_dim.dst_dim.height =
                jpg_job.encode_job.thumb_dim.dst_dim.width;
            jpg_job.encode_job.thumb_dim.dst_dim.width = temp;
        }
        jpg_job.encode_job.thumb_dim.crop = crop;
        jpg_job.encode_job.thumb_index = thumb_frame->buf_idx;
        ALOGD("%s, thumbnail src w/h (%dx%d), dst w/h (%dx%d)", __func__,
            jpg_job.encode_job.thumb_dim.src_dim.width,
            jpg_job.encode_job.thumb_dim.src_dim.height,
            jpg_job.encode_job.thumb_dim.dst_dim.width,
            jpg_job.encode_job.thumb_dim.dst_dim.height);
    }

    // find meta data frame
    mm_camera_buf_def_t *meta_frame = NULL;
    for (int i = 0; i < jpeg_job_data->src_frame->num_bufs; i++) {
        // look through input superbuf
        if (jpeg_job_data->src_frame->bufs[i]->stream_type == CAM_STREAM_TYPE_METADATA) {
            meta_frame = jpeg_job_data->src_frame->bufs[i];
            break;
        }
    }
    if (meta_frame == NULL && jpeg_job_data->src_reproc_frame != NULL) {
        // look through reprocess source superbuf
        for (int i = 0; i < jpeg_job_data->src_reproc_frame->num_bufs; i++) {
            if (jpeg_job_data->src_reproc_frame->bufs[i]->stream_type == CAM_STREAM_TYPE_METADATA) {
                meta_frame = jpeg_job_data->src_reproc_frame->bufs[i];
                break;
            }
        }
    }
    if (meta_frame != NULL) {
        // fill in meta data frame ptr
        jpg_job.encode_job.p_metadata = (metadata_buffer_t *)meta_frame->buffer;
    }

    jpg_job.encode_job.cam_exif_params = m_parent->mExifParams;

    ALOGE("[KPI Perf] %s : PROFILE_JPEG_JOB_START", __func__);
    ret = mJpegHandle.start_job(&jpg_job, &jobId);
    if (ret == NO_ERROR) {
        // remember job info
        jpeg_job_data->jobId = jobId;
    }

    return ret;
}

/*===========================================================================
 * FUNCTION   : processRawImageImpl
 *
 * DESCRIPTION: function to send raw image to upper layer
 *
 * PARAMETERS :
 *   @recvd_frame   : frame to be encoded
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::processRawImageImpl(mm_camera_super_buf_t *recvd_frame)
{
    int32_t rc = NO_ERROR;

    QCameraChannel *pChannel = m_parent->getChannelByHandle(recvd_frame->ch_id);
    QCameraStream *pStream = NULL;
    mm_camera_buf_def_t *frame = NULL;
    // check reprocess channel if not found
    if (pChannel == NULL) {
        if (m_pReprocChannel != NULL &&
            m_pReprocChannel->getMyHandle() == recvd_frame->ch_id) {
            pChannel = m_pReprocChannel;
        }
    }
    if (pChannel == NULL) {
        ALOGE("%s: No corresponding channel (ch_id = %d) exist, return here",
              __func__, recvd_frame->ch_id);
        return BAD_VALUE;
    }

    // find snapshot frame
    for (int i = 0; i < recvd_frame->num_bufs; i++) {
        QCameraStream *pCurStream =
            pChannel->getStreamByHandle(recvd_frame->bufs[i]->stream_id);
        if (pCurStream != NULL) {
            if (pCurStream->isTypeOf(CAM_STREAM_TYPE_SNAPSHOT) ||
                pCurStream->isTypeOf(CAM_STREAM_TYPE_RAW) ||
                pCurStream->isOrignalTypeOf(CAM_STREAM_TYPE_SNAPSHOT) ||
                pCurStream->isOrignalTypeOf(CAM_STREAM_TYPE_RAW)) {
                pStream = pCurStream;
                frame = recvd_frame->bufs[i];
                break;
            }
        }
    }

    if ( NULL == frame ) {
        ALOGE("%s: No valid raw buffer", __func__);
        return BAD_VALUE;
    }

    QCameraMemory *rawMemObj = (QCameraMemory *)frame->mem_info;
    camera_memory_t *raw_mem = NULL;

    if (rawMemObj != NULL) {
        raw_mem = rawMemObj->getMemory(frame->buf_idx, false);
    }

    if (NULL != rawMemObj && NULL != raw_mem) {
        // dump frame into file
        if (frame->stream_type == CAM_STREAM_TYPE_SNAPSHOT ||
            pStream->isOrignalTypeOf(CAM_STREAM_TYPE_SNAPSHOT)) {
            // for YUV422 NV16 case
            m_parent->dumpFrameToFile(pStream, frame, QCAMERA_DUMP_FRM_SNAPSHOT);
        } else {
            m_parent->dumpFrameToFile(pStream, frame, QCAMERA_DUMP_FRM_RAW);
        }

        // send data callback / notify for RAW_IMAGE
        if (NULL != m_parent->mDataCb &&
            m_parent->msgTypeEnabledWithLock(CAMERA_MSG_RAW_IMAGE) > 0) {
            qcamera_callback_argm_t cbArg;
            memset(&cbArg, 0, sizeof(qcamera_callback_argm_t));
            cbArg.cb_type = QCAMERA_DATA_CALLBACK;
            cbArg.msg_type = CAMERA_MSG_RAW_IMAGE;
            cbArg.data = raw_mem;
            cbArg.index = 0;
            m_parent->m_cbNotifier.notifyCallback(cbArg);
        }
        if (NULL != m_parent->mNotifyCb &&
            m_parent->msgTypeEnabledWithLock(CAMERA_MSG_RAW_IMAGE_NOTIFY) > 0) {
            qcamera_callback_argm_t cbArg;
            memset(&cbArg, 0, sizeof(qcamera_callback_argm_t));
            cbArg.cb_type = QCAMERA_NOTIFY_CALLBACK;
            cbArg.msg_type = CAMERA_MSG_RAW_IMAGE_NOTIFY;
            cbArg.ext1 = 0;
            cbArg.ext2 = 0;
            m_parent->m_cbNotifier.notifyCallback(cbArg);
        }

        bool zslChannelUsed = m_parent->isZSLMode() && ( pChannel != m_pReprocChannel );
        if ((m_parent->mDataCb != NULL) &&
            m_parent->msgTypeEnabledWithLock(CAMERA_MSG_COMPRESSED_IMAGE) > 0) {
            mRawBurstCount--;
            qcamera_release_data_t release_data;
            memset(&release_data, 0, sizeof(qcamera_release_data_t));
            if ( ( 0 == mRawBurstCount ) && (!zslChannelUsed) ) {
                release_data.streamBufs = rawMemObj;
                pStream->acquireStreamBufs();
            } else {
                release_data.frame = recvd_frame;
            }
            rc = sendDataNotify(CAMERA_MSG_COMPRESSED_IMAGE,
                                raw_mem,
                                0,
                                NULL,
                                &release_data);
        }
    } else {
        ALOGE("%s: Cannot get raw mem", __func__);
        rc = UNKNOWN_ERROR;
    }

    return rc;
}

/*===========================================================================
 * FUNCTION   : dataSaveRoutine
 *
 * DESCRIPTION: data saving routine
 *
 * PARAMETERS :
 *   @data    : user data ptr (QCameraPostProcessor)
 *
 * RETURN     : None
 *==========================================================================*/
void *QCameraPostProcessor::dataSaveRoutine(void *data)
{
    int running = 1;
    int ret;
    uint8_t is_active = FALSE;
    QCameraPostProcessor *pme = (QCameraPostProcessor *)data;
    QCameraCmdThread *cmdThread = &pme->m_saveProcTh;
    char saveName[PROPERTY_VALUE_MAX];

    ALOGD("%s: E", __func__);
    do {
        do {
            ret = cam_sem_wait(&cmdThread->cmd_sem);
            if (ret != 0 && errno != EINVAL) {
                ALOGE("%s: cam_sem_wait error (%s)",
                           __func__, strerror(errno));
                return NULL;
            }
        } while (ret != 0);

        // we got notified about new cmd avail in cmd queue
        camera_cmd_type_t cmd = cmdThread->getCmd();
        switch (cmd) {
        case CAMERA_CMD_TYPE_START_DATA_PROC:
            ALOGD("%s: start data proc", __func__);
            is_active = TRUE;
            break;
        case CAMERA_CMD_TYPE_STOP_DATA_PROC:
            {
                ALOGD("%s: stop data proc", __func__);
                is_active = FALSE;

                // flush input save Queue
                pme->m_inputSaveQ.flush();

                // signal cmd is completed
                cam_sem_post(&cmdThread->sync_sem);
            }
            break;
        case CAMERA_CMD_TYPE_DO_NEXT_JOB:
            {
                ALOGD("%s: Do next job, active is %d", __func__, is_active);

                qcamera_jpeg_evt_payload_t *job_data = (qcamera_jpeg_evt_payload_t *) pme->m_inputSaveQ.dequeue();
                if (job_data == NULL) {
                    ALOGE("%s: Invalid jpeg event data", __func__);
                    continue;
                }

                pme->m_ongoingJpegQ.flushNodes(matchJobId, (void*)&job_data->jobId);

                ALOGD("[KPI Perf] %s : jpeg job %d", __func__, job_data->jobId);

                if (is_active == TRUE) {
                    memset(saveName, '\0', sizeof(saveName));
                    snprintf(saveName,
                             sizeof(saveName),
                             QCameraPostProcessor::STORE_LOCATION,
                             pme->mSaveFrmCnt);

                    int file_fd = open(saveName, O_RDWR | O_CREAT, 0655);
                    if (file_fd > 0) {
                        size_t written_len = write(file_fd,
                                                job_data->out_data.buf_vaddr,
                                                job_data->out_data.buf_filled_len);
                        if ( job_data->out_data.buf_filled_len != written_len ) {
                            ALOGE("%s: Failed save complete data %d bytes written instead of %d bytes!",
                                  __func__,
                                  written_len,
                                  job_data->out_data.buf_filled_len);
                        } else {
                            ALOGD("%s: written number of bytes %d\n", __func__, written_len);
                        }

                        close(file_fd);
                    } else {
                        ALOGE("%s: fail t open file for saving", __func__);
                    }
                    pme->mSaveFrmCnt++;

                    camera_memory_t* jpeg_mem = pme->m_parent->mGetMemory(-1,
                                                         strlen(saveName),
                                                         1,
                                                         pme->m_parent->mCallbackCookie);
                    if (NULL == jpeg_mem) {
                        ret = NO_MEMORY;
                        ALOGE("%s : getMemory for jpeg, ret = NO_MEMORY", __func__);
                        goto end;
                    }
                    memcpy(jpeg_mem->data, saveName, strlen(saveName));

                    ALOGE("%s : Calling upperlayer callback to store JPEG image", __func__);
                    qcamera_release_data_t release_data;
                    memset(&release_data, 0, sizeof(qcamera_release_data_t));
                    release_data.data = jpeg_mem;
                    release_data.unlinkFile = true;
                    ALOGE("[KPI Perf] %s: PROFILE_JPEG_CB ",__func__);
                    ret = pme->sendDataNotify(CAMERA_MSG_COMPRESSED_IMAGE,
                                        jpeg_mem,
                                        0,
                                        NULL,
                                        &release_data);
                }

end:
                free(job_data);
            }
            break;
        case CAMERA_CMD_TYPE_EXIT:
            ALOGD("%s : save thread exit", __func__);
            running = 0;
            break;
        default:
            break;
        }
    } while (running);
    ALOGD("%s: X", __func__);
    return NULL;
}

/*===========================================================================
 * FUNCTION   : dataProcessRoutine
 *
 * DESCRIPTION: data process routine that handles input data either from input
 *              Jpeg Queue to do jpeg encoding, or from input PP Queue to do
 *              reprocess.
 *
 * PARAMETERS :
 *   @data    : user data ptr (QCameraPostProcessor)
 *
 * RETURN     : None
 *==========================================================================*/
void *QCameraPostProcessor::dataProcessRoutine(void *data)
{
    int running = 1;
    int ret;
    uint8_t is_active = FALSE;
    uint8_t needNewSess = TRUE;
    QCameraPostProcessor *pme = (QCameraPostProcessor *)data;
    QCameraCmdThread *cmdThread = &pme->m_dataProcTh;

    ALOGD("%s: E", __func__);
    do {
        do {
            ret = cam_sem_wait(&cmdThread->cmd_sem);
            if (ret != 0 && errno != EINVAL) {
                ALOGE("%s: cam_sem_wait error (%s)",
                           __func__, strerror(errno));
                return NULL;
            }
        } while (ret != 0);

        // we got notified about new cmd avail in cmd queue
        camera_cmd_type_t cmd = cmdThread->getCmd();
        switch (cmd) {
        case CAMERA_CMD_TYPE_START_DATA_PROC:
            ALOGD("%s: start data proc", __func__);
            is_active = TRUE;
            needNewSess = TRUE;
            pme->m_saveProcTh.sendCmd(CAMERA_CMD_TYPE_START_DATA_PROC,
                                      FALSE,
                                      FALSE);
            break;
        case CAMERA_CMD_TYPE_STOP_DATA_PROC:
            {
                ALOGD("%s: stop data proc", __func__);
                is_active = FALSE;

                pme->m_saveProcTh.sendCmd(CAMERA_CMD_TYPE_STOP_DATA_PROC,
                                           TRUE,
                                           TRUE);
                // cancel all ongoing jpeg jobs
                qcamera_jpeg_data_t *jpeg_job =
                    (qcamera_jpeg_data_t *)pme->m_ongoingJpegQ.dequeue();
                while (jpeg_job != NULL) {
                    pme->mJpegHandle.abort_job(jpeg_job->jobId);

                    pme->releaseJpegJobData(jpeg_job);
                    free(jpeg_job);

                    jpeg_job = (qcamera_jpeg_data_t *)pme->m_ongoingJpegQ.dequeue();
                }

                // destroy jpeg encoding session
                if ( 0 < pme->mJpegSessionId ) {
                    pme->mJpegHandle.destroy_session(pme->mJpegSessionId);
                    pme->mJpegSessionId = 0;
                }

                // free jpeg out buf and exif obj
                if (pme->m_pJpegOutputMem != NULL) {
                    pme->m_pJpegOutputMem->deallocate();
                    delete pme->m_pJpegOutputMem;
                    pme->m_pJpegOutputMem = NULL;
                }
                if (pme->m_pJpegExifObj != NULL) {
                    delete pme->m_pJpegExifObj;
                    pme->m_pJpegExifObj = NULL;
                }
                needNewSess = TRUE;

                // stop reproc channel if exists
                if (pme->m_pReprocChannel != NULL) {
                    pme->m_pReprocChannel->stop();
                    delete pme->m_pReprocChannel;
                    pme->m_pReprocChannel = NULL;
                }

                // flush ongoing postproc Queue
                pme->m_ongoingPPQ.flush();

                // flush input jpeg Queue
                pme->m_inputJpegQ.flush();

                // flush input Postproc Queue
                pme->m_inputPPQ.flush();

                // flush input raw Queue
                pme->m_inputRawQ.flush();

                // signal cmd is completed
                cam_sem_post(&cmdThread->sync_sem);
            }
            break;
        case CAMERA_CMD_TYPE_DO_NEXT_JOB:
            {
                ALOGD("%s: Do next job, active is %d", __func__, is_active);
                if (is_active == TRUE) {
                    qcamera_jpeg_data_t *jpeg_job =
                        (qcamera_jpeg_data_t *)pme->m_inputJpegQ.dequeue();

                    if (NULL != jpeg_job) {
                      // add into ongoing jpeg job Q
                      pme->m_ongoingJpegQ.enqueue((void *)jpeg_job);
                      ret = pme->encodeData(jpeg_job, needNewSess);
                      if (NO_ERROR != ret) {
                        // dequeue the last one
                        pme->m_ongoingJpegQ.dequeue(false);
                        pme->releaseJpegJobData(jpeg_job);
                        free(jpeg_job);
                        pme->sendEvtNotify(CAMERA_MSG_ERROR, UNKNOWN_ERROR, 0);
                      }
                    }


                    // process raw data if any
                    mm_camera_super_buf_t *super_buf =
                        (mm_camera_super_buf_t *)pme->m_inputRawQ.dequeue();

                    if (NULL != super_buf) {
                        //play shutter sound
                        pme->m_parent->playShutter();
                        ret = pme->processRawImageImpl(super_buf);
                        if (NO_ERROR != ret) {
                            pme->releaseSuperBuf(super_buf);
                            free(super_buf);
                            pme->sendEvtNotify(CAMERA_MSG_ERROR, UNKNOWN_ERROR, 0);
                        }
                    }

                    mm_camera_super_buf_t *pp_frame =
                        (mm_camera_super_buf_t *)pme->m_inputPPQ.dequeue();
                    if (NULL != pp_frame) {
                        qcamera_pp_data_t *pp_job =
                            (qcamera_pp_data_t *)malloc(sizeof(qcamera_pp_data_t));
                        if (pp_job != NULL) {
                            memset(pp_job, 0, sizeof(qcamera_pp_data_t));
                            if (pme->m_pReprocChannel != NULL) {
                                // add into ongoing PP job Q
                                pp_job->src_frame = pp_frame;
                                pme->m_ongoingPPQ.enqueue((void *)pp_job);
                                ret = pme->m_pReprocChannel->doReprocess(pp_frame);
                                if (NO_ERROR != ret) {
                                    // remove from ongoing PP job Q
                                    pme->m_ongoingPPQ.dequeue(false);
                                }
                            } else {
                                ALOGE("%s: Reprocess channel is NULL", __func__);
                                ret = -1;
                            }
                        } else {
                            ALOGE("%s: no mem for qcamera_pp_data_t", __func__);
                            ret = -1;
                        }

                        if (0 != ret) {
                            // free pp_job
                            if (pp_job != NULL) {
                                free(pp_job);
                            }
                            // free frame
                            if (pp_frame != NULL) {
                                pme->releaseSuperBuf(pp_frame);
                                free(pp_frame);
                            }
                            // send error notify
                            pme->sendEvtNotify(CAMERA_MSG_ERROR, UNKNOWN_ERROR, 0);
                        }
                    }
                } else {
                    // not active, simply return buf and do no op
                    qcamera_jpeg_data_t *jpeg_data =
                        (qcamera_jpeg_data_t *)pme->m_inputJpegQ.dequeue();
                    if (NULL != jpeg_data) {
                        pme->releaseJpegJobData(jpeg_data);
                        free(jpeg_data);
                    }
                    mm_camera_super_buf_t *super_buf =
                        (mm_camera_super_buf_t *)pme->m_inputRawQ.dequeue();
                    if (NULL != super_buf) {
                        pme->releaseSuperBuf(super_buf);
                        free(super_buf);
                    }
                    super_buf = (mm_camera_super_buf_t *)pme->m_inputPPQ.dequeue();
                    if (NULL != super_buf) {
                        pme->releaseSuperBuf(super_buf);
                        free(super_buf);
                    }
                }
            }
            break;
        case CAMERA_CMD_TYPE_EXIT:
            running = 0;
            break;
        default:
            break;
        }
    } while (running);
    ALOGD("%s: X", __func__);
    return NULL;
}

/*===========================================================================
 * FUNCTION   : getJpegPaddingReq
 *
 * DESCRIPTION: function to add an entry to exif data
 *
 * PARAMETERS :
 *   @padding_info : jpeg specific padding requirement
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraPostProcessor::getJpegPaddingReq(cam_padding_info_t &padding_info)
{
    // TODO: hardcode for now, needs to query from mm-jpeg-interface
    padding_info.width_padding  = CAM_PAD_NONE;
    padding_info.height_padding  = CAM_PAD_TO_16;
    padding_info.plane_padding  = CAM_PAD_TO_WORD;
    return NO_ERROR;
}

/*===========================================================================
 * FUNCTION   : setYUVFrameInfo
 *
 * DESCRIPTION: set Raw YUV frame data info for up-layer
 *
 * PARAMETERS :
 *   @frame   : process frame received from mm-camera-interface
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *
 * NOTE       : currently we return frame len, y offset, cbcr offset and frame format
 *==========================================================================*/
int32_t QCameraPostProcessor::setYUVFrameInfo(mm_camera_super_buf_t *recvd_frame)
{
    QCameraChannel *pChannel = m_parent->getChannelByHandle(recvd_frame->ch_id);
    // check reprocess channel if not found
    if (pChannel == NULL) {
        if (m_pReprocChannel != NULL &&
            m_pReprocChannel->getMyHandle() == recvd_frame->ch_id) {
            pChannel = m_pReprocChannel;
        }
    }

    if (pChannel == NULL) {
        ALOGE("%s: No corresponding channel (ch_id = %d) exist, return here",
              __func__, recvd_frame->ch_id);
        return BAD_VALUE;
    }

    // find snapshot frame
    for (int i = 0; i < recvd_frame->num_bufs; i++) {
        QCameraStream *pStream =
            pChannel->getStreamByHandle(recvd_frame->bufs[i]->stream_id);
        if (pStream != NULL) {
            if (pStream->isTypeOf(CAM_STREAM_TYPE_SNAPSHOT) ||
                pStream->isOrignalTypeOf(CAM_STREAM_TYPE_SNAPSHOT)) {
                //get the main frame, use stream info
                cam_frame_len_offset_t frame_offset;
                cam_dimension_t frame_dim;
                cam_format_t frame_fmt;
                const char *fmt_string;
                pStream->getFrameDimension(frame_dim);
                pStream->getFrameOffset(frame_offset);
                pStream->getFormat(frame_fmt);
                fmt_string = m_parent->mParameters.getFrameFmtString(frame_fmt);

                int cbcr_offset = frame_offset.mp[0].len - frame_dim.width * frame_dim.height;
                m_parent->mParameters.set("snapshot-framelen", frame_offset.frame_len);
                m_parent->mParameters.set("snapshot-yoff", frame_offset.mp[0].offset);
                m_parent->mParameters.set("snapshot-cbcroff", cbcr_offset);
                if(fmt_string != NULL){
                    m_parent->mParameters.set("snapshot-format", fmt_string);
                }else{
                    m_parent->mParameters.set("snapshot-format", "");
                }

                ALOGD("%s: frame width=%d, height=%d, yoff=%d, cbcroff=%d, fmt_string=%s", __func__,
                        frame_dim.width, frame_dim.height, frame_offset.mp[0].offset, cbcr_offset, fmt_string);
                return NO_ERROR;
            }
        }
    }

    return BAD_VALUE;
}

bool QCameraPostProcessor::matchJobId(void *data, void *, void *match_data)
{
  qcamera_jpeg_data_t * job = (qcamera_jpeg_data_t *) data;
  uint32_t job_id = *((uint32_t *) match_data);
  return job->jobId == job_id;
}


/*===========================================================================
 * FUNCTION   : QCameraExif
 *
 * DESCRIPTION: constructor of QCameraExif
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraExif::QCameraExif()
    : m_nNumEntries(0)
{
    memset(m_Entries, 0, sizeof(m_Entries));
}

/*===========================================================================
 * FUNCTION   : ~QCameraExif
 *
 * DESCRIPTION: deconstructor of QCameraExif. Will release internal memory ptr.
 *
 * PARAMETERS : None
 *
 * RETURN     : None
 *==========================================================================*/
QCameraExif::~QCameraExif()
{
    for (uint32_t i = 0; i < m_nNumEntries; i++) {
        switch (m_Entries[i].tag_entry.type) {
        case EXIF_BYTE:
            {
                if (m_Entries[i].tag_entry.count > 1 &&
                    m_Entries[i].tag_entry.data._bytes != NULL) {
                    free(m_Entries[i].tag_entry.data._bytes);
                    m_Entries[i].tag_entry.data._bytes = NULL;
                }
            }
            break;
        case EXIF_ASCII:
            {
                if (m_Entries[i].tag_entry.data._ascii != NULL) {
                    free(m_Entries[i].tag_entry.data._ascii);
                    m_Entries[i].tag_entry.data._ascii = NULL;
                }
            }
            break;
        case EXIF_SHORT:
            {
                if (m_Entries[i].tag_entry.count > 1 &&
                    m_Entries[i].tag_entry.data._shorts != NULL) {
                    free(m_Entries[i].tag_entry.data._shorts);
                    m_Entries[i].tag_entry.data._shorts = NULL;
                }
            }
            break;
        case EXIF_LONG:
            {
                if (m_Entries[i].tag_entry.count > 1 &&
                    m_Entries[i].tag_entry.data._longs != NULL) {
                    free(m_Entries[i].tag_entry.data._longs);
                    m_Entries[i].tag_entry.data._longs = NULL;
                }
            }
            break;
        case EXIF_RATIONAL:
            {
                if (m_Entries[i].tag_entry.count > 1 &&
                    m_Entries[i].tag_entry.data._rats != NULL) {
                    free(m_Entries[i].tag_entry.data._rats);
                    m_Entries[i].tag_entry.data._rats = NULL;
                }
            }
            break;
        case EXIF_UNDEFINED:
            {
                if (m_Entries[i].tag_entry.data._undefined != NULL) {
                    free(m_Entries[i].tag_entry.data._undefined);
                    m_Entries[i].tag_entry.data._undefined = NULL;
                }
            }
            break;
        case EXIF_SLONG:
            {
                if (m_Entries[i].tag_entry.count > 1 &&
                    m_Entries[i].tag_entry.data._slongs != NULL) {
                    free(m_Entries[i].tag_entry.data._slongs);
                    m_Entries[i].tag_entry.data._slongs = NULL;
                }
            }
            break;
        case EXIF_SRATIONAL:
            {
                if (m_Entries[i].tag_entry.count > 1 &&
                    m_Entries[i].tag_entry.data._srats != NULL) {
                    free(m_Entries[i].tag_entry.data._srats);
                    m_Entries[i].tag_entry.data._srats = NULL;
                }
            }
            break;
        }
    }
}

/*===========================================================================
 * FUNCTION   : addEntry
 *
 * DESCRIPTION: function to add an entry to exif data
 *
 * PARAMETERS :
 *   @tagid   : exif tag ID
 *   @type    : data type
 *   @count   : number of data in uint of its type
 *   @data    : input data ptr
 *
 * RETURN     : int32_t type of status
 *              NO_ERROR  -- success
 *              none-zero failure code
 *==========================================================================*/
int32_t QCameraExif::addEntry(exif_tag_id_t tagid,
                              exif_tag_type_t type,
                              uint32_t count,
                              void *data)
{
    int32_t rc = NO_ERROR;
    if(m_nNumEntries >= MAX_EXIF_TABLE_ENTRIES) {
        ALOGE("%s: Number of entries exceeded limit", __func__);
        return NO_MEMORY;
    }

    m_Entries[m_nNumEntries].tag_id = tagid;
    m_Entries[m_nNumEntries].tag_entry.type = type;
    m_Entries[m_nNumEntries].tag_entry.count = count;
    m_Entries[m_nNumEntries].tag_entry.copy = 1;
    switch (type) {
    case EXIF_BYTE:
        {
            if (count > 1) {
                uint8_t *values = (uint8_t *)malloc(count);
                if (values == NULL) {
                    ALOGE("%s: No memory for byte array", __func__);
                    rc = NO_MEMORY;
                } else {
                    memcpy(values, data, count);
                    m_Entries[m_nNumEntries].tag_entry.data._bytes = values;
                }
            } else {
                m_Entries[m_nNumEntries].tag_entry.data._byte = *(uint8_t *)data;
            }
        }
        break;
    case EXIF_ASCII:
        {
            char *str = NULL;
            str = (char *)malloc(count + 1);
            if (str == NULL) {
                ALOGE("%s: No memory for ascii string", __func__);
                rc = NO_MEMORY;
            } else {
                memset(str, 0, count + 1);
                memcpy(str, data, count);
                m_Entries[m_nNumEntries].tag_entry.data._ascii = str;
            }
        }
        break;
    case EXIF_SHORT:
        {
            if (count > 1) {
                uint16_t *values = (uint16_t *)malloc(count * sizeof(uint16_t));
                if (values == NULL) {
                    ALOGE("%s: No memory for short array", __func__);
                    rc = NO_MEMORY;
                } else {
                    memcpy(values, data, count * sizeof(uint16_t));
                    m_Entries[m_nNumEntries].tag_entry.data._shorts = values;
                }
            } else {
                m_Entries[m_nNumEntries].tag_entry.data._short = *(uint16_t *)data;
            }
        }
        break;
    case EXIF_LONG:
        {
            if (count > 1) {
                uint32_t *values = (uint32_t *)malloc(count * sizeof(uint32_t));
                if (values == NULL) {
                    ALOGE("%s: No memory for long array", __func__);
                    rc = NO_MEMORY;
                } else {
                    memcpy(values, data, count * sizeof(uint32_t));
                    m_Entries[m_nNumEntries].tag_entry.data._longs = values;
                }
            } else {
                m_Entries[m_nNumEntries].tag_entry.data._long = *(uint32_t *)data;
            }
        }
        break;
    case EXIF_RATIONAL:
        {
            if (count > 1) {
                rat_t *values = (rat_t *)malloc(count * sizeof(rat_t));
                if (values == NULL) {
                    ALOGE("%s: No memory for rational array", __func__);
                    rc = NO_MEMORY;
                } else {
                    memcpy(values, data, count * sizeof(rat_t));
                    m_Entries[m_nNumEntries].tag_entry.data._rats = values;
                }
            } else {
                m_Entries[m_nNumEntries].tag_entry.data._rat = *(rat_t *)data;
            }
        }
        break;
    case EXIF_UNDEFINED:
        {
            uint8_t *values = (uint8_t *)malloc(count);
            if (values == NULL) {
                ALOGE("%s: No memory for undefined array", __func__);
                rc = NO_MEMORY;
            } else {
                memcpy(values, data, count);
                m_Entries[m_nNumEntries].tag_entry.data._undefined = values;
            }
        }
        break;
    case EXIF_SLONG:
        {
            if (count > 1) {
                int32_t *values = (int32_t *)malloc(count * sizeof(int32_t));
                if (values == NULL) {
                    ALOGE("%s: No memory for signed long array", __func__);
                    rc = NO_MEMORY;
                } else {
                    memcpy(values, data, count * sizeof(int32_t));
                    m_Entries[m_nNumEntries].tag_entry.data._slongs = values;
                }
            } else {
                m_Entries[m_nNumEntries].tag_entry.data._slong = *(int32_t *)data;
            }
        }
        break;
    case EXIF_SRATIONAL:
        {
            if (count > 1) {
                srat_t *values = (srat_t *)malloc(count * sizeof(srat_t));
                if (values == NULL) {
                    ALOGE("%s: No memory for signed rational array", __func__);
                    rc = NO_MEMORY;
                } else {
                    memcpy(values, data, count * sizeof(srat_t));
                    m_Entries[m_nNumEntries].tag_entry.data._srats = values;
                }
            } else {
                m_Entries[m_nNumEntries].tag_entry.data._srat = *(srat_t *)data;
            }
        }
        break;
    }

    // Increase number of entries
    m_nNumEntries++;
    return rc;
}

}; // namespace qcamera
