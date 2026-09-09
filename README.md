# Datalogger ESP32-S3

Proyecto basado en ESP-IDF para capturar, guardar y retransmitir datos UART provenientes de una celda de carga o equipo compatible con el protocolo de Alert Technologies.

## Descripción breve

- recibe datos por UART
- detecta la cabecera del equipo
- parsea tramas con una máquina de estados
- acumula los datos en RAM
- guarda los datos en una partición FAT con wear leveling
- conserva los datos iniciales y compacta las secuencias del loop después de detectar su cabecera
- puede exponerse como pendrive USB mediante el conector USB OTG del ESP32-S3
- puede retransmitir el log por UART
- muestra los estados y avisos del sistema en un OLED SSD1306
- mide el voltaje de la batería mediante el ADC
- informa el voltaje de la batería en el monitor serie cada 5 segundos
- muestra el voltaje y el estado de la batería en un OLED SSD1306 de 128x64

## Requisitos

- ESP-IDF v6.1 o compatible
- Python 3.11
- VS Code con soporte para ESP-IDF o terminal PowerShell
- placa ESP32-S3
- conexión UART con la celda de carga

## Compilar

Desde la carpeta del proyecto:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py build
```

## Flashear

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py -p COM15 flash
```

Ajusta COM15 al puerto real de tu placa.

## Modos de funcionamiento

Después de cada reinicio, el firmware arranca como datalogger y espera hasta 5
segundos la cabecera de la celda (`Alert Technologies...Version`). Si recibe la
cabecera, continúa capturando datos por UART y los guarda en
`/archivos/log_uart.txt`. Si no la recibe, cierra la captura y cambia
automáticamente al modo USB.

El cambio a USB se realiza únicamente de forma automática cuando no se recibe la
cabecera durante esos 5 segundos. No existe un comando manual para cambiar a USB.

### Uso como pendrive

El firmware expone la partición `archivos` como una unidad FAT por USB Mass Storage.
Conecta el cable al puerto USB nativo/OTG del ESP32-S3, no al USB-UART, y el sistema
operativo debería mostrar una unidad extraíble de aproximadamente 1,8 MB.

Al entrar en modo USB, el logger detiene las tareas de captura y no vuelve a
acceder a la partición mientras el ordenador la usa, evitando corrupción del
sistema de archivos.

En este modo no se muestra ni se actualiza el voltaje de batería en el OLED.

Para volver a registrar, desconecta el USB y conecta la celda de carga.

La primera vez que se prepara una partición FAT nueva hay que flashear el firmware
y la imagen de datos:

```powershell
idf.py -p COM15 flash
```

Este comando también debe flashear `build/archivos.bin`, generado desde la
carpeta `archivos`. La imagen contiene el sistema FAT inicial; el firmware no
formatea automáticamente la partición para evitar perder los archivos existentes.

## Monitor serie

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py -p COM15 monitor
```

## Funcionamiento esperado

- `SECUENCIA ACTIVA`: se recibió la primera trama de la celda
- `TECNOLOGIA DETECTADA`: se detectó la cabecera del equipo
- `SIN DATOS UART`: no están llegando datos de la celda durante más de 30 s
- `BAT: ... V`: muestra el voltaje en la primera línea del OLED
- Cada trama procesada se muestra debajo de la línea de batería

## Formato del archivo

Antes de detectar el loop se conserva el flujo UART recibido para diagnóstico.
Después de la cabecera:

```text
"Running:"
"   seq #","ld cella","     dac","    temp","    tare"
	00001,     01970,     00752,     02132,         0
```

Cada registro del loop se reconstruye mediante el parser y se guarda compactado
en `/archivos/log_uart.txt`, en lugar de copiar el bloque UART crudo.

## Medición de batería

- Entrada ADC: `GPIO5` (`ADC1_CH4` en ESP32-S3)
- Relación configurada del divisor resistivo: `2.03`
- La lectura se calibra con el ADC del ESP-IDF cuando hay calibración disponible
- El valor se muestra en el monitor serie en milivoltios y voltios

La relación del divisor debe coincidir con el circuito instalado. Se configura en `BAT_VOLTAGE_DIVIDER_RATIO` dentro de [main/hardware.h](main/hardware.h).

## Display OLED SSD1306

El proyecto admite el módulo OLED SSD1306 monocromático de 0.96 pulgadas, resolución 128x64, con interfaz I2C.

| Módulo OLED | ESP32-S3 |
| --- | --- |
| VCC | 3.3 V |
| GND | GND |
| SDA | GPIO11 |
| SCL | GPIO12 |

- Dirección I2C configurada: `0x3C`
- Velocidad I2C: 400 kHz
- La primera línea del OLED muestra `BAT:` y el voltaje medido
- Las líneas restantes muestran el contenido de la última trama procesada
- En modo USB no se muestra la línea de batería
- Si el módulo utiliza la dirección `0x3D`, cambia `OLED_I2C_ADDRESS` en [main/hardware.h](main/hardware.h)
- Verifica que el módulo sea de 3.3 V o utiliza adaptación de nivel si requiere 5 V

## Archivos importantes

- [CMakeLists.txt](CMakeLists.txt): configuración del proyecto
- [main/main.c](main/main.c): lógica principal del datalogger
- [main/hardware.c](main/hardware.c): inicialización del hardware
- [main/hardware.h](main/hardware.h): pines y configuración hardware
- [partitions.csv](partitions.csv): partición FAT expuesta por USB MSC

## Historico de versiones

### Versión 25
Se probó con display ST7789 240x240. Consumo total aproximado 120 mA.

### Versión 27
Display apagado. Consumo aproximado 85 mA.

### Versión 28
Se añadió señalización de estados de la celda.

### Versión 29
Se eliminaron las rutinas de LVGL y se mantiene el almacenamiento en LittleFS.

### Versión 30
Se añadió la medición de batería por ADC en GPIO5. El voltaje se registra cada 5 segundos y se muestra en el OLED.

### Versión 31
Se añadió soporte para el display OLED SSD1306 128x64 por I2C. Muestra el voltaje y el estado de la batería.

### Versión actual
Se añadió el cambio automático a USB después de 5 segundos sin recibir la
cabecera de la celda. Se eliminó el comando manual `usb`, se corrigió el cierre
de las tareas FreeRTOS al entrar en USB y se ajustó el registro para compactar
las tramas del loop. En modo USB se oculta el voltaje de batería del OLED.


















