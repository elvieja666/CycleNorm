# CycleNorm v1.6.0

App para Mac que **normaliza un 808 semiciclo por semiciclo**: detecta cada semiciclo de la onda (el positivo y el negativo, de cruce por cero a cruce por cero) y lleva cada uno a **0 dBFS**. Así, aunque el 808 sea asimétrico (el lado negativo más chico que el positivo, o al revés), los dos lados quedan iguales. El decay desaparece y el 808 queda plano de principio a fin, pero con el timbre real de cada momento (el glide, la saturación y los armónicos de la cola se mantienen). Después lo exportás o lo arrastrás directo al DAW.

## Cómo usarla

1. Arrastrá uno o varios WAV / AIFF a la ventana, o una carpeta entera (también abre FLAC, MP3, M4A y CAF), o usá **Abrir…**. También podés soltar archivos sobre el ícono del Dock.
2. Arriba ves el **original** y abajo el **procesado**. Las líneas verticales finitas marcan cada semiciclo detectado. Aparecen cuando hacés zoom y entran.
3. La app suena por la salida que tenga elegida la Mac y la sigue sola: si enchufás auriculares, conectás unos bluetooth o cambiás la salida en Ajustes > Sonido, en un segundo pasa a esa salida (si estaba sonando, sigue sonando, con un corte mínimo al cambiar).
4. **Espacio** = play / stop (cada play arranca desde el principio; si con el stop puesto hacés clic en la forma de onda, el próximo play arranca desde ahí), **A** = alternar entre escuchar el original y el procesado (sin cortar), **↑ / ↓** = pasar de archivo. **Loop** repite.
5. Para llevarlo al DAW tenés dos caminos:
   - **Exportar…** guarda el archivo seleccionado (por defecto con `_norm` al final del nombre). **Exportar todos…** procesa toda la lista con los ajustes actuales y la guarda en la carpeta que elijas.
   - **Arrastrá el procesado al DAW**: arrastrás ese botón a una pista y listo. El archivo queda guardado en `~/Music/CycleNorm/`, así el DAW lo puede seguir leyendo.

## Controles

| Control | Qué hace |
|---|---|
| **Amount** | Cuánto normaliza. **100 %**: todos los semiciclos a 0 dBFS (plano). **0 %**: el original. En el medio, cada semiciclo recorre ese porcentaje del camino hasta 0 dB (por ejemplo, con 50 % uno que estaba en -24 dB queda en -12 dB) |
| **Skip** | Los primeros semiciclos que no se tocan, para respetar el ataque o el click del 808. Por defecto 4 (2 ondas). **No tiene barra: se cambia arrastrando la línea punteada amarilla** en el gráfico de abajo (procesado). Al pasar el mouse por encima, el puntero cambia a ↔. La línea **salta de semiciclo en semiciclo**, de 0 (bien a la izquierda, al principio del archivo: no saltea nada) a 512. Los semiciclos salteados se ven en el gris del original, y abajo a la izquierda se lee cuántos son |
| **Fin** | La línea roja punteada a la derecha del gráfico de abajo. **Se arrastra para cortar el final del sample**, por ejemplo para sacar el ruido que queda subido al final. Salta de semiciclo en semiciclo, así el corte cae siempre en un cruce por cero y no hace click. Lo que queda a la derecha se ve oscurecido y no suena ni se exporta: Play, Exportar, Exportar todos y el arrastre al DAW terminan ahí. Es **por archivo** (cada sample tiene su propio Fin) y arranca al final (sin cortar). No puede quedar antes de la línea de Skip. Abajo a la izquierda se lee dónde está ("Fin: 1.81 s (de 2.64 s)") |
| **Rampa** | De 0 a 100 % (por defecto **0**). Es la rampa en cada cambio de ganancia, centrada en el cruce por cero, y es proporcional a cada semiciclo. **0 %**: rampa mínima (1 ms como mucho, o el 6 % del semiciclo; unos 0,7 ms en un 808 de 45 Hz). **100 %**: cubre el 40 % de cada semiciclo a cada lado del cruce (unos 9 ms en total en un 808 de 45 Hz, 3,3 ms en uno de 120 Hz). Ojo: al mezclar la ganancia de un semiciclo con la del vecino, en un 808 asimétrico los lados vuelven a quedar un poco distintos (en las pruebas el 2º armónico sube de -60 a -29 dB). Puede servir en los saltos bruscos (la cola con ruido, el arranque de un golpe). Nunca deja pasar de 0 dB: si la rampa mete una ganancia más alta en el borde de un semiciclo, ahí se baja justo lo necesario |
| **Formato** | **Igual al original** (mismo formato, bits y sample rate), **WAV 24 bit** o **WAV 32 bit float** |

Los ajustes se aplican a todos los archivos de la lista y se guardan al cerrar la app.

**El objetivo es siempre 0 dBFS** (no hay control de Target) y **se normaliza todo**, incluida la cola hasta el final y el ruido de fondo (no hay Threshold). Abajo a la izquierda se ve cuántas ondas y semiciclos detectó y el pico de salida. Lo único que puede pasar de 0 dBFS es el ataque original que dejás sin tocar con Skip (si ya venía pasado). En ese caso aparece **¡SATURA!** en rojo: exportá en 32 bit float.

## Cómo funciona

- **Detección:** usa el canal con más energía (si L y R están en contrafase, sumarlos los anularía). Primero **mide la nota (la fundamental)** a lo largo de todo el archivo, siguiendo también el glide del principio. Después filtra el audio justo arriba de esa nota (1,4 veces la fundamental) con filtros de fase cero (ida y vuelta, sin desfase) y corta solo en los cruces de la fundamental. Si un 808 tiene armónicos tan fuertes que la onda real cruza el 0 varias veces por período, esos rulos quedan adentro del semiciclo y reciben la misma ganancia: no se normalizan como si fueran semiciclos. En el **ataque** (el arranque de cada golpe) la nota baja rápido, y el medidor, que mira ventanas de unos 100 ms, todavía ve la nota grave. Por eso, en el hueco entre el arranque y el primer corte de la nota, las ondas se buscan con el filtro abierto (250 Hz): así se detectan también las primeras ondas rápidas del glide y la línea de Skip puede llegar hasta ahí. Después corre cada cruce (el que sube y el que baja) al cruce real del audio más cercano. No usa el promedio: un 808 asimétrico tiene DC propio, y sus semiciclos empiezan y terminan donde el audio vale 0. Si cerca no hay un cruce real, ahí no corta y une ese tramo con el vecino. Justo antes de un golpe nuevo, la cola y el golpe a veces se pegan sin pasar por 0. Si un tramo queda mucho más largo que los anteriores, se corta en los cruces reales que tenga adentro, y si la señal salta de golpe (más de 3 veces el nivel de la cola y más de 3 veces el semiciclo anterior entero), se corta justo en el arranque. Un rulo chico de armónico al principio de un semiciclo no cuenta como golpe nuevo. Así la cola se sostiene a 0 dB hasta el golpe, sin un pozo antes.
- **Ganancia:** mide el pico de cada semiciclo en todos los canales y le aplica la ganancia que lo lleva a 0 dBFS. La misma ganancia va a todos los canales, así la imagen estéreo no cambia.
- **Sin clicks:** el cambio de ganancia entre una onda y la siguiente cae justo en el cruce por cero, con una rampa muy corta (1 ms como máximo, o el 6 % de la onda).
- **Puntas del archivo:** el pedacito antes del primer cruce y el que queda después del último (una onda incompleta) se normalizan con su propio pico, sin pasarse nunca de 0 dBFS.
- **Límite:** la ganancia nunca sube más de +60 dB, así el silencio digital no se lleva a 0 dB.
- **Varios golpes en un archivo:** cada golpe se normaliza al mismo objetivo. Entre golpes, si hay ruido, también se sube. El silencio digital absoluto queda en silencio.

## Compilar e instalar

1. En GitHub Desktop: escribí un Summary, **Commit to main**, **Push origin**. Cada push dispara la compilación; si querés guardar un cambio sin compilar, poné `[skip ci]` en el Summary.
2. Esperá la compilación en **Actions** ("Compilar app"). Antes de armar el zip, corre la prueba del motor, que tiene que dar TODO OK. Después descargá **CycleNorm.zip**.
3. Descomprimí y hacé doble clic en **Instalar.command**: copia `CycleNorm.app` a Aplicaciones, le saca el bloqueo de macOS y la abre. **Desinstalar.command** la borra junto con sus ajustes.

La app es universal (Apple Silicon e Intel) y anda en macOS 11 o más nuevo.

## Carpetas

- `Source/CycleEngine.h`: el motor (detección de ondas y ganancia). No depende de JUCE.
- `Source/Main.cpp`: la app (ventana, lista, controles, exportar).
- `Source/WaveformView.h`: el visor de forma de onda.
- `Source/ABSource.h`: la reproducción original / procesado.
- `Tests/EngineTest.cpp`: prueba del motor con 808 sintéticos (limpio, saturado con DC y ruido, estéreo invertido, 96 kHz, dos golpes, asimétrico, Threshold, Target fijo, Amount 50 %). Incluye un 808 con 2º y 3º armónico casi tan fuertes como la fundamental y uno con un glide muy rápido en el ataque (de 120 a 34 Hz en 50 ms). Comprueba que cada tramo llega al objetivo, que cada onda dura un período de la fundamental, que no hay un pozo antes de un golpe nuevo, que ningún punto lo pasa, que los semiciclos cortan donde el audio vale 0 y que no aparecen clicks. Las instrucciones para compilarla están arriba del archivo.

**Visor:** la rueda o el gesto vertical de dos dedos hace zoom, el gesto horizontal desplaza, el pinch también hace zoom, un clic mueve el cursor de reproducción y el doble clic vuelve a mostrar todo.
