/*
 * SPDX-FileCopyrightText: 2024 Seeed Technology Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <sys/unistd.h>
#include <sys/stat.h>

#include "driver/uart.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "mbedtls/base64.h"
#include "esp_jpeg_dec.h"

#include "sscma_client_io.h"
#include "sscma_client_ops.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "sensecap-watcher.h"

// !!!!!!! To change the status of the CPU/FPS counter, edit line 756 in /components/lvgl/src/lv_conf_internal.h
// I couldn't figure out how you're supposed to do it, so that's how I did it

#define CANVAS_WIDTH DRV_LCD_H_RES
#define CANVAS_HEIGHT DRV_LCD_V_RES

#define PREVIEW_IMG_WIDTH  416
#define PREVIEW_IMG_HEIGHT 416

#define CAMERA_SENSOR_RESOLUTION_240_240    0
#define CAMERA_SENSOR_RESOLUTION_416_416    1
#define CAMERA_SENSOR_RESOLUTION_480_480    2
#define CAMERA_SENSOR_RESOLUTION_640_480    3

#define DECODED_STR_MAX_SIZE (48 * 1028 * 2)

static const char *TAG = "Main";

//**********************************************************************************
// Definitions of variables
//**********************************************************************************

lv_disp_t *lvgl_disp;

static led_strip_handle_t s_rgb = 0;
static knob_handle_t s_knob = 0;
bool led_on = false;

esp_io_expander_handle_t io_expander = NULL;
sscma_client_handle_t client = NULL;

lv_obj_t *faceImage;
int photoNumber = -1;
char pictureNameBuffer[100];

bool captureInProgress;
int captureStartedCounter;
bool captureFinished;

static unsigned char decoded_str[DECODED_STR_MAX_SIZE];

static lv_img_dsc_t preview_img_dsc = {
    .header.always_zero = 0,
    .header.w = PREVIEW_IMG_WIDTH,
    .header.h = PREVIEW_IMG_HEIGHT,
    .data_size = 640 * 480 * LV_COLOR_DEPTH / 8,
    .header.cf = LV_IMG_CF_TRUE_COLOR,
    .data = NULL,
};

int randomInRange(int min, int max) {
    return rand() % (max - min + 1) + min;
}

int clamp(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

//**********************************************************************************
// SSCMA Stuff
//**********************************************************************************

void sscma_log();
void sscma_event();
void sscma_event_CAPTURE();
static esp_err_t write_file();
void display_one_image();

const sscma_client_callback_t sscmacallback = {
    .on_event = sscma_event,
    .on_log = sscma_log,
};

const sscma_client_callback_t sscmanocallback = {
    .on_event = NULL,
    .on_log = NULL,
};

void sscma_event(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    if (captureInProgress) {
        return;
    }

    char *currentFrame;

    printf("callback'd");

    int img_size = 0;

    // Note: reply is automatically recycled after exiting the function.
    if (sscma_utils_fetch_image_from_reply(reply, &currentFrame, &img_size) == ESP_OK)
    {
        if (captureStartedCounter == 3) { // capture ready
            char *num;

            photoNumber++;

            captureInProgress = true;
            if (asprintf(&num, "%d", photoNumber) == -1) {
                ESP_LOGE(TAG, "BIG OL STRING ERROR!!!! (USING ERROR NUMBER!!!)");
                num = "err";
            } else {
                // String concatenation using strcpy and strcat
                strcat(strcpy(pictureNameBuffer, "/sdcard/.unprocessedphotos/image-"), num);
                printf("%s\n", pictureNameBuffer);

                // Free dynamically allocated memory
                free(num);
            }
            write_file(pictureNameBuffer, currentFrame);

            captureFinished = true; // capture complete
        } else {
            if (captureStartedCounter != -1) { captureStartedCounter++; } // increment frames after counter started (to delay capture until the sensor resolution is changed)

            if (lvgl_port_lock(0))
            {
                display_one_image(faceImage, (const unsigned char *)currentFrame);
                lvgl_port_unlock();
            }
        }
        free(currentFrame);
    }

    lv_obj_set_scrollbar_mode(faceImage, LV_SCROLLBAR_MODE_OFF); // Never show the scrollbars
    lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_SCROLLABLE); // Never allow scrolling on the face
    lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_CLICKABLE);  /*To not allow adjusting by click*/
}

void sscma_log(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    if (reply->len >= 100)
    {
        strcpy(&reply->data[100 - 4], "...");
    }
    // Note: reply is automatically recycled after exiting the function.
    printf("log: %s\n", reply->data);
}

//**********************************************************************************
// File Stuff
//**********************************************************************************

static esp_err_t write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }
    fprintf(f, data);
    fclose(f);
    ESP_LOGI(TAG, "File written");

    return ESP_OK;
}

void getCurrentImageCount(void) {
    bool hasFoundPicture = false;
    FILE *file;
    char *num;
    int currentTest = 1;

    while (!hasFoundPicture) {
        if (asprintf(&num, "%d", currentTest) == -1) {
            ESP_LOGE(TAG, "string error");
            num = "err";
        } else {
            // String concatenation using strcpy and strcat
            strcat(strcpy(pictureNameBuffer, "/sdcard/.unprocessedphotos/image-"), num);

            file = fopen(pictureNameBuffer, "r");


            if (file != NULL) {
                fclose(file);
                printf("file exists");
                currentTest++;
            } else {
                photoNumber = currentTest - 1;
                printf("file doesn't exist");
                hasFoundPicture = true;
            }


            // Free dynamically allocated memory
            free(num);
        }
    }
}

//**********************************************************************************
// RGB Setup
//**********************************************************************************

static esp_err_t rgb_init()
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = BSP_RGB_CTRL,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_rgb));
    led_strip_set_pixel(s_rgb, 0, 0x00, 0x00, 0x00);
    led_strip_refresh(s_rgb);

    return ESP_OK;
}

static esp_err_t rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t ret = ESP_OK;
    uint32_t index = 0;

    ret |= led_strip_set_pixel(s_rgb, index, r, g, b);
    ret |= led_strip_refresh(s_rgb);
    return ret;
}

static void rgb_next_color(bool reverse)
{
    static uint8_t step = 4;
    static uint8_t target_color[3] = { 0, 255, 0 };
    static uint8_t current_color[3] = { 255, 0, 0 };

    for (int i = 0; i < 3; i++)
    {
        int pos = reverse ? ((i - 1 + 3) % 3) : ((i + 1) % 3);
        if (target_color[i] == 255 && current_color[i] == 255)
        {
            target_color[i] = 0;
            target_color[pos] = 255;
            break;
        }
    }

    for (int i = 0; i < 3; i++)
    {
        if (current_color[i] < target_color[i])
        {
            current_color[i] = current_color[i] > 255 - step ? 255 : current_color[i] + step;
        }
        else if (current_color[i] > target_color[i])
        {
            current_color[i] = current_color[i] < step ? 0 : current_color[i] - step;
        }
    }

    ESP_ERROR_CHECK(rgb_set(current_color[0] / 2, current_color[1] / 2, current_color[2] / 2));
}

//**********************************************************************************
// JPEG / Image Stuff
//**********************************************************************************

static int esp_jpeg_decoder_one_picture(uint8_t *input_buf, int len, uint8_t *output_buf)
{
    esp_err_t ret = ESP_OK;
    // Generate default configuration
    jpeg_dec_config_t config = { .output_type = JPEG_RAW_TYPE_RGB565_BE, .rotate = JPEG_ROTATE_0D };

    // Empty handle to jpeg_decoder
    jpeg_dec_handle_t jpeg_dec = NULL;

    // Create jpeg_dec
    jpeg_dec = jpeg_dec_open(&config);

    // Create io_callback handle
    jpeg_dec_io_t *jpeg_io = calloc(1, sizeof(jpeg_dec_io_t));
    if (jpeg_io == NULL)
    {
        return ESP_FAIL;
    }

    // Create out_info handle
    jpeg_dec_header_info_t *out_info = calloc(1, sizeof(jpeg_dec_header_info_t));
    if (out_info == NULL)
    {
        return ESP_FAIL;
    }

    // Set input buffer and buffer len to io_callback
    jpeg_io->inbuf = input_buf;
    jpeg_io->inbuf_len = len;

    // Parse jpeg picture header and get picture for user and decoder
    ret = jpeg_dec_parse_header(jpeg_dec, jpeg_io, out_info);

    if (ret < 0)
    {
        goto _exit;
    }

    jpeg_io->outbuf = output_buf;
    int inbuf_consumed = jpeg_io->inbuf_len - jpeg_io->inbuf_remain;
    jpeg_io->inbuf = input_buf + inbuf_consumed;
    jpeg_io->inbuf_len = jpeg_io->inbuf_remain;

    // Start decode jpeg raw data
    ret = jpeg_dec_process(jpeg_dec, jpeg_io);

    if (ret < 0)
    {
        goto _exit;
    }

    _exit:
    // Decoder deinitialize
    jpeg_dec_close(jpeg_dec);
    free(out_info);
    free(jpeg_io);
    return ret;
}

void display_one_image(lv_obj_t *image, const unsigned char *p_data)
{
    if (!p_data)
        return;

    size_t str_len = strlen((const char *)p_data);
    size_t output_len = 0;


    int decode_ret = mbedtls_base64_decode(decoded_str, DECODED_STR_MAX_SIZE, &output_len, p_data, str_len);

    if (decode_ret == 0)
    {
        if (preview_img_dsc.data == NULL)
        {
            preview_img_dsc.data = heap_caps_aligned_alloc(16, preview_img_dsc.data_size, MALLOC_CAP_SPIRAM);
        }

        int ret = esp_jpeg_decoder_one_picture(decoded_str, output_len, preview_img_dsc.data);

        if (ret == ESP_OK)
        {
            lv_img_set_src(image, &preview_img_dsc);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Failed to decode Base64 string, error: %d", decode_ret);
    }
}

//**********************************************************************************
// Knob Setup
//**********************************************************************************

bool knobJustMoved = false;
bool knobDir = false;

static void knob_right_callback(void *arg, void *data)
{
    knobJustMoved = true;
    knobDir = true;
}

static void knob_left_callback(void *arg, void *data)
{
    knobJustMoved = true;
    knobDir = false;
}

static esp_err_t knob_init(void)
{
    knob_config_t cfg = {
        .default_direction = 0,
        .gpio_encoder_a = BSP_KNOB_A,
        .gpio_encoder_b = BSP_KNOB_B,
    };
    s_knob = iot_knob_create(&cfg);
    if (NULL == s_knob)
    {
        ESP_LOGE(TAG, "knob create failed");
        return ESP_FAIL;
    }

    iot_knob_register_cb(s_knob, KNOB_LEFT, knob_left_callback, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_callback, NULL);
    return ESP_OK;
}

//**********************************************************************************
// MAIN CODE
//**********************************************************************************

void app_main(void)
{
    // Initialize the IO Expander
    io_expander = bsp_io_expander_init();
    assert(io_expander != NULL);

    // Set up the SSCMA client
    client = bsp_sscma_client_init();
    assert(client != NULL);

    // Initialize the display
    lvgl_disp = bsp_lvgl_init();
    assert(lvgl_disp != NULL);

    // Initalize the touchscreen
    lv_indev_t * tp = NULL;
    while (1) {
        tp = lv_indev_get_next(tp);
        if(tp->driver->type == LV_INDEV_TYPE_POINTER) {
            break;
        }
    }
    assert(tp != NULL);

    // Initialize some values for the camera stuff
    captureStartedCounter = -1; // Capture idle
    captureFinished = false;
    captureInProgress = false;

    // Initialize the RGB Light
    ESP_ERROR_CHECK(rgb_init());

    // Initialize the Knob
    ESP_ERROR_CHECK(knob_init());

    // Initalize the touches to offscreen
    lv_point_t pointTouched = {0, 0};
    lv_point_t previousPoint = {0, 0};

    faceImage = lv_img_create(lv_scr_act());
    lv_obj_add_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // hide it for later
    lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_align(faceImage, LV_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(faceImage, LV_SCROLLBAR_MODE_OFF); // Never show the scrollbars
    lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_SCROLLABLE); // Never allow scrolling on the face

    // Selection arc defenition
    lv_obj_t * selectorArc = lv_arc_create(lv_scr_act());
    lv_arc_set_rotation(selectorArc, 270);
    lv_arc_set_bg_angles(selectorArc, 0, 360);
    lv_obj_remove_style(selectorArc, NULL, LV_PART_KNOB);   /*Be sure the knob is not displayed*/
    lv_obj_clear_flag(selectorArc, LV_OBJ_FLAG_CLICKABLE);  /*To not allow adjusting by click*/
    lv_obj_center(selectorArc);
    lv_obj_add_flag(selectorArc, LV_OBJ_FLAG_HIDDEN); // hide it for later
    lv_obj_set_width(selectorArc, 412);
    lv_obj_set_height(selectorArc, 412);
    lv_obj_set_style_arc_color(selectorArc, lv_palette_main(LV_PALETTE_LIME), LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // Battery arc defenition
    lv_obj_t * batteryArc = lv_arc_create(lv_scr_act());
    lv_arc_set_rotation(batteryArc, 270);
    lv_arc_set_bg_angles(batteryArc, 0, 360);
    lv_obj_remove_style(batteryArc, NULL, LV_PART_KNOB);   /*Be sure the knob is not displayed*/
    lv_obj_clear_flag(batteryArc, LV_OBJ_FLAG_CLICKABLE);  /*To not allow adjusting by click*/
    lv_obj_center(batteryArc);
    lv_obj_add_flag(batteryArc, LV_OBJ_FLAG_HIDDEN); // hide it for later
    lv_obj_set_width(batteryArc, 412);
    lv_obj_set_height(batteryArc, 412);
    lv_obj_set_style_arc_color(batteryArc, lv_palette_main(LV_PALETTE_LIME), LV_PART_INDICATOR | LV_STATE_DEFAULT);

    // Battery percentage label defenition
    static lv_style_t style_whitetext;
    lv_style_init(&style_whitetext);
    lv_style_set_text_color(&style_whitetext, lv_color_white());

    lv_obj_t * batteryLabel = lv_label_create(lv_scr_act());
    lv_obj_add_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN); // hide it for later
    lv_obj_add_style(batteryLabel, &style_whitetext, 0);
    lv_obj_align(batteryLabel, LV_ALIGN_CENTER, 0, 150);
    lv_obj_set_width(batteryLabel, 200);
    lv_obj_set_style_text_align(batteryLabel, LV_TEXT_ALIGN_CENTER, 0); // Set the text to align to the center

    // Image Declarations
    LV_IMG_DECLARE(greeting1);
    LV_IMG_DECLARE(greeting2);
    LV_IMG_DECLARE(greeting3);
    LV_IMG_DECLARE(standby1);
    LV_IMG_DECLARE(standby2);
    LV_IMG_DECLARE(standby3);
    LV_IMG_DECLARE(standby4);
    LV_IMG_DECLARE(detected1);
    LV_IMG_DECLARE(detected2);
    LV_IMG_DECLARE(eyesclosed);
    LV_IMG_DECLARE(eyesopen);
    LV_IMG_DECLARE(eyesleft);
    LV_IMG_DECLARE(eyesright);
    LV_IMG_DECLARE(scanning1);
    LV_IMG_DECLARE(scanning2);
    LV_IMG_DECLARE(scanning3);
    LV_IMG_DECLARE(scanning4);
    LV_IMG_DECLARE(scanning5);

    lv_img_dsc_t greetingImages[] = {greeting1, greeting2, greeting3};
    lv_img_dsc_t sleepImages[] = {standby1, standby2, standby3, standby4};
    lv_img_dsc_t exclaimationImages[] = {detected1, detected2};
    lv_img_dsc_t idleImages[] = {eyesopen, eyesleft, eyesright};
    lv_img_dsc_t scanningImages[] = {scanning1, scanning2, scanning3, scanning4, scanning5};

    lv_obj_t * menuLabel = lv_label_create(lv_scr_act());
    lv_label_set_long_mode(menuLabel, LV_LABEL_LONG_WRAP);     /*Break the long lines*/
    lv_label_set_recolor(menuLabel, true);
    lv_obj_clear_flag(menuLabel, LV_OBJ_FLAG_CLICKABLE);  /*To not allow adjusting by click*/
    lv_obj_set_width(menuLabel, 150);
    lv_obj_center(menuLabel); // Align the label to the center of the screen
    lv_obj_add_flag(menuLabel, LV_OBJ_FLAG_HIDDEN); // hide the menu label for later
    lv_obj_set_style_text_align(menuLabel, LV_TEXT_ALIGN_CENTER, 0); // Set the text to align to the center



    static lv_style_t backgroundStyle;
    lv_style_set_bg_color(&backgroundStyle, lv_color_black());
    lv_obj_add_style(lv_scr_act(), &backgroundStyle, 0);

    int faceState = 0;
    int faceStateTimer = 0;
    int sleepAnimState = 0;
    int happyAnimState = 0;
    int scanAnimState = 0;
    int battAnimState = 0;
    int faceSleepTimer = randomInRange(50, 70);

    bool goToSleep = false;
    bool sleepCanceller = false;

    int menuTimeOut = 0;
    int menuSelection = 1;
    bool menuShouldMove = false;

    int battPercent = bsp_battery_get_percent();

    int state = 0; // Main System state


    ///////////////////////////////////////////////////////////////////////////////////
    //  SD Card Setup
    ///////////////////////////////////////////////////////////////////////////////////

    if (bsp_sdcard_is_inserted()) {
        bsp_sdcard_init_default(); // Mount sdcard to /sdcard
        vTaskDelay(10 / portTICK_PERIOD_MS);

        getCurrentImageCount();
    }
    else {
        while (!bsp_sdcard_is_inserted()) {
            lvgl_port_lock(portMAX_DELAY); // Lock the LVGL Port to ensure no weird threading shenanigans

            lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Show the face
            lv_obj_clear_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN); // Show the percentage

            lv_obj_move_foreground(faceImage); // face goes in the back
            lv_obj_move_foreground(batteryLabel); // label goes on top

            if (battAnimState == 1) {
                battAnimState = 0;
            } else {
                battAnimState = 1;
            }

            lv_label_set_text_fmt(batteryLabel, "Need an SD Card!");


            lv_img_set_src(faceImage, &exclaimationImages[battAnimState]);
            lvgl_port_unlock();
            vTaskDelay(200);
        }

        lvgl_port_lock(portMAX_DELAY);
        lv_obj_add_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Blank out the face for the reboot
        lv_obj_add_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN); // Blank out the label for the reboot
        lvgl_port_unlock();

        vTaskDelay(200);

        esp_restart(); // Reboot the device once an sd card is inserted
    }

    ///////////////////////////////////////////////////////////////////////////////////
    //  SSCMA Initializer
    ///////////////////////////////////////////////////////////////////////////////////

    if (sscma_client_register_callback(client, &sscmanocallback, NULL) != ESP_OK)
    {
        printf("set callback failed\n");
        abort();
    }

    sscma_client_init(client);
    sscma_client_set_model(client, 1);

    sscma_client_set_sensor(client, 1, CAMERA_SENSOR_RESOLUTION_416_416, true);
    vTaskDelay(10 / portTICK_PERIOD_MS);

    if (sscma_client_sample(client, -1) != ESP_OK)
    {
        printf("sample failed\n");
    }


    ///////////////////////////////////////////////////////////////////////////////////
    //  MAIN LOOP
    ///////////////////////////////////////////////////////////////////////////////////

    while (1) {

        printf("free_heap_size = %ld\n", esp_get_free_heap_size());

        ///////////////////////////////////////////////////////////////////////////////////
        //  Sleep Timer
        ///////////////////////////////////////////////////////////////////////////////////

        if (sleepCanceller)
        {
            faceSleepTimer = randomInRange(50, 70); // reset the sleep timer
            if (state == 1) // If we are in the FaceState
            {
                if (faceState == 3) // If asleep
                {
                    faceState = 0; // Wake up
                }
            }


            sleepCanceller = false;
        }

        ///////////////////////////////////////////////////////////////////////////////////
        //  Input Handling
        ///////////////////////////////////////////////////////////////////////////////////

        battPercent = bsp_battery_get_percent(); // Update the battery percentage

        previousPoint = pointTouched;
        lv_indev_get_point(tp, &pointTouched); // Get touch
        if ((pointTouched.x != previousPoint.x) && (pointTouched.y != previousPoint.y)) // If it's a new touch
        {
            sleepCanceller = true;
            if (state == 1)
            {
                faceState = 4;
                happyAnimState = 0;
            }
        }

        // If the scroll wheel button is pressed
        if (bsp_exp_io_get_level(BSP_KNOB_BTN) == 0) {
            sleepCanceller = true;
        }

        if (knobJustMoved)
        {
            state = -1; // Go to Menu
            sleepCanceller = true;
            knobJustMoved = false;
            menuTimeOut = 0;
            menuShouldMove = true;
        }

        ///////////////////////////////////////////////////////////////////////////////////
        //  Battery Low Warning
        ///////////////////////////////////////////////////////////////////////////////////

        if (battPercent < 3) { // Battery too low, block all further information
            lvgl_port_lock(portMAX_DELAY); // Lock the LVGL Port to ensure no weird threading shenanigans

            bsp_lcd_brightness_set(5); // dim the screen

            lv_obj_add_flag(menuLabel, LV_OBJ_FLAG_HIDDEN); // hide the menu label
            lv_obj_add_flag(batteryArc, LV_OBJ_FLAG_HIDDEN); // hide the battery arc
            lv_obj_add_flag(selectorArc, LV_OBJ_FLAG_HIDDEN); // hide the selection arc

            lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Show the face
            lv_obj_clear_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN); // Show the percentage

            lv_obj_move_foreground(faceImage); // face goes in the back
            lv_obj_move_foreground(batteryLabel); // label goes in the middle

            if (battAnimState == 1) {
                battAnimState = 0;
            } else {
                battAnimState = 1;
            }

            lv_img_set_src(faceImage, &exclaimationImages[battAnimState]);
            lv_label_set_text_fmt(batteryLabel, "Battery: %d%%", battPercent);

            lvgl_port_unlock();
            vTaskDelay(100);

            state = 0; // Go to init state
            menuShouldMove = false;

            continue;
        }
        else {
            lv_obj_add_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // hide the face
            lv_obj_add_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN); // hide the battery label
        }


        ///////////////////////////////////////////////////////////////////////////////////
        //  Main state machine
        ///////////////////////////////////////////////////////////////////////////////////

        switch (state) {
            case 6: // Camera mode
                lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Show the main image
                if (captureStartedCounter != -1) { // capture in progress
                    if (captureFinished) {
                        captureFinished = false;
                        captureStartedCounter = -1; // Capture idle
                        captureInProgress = false;
                        sscma_client_set_sensor(client, 1, CAMERA_SENSOR_RESOLUTION_416_416, true);
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                        if (sscma_client_sample(client, -1) != ESP_OK)
                        {
                            printf("sample failed\n");
                        }
                        printf("Capture Done!");
                    } else {
                        printf("Waiting on capture");
                    }
                }

                if (bsp_exp_io_get_level(BSP_KNOB_BTN) == 0) {
                    if (captureStartedCounter == -1) { // Capture idle
                        printf("Capture scheduled");

                        sscma_client_set_sensor(client, 1, CAMERA_SENSOR_RESOLUTION_640_480, true);
                        vTaskDelay(10 / portTICK_PERIOD_MS);
                        if (sscma_client_sample(client, -1) != ESP_OK)
                        {
                            printf("sample failed\n");
                        }
                        captureStartedCounter = 1; // Capture started
                        captureFinished = false;
                    }
                }

                vTaskDelay(10 / portTICK_PERIOD_MS);
                break;
            case -2: // Camera init
                lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Show the main image
                if (sscma_client_register_callback(client, &sscmacallback, NULL) != ESP_OK)
                {
                    printf("set callback failed\n");
                    abort();
                }
                state = 6;
                break;
            case 5: // Screen off
                lvgl_port_lock(portMAX_DELAY);
                bsp_lcd_brightness_set(0);
                lvgl_port_unlock();
                break;
            case 4: // Battery information
                lvgl_port_lock(portMAX_DELAY); // Lock the LVGL Port to ensure no weird threading shenanigans

                lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Show the face
                lv_obj_clear_flag(batteryArc, LV_OBJ_FLAG_HIDDEN); // Show the arc
                lv_obj_clear_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN);

                lv_obj_move_foreground(faceImage); // face goes in the back
                lv_obj_move_foreground(batteryLabel); // label goes in the middle
                lv_obj_move_foreground(batteryArc); // battery arc goes on top

                if (battAnimState == 1) {
                    battAnimState = 0;
                } else {
                    battAnimState = 1;
                }

                if (battPercent > 75) {
                    lv_obj_set_style_arc_color(batteryArc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    lv_img_set_src(faceImage, &greetingImages[battAnimState]);
                } else if (battPercent > 25) {
                    lv_obj_set_style_arc_color(batteryArc, lv_palette_main(LV_PALETTE_YELLOW), LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    lv_img_set_src(faceImage, &idleImages[0]);
                } else {
                    lv_obj_set_style_arc_color(batteryArc, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR | LV_STATE_DEFAULT);
                    lv_img_set_src(faceImage, &exclaimationImages[battAnimState]);
                }

                lv_label_set_text_fmt(batteryLabel, "Battery: %d%%", battPercent);

                lv_arc_set_value(batteryArc, battPercent);

                lvgl_port_unlock();
                vTaskDelay(200);

                break;
            case 3: //Flashlight
                lvgl_port_lock(portMAX_DELAY); // Lock the LVGL Port to ensure no weird threading shenanigans

                lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Show the face

                if (bsp_exp_io_get_level(BSP_KNOB_BTN) == 0) {
                    rgb_next_color(false);
                }
                else
                {
                    rgb_set(255, 255, 255);
                }

                scanAnimState++;
                if (scanAnimState >= 5)
                {
                    scanAnimState = 0;
                }

                lv_img_set_src(faceImage, &scanningImages[scanAnimState]);

                lvgl_port_unlock();

                vTaskDelay(120);

                break;
            case 2: // shutdown
                bsp_lcd_brightness_set(0);
                bsp_system_shutdown();
                vTaskDelay(pdMS_TO_TICKS(1000)); // This will only run if we are connected to usb-c (or shutdown is broken somehow)
                esp_restart();
                break;
            case 1: // Face (active)

                lv_obj_clear_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // Show the face

                // Sleep manager
                if (goToSleep)
                {
                    faceState = 3;
                    goToSleep = false;
                }

                lvgl_port_lock(portMAX_DELAY); // Lock the LVGL Port to ensure no weird threading shenanigans

                switch (faceState) {
                    case 1: // Idle, Face selected
                        if (randomInRange(0, 39) == 1) // If we roll the 1/40 chance to blink
                        {
                            faceState = 2;
                            faceStateTimer = -5;
                            lv_img_set_src(faceImage, &eyesclosed);
                        }
                        break;
                    case 2: // Blinking
                        break; // (Do nothing)
                    case 3: // Asleep
                        sleepAnimState++;
                        if (sleepAnimState >= 4)
                        {
                            sleepAnimState = 0;
                        }

                        faceStateTimer = 5; // This state is perpetual, and lasts until an external "Wakeup" is triggered

                        lv_img_set_src(faceImage, &sleepImages[sleepAnimState]);
                        break;
                    case 4: // Happy
                        happyAnimState++;
                        faceStateTimer = 5; // Keep this state running until the animation is done
                        if (happyAnimState >= 3) // Animate the happy until the animation finishes
                        {
                            happyAnimState = 0;
                            faceState = 0;
                        }
                        lv_img_set_src(faceImage, &greetingImages[happyAnimState]);
                        break;
                    default: // Unknown state
                    case 0:  // Default state (Idle face selection)
                        faceState = 1;
                        faceStateTimer = randomInRange(5, 25); // Face selection delay
                        lv_img_set_src(faceImage, &idleImages[randomInRange(0, 2)]); // set face to a random idle image

                        if (faceSleepTimer <= 0)
                        {
                            goToSleep = true;
                        }

                        break;
                }

                if (faceStateTimer <= 0) // If faceStateTimer is finished
                {
                    faceState = 0; // Select a new Idle Face
                }
                else
                {
                    faceStateTimer--; //Decrement stateTimer
                }

                lvgl_port_unlock();

                vTaskDelay(150);


                faceSleepTimer--;

                break;
            case 0: // Initializing
            default: //or an unknown state
                lvgl_port_lock(portMAX_DELAY);
                bsp_lcd_brightness_set(100);
                lvgl_port_unlock();
                state = 1;
                break;
            case -1:
                ///////////////////////////////////////////////////////////////////////////////
                //  Menu code
                ///////////////////////////////////////////////////////////////////////////////

                bsp_lcd_brightness_set(100); // reset the screen brightness

                rgb_set(0, 0, 0); // Reset the RGB LED

                faceState = 0; // Reset the face state

                menuTimeOut += 3;

                if (sscma_client_register_callback(client, &sscmanocallback, NULL) != ESP_OK)
                {
                    printf("set callback failed\n");
                    abort();
                }

                lvgl_port_lock(portMAX_DELAY); // Lock the LVGL Port
                lv_obj_add_flag(faceImage, LV_OBJ_FLAG_HIDDEN); // hide the face
                lv_obj_add_flag(batteryArc, LV_OBJ_FLAG_HIDDEN); // hide the battery arc
                lv_obj_add_flag(batteryLabel, LV_OBJ_FLAG_HIDDEN); // hide the battery label


                lv_obj_clear_flag(selectorArc, LV_OBJ_FLAG_HIDDEN); // Show the selector arc
                lv_obj_clear_flag(menuLabel, LV_OBJ_FLAG_HIDDEN); // Show the menu label

                lv_obj_move_foreground(menuLabel); // Menu label goes in the middle
                lv_obj_move_foreground(selectorArc); // Selector arc goes on top!

                lv_arc_set_value(selectorArc, menuTimeOut); // Fill in the selector arc
                if (menuTimeOut >= 100) {
                    menuTimeOut = 100;
                    switch (menuSelection) {
                        case 6:
                            state = -2;
                            break;
                        default:
                            state = menuSelection;
                            break;
                    }
                    lv_obj_add_flag(selectorArc, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(menuLabel, LV_OBJ_FLAG_HIDDEN); // hide the menu label
                }

                if (menuShouldMove){
                    if (knobDir) { // Right
                        menuSelection++;
                    }
                    else { // Left
                        menuSelection--;
                    }

                    if (menuSelection >= 7)
                    {
                        menuSelection = 1;
                    }
                    if (menuSelection <= 0)
                    {
                        menuSelection = 6;
                    }

                    switch (menuSelection) {
                        case 6:
                            lv_label_set_text(menuLabel,"#ffffff Camera#");
                            break;
                        case 5:
                            lv_label_set_text(menuLabel,"#ffffff Screen Off#");
                            break;
                        case 4:
                            lv_label_set_text(menuLabel,"#ffffff Battery Info#");
                            break;
                        case 3:
                            lv_label_set_text(menuLabel,"#ffffff Flashlight#");
                            break;
                        case 2:
                            lv_label_set_text(menuLabel,"#ff0000 Shutdown#");
                            break;
                        case 1:
                            lv_label_set_text(menuLabel,"#ffffff Normal#");
                            break;
                        case 0:
                        default:
                            lv_label_set_text(menuLabel,"#ff0000 Error#");
                            break;
                    }

                    menuShouldMove = false;
                }
                lvgl_port_unlock(); // Free the LVGL Port

                vTaskDelay(15); // Delay
                break;
        }
    }
}

