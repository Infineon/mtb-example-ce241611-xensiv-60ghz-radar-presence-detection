/*******************************************************************************
* \file xensiv_radar_presence.c
*
* \brief
* Provides API implementation for the RADAR presence detection library.
*
********************************************************************************
* \copyright
* (c) (2022), Infineon Technologies AG.
*
* SPDX-License-Identifier: Apache-2.0
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <inttypes.h>

#include "xensiv_radar_presence.h"

/************************************** Macros *******************************************/
#define RADAR_PRESENCE_MAX_RANGE_LIMIT_M    (5.0F)
#define RADAR_PRESENCE_DECIMATION_NUMTAPS   (129)
#define RADAR_PRESENCE_DECIMATION_FACTOR    (8)
#define RADAR_PRESENCE_BANDPASS_NUMTAPS     (65)
#define RADAR_PRESENCE_BANDPASS_BLOCKSIZE   (1)
#define RADAR_PRESENCE_BANDPASS_DELAY       (490U)

/**
 * @brief XENSIV(TM) RADAR presence detection algorithm context
 */
struct xensiv_radar_presence_context
{
    xensiv_radar_presence_config_t config;

    int32_t switches;

    float32_t* macro_fft_win;
    float32_t* range_intensity_win;
    cfloat32_t* macro_fft_buffer;
    cfloat32_t* last_macro_compare;

    arm_cfft_instance_f32 doppler_fft;

    cfloat32_t* bandpass_macro_fft_buffer;
    arm_fir_instance_f32* macro_fft_bandpass_fir_re_instances;
    arm_fir_instance_f32* macro_fft_bandpass_fir_im_instances;
    float32_t* macro_fft_bandpass_fir_re_states;
    float32_t* macro_fft_bandpass_fir_im_states;

    cfloat32_t* micro_fft_buffer;
    cfloat32_t* micro_fft_col_buffer;
    int32_t micro_fft_write_row_idx;
    int32_t micro_fft_calc_col_idx;

    int32_t micro_fft_decimation_write_row_idx;
    cfloat32_t* micro_fft_decimation_buffer;
    arm_fir_decimate_instance_f32* micro_fft_decimation_re_instances;
    arm_fir_decimate_instance_f32* micro_fft_decimation_im_instances;
    float32_t* micro_fft_decimation_re_states;
    float32_t* micro_fft_decimation_im_states;

    XENSIV_RADAR_PRESENCE_TIMESTAMP macro_last_compare_ms;

    float32_t max_macro;
    int32_t max_macro_idx;
    int32_t last_macro_reported_idx;

    float32_t max_micro;
    int32_t max_micro_idx;
    int32_t last_micro_reported_idx;

    int16_t max_micro_fft_size; // will be set to micro fft size at time of init (buffers allocated
                                // then)

    XENSIV_RADAR_PRESENCE_TIMESTAMP* macro_detect_timestamps;
    XENSIV_RADAR_PRESENCE_TIMESTAMP* micro_detect_timestamps;
    float32_t* macro_detect_confidences;
    float32_t* micro_detect_distances;

    XENSIV_RADAR_PRESENCE_TIMESTAMP last_data_timestamp_ms;
    int32_t last_reported_idx;

    XENSIV_RADAR_PRESENCE_TIMESTAMP bandpass_initial_time_ms;
    int32_t macro_movement_hit_count;

    bool micro_fft_calc_ready;
    bool micro_fft_all_calculated;

    xensiv_radar_presence_cb_t callback;
    void* callback_data;

    xensiv_radar_presence_state_t state;
    bool macro_last_compare_init;

    /* Constants calculated during initialization */
    int32_t max_range_limit_idx;
    int16_t macro_fft_size;
};

/********************************* Local variables ***************************************/
static void* (* malloc_fptr)(size_t size) = malloc;
static void (* free_fptr)(void* ptr) = free;
static const int32_t macro_fft_bandpass_fir_state_size = RADAR_PRESENCE_BANDPASS_NUMTAPS +
                                                         RADAR_PRESENCE_BANDPASS_BLOCKSIZE - 1;

/***************************** Local function prototypes *********************************/
static void* mem_alloc(size_t n);
static void* mem_calloc(size_t n);
static void mem_free(void* ptr);
static void switch_to_absence(const xensiv_radar_presence_handle_t handle,
                              XENSIV_RADAR_PRESENCE_TIMESTAMP time_ms);

/***************************** Function implementation ***********************************/

void xensiv_radar_presence_set_malloc_free(void* (*malloc_func)(size_t size),
                                           void (* free_func)(void* ptr))
{
    assert((malloc_func != NULL) && (free_func != NULL));

    malloc_fptr = malloc_func;
    free_fptr = free_func;
}


void xensiv_radar_presence_set_callback(xensiv_radar_presence_handle_t handle,
                                        xensiv_radar_presence_cb_t callback,
                                        void* data)
{
    assert(handle != NULL);

    handle->callback = callback;
    handle->callback_data = data;
}


void xensiv_radar_presence_init_config(xensiv_radar_presence_config_t* config)
{
    config->bandwidth                         = 460E6f;
    config->num_samples_per_chirp             = 128;
    config->micro_fft_decimation_enabled      = false;
    config->micro_fft_size                    = 128;
    config->macro_threshold                   = 1.0f;
    config->micro_threshold                   = 25.0f;
    config->min_range_bin                     = 1;
    config->max_range_bin                     = 5;
    config->macro_compare_interval_ms         = 250;
    config->macro_movement_validity_ms        = 1000;
    config->micro_movement_validity_ms        = 4000;
    config->macro_movement_confirmations      = 0;
    config->macro_trigger_range               = 1;
    config->mode                              = XENSIV_RADAR_PRESENCE_MODE_MICRO_IF_MACRO;
    config->macro_fft_bandpass_filter_enabled = false;
    config->micro_movement_compare_idx        = 5;
}


int32_t xensiv_radar_presence_alloc(xensiv_radar_presence_handle_t* handle,
                                    const xensiv_radar_presence_config_t* config)
{
    assert(handle != NULL);
    assert(config != NULL);

    // Low pass (5Hz) FIR Coefficients buffer generated using fir1() MATLAB function.
    // Test matlab code as below.
    /*
       x = [0.1953125:0.1953125:100];
       h = fir1(128, 5/100);
       fft = 20 * log10(abs(freqz(h, 1, 512)'));
       plot(x, fft);
     */
    static const float32_t arm_fir_decimate_pCoeffs[RADAR_PRESENCE_DECIMATION_NUMTAPS] =
    {
        -0.0002335706f, -0.0001845369f, -0.0001302661f, -0.0000692792f, +0.0000000000f,
        +0.0000790508f, +0.0001690467f,
        +0.0002706434f, +0.0003837746f, +0.0005074704f, +0.0006397080f, +0.0007773074f,
        +0.0009158812f, +0.0010498472f,
        +0.0011725089f, +0.0012762062f, +0.0013525367f, +0.0013926445f, +0.0013875686f,
        +0.0013286427f, +0.0012079324f,
        +0.0010186962f, +0.0007558520f, +0.0004164310f, +0.0000000000f, -0.0004909674f,
        -0.0010507895f, -0.0016703624f,
        -0.0023370475f, -0.0030346730f, -0.0037436590f, -0.0044412689f, -0.0051019897f,
        -0.0056980354f, -0.0061999662f,
        -0.0065774088f, -0.0067998622f, -0.0068375662f, -0.0066624096f, -0.0062488501f,
        -0.0055748192f, -0.0046225811f,
        -0.0033795172f, -0.0018388104f, +0.0000000000f, +0.0021306116f, +0.0045397210f,
        +0.0072069682f, +0.0101050712f,
        +0.0132001547f, +0.0164522689f, +0.0198160911f, +0.0232417935f, +0.0266760581f,
        +0.0300632143f, +0.0333464689f,
        +0.0364691958f, +0.0393762517f, +0.0420152803f, +0.0443379694f, +0.0463012239f,
        +0.0478682239f, +0.0490093339f,
        +0.0497028404f, +0.0499354938f, +0.0497028404f, +0.0490093339f, +0.0478682239f,
        +0.0463012239f, +0.0443379694f,
        +0.0420152803f, +0.0393762517f, +0.0364691958f, +0.0333464689f, +0.0300632143f,
        +0.0266760581f, +0.0232417935f,
        +0.0198160911f, +0.0164522689f, +0.0132001547f, +0.0101050712f, +0.0072069682f,
        +0.0045397210f, +0.0021306116f,
        +0.0000000000f, -0.0018388104f, -0.0033795172f, -0.0046225811f, -0.0055748192f,
        -0.0062488501f, -0.0066624096f,
        -0.0068375662f, -0.0067998622f, -0.0065774088f, -0.0061999662f, -0.0056980354f,
        -0.0051019897f, -0.0044412689f,
        -0.0037436590f, -0.0030346730f, -0.0023370475f, -0.0016703624f, -0.0010507895f,
        -0.0004909674f, +0.0000000000f,
        +0.0004164310f, +0.0007558520f, +0.0010186962f, +0.0012079324f, +0.0013286427f,
        +0.0013875686f, +0.0013926445f,
        +0.0013525367f, +0.0012762062f, +0.0011725089f, +0.0010498472f, +0.0009158812f,
        +0.0007773074f, +0.0006397080f,
        +0.0005074704f, +0.0003837746f, +0.0002706434f, +0.0001690467f, +0.0000790508f,
        +0.0000000000f, -0.0000692792f,
        -0.0001302661f, -0.0001845369f, -0.0002335706f
    };

    *handle = NULL;

    const size_t context_sz = sizeof(struct xensiv_radar_presence_context);
    struct xensiv_radar_presence_context* context =
        (struct xensiv_radar_presence_context*)mem_calloc(context_sz);
    if (context == NULL)
    {
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    /* Check if the num_samples setting is valid
       rfft is not used later, local var only for testing */
    arm_rfft_fast_instance_f32 rfft;
    if (arm_rfft_fast_init_f32(&rfft, (uint16_t)config->num_samples_per_chirp) != ARM_MATH_SUCCESS)
    {
        mem_free(context);
        return XENSIV_RADAR_PRESENCE_FFT_LEN_ERROR;
    }

    if (arm_cfft_init_f32(&context->doppler_fft, (uint16_t)config->micro_fft_size) !=
        ARM_MATH_SUCCESS)
    {
        mem_free(context);
        return XENSIV_RADAR_PRESENCE_FFT_LEN_ERROR;
    }

    (void)memcpy(&context->config, config, sizeof(context->config));

    context->macro_fft_size = config->num_samples_per_chirp / 2;
    context->max_range_limit_idx =
        (int32_t)floorf(RADAR_PRESENCE_MAX_RANGE_LIMIT_M / ifx_range_resolution(config->bandwidth));

    /* Allocate space for window function data */
    const size_t macro_fft_win_sz = (size_t)config->num_samples_per_chirp *
                                    sizeof(float32_t);
    context->macro_fft_win = mem_alloc(macro_fft_win_sz);
    if (context->macro_fft_win == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }
    ifx_window_hamming_f32(context->macro_fft_win, (uint32_t)config->num_samples_per_chirp);

    /* Allocate space for current and previous macro real FFT (N real samples -> N/2 complex
       coefficients) */
    const size_t macro_fft_buffer_sz = (size_t)context->macro_fft_size * sizeof(cfloat32_t);
    context->macro_fft_buffer = (cfloat32_t*)mem_alloc(macro_fft_buffer_sz);
    context->last_macro_compare = (cfloat32_t*)mem_alloc(macro_fft_buffer_sz);
    if ((context->macro_fft_buffer == NULL) || (context->last_macro_compare == NULL))
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    context->bandpass_macro_fft_buffer = (cfloat32_t*)mem_alloc(macro_fft_buffer_sz);
    if (context->bandpass_macro_fft_buffer == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t macro_fft_bandpass_fir_instances = sizeof(arm_fir_instance_f32) *
                                                    (size_t)context->max_range_limit_idx;
    context->macro_fft_bandpass_fir_re_instances =
        (arm_fir_instance_f32*)mem_alloc(macro_fft_bandpass_fir_instances);
    if (context->macro_fft_bandpass_fir_re_instances == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    context->macro_fft_bandpass_fir_im_instances =
        (arm_fir_instance_f32*)mem_alloc(macro_fft_bandpass_fir_instances);
    if (context->macro_fft_bandpass_fir_im_instances == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t macro_fft_bandpass_fir_states_sz = sizeof(float32_t) *
                                                    (size_t)macro_fft_bandpass_fir_state_size *
                                                    (size_t)context->max_range_limit_idx;
    context->macro_fft_bandpass_fir_re_states =
        (float32_t*)mem_alloc(macro_fft_bandpass_fir_states_sz);
    if (context->macro_fft_bandpass_fir_re_states == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    context->macro_fft_bandpass_fir_im_states =
        (float32_t*)mem_alloc(macro_fft_bandpass_fir_states_sz);
    if (context->macro_fft_bandpass_fir_im_states == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t micro_fft_decimation_buffer_sz = sizeof(cfloat32_t) *
                                                  (size_t)context->max_range_limit_idx *
                                                  (size_t)RADAR_PRESENCE_DECIMATION_FACTOR;
    context->micro_fft_decimation_buffer = (cfloat32_t*)mem_alloc(micro_fft_decimation_buffer_sz);
    if (context->micro_fft_decimation_buffer == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t micro_fft_decimation_instances_sz = sizeof(arm_fir_decimate_instance_f32) *
                                                     (size_t)context->max_range_limit_idx;
    context->micro_fft_decimation_re_instances =
        (arm_fir_decimate_instance_f32*)mem_alloc(micro_fft_decimation_instances_sz);

    context->micro_fft_decimation_im_instances =
        (arm_fir_decimate_instance_f32*)mem_alloc(micro_fft_decimation_instances_sz);

    if ((context->micro_fft_decimation_re_instances == NULL) ||
        (context->micro_fft_decimation_im_instances == NULL))
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const int32_t micro_fft_decimation_decimation_state_size = RADAR_PRESENCE_DECIMATION_NUMTAPS +
                                                               RADAR_PRESENCE_DECIMATION_FACTOR - 1;
    const size_t micro_fft_decimation_states_sz =
        sizeof(float32_t) *
        (size_t)micro_fft_decimation_decimation_state_size *
        (size_t)context->max_range_limit_idx;

    context->micro_fft_decimation_re_states = (float32_t*)mem_alloc(micro_fft_decimation_states_sz);
    context->micro_fft_decimation_im_states = (float32_t*)mem_alloc(micro_fft_decimation_states_sz);

    if ((context->micro_fft_decimation_re_states == NULL) ||
        (context->micro_fft_decimation_im_states == NULL))
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    for (int32_t i = 0; i < context->max_range_limit_idx; ++i)
    {
        int32_t state_idx = i * micro_fft_decimation_decimation_state_size;
        (void)arm_fir_decimate_init_f32(&context->micro_fft_decimation_re_instances[i],
                                        RADAR_PRESENCE_DECIMATION_NUMTAPS,
                                        RADAR_PRESENCE_DECIMATION_FACTOR,
                                        arm_fir_decimate_pCoeffs,
                                        &context->micro_fft_decimation_re_states[state_idx],
                                        RADAR_PRESENCE_DECIMATION_FACTOR);
        (void)arm_fir_decimate_init_f32(&context->micro_fft_decimation_im_instances[i],
                                        RADAR_PRESENCE_DECIMATION_NUMTAPS,
                                        RADAR_PRESENCE_DECIMATION_FACTOR,
                                        arm_fir_decimate_pCoeffs,
                                        &context->micro_fft_decimation_im_states[state_idx],
                                        RADAR_PRESENCE_DECIMATION_FACTOR);
    }

    /* Allocate space for micro complex FFT (N complex samples -> N complex coefficients) */
    const size_t micro_fft_buffer_sz = (size_t)context->max_range_limit_idx *
                                       (size_t)context->config.micro_fft_size *
                                       sizeof(cfloat32_t);
    context->micro_fft_buffer = (cfloat32_t*)mem_alloc(micro_fft_buffer_sz);
    if (context->micro_fft_buffer == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    /* Set max micro fft size to the initial set one, can be reduced in runtime, but not extended */
    context->max_micro_fft_size = context->config.micro_fft_size;

    /* Allocate space for micro doppler FFT */
    const size_t micro_fft_col_buffer_sz = (size_t)context->config.micro_fft_size *
                                           sizeof(cfloat32_t);
    context->micro_fft_col_buffer = (cfloat32_t*)mem_alloc(micro_fft_col_buffer_sz);
    if (context->micro_fft_col_buffer == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t range_intensity_win_sz = (size_t)context->macro_fft_size *
                                          sizeof(float32_t);
    context->range_intensity_win = (float32_t*)mem_alloc(range_intensity_win_sz);
    if (context->range_intensity_win == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    for (int32_t i = 0; i < context->macro_fft_size; ++i)
    {
        context->range_intensity_win[i] = 0.2f * ((float32_t)i + 1.0f);
    }

    const size_t macro_detect_timestamps_sz = (size_t)context->macro_fft_size *
                                              sizeof(XENSIV_RADAR_PRESENCE_TIMESTAMP);
    context->macro_detect_timestamps =
        (XENSIV_RADAR_PRESENCE_TIMESTAMP*)mem_calloc(macro_detect_timestamps_sz);
    if (context->macro_detect_timestamps == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t micro_detect_timestamps_sz = (size_t)context->macro_fft_size *
                                              sizeof(XENSIV_RADAR_PRESENCE_TIMESTAMP);
    context->micro_detect_timestamps =
        (XENSIV_RADAR_PRESENCE_TIMESTAMP*)mem_calloc(micro_detect_timestamps_sz);
    if (context->micro_detect_timestamps == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t macro_detect_confidences_sz = (size_t)context->macro_fft_size *
                                               sizeof(float32_t);
    context->macro_detect_confidences = (float32_t*)mem_calloc(macro_detect_confidences_sz);
    if (context->macro_detect_confidences == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    const size_t micro_detect_distances_sz = (size_t)context->macro_fft_size *
                                             sizeof(float32_t);
    context->micro_detect_distances = (float32_t*)mem_calloc(micro_detect_distances_sz);
    if (context->micro_detect_distances == NULL)
    {
        xensiv_radar_presence_free(context);
        return XENSIV_RADAR_PRESENCE_MEM_ERROR;
    }

    xensiv_radar_presence_reset(context);

    *handle = context;
    return XENSIV_RADAR_PRESENCE_OK;
}


int32_t xensiv_radar_presence_get_config(const xensiv_radar_presence_handle_t handle,
                                         xensiv_radar_presence_config_t* config)
{
    assert(handle != NULL);
    assert(config != NULL);

    (void)memcpy(config, &handle->config, sizeof(xensiv_radar_presence_config_t));

    return XENSIV_RADAR_PRESENCE_OK;
}


int32_t xensiv_radar_presence_set_config(xensiv_radar_presence_handle_t handle,
                                         const xensiv_radar_presence_config_t* config)
{
    assert(handle != NULL);
    assert(config != NULL);

    if (config->micro_fft_size > handle->max_micro_fft_size)
    {
        return XENSIV_RADAR_PRESENCE_FFT_LEN_ERROR;
    }
    (void)memcpy(&handle->config, config, sizeof(xensiv_radar_presence_config_t));
    if (config->min_range_bin > handle->max_range_limit_idx)
    {
        handle->config.min_range_bin = handle->max_range_limit_idx;
    }
    if (config->max_range_bin > handle->max_range_limit_idx)
    {
        handle->config.max_range_bin = handle->max_range_limit_idx;
    }
    return XENSIV_RADAR_PRESENCE_OK;
}


int32_t xensiv_radar_presence_process_frame(xensiv_radar_presence_handle_t handle,
                                            float32_t* frame,
                                            XENSIV_RADAR_PRESENCE_TIMESTAMP time_ms)
{
    assert(handle != NULL);
    assert(frame != NULL);

    // Band stop (10-35Hz) FIR Coefficients buffer generated using fir1() MATLAB function.
    // Test matlab code as below.
    /*
       x = [0.1953125:0.1953125:100];
       low = 10.0 / 100;
       bnd = [35/100 99/100];
       h = fir1(64,[low bnd],'DC-1');
       fft = 20 * log10(abs(freqz(h, 1, 512)'));
       plot(x, fft);
     */
    static const float32_t arm_fir_bandpass_pCoeffs[RADAR_PRESENCE_BANDPASS_NUMTAPS] =
    {
        -0.000672018944688787f, 5.40997750800323e-05f, -0.00170551007050673f,
        0.000706931294401583f,
        0.000529718080087782f,  0.00403359866465874f,  0.00102443397277923f,
        0.00234848093688213f,
        -0.00194992073010673f,  0.00451365295988384f,  0.00312574092180467f,
        0.00888191214923986f,
        -0.00340548841703134f,  -0.00434494380465395f, -0.0153910491204704f,
        -0.00133041100723547f,
        -0.00517641595111685f,  0.00200054539528286f,  -0.0241426155178683f,
        -0.0230852875573157f,
        -0.0293254372480552f,   0.0105956968865953f,   0.0175013648649183f,
        0.0306608940135099f,
        -0.00856346834860387f,  0.00160778144085906f,  0.0222545709144638f,
        0.112213549580022f,
        0.136465963717548f,     0.110216333677660f,    -0.0448122804532963f,
        -0.174898778170997f,
        0.740136712192538f,     -0.174898778170997f,   -0.0448122804532963f,
        0.110216333677660f,
        0.136465963717548f,     0.112213549580022f,    0.0222545709144638f,
        0.00160778144085906f,
        -0.00856346834860387f,  0.0306608940135099f,   0.0175013648649183f,
        0.0105956968865953f,
        -0.0293254372480552f,   -0.0230852875573157f,  -0.0241426155178683f,
        0.00200054539528286f,
        -0.00517641595111685f,  -0.00133041100723547f, -0.0153910491204704f,
        -0.00434494380465395f,
        -0.00340548841703134f,  0.00888191214923986f,  0.00312574092180467f,
        0.00451365295988384f,
        -0.00194992073010673f,  0.00234848093688213f,  0.00102443397277923f,
        0.00403359866465874f,
        0.000529718080087782f,  0.000706931294401583f, -0.00170551007050673f,
        5.40997750800323e-05f,
        -0.000672018944688787f
    };

    // if (handle->config.macro_fft_bandpass_filter_enabled)
    // {
    /* initial the bandpass state and initial time whenever bandpass is enabled */
    if (handle->bandpass_initial_time_ms == 0U)
    {
        /* Initialize CMSIS FIR structures */
        for (int32_t i = 0; i < handle->max_range_limit_idx; ++i)
        {
            int32_t state_idx = i * macro_fft_bandpass_fir_state_size;
            arm_fir_init_f32(&handle->macro_fft_bandpass_fir_re_instances[i],
                             RADAR_PRESENCE_BANDPASS_NUMTAPS,
                             arm_fir_bandpass_pCoeffs,
                             &handle->macro_fft_bandpass_fir_re_states[state_idx],
                             RADAR_PRESENCE_BANDPASS_BLOCKSIZE);

            arm_fir_init_f32(&handle->macro_fft_bandpass_fir_im_instances[i],
                             RADAR_PRESENCE_BANDPASS_NUMTAPS,
                             arm_fir_bandpass_pCoeffs,
                             &handle->macro_fft_bandpass_fir_im_states[state_idx],
                             RADAR_PRESENCE_BANDPASS_BLOCKSIZE);
        }
        handle->bandpass_initial_time_ms = time_ms + RADAR_PRESENCE_BANDPASS_DELAY;
    }
    // }

    memcpy(handle->macro_fft_buffer, frame,
           handle->config.num_samples_per_chirp * sizeof(float32_t));
    if (ifx_range_fft_f32((float32_t*)handle->macro_fft_buffer,
                          handle->macro_fft_buffer,
                          true,
                          handle->macro_fft_win,
                          (uint16_t)handle->config.num_samples_per_chirp,
                          1) != IFX_SENSOR_DSP_STATUS_OK)
    {
        return XENSIV_RADAR_PRESENCE_FFT_LEN_ERROR;
    }

    if (handle->config.macro_fft_bandpass_filter_enabled)
    {
        for (int32_t i = 0; i < handle->max_range_limit_idx; ++i)
        {
            float32_t* in = (float32_t*)&(handle->macro_fft_buffer[i]);
            float32_t* out = (float32_t*)&(handle->bandpass_macro_fft_buffer[i]);
            arm_fir_f32(&handle->macro_fft_bandpass_fir_re_instances[i],
                        &in[0],
                        &out[0],
                        RADAR_PRESENCE_BANDPASS_BLOCKSIZE);

            arm_fir_f32(&handle->macro_fft_bandpass_fir_im_instances[i],
                        &in[1],
                        &out[1],
                        RADAR_PRESENCE_BANDPASS_BLOCKSIZE);
        }
    }

    cfloat32_t* macro_fft_buffer =
        handle->config.macro_fft_bandpass_filter_enabled ? handle->bandpass_macro_fft_buffer :
        handle->
        macro_fft_buffer;
    if (handle->macro_last_compare_init == false)
    {
        (void)memcpy(handle->last_macro_compare,
                     macro_fft_buffer,
                     (size_t)handle->macro_fft_size * sizeof(cfloat32_t));
        handle->macro_last_compare_init = true;
    }

    // check macro movement every n-milliseconds and timestamp itself is above
    // macro_compare_interval_ms

    bool hit = false;
    if ((handle->config.mode != XENSIV_RADAR_PRESENCE_MODE_MICRO_ONLY) &&
        ((handle->macro_last_compare_ms + handle->config.macro_compare_interval_ms) < time_ms) &&
        (time_ms > handle->bandpass_initial_time_ms))
    {
        if ((handle->macro_last_compare_ms + (2U * handle->config.macro_compare_interval_ms)) >
            time_ms)
        {
            for (int32_t i = handle->config.min_range_bin; i <= handle->config.max_range_bin; ++i)
            {
                // compare the difference current FFT data and previous FFT data in range bin i
                cfloat32_t diff;
                diff = macro_fft_buffer[i] - handle->last_macro_compare[i];

                // apply a scale window to the FFT difference
                float32_t macro = cabsf(diff) * handle->range_intensity_win[i];

                // macro score need to multiply a scaling factor to avoid possible time delay when
                // person came in (bandpass will weaken the signal inevitably)
                if (handle->config.macro_fft_bandpass_filter_enabled)
                {
                    macro = macro * (0.5f / 0.45f);
                }

                // update the maximum macro and maximum macro range bin index
                if (macro >= handle->max_macro)
                {
                    handle->max_macro = macro;
                    handle->max_macro_idx = i;
                }

                // if macro satisfy threshold, a hit is found and set macro detect time stamp and
                // macro detect confidence.
                if (macro >= handle->config.macro_threshold)
                {
                    hit = true;
                    handle->macro_detect_timestamps[i] = time_ms +
                                                         handle->config.macro_movement_validity_ms;
                    handle->macro_detect_confidences[i] = macro - handle->config.macro_threshold;
                }
            }
        }

        // increment the _macroMovementHitCount if a hit is found , else set to 0
        if (hit)
        {
            handle->macro_movement_hit_count++;
        }
        else
        {
            handle->macro_movement_hit_count = 0;
        }

        // update lastMacroCompare.
        (void)memcpy(handle->last_macro_compare,
                     macro_fft_buffer,
                     (size_t)handle->macro_fft_size * sizeof(cfloat32_t));

        // check whether a macro movement is detected by checking the macroMovemntIndex if macro hit
        // count satisfy thresholds.
        int32_t macro_movement_idx = -1;
        if (handle->macro_movement_hit_count >= handle->config.macro_movement_confirmations)
        {
            // count the macroMovementDetectRange (number of range bin that has macro movement).
            int32_t macro_movement_detect_range = 0;
            for (int32_t i = handle->config.min_range_bin; i <= handle->config.max_range_bin; ++i)
            {
                if (time_ms <= handle->macro_detect_timestamps[i])
                {
                    macro_movement_detect_range++;
                }
            }

            // if macroMovementDetectRange larger than _macroTriggerRange(another threshold) or
            // current mode is not absence.
            if ((macro_movement_detect_range >= handle->config.macro_trigger_range) ||
                (handle->state != XENSIV_RADAR_PRESENCE_STATE_ABSENCE))
            {
                // check whether a macroMovementIndex is found,  exit the searching once it is
                // found.
                for (int32_t i = handle->config.min_range_bin; i <= handle->config.max_range_bin;
                     ++i)
                {
                    if (time_ms <= handle->macro_detect_timestamps[i])
                    {
                        macro_movement_idx = i;
                        break;
                    }
                }
            }
        }
        // update _macroLastCompareInMs
        handle->macro_last_compare_ms = time_ms;
        // check macro status by checking whether macroMovementIndex is different from the last
        // time.
        if (macro_movement_idx != handle->last_macro_reported_idx)
        {
            // if the macroMovementIndex is valid, macro presence is detected.
            if (macro_movement_idx >= 0)
            {
                if ((handle->callback) != NULL)
                {
                    const xensiv_radar_presence_event_t event =
                    {
                        .timestamp  = handle->macro_detect_timestamps[macro_movement_idx] -
                                      handle->config.macro_movement_validity_ms,
                        .range_bin  = macro_movement_idx,
                        .state      = XENSIV_RADAR_PRESENCE_STATE_MACRO_PRESENCE
                    };
                    handle->callback(handle, &event, handle->callback_data);
                }

                handle->state = XENSIV_RADAR_PRESENCE_STATE_MACRO_PRESENCE;
                handle->last_reported_idx = macro_movement_idx;
            }
            // if macro presence is not detected.
            else
            {
                // if the run mode is set to macro only, then it is absence.
                if (handle->config.mode == XENSIV_RADAR_PRESENCE_MODE_MACRO_ONLY)
                {
                    switch_to_absence(handle, time_ms);
                }
                // start looking for micro detection and set up required variables
                else
                {
                    handle->state = XENSIV_RADAR_PRESENCE_STATE_MICRO_PRESENCE;
                    handle->last_micro_reported_idx = -1;
                    // set microMovement Timestamp to data time (current time)
                    for (int32_t i =
                             handle->config.min_range_bin; i <= handle->config.max_range_bin;
                         ++i)
                    {
                        // only interested in micro range bin that is >= last reported macro range
                        // bin
                        handle->micro_detect_timestamps[i] =
                            (i >= handle->last_macro_reported_idx) ?
                            (time_ms + handle->config.micro_movement_validity_ms) :
                            0U;
                    }
                }
                handle->micro_fft_calc_col_idx = handle->config.min_range_bin;
            }
            handle->last_macro_reported_idx = macro_movement_idx;
        }
    }

    if (handle->config.micro_fft_decimation_enabled)
    {
        cfloat32_t* micro_fft_decimation_buffer_ptr =
            &handle->micro_fft_decimation_buffer[handle->micro_fft_decimation_write_row_idx *
                                                 handle->max_range_limit_idx];
        (void)memcpy(micro_fft_decimation_buffer_ptr,
                     handle->macro_fft_buffer,
                     (size_t)handle->max_range_limit_idx * sizeof(cfloat32_t));

        handle->micro_fft_decimation_write_row_idx++;

        if (handle->micro_fft_decimation_write_row_idx == RADAR_PRESENCE_DECIMATION_FACTOR)
        {
            handle->micro_fft_decimation_write_row_idx = 0;

            float32_t micro_fft_decimation_input_re_block[RADAR_PRESENCE_DECIMATION_FACTOR];
            float32_t micro_fft_decimation_input_im_block[RADAR_PRESENCE_DECIMATION_FACTOR];

            for (int32_t i = 0; i < handle->max_range_limit_idx; ++i)
            {
                for (int32_t j = 0; j < RADAR_PRESENCE_DECIMATION_FACTOR; j++)
                {
                    cfloat32_t m =
                        handle->micro_fft_decimation_buffer[(j * handle->max_range_limit_idx) + i];
                    micro_fft_decimation_input_re_block[j] = crealf(m);
                    micro_fft_decimation_input_im_block[j] = cimagf(m);
                }

                float32_t result_re;
                float32_t result_im;

                arm_fir_decimate_f32(&handle->micro_fft_decimation_re_instances[i],
                                     micro_fft_decimation_input_re_block,
                                     &result_re,
                                     RADAR_PRESENCE_DECIMATION_FACTOR);
                arm_fir_decimate_f32(&handle->micro_fft_decimation_im_instances[i],
                                     micro_fft_decimation_input_im_block,
                                     &result_im,
                                     RADAR_PRESENCE_DECIMATION_FACTOR);

                // write to microBuffer
                int32_t micro_fft_buffer_idx = (handle->micro_fft_write_row_idx *
                                                handle->max_range_limit_idx) + i;
                handle->micro_fft_buffer[micro_fft_buffer_idx] = result_re + (I * result_im);
            }
            handle->micro_fft_write_row_idx++;
        }
    }
    else
    {
        // write to microBuffer
        (void)memcpy(&handle->micro_fft_buffer[handle->micro_fft_write_row_idx *
                                               handle->max_range_limit_idx],
                     handle->macro_fft_buffer,
                     (size_t)handle->max_range_limit_idx * sizeof(cfloat32_t));
        handle->micro_fft_write_row_idx++;
    }

    if (handle->micro_fft_write_row_idx == handle->config.micro_fft_size)
    {
        handle->micro_fft_calc_ready = true;
        handle->micro_fft_write_row_idx = 0;
        handle->micro_fft_calc_col_idx = handle->config.min_range_bin;
    }

    // check macro only if enabled and not currently in absence or macro presence mode
    if ((handle->config.mode == XENSIV_RADAR_PRESENCE_MODE_MACRO_ONLY) ||
        ((handle->config.mode == XENSIV_RADAR_PRESENCE_MODE_MICRO_IF_MACRO) &&
         ((handle->state == XENSIV_RADAR_PRESENCE_STATE_ABSENCE) ||
          (handle->state == XENSIV_RADAR_PRESENCE_STATE_MACRO_PRESENCE))))
    {
        return XENSIV_RADAR_PRESENCE_OK;
    }

    // do micro fft
    if (handle->micro_fft_calc_ready == true)
    {
        // Calculate the mean of micro FFT data
        cfloat32_t mean = 0.0f + (I * 0.0f);
        for (int32_t i = handle->micro_fft_write_row_idx;
             i < (handle->micro_fft_write_row_idx + handle->config.micro_fft_size); ++i)
        {
            int32_t row = i % handle->config.micro_fft_size;
            cfloat32_t value =
                handle->micro_fft_buffer[(row * handle->max_range_limit_idx) +
                                         handle->micro_fft_calc_col_idx];
            handle->micro_fft_col_buffer[i - handle->micro_fft_write_row_idx] = value;
            mean += value;
        }
        /* workaround for ARM compiler Error: L6218E: Undefined symbol __divsc3 (referred from
           xensiv_radar_presence.o) */
        float32_t mean_real = crealf(mean) / (float32_t)handle->config.micro_fft_size;
        float32_t mean_imag = cimagf(mean) / (float32_t)handle->config.micro_fft_size;
        mean = mean_real + (I * mean_imag);

        // perform mean removal
        for (int32_t i = 0; i < handle->config.micro_fft_size; ++i)
        {
            handle->micro_fft_col_buffer[i] -= mean;
        }

        // perform doppler fft
        arm_cfft_f32(&handle->doppler_fft, (float32_t*)handle->micro_fft_col_buffer, 0, 1);

        float32_t speed = 0.0f;
        for (int32_t i = 1; i <= handle->config.micro_movement_compare_idx; ++i)
        {
            speed += cabsf(handle->micro_fft_col_buffer[i]);
        }

        // update maxMicro seen
        if (handle->max_micro < speed)
        {
            handle->max_micro = speed;
            handle->max_micro_idx = handle->micro_fft_calc_col_idx;
        }
        // if speed satisfy the threshold, set _microDetectTimestamps, _microDetectDistances and
        // current mode to microPresence.
        float32_t confidence = speed - handle->config.micro_threshold;
        if (confidence >= 0.0f)
        {
            handle->micro_detect_timestamps[handle->micro_fft_calc_col_idx] =
                time_ms + handle->config.micro_movement_validity_ms;
            handle->micro_detect_distances[handle->micro_fft_calc_col_idx] = confidence;
            handle->state = XENSIV_RADAR_PRESENCE_STATE_MICRO_PRESENCE;
        }

        handle->micro_fft_calc_col_idx++;
        if (handle->micro_fft_calc_col_idx > handle->config.max_range_bin)
        {
            handle->micro_fft_calc_col_idx = handle->config.min_range_bin;
            handle->micro_fft_all_calculated= true;
        }
    }

    int32_t micro_movement_idx = -1;
    if (handle->config.micro_fft_decimation_enabled)
    {
        bool all_previous_event_expired = true;
        for (int32_t i = handle->config.min_range_bin; i <= handle->last_reported_idx; ++i)
        {
            if (time_ms <= handle->macro_detect_timestamps[i])
            {
                all_previous_event_expired = false;
            }
        }

        bool macro_not_displayed = false;
        if (all_previous_event_expired)
        {
            for (int32_t i = handle->last_reported_idx + 1; i <= handle->config.max_range_bin; ++i)
            {
                if (time_ms <= handle->macro_detect_timestamps[i])
                {
                    micro_movement_idx = i;
                    macro_not_displayed = true;
                    break;
                }
            }
        }
        // check micro movement state
        if ((time_ms <= handle->micro_detect_timestamps[handle->last_reported_idx]) &&
            !macro_not_displayed)
        {
            micro_movement_idx = handle->last_reported_idx;
        }
        else if (handle->micro_fft_all_calculated && !macro_not_displayed)
        {
            float32_t max_micro_confidence = 0.0f;
            for (int32_t i = handle->config.min_range_bin; i <= handle->config.max_range_bin; ++i)
            {
                if ((time_ms <= handle->micro_detect_timestamps[i]) &&
                    (handle->micro_detect_distances[i] > max_micro_confidence) &&
                    ((handle->micro_detect_timestamps[i] -
                      handle->micro_detect_timestamps[handle->last_reported_idx]) > 2000U))
                {
                    micro_movement_idx = i;
                    max_micro_confidence = handle->micro_detect_distances[i];
                }
            }
        }
        else
        {
        }
    }
    else
    {
        // previous release behaviour for non-MAX settings

        // check micro movement state
        for (int32_t i = handle->config.min_range_bin; i <= handle->config.max_range_bin; ++i)
        {
            if (time_ms <= handle->micro_detect_timestamps[i])
            {
                micro_movement_idx = i;
                break;
            }
        }
    }

    // report if micro movement changed distance
    if (micro_movement_idx != handle->last_micro_reported_idx)
    {
        handle->last_micro_reported_idx = micro_movement_idx;
        if (micro_movement_idx >= 0)
        {
            if ((handle->callback) != NULL)
            {
                const xensiv_radar_presence_event_t event =
                {
                    .timestamp  = handle->micro_detect_timestamps[micro_movement_idx] -
                                  handle->config.micro_movement_validity_ms,
                    .range_bin  = micro_movement_idx,
                    .state      = XENSIV_RADAR_PRESENCE_STATE_MICRO_PRESENCE
                };
                handle->callback(handle, &event, handle->callback_data);
            }
            handle->last_reported_idx = micro_movement_idx;
        }
    }

    // report absence
    if ((micro_movement_idx == -1) &&
        (handle->state == XENSIV_RADAR_PRESENCE_STATE_MICRO_PRESENCE) &&
        handle->micro_fft_all_calculated)
    {
        switch_to_absence(handle, time_ms);
    }

    return XENSIV_RADAR_PRESENCE_OK;
}


cfloat32_t* xensiv_radar_presence_get_macro_fft_buffer(const xensiv_radar_presence_handle_t handle)
{
    assert(handle != NULL);

    return handle->macro_fft_buffer;
}


bool xensiv_radar_presence_get_max_macro(const xensiv_radar_presence_handle_t handle, float* macro,
                                         int* index)
{
    assert(handle != NULL);
    if (handle->max_macro_idx < 0)
    {
        return false;
    }
    if (macro != NULL)
    {
        *macro = handle->max_macro;
    }
    if (index != NULL)
    {
        *index = handle->max_macro_idx;
    }
    handle->max_macro = 0.0f;
    handle->max_macro_idx = -1;
    return true;
}


bool xensiv_radar_presence_get_max_micro(const xensiv_radar_presence_handle_t handle, float* micro,
                                         int* index)
{
    assert(handle != NULL);
    if (handle->max_micro_idx < 0)
    {
        return false;
    }
    if (micro != NULL)
    {
        *micro = handle->max_micro;
    }
    if (index != NULL)
    {
        *index = handle->max_micro_idx;
    }
    handle->max_micro = 0.0f;
    handle->max_micro_idx = -1;
    return true;
}


float32_t xensiv_radar_presence_get_bin_length(const xensiv_radar_presence_handle_t handle)
{
    assert(handle != NULL);
    return ifx_range_resolution(handle->config.bandwidth);
}


void xensiv_radar_presence_reset(xensiv_radar_presence_handle_t handle)
{
    assert(handle != NULL);

    handle->switches = 0;
    handle->micro_fft_decimation_write_row_idx = 0;
    handle->micro_fft_write_row_idx = 0;
    handle->micro_fft_calc_ready = false;
    handle->micro_fft_calc_col_idx = 0;
    handle->micro_fft_all_calculated = false;

    (void)memset(handle->macro_detect_timestamps,
                 0,
                 (size_t)handle->macro_fft_size * sizeof(XENSIV_RADAR_PRESENCE_TIMESTAMP));
    (void)memset(handle->micro_detect_timestamps,
                 0,
                 (size_t)handle->macro_fft_size * sizeof(XENSIV_RADAR_PRESENCE_TIMESTAMP));
    (void)memset(handle->macro_detect_confidences,
                 0,
                 (size_t)handle->macro_fft_size * sizeof(float32_t));
    (void)memset(handle->micro_detect_distances,
                 0,
                 (size_t)handle->macro_fft_size * sizeof(float32_t));

    handle->macro_last_compare_init = false;
    handle->macro_last_compare_ms = 0U;
    handle->macro_movement_hit_count = 0;
    handle->last_macro_reported_idx = -1;
    handle->last_micro_reported_idx = -1;
    handle->state = XENSIV_RADAR_PRESENCE_STATE_ABSENCE;
    handle->max_macro = 0.0f;
    handle->max_micro = 0.0f;
    handle->max_macro_idx = -1;
    handle->max_micro_idx = -1;
    handle->last_reported_idx = -1;
    handle->bandpass_initial_time_ms = 0U;
}


void xensiv_radar_presence_free(xensiv_radar_presence_handle_t handle)
{
    assert(handle != NULL);

    mem_free(handle->micro_detect_distances);
    mem_free(handle->macro_detect_confidences);
    mem_free(handle->micro_detect_timestamps);
    mem_free(handle->macro_detect_timestamps);
    mem_free(handle->range_intensity_win);
    mem_free(handle->micro_fft_col_buffer);
    mem_free(handle->micro_fft_buffer);
    mem_free(handle->micro_fft_decimation_im_states);
    mem_free(handle->micro_fft_decimation_re_states);
    mem_free(handle->micro_fft_decimation_im_instances);
    mem_free(handle->micro_fft_decimation_re_instances);
    mem_free(handle->micro_fft_decimation_buffer);
    mem_free(handle->macro_fft_bandpass_fir_re_states);
    mem_free(handle->macro_fft_bandpass_fir_re_instances);
    mem_free(handle->macro_fft_bandpass_fir_im_states);
    mem_free(handle->macro_fft_bandpass_fir_im_instances);
    mem_free(handle->last_macro_compare);
    mem_free(handle->macro_fft_buffer);
    mem_free(handle->macro_fft_win);
    mem_free(handle->bandpass_macro_fft_buffer);
    mem_free(handle);
}


static void* mem_alloc(size_t n)
{
    return malloc_fptr(n);
}


static void* mem_calloc(size_t n)
{
    void* ptr = malloc_fptr(n);
    if (ptr != NULL)
    {
        (void)memset(ptr, 0, n);
    }

    return ptr;
}


static void mem_free(void* ptr)
{
    if (ptr != NULL)
    {
        free_fptr(ptr);
    }
}


static void switch_to_absence(const xensiv_radar_presence_handle_t handle,
                              XENSIV_RADAR_PRESENCE_TIMESTAMP time_ms)
{
    assert(handle->state != XENSIV_RADAR_PRESENCE_STATE_ABSENCE);

    if ((handle->callback) != NULL)
    {
        const xensiv_radar_presence_event_t event =
        {
            .timestamp  = time_ms,
            .range_bin  = -1,
            .state      = XENSIV_RADAR_PRESENCE_STATE_ABSENCE
        };
        handle->callback(handle, &event, handle->callback_data);
    }

    handle->state = XENSIV_RADAR_PRESENCE_STATE_ABSENCE;
    handle->last_micro_reported_idx = -1;
    handle->micro_fft_all_calculated = false;
}
