/*
 * Copyright (c) 2023 EdgeImpulse Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an "AS
 * IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language
 * governing permissions and limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Include ----------------------------------------------------------------- */
#include "Particle.h"
#include "Microphone_PDM.h"
#include <_You_re_Muted__inferencing.h>
#include <cstdint>

SYSTEM_THREAD(ENABLED);

/* Forward declerations ---------------------------------------------------- */
int raw_feature_get_data(size_t offset, size_t length, float *out_ptr);
void setup();
void loop();

/** Audio buffers, pointers and selectors */
typedef struct {
    int16_t *buffer;
    uint8_t buf_ready;
    uint32_t buf_count;
    uint32_t n_samples;
} inference_t;

static inference_t inference;
static signed short sampleBuffer[2048];

static bool microphone_inference_start(uint32_t n_samples);
static bool microphone_inference_record(void);
static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr);
static void microphone_inference_end(void);


void print_inference_result(ei_impulse_result_t result);

/**
 * @brief      Particle setup function
 */
void setup()
{
    delay(2000);

    // put your setup code here, to run once:
    Serial.begin(115200);
    // comment out the below line to cancel the wait for USB connection (needed for native USB)
    while (!Serial);
    Serial.println("Edge Impulse Inferencing Demo");

	int err = Microphone_PDM::instance()
		.withOutputSize(Microphone_PDM::OutputSize::SIGNED_16)
		.withRange(Microphone_PDM::Range::RANGE_32768)
		.withSampleRate(EI_CLASSIFIER_FREQUENCY)
		.init();

	if (err) {
		Log.error("PDM decoder init err=%d", err);
	}

    // summary of inferencing settings (from model_metadata.h)
    ei_printf("Inferencing settings:\n");
    ei_printf("\tInterval: %.2f ms.\n", (float)EI_CLASSIFIER_INTERVAL_MS);
    ei_printf("\tFrame size: %d\n", EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE);
    ei_printf("\tSample length: %d ms.\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT / 16);
    ei_printf("\tNo. of classes: %d\n", sizeof(ei_classifier_inferencing_categories) / sizeof(ei_classifier_inferencing_categories[0]));

    if (microphone_inference_start(EI_CLASSIFIER_RAW_SAMPLE_COUNT) == false) {
        ei_printf("ERR: Could not allocate audio buffer (size %d), this could be due to the window length of your model\r\n", EI_CLASSIFIER_RAW_SAMPLE_COUNT);
        return;
    }
}

/**
 * @brief      Particle main function
 */
void loop()
{
    ei_printf("Starting inferencing in 2 seconds...\n");

    delay(2000);

    ei_printf("Recording...\n");

    bool m = microphone_inference_record();
    if (!m) {
        ei_printf("ERR: Failed to record audio...\n");
        return;
    }

    ei_printf("Recording done\n");

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    signal.get_data = &microphone_audio_signal_get_data;

    ei_impulse_result_t result = { 0 };

    // invoke the impulse
    EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false /* debug */);
    if (res != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", res);
        return;
    }

    // print inference return code
    ei_printf("run_classifier returned: %d\r\n", res);
    print_inference_result(result);

    delay(1000);
}

void print_inference_result(ei_impulse_result_t result) {

    // Print how long it took to perform inference
    ei_printf("Timing: DSP %d ms, inference %d ms, anomaly %d ms\r\n",
            result.timing.dsp,
            result.timing.classification,
            result.timing.anomaly);

    // Print the prediction results (object detection)
#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    ei_printf("Object detection bounding boxes:\r\n");
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
    }

    // Print the prediction results (classification)
#else
    ei_printf("Predictions:\r\n");
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        ei_printf("  %s: ", ei_classifier_inferencing_categories[i]);
        ei_printf("%.5f\r\n", result.classification[i].value);
    }
#endif

    // Print anomaly result (if it exists)
#if EI_CLASSIFIER_HAS_ANOMALY == 1
    ei_printf("Anomaly prediction: %.3f\r\n", result.anomaly);
#endif

}

static int16_t *sptr;
static uint32_t sample_length = 0;

/**
 * @brief      PDM buffer full callback
 *             Get data and call audio thread callback
 */
static void pdm_data_ready_inference_callback(void)
{
    bool dma_ready = Microphone_PDM::instance().noCopySamples([](void *pSamples, size_t numSamples) {

        sample_length = Microphone_PDM::instance().getBufferSizeInBytes() / 2;
        sptr = (int16_t *)pSamples;
    });

    if(sample_length && dma_ready) {

        if (inference.buf_ready == 0) {
            for(int i = 0; i < sample_length; i++) {
                inference.buffer[inference.buf_count++] = sptr[i];

                if(inference.buf_count >= inference.n_samples) {
                    inference.buf_count = 0;
                    inference.buf_ready = 1;
                    break;
                }
            }
        }
    }
}

/**
 * @brief      Init inferencing struct and setup/start PDM
 *
 * @param[in]  n_samples  The n samples
 *
 * @return     { description_of_the_return_value }
 */
static bool microphone_inference_start(uint32_t n_samples)
{
    inference.buffer = (int16_t *)malloc(n_samples * sizeof(int16_t));

    if(inference.buffer == NULL) {
        return false;
    }

    inference.buf_count  = 0;
    inference.n_samples  = n_samples;
    inference.buf_ready  = 0;

    if (Microphone_PDM::instance().start()) {
        ei_printf("Failed to start PDM!");
        microphone_inference_end();

        return false;
    }

    return true;
}

/**
 * @brief      Wait on new data
 *
 * @return     True when finished
 */
static bool microphone_inference_record(void)
{
    inference.buf_ready = 0;
    inference.buf_count = 0;

    uint64_t sample_start_us = ei_read_timer_us();

    /* Empty DMA buffers */
    while(Microphone_PDM::instance().noCopySamples([](void *pSamples, size_t numSamples){})){};

    while(inference.buf_ready == 0) {

        pdm_data_ready_inference_callback();
    }

    ei_printf("Sample time: %d\r\n", (int) (ei_read_timer_us() - sample_start_us) / 1000);

    return true;
}

/**
 * Get raw audio signal data
 */
static int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr)
{
    numpy::int16_to_float(&inference.buffer[offset], out_ptr, length);

    return 0;
}

/**
 * @brief      Stop PDM and release buffers
 */
static void microphone_inference_end(void)
{
    Microphone_PDM::instance().stop();
    free(inference.buffer);
}
