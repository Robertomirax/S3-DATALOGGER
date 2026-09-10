# Guía de instalación paso a paso

Esta guía explica cómo preparar, compilar y flashear el proyecto Datalogger ESP32-S3 en Windows usando ESP-IDF 6.1.

## 1) Requisitos

Necesitas lo siguiente:

- Windows 10 o 11
- Python 3.11
- Git
- VS Code
- ESP-IDF 6.1 instalado en:
  `C:\esp\v6.1\esp-idf`
- Cable USB-C para la placa ESP32-S3

---

## 2) Instalar ESP-IDF

Si no lo tienes instalado, descarga ESP-IDF 6.1 y deja la carpeta en:

```powershell
C:\esp\v6.1\esp-idf
```

Después, prueba que funciona:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py --version
```

Si se muestra la versión de ESP-IDF, la instalación es correcta.

---

## 3) Abrir la terminal en la carpeta del proyecto

Desde PowerShell:

```powershell
cd C:\DATALOGGER\S3-DATALOGGER
```

También puedes usar el script del proyecto:

```powershell
. .\tools\setup-esp-idf.ps1
```

Este script deja preparado el entorno ESP-IDF para la sesión actual.

---

## 4) Configurar el target

El firmware está pensado para ESP32-S3:

```powershell
idf.py set-target esp32s3
```

---

## 5) Compilar el proyecto

Ejecuta:

```powershell
idf.py build
```

La compilación genera los binarios en la carpeta `build/`.

Si quieres preparar el entorno con el script del proyecto, también puedes hacerlo así:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
idf.py build
```

---

## 6) Flashear la placa

### Modo de programación

1. Conecta la placa por USB-C.
2. Mantén pulsado `BOOT`.
3. Pulsa `RESET` o `EN`.
4. Suelta `BOOT`.
5. Windows detectará un puerto serial/JTAG.

Después, flashea la placa con:

```powershell
idf.py -p COMx flash
```

Por ejemplo:

```powershell
idf.py -p COM3 flash
```

Sustituye `COMx` por el puerto real de tu placa.

> La primera carga completa debe incluir también la partición FAT del proyecto. El proyecto genera la imagen desde la carpeta `archivos` y la usa en la partición `archivos` del flash.

---

## 7) Reiniciar el firmware

Tras el flash:

- Suelta `BOOT`
- Pulsa `RESET`

La placa arrancará normalmente en modo datalogger.

Si la celda no envía cabecera durante unos 5 segundos, el firmware cambia automáticamente a modo USB MSC.

---

## 8) Ver logs por monitor serie

```powershell
idf.py -p COM15 monitor
```

Usa el puerto correcto de tu placa. Si tienes dudas, revisa el Administrador de dispositivos de Windows.

---

## 9) Modo USB / pendrive

Cuando el sistema entra en modo USB:

- la partición `archivos` se expone como unidad FAT
- el ordenador puede ver el dispositivo como pendrive
- el firmware evita acceder a la partición mientras el host la usa

Para actualizar el firmware desde USB:

1. Compila el firmware:
   ```powershell
   idf.py build
   ```
2. Copia el archivo `build/S3-DATALOGGER.bin` al pendrive con el nombre:
   ```text
   firmware.bin
   ```
3. Expulsa la unidad desde Windows.
4. Desconecta el cable USB-C.
5. El equipo valida la imagen, la instala y reinicia automáticamente.

---

## 10) Configuración del hardware

Este proyecto usa la siguiente configuración actual:

- UART: `UART_NUM_1`
- TX: `GPIO17`
- RX: `GPIO18`
- velocidad: `300 bauds`
- ADC batería: `GPIO5` (`ADC1_CH4`)
- divisor de batería: `2.03`
- OLED SSD1306 128x64
- SDA: `GPIO11`
- SCL: `GPIO12`
- dirección OLED: `0x3C`

Se define en:

- [main/hardware.h](main/hardware.h)
- [main/hardware.c](main/hardware.c)

---

## 11) Problemas habituales

### El puerto COM no aparece
- Repite el proceso con `BOOT + RESET`
- Instala drivers USB Serial/JTAG para ESP32-S3
- Comprueba si aparece otro COM en el Administrador de dispositivos

### `idf.py` no está disponible
Ejecuta:

```powershell
$env:IDF_PATH = 'C:\esp\v6.1\esp-idf'
. "$env:IDF_PATH\export.ps1"
```

### El proyecto no compila
- Verifica que `IDF_PATH` apunta a la carpeta correcta
- Revisa la ruta de instalación de ESP-IDF
- vuelve a ejecutar:

```powershell
idf.py build
```

### La placa no entra en USB
- Comprueba que no está recibiendo la cabecera de la celda
- La transición a USB se realiza automáticamente tras 5 segundos sin cabecera

---

## 12) Comandos rápidos

```powershell
cd C:\DATALOGGER\S3-DATALOGGER
. .\tools\setup-esp-idf.ps1
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash
idf.py -p COM15 monitor
```

---

## 13) Archivos relevantes

- [README.md](README.md)
- [CMakeLists.txt](CMakeLists.txt)
- [main/hardware.h](main/hardware.h)
- [main/hardware.c](main/hardware.c)
- [partitions.csv](partitions.csv)
- [archivos/README.md](archivos/README.md)

Si quieres, puedo dejarte también una versión de esta guía en formato más corto para imprimir o una versión enfocada solo a “primer arranque del equipo”.
