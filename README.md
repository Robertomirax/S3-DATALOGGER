------------------------------------------------------------------------------------------------------
versión 25
Funciona ok con display st7789 240x240 consume 120mA en total, el display consume 38mA el tiempo desde
el reset hasta que comienza a transmitir el encabezado es de aprox. 500mS
------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------
versión 27
Funciona ok con display apagado y led rgb conectado al pin 48, el consumo es de 85mA lo demás igual 
que la versión 25
------------------------------------------------------------------------------------------------------
------------------------------------------------------------------------------------------------------
versión 28
el led se enciende rojo al conectar la celda de carga.
Cuando llega el encabezado de Alert Technologies se pone azul indicando que se estableció la
comunicación con la celda de carga.
Cuando empieza a leer y procesar el compactado de los valores de la celda de carga el led
se pone verde con parpadeo lento.
Si deja de recibir datos desde la celda por mas de 30 segundos, pasa a parpadeo rojo rápido.
-----------------------------------------------------------------------------------------------------
-----------------------------------------------------------------------------------------------------
version 29
se eliminaron las rutinas del lvgl. Ahora no hay ningún display conectado solo el led WS2812B
-----------------------------------------------------------------------------------------------------
-----------------------------------------------------------------------------------------------------


















