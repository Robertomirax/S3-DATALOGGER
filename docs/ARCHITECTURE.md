# Arquitectura del Datalogger

Este documento describe el comportamiento implementado en el firmware ESP32-S3.
La fuente de verdad para pines, version y rutas es `main/hardware.h` y `main/main.c`.

## Componentes

| Componente | Responsabilidad |
| --- | --- |
| `main/main.c` | Arranque, recepcion UART, parser, persistencia, modo USB y OTA |
| `main/hardware.c` | UART, ADC de bateria, OLED, FAT con wear levelling y TinyUSB MSC |
| `main/hardware.h` | Pines, constantes de hardware y API entre modulos |
| `archivos/` | Contenido inicial de la imagen FAT creada por CMake |
| `partitions.csv` | Particiones NVS, OTA y FAT del flash |
| `tools/setup-esp-idf.ps1` | Preparacion del entorno ESP-IDF en PowerShell |

## Arranque

`app_main` ejecuta estas operaciones, en orden:

1. Crea el mutex del archivo y el ring buffer de 8 KiB.
2. Inicializa UART, ADC, OLED, FAT y USB.
3. Comprueba que `log_uart.txt` exista o pueda crearse.
4. Crea un README dentro de la FAT solo si no existe.
5. Busca una OTA pendiente en `/archivos/S3-DATALOGGER.bin`.
6. Crea las tareas de recepcion, escritura, supervision, bateria y seleccion de modo.
7. Envia la cabecera del datalogger al equipo conectado por UART.

## Flujo de modos

Al arrancar, el equipo funciona como logger. La tarea de seleccion espera 5 segundos.
Si durante ese intervalo no se detecta una cabecera con el formato `Alert Technologies,
Model ..., Version ...`, vacia el buffer pendiente y entrega la particion FAT a USB MSC.

Cuando el equipo entra en USB:

- se detienen las tareas que acceden a la FAT;
- el host ve la particion `archivos` como unidad FAT;
- al detectar la desconexion fisica, el firmware recupera la FAT;
- si encuentra una imagen OTA valida, la instala en la particion OTA alternativa y reinicia;
- si no encuentra una imagen valida, reinicia conservando el firmware actual.

La transicion a USB depende de la cabecera de identificacion. Si la cabecera se recibe
pero falta una linea de cliente valida (`C:... T:...`), el firmware deja de registrar datos
y muestra el aviso correspondiente; la variable de cabecera ya queda marcada.

## Recepcion y parser

La recepcion UART usa UART1 a 300 baudios, 8N1, con TX en GPIO17 y RX en GPIO18.
Los bloques recibidos pasan por un ring buffer protegido por mutex.

Antes de encontrar la tabla `Running:`, se conserva el flujo recibido. Luego la maquina
de estados reconoce tramas con los campos `seq #`, `ld cella`, `dac`, `temp` y `tare`.
La linea compactada se muestra en el OLED y se encola para su persistencia. Las tablas
`Running:` y `Taring load cell a:` tambien actualizan la informacion visual de tara y tarado.

La escritura en flash ocurre cuando hay 2048 bytes pendientes o cuando han pasado 5 segundos
desde el ultimo vaciado. Antes de ceder la FAT al USB se fuerza un vaciado final.

## Formato de almacenamiento

- Ruta del log: `/archivos/log_uart.txt`.
- El contenido se escribe con inversion bit a bit de cada byte (`~byte`).
- El parser conserva la cabecera y las tramas compactadas, no un duplicado completo del flujo UART.
- Para interpretar el archivo fuera del firmware hay que invertir de nuevo cada byte.

## Hardware

| Funcion | Configuracion |
| --- | --- |
| UART | UART1, 300 baudios, 8N1 |
| UART TX / RX | GPIO17 / GPIO18 |
| ADC bateria | GPIO5, ADC1_CH4 |
| Divisor ADC | 2.04 |
| Aviso bateria | Amarillo < 4000 mV; rojo < 3800 mV |
| OLED | SSD1306 monocromo 128x64, I2C `0x3C` |
| I2C SDA / SCL | GPIO11 / GPIO12 |
| I2C | 400 kHz |
| USB | USB OTG nativo del ESP32-S3, TinyUSB MSC |

El valor 2.04 debe corresponder al divisor resistivo realmente montado. Si cambia el
circuito, hay que recalibrar este valor y revisar los umbrales.

## Particiones

La tabla actual reserva dos particiones OTA de 7 MiB y una FAT de aproximadamente 1.9 MiB:

| Etiqueta | Tipo | Tamano |
| --- | --- | --- |
| `nvs` | Datos NVS | 0x7000 |
| `otadata` | Datos OTA | 0x2000 |
| `ota_0` | Aplicacion OTA | 0x700000 |
| `ota_1` | Aplicacion OTA | 0x700000 |
| `phy` | Datos PHY | 0x1000 |
| `archivos` | FAT | 0x1DF000 |

La imagen FAT se genera desde `archivos/` mediante `fatfs_create_spiflash_image` y se
incluye en el flash durante `idf.py build` y `idf.py flash`.

## Limitaciones operativas

- No se debe abrir ni modificar la FAT desde el firmware mientras el host USB la tiene montada.
- Hay que expulsar la unidad desde el sistema operativo antes de desconectar el USB.
- La OTA valida la imagen con el mecanismo OTA de ESP-IDF, pero no implementa una firma o
  autenticacion adicional propia.
- Los pines y el divisor descritos aqui son los valores del codigo; el esquema electrico
  externo de la placa debe verificarse por separado.