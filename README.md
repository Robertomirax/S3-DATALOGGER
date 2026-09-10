# Datalogger ESP32-S3

Proyecto basado en ESP-IDF para capturar, guardar y exponer datos UART de una celda de carga o equipo compatible con el protocolo de Alert Technologies. La versión actual del firmware está orientada a una placa ESP32-S3 con almacenamiento FAT en flash, USB MSC y visualización OLED SSD1306.

## Resumen del sistema

- Captura de datos por UART con la celda de carga.
- Detección de cabecera y parsing de tramas con máquina de estados.
- Almacenamiento compacto en la partición FAT `/archivos`.
- Generación automática de la imagen FAT desde la carpeta `archivos` con `fatfs_create_spiflash_image`.
- Modo USB automático cuando no llega la cabecera durante 5 segundos.
- Exposición de la partición como pendrive por USB MSC (TinyUSB).
- Actualización OTA desde un archivo `firmware.bin` colocado en la raíz del pendrive.
- Monitor serie con voltaje de batería cada 5 segundos.
- OLED SSD1306 de 128x64 con batería y estado del sistema.

## Configuración actual del hardware

La implementación actual del código define estos valores:

- UART del logger: `UART_NUM_1`
- TX: `GPIO17`
- RX: `GPIO18`
- Velocidad: `300 bauds`
- ADC de batería: `GPIO5` (`ADC1_CH4`)
- Divisor de batería: `2.03`
- OLED: SSD1306 monocromo 128x64
- SDA: `GPIO11`
- SCL: `GPIO12`
- Dirección I2C: `0x3C`
- Velocidad I2C: `400 kHz`

Estos valores quedan centralizados en [main/hardware.h](main/hardware.h).

## Requisitos

- ESP-IDF 6.1 (ruta prevista: `C:\esp\v6.1\esp-idf`)
- Python 3.11
- PowerShell o terminal con soporte de ESP-IDF
- VS Code con la extensión ESP-IDF o acceso directo a `idf.py`
- Placa ESP32-S3 con conector USB nativo para programación y USB MSC
- Celda de carga o equipo Alert Technologies conectado al UART

## Configuración del proyecto

El proyecto usa una raíz CMake con la creación automática de la imagen FAT:

```cmake
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(S3-DATALOGGER)

fatfs_create_spiflash_image(archivos archivos FLASH_IN_PROJECT)
```

La partición `archivos` se define en [partitions.csv](partitions.csv) con el siguiente esquema:

- `nvs`
- `otadata`
- `ota_0`
- `ota_1`
- `phy`
- `archivos` como `fat` en la zona final del flash

La partición FAT se monta como `/archivos` y se usa para guardar:

- `/archivos/log_uart.txt`
- `/archivos/firmware.bin`

## Compilar

Desde la carpeta del proyecto:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py build
```

También está disponible el helper del proyecto:

```powershell
. .\tools\setup-esp-idf.ps1
idf.py build
```

## Flashear

Para programar la placa en modo bootloader ROM:

1. Conecta la placa por USB-C.
2. Mantén pulsado `BOOT`.
3. Pulsa `RESET` o `EN`.
4. Suelta `BOOT`.
5. Usa el puerto serial/JTAG que aparezca en Windows.

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py -p COMx flash
```

Sustituye `COMx` por el puerto real de la placa.

> La primera carga completa de la placa debe incluir la imagen FAT de la partición. El proyecto ya genera `build/archivos.bin` desde la carpeta `archivos` y esto es necesario para preparar el volumen FAT inicial.

## Modo de funcionamiento

Después de cada reinicio, el firmware arranca en modo datalogger y espera hasta 5 segundos la cabecera de la celda. Si la cabecera no llega, cambia automáticamente a modo USB y expone la partición FAT por Mass Storage.

### Modo logger

- Espera la cabecera `Alert Technologies...Version`.
- Captura datos UART.
- Recolecta y compacta tramas.
- Guarda el resultado en `/archivos/log_uart.txt`.
- Muestra el voltaje en OLED y serie cada 5 segundos.

### Modo USB

- La partición `archivos` se expone como pendrive por USB OTG.
- Se detienen las tareas de captura y acceso al sistema de archivos para evitar corrupción.
- La batería no se muestra en el OLED mientras se está en modo USB.
- La desconexión USB se usa como señal para recuperar la FAT y validar una actualización OTA.

## Actualización OTA desde pendrive

1. Compila el firmware y genera `build/S3-DATALOGGER.bin`.
2. Copia ese archivo al pendrive con el nombre `firmware.bin`.
3. Expulsa la unidad desde Windows.
4. Desconecta el cable USB-C.
5. El sistema detecta la desconexión, valida la imagen y la instala en la OTA alternativa.

Si el archivo no existe o la imagen no es válida, el sistema reinicia sin tocar el firmware actual.

## Monitor serie

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py -p COM15 monitor
```

## Funcionamiento esperado

- `SECUENCIA ACTIVA`: la primera trama de la celda fue detectada.
- `TECNOLOGIA DETECTADA`: se recibió la cabecera del equipo.
- `SIN DATOS UART`: no llegan datos durante más de 30 segundos.
- `BAT: ... V`: voltaje de batería en la primera línea del OLED.
- Las tramas procesadas se muestran debajo de la línea de batería.

## Formato del archivo de log

Antes de detectar el loop se conserva el flujo UART recibido para diagnóstico. Tras reconocer la cabecera, el sistema genera un log compacto:

```text
"Running:"
"   seq #","ld cella","     dac","    temp","    tare"
00001,     01970,     00752,     02132,         0
```

Cada línea se reconstruye desde el parser y se guarda en `/archivos/log_uart.txt` en lugar de registrar el bloque UART crudo completo.

## Datos clave del firmware

- Proyecto: `S3-DATALOGGER`
- Versión de firmware en pantalla: `FIRM 35`
- Varios de estado y UI se gestionan desde [main/main.c](main/main.c)
- Inicialización de hardware, periféricos y USB MSC en [main/hardware.c](main/hardware.c)
- Pines, ADC y configuración OLED en [main/hardware.h](main/hardware.h)

## Historial de versiones

### Versión 25
Se probó con display ST7789 de 240x240. Consumo aproximado: 120 mA.

### Versión 27
Display apagado. Consumo aproximado: 85 mA.

### Versión 28
Se añadió señalización de estados de la celda.

### Versión 29
Se eliminaron las rutinas de LVGL y se mantiene almacenamiento en LittleFS.

### Versión 30
Se añadió medida de batería por ADC en GPIO5. El voltaje se registra cada 5 segundos y se muestra en el OLED.

### Versión 31
Se añadió soporte para display OLED SSD1306 128x64 por I2C. Muestra voltaje y estado de la batería.

### Versión actual
Se añadió la transición automática a USB tras 5 segundos sin cabecera de la celda, se eliminó el comando manual `usb`, se corrigió el cierre de tareas al entrar en USB y se ajustó la compactación del log del loop. En modo USB se oculta la batería del OLED.

## Archivos importantes

- [CMakeLists.txt](CMakeLists.txt): configuración principal del proyecto
- [main/CMakeLists.txt](main/CMakeLists.txt): componentes y requerimientos del firmware
- [main/main.c](main/main.c): lógica principal del datalogger
- [main/hardware.c](main/hardware.c): inicialización del hardware y del USB MSC
- [main/hardware.h](main/hardware.h): pines y definiciones hardware
- [partitions.csv](partitions.csv): tabla de particiones del flash
- [archivos/README.md](archivos/README.md): detalle de la partición FAT generada por el proyecto
















