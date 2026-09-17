/*
 * Ukazka pripojeni OLED displeje SH1106 pres 4-vodicove SPI (ESP32-C6, ESP-IDF 6.0)
 *
 * Driver tny-robotics/sh1106-esp-idf je sbernicove neutralni - komunikuje pouze
 * pres esp_lcd_panel_io_handle_t, takze misto esp_lcd_new_panel_io_i2c()
 * staci vytvorit IO handle pomoci esp_lcd_new_panel_io_spi().
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_sh1106.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "rssi_meter";

/* ---- Zapojeni -------------------------------------------------------------
 * Modul SH1106 v SPI rezimu ma 7 vyvodu: VCC, GND, D0 (=SCLK), D1 (=MOSI),
 * RES, DC, CS. Displej se pouze zapisuje, takze MISO neni potreba.
 *
 *   OLED        ESP32-C6
 *   ---------   --------------------
 *   D0 / SCK    GPIO6   (IO MUX pin SPI2 - nejrychlejsi cesta)
 *   D1 / MOSI   GPIO7   (IO MUX pin SPI2)
 *   CS          GPIO10
 *   DC          GPIO11
 *   RES         GPIO18
 *   VCC         3V3
 *   GND         GND
 */
#define LCD_HOST            SPI2_HOST
#define PIN_NUM_SCLK        GPIO_NUM_19
#define PIN_NUM_MOSI        GPIO_NUM_18
#define PIN_NUM_CS          GPIO_NUM_17
#define PIN_NUM_DC          GPIO_NUM_23
#define PIN_NUM_RST         GPIO_NUM_22

/* Datasheet SH1106 udava minimalni periodu hodin 250 ns, tj. max. 4 MHz.
 * Vetsina modulu zvladne i 8-10 MHz, ale 4 MHz je bezpecna vychozi hodnota. */
#define LCD_PIXEL_CLOCK_HZ  (4 * 1000 * 1000)

/* Framebuffer drzime staticky, ne na zasobniku: posledni prenos barevnych dat
 * z esp_lcd_panel_draw_bitmap() bezi pres DMA asynchronne a jeste chvili po
 * navratu funkce cte z tohoto bufferu. */
static uint8_t s_framebuffer[SH1106_BUFFER_SIZE];

void app_main(void)
{
    /* ---- 1) Inicializace SPI sbernice ---------------------------------- */
    spi_bus_config_t bus_config = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = GPIO_NUM_NC,   // z displeje nectem
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        // Nejdelsi jeden prenos je jedna stranka displeje = SH1106_WIDTH bajtu.
        .max_transfer_sz = SH1106_WIDTH,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    /* ---- 2) Panel IO pro SPI ------------------------------------------- */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_NUM_CS,
        .dc_gpio_num = PIN_NUM_DC,
        .spi_mode = 0,                       // SH1106 vzorkuje na nabeznou hranu (CPOL=0, CPHA=0)
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            // DULEZITE: SH1106 posila parametry prikazu (napr. hodnotu kontrastu
            // za 0x81) take s DC=0. Vychozi nastaveni esp_lcd je DC=1 pro
            // parametry, coz by je poslalo do zobrazovaci pameti misto do
            // prikazoveho registru a displej by zustal prazdny.
            .dc_low_on_param = 1,
            // Zbytek staci vychozi: DC=0 pro prikaz, DC=1 pro obrazova data.
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    /* ---- 3) Panel SH1106 ------------------------------------------------ */
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        // Na rozdil od I2C varianty ma SPI modul vyvedeny RES pin.
        .reset_gpio_num = PIN_NUM_RST,
        .bits_per_pixel = 1,                          // monochromaticky displej
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,   // driver nepouziva, vyplnujeme kvuli warningum
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,    // dtto
        .flags = {
            .reset_active_high = false,               // RES je aktivni v L
        },
        .vendor_config = NULL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "SH1106 pres SPI inicializovan (%dx%d)", SH1106_WIDTH, SH1106_HEIGHT);

    /* ---- 4) Test zobrazeni ---------------------------------------------- */

    // Buffer je linearni pole stranek: bajt nese 8 pixelu nad sebou,
    // bit 0 = horni pixel. Rozsvitime rohovy pixel vlevo nahore a vpravo dole.
    memset(s_framebuffer, 0x00, SH1106_BUFFER_SIZE);
    s_framebuffer[0] = 0x01;
    s_framebuffer[SH1106_BUFFER_SIZE - 1] = 0x80;
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, SH1106_WIDTH, SH1106_HEIGHT, s_framebuffer));
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Castecna aktualizace - y hranice musi byt nasobky 8 (stranka displeje).
    memset(s_framebuffer, 0xFF, SH1106_BUFFER_SIZE);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 48, 16, 80, 48, s_framebuffer));
    vTaskDelay(pdMS_TO_TICKS(2000));

    // Blikani celeho displeje
    while (1) {
        memset(s_framebuffer, 0xFF, SH1106_BUFFER_SIZE);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, SH1106_WIDTH, SH1106_HEIGHT, s_framebuffer));
        vTaskDelay(pdMS_TO_TICKS(500));

        memset(s_framebuffer, 0x00, SH1106_BUFFER_SIZE);
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, SH1106_WIDTH, SH1106_HEIGHT, s_framebuffer));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
