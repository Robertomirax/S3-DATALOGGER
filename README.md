# Datalogger ESP32-S3

Proyecto basado en ESP-IDF para capturar, guardar y retransmitir datos UART provenientes de una celda de carga o equipo compatible con el protocolo de Alert Technologies.

## Descripción breve

- recibe datos por UART
- detecta la cabecera del equipo
- parsea tramas con una máquina de estados
- acumula los datos en RAM
- vuelca a la partición LittleFS
- puede retransmitir el log por UART
- usa un LED WS2812B para indicar estado visual

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

## Monitor serie

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py -p COM15 monitor
```

## Funcionamiento esperado

- LED rojo fijo: sistema esperando la llegada de la cabecera de la celda
- LED azul: cabecera del equipo detectada
- LED verde parpadeando lento: captura activa y parseo de secuencia
- LEC rojo parapadeando rápido: No están llegando datos de la celda

## Archivos importantes

- [CMakeLists.txt](CMakeLists.txt): configuración del proyecto
- [main/main.c](main/main.c): lógica principal del datalogger
- [main/hardware.c](main/hardware.c): inicialización del hardware
- [main/hardware.h](main/hardware.h): pines y configuración hardware
- [partitions.csv](partitions.csv): partición LittleFS

## Historico de versiones

### Versión 25
Se probó con display ST7789 240x240. Consumo total aproximado 120 mA.

### Versión 27
Display apagado y LED RGB conectado al pin 48. Consumo aproximado 85 mA.

### Versión 28
El LED cambia según estado de la celda:
- rojo al conectar la celda
- azul al detectar encabezado
- verde durante captura
- rojo rápido si se pierde la comunicación por más de 30 s

### Versión 29
Se eliminaron las rutinas de LVGL. Ahora solo se usa el LED WS2812B y se mantiene el almacenamiento en LittleFS.


















