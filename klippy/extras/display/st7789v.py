import logging
from .. import bus
from . import display_util
import collections # Для буферов
import math

# Константы для Klipper API
BACKGROUND_PRIORITY_CLOCK = 0x7fffffff00000000

# Размеры дисплея ST7789V
ST7789V_WIDTH = 240
ST7789V_HEIGHT = 320 

# Размер полосы для обновления (8 строк, RGB565 = 2 байта/пиксель)
# 240 * 8 строк * 2 байта/пиксель = 3840 байт
STRIP_HEIGHT = 8
STRIP_BUFFER_SIZE = ST7789V_WIDTH * STRIP_HEIGHT * 2 


class ST7789V:
    def __init__(self, config):
        self.printer = config.get_printer()
        self.reactor = self.printer.get_reactor()
        
        # 1. Сбор пинов и SPI
        self.spi = bus.MCU_SPI_from_config(config, 0, default_speed=32000000)
        self.mcu = self.spi.get_mcu()
        self.oid = self.mcu.create_oid()

        # Получаем строковые имена пинов
        self.dc_pin_name = config.get('dc_pin') 
        self.cs_pin_name = config.get('cs_pin')
        self.bl_pin_name = config.get('backlight_pin')
        
        # NOTE: Имя шины SPI1 (ID=1) передано в C-код
        self.spi_bus_id = 1 
        self.spi_rate = config.getint('spi_speed', 32000000, minval=1000000)
        
        # 2. Буферы для отрисовки
        self.x_max, self.y_max = ST7789V_WIDTH, ST7789V_HEIGHT
        
        # Буфер полосы (strip buffer) и буфер для сравнения (cached buffer)
        self.buffer = bytearray(STRIP_BUFFER_SIZE)
        self.cached_buffer = bytearray(STRIP_BUFFER_SIZE)

        # 3. Регистрация команд MCU
        self.mcu.register_config_callback(self.build_config)
        self.lcd_chip = display_util.DisplayChip(self, config)

        # Переменные состояния отрисовки
        self.current_strip = 0
        self.total_strips = math.ceil(self.y_max / STRIP_HEIGHT)
        self.last_flush_time = 0.0

    def build_config(self):
        # 1. Вызов C-функции для конфигурации дисплея
        self.mcu.add_config_cmd(
             "config_st7789_display oid=%u dc_pin=%s cs_pin=%s bl_pin=%s spi_bus=%u spi_rate=%u"
             % (self.oid, self.dc_pin_name, self.cs_pin_name, self.bl_pin_name,
                self.spi_bus_id, self.spi_rate))

        # 2. Поиск C-функций для управления графикой
        cmd_queue = self.mcu.alloc_command_queue()
        
        # NOTE: Мы ищем команды, которые мы зарегистрировали в lcd_st7789v.c
        self.set_window_cmd = self.mcu.lookup_command(
            "st7789_set_window oid=%c x_start=%hu x_end=%hu y_start=%hu y_end=%hu", cq=cmd_queue)
            
        self.write_data_cmd = self.mcu.lookup_command(
            "st7789_write_data oid=%c data=%*s", cq=cmd_queue)

    def send_window(self, x0, y0, x1, y1):
        """Вызов C-функции для установки окна рисования (CASET/RASET)."""
        # NOTE: Координаты Y должны быть правильно скорректированы для ST7789
        self.set_window_cmd.send([self.oid, x0, x1, y0, y1], reqclock=BACKGROUND_PRIORITY_CLOCK)

    def send_data(self, data_buffer):
        """Вызов C-функции для потоковой передачи пиксельных данных (RAMWR)."""
        self.write_data_cmd.send([self.oid, data_buffer], 
                                 reqclock=BACKGROUND_PRIORITY_CLOCK)

    def flush(self):
        """Метод, вызываемый Klipper для обновления экрана."""
        # 1. Определение текущей полосы (0 до total_strips-1)
        self.current_strip = (self.current_strip + 1) % self.total_strips
        
        # Координаты Y для текущей полосы
        y_start = self.current_strip * STRIP_HEIGHT
        y_end = min(y_start + STRIP_HEIGHT - 1, self.y_max - 1)
        
        # 2. Режим отрисовки: 
        # NOTE: Здесь должен быть код, который заполняет self.buffer пиксельными данными. 
        # Поскольку у нас нет Jinja2-рендерера, мы просто отправляем буфер.
        
        # 3. Отправка команды установки окна
        self.send_window(0, y_start, self.x_max - 1, y_end)
        
        # 4. Отправка данных полосы
        # NOTE: В реальной реализации здесь нужно отправлять только измененные данные
        self.send_data(self.buffer) 


    # Заглушки для совместимости с PrinterLCD
    def init(self):
        # Инициализация дисплея выполняется в C-коде через build_config
        pass 
        
    def get_max_duration(self):
        # Дисплей ST7789V очень быстрый, но передача по SPI занимает время.
        return 0.1 # 100 мс для обновления экрана

    def get_dimensions(self):
        return (ST7789V_WIDTH, ST7789V_HEIGHT)

# Регистрация драйвера в Klipper
def load_config(config):
    # Используем DisplayChip для совместимости с основной логикой Klipper
    return display_util.DisplayChip(ST7789V(config), config)