#include "board/gpio.h"    // Для GPIO, struct spi_config, gpio_transfer
#include "command.h"       // Для oid_alloc, oid_lookup, DECL_COMMAND
#include "sched.h"         // Для sched_sleep (задержки)

// --- ПРЕДВАРИТЕЛЬНЫЕ ДЕКЛАРАЦИИ Klipper API ---

// 1. Декларации функций SPI (из spi.c, объявлены в gpio.h)
struct spi_config spi_setup(uint32_t bus, uint8_t mode, uint32_t rate);
void spi_prepare(struct spi_config config);
void spi_transfer(struct spi_config config, uint8_t receive_data,
                  uint8_t len, uint8_t *data);

// 2. Декларации функций для API Klipper
void command_config_st7789_display(uint32_t *args); 
void command_st7789_set_window(uint32_t *args);
void command_st7789_write_data(uint32_t *args);

// Декларация API LCD (для register_display_lcd)
struct display_lcd; 
void register_display_lcd(struct display_lcd *lcd); 

// Макрос для корректной передачи типа в oid_lookup/alloc
#define ST7789_OID_TYPE command_config_st7789_display


// --- КОНСТАНТЫ И ПРОТОКОЛ ST7789V (Портировано из Marlin) ---

#define ST7789V_CMD   0
#define ST7789V_DATA  1

// Основные команды, используемые для инициализации и записи
#define ST7789V_SWRESET 0x01
#define ST7789V_SLPOUT  0x11
#define ST7789V_DISPON  0x29
#define ST7789V_CASET   0x2A
#define ST7789V_RASET   0x2B
#define ST7789V_RAMWR   0x2C
#define ST7789V_COLMOD  0x3A
#define ST7789V_MADCTL  0x36
#define ST7789V_PORCTRL 0xB2
#define ST7789V_GCTRL   0xB7
#define ST7789V_VCOMS   0xBB
#define ST7789V_LCMCTRL 0xC0
#define ST7789V_VDVVRHEN 0xC2
#define ST7789V_VDVS    0xC4
#define ST7789V_FRCTRL2 0xC6
#define ST7789V_PWCTRL1 0xD0
#define ST7789V_NORON   0x13


// --- СТРУКТУРА ДРАЙВЕРА ---

struct st7789_lcd {
    // struct display_lcd base; // Удалено, т.к. не используется в архитектуре Klipper
    uint32_t last_cmd_time;
    struct spi_config spi_config;
    struct gpio_out dc_pin;  // Data/Command (PA6)
    struct gpio_out cs_pin;  // Chip Select (PA4)
    struct gpio_out bl_pin;  // Backlight (PC0)
};


// --- НИЗКОУРОВНЕВЫЕ ФУНКЦИИ ---

// Функция задержки в миллисекундах (использует API активного ожидания Klipper)
static void
mhz_delay(double ms)
{
    // Вставьте mhz_delay() или udelay(), как мы обсуждали ранее. 
    // Для простоты используем sched_sleep, добавив его декларацию.
    sched_sleep(ms / 1000.0); 
}

// Функция отправки одного байта команды/данных
static void
st7789_xmit(struct st7789_lcd *lcd, uint8_t is_data, uint8_t data)
{
    gpio_out_write(lcd->dc_pin, is_data); 
    gpio_out_write(lcd->cs_pin, 0); 
    spi_prepare(lcd->spi_config);
    spi_transfer(lcd->spi_config, 0, 1, &data);
    gpio_out_write(lcd->cs_pin, 1);
}

// Функция отправки массива данных (для пикселей)
static void
st7789_xmit_buffer(struct st7789_lcd *lcd, uint8_t is_data, uint8_t *data, uint16_t len)
{
    gpio_out_write(lcd->dc_pin, is_data); 
    gpio_out_write(lcd->cs_pin, 0); 
    spi_prepare(lcd->spi_config);
    spi_transfer(lcd->spi_config, 0, len, data);
    gpio_out_write(lcd->cs_pin, 1);
}


// --- ИНИЦИАЛИЗАЦИЯ РЕГИСТРОВ (ПОРТИРОВАНО ИЗ MARLIN) ---

static void
st7789_init_registers(struct st7789_lcd *lcd)
{
    // SWRESET (0x01) и SLPOUT (0x11)
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_SWRESET); 
    sched_sleep(0.005); // 5 мс
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_SLPOUT); 
    sched_sleep(0.120); // 120 мс

    // PORCTRL (0xB2) - Porch Setting
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_PORCTRL); 
    st7789_xmit(lcd, ST7789V_DATA, 0x0C); st7789_xmit(lcd, ST7789V_DATA, 0x0C); 
    st7789_xmit(lcd, ST7789V_DATA, 0x00); st7789_xmit(lcd, ST7789V_DATA, 0x33); 
    st7789_xmit(lcd, ST7789V_DATA, 0x33); 

    // GCTRL (0xB7) - Gate Control
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_GCTRL); 
    st7789_xmit(lcd, ST7789V_DATA, 0x35); 

    // VCOMS (0xBB) - VCOM Setting
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_VCOMS); 
    st7789_xmit(lcd, ST7789V_DATA, 0x1F); 

    // LCMCTRL (0xC0) - LCM Control
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_LCMCTRL); 
    st7789_xmit(lcd, ST7789V_DATA, 0x2C); 

    // VDVVRHEN (0xC2), VDVS (0xC4)
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_VDVVRHEN); st7789_xmit(lcd, ST7789V_DATA, 0x01); 
    st7789_xmit(lcd, ST7789V_CMD, 0xC3); st7789_xmit(lcd, ST7789V_DATA, 0xC3); // VRH Set
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_VDVS); st7789_xmit(lcd, ST7789V_DATA, 0x20); 

    // FRCTRL2 (0xC6), PWCTRL1 (0xD0)
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_FRCTRL2); st7789_xmit(lcd, ST7789V_DATA, 0x0F); 
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_PWCTRL1); st7789_xmit(lcd, ST7789V_DATA, 0xA4); 
    st7789_xmit(lcd, ST7789V_DATA, 0xA1); 

    // MADCTL (0x36) - Orientation (Ориентация)
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_MADCTL); 
    st7789_xmit(lcd, ST7789V_DATA, 0x00); // 0x00 или 0xB0 в зависимости от желаемой ориентации

    // COLMOD (0x3A) - 16-бит цвет
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_COLMOD); 
    st7789_xmit(lcd, ST7789V_DATA, 0x05); // 0x05 для RGB565

    // NORON (0x13), DISPON (0x29)
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_NORON); 
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_DISPON); 
    
    // Включение подсветки (PC0)
    gpio_out_write(lcd->bl_pin, 1);
}


// --- ФУНКЦИИ ОБНОВЛЕНИЯ ГРАФИКИ (ВЫЗЫВАЮТСЯ PYTHON) ---

void
command_st7789_set_window(uint32_t *args)
{
    struct st7789_lcd *lcd = oid_lookup(args[0], ST7789_OID_TYPE);
    uint16_t x_start = args[1], x_end = args[2], y_start = args[3], y_end = args[4];

    // Установка адреса столбцов, CASET (0x2A)
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_CASET);
    st7789_xmit(lcd, ST7789V_DATA, x_start >> 8);
    st7789_xmit(lcd, ST7789V_DATA, x_start & 0xFF);
    st7789_xmit(lcd, ST7789V_DATA, x_end >> 8);
    st7789_xmit(lcd, ST7789V_DATA, x_end & 0xFF);

    // Установка адреса строк, RASET (0x2B)
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_RASET);
    st7789_xmit(lcd, ST7789V_DATA, y_start >> 8);
    st7789_xmit(lcd, ST7789V_DATA, y_start & 0xFF);
    st7789_xmit(lcd, ST7789V_DATA, y_end >> 8);
    st7789_xmit(lcd, ST7789V_DATA, y_end & 0xFF);
    
    // Команда записи в память RAMWR (0x2C) - готова к приему данных
    st7789_xmit(lcd, ST7789V_CMD, ST7789V_RAMWR);
}
DECL_COMMAND(command_st7789_set_window,
             "st7789_set_window oid=%c x_start=%hu x_end=%hu y_start=%hu y_end=%hu");

void
command_st7789_write_data(uint32_t *args)
{
    struct st7789_lcd *lcd = oid_lookup(args[0], ST7789_OID_TYPE);
    uint16_t len = args[1];
    uint8_t *data = command_decode_ptr(args[2]);

    // Отправка потока данных
    st7789_xmit_buffer(lcd, ST7789V_DATA, data, len);
}
DECL_COMMAND(command_st7789_write_data, "st7789_write_data oid=%c data=%*s");


// --- РЕГИСТРАЦИЯ КОНФИГУРАЦИИ ---

void
command_config_st7789_display(uint32_t *args)
{
    // ОID, DC_PIN, CS_PIN, BL_PIN, SPI_BUS_ID, SPI_RATE
    struct st7789_lcd *lcd = oid_alloc(args[0], ST7789_OID_TYPE, sizeof(*lcd));
    
    // Инициализация пинов GPIO (args[1]...args[3])
    lcd->dc_pin = gpio_out_setup(args[1], 0); 
    lcd->cs_pin = gpio_out_setup(args[2], 1); 
    lcd->bl_pin = gpio_out_setup(args[3], 0); 

    // Инициализация аппаратного SPI
    uint32_t spi_bus = args[4];
    uint32_t spi_rate = args[5];
    lcd->spi_config = spi_setup(spi_bus, 0, spi_rate); // mode 0

    st7789_init_registers(lcd);

    // В Klipper объект регистрируется через oid_alloc
    // register_display_lcd((struct display_lcd *)lcd); // Эта строка не нужна
}
DECL_COMMAND(command_config_st7789_display,
             "config_st7789_display oid=%c dc_pin=%u cs_pin=%u bl_pin=%u spi_bus=%u spi_rate=%u");

// --- SHUTDOWN ---

void
st7789_shutdown(void)
{
    // Отправка команды DISPOFF (0x28) и выключение подсветки
}
DECL_SHUTDOWN(st7789_shutdown);