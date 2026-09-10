# Volumen FAT del proyecto

La carpeta `archivos` se usa como origen para la partición FAT del firmware. El proyecto genera esa imagen durante la configuración de CMake mediante la instrucción:

```cmake
fatfs_create_spiflash_image(archivos archivos FLASH_IN_PROJECT)
```

Esto crea un volumen FAT que se monta en la aplicación como `/archivos`.

## Contenido actual del volumen

La partición está reservada para archivos del logger y actualizaciones OTA:

- `/archivos/log_uart.txt`: log de tramas parseadas y compactadas
- `/archivos/firmware.bin`: firmware binario para actualización OTA desde USB

## Regla de uso

- El volumen no se formatea automáticamente desde la aplicación para no borrar datos existentes.
- La imagen base se crea desde el contenido físico de la carpeta `archivos` y debe volcarse al flash en la primera programación completa.
- Cuando el sistema entra en modo USB, la partición se expone al host como unidad FAT y no debe accederse desde la aplicación hasta que se cierre la sesión MSC.

## Configuración relacionada

- `partitions.csv` define la partición `archivos` en el final del flash
- [main/hardware.c](../main/hardware.c) gestiona la activación del almacenamiento USB y la recuperación de la FAT
- [main/main.c](../main/main.c) escribe la captura y las actualizaciones en la partición

## Flujo típico

1. Flashes iniciales con la imagen completa del proyecto.
2. El firmware crea y usa `/archivos/log_uart.txt`.
3. Si no llega cabecera de la celda, se pasa a modo USB.
4. El host puede copiar `firmware.bin` en la raíz del pendrive.
5. La desconexión USB dispara la validación OTA y el reinicio con la imagen nueva.


















